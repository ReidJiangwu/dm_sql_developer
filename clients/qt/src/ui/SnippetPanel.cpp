#include "SearchField.h"
#include "Theme.h"
#include "SaveFileDialog.h"
#include "../core/AppPaths.h"
#include "SnippetPanel.h"
#include "../core/BuiltInSnippets.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSplitter>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>
#include <QUuid>
#include <algorithm>

SnippetPanel::SnippetPanel(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setObjectName(QStringLiteral("snippetEditorSplitter"));
    splitter->setAccessibleName(QStringLiteral("代码片段列表与编辑区大小调整"));
    splitter->setChildrenCollapsible(false);

    auto* listPane = new QWidget(splitter);
    auto* listLayout = new QVBoxLayout(listPane);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(6);
    search_ = new QLineEdit(listPane);
    search_->setObjectName(QStringLiteral("snippetSearch"));
    SearchField::setPrompt(search_, QStringLiteral("名称／触发词／SQL"), QStringLiteral("搜索名称、触发词或 SQL"));
    search_->setAccessibleName(QStringLiteral("搜索代码片段"));
    search_->setClearButtonEnabled(true);
    listLayout->addWidget(search_);
    list_ = new QListWidget(listPane);
    list_->setObjectName(QStringLiteral("snippetList"));
    list_->setAccessibleName(QStringLiteral("代码片段列表"));
    list_->setMinimumHeight(52);
    list_->setUniformItemSizes(true);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setTextElideMode(Qt::ElideRight);
    listLayout->addWidget(list_, 1);

    auto* editorPane = new QWidget(splitter);
    editorPane->setObjectName(QStringLiteral("snippetEditorPane"));
    name_ = new QLineEdit(editorPane);
    name_->setObjectName(QStringLiteral("snippetName"));
    name_->setClearButtonEnabled(true);
    trigger_ = new QLineEdit(editorPane);
    trigger_->setObjectName(QStringLiteral("snippetTrigger"));
    trigger_->setClearButtonEnabled(true);
    sql_ = new QPlainTextEdit(editorPane);
    sql_->setObjectName(QStringLiteral("snippetSql"));
    sql_->setFont(Theme::editorFont());
    sql_->setPlaceholderText(QStringLiteral("输入可复用 SQL…"));
    sql_->setMinimumHeight(160);
    auto* form = new QFormLayout(editorPane);
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(8);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->addRow(QStringLiteral("名称："), name_);
    form->addRow(QStringLiteral("触发词："), trigger_);
    form->addRow(QStringLiteral("SQL："), sql_);

    splitter->addWidget(listPane);
    splitter->addWidget(editorPane);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({280, 420});
    layout->addWidget(splitter, 1);

    auto* management = new QWidget(this);
    management->setObjectName(QStringLiteral("snippetManagementActions"));
    auto* actions = new QGridLayout(management);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setHorizontalSpacing(6);
    actions->setVerticalSpacing(6);
    auto* create = new QPushButton(QStringLiteral("新建"), this);
    create->setObjectName(QStringLiteral("createSnippet"));
    auto* saveButton = new QPushButton(QStringLiteral("保存"), this);
    saveButton->setObjectName(QStringLiteral("saveSnippet"));
    auto* remove = new QPushButton(QStringLiteral("删除"), this);
    remove->setObjectName(QStringLiteral("deleteSnippet"));
    remove->setEnabled(false);
    auto* insert = new QPushButton(QStringLiteral("插入"), this);
    insert->setObjectName(QStringLiteral("insertSnippet"));
    auto* copy = new QPushButton(QStringLiteral("复制"), this);
    copy->setObjectName(QStringLiteral("copySnippet"));
    auto* importButton = new QPushButton(QStringLiteral("导入"), this);
    importButton->setObjectName(QStringLiteral("importSnippets"));
    auto* exportButton = new QPushButton(QStringLiteral("导出"), this);
    exportButton->setObjectName(QStringLiteral("exportSnippets"));
    insert->setProperty("role", QStringLiteral("primary"));
    remove->setProperty("role", QStringLiteral("danger"));
    for (auto* button : {create, copy, importButton, exportButton}) {
        button->setProperty("quietAction", true);
    }
    actions->addWidget(create, 0, 0);
    actions->addWidget(saveButton, 0, 1);
    actions->addWidget(remove, 0, 2);
    actions->addWidget(copy, 1, 0);
    actions->addWidget(importButton, 1, 1);
    actions->addWidget(exportButton, 1, 2);
    for (int column = 0; column < 3; ++column) actions->setColumnStretch(column, 1);
    layout->addWidget(management);
    auto* quickActions = new QHBoxLayout();
    quickActions->addWidget(insert, 1);
    auto* edit = new QToolButton(this);
    edit->setObjectName(QStringLiteral("snippetEditToggle"));
    edit->setText(QStringLiteral("编辑片段"));
    edit->setProperty("textButton", true);
    edit->setCheckable(true);
    quickActions->addWidget(edit);
    layout->addLayout(quickActions);
    editorPane->hide();
    management->hide();
    connect(edit, &QToolButton::toggled, this, [editorPane, management](bool expanded) {
        editorPane->setVisible(expanded);
        management->setVisible(expanded);
    });
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("snippetStatus"));
    status_->setWordWrap(true);
    status_->hide();
    layout->addWidget(status_);

    connect(list_, &QListWidget::currentRowChanged, this, &SnippetPanel::selectSnippet);
    connect(list_, &QListWidget::currentRowChanged, this, [this, remove](int row) {
        remove->setEnabled(row >= 0 && row < snippets_.size() && !snippets_[row].builtIn);
    });
    connect(create, &QPushButton::clicked, this, [this]() {
        list_->setCurrentRow(-1);
        name_->clear();
        trigger_->clear();
        sql_->clear();
        name_->setFocus();
    });
    connect(saveButton, &QPushButton::clicked, this, &SnippetPanel::saveCurrent);
    connect(remove, &QPushButton::clicked, this, &SnippetPanel::deleteCurrent);
    connect(copy, &QPushButton::clicked, this, &SnippetPanel::duplicateCurrent);
    connect(importButton, &QPushButton::clicked, this, &SnippetPanel::importSnippets);
    connect(exportButton, &QPushButton::clicked, this, &SnippetPanel::exportSnippets);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        const QString query = text.trimmed();
        for (int index = 0; index < snippets_.size(); ++index) {
            const Snippet& item = snippets_[index];
            list_->item(index)->setHidden(!query.isEmpty()
                && !item.name.contains(query, Qt::CaseInsensitive)
                && !item.trigger.contains(query, Qt::CaseInsensitive)
                && !item.sql.contains(query, Qt::CaseInsensitive));
        }
    });
    connect(insert, &QPushButton::clicked, this, [this]() {
        if (!sql_->toPlainText().trimmed().isEmpty()) emit insertRequested(sql_->toPlainText());
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this]() {
        if (!sql_->toPlainText().trimmed().isEmpty()) emit insertRequested(sql_->toPlainText());
    });
    load();
    if (list_->count() > 0) list_->setCurrentRow(0);
}

QString SnippetPanel::storagePath() {
    return QDir(AppPaths::dataRoot())
        .filePath(QStringLiteral("snippets.json"));
}

void SnippetPanel::load() {
    snippets_.clear();
    for (const auto& item : builtInSnippets()) {
        snippets_.append({item.id, item.name, item.trigger, item.sql, true});
    }
    for (const Snippet& snippet : snippets_) {
        list_->addItem(QStringLiteral("内置｜%1").arg(snippet.name));
        list_->item(list_->count() - 1)->setToolTip(list_->item(list_->count() - 1)->text());
    }
    QFile file(storagePath());
    if (!file.exists()) return;
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("无法读取代码片段：%1").arg(file.errorString()));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || document.object().value(QStringLiteral("version")).toInt() != 1) {
        setStatus(QStringLiteral("代码片段文件损坏，已回退到内置片段；可导入备份恢复。"));
        return;
    }
    for (const QJsonValue& value : document.object().value(QStringLiteral("snippets")).toArray()) {
        const QJsonObject item = value.toObject();
        const QString itemSql = item.value(QStringLiteral("sql")).toString();
        if (containsSensitiveSql(itemSql)) continue;
        snippets_.push_back({item.value(QStringLiteral("id")).toString(),
            item.value(QStringLiteral("name")).toString(),
            item.value(QStringLiteral("trigger")).toString(
                item.value(QStringLiteral("tags")).toString()),
            itemSql, false});
        list_->addItem(snippets_.last().name);
        list_->item(list_->count() - 1)->setToolTip(list_->item(list_->count() - 1)->text());
    }
}

void SnippetPanel::save() {
    QLockFile lock(storagePath() + QStringLiteral(".lock"));
    lock.setStaleLockTime(5'000);
    if (!lock.tryLock(5'000)) {
        setStatus(QStringLiteral("代码片段正被另一个实例修改，请稍后重试。"));
        return;
    }
    QJsonArray items;
    for (const Snippet& snippet : snippets_) {
        if (snippet.builtIn) continue;
        items.append(QJsonObject{{QStringLiteral("id"), snippet.id},
            {QStringLiteral("name"), snippet.name},
            {QStringLiteral("trigger"), snippet.trigger}, {QStringLiteral("sql"), snippet.sql}});
    }
    QDir directory = QFileInfo(storagePath()).dir();
    directory.mkpath(QStringLiteral("."));
    QSaveFile file(storagePath());
    const QByteArray data = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
        {QStringLiteral("snippets"), items}}).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        setStatus(QStringLiteral("无法保存代码片段：%1").arg(file.errorString()));
    }
}

void SnippetPanel::selectSnippet(int row) {
    if (row < 0 || row >= snippets_.size()) return;
    name_->setText(snippets_[row].name);
    trigger_->setText(snippets_[row].trigger);
    sql_->setPlainText(snippets_[row].sql);
}

void SnippetPanel::saveCurrent() {
    const QString name = name_->text().trimmed();
    if (name.isEmpty() || sql_->toPlainText().trimmed().isEmpty()) {
        setStatus(QStringLiteral("名称和 SQL 不能为空。"));
        return;
    }
    if (containsSensitiveSql(sql_->toPlainText())) {
        setStatus(QStringLiteral("代码片段不能保存密码、token 或凭据字面量。"));
        return;
    }
    for (int index = 0; index < snippets_.size(); ++index) {
        if (index != list_->currentRow()
            && snippets_[index].name.compare(name, Qt::CaseInsensitive) == 0) {
            setStatus(QStringLiteral("片段名称重复：%1").arg(name));
            return;
        }
    }
    const int row = list_->currentRow();
    const QString id = row >= 0 && row < snippets_.size() && !snippets_[row].builtIn
        ? snippets_[row].id : QUuid::createUuid().toString(QUuid::WithoutBraces);
    const Snippet value{id, name, trigger_->text().trimmed(), sql_->toPlainText(), false};
    if (row < 0 || row >= snippets_.size() || snippets_[row].builtIn) {
        snippets_.push_back(value);
        list_->addItem(value.name);
        list_->item(list_->count() - 1)->setToolTip(list_->item(list_->count() - 1)->text());
        list_->setCurrentRow(snippets_.size() - 1);
    } else {
        snippets_[row] = value;
        list_->item(row)->setText(value.name);
        list_->item(row)->setToolTip(value.name);
    }
    save();
    setStatus(QStringLiteral("代码片段已保存。"));
}

void SnippetPanel::duplicateCurrent() {
    const int row = list_->currentRow();
    if (row < 0 || row >= snippets_.size()) return;
    const Snippet& source = snippets_[row];
    QString name = source.name + QStringLiteral(" 副本");
    int suffix = 2;
    const auto exists = [this](const QString& candidate) {
        return std::any_of(snippets_.cbegin(), snippets_.cend(), [&candidate](const Snippet& item) {
            return item.name.compare(candidate, Qt::CaseInsensitive) == 0;
        });
    };
    const QString base = name;
    while (exists(name)) name = QStringLiteral("%1 %2").arg(base).arg(suffix++);
    snippets_.push_back({QUuid::createUuid().toString(QUuid::WithoutBraces),
        name, source.trigger, source.sql, false});
    list_->addItem(name);
    list_->item(list_->count() - 1)->setToolTip(list_->item(list_->count() - 1)->text());
    list_->setCurrentRow(snippets_.size() - 1);
    save();
    setStatus(QStringLiteral("代码片段已复制。"));
}

void SnippetPanel::exportSnippets() {
    QString path = SaveFileDialog::getSaveFileName(this, QStringLiteral("导出代码片段"),
        QStringLiteral("小智达梦管理工具-代码片段.json"), QStringLiteral("JSON 文件 (*.json)"));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".json");
    QJsonArray items;
    for (const Snippet& item : snippets_) {
        if (item.builtIn || containsSensitiveSql(item.sql)) continue;
        items.append(QJsonObject{{QStringLiteral("id"), item.id}, {QStringLiteral("name"), item.name},
            {QStringLiteral("trigger"), item.trigger}, {QStringLiteral("sql"), item.sql}});
    }
    const QByteArray data = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
        {QStringLiteral("credentialsIncluded"), false}, {QStringLiteral("snippets"), items}})
        .toJson(QJsonDocument::Indented);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        setStatus(QStringLiteral("导出失败：%1").arg(file.errorString()));
        return;
    }
    setStatus(QStringLiteral("已导出 %1 个用户片段。").arg(items.size()));
}

void SnippetPanel::importSnippets() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入代码片段"),
        {}, QStringLiteral("JSON 文件 (*.json)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("导入失败：%1").arg(file.errorString()));
        return;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()
        || document.object().value(QStringLiteral("version")).toInt() != 1) {
        setStatus(QStringLiteral("导入失败：片段文件格式无效。"));
        return;
    }
    QVector<Snippet> imported;
    for (const QJsonValue& value : document.object().value(QStringLiteral("snippets")).toArray()) {
        const QJsonObject object = value.toObject();
        const QString name = object.value(QStringLiteral("name")).toString().trimmed();
        const QString sql = object.value(QStringLiteral("sql")).toString();
        if (name.isEmpty() || sql.trimmed().isEmpty() || containsSensitiveSql(sql)) {
            setStatus(QStringLiteral("导入失败：存在空片段或包含密码、token、凭据的片段。"));
            return;
        }
        QString importedName = name;
        int suffix = 2;
        const QString base = importedName;
        while (std::any_of(snippets_.cbegin(), snippets_.cend(), [&importedName](const Snippet& item) {
            return item.name.compare(importedName, Qt::CaseInsensitive) == 0;
        }) || std::any_of(imported.cbegin(), imported.cend(), [&importedName](const Snippet& item) {
            return item.name.compare(importedName, Qt::CaseInsensitive) == 0;
        })) importedName = QStringLiteral("%1 %2").arg(base).arg(suffix++);
        imported.push_back({QUuid::createUuid().toString(QUuid::WithoutBraces), importedName,
            object.value(QStringLiteral("trigger")).toString(), sql, false});
    }
    for (const Snippet& item : imported) {
        snippets_.push_back(item);
        list_->addItem(item.name);
        list_->item(list_->count() - 1)->setToolTip(list_->item(list_->count() - 1)->text());
    }
    save();
    setStatus(QStringLiteral("已导入 %1 个用户片段。").arg(imported.size()));
}

bool SnippetPanel::containsSensitiveSql(const QString& sql) {
    return QRegularExpression(QStringLiteral(
        "(?i)((password|passwd|pwd|token|secret|credential|api[_-]?key)\\s*=\\s*'[^']*'|"
        "identified\\s+by\\s+('[^']*'|\"[^\"]*\"|[^\\s;]+))"))
        .match(sql).hasMatch();
}

void SnippetPanel::deleteCurrent() {
    const int row = list_->currentRow();
    if (row < 0 || row >= snippets_.size()) return;
    if (snippets_[row].builtIn) {
        setStatus(QStringLiteral("内置代码片段不能删除。"));
        return;
    }
    snippets_.removeAt(row);
    delete list_->takeItem(row);
    name_->clear();
    trigger_->clear();
    sql_->clear();
    save();
    setStatus(QStringLiteral("代码片段已删除。"));
}

void SnippetPanel::setStatus(const QString& text) {
    status_->setText(text);
    status_->setVisible(!text.isEmpty());
}
