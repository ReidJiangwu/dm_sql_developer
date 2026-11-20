#include "../src/core/ContextBootstrapTask.h"
#include "../src/core/CommandDiagnostics.h"
#include "../src/data/ModuleOwnership.h"
#include "../src/data/NativeProgramDiagnostics.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <iostream>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
}
}
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        const QString scenario = application.arguments().value(1);
        if (scenario == "native-diagnostics") {
            const QVector<SessionDiagnosticRecord> records{
                {-3328, "包体[P]解析出错\n第1535行，第9列：[-2193] 无法解析 RANDOMBYTES\n第196行：[-2207] 无法解析 RENAME_FILE___"},
                {-6105, "第608行：数据类型不匹配"}};
            const auto parsed = parseNativeProgramDiagnostics(records, {},
                {"fictional", "APP_TEST", DatabaseObjectType::Package, "P"});
            require(parsed.size() == 4, "preserve all records and concrete root causes");
            require(parsed[1].code == -2193 && parsed[1].line == 1535 && parsed[1].column == 9,
                    "native code and internal line/column");
            require(parsed[2].code == -2207 && parsed[3].code == -6105, "native return order and per-record codes");
            require(parsed[1].sourceText.isEmpty() && parsed[1].sourceUnit == "PACKAGE BODY",
                    "body attribution from explicit -3328, no invented source-file mapping");
            const QString longMessage = QStringLiteral("第42行：[-2207] ") + QString(100000, 'X') + "END";
            const auto longErrors = parseNativeProgramDiagnostics({{-2207, longMessage}}, {},
                {"fictional", "APP_TEST", DatabaseObjectType::PackageBody, "P"});
            require(longErrors.size() == 1 && longErrors[0].message == longMessage,
                    "structured native errors retain long messages");
            const CommandCompiledObject specification{"APP_TEST", "P", "PACKAGE"};
            require(!CommandDiagnostics::queries(specification).join(' ').contains("ORACLE_USER_ERRORS")
                    && CommandDiagnostics::queries(specification, false, true).join(' ').contains("ORACLE_USER_ERRORS"),
                    "CLI explicitly opts into aggregate fallback without changing GUI defaults");
            auto native = Publication::diagnosticJson({"APP_TEST", "P", "PACKAGE BODY"}, parsed[1].message, parsed[1].code);
            native.insert("database_line", parsed[1].line);
            QJsonObject result{{"objects", QJsonArray{QJsonObject{{"diagnostics_complete", false},
                {"errors", QJsonArray{}}, {"native_errors", QJsonArray{native}}}}}};
            Publication::summarizeResult({}, result);
            const auto retained = result.value("errors").toArray();
            require(retained.size() == 1 && retained[0].toObject().value("code").toInt() == -2193
                    && retained[0].toObject().value("database_line").toInt() == 1535
                    && retained[0].toObject().value("source_line").isNull(),
                    "auxiliary query failure cannot erase native errors or invent current-file lines");
        } else if (scenario == "module-ownership") {
            const QString source = "/* MODULE_ CONSTANT VARCHAR2(6) := 'WRONG'; */\n"
                "-- MODULE_ CONSTANT VARCHAR2(6) := 'WRONG';\n"
                "text_ CONSTANT VARCHAR2(200) := 'MODULE_ CONSTANT VARCHAR2(6) := ''FAKE'';';\n"
                "module_ CONSTANT VARCHAR2(6) := 'FNDBAS';\n";
            const auto modules = installedObjectModules(source, false);
            require(modules.size() == 1 && modules.contains("FNDBAS"), "ignore comments and string literals");
            require(installedObjectModules("CREATE PACKAGE FND_P AS END;", false).isEmpty(),
                    "never guess module from object prefix");
            require(installedObjectModules(source + "module_ CONSTANT VARCHAR2(6) := 'OTHER';", false).size() == 2,
                    "ambiguous module ownership remains unresolved");
            require(installedObjectModules("LU=P^MODULE=DOCMAN^", true).contains("DOCMAN"), "view comment metadata");
        } else if (scenario == "context-sql") {
            const auto sql = Publication::createMissingContextSql({"DEMO_CTX", "APP_TEST", "DEMO_API"});
            require(sql.contains("IF NOT EXISTS") && !sql.contains("OR REPLACE") && !sql.contains("DROP"),
                    "create only absent namespaces, never replace/drop");
            const auto inspection = Publication::contextInspectionSql("DEMO_CTX");
            require(inspection.contains("SYS.SYSOBJECTS") && inspection.contains("INFO5") && inspection.contains("INFO6")
                    && !inspection.contains("ORACLE"), "inspect native namespace and trusted binding directly");
        } else if (scenario == "context-input") {
            QTemporaryDir directory;
            require(directory.isValid(), "temporary directory");
            const QByteArray source("-- fixture provenance only, never executed by bootstrap\n");
            const QString sourcePath = directory.filePath("restore.sql");
            const QString taskPath = directory.filePath("context.json");
            write(sourcePath, source);
            QJsonObject task{{"schema_version", 1}, {"connection_id", "fictional"}, {"schema", "APP_TEST"},
                {"expected_database_identity", QJsonObject{{"identity", "sha256:" + QString(64, '0')}}},
                {"source", QJsonObject{{"path", sourcePath}, {"sha256", QString::fromLatin1(
                    QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex())}}},
                {"contexts", QJsonArray{QJsonObject{{"namespace", "DEMO_CTX"}, {"trusted_owner", "APP_TEST"},
                    {"trusted_package", "DEMO_API"}}}}};
            const auto prepare = [&](const QJsonObject& value) {
                write(taskPath, QJsonDocument(value).toJson());
                return Publication::prepareContextBootstrapTask(taskPath);
            };
            require(prepare(task).contexts.size() == 1, "bounded declared bootstrap");
            const auto rejects = [&](const QJsonObject& value) {
                try { (void)prepare(value); }
                catch (const Publication::Failure& error) {
                    require(error.exitCode == Publication::InputError, "bootstrap input exit code");
                    return;
                }
                throw std::runtime_error("unsafe bootstrap accepted");
            };
            auto bad = task;
            auto contexts = task.value("contexts").toArray();
            auto context = contexts[0].toObject();
            context.insert("namespace", "USERENV");
            bad.insert("contexts", QJsonArray{context});
            rejects(bad);
            context.insert("namespace", "DEMO_CTX");
            context.insert("trusted_owner", "OTHER");
            bad.insert("contexts", QJsonArray{context});
            rejects(bad);
            contexts.append(contexts[0]);
            bad = task;
            bad.insert("contexts", contexts);
            rejects(bad);
            write(sourcePath, "changed provenance");
            rejects(task);
        } else throw std::runtime_error("unknown scenario");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
