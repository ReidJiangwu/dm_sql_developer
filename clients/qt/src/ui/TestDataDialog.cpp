#include "Theme.h"
#include "SaveFileDialog.h"
#include "TestDataDialog.h"

#include "../core/TestDataGenerator.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSplitter>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {

int number(const QString& value) {
    bool valid = false;
    const int result = value.toInt(&valid);
    return valid ? result : 0;
}

}  // namespace

TestDataDialog::TestDataDialog(ObjectKey key, QVector<MetadataColumn> columns, QWidget* parent)
    : QDialog(parent)
    , key_(std::move(key))
    , columns_(std::move(columns)) {
    setObjectName(QStringLiteral("testDataDialog"));
    setWindowTitle(QStringLiteral("生成测试数据 · %1.%2").arg(key_.schema, key_.name));
    resize(920, 680);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);
    auto* lead = new QLabel(QStringLiteral(
        "只生成 INSERT 文本，不会连接数据库或执行写入。"), this);
    lead->setProperty("callout", QStringLiteral("info"));
    layout->addWidget(lead);

    auto* rowBar = new QHBoxLayout();
    auto* rowsLabel = new QLabel(QStringLiteral("生成行数："), this);
    rows_ = new QSpinBox(this);
    rowsLabel->setBuddy(rows_);
    rows_->setRange(1, TestDataGenerator::kMaximumRows);
    rows_->setValue(100);
    rows_->setAccessibleName(QStringLiteral("生成行数"));
    rowBar->addWidget(rowsLabel);
    rowBar->addWidget(rows_);
    rowBar->addStretch(1);
    generate_ = new QPushButton(QStringLiteral("生成预览"), this);
    generate_->setProperty("role", QStringLiteral("primary"));
    generate_->setDefault(true);
    rowBar->addWidget(generate_);
    layout->addLayout(rowBar);

    columnsTable_ = new QTableWidget(columns_.size(), 5, this);
    columnsTable_->setObjectName(QStringLiteral("testDataColumnsTable"));
    columnsTable_->setAccessibleName(QStringLiteral("测试数据生成列"));
    columnsTable_->setAccessibleDescription(QStringLiteral(
        "勾选需要生成数据的列；不支持的列默认不勾选。"));
    columnsTable_->setHorizontalHeaderLabels({
        QStringLiteral("生成"), QStringLiteral("列名"), QStringLiteral("类型"),
        QStringLiteral("可空／默认"), QStringLiteral("支持状态")});
    columnsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    columnsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    for (int row = 0; row < columns_.size(); ++row) {
        const MetadataColumn& column = columns_.at(row);
        const bool supported =
            TestDataGenerator::category(column.dataType) != GeneratedValueCategory::Unsupported;
        auto* selected = new QTableWidgetItem();
        selected->setFlags(selected->flags() | Qt::ItemIsUserCheckable);
        selected->setCheckState(supported ? Qt::Checked : Qt::Unchecked);
        columnsTable_->setItem(row, 0, selected);
        columnsTable_->setItem(row, 1, new QTableWidgetItem(column.name));
        columnsTable_->setItem(row, 2, new QTableWidgetItem(column.dataType));
        columnsTable_->setItem(row, 3, new QTableWidgetItem(
            QStringLiteral("%1／%2").arg(column.nullable ? QStringLiteral("可空")
                : QStringLiteral("非空"), column.defaultValue.trimmed().isEmpty()
                ? QStringLiteral("无默认") : QStringLiteral("有默认"))));
        columnsTable_->setItem(row, 4, new QTableWidgetItem(
            supported ? QStringLiteral("支持") : QStringLiteral("不支持，默认排除")));
    }
    preview_ = new QPlainTextEdit(this);
    preview_->setFont(Theme::editorFont());
    preview_->setObjectName(QStringLiteral("testDataSqlPreview"));
    preview_->setAccessibleName(QStringLiteral("生成的 INSERT SQL 预览"));
    preview_->setAccessibleDescription(QStringLiteral(
        "只读 SQL 文本，可复制、保存或打开到 SQL Window。"));
    preview_->setReadOnly(true);
    preview_->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview_->setPlaceholderText(QStringLiteral("点击“生成预览”后显示 INSERT SQL。"));

    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setObjectName(QStringLiteral("testDataSplitter"));
    auto createSection = [](const QString& title, QWidget* target) {
        auto* panel = new QWidget;
        auto* panelLayout = new QVBoxLayout(panel);
        panelLayout->setContentsMargins(0, 0, 0, 0);
        panelLayout->setSpacing(6);
        auto* label = new QLabel(title, panel);
        label->setProperty("sectionHeading", true);
        label->setBuddy(target);
        panelLayout->addWidget(label);
        panelLayout->addWidget(target, 1);
        return panel;
    };
    splitter->addWidget(createSection(QStringLiteral("生成列"), columnsTable_));
    splitter->addWidget(createSection(QStringLiteral("SQL 预览"), preview_));
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    layout->addWidget(splitter, 1);
    state_ = new QLabel(QStringLiteral("等待生成。"), this);
    state_->setObjectName(QStringLiteral("testDataState"));
    state_->setProperty("callout", QStringLiteral("info"));
    state_->setAccessibleName(QStringLiteral("测试数据生成状态"));
    state_->setAccessibleDescription(state_->text());
    state_->setWordWrap(true);
    layout->addWidget(state_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    copy_ = buttons->addButton(QStringLiteral("复制"), QDialogButtonBox::ActionRole);
    save_ = buttons->addButton(QStringLiteral("保存 UTF-8…"), QDialogButtonBox::ActionRole);
    openSql_ = buttons->addButton(QStringLiteral("SQL 窗口"),
        QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(generate_, &QPushButton::clicked, this, &TestDataDialog::generate);
    connect(copy_, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(preview_->toPlainText());
        state_->setText(QStringLiteral("已复制生成脚本。"));
        state_->setAccessibleDescription(state_->text());
    });
    connect(save_, &QPushButton::clicked, this, &TestDataDialog::save);
    connect(openSql_, &QPushButton::clicked, this, [this]() {
        emit openSqlRequested(preview_->toPlainText());
        accept();
    });
    layout->addWidget(buttons);
    setTabOrder(rows_, generate_);
    setTabOrder(generate_, columnsTable_);
    setTabOrder(columnsTable_, preview_);
    setTabOrder(preview_, copy_);
    updateActions();
}

void TestDataDialog::generate() {
    QVector<TestDataColumn> generationColumns;
    generationColumns.reserve(columns_.size());
    for (int row = 0; row < columns_.size(); ++row) {
        const MetadataColumn& source = columns_.at(row);
        generationColumns.append({source.name, source.dataType, number(source.length),
            number(source.precision), number(source.scale), source.nullable,
            !source.defaultValue.trimmed().isEmpty(),
            columnsTable_->item(row, 0)->checkState() == Qt::Checked});
    }
    const QString schema = key_.schema;
    const QString table = key_.name;
    const int rowCount = rows_->value();
    int selectedCount = 0;
    for (const TestDataColumn& column : std::as_const(generationColumns)) {
        if (column.selected) ++selectedCount;
    }
    busy_ = true;
    state_->setText(QStringLiteral("正在后台生成…"));
    state_->setAccessibleDescription(state_->text());
    updateActions();
    auto* watcher = new QFutureWatcher<TestDataGenerationResult>(this);
    connect(watcher, &QFutureWatcher<TestDataGenerationResult>::finished, this,
        [this, watcher, rowCount, selectedCount]() {
            const TestDataGenerationResult result = watcher->result();
            watcher->deleteLater();
            busy_ = false;
            if (!result.errors.isEmpty()) {
                preview_->clear();
                state_->setText(result.errors.join(QStringLiteral("；")));
                state_->setAccessibleDescription(state_->text());
                QMessageBox::warning(this, QStringLiteral("无法生成测试数据"),
                    result.errors.join(QLatin1Char('\n')));
            } else {
                preview_->setPlainText(result.sql);
                state_->setText(QStringLiteral(
                    "生成完成：%1 行，%2 列，%3 字节；上限 16 MiB。%4")
                    .arg(rowCount).arg(selectedCount).arg(result.sql.toUtf8().size())
                    .arg(result.warnings.join(QStringLiteral("；"))));
                state_->setAccessibleDescription(state_->text());
            }
            updateActions();
        });
    watcher->setFuture(QtConcurrent::run([schema, table,
            generationColumns = std::move(generationColumns), rowCount]() {
        return TestDataGenerator::generate(schema, table, generationColumns, rowCount);
    }));
}

void TestDataDialog::save() {
    const QString path = SaveFileDialog::getSaveFileName(this,
        QStringLiteral("保存测试数据脚本"), {}, QStringLiteral("SQL 脚本 (*.sql)"));
    if (path.isEmpty()) return;
    const QByteArray bytes = preview_->toPlainText().toUtf8();
    busy_ = true;
    state_->setText(QStringLiteral("正在后台保存…"));
    state_->setAccessibleDescription(state_->text());
    updateActions();
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, path]() {
        const QString error = watcher->result();
        watcher->deleteLater();
        busy_ = false;
        state_->setText(error.isEmpty()
            ? QStringLiteral("已保存：%1").arg(path)
            : QStringLiteral("保存失败：%1").arg(error));
        state_->setAccessibleDescription(state_->text());
        if (!error.isEmpty()) QMessageBox::warning(this, QStringLiteral("保存失败"), error);
        updateActions();
    });
    watcher->setFuture(QtConcurrent::run([path, bytes]() {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return file.errorString();
        if (file.write(bytes) != bytes.size() || !file.commit()) return file.errorString();
        return QString();
    }));
}

void TestDataDialog::updateActions() {
    const bool hasSql = !preview_->toPlainText().isEmpty();
    generate_->setEnabled(!busy_);
    columnsTable_->setEnabled(!busy_);
    rows_->setEnabled(!busy_);
    copy_->setEnabled(!busy_ && hasSql);
    save_->setEnabled(!busy_ && hasSql);
    openSql_->setEnabled(!busy_ && hasSql);
}
