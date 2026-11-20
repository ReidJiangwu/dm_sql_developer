#include "ContextBootstrapTask.h"
#include "CommandDiagnostics.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace Publication {
namespace {
void require(bool condition, const QString& text) { if (!condition) throw Failure(InputError, text); }
void keys(const QJsonObject& object, const QStringList& allowed) {
    for (auto it = object.begin(); it != object.end(); ++it)
        require(allowed.contains(it.key()), QStringLiteral("未知引导任务字段：%1").arg(it.key()));
}
bool identifier(const QString& name) { return validIdentifier(name) && name.toUtf8().size() <= 30; }
}
ContextBootstrapTask prepareContextBootstrapTask(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() <= 1024 * 1024, QStringLiteral("引导任务不可读或超过 1 MiB。"));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), QStringLiteral("引导任务不是 JSON 对象。"));
    const auto root = document.object();
    keys(root, {"schema_version", "connection_id", "schema", "expected_database_identity", "source", "contexts"});
    ContextBootstrapTask task;
    task.connectionId = root.value("connection_id").toString();
    task.schema = root.value("schema").toString();
    const auto identity = root.value("expected_database_identity").toObject();
    keys(identity, {"identity"});
    task.expectedIdentity = identity.value("identity").toString();
    require(root.value("schema_version").toDouble() == 1 && !task.connectionId.trimmed().isEmpty()
            && validIdentifier(task.schema) && QRegularExpression("^sha256:[0-9a-f]{64}$")
                .match(task.expectedIdentity).hasMatch(), QStringLiteral("引导任务缺少版本、连接 ID、Schema 或身份摘要。"));
    require(root.value("source").isObject(), QStringLiteral("必须声明恢复参考文件及摘要；不会执行该参考文件。"));
    const auto source = root.value("source").toObject();
    keys(source, {"path", "sha256"});
    const QString requested = source.value("path").toString();
    const auto info = QFileInfo(QDir(QFileInfo(path).absolutePath()).absoluteFilePath(requested));
    QFile input(info.canonicalFilePath());
    require(!requested.isEmpty() && info.isFile() && input.open(QIODevice::ReadOnly)
            && input.size() <= 4 * 1024 * 1024, QStringLiteral("恢复参考文件缺失、不可读或超过 4 MiB。"));
    const QByteArray bytes = input.read(4 * 1024 * 1024 + 1);
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    require(input.error() == QFileDevice::NoError && input.atEnd() && bytes.size() <= 4 * 1024 * 1024
            && QRegularExpression("^[0-9a-fA-F]{64}$").match(source.value("sha256").toString()).hasMatch()
            && hash.compare(source.value("sha256").toString(), Qt::CaseInsensitive) == 0,
            QStringLiteral("恢复参考文件摘要不匹配或未完整读取。"));
    task.source = {{"path", info.canonicalFilePath()}, {"sha256", hash}, {"execution", "provenance_only"}};
    require(root.value("contexts").isArray() && !root.value("contexts").toArray().isEmpty()
            && root.value("contexts").toArray().size() <= 32, QStringLiteral("必须声明 1～32 个 CONTEXT。"));
    QSet<QString> names;
    for (const auto& value : root.value("contexts").toArray()) {
        require(value.isObject(), QStringLiteral("CONTEXT 声明格式错误。"));
        const auto item = value.toObject();
        keys(item, {"namespace", "trusted_owner", "trusted_package"});
        ContextDeclaration context{item.value("namespace").toString(), item.value("trusted_owner").toString(),
            item.value("trusted_package").toString()};
        require(identifier(context.name) && context.name != "USERENV" && identifier(context.owner)
                && context.owner == task.schema && identifier(context.package) && !names.contains(context.name),
                QStringLiteral("CONTEXT 名称、绑定或 Schema 无效，或存在重复／系统名字空间。"));
        names.insert(context.name);
        task.contexts.append(context);
    }
    return task;
}
QString contextInspectionSql(const QString& name) {
    return QStringLiteral("SELECT O.NAME,O.INFO5,O.INFO6,S.NAME FROM SYS.SYSOBJECTS O "
        "JOIN SYS.SYSOBJECTS S ON O.SCHID=S.ID AND S.TYPE$='SCH' "
        "WHERE O.TYPE$='SCHOBJ' AND O.SUBTYPE$='CONTEXT' AND UPPER(O.NAME)=%1")
        .arg(CommandDiagnostics::literal(name.toUpper()));
}
QString createMissingContextSql(const ContextDeclaration& declaration) {
    if (!identifier(declaration.name) || declaration.name == "USERENV"
            || !identifier(declaration.owner) || !identifier(declaration.package))
        throw Failure(InputError, QStringLiteral("不允许生成该 CONTEXT 声明。"));
    // IF NOT EXISTS also protects a namespace created concurrently after inspection.
    return QStringLiteral("CREATE CONTEXT IF NOT EXISTS \"%1\" USING \"%2\".\"%3\"")
        .arg(declaration.name, declaration.owner, declaration.package);
}
}
