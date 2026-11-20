#pragma once
#include "../src/ui/InterfaceIcons.h"

// Offline visual audit: instantiate existing pages without attaching a session.
// Never invoke execution, persistence, installation, or database actions.
#include "../src/ui/ConnectionDialog.h"
#include "../src/ui/ConnectionFailureDialog.h"
#include "../src/ui/KeywordReferenceDialog.h"
#include "../src/ui/ResultExportDialog.h"
#include "../src/ui/ResultContentDialog.h"
#include "../src/ui/HelpDocumentDialog.h"
#include "../src/ui/VersionNotesDialog.h"
#include "../src/ui/PackageBodyBackupsDialog.h"
#include "../src/ui/ObjectDropConfirmationDialog.h"
#include "../src/ui/TestDataDialog.h"
#include "../src/ui/ProgramDocumentWidget.h"
#include "../src/ui/ViewDocumentWidget.h"
#include "../src/ui/ObjectDetailsWidget.h"
#include "../src/ui/ObjectListWidget.h"
#include "../src/ui/ObjectDdlWidget.h"
#include "../src/ui/UserEditorDocumentWidget.h"
#include "../src/ui/RoleEditorDocumentWidget.h"
#include "../src/ui/InvalidObjectsWidget.h"
#include "../src/ui/MissingPackageBodiesWidget.h"
#include "../src/ui/SqlDocumentWidget.h"
#include "../src/ui/OperationLogPanel.h"
#include "../src/ui/StartupSplash.h"
#include <QCryptographicHash>
#include <QAbstractButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QStackedWidget>
#include <QListWidget>
#include <QToolButton>
#include <QToolTip>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextImageFormat>
#include <QClipboard>
#include <QMimeData>
#include <QJsonArray>
#include <QMenuBar>
#include <QFontInfo>
#include <QInputDialog>
#include <QFileDialog>
#include <QColorDialog>
#include "../src/ui/DiagnosticTextInteraction.h"
#include "../src/ui/HistoryCleanupDialog.h"
#include "../src/ui/LegacyMigrationProgressDialog.h"
#include "../src/ui/SnippetPanel.h"

class UiAppearanceCatalogAccess {
public:
    static void showcaseSql(SqlDocumentWidget& document) {
        document.editor_->setPlainText(QStringLiteral(
            "-- 离线示例：以下数据仅用于展示界面\n"
            "SELECT id, name, status\n  FROM sample_items\n ORDER BY id;"));
        auto* model = document.modelForResult(0, {QStringLiteral("ID"), QStringLiteral("NAME"), QStringLiteral("STATUS")});
        QVector<QStringList> rows;
        for (int i = 1; i <= 8; ++i)
            rows.append({QString::number(i), QStringLiteral("示例记录 %1").arg(i, 2, 10, QLatin1Char('0')),
                i % 3 ? QStringLiteral("READY") : QStringLiteral("PENDING")});
        model->append(std::move(rows));
        model->setSourceExhausted();
        document.updateResultSummary();
        if (auto* pane = document.resultPanes_.value(0))
            pane->setStatus(QueryResultPane::Status::Ready, QStringLiteral("离线示例数据"));
        document.resultTabs_->setCurrentIndex(0);
        document.outputTabs_->setCurrentIndex(0);
        document.splitSizes_ = {240, 500};
        document.setOutputExpanded(true);
        document.resultSummaryLabel_->setText(QStringLiteral("离线示例 · 8 行 · 未连接数据库"));
    }
    static void showcaseHome(MainWindow& main) {
        main.showNavigationPage(main.windowNavigationPage_);
        if (auto* document = main.currentDocument()) {
            document->editor()->setPlainText(QStringLiteral("-- 社区版工作台 · 离线演示\nSELECT 1 AS sample_value FROM DUAL;\n"));
            document->editor()->setFocus();
        }
    }
    static OperationLogPanel* operationLog() {
        auto* panel = new OperationLogPanel;
        panel->table_->setRowCount(0);
        panel->records_.clear();
        for (int i = 0; i < 8; ++i) {
            OperationLogRecord record;
            record.executedAt = QDateTime::currentDateTime().addSecs(-i * 90);
            record.connectionName = QStringLiteral("离线示例连接（长名称）");
            record.connectionId = QStringLiteral("offline-example-connection");
            record.source = QStringLiteral("sql-window");
            record.schema = QStringLiteral("EXAMPLE");
            record.operation = QStringLiteral("SELECT");
            record.success = i % 2 != 0;
            record.sql = QStringLiteral("SELECT *\nFROM EXAMPLE.SAMPLE_TABLE;");
            record.error = record.success ? QString() : QStringLiteral("离线示例错误\n第二行完整诊断");
            panel->records_.append(record);
            panel->appendRecordRow(record);
        }
        panel->applyRowsHeader(8);
        panel->table_->selectRow(0);
        return panel;
    }
    static void systemInfo(MainWindow& main, const SystemInfoResult& result) {
        main.showSystemInfoResult(result);
    }
};

inline int runCommunityShowcase(QApplication& application, const QString& directory) {
    if (ProductFeatures::erp || ProductFeatures::internalHub || directory.isEmpty()) return 2;
    QDir output(directory);
    if (output.exists() && !output.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) return 2;
    if (!QDir().mkpath(directory)) return 2;
    QTemporaryDir settings;
    if (!settings.isValid()) return 2;
    QStandardPaths::setTestModeEnabled(true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    application.setQuitOnLastWindowClosed(false);
    Theme::apply(application, ThemeMode::Light);
    const auto settle = [](int milliseconds = 100) {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    };
    QJsonArray images;
    bool passed = true;
    const auto capture = [&](QWidget& widget, const QString& fileName, const QString& title) {
        const auto pixmap = widget.grab();
        const QString path = output.filePath(fileName);
        const bool saved = !pixmap.isNull() && pixmap.save(path);
        passed &= saved;
        QFile image(path);
        if (!saved || !image.open(QIODevice::ReadOnly)) { passed = false; return; }
        images.append(QJsonObject{{"file", fileName}, {"title", title},
            {"width", pixmap.width()}, {"height", pixmap.height()},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(image.readAll(), QCryptographicHash::Sha256).toHex())}});
    };
    {
        StartupSplash startup(application.applicationVersion());
        startup.show(); settle(StartupSplash::introMilliseconds + 350);
        capture(startup, QStringLiteral("startup.png"), QStringLiteral("启动页"));
        startup.close();
    }
    {
        MainWindow main(nullptr, false, true);
        UiAppearanceCatalogAccess::showcaseHome(main);
        main.resize(1280, 800); main.show(); settle();
        capture(main, QStringLiteral("home.png"), QStringLiteral("首页工作台"));
        main.hide();
    }
    {
        ConnectionDialog connection(nullptr, settings.filePath(QStringLiteral("showcase-connections.json")));
        connection.findChild<QLineEdit*>(QStringLiteral("connectionName"))->setText(QStringLiteral("示例开发库"));
        connection.findChild<QLineEdit*>(QStringLiteral("connectionHost"))->setText(QStringLiteral("db.example.test"));
        connection.findChild<QLineEdit*>(QStringLiteral("connectionUser"))->setText(QStringLiteral("APP_USER"));
        connection.resize(1000, 700); connection.show(); settle();
        capture(connection, QStringLiteral("connections.png"), QStringLiteral("连接管理"));
        connection.hide();
    }
    {
        SqlDocumentWidget sql(1);
        UiAppearanceCatalogAccess::showcaseSql(sql);
        sql.resize(1120, 740); sql.show(); settle();
        capture(sql, QStringLiteral("sql-results.png"), QStringLiteral("SQL 编辑与结果"));
        sql.hide();
    }
    {
        std::unique_ptr<OperationLogPanel> logs(UiAppearanceCatalogAccess::operationLog());
        logs->findChild<QTableWidget*>(QStringLiteral("operationLogTable"))->selectRow(1);
        logs->findChild<QPlainTextEdit*>(QStringLiteral("operationLogDetails"))->setFocus();
        logs->resize(1120, 740); logs->show(); settle();
        capture(*logs, QStringLiteral("operation-log.png"), QStringLiteral("操作日志"));
        logs->hide();
    }
    if (QFile::exists(QStringLiteral(":/help/images/startup.png"))) {
        HelpDocumentDialog help;
        auto* contents = help.findChild<QListWidget*>(QStringLiteral("helpDocumentContents"));
        auto* body = help.findChild<QTextBrowser*>(QStringLiteral("helpDocumentBody"));
        passed &= contents && body && contents->count() == 15;
        if (contents && body) {
            const QList<QPair<int, QString>> illustratedChapters = {
                {1, QStringLiteral("startup.png")}, {2, QStringLiteral("connections.png")},
                {3, QStringLiteral("home.png")}, {4, QStringLiteral("sql-results.png")},
                {11, QStringLiteral("operation-log.png")}};
            help.resize(1280, 900); help.show();
            for (const auto& chapter : illustratedChapters) {
                contents->setCurrentRow(chapter.first); settle();
                const auto resource = body->document()->resource(QTextDocument::ImageResource,
                    QUrl(QStringLiteral("images/") + chapter.second));
                passed &= resource.isValid() && !resource.isNull();
            }
            contents->setCurrentRow(1); settle();
            passed &= help.grab().save(output.filePath(QStringLiteral("help-preview.png")));
            contents->setCurrentRow(3);
            for (const QSize size : {QSize(680, 460), QSize(1280, 900), QSize(1040, 720)}) {
                help.resize(size); settle();
                int imageCount = 0;
                for (auto block = body->document()->begin(); block.isValid(); block = block.next()) {
                    for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
                        const auto text = fragment.fragment();
                        if (!text.isValid() || !text.charFormat().isImageFormat()) continue;
                        const auto image = text.charFormat().toImageFormat();
                        ++imageCount;
                        passed &= image.width() > 0 && image.width() <= body->viewport()->width()
                            && qAbs(image.width() / image.height() - 1.6) < 0.001;
                    }
                }
                passed &= imageCount == 1;
            }
            passed &= help.grab().save(output.filePath(QStringLiteral("help-home-preview.png")));
            help.hide();
        }
    }
    const QJsonObject manifest{{"edition", QStringLiteral("community")},
        {"version", application.applicationVersion()}, {"data", QStringLiteral("offline-fixtures")},
        {"renderer", QGuiApplication::platformName()}, {"images", images}};
    QFile metadata(output.filePath(QStringLiteral("showcase.json")));
    if (!metadata.open(QIODevice::WriteOnly) || metadata.write(QJsonDocument(manifest).toJson()) < 0) passed = false;
    QTextStream(passed ? stdout : stderr) << "COMMUNITY_SHOWCASE=" << (passed ? "PASS\n" : "FAIL\n");
    return passed && images.size() == 5 ? 0 : 1;
}

inline int runOperationLogPriorityCheck(QApplication& application) {
    QStandardPaths::setTestModeEnabled(true);
    bool passed = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { qWarning("Operation log priority: %s", message); passed = false; }
    };
    for (const auto mode : {ThemeMode::Light, ThemeMode::Dark}) {
        Theme::apply(application, mode);
        std::unique_ptr<OperationLogPanel> panel(UiAppearanceCatalogAccess::operationLog());
        auto* table = panel->findChild<QTableWidget*>(QStringLiteral("operationLogTable"));
        auto* details = panel->findChild<QPlainTextEdit*>(QStringLiteral("operationLogDetails"));
        check(table && details, "table and details must exist");
        if (!table || !details) continue;
        check(table->horizontalHeader()->logicalIndex(0) == 11
                && table->horizontalHeader()->logicalIndex(1) == 3
                && table->horizontalHeader()->logicalIndex(2) == 0, "SQL, connection, time must lead the table");
        for (int row : {0, 1}) {
            table->selectRow(row);
            const QString text = details->toPlainText();
            const QString prefix = QStringLiteral("SQL：%1\n\n连接名：%2\n时间：%3\n结果：%4\n")
                .arg(table->item(row, 11)->text(), table->item(row, 3)->text(),
                    table->item(row, 0)->text(), table->item(row, 9)->text());
            check(text.startsWith(prefix), "full multiline SQL, connection and time must lead success/failure details");
            for (int column = 0; column < table->columnCount(); ++column)
                check(text.contains(QStringLiteral("%1：%2").arg(table->horizontalHeaderItem(column)->text(),
                    table->item(row, column)->text())), "all original fields and multiline diagnostics must remain");
        }
        table->clearSelection();
        check(details->toPlainText().isEmpty(), "empty selection must clear details");
    }
    Theme::apply(application, ThemeMode::Light);
    QTextStream(passed ? stdout : stderr) << "OPERATION_LOG_PRIORITY=" << (passed ? "PASS\n" : "FAIL\n");
    return passed ? 0 : 1;
}

inline int runLicensePresentationCheck(QApplication& application) {
    QTemporaryDir settings;
    if (!settings.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    const bool quitOnClose = application.quitOnLastWindowClosed();
    application.setQuitOnLastWindowClosed(false);
    bool passed = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { qWarning("License presentation: %s", message); passed = false; }
    };
    MainWindow main(nullptr, false, false);
    SystemInfoResult result;
    result.kind = SystemInfoKind::License;
    result.columns = {QStringLiteral("HARDWARE_ID"), QStringLiteral("CPU_TYPE"),
        QStringLiteral("CLUSTER_TYPE"), QStringLiteral("EXPIRED_DATE"), QStringLiteral("SERIES_NO"),
        QStringLiteral("AUTHORIZED_CUSTOMER"), QStringLiteral("MAX_CPU_NUM"), QStringLiteral("EXTRA")};
    result.rows = {{QString(), QStringLiteral("X86"), QStringLiteral("1010"), QString(),
        QStringLiteral("OFFLINE-SAMPLE"), QStringLiteral("<示例> & 单位"), QStringLiteral("0"), QStringLiteral("  ")}};
    const QStringList expected{QStringLiteral("SERIES_NO"), QStringLiteral("AUTHORIZED_CUSTOMER"),
        QStringLiteral("CLUSTER_TYPE"), QStringLiteral("CPU_TYPE"), QStringLiteral("MAX_CPU_NUM"),
        QStringLiteral("EXPIRED_DATE"), QStringLiteral("HARDWARE_ID"), QStringLiteral("EXTRA")};
    const QStringList expectedTitles{QStringLiteral("许可证序列号"), QStringLiteral("授权单位"),
        QStringLiteral("集群授权标识"), QStringLiteral("授权 CPU 类型"), QStringLiteral("最大 CPU 数量"),
        QStringLiteral("授权到期日期"), QStringLiteral("硬件绑定编码"), QStringLiteral("EXTRA")};
    for (const auto mode : {ThemeMode::Light, ThemeMode::Dark}) {
        main.setThemeMode(mode);
        for (const int pointSize : {13, 20}) {
            QFont font = main.font(); font.setPointSize(pointSize); main.setFont(font);
            QTimer::singleShot(0, &main, [&]() {
                auto* dialog = qobject_cast<QDialog*>(application.activeModalWidget());
                check(dialog != nullptr, "missing modal dialog");
                if (!dialog) return;
                QStringList fields;
                QStringList titles;
                for (auto* label : dialog->findChildren<QLabel*>()) {
                    if (!label->property("systemInfoPropertyCaption").toBool()) continue;
                    fields.append(label->property("systemInfoField").toString());
                    titles.append(label->text());
                    check(label->toolTip() == fields.last(), "caption lost the original field tooltip");
                    check(!label->text().contains(QLatin1Char('\n')), "caption still occupies two lines");
                }
                check(fields == expected, "populated columns, zero or preferred order incorrect");
                check(titles == expectedTitles, "license fields did not use the approved Chinese names");
                check(dialog->findChildren<QToolButton*>(QStringLiteral("systemInfoPropertyHelp")).size() == 4,
                    "missing field help icons");
                check(!dialog->findChild<QLabel*>(QStringLiteral("secondaryText")),
                    "field explanation still occupies the main form");
                for (auto* help : dialog->findChildren<QToolButton*>(QStringLiteral("systemInfoPropertyHelp"))) {
                    check(!help->accessibleName().isEmpty() && !help->icon().isNull(), "inaccessible help icon");
                    if (help->property("systemInfoField").toString() != QStringLiteral("CLUSTER_TYPE")) continue;
                    QToolTip::hideText();
                    help->click();
                    check(QToolTip::text() == help->toolTip()
                            && help->toolTip().contains(QStringLiteral("DataWatch 数据守护：已授权"))
                            && help->toolTip().contains(QStringLiteral("MPP：未授权"))
                            && help->toolTip().contains(QStringLiteral("读写分离：已授权")),
                        "click did not show decoded cluster help");
                    QToolTip::hideText();
                }
                auto* copy = dialog->findChild<QPushButton*>(QStringLiteral("copySystemInfo"));
                auto* close = dialog->findChild<QPushButton*>(QStringLiteral("closeSystemInfo"));
                check(copy && close, "missing footer actions");
                if (copy) {
                    copy->click();
                    const auto* mime = QApplication::clipboard()->mimeData();
                    check(mime && mime->hasHtml() && mime->text().section(QLatin1Char('\n'), 0, 0)
                            == expectedTitles.join(QLatin1Char('\t'))
                            && mime->html().contains(QStringLiteral("&lt;示例&gt; &amp; 单位"))
                            && mime->text().contains(QStringLiteral("<示例> & 单位")),
                        "full copy lost raw values, empty columns, escaping or displayed order");
                }
                dialog->resize(520, 360);
                application.processEvents();
                check(close && dialog->rect().contains(close->geometry()), "narrow dialog clipped close");
                dialog->reject();
            });
            UiAppearanceCatalogAccess::systemInfo(main, result);
        }
    }
    const auto checkOrder = [&](const SystemInfoResult& fixture, const QStringList& order, const char* scenario,
                               const QStringList& expectedNames = {}) {
        QTimer::singleShot(0, &main, [&]() {
            auto* dialog = qobject_cast<QDialog*>(application.activeModalWidget());
            check(dialog != nullptr, scenario);
            if (!dialog) return;
            QStringList fields;
            QStringList titles;
            for (auto* label : dialog->findChildren<QLabel*>()) {
                if (label->property("systemInfoPropertyCaption").toBool()) {
                    fields.append(label->property("systemInfoField").toString());
                    titles.append(label->text());
                    check(fixture.columns.contains(label->toolTip()), "original field name tooltip was changed");
                }
            }
            check(fields == order, scenario);
            if (!expectedNames.isEmpty()) {
                check(titles == expectedNames, "Chinese field mapping or unknown-name fallback changed");
                auto* copy = dialog->findChild<QPushButton*>(QStringLiteral("copySystemInfo"));
                check(copy != nullptr, "missing full-copy action");
                if (copy) {
                    copy->click();
                    const auto* mime = QApplication::clipboard()->mimeData();
                    check(mime && mime->text().section(QLatin1Char('\n'), 0, 0)
                            == expectedNames.join(QLatin1Char('\t')),
                        "full-copy header differs from Chinese captions");
                    for (const auto& title : expectedNames)
                        check(mime && mime->hasHtml() && mime->html().contains(title.toHtmlEscaped()),
                            "HTML copy omitted a translated header");
                }
            }
            dialog->reject();
        });
        UiAppearanceCatalogAccess::systemInfo(main, fixture);
    };
    auto multiple = result;
    multiple.rows.append({QStringLiteral("SECOND-ROW-HARDWARE"), QStringLiteral(""), QStringLiteral("invalid")});
    QStringList multipleOrder{QStringLiteral("SERIES_NO"), QStringLiteral("AUTHORIZED_CUSTOMER"),
        QStringLiteral("CLUSTER_TYPE"), QStringLiteral("HARDWARE_ID"), QStringLiteral("CPU_TYPE"),
        QStringLiteral("MAX_CPU_NUM"), QStringLiteral("EXPIRED_DATE"), QStringLiteral("EXTRA")};
    const QStringList firstRecordOrder = multipleOrder;
    multipleOrder.append(firstRecordOrder); // Column population considers every record.
    checkOrder(multiple, multipleOrder, "multi-record columns or missing cells were dropped");
    SystemInfoResult emptyFields;
    emptyFields.kind = SystemInfoKind::License;
    emptyFields.columns = {QStringLiteral("EXTRA"), QStringLiteral("EXPIRED_DATE"), QStringLiteral("SERIES_NO")};
    emptyFields.rows = {{QStringLiteral("  "), QString(), QStringLiteral("")}};
    checkOrder(emptyFields, {QStringLiteral("SERIES_NO"), QStringLiteral("EXPIRED_DATE"), QStringLiteral("EXTRA")},
        "all-empty columns lost their stable preferred order");
    emptyFields.columns = {QStringLiteral("UNKNOWN_EMPTY"), QStringLiteral("UNKNOWN_VALUE")};
    emptyFields.rows = {{QString(), QStringLiteral("value")}};
    checkOrder(emptyFields, {QStringLiteral("UNKNOWN_VALUE"), QStringLiteral("UNKNOWN_EMPTY")},
        "unknown fields were dropped or failed populated-first order",
        {QStringLiteral("UNKNOWN_VALUE"), QStringLiteral("UNKNOWN_EMPTY")});
    emptyFields.rows.clear();
    checkOrder(emptyFields, {}, "empty result lost its empty state");
    SystemInfoResult translated;
    translated.kind = SystemInfoKind::License;
    translated.columns = {QStringLiteral("SERIES_NO"), QStringLiteral("EXPIRED_DATE"),
        QStringLiteral("AUTHORIZED_CUSTOMER"), QStringLiteral("CLUSTER_TYPE"), QStringLiteral(" lic_version "),
        QStringLiteral("SERVER_SERIES"), QStringLiteral("SERVER_TYPE"), QStringLiteral("SERVER_VER"),
        QStringLiteral("PRODUCT_TYPE"), QStringLiteral("PROJECT_NAME"), QStringLiteral("CPU_TYPE"),
        QStringLiteral("OS_TYPE"), QStringLiteral("DATE_GEN"), QStringLiteral("SERVER_SERIES_NAME"),
        QStringLiteral("CHECK_CODE"), QStringLiteral("AUTHORIZED_USER_NUMBER"), QStringLiteral("CONCURRENCY_USER_NUMBER"),
        QStringLiteral("MAX_CPU_NUM"), QStringLiteral("MAX_CORE_NUM"), QStringLiteral("NOACTIVE_DEADLINE"),
        QStringLiteral("HARDWARE_ID"), QStringLiteral("HARDWARE_TYPE"), QStringLiteral("TABLE_RECORD_NUMBER_LIMIT"),
        QStringLiteral("TOTAL_SPACE_LIMIT")};
    QStringList translatedOrder;
    QStringList translatedValues;
    for (const auto& column : translated.columns) {
        translatedOrder.append(column.trimmed().toUpper());
        translatedValues.append(QStringLiteral("0"));
    }
    translated.rows = {translatedValues};
    checkOrder(translated, translatedOrder, "24 license fields were lost or reordered",
        {QStringLiteral("许可证序列号"), QStringLiteral("授权到期日期"), QStringLiteral("授权单位"),
            QStringLiteral("集群授权标识"), QStringLiteral("许可证版本"), QStringLiteral("服务器版本类型"),
            QStringLiteral("服务器发布类型"), QStringLiteral("服务器版本号"), QStringLiteral("产品类型"),
            QStringLiteral("项目名称"), QStringLiteral("授权 CPU 类型"), QStringLiteral("授权操作系统"),
            QStringLiteral("授权文件生成日期"), QStringLiteral("版本名称"), QStringLiteral("校验码"),
            QStringLiteral("授权用户数"), QStringLiteral("并发连接数"), QStringLiteral("最大 CPU 数量"),
            QStringLiteral("最大 CPU 核心数"), QStringLiteral("未激活截止日期"), QStringLiteral("硬件绑定编码"),
            QStringLiteral("硬件绑定类型"), QStringLiteral("单表行数上限（万行）"), QStringLiteral("数据总空间上限（GB）")});
    application.setQuitOnLastWindowClosed(quitOnClose);
    return passed ? 0 : 1;
}

inline int runUiAppearanceCatalog(QApplication& application, const QString& directory) {
    QTemporaryDir settings;
    if (!settings.isValid() || directory.isEmpty() || !QDir().mkpath(directory)) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    QStandardPaths::setTestModeEnabled(true);
    application.setQuitOnLastWindowClosed(false);
    const ObjectKey table{QStringLiteral("visual-offline"), QStringLiteral("EXAMPLE"),
        DatabaseObjectType::Table, QStringLiteral("SAMPLE_TABLE")};
    ObjectKey package = table; package.type = DatabaseObjectType::Package; package.name = QStringLiteral("SAMPLE_API");
    ObjectKey view = table; view.type = DatabaseObjectType::View; view.name = QStringLiteral("SAMPLE_VIEW");
    QJsonArray records;
    bool passed = true;
    const auto settle = [&]() {
        QEventLoop loop;
        QTimer::singleShot(35, &loop, &QEventLoop::quit);
        loop.exec();
    };
    for (const auto mode : {ThemeMode::Light, ThemeMode::Dark}) {
        Theme::apply(application, mode);
        const QString theme = mode == ThemeMode::Light ? QStringLiteral("light") : QStringLiteral("dark");
        int serial = 0;
        const auto capture = [&](QWidget& widget, const QString& page, const QString& state) {
            if (page == QStringLiteral("sql") || page == QStringLiteral("main")) {
                if (auto* editor = widget.findChild<QPlainTextEdit*>(QStringLiteral("sqlEditor")); editor && editor->toPlainText().isEmpty())
                    editor->setPlainText(QStringLiteral("-- 离线字体预览：中文、English、0123456789\nSELECT object_name, object_type\n  FROM user_objects\n WHERE object_name LIKE 'SAMPLE_%';"));
            }
            settle();
            const QString file = QStringLiteral("%1-%2-%3.png").arg(theme, page).arg(++serial, 3, 10, QLatin1Char('0'));
            const bool saved = widget.grab().save(QDir(directory).filePath(file));
            const bool themeMatches = (application.palette().color(QPalette::Window).lightness() < 128)
                == (mode == ThemeMode::Dark);
            passed &= saved && themeMatches;
            QJsonArray buttons;
            for (auto* button : widget.findChildren<QAbstractButton*>()) {
                if (!button->isVisibleTo(&widget)) continue;
                buttons.append(QJsonObject{{"name", button->objectName()}, {"text", button->text()},
                    {"enabled", button->isEnabled()}, {"role", button->property("role").toString()}});
            }
            QJsonArray editorFonts;
            for (auto* editor : widget.findChildren<QPlainTextEdit*>()) {
                if (editor->isVisibleTo(&widget)) editorFonts.append(QJsonObject{
                    {"name", editor->objectName()}, {"family", QFontInfo(editor->font()).family()},
                    {"readOnly", editor->isReadOnly()}});
            }
            records.append(QJsonObject{{"theme", theme}, {"page", page}, {"state", state},
                {"file", file}, {"width", widget.width()}, {"height", widget.height()},
                {"dpr", widget.devicePixelRatioF()}, {"uiFont", QFontInfo(widget.font()).family()},
                {"codeFont", QFontInfo(Theme::editorFont()).family()}, {"editorFonts", editorFonts},
                {"saved", saved}, {"themeMatches", themeMatches}, {"buttons", buttons}});
        };
        const auto symbols = QDir(QStringLiteral(":/icons")).entryList({QStringLiteral("*.svg")}, QDir::Files);
        QStringList outlines;
        for (const auto& symbol : symbols)
            if (!symbol.endsWith(QStringLiteral("-filled.svg")) && symbol != QStringLiteral("app.svg")) outlines.append(symbol);
        for (int start = 0; start < outlines.size(); start += 12) {
            QWidget sheet;
            auto* rows = new QGridLayout(&sheet);
            for (int i = start; i < qMin(start + 12, outlines.size()); ++i) {
                QString name = outlines[i]; name.chop(4);
                rows->addWidget(new QLabel(name), i-start, 0);
                int column = 1;
                for (const auto form : {InterfaceIcons::Form::Outline, InterfaceIcons::Form::Filled}) {
                    for (const int size : {16, 20, 24, 32}) {
                        auto icon = InterfaceIcons::icon(name, form);
                        for (const qreal dpr : {1.0, 1.25, 1.5, 2.5}) {
                            const auto scaled = icon.pixmap(QSize(size, size), dpr);
                            const auto pixels = scaled.toImage();
                            bool visible = false;
                            for (int y = 0; y < pixels.height() && !visible; ++y)
                                for (int x = 0; x < pixels.width(); ++x)
                                    if (qAlpha(pixels.pixel(x,y)) > 0) { visible = true; break; }
                            const bool valid = visible && scaled.size() == QSize(qRound(size*dpr), qRound(size*dpr));
                            passed &= valid;
                            if (!valid) qWarning("ICON_RENDER_FAIL name=%s size=%d dpr=%.2f", qPrintable(name), size, dpr);
                        }
                        const auto image = icon.pixmap(QSize(size, size));
                        passed &= !image.isNull() && image.width() >= size;
                        auto* label = new QLabel; label->setPixmap(image); label->setAlignment(Qt::AlignCenter);
                        rows->addWidget(label, i-start, column++);
                    }
                }
                for (const auto state : {QIcon::Disabled, QIcon::Selected}) {
                    auto* label = new QLabel;
                    if (state == QIcon::Selected) {
                        label->setAutoFillBackground(true);
                        auto colors = label->palette();
                        colors.setColor(QPalette::Window, colors.color(QPalette::Highlight));
                        label->setPalette(colors);
                    }
                    label->setPixmap(InterfaceIcons::icon(name).pixmap(QSize(24,24), state));
                    rows->addWidget(label, i-start, column++);
                }
            }
            sheet.show();
            capture(sheet, "icon-catalog", QStringLiteral("%1：线框／实心 16、20、24、32；禁用／选中").arg(start/12+1));
        }
        if (qEnvironmentVariableIsSet("REID_UI_ICONS_ONLY")) continue;
        const auto audit = [&](const QString& name, QWidget* owned) {
            std::unique_ptr<QWidget> widget(owned);
            if (!qobject_cast<QDialog*>(widget.get())) widget->resize(1120, 740);
            widget->show(); settle();
            const auto captureModal = [&](const std::function<void()>& open, const QString& state) {
                QTimer timer; timer.setSingleShot(true);
                QObject::connect(&timer, &QTimer::timeout, widget.get(), [&]() {
                    if (auto* dialog = qobject_cast<QDialog*>(application.activeModalWidget())) {
                        capture(*dialog, name + QStringLiteral("-dialog"), state);
                        dialog->reject();
                    }
                });
                timer.start(80); open(); timer.stop();
            };
            if (name == QStringLiteral("connection-cluster")) {
                auto* type = widget->findChild<QComboBox*>(QStringLiteral("connectionType"));
                type->setCurrentIndex(type->findData(static_cast<int>(ConnectionType::DmService)));
                widget->findChild<QLineEdit*>(QStringLiteral("connectionName"))->setText(QStringLiteral("离线示例集群连接"));
                widget->findChild<QLineEdit*>(QStringLiteral("connectionUser"))->setText(QStringLiteral("EXAMPLE_USER"));
                widget->findChild<QPlainTextEdit*>(QStringLiteral("connectionEndpoints"))->setPlainText(
                    QStringLiteral("node-a.example:5236\nnode-b.example:5236"));
                settle();
            }
            capture(*widget, name, QStringLiteral("默认"));
            if (name == QStringLiteral("operation-log")) {
                auto* logTable = widget->findChild<QTableWidget*>(QStringLiteral("operationLogTable"));
                captureModal([&]() {
                    const QModelIndex index = logTable->model()->index(0, 11);
                    QStyleOptionViewItem option;
                    option.rect = logTable->visualRect(index);
                    const QPointF point(option.rect.right() - 14, option.rect.center().y());
                    QMouseEvent event(QEvent::MouseButtonRelease, point, point,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    logTable->itemDelegateForColumn(11)->editorEvent(&event, logTable->model(), option, index);
                }, QStringLiteral("SQL 全文弹窗"));
                widget->resize(420, 740); settle();
                capture(*widget, name, QStringLiteral("窄侧栏及完整诊断"));
                widget->findChild<QToolButton*>(QStringLiteral("operationLogFilterToggle"))->setChecked(true);
                settle();
                capture(*widget, name, QStringLiteral("展开筛选"));
            }
            const auto tabs = widget->findChildren<QTabWidget*>();
            for (auto* tab : tabs) {
                const int original = tab->currentIndex();
                for (int i = 0; i < tab->count(); ++i) {
                    if (!tab->isTabEnabled(i) || !tab->isTabVisible(i) || i == original) continue;
                    tab->setCurrentIndex(i); settle();
                    if (tab->isVisibleTo(widget.get())) capture(*widget, name, tab->tabText(i));
                }
                tab->setCurrentIndex(original);
            }
            if (name == QStringLiteral("help")) {
                for (auto* chapters : widget->findChildren<QListWidget*>()) {
                    for (int i = 0; i < chapters->count(); ++i) {
                        chapters->setCurrentRow(i);
                        capture(*widget, name, chapters->item(i)->text());
                    }
                }
            }
            if (name == QStringLiteral("dba-9")) {
                if (auto* table = widget->findChild<QTableWidget*>(QStringLiteral("watchedParametersTable")); table && table->rowCount() > 0)
                    captureModal([&]() { emit table->cellDoubleClicked(0, 0); }, QStringLiteral("参数说明"));
            }
            bool capturePasteDialog = name == QStringLiteral("connection-cluster");
            if (capturePasteDialog) {
                for (auto* button : widget->findChildren<QPushButton*>()) {
                    if (!button->isVisibleTo(widget.get()) || !button->isEnabled()) continue;
                    if (button->text() == QStringLiteral("EE 格式") || button->text() == QStringLiteral("批量粘贴…")
                            || button->text() == QStringLiteral("批量粘贴..."))
                        captureModal([&]() { button->click(); }, button->text());
                }
            }
            for (auto* button : widget->findChildren<QToolButton*>()) {
                if (button->isCheckable() && (button->objectName() == QStringLiteral("connectionAdvancedToggle")
                        || button->text() == QStringLiteral("高级选项"))) {
                    button->click(); capture(*widget, name, QStringLiteral("高级设置")); button->click();
                }
                if (button->menu() && button->isVisibleTo(widget.get()) && button->isEnabled()) {
                    button->menu()->popup(button->mapToGlobal(QPoint(0, button->height())));
                    capture(*button->menu(), name, button->text() + QStringLiteral("菜单"));
                    button->menu()->hide();
                }
            }
            for (auto* combo : widget->findChildren<QComboBox*>()) {
                if (!combo->isVisibleTo(widget.get()) || !combo->isEnabled() || combo->count() == 0) continue;
                combo->showPopup(); settle();
                capture(*combo->view()->window(), name, combo->objectName() + QStringLiteral("下拉选项"));
                combo->hidePopup();
            }
            if (qobject_cast<QDialog*>(widget.get()) && name != QStringLiteral("find-replace")) {
                widget->resize(qMax(620, widget->minimumWidth()), 480);
                settle();
                capture(*widget, name, QStringLiteral("窄窗口"));
            }
            widget->hide();
        };
        audit("connection", new ConnectionDialog(nullptr, settings.filePath("connections.json")));
        audit("connection-cluster", new ConnectionDialog(nullptr, settings.filePath("cluster.json")));
        audit("connection-failure", new ConnectionFailureDialog(QStringLiteral("示例连接"),
            QStringLiteral("尚未连接数据库。此窗口用于离线检查提示样式。"), QStringLiteral("检查连接参数后重试。")));
        audit("sql", new SqlDocumentWidget(1));
        audit("operation-log", UiAppearanceCatalogAccess::operationLog());
        audit("command", new CommandDocumentWidget(1));
        audit("test", new TestDocumentWidget(package));
        audit("program", new ProgramDocumentWidget(package));
        audit("view", new ViewDocumentWidget(view));
        audit("object-details", new ObjectDetailsWidget(table));
        audit("object-list", new ObjectListWidget(table.connectionId, table.schema, DatabaseObjectType::Package));
        audit("object-ddl", new ObjectDdlWidget(table));
        audit("invalid", new InvalidObjectsWidget(table.connectionId));
        audit("missing-body", new MissingPackageBodiesWidget(table.connectionId));
        audit("user", new UserEditorDocumentWidget(QStringLiteral("EXAMPLE_USER")));
        audit("role", new RoleEditorDocumentWidget(QStringLiteral("EXAMPLE_ROLE")));
        for (const auto kind : {DbaDocumentKind::Sessions, DbaDocumentKind::Locks, DbaDocumentKind::Performance,
                DbaDocumentKind::Tablespaces, DbaDocumentKind::Users, DbaDocumentKind::Roles,
                DbaDocumentKind::Jobs, DbaDocumentKind::JobSchedules, DbaDocumentKind::JobHistory, DbaDocumentKind::Parameters})
            audit(QStringLiteral("dba-%1").arg(static_cast<int>(kind)), new DbaDocumentWidget(kind, 1));
        audit("export", new ResultExportDialog);
        audit("content", new ResultContentDialog(QStringLiteral("内容预览"), QByteArray("{\"example\": true}"), false, nullptr));
        audit("test-data", new TestDataDialog(table, {}));
        audit("keywords", new KeywordReferenceDialog);
        audit("help", new HelpDocumentDialog);
        audit("version", new VersionNotesDialog);
        audit("update", new UpdateDialog);
        audit("backups", new PackageBodyBackupsDialog(nullptr, settings.filePath("backups")));
        audit("history-cleanup", new HistoryCleanupDialog({}));
        audit("migration-progress", new LegacyMigrationProgressDialog(QStringLiteral("离线迁移进度预览")));
        audit("snippets", new SnippetPanel);
        auto* snippetEditor = new SnippetPanel;
        snippetEditor->findChild<QToolButton*>(QStringLiteral("snippetEditToggle"))->setChecked(true);
        audit("snippet-editor", snippetEditor);
        auto* prompt = new QInputDialog;
        prompt->setWindowTitle(QStringLiteral("名称输入"));
        prompt->setLabelText(QStringLiteral("请输入名称："));
        prompt->setTextValue(QStringLiteral("中文名称 English 0123456789"));
        audit("input-prompt", prompt);
        for (const auto icon : {QMessageBox::Information, QMessageBox::Warning, QMessageBox::Critical, QMessageBox::Question}) {
            auto* message = new QMessageBox(icon, QStringLiteral("离线提示预览"),
                QStringLiteral("这是一条用于检查换行、字体与按钮对齐的提示。\n此窗口不会执行保存、删除或数据库操作。"), QMessageBox::Ok | QMessageBox::Cancel);
            message->setDetailedText(QStringLiteral("示例详情：中文、English、0123456789。"));
            audit(QStringLiteral("message-%1").arg(static_cast<int>(icon)), message);
        }
        auto* filePicker = new QFileDialog(nullptr, QStringLiteral("文件选择预览"), settings.path());
        filePicker->setOption(QFileDialog::DontUseNativeDialog);
        audit("file-picker", filePicker);
        auto* colorPicker = new QColorDialog(QColor(QStringLiteral("#2B65D9")));
        colorPicker->setOption(QColorDialog::DontUseNativeDialog);
        audit("color-picker", colorPicker);
#ifdef Q_OS_WIN
        audit("native-save-wait", new SaveFileDialog::Detail::WaitDialog(nullptr));
#endif
        QWidget diagnosticParent;
        DiagnosticTextInteraction::showText(&diagnosticParent, QStringLiteral("离线详细信息：完整文本可选择复制。\nSQL 示例：SELECT 1 FROM DUAL;"));
        if (auto* details = diagnosticParent.findChild<QDialog*>(QStringLiteral("diagnosticTextDialog")))
            audit("diagnostic-details", details);
        ConnectionProfile profile; profile.id = table.connectionId; profile.name = QStringLiteral("离线示例");
        audit("delete-confirmation", new ObjectDropConfirmationDialog(profile, {package}));
        SqlEditor editor; editor.setPlainText(QStringLiteral("SELECT 1 FROM DUAL;"));
        auto* find = new FindReplaceDialog; find->showFor(&editor, true); audit("find-replace", find);
        MainWindow main(nullptr, false, true); main.setThemeMode(mode);
        main.resize(1280, 800); main.show(); settle();
        capture(main, "main", QStringLiteral("默认"));
        for (const auto kind : {SystemInfoKind::Version, SystemInfoKind::License, SystemInfoKind::ReservedWords}) {
            SystemInfoResult result; result.kind = kind;
            if (kind == SystemInfoKind::Version) {
                result.serverAddress = QStringLiteral("192.0.2.10");
                result.serverPort = 5236;
                result.instanceName = QStringLiteral("OFFLINE_INSTANCE");
            }
            result.columns = kind == SystemInfoKind::Version ? QStringList{QStringLiteral("INNER_VER"), QStringLiteral("BANNER")}
                : kind == SystemInfoKind::License ? QStringList{QStringLiteral("LIC_VERSION"),
                    QStringLiteral("AUTHORIZED_CUSTOMER"), QStringLiteral("CLUSTER_TYPE"),
                    QStringLiteral("EXPIRED_DATE"), QStringLiteral("SERIES_NO"), QStringLiteral("HARDWARE_ID")}
                : QStringList{QStringLiteral("KEYWORD"), QStringLiteral("RESERVED")};
            result.rows = kind == SystemInfoKind::ReservedWords ? QVector<QStringList>{{QStringLiteral("SELECT"), QStringLiteral("Y")}}
                : kind == SystemInfoKind::License ? QVector<QStringList>{{QStringLiteral("3.00"),
                    QStringLiteral("离线示例单位 <示例>（非真实授权）"), QStringLiteral("1010"),
                    QString(), QStringLiteral("OFFLINE-SAMPLE"), QStringLiteral("SAMPLE-HARDWARE")}}
                : QVector<QStringList>{{QStringLiteral("离线示例"), QStringLiteral("仅用于检查显示，不代表实际数据库信息")}};
            QTimer::singleShot(80, &main, [&]() {
                auto* dialog = qobject_cast<QDialog*>(application.activeModalWidget());
                if (!dialog) { passed = false; return; }
                capture(*dialog, "database-information", dialog->windowTitle());
                dialog->reject();
            });
            UiAppearanceCatalogAccess::systemInfo(main, result);
        }
        std::function<void(QMenu*, const QString&)> captureMenu = [&](QMenu* menu, const QString& path) {
            menu->popup(main.menuBar()->mapToGlobal(QPoint(0, main.menuBar()->height())));
            capture(*menu, "main-menu", path);
            for (auto* action : menu->actions())
                if (action->menu()) captureMenu(action->menu(), path + QStringLiteral(" / ") + action->text());
            menu->hide();
        };
        for (auto* action : main.menuBar()->actions())
            if (action->menu()) captureMenu(action->menu(), action->text());
        for (auto* action : main.findChildren<QAction*>()) {
            if (action->text() != QStringLiteral("设置…") && action->text() != QStringLiteral("工具栏…")
                    && action->text() != QStringLiteral("配置主工具栏…")) continue;
            QTimer::singleShot(80, &main, [&]() {
                auto* dialog = qobject_cast<QDialog*>(application.activeModalWidget());
                if (!dialog) { passed = false; return; }
                capture(*dialog, "settings", dialog->windowTitle());
                for (auto* tabs : dialog->findChildren<QTabWidget*>()) {
                    for (int i = 0; i < tabs->count(); ++i) {
                        tabs->setCurrentIndex(i); settle();
                        if (tabs->isVisibleTo(dialog)) capture(*dialog, "settings", tabs->tabText(i));
                    }
                }
                if (auto* pages = dialog->findChild<QStackedWidget*>(QStringLiteral("settingsCategories"))) {
                    auto* navigation = dialog->findChild<QListWidget*>(QStringLiteral("settingsCategoryList"));
                    for (int index = 0; index < pages->count(); ++index) {
                        pages->setCurrentIndex(index); settle();
                        capture(*dialog, "settings", navigation && navigation->item(index)
                            ? navigation->item(index)->text() : QString::number(index));
                    }
                }
                dialog->reject();
            });
            action->trigger();
        }
        main.hide();
    }
    QFile output(QDir(directory).filePath(QStringLiteral("catalog.json")));
    if (!output.open(QIODevice::WriteOnly)) return 1;
    output.write(QJsonDocument(records).toJson());
    QTextStream(stdout) << "UI_APPEARANCE_CATALOG=" << (passed ? "PASS" : "FAIL")
        << " CAPTURES=" << records.size() << '\n';
    return passed ? 0 : 1;
}
