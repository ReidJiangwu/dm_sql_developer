#include "../src/core/PublicationTask.h"
#include "../src/core/ProductFeatures.h"
#include "../src/core/CommandDiagnostics.h"
#include "../src/core/PublicationSchedule.h"
#include "../src/data/PowerCompileWorkers.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <iostream>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
}
QJsonObject file(const QString& path, const QByteArray& bytes) {
    return {{"path", path}, {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes,
        QCryptographicHash::Sha256).toHex())}};
}
}
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        if (application.arguments().value(1) == "prepare-task") {
            const auto prepared = Publication::prepareTask(application.arguments().value(2));
            std::cout << "prepared units=" << prepared.units.size() << " files=" << prepared.files.size() << std::endl;
            return 0;
        }
        QTemporaryDir temporary;
        require(temporary.isValid(), "temporary directory");
        const QString root = temporary.filePath("root.sql");
        const QString child = temporary.filePath("child.sql");
        const QString path = temporary.filePath("task.json");
        const QByteArray sql("CREATE OR REPLACE PACKAGE P AS PROCEDURE T; END;\n/\n"
            "CREATE OR REPLACE PACKAGE BODY P AS PROCEDURE T IS BEGIN NULL; END; END;\n/\n");
        write(root, sql);
        QJsonObject task{{"schema_version", 1}, {"connection_id", "fictional-id"}, {"schema", "APP_TEST"},
            {"expected_database_identity", QJsonObject{{"identity", "sha256:" + QString(64, '0')}}},
            {"files", QJsonArray{file(root, sql)}},
            {"expected_objects", QJsonArray{QJsonObject{{"owner", "APP_TEST"}, {"name", "P"}, {"type", "PACKAGE"}},
                QJsonObject{{"owner", "APP_TEST"}, {"name", "P"}, {"type", "PACKAGE BODY"}}}},
            {"execution", QJsonObject{{"parallelism", 1}, {"stop_on_error", true}, {"compile_expected_objects", true}}}};
        const auto prepare = [&](const QJsonObject& object) {
            write(path, QJsonDocument(object).toJson());
            return Publication::prepareTask(path);
        };
        const auto rejects = [&](const QJsonObject& object) {
            try { (void)prepare(object); }
            catch (const Publication::Failure& failure) {
                require(failure.exitCode == Publication::InputError, "input exit code");
                return;
            }
            throw std::runtime_error("unsafe task accepted");
        };
        const auto scenario = application.arguments().value(1);
        if (scenario == "many-files") {
            QJsonArray inputs;
            for (int index = 0; index < 540; ++index) {
                const QString filePath = temporary.filePath(QString::number(index) + ".sql");
                const QByteArray content("BEGIN NULL; END;\n/\n");
                write(filePath, content);
                inputs.append(file(filePath, content));
            }
            task.insert("files", inputs);
            const auto prepared = prepare(task);
            require(prepared.files.size() == 540 && prepared.units.size() == 540,
                    "retain and plan every small input without maximum-size allocations");
        } else if (scenario == "single-file") {
            const auto prepared = prepare(task);
            require(prepared.units.size() == 2 && prepared.objects.size() == 2, "slash-delimited package units");
            require(CommandScript::compiledObject(prepared.units[1].text, prepared.schema)->type == "PACKAGE BODY",
                    "spec and body distinct");
        } else if (scenario == "hash") {
            auto bad = task;
            bad.insert("files", QJsonArray{file(root, "different")});
            rejects(bad);
        } else if (scenario == "scope") {
            write(root, "@@child.sql\n");
            write(child, sql);
            auto included = task;
            included.insert("files", QJsonArray{file(root, "@@child.sql\n")});
            rejects(included);
            included.insert("referenced_files", QJsonArray{file(child, sql)});
            require(prepare(included).units.size() == 2, "approved include");
        } else if (scenario == "snapshot") {
            const QMap<QString, QByteArray> snapshot{{QFileInfo(root).canonicalFilePath(), sql}};
            write(root, "DROP TABLE MUST_NOT_EXECUTE;\n");
            const auto plan = CommandScript::planVerifiedFiles({root}, snapshot, temporary.path());
            const auto chunk = CommandScript::readPlanChunk(plan.planPath, 0);
            QFile::remove(plan.planPath);
            require(plan.errors.isEmpty() && chunk.error.isEmpty() && chunk.units.size() == 2
                    && !chunk.units[0].text.contains("DROP TABLE"), "immutable bytes after hash validation");
        } else if (scenario == "variables") {
            const QByteArray rootSql("DEFINE P = P\n@@child.sql &P\n");
            const QByteArray childSql("CREATE OR REPLACE PACKAGE &1 AS PROCEDURE T; END;\n/\n");
            write(root, rootSql);
            write(child, childSql);
            task.insert("files", QJsonArray{file(root, rootSql)});
            task.insert("referenced_files", QJsonArray{file(child, childSql)});
            const auto prepared = prepare(task);
            require(prepared.units.last().text.contains("PACKAGE P AS"), "include positional expansion");
            require(prepared.units.last().source.file == QFileInfo(child).canonicalFilePath(), "included source path");
        } else if (scenario == "nested-declarations") {
            const QByteArray input("DECLARE\n"
                "PROCEDURE check_tablespace IS\n"
                "FUNCTION verify_ts RETURN BOOLEAN IS BEGIN IF 1=1 THEN RETURN TRUE; END IF; END;\n"
                "BEGIN IF NOT verify_ts THEN NULL; END IF; END check_tablespace;\n"
                "FUNCTION another RETURN NUMBER;\n"
                "FUNCTION another RETURN NUMBER IS BEGIN RETURN 1; END;\n"
                "BEGIN check_tablespace; END;\n/\nCOMMIT;\n");
            write(root, input);
            task.insert("files", QJsonArray{file(root, input)});
            const auto prepared = prepare(task);
            require(prepared.units.size() == 2, "nested declaration routines must stay in one anonymous block");
            require(prepared.units[0].text.contains("END check_tablespace;")
                    && prepared.units[0].text.contains("BEGIN check_tablespace; END;"),
                "anonymous block must retain local routine bodies and its main BEGIN");
            require(prepared.units[1].text.trimmed().startsWith("COMMIT"), "following transaction remains separate");
        } else if (scenario == "prompt-apostrophe") {
            const QString input = QStringLiteral("PROMPT Person's initialization\nPROMPT *****\n"
                "DECLARE\nPROCEDURE Attribute_Row_ IS BEGIN NULL; END;\n"
                "BEGIN Attribute_Row_; END;\n/\nCOMMIT;\n");
            const auto units = CommandScript::parse(input, root);
            require(units.size() == 4 && units[0].type == CommandUnitType::Prompt
                    && units[1].type == CommandUnitType::Prompt
                    && units[2].text.contains("BEGIN Attribute_Row_; END;"),
                "PROMPT apostrophe must not enter SQL quote state or split local routines");
        } else if (scenario == "alternative-quotes") {
            const QString simple = QStringLiteral("SELECT q'[a'b; END;]' FROM DUAL;\nSELECT 2;\n");
            const auto simpleUnits = CommandScript::parse(simple, root);
            require(simpleUnits.size() == 2 && simpleUnits[0].text == simple.section('\n', 0, 0).chopped(1),
                "q quote containing apostrophe and semicolons must not split ordinary SQL");
            for (const QByteArray delimiter : {QByteArray("[]"), QByteArray("{}"),
                    QByteArray("()"), QByteArray("<>"), QByteArray("!!"), QByteArray("\"\""), QByteArray("''")}) {
                for (const QByteArray prefix : {QByteArray("q"), QByteArray("Q"), QByteArray("nq")}) {
                    const QByteArray literal = prefix + "'" + delimiter.left(1)
                        + "Person's; ' unmatched\nPROMPT must stay literal\n/\nEND inner; -- &VALUE\n"
                        + delimiter.right(1) + "'";
                    const QByteArray block = "DECLARE\nx VARCHAR2(32767) := " + literal
                        + ";\nPROCEDURE Attribute_Row_ IS BEGIN NULL; END;\n"
                        "BEGIN Attribute_Row_; END;\n/\nCOMMIT;\n";
                    const auto units = CommandScript::parse(QString::fromUtf8(block), root);
                    require(units.size() == 2 && units[0].type == CommandUnitType::Sql
                            && units[0].text.contains(QString::fromUtf8(literal))
                            && units[0].text.contains("BEGIN Attribute_Row_; END;"),
                        "q quote text and local routine must remain one unchanged SQL block");
                    require(!CommandScript::sqlWithoutClientDelimiter(units[0].text).trimmed().endsWith("/"),
                        "real trailing slash must be removed after q quote closes");
                    require(CommandScript::variableReferences(QString::fromUtf8(literal), true).size() == 1,
                        "substitution remains active inside q quote even after literal comment marker");
                }
            }
            const auto negative = CommandScript::parse(QStringLiteral(
                "-- q'[comment with apostrophe\nSELECT 'q''[ordinary]'';';\n"
                "/* q'{also comment */\nSELECT 2;\n"), root);
            require(negative.size() == 2, "q markers in comments and ordinary quoted text are not delimiters");
        } else if (scenario == "parse-input") {
            QFile input(application.arguments().value(2));
            require(input.open(QIODevice::ReadOnly), "open actual parser input");
            const auto units = CommandScript::parse(QString::fromUtf8(input.readAll()), input.fileName());
            QJsonArray rows;
            for (const auto& unit : units) rows.append(QJsonObject{{"line", unit.source.line},
                {"kind", static_cast<int>(unit.type)}, {"text", unit.text}});
            std::cout << QJsonDocument(rows).toJson(QJsonDocument::Compact).constData() << std::endl;
        } else if (scenario == "define-quotes") {
            const QByteArray rootSql("DEFINE MODULE = 'FINCFA' -- registration component\n"
                "DEFINE PERSON = 'O''Brien'\nDEFINE EMPTY = ''\nDEFINE DOUBLE = \"ABC\"\n@@child.sql &MODULE\n");
            const QByteArray childSql("CREATE OR REPLACE PACKAGE P AS\n"
                "m CONSTANT VARCHAR2(30) := '&1';\n"
                "n CONSTANT VARCHAR2(30) := '&PERSON';\n"
                "e CONSTANT VARCHAR2(30) := '&EMPTY';\n"
                "d CONSTANT VARCHAR2(30) := '&DOUBLE';\nEND;\n/\n");
            write(root, rootSql);
            write(child, childSql);
            task.insert("files", QJsonArray{file(root, rootSql)});
            task.insert("referenced_files", QJsonArray{file(child, childSql)});
            const auto prepared = prepare(task);
            const auto expanded = prepared.units.last().text;
            require(expanded.contains(":= 'FINCFA';") && !expanded.contains("''FINCFA''"),
                "single-quoted DEFINE values must not duplicate SQL quotes");
            require(expanded.contains(":= 'O''Brien';"), "embedded SQL quote escapes must remain literal");
            require(expanded.contains(":= '';"), "empty DEFINE values must expand to empty text");
            require(expanded.contains(":= 'ABC';"), "double-quoted DEFINE behavior must remain compatible");
        } else if (scenario == "noninteractive") {
            for (const QByteArray input : {QByteArray("ACCEPT X\nSELECT 1 FROM DUAL;\n"),
                    QByteArray("SET FEEDBACK OFF\nSELECT 1 FROM DUAL;\n"),
                    QByteArray("SHOW ERROR\nSELECT 1 FROM DUAL;\n"),
                    QByteArray("SET VERIFY MAYBE\nSELECT 1 FROM DUAL;\n"),
                    QByteArray("SET SERVEROUTPUT ON FORMAT UNKNOWN\nSELECT 1 FROM DUAL;\n"),
                    QByteArray("SET SERVEROUTPUT ON FORMAT WRAPPED EXTRA\nSELECT 1 FROM DUAL;\n"),
                    QByteArray("SELECT &UNKNOWN FROM DUAL;\n")}) {
                write(root, input);
                task.insert("files", QJsonArray{file(root, input)});
                rejects(task);
            }
            for (const QByteArray input : {QByteArray("SET VERIFY OFF\nSELECT 1 FROM DUAL;\n"),
                    QByteArray("SET VERIFY ON\nSELECT 1 FROM DUAL;\n")}) {
                write(root, input);
                task.insert("files", QJsonArray{file(root, input)});
                require(prepare(task).units.size() == 2, "VERIFY is a known display-only command");
            }
            for (const QByteArray input : {
                    QByteArray("SET SERVEROUTPUT ON FORMAT WRAPPED\nSELECT 1 FROM DUAL;\n"),
                    QByteArray("SET SERVEROUTPUT ON SIZE UNLIMITED FORMAT WORD_WRAPPED\nSELECT 1 FROM DUAL;\n"),
                    QByteArray("SET SERVEROUTPUT OFF FORMAT TRUNCATED\nSELECT 1 FROM DUAL;\n")}) {
                write(root, input);
                task.insert("files", QJsonArray{file(root, input)});
                const auto prepared = prepare(task);
                require(prepared.units.size() == 2
                    && prepared.units.first().type == CommandUnitType::SetServerOutput,
                    "SERVEROUTPUT FORMAT controls display without becoming SQL");
            }
        } else if (scenario == "policy") {
            auto bad = task;
            bad.insert("unexpected", true);
            rejects(bad);
            bad = task;
            bad.insert("schema", "OTHER_OWNER");
            rejects(bad);
            bad = task;
            for (double invalid : {0.0, 9.0, 10.0, 15.0, 16.0, 20.0, 1.5, -1.0}) {
                bad.insert("execution", QJsonObject{{"parallelism", invalid}});
                rejects(bad);
            }
            bad.insert("execution", QJsonObject{{"stop_on_error", "false"}});
            rejects(bad);
            bad.insert("execution", false);
            rejects(bad);
        } else if (scenario == "defaults") {
            task.remove("execution");
            const auto defaults = prepare(task);
            require(defaults.parallelism == 5 && !defaults.stopOnError && defaults.compile,
                    "default five workers, continue errors, compile expected objects");
            task.insert("execution", QJsonObject{{"parallelism", 7}, {"stop_on_error", true}});
            const auto explicitPolicy = prepare(task);
            require(explicitPolicy.parallelism == 7 && explicitPolicy.stopOnError, "explicit stop policy");
            task.insert("execution", QJsonObject{{"parallelism", 8}, {"initialize_fnd_user", true}});
            bool rejected = false;
            try { prepare(task); } catch (const std::exception&) { rejected = true; }
            require(rejected, "Community tasks must reject private session initialization");
            task.insert("execution", QJsonObject{{"parallelism", 2}, {"stop_on_error", false}});
            require(prepare(task).parallelism == 2 && !prepare(task).stopOnError, "parallel continue policy");
        } else if (scenario == "batch-plan" || scenario == "schedule-stop" || scenario == "session-barrier") {
            Publication::Task batch;
            batch.schema = "APP_TEST";
            batch.parallelism = 3;
            const auto append = [&](const QString& source, const QString& sql) {
                batch.units += CommandScript::parse(sql, source);
            };
            append("a.svc", "CREATE OR REPLACE PACKAGE P AS PROCEDURE T; END;\n/\n");
            append("b.svc", "CREATE OR REPLACE PACKAGE Q AS PROCEDURE T; END;\n/\n");
            // Different file, same object: must wait for the first specification.
            append("body.svc", "CREATE OR REPLACE PACKAGE BODY P AS END;\n/\n");
            if (scenario == "session-barrier") {
                append("transaction.sql", "-- keep session\nCOMMIT;\n");
                const auto plan = Publication::planBatch(batch);
                require(plan.requiresSameSession && plan.workers == 1, "explicit transaction preserves session");
            } else {
                Publication::Schedule scheduler(Publication::planBatch(batch));
                require(scheduler.takeReady() == 0 && scheduler.takeReady() == 1, "independent files start together");
                require(scheduler.takeReady() == -1, "same object cannot publish concurrently");
                if (scenario == "schedule-stop") {
                    scheduler.stop();
                    scheduler.complete(0);
                    scheduler.complete(1);
                    require(scheduler.active() == 0 && scheduler.takeReady() == -1,
                            "stop drains active jobs and preserves pending jobs");
                } else {
                    scheduler.complete(0);
                    require(scheduler.takeReady() == 2, "body follows its specification");
                    scheduler.complete(1);
                    scheduler.complete(2);
                    require(scheduler.active() == 0, "all file jobs drained");
                }
            }
        } else if (scenario == "exclusive-barrier") {
            Publication::Task batch;
            batch.schema = "APP_TEST";
            batch.parallelism = 3;
            for (const QString& name : {QStringLiteral("P"), QStringLiteral("Q")})
                batch.units += CommandScript::parse("CREATE OR REPLACE PACKAGE " + name
                    + " AS PROCEDURE T; END;\n/\n", name + ".svc");
            batch.units += CommandScript::parse("CREATE TABLE C(ID INT);\n", "unknown-write.sql");
            batch.units += CommandScript::parse("CREATE OR REPLACE PACKAGE R AS END;\n/\n", "r.svc");
            Publication::Schedule scheduler(Publication::planBatch(batch));
            require(scheduler.takeReady() == 0 && scheduler.takeReady() == 1 && scheduler.takeReady() == -1,
                    "later work does not pass an exclusive write");
            scheduler.complete(0);
            scheduler.complete(1);
            require(scheduler.takeReady() == 2 && scheduler.takeReady() == -1, "exclusive write runs alone");
            scheduler.complete(2);
            require(scheduler.takeReady() == 3, "following work resumes after barrier");
        } else if (scenario == "output-context") {
            Publication::Task batch;
            batch.units += CommandScript::parse("SET SERVEROUTPUT ON\n", "list.sql");
            batch.units += CommandScript::parse("SELECT 1 FROM DUAL;\n", "a.sql");
            batch.units += CommandScript::parse("SET SERVEROUTPUT OFF\n", "list.sql");
            batch.units += CommandScript::parse("SELECT 2 FROM DUAL;\n", "b.sql");
            const auto plan = Publication::planBatch(batch);
            require(plan.jobs.size() == 4 && plan.jobs[1].initialServerOutput
                    && !plan.jobs[3].initialServerOutput, "per-file server output inheritance");
            Publication::Schedule scheduler(plan);
            require(scheduler.takeReady() == 0 && scheduler.takeReady() == 1 && scheduler.takeReady() == 3,
                    "later invocation of the same file cannot overlap");
        } else if (scenario == "worker-affinity" || scenario == "worker-limit") {
            struct Session { std::thread::id thread = std::this_thread::get_id(); };
            std::atomic_int logins{0};
            std::mutex mutex;
            std::condition_variable changed;
            int active = 0;
            bool release = false;
            const bool atLimit = scenario == "worker-limit";
            const int count = atLimit ? Publication::maximumFileParallelism : 3;
            if (atLimit) {
                bool rejected = false;
                try {
                    PowerCompileWorkers<Session> invalid(count + 1,
                        [](int) { return std::make_unique<Session>(); }, count);
                } catch (const std::invalid_argument&) { rejected = true; }
                require(rejected, "worker count above the supported limit must be rejected");
            }
            PowerCompileWorkers<Session> workers(count, [&](int) {
                ++logins;
                return std::make_unique<Session>();
            }, atLimit ? Publication::maximumFileParallelism : 4);
            require(logins == count, "all session factories finish before dispatch");
            std::vector<std::future<bool>> futures;
            for (int index = 0; index < count; ++index) futures.push_back(workers.submit([&](Session& session, int) {
                std::unique_lock<std::mutex> lock(mutex);
                ++active;
                changed.notify_all();
                changed.wait(lock, [&] { return release; });
                return session.thread == std::this_thread::get_id();
            }));
            bool concurrent = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                concurrent = changed.wait_for(lock, std::chrono::seconds(5), [&] { return active == count; });
                release = true;
            }
            changed.notify_all();
            for (auto& future : futures) require(future.get(), "session thread affinity");
            require(concurrent, "all requested file workers execute concurrently");
        } else if (scenario == "shared-compile") {
            require(CommandDiagnostics::knownStatus("VALID") && CommandDiagnostics::knownStatus("INVALID")
                    && CommandDiagnostics::knownStatus("NOT_FOUND"), "known status states");
            require(!CommandDiagnostics::knownStatus("UNKNOWN") && !CommandDiagnostics::knownStatus("")
                    && !CommandDiagnostics::knownStatus("DISABLED"), "unknown status cannot authorize replacement");
            const CommandCompiledObject specification{"APP_TEST", "P", "PACKAGE"};
            const CommandCompiledObject body{"APP_TEST", "P", "PACKAGE BODY"};
            require(CommandScript::compileObjectSql(specification) == CommandScript::compileObjectSql(body),
                    "DM package compilation covers both types");
            const auto queries = CommandDiagnostics::queries(specification);
            require(!queries.join(' ').contains("ORACLE_USER_ERRORS")
                    && queries.first().contains("ORDER BY SEQUENCE"), "preserve DM spec compatibility");
        } else throw std::runtime_error("unknown scenario");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
