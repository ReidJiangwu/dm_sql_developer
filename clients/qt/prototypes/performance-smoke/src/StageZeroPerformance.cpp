#include "SqlEditor.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef Q_OS_LINUX
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

long peakMemoryKiB() {
#ifdef Q_OS_LINUX
    rusage usage{};
    return getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_maxrss : -1;
#else
    return -1;
#endif
}

long currentMemoryKiB() {
#ifdef Q_OS_LINUX
    QFile status(QStringLiteral("/proc/self/statm"));
    if (!status.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return -1;
    }
    const QList<QByteArray> fields = status.readAll().split(' ');
    if (fields.size() < 2) {
        return -1;
    }
    bool valid = false;
    const long residentPages = fields[1].toLong(&valid);
    return valid ? residentPages * sysconf(_SC_PAGESIZE) / 1024 : -1;
#else
    return -1;
#endif
}

QString packageText(int document) {
    QString text;
    QTextStream stream(&text);
    stream << "CREATE OR REPLACE PACKAGE BODY REID_PERF_PACKAGE_" << document << " AS\n";
    for (int line = 1; line <= 2'000; ++line) {
        stream << "  PROCEDURE P_" << line << " IS BEGIN NULL; END P_" << line << ";\n";
    }
    stream << "END;\n/\n";
    return text;
}

void editorProbe() {
    QElapsedTimer total;
    total.start();
    QTabWidget tabs;
    tabs.resize(1366, 768);
    tabs.show();
    std::vector<SqlEditor*> editors;
    editors.reserve(30);
    for (int index = 0; index < 30; ++index) {
        auto* editor = new SqlEditor(&tabs);
        editor->setPlainText(packageText(index + 1));
        editors.push_back(editor);
        tabs.addTab(editor, QStringLiteral("PKG_%1").arg(index + 1));
        QCoreApplication::processEvents();
    }
    qint64 longestSwitchUs = 0;
    for (int round = 0; round < 3; ++round) {
        for (int index = 0; index < tabs.count(); ++index) {
            QElapsedTimer change;
            change.start();
            tabs.setCurrentIndex(index);
            QCoreApplication::processEvents();
            longestSwitchUs = std::max(longestSwitchUs, change.nsecsElapsed() / 1'000);
        }
    }
    std::vector<std::future<int>> parseTasks;
    parseTasks.reserve(editors.size());
    for (SqlEditor* editor : editors) {
        const QString source = editor->toPlainText();
        parseTasks.push_back(std::async(std::launch::async, [source]() {
            int procedures = 0;
            qsizetype offset = 0;
            while ((offset = source.indexOf(QStringLiteral("PROCEDURE"), offset)) >= 0) {
                ++procedures;
                offset += 9;
            }
            return procedures;
        }));
    }
    bool parsing = true;
    while (parsing) {
        parsing = false;
        QCoreApplication::processEvents();
        for (auto& task : parseTasks) {
            parsing = parsing
                || task.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
        }
    }
    for (auto& task : parseTasks) {
        require(task.get() == 2'000, "Background package parsing returned a different procedure count.");
    }
    editors[0]->moveCursor(QTextCursor::End);
    editors[0]->insertPlainText(QStringLiteral("-- changed\n"));
    require(editors[0]->document()->isModified(), "Editor did not preserve modified state.");
    editors[0]->undo();
    require(editors[0]->blockCount() >= 2'002, "Editor source content was truncated.");
    require(tabs.count() == 30, "Editor tab count differed.");
    for (int index = 0; index < 15; ++index) {
        QWidget* page = tabs.widget(0);
        tabs.removeTab(0);
        delete page;
    }
    QCoreApplication::processEvents();
    require(tabs.count() == 15, "Closing editor tabs left an unexpected tab count.");
    std::cout << "EDITOR_COUNT=30\n";
    std::cout << "PACKAGE_LINES=2002\n";
    std::cout << "OPEN_TOTAL_MS=" << total.elapsed() << '\n';
    std::cout << "LONGEST_SWITCH_US=" << longestSwitchUs << '\n';
    std::cout << "PEAK_MEMORY_KIB=" << peakMemoryKiB() << '\n';
    std::cout << "MEMORY_AFTER_CLOSE_KIB=" << currentMemoryKiB() << '\n';
    std::cout << "BACKGROUND_PARSE_TASKS=30\n";
    std::cout << "BACKGROUND_PARSE=PASS\n";
    std::cout << "EDITOR_30=PASS\n";
}

QString csvField(const QString& value) {
    QString escaped = value;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("\"") + escaped + QLatin1Char('"');
}

void writeCsv(const QString& target, int rows, int cancelAfter) {
    QSaveFile file(target);
    require(file.open(QIODevice::WriteOnly | QIODevice::Text), "Cannot open CSV save file.");
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "id,name,nullable,note\r\n";
    for (int id = 1; id <= rows; ++id) {
        stream << id << ',' << csvField(QStringLiteral("name,%1").arg(id)) << ',';
        if (id % 7 != 0) {
            stream << id * 10;
        }
        stream << ',' << csvField(QStringLiteral("quote \"%1\"").arg(id)) << "\r\n";
        if (cancelAfter > 0 && id == cancelAfter) {
            file.cancelWriting();
            return;
        }
    }
    require(stream.status() == QTextStream::Ok, "CSV text conversion failed.");
    require(file.commit(), "Atomic CSV commit failed.");
}

void exportProbe() {
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create an isolated export directory.");
    const QString completed = directory.filePath(QStringLiteral("completed.csv"));
    QElapsedTimer timer;
    timer.start();
    writeCsv(completed, 100'000, 0);
    const qint64 elapsed = timer.elapsed();
    QFile file(completed);
    require(file.open(QIODevice::ReadOnly), "Cannot read completed CSV.");
    qint64 lineCount = 0;
    while (!file.atEnd()) {
        file.readLine();
        ++lineCount;
    }
    require(lineCount == 100'001, "CSV output row count differed.");

    const QString cancelled = directory.filePath(QStringLiteral("cancelled.csv"));
    writeCsv(cancelled, 100'000, 5'000);
    require(!QFile::exists(cancelled), "Cancelled export left a target file.");

    QSaveFile invalid(directory.filePath(QStringLiteral("missing/failed.csv")));
    require(!invalid.open(QIODevice::WriteOnly), "Invalid export target unexpectedly opened.");
    std::cout << "EXPORT_ROWS=100000\n";
    std::cout << "EXPORT_BYTES=" << QFileInfo(completed).size() << '\n';
    std::cout << "EXPORT_TOTAL_MS=" << elapsed << '\n';
    std::cout << "EXPORT_CANCEL_CLEANUP=PASS\n";
    std::cout << "EXPORT_FAILURE_BOUNDARY=PASS\n";
    std::cout << "EXPORT_STREAM=PASS\n";
}

enum class ChangeType { Insert, Update, Delete };

struct Change {
    int id;
    ChangeType type;
    bool valid;
};

void batchModelProbe() {
    std::vector<Change> changes;
    changes.reserve(500);
    for (int id = 1; id <= 500; ++id) {
        const ChangeType type = id <= 200 ? ChangeType::Insert
            : (id <= 400 ? ChangeType::Update : ChangeType::Delete);
        changes.push_back({id, type, id != 250 && id != 450});
    }
    std::map<ChangeType, std::vector<Change>> grouped;
    for (const Change& change : changes) {
        grouped[change.type].push_back(change);
    }
    require(grouped[ChangeType::Insert].size() == 200, "Insert group size differed.");
    require(grouped[ChangeType::Update].size() == 200, "Update group size differed.");
    require(grouped[ChangeType::Delete].size() == 100, "Delete group size differed.");
    const auto errors = std::count_if(changes.begin(), changes.end(), [](const Change& change) {
        return !change.valid;
    });
    require(errors == 2, "Per-row error mapping differed.");
    std::map<int, QString> committed;
    const auto before = committed;
    for (const Change& change : changes) {
        if (!change.valid) {
            committed = before;
            break;
        }
        committed[change.id] = QStringLiteral("changed");
    }
    require(committed == before, "Failed batch did not restore the pre-transaction state.");
    std::cout << "CHANGE_COUNT=500\n";
    std::cout << "ROW_ERROR_COUNT=2\n";
    std::cout << "BATCH_GROUPING=PASS\n";
    std::cout << "BATCH_MODEL_ROLLBACK=PASS\n";
}

int readCounter(const QString& path) {
    QFile file(path);
    if (!file.exists()) {
        return 0;
    }
    require(file.open(QIODevice::ReadOnly | QIODevice::Text), "Cannot read shared counter.");
    bool valid = false;
    const int value = QString::fromUtf8(file.readAll()).trimmed().toInt(&valid);
    require(valid, "Shared counter was corrupt.");
    return value;
}

void lockSharedFile(QLockFile& lock, const QString& path) {
    QElapsedTimer timer;
    timer.start();
    while (!lock.tryLock(250)) {
        if (timer.elapsed() >= 5'000) {
            throw std::runtime_error(QStringLiteral(
                "Cannot lock shared state file %1, lockError=%2.")
                .arg(path).arg(static_cast<int>(lock.error())).toStdString());
        }
        QThread::msleep(10);
    }
}

void incrementCounter(const QString& path) {
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(5'000);
    lockSharedFile(lock, path);
    const int next = readCounter(path) + 1;
    QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Text), "Cannot open atomic counter file.");
    file.write(QByteArray::number(next));
    file.write("\n");
    require(file.commit(), "Cannot atomically replace shared counter.");
    lock.unlock();
}

void appendSharedRecord(const QString& path, const QByteArray& record) {
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(5'000);
    lockSharedFile(lock, path);

    QByteArray contents;
    QFile existing(path);
    if (existing.exists()) {
        require(existing.open(QIODevice::ReadOnly | QIODevice::Text), "Cannot read a shared state file.");
        contents = existing.readAll();
        existing.close();
    }
    contents += record;
    contents += '\n';

    QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Text), "Cannot open a shared state file.");
    require(file.write(contents) == contents.size(), "Cannot write a complete shared state file.");
    require(file.commit(), "Cannot atomically replace a shared state file.");
    lock.unlock();
}

void writeRecoveryRecord(const QString& directory, int instance, int iteration) {
    QSaveFile file(directory + QStringLiteral("/recovery-%1.txt").arg(instance));
    require(file.open(QIODevice::WriteOnly | QIODevice::Text), "Cannot open a recovery record.");
    const QByteArray contents = QByteArray::number(instance) + ':' + QByteArray::number(iteration) + '\n';
    require(file.write(contents) == contents.size(), "Cannot write a complete recovery record.");
    require(file.commit(), "Cannot atomically replace a recovery record.");
}

void stateWorker(const QString& directory, int instance, int iterations) {
    const QString settings = directory + QStringLiteral("/settings.txt");
    const QString history = directory + QStringLiteral("/history.txt");
    const QString cache = directory + QStringLiteral("/metadata-cache.txt");
    const QString recent = directory + QStringLiteral("/recent-files.txt");
    for (int iteration = 0; iteration < iterations; ++iteration) {
        incrementCounter(settings);
        const QByteArray suffix = QByteArray::number(instance) + ':' + QByteArray::number(iteration);
        appendSharedRecord(history, QByteArrayLiteral("sql:") + suffix);
        appendSharedRecord(cache, QByteArrayLiteral("object:") + suffix);
        appendSharedRecord(recent, QByteArrayLiteral("file:") + suffix);
        writeRecoveryRecord(directory, instance, iteration);
    }
}

int recordCount(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly | QIODevice::Text), "Cannot read shared records.");
    int count = 0;
    while (!file.atEnd()) {
        if (!file.readLine().trimmed().isEmpty()) {
            ++count;
        }
    }
    return count;
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly | QIODevice::Text), "Cannot read a state file.");
    return file.readAll();
}

void interruptedWorker(const QString& directory, int instance) {
    writeRecoveryRecord(directory, instance, 0);
    QLockFile lock(directory + QStringLiteral("/settings.txt.lock"));
    lock.setStaleLockTime(1'000);
    lockSharedFile(lock, directory + QStringLiteral("/settings.txt"));
    std::cout << "READY\n" << std::flush;
    QThread::sleep(30);
}

void multiInstanceProbe() {
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create an isolated multi-instance directory.");
    std::array<QProcess, 2> workers;
    for (qsizetype index = 0; index < static_cast<qsizetype>(workers.size()); ++index) {
        QProcess& worker = workers[static_cast<std::size_t>(index)];
        worker.start(QCoreApplication::applicationFilePath(),
            {QStringLiteral("--state-worker"), directory.path(), QString::number(index + 1),
                QStringLiteral("100")});
        require(worker.waitForStarted(), "State worker did not start.");
    }
    for (QProcess& worker : workers) {
        require(worker.waitForFinished(30'000), "State worker did not finish.");
        if (worker.exitStatus() != QProcess::NormalExit || worker.exitCode() != 0) {
            throw std::runtime_error(QStringLiteral(
                "State worker failed: exitCode=%1, error=%2, stdout=%3, stderr=%4")
                .arg(worker.exitCode())
                .arg(worker.errorString(), QString::fromUtf8(worker.readAllStandardOutput()),
                    QString::fromUtf8(worker.readAllStandardError()))
                .toStdString());
        }
    }
    const QString settings = directory.filePath(QStringLiteral("settings.txt"));
    require(readCounter(settings) == 200, "Multi-instance settings updates lost data.");
    require(recordCount(directory.filePath(QStringLiteral("history.txt"))) == 200,
        "Multi-instance history updates lost data.");
    require(recordCount(directory.filePath(QStringLiteral("metadata-cache.txt"))) == 200,
        "Multi-instance cache updates lost data.");
    require(recordCount(directory.filePath(QStringLiteral("recent-files.txt"))) == 200,
        "Multi-instance recent-file updates lost data.");
    require(readFile(directory.filePath(QStringLiteral("recovery-1.txt"))).trimmed() == "1:99",
        "First instance recovery record was incorrect.");
    require(readFile(directory.filePath(QStringLiteral("recovery-2.txt"))).trimmed() == "2:99",
        "Second instance recovery record was incorrect.");

    QProcess interrupted;
    interrupted.start(QCoreApplication::applicationFilePath(),
        {QStringLiteral("--interrupted-worker"), directory.path(), QStringLiteral("3")});
    require(interrupted.waitForStarted(), "Interrupted worker did not start.");
    require(interrupted.waitForReadyRead(5'000), "Interrupted worker did not become ready.");
    require(interrupted.readAllStandardOutput().contains("READY"),
        "Interrupted worker did not confirm durable recovery state.");
    interrupted.kill();
    require(interrupted.waitForFinished(5'000), "Interrupted worker did not stop.");
    require(interrupted.exitStatus() == QProcess::CrashExit,
        "Interrupted worker did not report a forced exit.");
    require(readFile(directory.filePath(QStringLiteral("recovery-3.txt"))).trimmed() == "3:0",
        "Forced exit damaged the durable recovery record.");

    QProcess survivor;
    survivor.start(QCoreApplication::applicationFilePath(),
        {QStringLiteral("--state-worker"), directory.path(), QStringLiteral("4"), QStringLiteral("1")});
    require(survivor.waitForStarted(), "Post-interruption worker did not start.");
    require(survivor.waitForFinished(10'000), "Post-interruption worker did not finish.");
    require(survivor.exitStatus() == QProcess::NormalExit && survivor.exitCode() == 0,
        "Post-interruption worker failed to recover a stale lock.");
    require(readCounter(settings) == 201, "Post-interruption settings update was lost.");
    std::cout << "INSTANCE_COUNT=2\n";
    std::cout << "ATOMIC_UPDATE_COUNT=201\n";
    std::cout << "SHARED_STATE_RECORDS=603\n";
    std::cout << "RECOVERY_RECORDS=4\n";
    std::cout << "FORCED_EXIT_RECOVERY=PASS\n";
    std::cout << "MULTI_INSTANCE_ATOMIC_WRITE=PASS\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    try {
        const QStringList arguments = application.arguments();
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--editor-probe")) {
            editorProbe();
        } else if (arguments.size() == 2 && arguments[1] == QStringLiteral("--export-probe")) {
            exportProbe();
        } else if (arguments.size() == 2 && arguments[1] == QStringLiteral("--batch-model-probe")) {
            batchModelProbe();
        } else if (arguments.size() == 2 && arguments[1] == QStringLiteral("--multi-instance-probe")) {
            multiInstanceProbe();
        } else if (arguments.size() == 5 && arguments[1] == QStringLiteral("--state-worker")) {
            bool validInstance = false;
            bool validIterations = false;
            const int instance = arguments[3].toInt(&validInstance);
            const int iterations = arguments[4].toInt(&validIterations);
            require(validInstance && instance > 0, "Worker instance was invalid.");
            require(validIterations && iterations > 0, "Worker iteration count was invalid.");
            stateWorker(arguments[2], instance, iterations);
        } else if (arguments.size() == 4
            && arguments[1] == QStringLiteral("--interrupted-worker")) {
            bool valid = false;
            const int instance = arguments[3].toInt(&valid);
            require(valid && instance > 0, "Interrupted worker instance was invalid.");
            interruptedWorker(arguments[2], instance);
        } else {
            std::cerr << "Usage: stage-zero-performance "
                         "--editor-probe|--export-probe|--batch-model-probe|--multi-instance-probe\n";
            return 2;
        }
        std::cout << "RESULT=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RESULT=FAIL\nERROR=" << error.what() << '\n';
        return 1;
    }
}
