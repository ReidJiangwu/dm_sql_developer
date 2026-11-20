#pragma once

#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace SaveFileDialog {

inline QStringList extensions(const QString& filter) {
    const QString first = filter.section(QStringLiteral(";;"), 0, 0);
    const QString patterns = first.section(u'(', -1).section(u')', 0, 0);
    QStringList result;
    const QRegularExpression extension(QStringLiteral(R"(^\*\.([A-Za-z0-9_+-]+(?:\.[A-Za-z0-9_+-]+)*)$)"));
    for (const auto& pattern : patterns.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
        const auto match = extension.match(pattern);
        if (match.hasMatch()) result.append(match.captured(1));
    }
    result.removeDuplicates();
    return result;
}

struct Reply {
    QString path;
    QString error;
};

inline Reply parseReply(const QByteArray& bytes) {
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return {{}, QStringLiteral("macOS 保存窗口返回了无效结果。")};
    const auto object = document.object();
    const auto status = object.value(QStringLiteral("status")).toString();
    if (status == QStringLiteral("cancelled")) return {};
    const QString path = object.value(QStringLiteral("path")).toString();
    if (status != QStringLiteral("selected") || !path.startsWith(u'/') || path.contains(QChar::Null))
        return {{}, QStringLiteral("macOS 保存窗口未返回有效文件路径。")};
    return {path, {}};
}

#ifdef Q_OS_WIN
namespace Detail {
// Wine 11.0 extensions, resolved only in the bundled macOS Wine client.
using ToUnix = char* (__cdecl*)(LPCWSTR);
using ToDos = WCHAR* (__cdecl*)(LPCSTR);
using Spawn = LONG (WINAPI*)(char* const[], int);

inline QString unixPath(ToUnix convert, const QString& path) {
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    char* value = convert(native.c_str());
    if (!value) return {};
    const QString result = QString::fromUtf8(value);
    HeapFree(GetProcessHeap(), 0, value);
    return result;
}

inline QString dosPath(ToDos convert, const QString& path) {
    const auto utf8 = path.toUtf8();
    WCHAR* value = convert(utf8.constData());
    if (!value) return {};
    const QString result = QDir::fromNativeSeparators(QString::fromWCharArray(value));
    HeapFree(GetProcessHeap(), 0, value);
    return result;
}

class WaitDialog final : public QDialog {
public:
    explicit WaitDialog(QWidget* parent) : QDialog(parent, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint) {
        setWindowTitle(QStringLiteral("保存文件"));
        setWindowModality(Qt::ApplicationModal);
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel(QStringLiteral("请在 macOS 保存窗口中选择文件位置。"), this));
    }
    // Cancellation belongs to the native panel; keep the caller alive until it exits.
    void reject() override {}
};

inline Reply select(QWidget* parent, const QString& caption, const QString& directory, const QString& filter) {
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto toUnix = reinterpret_cast<ToUnix>(GetProcAddress(kernel, "wine_get_unix_file_name"));
    const auto toDos = reinterpret_cast<ToDos>(GetProcAddress(kernel, "wine_get_dos_file_name"));
    const auto spawn = reinterpret_cast<Spawn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__wine_unix_spawnvp"));
    const QString script = qEnvironmentVariable("REID_MACOS_SAVE_DIALOG");
    if (!toUnix || !toDos || !spawn || !script.startsWith(u'/')
            || !QFileInfo::exists(dosPath(toDos, script)))
        return {{}, QStringLiteral("macOS 原生保存组件缺失，请安装包含此功能的 Mac 版本。")};

    QTemporaryDir temporary;
    if (!temporary.isValid()) return {{}, QStringLiteral("无法创建保存窗口临时目录。")};
    QFile::setPermissions(temporary.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString requestPath = temporary.filePath(QStringLiteral("request.json"));
    const QString responsePath = temporary.filePath(QStringLiteral("response.json"));
    const QString requestUnix = unixPath(toUnix, requestPath);
    const QString responseUnix = unixPath(toUnix, responsePath);
    if (requestUnix.isEmpty() || responseUnix.isEmpty())
        return {{}, QStringLiteral("无法转换 macOS 保存窗口临时路径。")};

    const QFileInfo initial(directory);
    QString initialDirectory;
    QString name;
    if (!directory.isEmpty()) {
        name = initial.isDir() ? QString() : initial.fileName();
        if (initial.isAbsolute())
            initialDirectory = unixPath(toUnix, initial.isDir() ? initial.absoluteFilePath() : initial.absolutePath());
    }
    QFile request(requestPath);
    const QByteArray payload = QJsonDocument(QJsonObject{
        {QStringLiteral("caption"), caption}, {QStringLiteral("directory"), initialDirectory},
        {QStringLiteral("name"), name}, {QStringLiteral("extensions"), QJsonArray::fromStringList(extensions(filter))}
    }).toJson(QJsonDocument::Compact);
    if (!request.open(QIODevice::WriteOnly) || request.write(payload) != payload.size())
        return {{}, QStringLiteral("无法写入 macOS 保存窗口请求。")};
    request.close();

    LONG exitCode = -1;
    WaitDialog waiting(parent);
    auto* worker = QThread::create([&]() {
        QByteArray executable("/usr/bin/osascript"), language("-l"), javascript("JavaScript");
        auto scriptBytes = script.toUtf8(), requestBytes = requestUnix.toUtf8(), responseBytes = responseUnix.toUtf8();
        char* arguments[]{executable.data(), language.data(), javascript.data(), scriptBytes.data(),
            requestBytes.data(), responseBytes.data(), nullptr};
        exitCode = spawn(arguments, TRUE);
    });
    QObject::connect(worker, &QThread::finished, &waiting, &QDialog::accept, Qt::QueuedConnection);
    worker->start();
    waiting.exec();
    worker->wait();
    delete worker;
    if (exitCode != 0) return {{}, QStringLiteral("macOS 原生保存窗口启动或运行失败。")};
    QFile response(responsePath);
    if (!response.open(QIODevice::ReadOnly) || response.size() > 65536)
        return {{}, QStringLiteral("无法读取 macOS 保存窗口结果。")};
    Reply reply = parseReply(response.readAll());
    if (!reply.path.isEmpty()) {
        reply.path = dosPath(toDos, reply.path);
        if (reply.path.isEmpty()) reply.error = QStringLiteral("无法转换所选 macOS 文件路径。");
    }
    return reply;
}
} // namespace Detail
#endif

inline QString getSaveFileName(QWidget* parent = nullptr, const QString& caption = {},
        const QString& directory = {}, const QString& filter = {}, QString* selectedFilter = nullptr,
        QFileDialog::Options options = {}) {
    if (qEnvironmentVariable("REID_MACOS_WINE") != QStringLiteral("1"))
        return QFileDialog::getSaveFileName(parent, caption, directory, filter, selectedFilter, options);
#ifdef Q_OS_WIN
    const QString chosenFilter = selectedFilter && !selectedFilter->isEmpty()
        ? *selectedFilter : filter.section(QStringLiteral(";;"), 0, 0);
    const Reply reply = Detail::select(parent, caption, directory, chosenFilter);
    if (!reply.error.isEmpty()) QMessageBox::warning(parent, QStringLiteral("保存文件"), reply.error);
    if (!reply.path.isEmpty() && selectedFilter) *selectedFilter = chosenFilter;
    return reply.path;
#else
    return {};
#endif
}
} // namespace SaveFileDialog
