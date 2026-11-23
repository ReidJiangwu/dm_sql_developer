#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <windows.h>
#include <vector>

static bool verify(const QString& file, const QString& version) {
    const auto path = QDir::toNativeSeparators(file).toStdWString();
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), nullptr);
    std::vector<BYTE> data(size);
    if (!size || !GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return false;
    struct Translation { WORD language; WORD page; };
    void* table = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", &table, &length)
            || length < sizeof(Translation)) return false;
    const auto translation = *static_cast<const Translation*>(table);
    const QString prefix = QStringLiteral("\\StringFileInfo\\%1%2\\")
        .arg(translation.language, 4, 16, QLatin1Char('0'))
        .arg(translation.page, 4, 16, QLatin1Char('0'));
    for (const auto* field : {"FileDescription", "ProductName", "FileVersion", "ProductVersion"}) {
        void* value = nullptr;
        const auto key = (prefix + QString::fromLatin1(field)).toStdWString();
        const auto expected = QByteArray(field).endsWith("Version")
            ? version : QStringLiteral("小智达梦管理工具");
        if (!VerQueryValueW(data.data(), key.c_str(), &value, &length) || !length
                || QString::fromWCharArray(static_cast<const wchar_t*>(value)) != expected) {
            qCritical() << file << field << "version resource mismatch";
            return false;
        }
    }
    return true;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3) return 2;
    for (const auto* exe : {"GzydDmTools.exe", "GzydDmToolsCli.exe", "GzydDmToolsDebugHost.exe"})
        if (!verify(QDir(args[1]).filePath(QString::fromLatin1(exe)), args[2])) return 1;
    qInfo("WINDOWS_VERSION_RESOURCES=PASS");
    return 0;
}
