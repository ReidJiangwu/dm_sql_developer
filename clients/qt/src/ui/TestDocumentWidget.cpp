#include "LogView.h"
#include "CommandButton.h"
#include "Theme.h"
#include "MessagePrompt.h"
#include "TestDocumentWidget.h"
#include "../codeintel/LanguageServerClient.h"
#include "SqlEditor.h"
#include "DiagnosticTextInteraction.h"
#include "CompletionSounds.h"

#include "../core/CommandScript.h"
#include "../core/DmdbgCallBuilder.h"
#include "../core/RuntimeErrorParser.h"
#include "../core/SqlTools.h"
#include "../core/TestCallBuilder.h"
#include "../data/DpiSession.h"
#include "../data/DmdbgProcessAdapter.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeySequence>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMouseEvent>
#include <QDateTime>
#include <QTextBlock>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QWidgetAction>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {

QList<int> boundedResultSizes(int editorSize, int resultSize) {
    const int total = qMax(2, editorSize + resultSize);
    const int minimumResult = qMin(180, qMax(80, total / 5));
    const int maximumResult = qMax(minimumResult, total * 35 / 100);
    const int boundedResult = qBound(minimumResult, resultSize, maximumResult);
    return {qMax(1, total - boundedResult), boundedResult};
}

QString compactActivityText(QString detail) {
    detail.replace(QLatin1Char('\r'), QLatin1Char(' '));
    detail.replace(QLatin1Char('\n'), QStringLiteral(" · "));
    return detail.simplified();
}

constexpr auto kTestAssetFormat = "reid-dm-test";
constexpr int kTestAssetVersion = 3;

class DirectionDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
            const QModelIndex&) const override {
        auto* editor = new QComboBox(parent);
        editor->addItems({QStringLiteral("IN"), QStringLiteral("OUT"),
            QStringLiteral("IN OUT"), QStringLiteral("RETURN")});
        return editor;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        auto* combo = qobject_cast<QComboBox*>(editor);
        if (combo == nullptr) return;
        combo->setCurrentText(index.data(Qt::EditRole).toString());
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model,
            const QModelIndex& index) const override {
        auto* combo = qobject_cast<QComboBox*>(editor);
        if (combo != nullptr) model->setData(index, combo->currentText(), Qt::EditRole);
    }
};

QString directionText(ProgramParameterDirection direction) {
    switch (direction) {
    case ProgramParameterDirection::Out: return QStringLiteral("OUT");
    case ProgramParameterDirection::InOut: return QStringLiteral("IN OUT");
    case ProgramParameterDirection::Return: return QStringLiteral("RETURN");
    default: return QStringLiteral("IN");
    }
}

bool acceptsInput(ProgramParameterDirection direction) {
    return direction == ProgramParameterDirection::In
        || direction == ProgramParameterDirection::InOut;
}

QString boundedOutput(QString value) {
    constexpr qsizetype kMaximumCharacters = 1'000'000;
    if (value.size() <= kMaximumCharacters) return value;
    value.truncate(kMaximumCharacters);
    value.append(QStringLiteral(
        "\n\n[输出已截断：Test Window 最多显示 1000000 个字符。]"));
    return value;
}

QString objectTypeName(DatabaseObjectType type) {
    switch (type) {
    case DatabaseObjectType::Table: return QStringLiteral("TABLE");
    case DatabaseObjectType::View: return QStringLiteral("VIEW");
    case DatabaseObjectType::MaterializedView: return QStringLiteral("MATERIALIZED_VIEW");
    case DatabaseObjectType::Sequence: return QStringLiteral("SEQUENCE");
    case DatabaseObjectType::Synonym: return QStringLiteral("SYNONYM");
    case DatabaseObjectType::Procedure: return QStringLiteral("PROCEDURE");
    case DatabaseObjectType::Function: return QStringLiteral("FUNCTION");
    case DatabaseObjectType::Package: return QStringLiteral("PACKAGE");
    case DatabaseObjectType::PackageBody: return QStringLiteral("PACKAGE_BODY");
    case DatabaseObjectType::Trigger: return QStringLiteral("TRIGGER");
    case DatabaseObjectType::DatabaseLink: return QStringLiteral("DATABASE_LINK");
    case DatabaseObjectType::UserDefinedType: return QStringLiteral("USER_DEFINED_TYPE");
    }
    return QStringLiteral("PROCEDURE");
}

bool objectTypeFromValue(const QJsonValue& value, DatabaseObjectType* type) {
    if (type == nullptr) return false;
    if (value.isDouble()) {
        if (!std::isfinite(value.toDouble())
                || value.toDouble() != std::trunc(value.toDouble())) {
            return false;
        }
        const int raw = value.toInt(-1);
        if (raw < static_cast<int>(DatabaseObjectType::Table)
                || raw > static_cast<int>(DatabaseObjectType::UserDefinedType)) {
            return false;
        }
        *type = static_cast<DatabaseObjectType>(raw);
        return true;
    }
    const QString name = value.toString().trimmed().toUpper();
    static const QMap<QString, DatabaseObjectType> values{
        {QStringLiteral("TABLE"), DatabaseObjectType::Table},
        {QStringLiteral("VIEW"), DatabaseObjectType::View},
        {QStringLiteral("MATERIALIZED_VIEW"), DatabaseObjectType::MaterializedView},
        {QStringLiteral("SEQUENCE"), DatabaseObjectType::Sequence},
        {QStringLiteral("SYNONYM"), DatabaseObjectType::Synonym},
        {QStringLiteral("PROCEDURE"), DatabaseObjectType::Procedure},
        {QStringLiteral("FUNCTION"), DatabaseObjectType::Function},
        {QStringLiteral("PACKAGE"), DatabaseObjectType::Package},
        {QStringLiteral("PACKAGE_BODY"), DatabaseObjectType::PackageBody},
        {QStringLiteral("TRIGGER"), DatabaseObjectType::Trigger},
        {QStringLiteral("DATABASE_LINK"), DatabaseObjectType::DatabaseLink},
        {QStringLiteral("USER_DEFINED_TYPE"), DatabaseObjectType::UserDefinedType},
    };
    const auto iterator = values.constFind(name);
    if (iterator == values.cend()) return false;
    *type = iterator.value();
    return true;
}

QString unescapeJavaProperty(const QString& text, bool* okay) {
    QString result;
    result.reserve(text.size());
    for (int index = 0; index < text.size(); ++index) {
        if (text.at(index) != QLatin1Char('\\')) {
            result.append(text.at(index));
            continue;
        }
        if (++index >= text.size()) {
            if (okay != nullptr) *okay = false;
            return {};
        }
        const QChar escaped = text.at(index);
        if (escaped == QLatin1Char('t')) result.append(QLatin1Char('\t'));
        else if (escaped == QLatin1Char('n')) result.append(QLatin1Char('\n'));
        else if (escaped == QLatin1Char('r')) result.append(QLatin1Char('\r'));
        else if (escaped == QLatin1Char('f')) result.append(QLatin1Char('\f'));
        else if (escaped == QLatin1Char('u')) {
            if (index + 4 >= text.size()) {
                if (okay != nullptr) *okay = false;
                return {};
            }
            bool converted = false;
            const ushort code = text.mid(index + 1, 4).toUShort(&converted, 16);
            if (!converted) {
                if (okay != nullptr) *okay = false;
                return {};
            }
            result.append(QChar(code));
            index += 4;
        } else {
            result.append(escaped);
        }
    }
    if (okay != nullptr) *okay = true;
    return result;
}

QMap<QString, QString> parseJavaProperties(const QByteArray& state, bool* okay) {
    QMap<QString, QString> values;
    const QStringList lines = QString::fromUtf8(state).split(
        QRegularExpression(QStringLiteral("\r?\n")));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))
                || line.startsWith(QLatin1Char('!'))) continue;
        int separator = -1;
        bool escaped = false;
        for (int index = 0; index < line.size(); ++index) {
            const QChar current = line.at(index);
            if (!escaped && (current == QLatin1Char('=')
                    || current == QLatin1Char(':') || current.isSpace())) {
                separator = index;
                break;
            }
            if (current == QLatin1Char('\\') && !escaped) escaped = true;
            else escaped = false;
        }
        if (separator < 0) separator = line.size();
        int valueStart = separator;
        while (valueStart < line.size()
                && (line.at(valueStart).isSpace()
                    || line.at(valueStart) == QLatin1Char('=')
                    || line.at(valueStart) == QLatin1Char(':'))) {
            ++valueStart;
        }
        bool keyOkay = false;
        bool valueOkay = false;
        const QString key = unescapeJavaProperty(
            line.left(separator).trimmed(), &keyOkay);
        const QString value = unescapeJavaProperty(line.mid(valueStart), &valueOkay);
        if (!keyOkay || !valueOkay || key.isEmpty()) {
            if (okay != nullptr) *okay = false;
            return {};
        }
        values.insert(key, value);
    }
    if (okay != nullptr) *okay = true;
    return values;
}

struct ParsedTestAsset {
    ObjectKey key;
    QString routine;
    QString signature;
    QJsonObject workspace;
};

bool isExactJsonInteger(const QJsonValue& value, int expected) {
    return value.isDouble()
        && value.toDouble() == static_cast<double>(expected);
}

bool isJsonInteger(const QJsonValue& value) {
    return value.isDouble() && std::isfinite(value.toDouble())
        && value.toDouble() == std::trunc(value.toDouble());
}

bool parseTestAsset(const QByteArray& state, ParsedTestAsset* parsed, QString* error) {
    if (parsed == nullptr || state.trimmed().isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("Test Script 资产为空。");
        return false;
    }
    QJsonObject root;
    const QJsonDocument document = QJsonDocument::fromJson(state);
    if (document.isObject()) {
        root = document.object();
    } else {
        bool propertiesOkay = false;
        const QMap<QString, QString> properties =
            parseJavaProperties(state, &propertiesOkay);
        if (!propertiesOkay || properties.value(QStringLiteral("version")) != QStringLiteral("2")) {
            if (error != nullptr) {
                *error = QStringLiteral("不是受支持的小智达梦管理工具 Test Script 资产。");
            }
            return false;
        }
        root.insert(QStringLiteral("version"), 2);
        root.insert(QStringLiteral("script"), properties.value(QStringLiteral("script")));
        root.insert(QStringLiteral("connectionId"),
            properties.value(QStringLiteral("connectionId")));
        root.insert(QStringLiteral("schema"), properties.value(QStringLiteral("schema")));
        root.insert(QStringLiteral("objectType"),
            properties.value(QStringLiteral("objectType"), QStringLiteral("PROCEDURE")));
        root.insert(QStringLiteral("objectName"),
            properties.value(QStringLiteral("objectName")));
        root.insert(QStringLiteral("routine"), properties.value(QStringLiteral("routine")));
        root.insert(QStringLiteral("signature"), properties.value(QStringLiteral("signature")));
        root.insert(QStringLiteral("breakpointLine"),
            properties.value(QStringLiteral("breakpoints"), QStringLiteral("1"))
                .section(QLatin1Char(','), 0, 0).toInt());
        root.insert(QStringLiteral("resultTab"),
            properties.value(QStringLiteral("resultTab")).toInt());
        const double ratio = properties.value(
            QStringLiteral("splitRatio"), QStringLiteral("0.6")).toDouble();
        root.insert(QStringLiteral("splitSizes"), QJsonArray{
            qRound(1000.0 * std::clamp(ratio, 0.2, 0.8)),
            qRound(1000.0 * (1.0 - std::clamp(ratio, 0.2, 0.8)))});
        QJsonArray arguments;
        const int count = properties.value(QStringLiteral("argument.count")).toInt();
        for (int index = 0; index < count; ++index) {
            const QString prefix = QStringLiteral("argument.%1.").arg(index);
            QJsonObject argument;
            argument.insert(QStringLiteral("name"),
                properties.value(prefix + QStringLiteral("name")));
            argument.insert(QStringLiteral("type"),
                properties.value(prefix + QStringLiteral("type"), QStringLiteral("VARCHAR2")));
            argument.insert(QStringLiteral("direction"),
                properties.value(prefix + QStringLiteral("direction"), QStringLiteral("IN")));
            argument.insert(QStringLiteral("value"),
                properties.value(prefix + QStringLiteral("value")));
            argument.insert(QStringLiteral("null"),
                properties.value(prefix + QStringLiteral("null")) == QStringLiteral("true"));
            argument.insert(QStringLiteral("default"),
                properties.value(prefix + QStringLiteral("default")) == QStringLiteral("true"));
            arguments.append(argument);
        }
        root.insert(QStringLiteral("arguments"), arguments);
    }

    const bool currentFormat = root.value(QStringLiteral("format")).toString()
        == QString::fromLatin1(kTestAssetFormat);
    if (currentFormat) {
        if (!isExactJsonInteger(
                root.value(QStringLiteral("version")), kTestAssetVersion)
                || !root.value(QStringLiteral("script")).isString()
                || !root.value(QStringLiteral("target")).isObject()
                || !root.value(QStringLiteral("ui")).isObject()
                || !root.value(QStringLiteral("variables")).isArray()) {
            if (error != nullptr) *error = QStringLiteral("不支持的 Test Script 资产格式或版本。");
            return false;
        }
        const QJsonObject target = root.value(QStringLiteral("target")).toObject();
        if (target.contains(QStringLiteral("identity")) && !target.value(QStringLiteral("identity")).isString()) {
            if (error != nullptr) *error = QStringLiteral("Test Script 目标身份字段无效。");
            return false;
        }
        const QStringList requiredTargetFields{QStringLiteral("connectionId"),
            QStringLiteral("schema"), QStringLiteral("objectType"),
            QStringLiteral("objectName"), QStringLiteral("routine"),
            QStringLiteral("signature")};
        for (const QString& field : requiredTargetFields) {
            if (!target.value(field).isString()) {
                if (error != nullptr) *error = QStringLiteral("Test Script 目标字段无效：%1").arg(field);
                return false;
            }
        }
        QJsonObject workspace;
        workspace.insert(QStringLiteral("version"), 2);
        workspace.insert(QStringLiteral("script"), root.value(QStringLiteral("script")));
        workspace.insert(QStringLiteral("connectionId"), target.value(QStringLiteral("connectionId")));
        workspace.insert(QStringLiteral("schema"), target.value(QStringLiteral("schema")));
        workspace.insert(QStringLiteral("objectType"), target.value(QStringLiteral("objectType")));
        workspace.insert(QStringLiteral("objectName"), target.value(QStringLiteral("objectName")));
        workspace.insert(QStringLiteral("routine"), target.value(QStringLiteral("routine")));
        workspace.insert(QStringLiteral("signature"), target.value(QStringLiteral("signature")));
        workspace.insert(QStringLiteral("targetIdentity"), target.value(QStringLiteral("identity")));
        const QJsonObject ui = root.value(QStringLiteral("ui")).toObject();
        if (!ui.value(QStringLiteral("splitRatio")).isDouble()
                || !isJsonInteger(ui.value(QStringLiteral("resultTab")))
                || ui.value(QStringLiteral("resultTab")).toInt() < 0
                || !ui.value(QStringLiteral("breakpoints")).isArray()) {
            if (error != nullptr) *error = QStringLiteral("Test Script ui 字段无效。");
            return false;
        }
        const double ratio = std::clamp(
            ui.value(QStringLiteral("splitRatio")).toDouble(0.6), 0.2, 0.8);
        workspace.insert(QStringLiteral("splitSizes"),
            QJsonArray{qRound(1000.0 * ratio), qRound(1000.0 * (1.0 - ratio))});
        workspace.insert(QStringLiteral("resultTab"),
            ui.value(QStringLiteral("resultTab")).toInt());
        if (ui.value(QStringLiteral("resultTabId")).isString()) {
            workspace.insert(QStringLiteral("resultTabId"), ui.value(QStringLiteral("resultTabId")));
        }
        const QJsonArray breakpoints = ui.value(QStringLiteral("breakpoints")).toArray();
        for (const QJsonValue& breakpoint : breakpoints) {
            if (!isJsonInteger(breakpoint) || breakpoint.toInt() <= 0) {
                if (error != nullptr) *error = QStringLiteral("Test Script 断点行号无效。");
                return false;
            }
        }
        workspace.insert(QStringLiteral("breakpointLine"),
            breakpoints.isEmpty() ? 1 : breakpoints.first().toInt(1));
        workspace.insert(QStringLiteral("breakpoints"), breakpoints);
        const QJsonValue watches = ui.value(QStringLiteral("watches"));
        if (!watches.isUndefined() && !watches.isArray()) {
            if (error != nullptr) *error = QStringLiteral("Test Script 监视表达式无效。");
            return false;
        }
        if (watches.isArray()) {
            for (const QJsonValue& watch : watches.toArray()) {
                if (!watch.isString() || watch.toString().trimmed().isEmpty()) {
                    if (error != nullptr) *error = QStringLiteral("Test Script 监视表达式无效。");
                    return false;
                }
            }
        }
        workspace.insert(QStringLiteral("watches"), watches.isArray()
            ? watches.toArray() : QJsonArray());
        workspace.insert(QStringLiteral("variableName"), QString());
        QJsonArray arguments;
        for (const QJsonValue& value : root.value(QStringLiteral("variables")).toArray()) {
            if (!value.isObject()) {
                if (error != nullptr) *error = QStringLiteral("Test Script variables 包含非法元素。");
                return false;
            }
            const QJsonObject variable = value.toObject();
            const QString direction = variable.value(QStringLiteral("direction")).toString();
            if (!variable.value(QStringLiteral("name")).isString()
                    || !variable.value(QStringLiteral("type")).isString()
                    || !variable.value(QStringLiteral("direction")).isString()
                    || !variable.value(QStringLiteral("value")).isString()
                    || !variable.value(QStringLiteral("null")).isBool()
                    || !variable.value(QStringLiteral("default")).isBool()
                    || (direction != QStringLiteral("IN")
                        && direction != QStringLiteral("OUT")
                        && direction != QStringLiteral("IN OUT")
                        && direction != QStringLiteral("RETURN"))) {
                if (error != nullptr) *error = QStringLiteral("Test Script 变量字段无效。");
                return false;
            }
            arguments.append(variable);
        }
        workspace.insert(QStringLiteral("arguments"), arguments);
        root = workspace;
    } else {
        if (!isExactJsonInteger(root.value(QStringLiteral("version")), 2)
                || !root.value(QStringLiteral("script")).isString()
                || !root.value(QStringLiteral("arguments")).isArray()) {
            if (error != nullptr) *error = QStringLiteral("不支持的 Test Script 资产格式或版本。");
            return false;
        }
        for (const QJsonValue& value : root.value(QStringLiteral("arguments")).toArray()) {
            if (!value.isObject()) {
                if (error != nullptr) *error = QStringLiteral("旧版 Test Script 参数字段无效。");
                return false;
            }
            const QString direction = value.toObject()
                .value(QStringLiteral("direction")).toString(QStringLiteral("IN")).toUpper();
            if (direction != QStringLiteral("IN")
                    && direction != QStringLiteral("OUT")
                    && direction != QStringLiteral("IN OUT")
                    && direction != QStringLiteral("IN_OUT")
                    && direction != QStringLiteral("RETURN")) {
                if (error != nullptr) *error = QStringLiteral("旧版 Test Script 参数方向无效。");
                return false;
            }
        }
    }

    ParsedTestAsset value;
    value.workspace = root;
    value.key.connectionId = root.value(QStringLiteral("connectionId")).toString();
    value.key.schema = root.value(QStringLiteral("schema")).toString();
    value.key.name = root.value(QStringLiteral("objectName")).toString();
    if (!objectTypeFromValue(root.value(QStringLiteral("objectType")), &value.key.type)) {
        if (error != nullptr) *error = QStringLiteral("Test Script 对象类型无效。");
        return false;
    }
    value.routine = root.value(QStringLiteral("routine")).toString();
    value.signature = root.value(QStringLiteral("signature")).toString();
    *parsed = std::move(value);
    return true;
}

}  // namespace

TestDocumentWidget::TestDocumentWidget(ObjectKey key, QWidget* parent, int documentNumber)
    : QWidget(parent), key_(std::move(key)), defaultTitle_(QStringLiteral("Test %1").arg(documentNumber)) {
    setWindowTitle(QStringLiteral("Test Window %1").arg(documentNumber));
    setObjectName(QStringLiteral("testDocumentWidget"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QToolBar(QStringLiteral("Test Window 操作"), this);
    toolbar->setObjectName(QStringLiteral("testContextToolBar"));
    toolbar->setProperty("documentContext", true);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    connectionSelector_ = new QToolButton(toolbar);
    connectionSelector_->setObjectName(QStringLiteral("testWindowConnectionSelector"));
    connectionSelector_->setProperty("textButton", true);
    connectionSelector_->setProperty("contextControl", true);
    connectionSelector_->setText(QStringLiteral("未连接"));
    connectionSelector_->setPopupMode(QToolButton::InstantPopup);
    connectionSelector_->setAccessibleName(QStringLiteral("Test Window 当前连接"));
    run_ = new QPushButton(QStringLiteral("执行"), toolbar);
    run_->setProperty("role", QStringLiteral("primary"));
    run_->setShortcut(QKeySequence(Qt::Key_F8));
    run_->setToolTip(QStringLiteral("执行 Test Script（F8）"));
    stop_ = new QPushButton(QStringLiteral("停止"), toolbar);
    stop_->setProperty("role", QStringLiteral("danger"));
    stop_->setProperty("quietAction", true);
    commit_ = new QPushButton(QStringLiteral("提交"), toolbar);
    commit_->setProperty("quietAction", true);
    rollback_ = new QPushButton(QStringLiteral("回滚"), toolbar);
    rollback_->setProperty("quietAction", true);
    runDebug_ = new QPushButton(QStringLiteral("开始调试"), toolbar);
    runDebug_->setProperty("quietAction", true);
    startDmdbg_ = new QPushButton(QStringLiteral("连接 dmdbg"), toolbar);
    continueDebug_ = new QPushButton(QStringLiteral("继续"), toolbar);
    stepInto_ = new QPushButton(QStringLiteral("进入"), toolbar);
    stepOver_ = new QPushButton(QStringLiteral("跳过"), toolbar);
    stepOut_ = new QPushButton(QStringLiteral("跳出"), toolbar);
    stopDebug_ = new QPushButton(QStringLiteral("结束调试"), toolbar);
    continueDebug_->setShortcut(QKeySequence(Qt::Key_F5));
    stepInto_->setShortcut(QKeySequence(Qt::Key_F6));
    stepOver_->setShortcut(QKeySequence(Qt::Key_F7));
    continueDebug_->setToolTip(QStringLiteral("继续（F5）"));
    stepInto_->setToolTip(QStringLiteral("进入（F6）"));
    stepOver_->setToolTip(QStringLiteral("跳过（F7）"));
    toolbar->addWidget(connectionSelector_);
    toolbar->addSeparator();
    toolbar->addWidget(run_);
    toolbar->addWidget(stop_);
    toolbar->addWidget(commit_);
    toolbar->addWidget(rollback_);
    toolbar->addWidget(runDebug_);
    auto* resultToggle = new QToolButton(toolbar);
    resultToggle->setObjectName(QStringLiteral("testResultToggle"));
    resultToggle->setProperty("textButton", true);
    resultToggle->setProperty("quietAction", true);
    resultToggle->setText(QStringLiteral("收起结果"));
    resultToggle->setToolTip(QStringLiteral("收起变量、输出、错误和调试结果区"));
    toolbar->addWidget(resultToggle);
    auto* toolbarSpacer = new QWidget(toolbar);
    toolbarSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(toolbarSpacer);
    activityState_ = new QLabel(key_.name.isEmpty()
        ? QStringLiteral("空白 Test Script") : QStringLiteral("等待程序元数据"), toolbar);
    activityState_->setObjectName(QStringLiteral("testActivityState"));
    activityState_->setMinimumWidth(0);
    activityState_->setMaximumWidth(360);
    activityState_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    activityState_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    toolbar->addWidget(activityState_);
    layout->addWidget(toolbar);

    connectionState_ = new QLabel(QStringLiteral("未连接"), this);
    transactionStateLabel_ = new QLabel(QStringLiteral("事务：无未提交"), this);
    connectionState_->hide();
    transactionStateLabel_->hide();

    splitter_ = new QSplitter(Qt::Vertical, this);
    splitter_->setObjectName(QStringLiteral("testWorkspaceSplitter"));
    splitter_->setAccessibleName(QStringLiteral("Test Script 与变量结果区大小调整"));
    splitter_->setChildrenCollapsible(true);
    splitter_->setHandleWidth(3);
    splitter_->setCollapsible(0, false);
    splitter_->setCollapsible(1, true);
    auto* scriptPage = new QWidget(splitter_);
    auto* scriptLayout = new QVBoxLayout(scriptPage);
    scriptLayout->setContentsMargins(0, 0, 0, 0);
    scriptLayout->setSpacing(0);
    auto* targetRow = new QWidget(scriptPage);
    targetRow->setObjectName(QStringLiteral("testTargetBar"));
    targetRow->setProperty("controlBar", true);
    auto* targetLayout = new QHBoxLayout(targetRow);
    targetLayout->setContentsMargins(8, 1, 8, 1);
    targetLayout->setSpacing(6);
    auto* targetCaption = new QLabel(QStringLiteral("目标"), targetRow);
    targetCaption->setObjectName(QStringLiteral("testTargetCaption"));
    targetCaption->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    targetLayout->addWidget(targetCaption);
    targetCombo_ = new QComboBox(targetRow);
    targetCombo_->setObjectName(QStringLiteral("testTargetSelector"));
    targetCombo_->setAccessibleName(QStringLiteral("程序重载"));
    targetCombo_->setMinimumWidth(240);
    targetCombo_->setMaximumWidth(560);
    targetCombo_->setMinimumContentsLength(24);
    targetCombo_->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    connect(targetCombo_, &QComboBox::currentTextChanged,
        targetCombo_, &QComboBox::setToolTip);
    targetLayout->addWidget(targetCombo_);
    targetLayout->addStretch(1);
    scriptLayout->addWidget(targetRow);
    scriptEditor_ = new SqlEditor(scriptPage);
    connect(LanguageServerClient::instance(), &LanguageServerClient::formattingFinished,
        this, [this](SqlEditor* editor, bool success, const QString& message) {
            if (editor == scriptEditor_ && messages_ != nullptr) {
                messages_->appendPlainText(QStringLiteral("SQL 格式化%1：%2").arg(
                    success ? QStringLiteral("完成") : QStringLiteral("失败"), message));
            }
        });
    scriptEditor_->setObjectName(QStringLiteral("testScriptEditor"));
    scriptEditor_->setAccessibleName(QStringLiteral("Test Script PL/SQL Block"));
    scriptEditor_->setPlaceholderText(QStringLiteral(
        "BEGIN\n   -- 在此编写可执行的 PL/SQL Test Script\nEND;\n/"));
    scriptEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    scriptEditor_->setFont(Theme::editorFont());
    scriptEditor_->setObjectNavigationEnabled(true);
    scriptEditor_->setToolTip(QStringLiteral("F12 转到当前源码定义；Ctrl/Command+单击优先定位定义，未找到时打开对象"));
    scriptEditor_->setTabStopDistance(
        scriptEditor_->fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 3);
    connect(scriptEditor_, &SqlEditor::objectNavigationRequested, this,
        [this](const QStringList& parts) {
            ObjectKey context = key_;
            context.connectionId = connectionProfileId();
            if (context.schema.isEmpty()) {
                context.schema = profile_.user.trimmed().toUpper();
            }
            emit objectDrillRequested(std::move(context), parts);
        });
    scriptLayout->addWidget(scriptEditor_, 1);
    splitter_->addWidget(scriptPage);

    resultTabs_ = new QTabWidget(splitter_);
    resultTabs_->setObjectName(QStringLiteral("testResultTabs"));
    auto* variablePage = new QWidget(resultTabs_);
    variablesPage_ = variablePage;
    variablePage->setObjectName(QStringLiteral("testVariablesPage"));
    auto* variableLayout = new QVBoxLayout(variablePage);
    variableLayout->setContentsMargins(0, 0, 0, 0);
    variableLayout->setSpacing(0);
    auto* variableControlsPane = new QWidget(variablePage);
    variableControlsPane->setObjectName(QStringLiteral("testVariableControls"));
    variableControlsPane->setProperty("controlBar", true);
    auto* variableControls = new QHBoxLayout(variableControlsPane);
    variableControls->setContentsMargins(8, 1, 8, 1);
    auto* variableHint = new QLabel(QStringLiteral(
        "变量来自 :name 绑定；手工变量请确认类型和方向。"), variablePage);
    variableControls->addWidget(variableHint);
    variableControls->addStretch(1);
    auto* scanInside = new QPushButton(QStringLiteral("扫描变量"), variablePage);
    scanInside->setObjectName(QStringLiteral("testScanVariables"));
    scanInside->setProperty("quietAction", true);
    variableHint->setObjectName(QStringLiteral("testVariableHint"));
    variableControls->addWidget(scanInside);
    variableLayout->addWidget(variableControlsPane);
    arguments_ = new QTableWidget(0, 7, variablePage);
    arguments_->setObjectName(QStringLiteral("testArguments"));
    arguments_->setHorizontalHeaderLabels({QStringLiteral("绑定变量"), QStringLiteral("类型"),
        QStringLiteral("方向"), QStringLiteral("输入值"), QStringLiteral("NULL"),
        QStringLiteral("使用默认值"), QStringLiteral("支持")});
    arguments_->verticalHeader()->setVisible(false);
    arguments_->verticalHeader()->setDefaultSectionSize(26);
    arguments_->horizontalHeader()->setStretchLastSection(false);
    arguments_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    arguments_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    arguments_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    arguments_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    arguments_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    arguments_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    arguments_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    arguments_->setColumnWidth(0, 160);
    arguments_->setColumnWidth(1, 150);
    arguments_->setAlternatingRowColors(true);
    arguments_->setShowGrid(false);
    arguments_->setWordWrap(false);
    arguments_->setSelectionBehavior(QAbstractItemView::SelectRows);
    arguments_->setItemDelegateForColumn(2, new DirectionDelegate(arguments_));
    variableLayout->addWidget(arguments_, 1);
    resultTabs_->addTab(variablePage, QStringLiteral("参数"));

    outputs_ = new QTableWidget(0, 6, resultTabs_);
    outputs_->setObjectName(QStringLiteral("testOutputs"));
    outputs_->setHorizontalHeaderLabels({QStringLiteral("参数"), QStringLiteral("方向"),
        QStringLiteral("调用前"), QStringLiteral("调用前 NULL"),
        QStringLiteral("调用后"), QStringLiteral("调用后 NULL")});
    outputs_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    outputs_->verticalHeader()->setVisible(false);
    outputs_->horizontalHeader()->setStretchLastSection(false);
    outputs_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    outputs_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    outputs_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    outputs_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    outputs_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    outputs_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    outputs_->setColumnWidth(0, 240);
    outputs_->setColumnWidth(2, 240);
    parameterOutputs_ = new QWidget(variablePage);
    parameterOutputs_->setObjectName(QStringLiteral("testParameterOutputs"));
    auto* outputLayout = new QVBoxLayout(parameterOutputs_);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    outputLayout->addWidget(new QLabel(QStringLiteral("输出参数 · 调用前／调用后（只读）"), parameterOutputs_));
    outputLayout->addWidget(outputs_, 1);
    variableLayout->addWidget(parameterOutputs_, 1);
    parameterOutputs_->hide();
    scriptOutput_ = new QPlainTextEdit(resultTabs_); LogView::configure(scriptOutput_);
    scriptOutput_->setReadOnly(true);
    scriptOutput_->document()->setMaximumBlockCount(5'000);
    scriptOutput_->setObjectName(QStringLiteral("testDbmsOutput"));
    scriptOutput_->setPlaceholderText(QStringLiteral("本次执行暂无 DBMS_OUTPUT 输出。"));
    errors_ = new QTableWidget(0, 6, resultTabs_);
    errors_->setObjectName(QStringLiteral("testErrorStack"));
    errors_->setAccessibleName(
        QStringLiteral("程序错误堆栈，双击或按 Enter 打开对应程序对象"));
    errors_->setToolTip(
        QStringLiteral("双击堆栈帧或选中后按 Enter，可打开对应包、过程或函数并定位到错误行"));
    errors_->setContextMenuPolicy(Qt::CustomContextMenu);
    DiagnosticTextInteraction::setObjectNavigation(errors_, [this](const QVariant& value) {
        emit errorFrameActivated(value.value<RuntimeErrorFrame>());
    });
    errors_->setHorizontalHeaderLabels({QStringLiteral("Schema"), QStringLiteral("对象"),
        QStringLiteral("类型"), QStringLiteral("行"), QStringLiteral("列"),
        QStringLiteral("消息")});
    errors_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    errors_->verticalHeader()->setVisible(false);
    errors_->horizontalHeader()->setStretchLastSection(false);
    errors_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    errors_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    errors_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    errors_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    errors_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    errors_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    errors_->setColumnWidth(1, 480);
    connect(errors_, &QTableWidget::itemActivated, this,
        [this](QTableWidgetItem* item) {
            if (item != nullptr) activateErrorFrame(item->row());
        });
    connect(errors_, &QTableWidget::customContextMenuRequested, this,
        [this](const QPoint& point) {
            QTableWidgetItem* item = errors_->itemAt(point);
            if (item == nullptr) return;
            errors_->setCurrentCell(item->row(), item->column());
            QMenu menu(errors_);
            QAction* open = menu.addAction(QStringLiteral("定位程序对象"));
            const QVariant frame = errors_->item(item->row(), 0) == nullptr
                ? QVariant() : errors_->item(item->row(), 0)->data(Qt::UserRole);
            open->setEnabled(frame.isValid());
            connect(open, &QAction::triggered, this,
                [this, row = item->row()]() { activateErrorFrame(row); });
            menu.exec(errors_->viewport()->mapToGlobal(point));
        });

    messagesPage_ = new QWidget(resultTabs_);
    auto* messagesLayout = new QVBoxLayout(messagesPage_);
    messagesLayout->setContentsMargins(0, 0, 0, 0);
    auto* messagesToolbar = new QToolBar(QStringLiteral("消息操作"), messagesPage_);
    messagesToolbar->setProperty("secondaryToolBar", true);
    clearMessages_ = messagesToolbar->addAction(QStringLiteral("清空消息"));
    clearMessages_->setObjectName(QStringLiteral("clearTestMessages"));
    clearMessages_->setToolTip(QStringLiteral("清空消息和本次 DBMS_OUTPUT；执行期间不可清空。"));
    messagesLayout->addWidget(messagesToolbar);
    messages_ = new QPlainTextEdit(messagesPage_); LogView::configure(messages_);
    messagesLayout->addWidget(messages_, 1);
    dbmsOutputPane_ = new QWidget(resultTabs_);
    dbmsOutputPane_->setObjectName(QStringLiteral("testDbmsOutputPane"));
    auto* dbmsLayout = new QVBoxLayout(dbmsOutputPane_);
    dbmsLayout->setContentsMargins(0, 0, 0, 0);
    dbmsLayout->addWidget(new QLabel(QStringLiteral("DBMS_OUTPUT（本次执行）"), dbmsOutputPane_));
    dbmsLayout->addWidget(scriptOutput_, 1);
    connect(clearMessages_, &QAction::triggered, this, [this]() {
        if (isBusy()) return;
        messages_->clear();
        scriptOutput_->clear();
    });
    messages_->setObjectName(QStringLiteral("testMessages"));
    messages_->setReadOnly(true);
    messages_->setToolTip(QStringLiteral("Ctrl/Command+单击对象名称，或右键选择“打开对象”。"));
    messages_->viewport()->installEventFilter(this);
    messages_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(messages_, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        const QStringList parts = messageObjectAt(position);
        QMenu* menu = messages_->createStandardContextMenu();
        menu->addSeparator();
        QAction* open = menu->addAction(QStringLiteral("打开对象"));
        open->setEnabled(!parts.isEmpty() && isConnected());
        connect(open, &QAction::triggered, this, [this, parts]() { openMessageObject(parts); });
        menu->addSeparator();
        menu->addAction(clearMessages_);
        menu->exec(messages_->viewport()->mapToGlobal(position));
        delete menu;
    });
    messages_->document()->setMaximumBlockCount(2'000);
    messagesPage_->setObjectName(QStringLiteral("testMessagesPage"));
    resultTabs_->addTab(messagesPage_, QStringLiteral("消息"));
    resultTabs_->addTab(dbmsOutputPane_, QStringLiteral("输出"));
    resultTabs_->addTab(errors_, QStringLiteral("错误堆栈"));

    auto* debugPage = new QWidget(resultTabs_);
    auto* debugLayout = new QVBoxLayout(debugPage);
    debugLayout->setContentsMargins(0, 0, 0, 0);
    debugPage->setObjectName(QStringLiteral("testDebugPage"));
    auto* debugStepToolbar = new QToolBar(QStringLiteral("调试操作"), debugPage);
    debugStepToolbar->setObjectName(QStringLiteral("testDebugStepToolbar"));
    debugStepToolbar->setProperty("secondaryToolBar", true);
    debugStepToolbar->setMovable(false);
    debugStepToolbar->setFloatable(false);
    debugStepToolbar->addWidget(startDmdbg_);
    debugStepToolbar->addWidget(continueDebug_);
    debugStepToolbar->addWidget(stepInto_);
    debugStepToolbar->addWidget(stepOver_);
    debugStepToolbar->addWidget(stepOut_);
    debugStepToolbar->addWidget(stopDebug_);
    debugStatus_ = new QLabel(debugStepToolbar);
    debugStatus_->setObjectName(QStringLiteral("testDebugStatus"));
    debugStepToolbar->addWidget(debugStatus_);
    debugLayout->addWidget(debugStepToolbar);
    auto* debugTabs = new QTabWidget(debugPage);
    auto* stackPage = new QWidget(debugTabs);
    auto* stackLayout = new QVBoxLayout(stackPage);
    stackLayout->setContentsMargins(0, 0, 0, 0);
    auto* frameControls = new QHBoxLayout();
    frameUp_ = new QPushButton(QStringLiteral("上移栈帧"), stackPage);
    frameDown_ = new QPushButton(QStringLiteral("下移栈帧"), stackPage);
    frameControls->addWidget(frameUp_);
    frameControls->addWidget(frameDown_);
    frameControls->addStretch(1);
    stackLayout->addLayout(frameControls);
    stackTable_ = new QTableWidget(0, 6, stackPage);
    stackTable_->setHorizontalHeaderLabels({QStringLiteral("帧"), QStringLiteral("Schema"),
        QStringLiteral("程序"), QStringLiteral("参数"), QStringLiteral("行"),
        QStringLiteral("源码")});
    stackTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    stackTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    stackTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    stackTable_->verticalHeader()->setVisible(false);
    stackTable_->horizontalHeader()->setStretchLastSection(false);
    stackTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    stackTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    stackTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    stackTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    stackTable_->setColumnWidth(2, 480);
    stackTable_->setColumnWidth(3, 240);
    stackLayout->addWidget(stackTable_);
    debugTabs->addTab(stackPage, QStringLiteral("调用栈"));
    auto* watchPage = new QWidget(debugTabs);
    auto* watchLayout = new QVBoxLayout(watchPage);
    auto* watchControls = new QHBoxLayout();
    variableName_ = new QLineEdit(watchPage);
    variableName_->setPlaceholderText(QStringLiteral("监视表达式，例如 V_ACC 或 REC.ID"));
    inspectVariable_ = new QPushButton(QStringLiteral("添加"), watchPage);
    refreshWatches_ = CommandButton::refresh(watchPage, QStringLiteral("刷新监视表达式"));
    removeWatch_ = new QPushButton(QStringLiteral("移除"), watchPage);
    watchControls->addWidget(variableName_, 1);
    watchControls->addWidget(inspectVariable_);
    watchControls->addWidget(refreshWatches_);
    watchControls->addWidget(removeWatch_);
    watchLayout->addLayout(watchControls);
    watchTable_ = new QTableWidget(0, 2, watchPage);
    watchTable_->setHorizontalHeaderLabels(
        {QStringLiteral("表达式"), QStringLiteral("值／错误")});
    watchTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    watchTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    watchTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    watchTable_->verticalHeader()->setVisible(false);
    watchTable_->horizontalHeader()->setStretchLastSection(false);
    watchTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    watchTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    watchTable_->setColumnWidth(0, 320);
    watchLayout->addWidget(watchTable_);
    debugTabs->addTab(watchPage, QStringLiteral("监视"));
    auto* breakpointPage = new QWidget(debugTabs);
    auto* breakpointLayout = new QVBoxLayout(breakpointPage);
    auto* breakpointControls = new QHBoxLayout();
    breakpointLine_ = new QSpinBox(breakpointPage);
    breakpointLine_->setRange(1, 1'000'000);
    addBreakpoint_ = new QPushButton(QStringLiteral("添加断点"), breakpointPage);
    removeBreakpoint_ = new QPushButton(QStringLiteral("移除"), breakpointPage);
    clearBreakpoints_ = new QPushButton(QStringLiteral("清空"), breakpointPage);
    breakpointControls->addWidget(new QLabel(QStringLiteral("目标源码行"), breakpointPage));
    breakpointControls->addWidget(breakpointLine_);
    breakpointControls->addWidget(addBreakpoint_);
    breakpointControls->addWidget(removeBreakpoint_);
    breakpointControls->addWidget(clearBreakpoints_);
    breakpointControls->addStretch(1);
    breakpointLayout->addLayout(breakpointControls);
    breakpointTable_ = new QTableWidget(0, 5, breakpointPage);
    breakpointTable_->setHorizontalHeaderLabels({QStringLiteral("状态"),
        QStringLiteral("编号"), QStringLiteral("Schema"), QStringLiteral("程序"),
        QStringLiteral("行")});
    breakpointTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    breakpointTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    breakpointTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    breakpointTable_->verticalHeader()->setVisible(false);
    breakpointTable_->horizontalHeader()->setStretchLastSection(false);
    breakpointTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    breakpointTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    breakpointTable_->setColumnWidth(3, 480);
    breakpointLayout->addWidget(breakpointTable_);
    debugTabs->addTab(breakpointPage, QStringLiteral("断点"));
    debugLayout->addWidget(debugTabs, 1);
    resultTabs_->addTab(debugPage, QStringLiteral("调试"));
    splitter_->addWidget(resultTabs_);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 1);
    splitter_->setSizes({760, 240});
    layout->addWidget(splitter_, 1);

    const auto updateResultToggle = [splitter = splitter_, resultToggle]() {
        const QList<int> sizes = splitter->sizes();
        const bool compact = sizes.size() > 1 && sizes.at(1) <= 48;
        resultToggle->setText(compact
            ? QStringLiteral("展开结果") : QStringLiteral("收起结果"));
        resultToggle->setToolTip(compact
            ? QStringLiteral("展开变量、输出、错误和调试结果区")
            : QStringLiteral("收起变量、输出、错误和调试结果区"));
    };
    connect(resultToggle, &QToolButton::clicked, this,
        [splitter = splitter_, resultToggle, updateResultToggle]() {
            const QList<int> sizes = splitter->sizes();
            if (sizes.size() < 2) return;
            const int total = sizes.at(0) + sizes.at(1);
            if (sizes.at(1) > 48) {
                resultToggle->setProperty("expandedResultHeight", sizes.at(1));
                splitter->setSizes({qMax(0, total - 36), 36});
            } else {
                const int requested = qMax(180,
                    resultToggle->property("expandedResultHeight").toInt());
                const QList<int> restored = boundedResultSizes(
                    qMax(1, total - requested), requested);
                splitter->setSizes(restored);
            }
            updateResultToggle();
        });
    connect(splitter_, &QSplitter::splitterMoved, this,
        [updateResultToggle](int, int) { updateResultToggle(); });
    updateResultToggle();

    variableScanTimer_ = new QTimer(this);
    variableScanTimer_->setSingleShot(true);
    variableScanTimer_->setInterval(300);
    connect(scanInside, &QPushButton::clicked, this, &TestDocumentWidget::scanVariables);
    connect(scriptEditor_, &QPlainTextEdit::textChanged, this, [this]() {
        if (updatingScript_) return;
        dmdbgConfigured_ = false;
        dmdbgSetupStage_ = DmdbgSetupStage::None;
        pendingDmdbgSql_.clear();
        scriptEditor_->document()->setModified(true);
        variableScanTimer_->start();
        updateActions();
    });
    connect(variableScanTimer_, &QTimer::timeout, this, &TestDocumentWidget::scanVariables);
    connect(arguments_, &QTableWidget::itemChanged, this,
        [this](QTableWidgetItem* item) {
            if (updatingArguments_) return;
            if (item != nullptr && item->column() <= 2
                    && item->flags().testFlag(Qt::ItemIsEditable)) {
                assetModified_ = true;
            }
            updateActions();
        });
    connect(resultTabs_, &QTabWidget::currentChanged, this, [this](int) {
        assetModified_ = true;
        emit statusChanged();
    });
    connect(splitter_, &QSplitter::splitterMoved, this, [this](int, int) {
        assetModified_ = true;
        emit statusChanged();
    });
    connect(breakpointLine_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        assetModified_ = true;
        emit statusChanged();
    });

    sessionThread_ = new QThread(this);
    sessionThread_->setObjectName(QStringLiteral("testDpiSessionThread"));
    session_ = new DpiSession();
    session_->moveToThread(sessionThread_);
    connect(sessionThread_, &QThread::finished, session_, &QObject::deleteLater);
    connect(session_, &DpiSession::connected, this, [this](const ConnectionProfile& profile) {
        profile_ = profile;
        connectingProfile_ = {};
        connected_ = true;
        setTransactionState(TestTransactionState::Clean);
        connectionState_->setText(QStringLiteral("已连接：%1").arg(profile.name));
        QMetaObject::invokeMethod(session_, &DpiSession::setScriptOutputEnabled,
            Qt::QueuedConnection, true);
        updateActions();
    });
    connect(session_, &DpiSession::activeEndpointChanged, this,
        [this](const ConnectionEndpoint& endpoint,
            const ObservedDatabaseIdentity& identity) {
            activeEndpoint_ = endpoint;
            activeIdentity_ = identity;
            connectionState_->setText(QStringLiteral("已连接：%1｜%2:%3｜%4")
                .arg(profile_.name, endpoint.host)
                .arg(endpoint.port)
                .arg(identity.summary()));
            emit statusChanged();
        });
    connect(session_, &DpiSession::stateChanged, this,
        [this](SessionState state, const QString& detail) {
            if (state == SessionState::Cancelling) executionSoundCancelled_ = true;
            busy_ = state == SessionState::Connecting || state == SessionState::Executing
                || state == SessionState::Cancelling;
            activityState_->setToolTip(detail);
            activityState_->setText(compactActivityText(detail));
            if (state == SessionState::Disconnected) {
                connected_ = false;
                if (transactionState_ != TestTransactionState::OutcomeUnknown) {
                    setTransactionState(TestTransactionState::ConnectionLost);
                }
            }
            updateActions();
        });
    connect(session_, &DpiSession::transactionStateChanged, this, [this](bool pending) {
        if (pending) {
            setTransactionState(TestTransactionState::PendingDml);
        } else if (transactionState_ != TestTransactionState::OutcomeUnknown
                && transactionState_ != TestTransactionState::ConnectionLost) {
            setTransactionState(TestTransactionState::Clean);
        }
        updateActions();
    });
    connect(session_, &DpiSession::testFinished,
        this, &TestDocumentWidget::applyResult);
    connect(session_, &DpiSession::scriptOutput, this, [this](const QString& output) {
        scriptOutput_->setPlainText(boundedOutput(output));
    });
    connect(session_, &DpiSession::message, this, [this](const QString& message) {
        messages_->appendPlainText(message);
    });
    connect(session_, &DpiSession::failed, this, [this](const SessionError& error) {
        if (!lastHandledTestError_.isEmpty()
                && error.message.trimmed() == lastHandledTestError_) {
            lastHandledTestError_.clear();
            if (!error.recovery.trimmed().isEmpty()) {
                messages_->appendPlainText(
                    QStringLiteral("处理建议：%1").arg(error.recovery.trimmed()));
            }
            return;
        }
        if (!error.message.isEmpty()
                && messages_->toPlainText().trimmed().endsWith(error.message.trimmed())) {
            if (!error.recovery.trimmed().isEmpty()) {
                messages_->appendPlainText(
                    QStringLiteral("处理建议：%1").arg(error.recovery.trimmed()));
            }
            return;
        }
        messages_->appendPlainText(QStringLiteral("[%1/%2] %3\n%4")
            .arg(error.category).arg(error.code).arg(error.message, error.recovery));
    });
    sessionThread_->start();

    dmdbg_ = new DmdbgProcessAdapter(this);
    dmdbgPath_ = findDmdbg(QCoreApplication::applicationDirPath());
    connect(dmdbg_, &DmdbgProcessAdapter::stateChanged, this,
        [this](DmdbgProcessState, const QString& detail) {
            activityState_->setToolTip(detail);
            activityState_->setText(compactActivityText(detail));
            updateActions();
        });
    connect(dmdbg_, &DmdbgProcessAdapter::runtimeStateChanged, this,
        [this](DmdbgRuntimeState state) {
            dmdbgPaused_ = state == DmdbgRuntimeState::Debugging;
            if (!dmdbgPaused_) scriptEditor_->setDebugLine(0);
            updateActions();
        });
    connect(dmdbg_, &DmdbgProcessAdapter::ready, this,
        [this](const DebuggerCapabilities& capabilities) {
            dmdbgCapabilities_ = capabilities;
            if (!capabilities.limitation.isEmpty()) {
                messages_->appendPlainText(capabilities.limitation);
            }
            configureDmdbgTarget();
        });
    connect(dmdbg_, &DmdbgProcessAdapter::commandCompleted, this,
        [this](const QString& command, const QString& response) {
            appendDmdbgResponse(command, response);
        });
    connect(dmdbg_, &DmdbgProcessAdapter::commandFailed, this,
        &TestDocumentWidget::handleDmdbgCommandFailure);
    connect(dmdbg_, &DmdbgProcessAdapter::failed, this, [this](const QString& detail) {
        pendingDebugRun_ = false;
        pendingEntryBreakpoint_ = false;
        entryBreakpointNumber_ = 0;
        dmdbgSetupStage_ = DmdbgSetupStage::None;
        pendingDmdbgSql_.clear();
        dmdbgConfigured_ = false;
        messages_->appendPlainText(QStringLiteral("dmdbg：%1").arg(detail));
        showDebugMessages();
        updateActions();
    });
    connect(dmdbg_, &DmdbgProcessAdapter::stopped, this, [this](bool normal) {
        pendingDebugRun_ = false;
        pendingEntryBreakpoint_ = false;
        entryBreakpointNumber_ = 0;
        dmdbgSetupStage_ = DmdbgSetupStage::None;
        pendingDmdbgSql_.clear();
        dmdbgConfigured_ = false;
        dmdbgPaused_ = false;
        pendingBreakpointLines_.clear();
        pendingBreakpointDeletes_.clear();
        deletedBreakpointLines_.clear();
        failedBreakpointDeletes_.clear();
        activeBreakpointDelete_ = 0;
        pendingWatchExpressions_.clear();
        activeWatchExpression_.clear();
        for (auto iterator = breakpoints_.begin(); iterator != breakpoints_.end(); ++iterator) {
            iterator.value() = 0;
        }
        refreshBreakpointTable();
        scriptEditor_->setDebugLine(0);
        messages_->appendPlainText(normal
            ? QStringLiteral("dmdbg 已正常结束。")
            : QStringLiteral("dmdbg 会话异常结束。"));
        updateActions();
    });

    connect(targetCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
        this, &TestDocumentWidget::rebuildArguments);
    connect(run_, &QPushButton::clicked, this, &TestDocumentWidget::executeTest);
    connect(stop_, &QPushButton::clicked, this, &TestDocumentWidget::cancelCurrentTask);
    connect(commit_, &QPushButton::clicked, this, &TestDocumentWidget::commitTransaction);
    connect(rollback_, &QPushButton::clicked, this, &TestDocumentWidget::rollbackTransaction);
    connect(startDmdbg_, &QPushButton::clicked, this, &TestDocumentWidget::beginDebugging);
    connect(runDebug_, &QPushButton::clicked, this, &TestDocumentWidget::beginDebugging);
    connect(continueDebug_, &QPushButton::clicked,
        dmdbg_, &DmdbgProcessAdapter::continueExecution);
    connect(stepInto_, &QPushButton::clicked, dmdbg_, &DmdbgProcessAdapter::stepInto);
    connect(stepOver_, &QPushButton::clicked, dmdbg_, &DmdbgProcessAdapter::stepOver);
    connect(stepOut_, &QPushButton::clicked, dmdbg_, &DmdbgProcessAdapter::stepOut);
    connect(stopDebug_, &QPushButton::clicked, dmdbg_, &DmdbgProcessAdapter::stop);
    connect(addBreakpoint_, &QPushButton::clicked, this,
        [this]() { addBreakpoint(breakpointLine_->value()); });
    connect(removeBreakpoint_, &QPushButton::clicked,
        this, &TestDocumentWidget::removeSelectedBreakpoint);
    connect(clearBreakpoints_, &QPushButton::clicked, this, [this]() {
        const QList<int> lines = breakpoints_.keys();
        for (int line : lines) {
            const int number = breakpoints_.value(line);
            breakpoints_.remove(line);
            if (number > 0) {
                deletedBreakpointLines_.insert(number, line);
                pendingBreakpointDeletes_.append(number);
            }
            failedBreakpointDeletes_.remove(line);
        }
        refreshBreakpointTable();
        synchronizePendingBreakpoints();
    });
    connect(inspectVariable_, &QPushButton::clicked,
        this, &TestDocumentWidget::addWatchExpression);
    connect(removeWatch_, &QPushButton::clicked,
        this, &TestDocumentWidget::removeSelectedWatch);
    connect(refreshWatches_, &QPushButton::clicked,
        this, &TestDocumentWidget::refreshDebugViews);
    connect(variableName_, &QLineEdit::returnPressed,
        this, &TestDocumentWidget::addWatchExpression);
    connect(stackTable_, &QTableWidget::cellDoubleClicked, this,
        [this](int row, int) {
            if (row < 0 || row >= stackTable_->rowCount()) return;
            DmdbgFrameInfo location;
            location.schema = stackTable_->item(row, 1)->text();
            location.method = stackTable_->item(row, 2)->text();
            location.line = stackTable_->item(row, 4)->text().toInt();
            showDebugLocation(location);
        });
    connect(frameUp_, &QPushButton::clicked, dmdbg_, &DmdbgProcessAdapter::frameUp);
    connect(frameDown_, &QPushButton::clicked, dmdbg_, &DmdbgProcessAdapter::frameDown);
    connect(variableName_, &QLineEdit::textChanged,
        this, &TestDocumentWidget::updateDebugActions);
    connect(breakpointTable_, &QTableWidget::itemSelectionChanged,
        this, &TestDocumentWidget::updateDebugActions);
    connect(watchTable_, &QTableWidget::itemSelectionChanged,
        this, &TestDocumentWidget::updateDebugActions);
    if (isBlankDocument()) {
        targetCombo_->addItem(QStringLiteral("手工 Test Script"));
        updatingScript_ = true;
        scriptEditor_->setPlainText(QStringLiteral("BEGIN\n   NULL;\nEND;\n/"));
        scriptEditor_->document()->setModified(false);
        updatingScript_ = false;
        activityState_->setText(QStringLiteral("可编辑，等待连接"));
    }
    updateActions();
}

TestDocumentWidget::~TestDocumentWidget() {
    if (dmdbg_ != nullptr && dmdbg_->state() != DmdbgProcessState::Stopped) {
        dmdbg_->stop();
    }
    if (session_ != nullptr && sessionThread_->isRunning()) {
        session_->requestCancel();
        QMetaObject::invokeMethod(
            session_, [session = session_, thread = sessionThread_]() {
                session->disconnectFromDatabase();
                thread->quit();
            }, Qt::QueuedConnection);
        if (!sessionThread_->wait(5'000)) {
            sessionThread_->setParent(nullptr);
            connect(sessionThread_, &QThread::finished,
                sessionThread_, &QObject::deleteLater);
            sessionThread_ = nullptr;
            session_ = nullptr;
        }
    } else if (sessionThread_ != nullptr) {
        sessionThread_->quit();
    }
}

const ObjectKey& TestDocumentWidget::key() const { return key_; }
QToolButton* TestDocumentWidget::connectionSelector() const { return connectionSelector_; }

bool TestDocumentWidget::isBlankDocument() const {
    return key_.name.trimmed().isEmpty();
}

bool TestDocumentWidget::matchesTarget(
        const ObjectKey& key, const QString& routineName, const QString& signature) const {
    if (isBlankDocument() || !(key_ == key)) return false;
    const QString expectedRoutine = routineName.trimmed().isEmpty()
        ? key.name : routineName;
    const QString currentRoutine = preferredRoutine_.trimmed().isEmpty()
        ? key_.name : preferredRoutine_;
    if (currentRoutine.compare(expectedRoutine, Qt::CaseInsensitive) != 0) return false;
    return signature.trimmed().isEmpty()
        || preferredSignature_.compare(signature, Qt::CaseInsensitive) == 0;
}

QString TestDocumentWidget::displayName() const {
    if (!filePath_.isEmpty()) {
        return QFileInfo(filePath_).fileName() + (isModified() ? QStringLiteral(" *") : QString());
    }
    if (isBlankDocument()) return defaultTitle_;
    return QStringLiteral("Test · %1.%2%3").arg(key_.schema, key_.name,
        isModified() ? QStringLiteral(" *") : QString());
}

QString TestDocumentWidget::scriptText() const {
    return scriptEditor_ == nullptr ? QString() : scriptEditor_->toPlainText();
}

bool TestDocumentWidget::isModified() const {
    return assetModified_
        || (scriptEditor_ != nullptr && scriptEditor_->document()->isModified());
}

void TestDocumentWidget::setModified(bool modified) {
    if (scriptEditor_ != nullptr) scriptEditor_->document()->setModified(modified);
    assetModified_ = modified;
}

QString TestDocumentWidget::filePath() const { return filePath_; }

bool TestDocumentWidget::inspectTestAsset(const QString& path,
        ObjectKey* key, QString* routineName, QString* signature, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    ParsedTestAsset parsed;
    if (!parseTestAsset(file.readAll(), &parsed, error)) return false;
    if (key != nullptr) *key = parsed.key;
    if (routineName != nullptr) *routineName = parsed.routine;
    if (signature != nullptr) *signature = parsed.signature;
    return true;
}

bool TestDocumentWidget::loadTestAsset(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    ParsedTestAsset parsed;
    if (!parseTestAsset(file.readAll(), &parsed, error)) return false;
    key_ = parsed.key;
    preferredRoutine_ = parsed.routine;
    preferredSignature_ = parsed.signature;
    restoreWorkspaceState(QJsonDocument(parsed.workspace).toJson(QJsonDocument::Compact));
    filePath_ = QFileInfo(path).absoluteFilePath();
    setModified(false);
    emit statusChanged();
    return true;
}

bool TestDocumentWidget::saveTestAsset(const QString& path, QString* error) {
    QJsonObject root;
    root.insert(QStringLiteral("format"), QString::fromLatin1(kTestAssetFormat));
    root.insert(QStringLiteral("version"), kTestAssetVersion);
    root.insert(QStringLiteral("script"), scriptText());
    QJsonObject target;
    target.insert(QStringLiteral("connectionId"), key_.connectionId);
    target.insert(QStringLiteral("schema"), key_.schema);
    target.insert(QStringLiteral("objectType"), objectTypeName(key_.type));
    target.insert(QStringLiteral("objectName"), key_.name);
    const int targetIndex = targetCombo_->currentIndex();
    if (targetIndex >= 0 && targetIndex < targets_.size()) {
        const ProgramTarget& selected = targets_.at(targetIndex);
        target.insert(QStringLiteral("routine"), selected.routineName);
        target.insert(QStringLiteral("signature"), selected.signature());
        target.insert(QStringLiteral("identity"), selected.identity());
    } else {
        target.insert(QStringLiteral("routine"), preferredRoutine_);
        target.insert(QStringLiteral("signature"), preferredSignature_);
        target.insert(QStringLiteral("identity"), argumentTargetIdentity_);
    }
    root.insert(QStringLiteral("target"), target);
    const QList<int> sizes = splitter_->sizes();
    const int totalSize = sizes.size() == 2 ? sizes.at(0) + sizes.at(1) : 0;
    const double splitRatio = totalSize > 0
        ? std::clamp(static_cast<double>(sizes.at(0)) / totalSize, 0.2, 0.8)
        : 0.6;
    QJsonObject ui;
    ui.insert(QStringLiteral("splitRatio"), splitRatio);
    const QStringList resultPageIds{QStringLiteral("parameters"), QStringLiteral("messages"),
        QStringLiteral("output"), QStringLiteral("errors"), QStringLiteral("debug")};
    const QList<int> legacyResultPages{0, 4, 2, 3, 5};
    ui.insert(QStringLiteral("resultTabId"), resultPageIds.value(resultTabs_->currentIndex()));
    ui.insert(QStringLiteral("resultTab"), legacyResultPages.value(resultTabs_->currentIndex()));
    QJsonArray savedBreakpoints;
    for (int line : breakpoints_.keys()) savedBreakpoints.append(line);
    ui.insert(QStringLiteral("breakpoints"), savedBreakpoints);
    QJsonArray savedWatches;
    for (const QString& expression : watches_.keys()) savedWatches.append(expression);
    ui.insert(QStringLiteral("watches"), savedWatches);
    root.insert(QStringLiteral("ui"), ui);
    QJsonArray variables;
    for (int row = 0; row < arguments_->rowCount(); ++row) {
        QJsonObject variable;
        variable.insert(QStringLiteral("name"), arguments_->item(row, 0) == nullptr
            ? QString() : arguments_->item(row, 0)->text());
        variable.insert(QStringLiteral("type"), arguments_->item(row, 1) == nullptr
            ? QStringLiteral("VARCHAR2") : arguments_->item(row, 1)->text());
        QString direction = arguments_->item(row, 2) == nullptr
            ? QStringLiteral("IN") : arguments_->item(row, 2)->text().trimmed().toUpper();
        if (direction == QStringLiteral("IN_OUT")) direction = QStringLiteral("IN OUT");
        variable.insert(QStringLiteral("direction"), direction);
        auto* value = qobject_cast<QLineEdit*>(arguments_->cellWidget(row, 3));
        auto* nullValue = qobject_cast<QCheckBox*>(arguments_->cellWidget(row, 4));
        auto* useDefault = qobject_cast<QCheckBox*>(arguments_->cellWidget(row, 5));
        variable.insert(QStringLiteral("value"), value == nullptr ? QString() : value->text());
        variable.insert(QStringLiteral("null"),
            nullValue != nullptr && nullValue->isChecked());
        variable.insert(QStringLiteral("default"),
            useDefault != nullptr && useDefault->isChecked());
        variables.append(variable);
    }
    root.insert(QStringLiteral("variables"), variables);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0
            || !file.commit()) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    filePath_ = QFileInfo(path).absoluteFilePath();
    setModified(false);
    emit statusChanged();
    return true;
}

QString TestDocumentWidget::connectionProfileId() const {
    return connectingProfile_.id.isEmpty() ? profile_.id : connectingProfile_.id;
}

ConnectionProfile TestDocumentWidget::connectionProfile() const { return profile_; }

bool TestDocumentWidget::isConnected() const { return connected_; }

QString TestDocumentWidget::connectionDisplay() const {
    if (connected_) {
        QStringList details{profile_.name};
        if (!activeEndpoint_.host.isEmpty()) {
            details.append(QStringLiteral("%1:%2")
                .arg(activeEndpoint_.host).arg(activeEndpoint_.port));
        }
        if (!activeIdentity_.summary().isEmpty()) {
            details.append(activeIdentity_.summary());
        }
        return details.join(QStringLiteral("｜"));
    }
    if (!connectingProfile_.name.isEmpty()) {
        return QStringLiteral("%1（连接中）").arg(connectingProfile_.name);
    }
    return QStringLiteral("未连接");
}

QString TestDocumentWidget::connectionNameDisplay() const {
    if (connected_) return profile_.name;
    if (!connectingProfile_.name.isEmpty()) return connectingProfile_.name;
    return QStringLiteral("未连接");
}

QString TestDocumentWidget::connectionStateDisplay() const {
    if (connected_) return QStringLiteral("已连接");
    if (!connectingProfile_.name.isEmpty()) return QStringLiteral("连接中");
    return QStringLiteral("未连接");
}

QString TestDocumentWidget::activityDisplay() const {
    return activityState_ == nullptr ? QString() : activityState_->text();
}

QString TestDocumentWidget::transactionDisplay() const {
    return testTransactionStateText(transactionState_);
}

bool TestDocumentWidget::isBusy() const {
    return busy_ || pendingDebugRun_ || (dmdbg_ != nullptr
        && dmdbg_->state() != DmdbgProcessState::Stopped);
}

bool TestDocumentWidget::hasPendingTransaction() const {
    return testTransactionDecisionRequired(transactionState_);
}

bool TestDocumentWidget::hasOutcomeUnknown() const {
    return testOutcomeAcknowledgementRequired(transactionState_);
}

QStringList TestDocumentWidget::messageObjectAt(const QPoint& position) const {
    const QTextCursor cursor = messages_->cursorForPosition(position);
    const QRect characterRect = messages_->cursorRect(cursor);
    if (position.y() < characterRect.top() || position.y() > characterRect.bottom()) return {};
    const QString line = cursor.block().text();
    return SqlEditor::objectReferenceAt(line, cursor.positionInBlock());
}

void TestDocumentWidget::openMessageObject(const QStringList& parts) {
    if (parts.isEmpty() || !isConnected()) return;
    ObjectKey context = key_;
    context.connectionId = connectionProfileId();
    if (context.schema.isEmpty()) context.schema = profile_.user.trimmed().toUpper();
    emit objectDrillRequested(std::move(context), parts);
}

bool TestDocumentWidget::eventFilter(QObject* watched, QEvent* event) {
    if (messages_ != nullptr && watched == messages_->viewport()
            && event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton
                && (mouse->modifiers().testFlag(Qt::ControlModifier)
                    || mouse->modifiers().testFlag(Qt::MetaModifier))) {
            const QStringList parts = messageObjectAt(mouse->position().toPoint());
            if (!parts.isEmpty() && isConnected()) {
                openMessageObject(parts);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TestDocumentWidget::formatSql() {
    if (isBusy() || scriptEditor_ == nullptr || scriptEditor_->isReadOnly()) return;
    const QTextCursor cursor = scriptEditor_->textCursor();
    const int start = cursor.hasSelection() ? cursor.selectionStart() : 0;
    const int end = cursor.hasSelection() ? cursor.selectionEnd() : scriptEditor_->toPlainText().size();
    if (!LanguageServerClient::instance()->requestFormatting(scriptEditor_, start, end)) {
        messages_->appendPlainText(QStringLiteral("SQL 格式化失败：语言服务当前不可用，原文未改变。"));
    }
}

void TestDocumentWidget::commitTransaction() {
    if (session_ == nullptr || !connected_ || isBusy() || !hasPendingTransaction()) return;
    QMetaObject::invokeMethod(session_, &DpiSession::commit, Qt::QueuedConnection);
}

void TestDocumentWidget::rollbackTransaction() {
    if (session_ == nullptr || !connected_ || isBusy() || !hasPendingTransaction()) return;
    QMetaObject::invokeMethod(session_, &DpiSession::rollback, Qt::QueuedConnection);
}

bool TestDocumentWidget::hasActiveBreakpoints() const {
    return dmdbgConfigured_ && dmdbg_ != nullptr
        && dmdbg_->hasActiveBreakpoints();
}

QByteArray TestDocumentWidget::workspaceState() const {
    QJsonObject root;
    root.insert(QStringLiteral("version"), 3);
    root.insert(QStringLiteral("script"), scriptText());
    root.insert(QStringLiteral("connectionId"), key_.connectionId);
    root.insert(QStringLiteral("schema"), key_.schema);
    root.insert(QStringLiteral("objectName"), key_.name);
    root.insert(QStringLiteral("objectType"), static_cast<int>(key_.type));
    const int targetIndex = targetCombo_->currentIndex();
    if (targetIndex >= 0 && targetIndex < targets_.size()) {
        const ProgramTarget& target = targets_.at(targetIndex);
        root.insert(QStringLiteral("routine"), target.routineName);
        root.insert(QStringLiteral("signature"), target.signature());
        root.insert(QStringLiteral("targetIdentity"), target.identity());
    } else {
        root.insert(QStringLiteral("routine"), preferredRoutine_);
        root.insert(QStringLiteral("signature"), preferredSignature_);
        root.insert(QStringLiteral("targetIdentity"), argumentTargetIdentity_);
    }
    root.insert(QStringLiteral("breakpointLine"), breakpointLine_->value());
    root.insert(QStringLiteral("variableName"), variableName_->text());
    QJsonArray savedBreakpoints;
    for (int line : breakpoints_.keys()) savedBreakpoints.append(line);
    root.insert(QStringLiteral("breakpoints"), savedBreakpoints);
    QJsonArray savedWatches;
    for (const QString& expression : watches_.keys()) savedWatches.append(expression);
    root.insert(QStringLiteral("watches"), savedWatches);
    const QStringList resultPageIds{QStringLiteral("parameters"), QStringLiteral("messages"),
        QStringLiteral("output"), QStringLiteral("errors"), QStringLiteral("debug")};
    const QList<int> legacyResultPages{0, 4, 2, 3, 5};
    root.insert(QStringLiteral("resultTabId"), resultPageIds.value(resultTabs_->currentIndex()));
    root.insert(QStringLiteral("resultTab"), legacyResultPages.value(resultTabs_->currentIndex()));
    QJsonArray splitSizes;
    for (const int size : splitter_->sizes()) splitSizes.append(size);
    root.insert(QStringLiteral("splitSizes"), splitSizes);
    QJsonArray arguments;
    for (int row = 0; row < arguments_->rowCount(); ++row) {
        QJsonObject argument;
        argument.insert(QStringLiteral("name"),
            arguments_->item(row, 0) == nullptr ? QString()
                : arguments_->item(row, 0)->text());
        argument.insert(QStringLiteral("type"),
            arguments_->item(row, 1) == nullptr ? QStringLiteral("VARCHAR2")
                : arguments_->item(row, 1)->text());
        argument.insert(QStringLiteral("direction"),
            arguments_->item(row, 2) == nullptr ? QStringLiteral("IN")
                : arguments_->item(row, 2)->text());
        auto* value = qobject_cast<QLineEdit*>(arguments_->cellWidget(row, 3));
        auto* nullValue = qobject_cast<QCheckBox*>(arguments_->cellWidget(row, 4));
        auto* useDefault = qobject_cast<QCheckBox*>(arguments_->cellWidget(row, 5));
        argument.insert(QStringLiteral("value"), value == nullptr ? QString() : value->text());
        argument.insert(QStringLiteral("null"),
            nullValue != nullptr && nullValue->isChecked());
        argument.insert(QStringLiteral("default"),
            useDefault != nullptr && useDefault->isChecked());
        arguments.append(argument);
    }
    root.insert(QStringLiteral("arguments"), arguments);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

void TestDocumentWidget::restoreWorkspaceState(const QByteArray& state) {
    const QJsonDocument document = QJsonDocument::fromJson(state);
    if (!document.isObject()) {
        return;
    }
    const QJsonObject root = document.object();
    const int version = root.value(QStringLiteral("version")).toInt();
    if (version != 1 && version != 2 && version != 3) return;
    preferredRoutine_ = root.value(QStringLiteral("routine")).toString();
    preferredSignature_ = root.value(QStringLiteral("signature")).toString();
    breakpointLine_->setValue(std::clamp(
        root.value(QStringLiteral("breakpointLine")).toInt(1),
        breakpointLine_->minimum(), breakpointLine_->maximum()));
    variableName_->setText(root.value(QStringLiteral("variableName")).toString());
    breakpoints_.clear();
    const QJsonArray savedBreakpoints = root.value(QStringLiteral("breakpoints")).toArray();
    for (const QJsonValue& value : savedBreakpoints) {
        if (value.isDouble() && value.toInt() > 0) breakpoints_.insert(value.toInt(), 0);
    }
    if (breakpoints_.isEmpty() && version < 3) {
        breakpoints_.insert(breakpointLine_->value(), 0);
    }
    watches_.clear();
    for (const QJsonValue& value : root.value(QStringLiteral("watches")).toArray()) {
        const QString expression = value.toString().trimmed();
        if (!expression.isEmpty()) watches_.insert(expression, QStringLiteral("待求值"));
    }
    refreshBreakpointTable();
    refreshWatchTable();
    const QStringList resultPageIds{QStringLiteral("parameters"), QStringLiteral("messages"),
        QStringLiteral("output"), QStringLiteral("errors"), QStringLiteral("debug")};
    const QList<int> legacyToCurrent{0, 0, 2, 3, 1, 4};
    const int resultPage = root.contains(QStringLiteral("resultTabId"))
        ? resultPageIds.indexOf(root.value(QStringLiteral("resultTabId")).toString())
        : legacyToCurrent.value(root.value(QStringLiteral("resultTab")).toInt(), 0);
    resultTabs_->setCurrentIndex(qMax(0, resultPage));
    if (version >= 2) {
        updatingScript_ = true;
        scriptEditor_->setPlainText(root.value(QStringLiteral("script")).toString());
        updatingScript_ = false;
        const QJsonArray savedSizes = root.value(QStringLiteral("splitSizes")).toArray();
        if (savedSizes.size() == 2) {
            splitter_->setSizes(boundedResultSizes(
                std::max(120, savedSizes.at(0).toInt()),
                std::max(120, savedSizes.at(1).toInt())));
        }
    }
    pendingWorkspaceState_ = state;
    if (isBlankDocument() && version >= 2) {
        scanVariables();
    }
    applyPendingWorkspaceArguments();
    setModified(false);
}

void TestDocumentWidget::applyPendingWorkspaceArguments() {
    if (pendingWorkspaceState_.isEmpty()) return;
    const QJsonObject root =
        QJsonDocument::fromJson(pendingWorkspaceState_).object();
    const int targetIndex = targetCombo_->currentIndex();
    const ProgramTarget* target = targetIndex >= 0 && targetIndex < targets_.size()
        ? &targets_.at(targetIndex) : nullptr;
    const QString routine = root.value(QStringLiteral("routine")).toString();
    const QString signature = root.value(QStringLiteral("signature")).toString();
    const QString identity = root.value(QStringLiteral("targetIdentity")).toString();
    if (!identity.isEmpty() && (target == nullptr || target->identity() != identity)) return;
    if (target == nullptr && !isBlankDocument()
            && (!routine.isEmpty() || !signature.isEmpty())) {
        return;
    }
    if (identity.isEmpty() && target != nullptr && ((!routine.isEmpty()
            && target->routineName.compare(routine, Qt::CaseInsensitive) != 0)
            || (!signature.isEmpty()
                && target->signature().compare(signature, Qt::CaseInsensitive) != 0))) {
        return;
    }
    const QJsonArray values = root.value(QStringLiteral("arguments")).toArray();
    pendingWorkspaceState_.clear();
    const bool publishFinalState = !updatingArguments_;
    QScopedValueRollback<bool> updating(updatingArguments_, true);
    const QSignalBlocker tableSignals(arguments_);
    const bool updatesWereEnabled = arguments_->updatesEnabled();
    arguments_->setUpdatesEnabled(false);
    for (int savedRow = 0; savedRow < values.size(); ++savedRow) {
        const QJsonObject saved = values.at(savedRow).toObject();
        const QString savedName = saved.value(QStringLiteral("name")).toString();
        int row = savedRow < arguments_->rowCount() ? savedRow : -1;
        if (!savedName.isEmpty()) {
            row = -1;
            for (int candidate = 0; candidate < arguments_->rowCount(); ++candidate) {
                if (arguments_->item(candidate, 0) != nullptr
                        && arguments_->item(candidate, 0)->text()
                            .compare(savedName, Qt::CaseInsensitive) == 0) {
                    row = candidate;
                    break;
                }
            }
        }
        if (row < 0) {
            ProgramParameter parameter;
            parameter.name = savedName;
            parameter.dataType = saved.value(QStringLiteral("type"))
                .toString(QStringLiteral("VARCHAR2"));
            const QString direction = saved.value(QStringLiteral("direction"))
                .toString(QStringLiteral("IN")).toUpper();
            parameter.direction = direction == QStringLiteral("OUT")
                ? ProgramParameterDirection::Out
                : direction == QStringLiteral("IN OUT")
                    ? ProgramParameterDirection::InOut
                    : direction == QStringLiteral("RETURN")
                        ? ProgramParameterDirection::Return
                        : ProgramParameterDirection::In;
            appendVariable(savedName, parameter, true);
            row = arguments_->rowCount() - 1;
        }
        const QJsonObject argument = values.at(savedRow).toObject();
        if (arguments_->item(row, 1) != nullptr
                && arguments_->item(row, 1)->flags().testFlag(Qt::ItemIsEditable)
                && argument.contains(QStringLiteral("type"))) {
            arguments_->item(row, 1)->setText(
                argument.value(QStringLiteral("type")).toString());
        }
        if (arguments_->item(row, 2) != nullptr
                && arguments_->item(row, 2)->flags().testFlag(Qt::ItemIsEditable)
                && argument.contains(QStringLiteral("direction"))) {
            arguments_->item(row, 2)->setText(
                argument.value(QStringLiteral("direction")).toString());
        }
        auto* value = qobject_cast<QLineEdit*>(arguments_->cellWidget(row, 3));
        auto* nullValue = qobject_cast<QCheckBox*>(arguments_->cellWidget(row, 4));
        auto* useDefault = qobject_cast<QCheckBox*>(arguments_->cellWidget(row, 5));
        if (value != nullptr) {
            value->setText(argument.value(QStringLiteral("value")).toString());
        }
        if (nullValue != nullptr && nullValue->isEnabled()) {
            nullValue->setChecked(argument.value(QStringLiteral("null")).toBool());
        }
        if (useDefault != nullptr && useDefault->isEnabled()) {
            useDefault->setChecked(argument.value(QStringLiteral("default")).toBool());
        }
    }
    arguments_->setUpdatesEnabled(updatesWereEnabled);
    if (updatesWereEnabled) arguments_->viewport()->update();
    if (publishFinalState) updateActions();
}

void TestDocumentWidget::setLoading() {
    activityState_->setText(QStringLiteral("正在读取程序参数"));
    run_->setEnabled(false);
}

void TestDocumentWidget::setError(const QString& message) {
    if (dmdbg_ != nullptr && dmdbg_->state() == DmdbgProcessState::Stopped)
        pendingDebugRun_ = false;
    activityState_->setToolTip(message);
    activityState_->setText(compactActivityText(message));
    messages_->appendPlainText(message);
    {
        const QSignalBlocker tabsBlocker(resultTabs_);
        const QSignalBlocker splitterBlocker(splitter_);
        resultTabs_->setCurrentWidget(messagesPage_);
        const QList<int> sizes = splitter_->sizes();
        if (sizes.size() > 1 && sizes.at(1) <= 48) {
            splitter_->setSizes(boundedResultSizes(sizes.at(0), 180));
        }
    }
    updateActions();
}

void TestDocumentWidget::setTargets(QVector<ProgramTarget> targets) {
    if (isBusy()) return;
    const QString savedIdentity = QJsonDocument::fromJson(pendingWorkspaceState_).object()
        .value(QStringLiteral("targetIdentity")).toString();
    const QString retainedIdentity = savedIdentity.isEmpty() ? argumentTargetIdentity_ : savedIdentity;
    int rejectedTargets = 0;
    QString firstRejection;
    for (int index = targets.size() - 1; index >= 0; --index) {
        const QString reason = TestCallBuilder::targetValidationError(targets.at(index));
        if (reason.isEmpty()) continue;
        firstRejection = reason;
        targets.removeAt(index);
        ++rejectedTargets;
    }
    targets_ = std::move(targets);
    int preferredIndex = 0;
    {
        const QSignalBlocker targetSignals(targetCombo_);
        targetCombo_->clear();
        for (const ProgramTarget& target : targets_) {
            targetCombo_->addItem(target.displayName());
        }
        for (int index = 0; index < targets_.size(); ++index) {
            if (!preferredRoutine_.isEmpty()
                    && targets_.at(index).routineName.compare(
                        preferredRoutine_, Qt::CaseInsensitive) == 0
                    && (preferredSignature_.isEmpty()
                        || targets_.at(index).signature()
                            .compare(preferredSignature_, Qt::CaseInsensitive) == 0)) {
                preferredIndex = index;
                break;
            }
        }
        for (int index = 0; index < targets_.size(); ++index) {
            if (targets_.at(index).identity() == retainedIdentity) {
                preferredIndex = index;
                break;
            }
        }
        if (!targets_.isEmpty()) targetCombo_->setCurrentIndex(preferredIndex);
    }
    if (targets_.isEmpty()) {
        setError(QStringLiteral("没有找到可测试的公开程序或重载。"));
    } else {
        activityState_->setText(QStringLiteral("参数已就绪"));
        rebuildArguments();
    }
    if (targets_.isEmpty()) updateActions();
    if (rejectedTargets > 0) {
        setError(QStringLiteral("已排除 %1 个无效程序目标：%2 无效目标不会生成调用脚本。")
            .arg(rejectedTargets).arg(firstRejection));
    }
}

void TestDocumentWidget::setPreferredRoutine(QString routineName, QString signature) {
    preferredRoutine_ = std::move(routineName);
    preferredSignature_ = std::move(signature);
    for (int index = 0; index < targets_.size(); ++index) {
        if (targets_.at(index).routineName.compare(
                preferredRoutine_, Qt::CaseInsensitive) == 0
                && (preferredSignature_.isEmpty()
                    || targets_.at(index).signature()
                        .compare(preferredSignature_, Qt::CaseInsensitive) == 0)) {
            const QSignalBlocker targetSignals(targetCombo_);
            targetCombo_->setCurrentIndex(index);
            rebuildArguments();
            return;
        }
    }
}

void TestDocumentWidget::appendVariable(
        const QString& bindName, const ProgramParameter& parameter,
        bool definitionEditable) {
    const int row = arguments_->rowCount();
    arguments_->insertRow(row);
    auto* nameItem = new QTableWidgetItem(bindName);
    nameItem->setToolTip(parameter.name.isEmpty()
        ? QStringLiteral("函数返回值")
        : QStringLiteral("数据库参数：%1").arg(parameter.name));
    arguments_->setItem(row, 0, nameItem);
    auto* typeItem = new QTableWidgetItem(
        parameter.dataType.isEmpty() ? QStringLiteral("VARCHAR2") : parameter.dataType);
    auto* directionItem = new QTableWidgetItem(directionText(parameter.direction));
    if (!definitionEditable) {
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
        directionItem->setFlags(directionItem->flags() & ~Qt::ItemIsEditable);
    }
    arguments_->setItem(row, 1, typeItem);
    arguments_->setItem(row, 2, directionItem);
    auto* value = new QLineEdit(arguments_);
    value->setEnabled(acceptsInput(parameter.direction) && parameter.supported);
    arguments_->setCellWidget(row, 3, value);
    auto* nullValue = new QCheckBox(arguments_);
    nullValue->setEnabled(acceptsInput(parameter.direction) && parameter.supported);
    arguments_->setCellWidget(row, 4, nullValue);
    auto* useDefault = new QCheckBox(arguments_);
    useDefault->setEnabled(parameter.defaulted
        && parameter.direction == ProgramParameterDirection::In);
    arguments_->setCellWidget(row, 5, useDefault);
    arguments_->setItem(row, 6, new QTableWidgetItem(
        parameter.supported ? QStringLiteral("是") : QStringLiteral("暂不支持")));
    connect(value, &QLineEdit::textChanged, this, [this]() {
        if (updatingArguments_) return;
        assetModified_ = true;
        updateActions();
    });
    connect(nullValue, &QCheckBox::toggled, this, [this, value](bool enabled) {
        value->setEnabled(!enabled);
        if (updatingArguments_) return;
        assetModified_ = true;
        updateActions();
    });
    connect(useDefault, &QCheckBox::toggled, this,
        [this, value, nullValue](bool enabled) {
            value->setEnabled(!enabled && !nullValue->isChecked());
            nullValue->setEnabled(!enabled);
            if (updatingArguments_) return;
            assetModified_ = true;
            updateGeneratedScript();
            updateActions();
        });
}

void TestDocumentWidget::scanVariables() {
    if (isBusy()) return;
    const bool publishFinalState = !updatingArguments_;
    QScopedValueRollback<bool> updating(updatingArguments_, true);
    const QSignalBlocker tableSignals(arguments_);
    const bool updatesWereEnabled = arguments_->updatesEnabled();
    arguments_->setUpdatesEnabled(false);
    const SqlTools::NamedBindPlan plan = SqlTools::namedBindPlan(scriptText());
    QSet<QString> active;
    for (const QString& name : plan.names) {
        const QString identity = name.toUpper();
        active.insert(identity);
        bool found = false;
        for (int row = 0; row < arguments_->rowCount(); ++row) {
            if (arguments_->item(row, 0) != nullptr
                    && arguments_->item(row, 0)->text()
                        .compare(name, Qt::CaseInsensitive) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            ProgramParameter parameter;
            parameter.name = name;
            parameter.dataType = QStringLiteral("VARCHAR2");
            parameter.direction = ProgramParameterDirection::In;
            parameter.supported = true;
            appendVariable(name, parameter, true);
            arguments_->item(arguments_->rowCount() - 1, 6)
                ->setText(QStringLiteral("需确认类型"));
        }
    }
    for (int row = 0; row < arguments_->rowCount(); ++row) {
        const QString name = arguments_->item(row, 0) == nullptr
            ? QString() : arguments_->item(row, 0)->text();
        auto* status = arguments_->item(row, 6);
        if (status == nullptr) continue;
        if (!active.contains(name.toUpper())) {
            status->setText(QStringLiteral("脚本未使用"));
        } else if (status->text() == QStringLiteral("脚本未使用")) {
            status->setText(QStringLiteral("是"));
        }
    }
    arguments_->setUpdatesEnabled(updatesWereEnabled);
    if (updatesWereEnabled) arguments_->viewport()->update();
    if (publishFinalState) updateActions();
}

void TestDocumentWidget::updateGeneratedScript() {
    const int targetIndex = targetCombo_->currentIndex();
    if (targetIndex < 0 || targetIndex >= targets_.size()) return;
    QString generated;
    try {
        generated = TestCallBuilder::generatedScript(targets_.at(targetIndex));
    } catch (const std::exception&) {
        // Generation is a UI callback: a bad target must not terminate the process
        // or replace a user's script / previous generated baseline.
        setError(QStringLiteral("无法生成调用脚本：程序目标或参数元数据无效。现有脚本已保留，请刷新程序目标。"));
        return;
    }
    for (int row = arguments_->rowCount() - 1; row >= 0; --row) {
        auto* useDefault = qobject_cast<QCheckBox*>(arguments_->cellWidget(row, 5));
        if (useDefault == nullptr || !useDefault->isChecked()
                || arguments_->item(row, 0) == nullptr) continue;
        const QString bind = QRegularExpression::escape(arguments_->item(row, 0)->text());
        generated.remove(QRegularExpression(
            QStringLiteral(R"(^\s*"[^"]+"\s*=>\s*:%1\s*,?\s*$\n?)").arg(bind),
            QRegularExpression::MultilineOption
                | QRegularExpression::CaseInsensitiveOption));
        generated.replace(QStringLiteral(",\n   )"), QStringLiteral("\n   )"));
        generated.replace(QStringLiteral("(\n   )"), QStringLiteral("()"));
    }
    const QString previousGenerated = lastGeneratedScript_;
    lastGeneratedScript_ = generated;
    const QString current = scriptText().trimmed();
    if (!current.isEmpty()
            && current != previousGenerated.trimmed()
            && current != generated.trimmed()) {
        return;
    }
    const bool preserveModified = isModified();
    updatingScript_ = true;
    scriptEditor_->setPlainText(generated);
    scriptEditor_->document()->setModified(preserveModified);
    updatingScript_ = false;
}

void TestDocumentWidget::setConnectionPending(
    ConnectionProfile profile, const QString& detail) {
    connectingProfile_ = std::move(profile);
    connectionState_->setText(detail);
    updateActions();
}

void TestDocumentWidget::connectDatabase(ConnectionProfile profile, QByteArray password) {
    connectingProfile_ = profile;
    connectionState_->setText(QStringLiteral("正在连接：%1").arg(profile.name));
    QMetaObject::invokeMethod(session_,
        [session = session_, profile = std::move(profile),
            password = std::move(password)]() mutable {
            session->connectToDatabase(std::move(profile), std::move(password));
        }, Qt::QueuedConnection);
}

void TestDocumentWidget::disconnectDatabase() {
    if (session_ == nullptr || busy_ || hasPendingTransaction()) return;
    QMetaObject::invokeMethod(session_, &DpiSession::disconnectFromDatabase,
        Qt::QueuedConnection);
}

void TestDocumentWidget::startDmdbg(QByteArray password) {
    if (busy_ || !connected_ || profile_.id.isEmpty() || dmdbgPath_.isEmpty()) {
        password.fill('\0');
        setError(QStringLiteral("调试器启动条件不完整：需要当前窗口已连接且内置调试器文件完整。"));
        return;
    }
    scanVariables();
    dmdbgConfigured_ = false;
    dmdbgSetupStage_ = DmdbgSetupStage::None;
    pendingDmdbgSql_.clear();
    dmdbgPaused_ = false;
    stackTable_->setRowCount(0);
    pendingWatchExpressions_.clear();
    activeWatchExpression_.clear();
    for (auto iterator = watches_.begin(); iterator != watches_.end(); ++iterator) {
        iterator.value() = QStringLiteral("待求值");
    }
    refreshWatchTable();
    const ConnectionEndpoint endpoint =
        activeEndpoint_.host.isEmpty() && profile_.connectionType == ConnectionType::Direct
            ? profile_.effectiveEndpoints().value(0) : activeEndpoint_;
    if (endpoint.host.isEmpty()) {
        password.fill('\0');
        setError(QStringLiteral("无法确定当前物理连接端点，已拒绝启动 dmdbg。"));
        return;
    }
    QString endpointHost = endpoint.host;
    if (endpointHost.contains(QLatin1Char(':'))
            && !endpointHost.startsWith(QLatin1Char('['))) {
        endpointHost = QStringLiteral("[%1]").arg(endpointHost);
    }
    dmdbg_->start(dmdbgPath_,
        QStringLiteral("%1:%2").arg(endpointHost).arg(endpoint.port),
        profile_.user, std::move(password));
    updateDebugActions();
}

void TestDocumentWidget::rebuildArguments() {
    if (isBusy()) return;
    if (updatingArguments_) return;
    QScopedValueRollback<bool> updating(updatingArguments_, true);
    const QSignalBlocker tableSignals(arguments_);
    arguments_->setUpdatesEnabled(false);
    dmdbgConfigured_ = false;
    dmdbgPaused_ = false;
    const int targetIndex = targetCombo_->currentIndex();
    if (targetIndex < 0 || targetIndex >= targets_.size()) {
        arguments_->setUpdatesEnabled(true);
        arguments_->viewport()->update();
        updateActions();
        return;
    }
    const ProgramTarget& target = targets_.at(targetIndex);
    if (pendingWorkspaceState_.isEmpty() && target.identity() == argumentTargetIdentity_) {
        pendingWorkspaceState_ = workspaceState();
    }
    argumentTargetIdentity_ = target.identity();
    arguments_->setRowCount(0);
    const QStringList bindNames = TestCallBuilder::bindNames(target);
    for (int row = 0; row < target.parameters.size(); ++row) {
        appendVariable(bindNames.at(row), target.parameters.at(row), false);
    }
    scanVariables();
    applyPendingWorkspaceArguments();
    updateGeneratedScript();
    scanVariables();
    arguments_->setUpdatesEnabled(true);
    arguments_->viewport()->update();
    updateActions();
}

TestInvocation TestDocumentWidget::invocationFromEditor() const {
    const int targetIndex = targetCombo_->currentIndex();
    TestInvocation invocation;
    if (targetIndex >= 0 && targetIndex < targets_.size()) {
        invocation.target = targets_.at(targetIndex);
    } else {
        invocation.target.connectionId = connectionProfileId();
        invocation.target.schema = key_.schema.trimmed().isEmpty()
            ? profile_.user.trimmed().toUpper() : key_.schema;
        invocation.target.routineName = QStringLiteral("手工 Test Script");
    }
    invocation.script = CommandScript::prepareTestScript(scriptText());
    QSet<QString> activeVariables;
    for (const QString& name : SqlTools::namedBindPlan(invocation.script).names) {
        activeVariables.insert(name.toUpper());
    }
    for (int row = 0; row < arguments_->rowCount(); ++row) {
        const QString bindName = arguments_->item(row, 0) == nullptr
            ? QString() : arguments_->item(row, 0)->text().trimmed();
        if (!activeVariables.contains(bindName.toUpper())) continue;
        ProgramParameter parameter = row < invocation.target.parameters.size()
            ? invocation.target.parameters.at(row) : ProgramParameter{};
        if (parameter.name.isEmpty()) parameter.name = bindName;
        parameter.dataType = arguments_->item(row, 1) == nullptr
            ? QStringLiteral("VARCHAR2") : arguments_->item(row, 1)->text().trimmed();
        const QString direction = arguments_->item(row, 2) == nullptr
            ? QStringLiteral("IN") : arguments_->item(row, 2)->text().trimmed().toUpper();
        if (direction == QStringLiteral("IN")) {
            parameter.direction = ProgramParameterDirection::In;
        } else if (direction == QStringLiteral("OUT")) {
            parameter.direction = ProgramParameterDirection::Out;
        } else if (direction == QStringLiteral("IN OUT")
                || direction == QStringLiteral("IN_OUT")) {
            parameter.direction = ProgramParameterDirection::InOut;
        } else if (direction == QStringLiteral("RETURN")) {
            parameter.direction = ProgramParameterDirection::Return;
        } else {
            throw std::invalid_argument(
                "参数方向必须是 IN、OUT、IN OUT 或 RETURN。");
        }
        parameter.supported = isSupportedProgramScalarType(parameter.dataType);
        auto* value = qobject_cast<QLineEdit*>(arguments_->cellWidget(row, 3));
        auto* nullValue = qobject_cast<QCheckBox*>(arguments_->cellWidget(row, 4));
        auto* useDefault = qobject_cast<QCheckBox*>(arguments_->cellWidget(row, 5));
        const bool usesDefault = useDefault != nullptr && useDefault->isChecked();
        if (!parameter.supported && !usesDefault) {
            throw std::invalid_argument(
                QStringLiteral("参数“%1”的类型 %2 暂不支持。")
                    .arg(parameter.name, parameter.dataType).toUtf8().constData());
        }
        invocation.arguments.append({parameter, value == nullptr ? QString() : value->text(),
            nullValue != nullptr && nullValue->isChecked(),
            usesDefault, bindName});
    }
    return invocation;
}

void TestDocumentWidget::requestObjectAtCursor() {
    const auto parts = SqlEditor::objectReferenceAt(scriptText(), scriptEditor_->textCursor().position());
    if (!parts.isEmpty()) emit scriptEditor_->objectNavigationRequested(parts);
}

void TestDocumentWidget::executeTest() {
    if (!connected_ || isBusy()) return;
    TestInvocation invocation;
    try {
        invocation = invocationFromEditor();
    } catch (const std::exception& error) {
        setError(QString::fromUtf8(error.what()));
        return;
    }
    if (!confirmExecution(invocation.target)) return;
    busy_ = true;
    executionSoundPending_ = true;
    executionSoundCancelled_ = false;
    variableScanTimer_->stop();
    updateActions();
    messages_->appendPlainText(QStringLiteral("\n──────── 执行开始 · %1 ────────")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    outputs_->setRowCount(0);
    parameterOutputs_->hide();
    errors_->setRowCount(0);
    lastHandledTestError_.clear();
    scriptOutput_->clear();
    messages_->appendPlainText(
        QStringLiteral("开始执行：%1").arg(invocation.target.displayName()));
    QMetaObject::invokeMethod(session_,
        [session = session_, invocation = std::move(invocation)]() mutable {
            session->executeTest(std::move(invocation));
        }, Qt::QueuedConnection);
}

void TestDocumentWidget::applyResult(const TestExecutionResult& result) {
    const bool notifyCompletion = executionSoundPending_ && !executionSoundCancelled_;
    executionSoundPending_ = false;
    setTransactionState(result.transactionState);
    if (result.transactionState == TestTransactionState::PendingDml) {
        activityState_->setText(QStringLiteral("存在未提交 DML：请选择提交或回滚"));
    } else if (result.transactionState == TestTransactionState::Unknown) {
        activityState_->setText(QStringLiteral("事务状态未知：请选择提交或回滚"));
    } else if (result.transactionState == TestTransactionState::OutcomeUnknown) {
        activityState_->setText(QStringLiteral(
            "连接中断，执行结果未知：请人工核对数据库状态后重新连接"));
    }
    outputs_->setRowCount(result.outputs.size());
    parameterOutputs_->setVisible(!result.outputs.isEmpty());
    for (int row = 0; row < result.outputs.size(); ++row) {
        const TestOutputValue& output = result.outputs.at(row);
        const QStringList cells{output.name, directionText(output.direction),
            output.originalValue,
            output.originalNull ? QStringLiteral("是") : QStringLiteral("否"),
            output.value, output.isNull ? QStringLiteral("是") : QStringLiteral("否")};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells.at(column));
            item->setToolTip(cells.at(column));
            outputs_->setItem(row, column, item);
        }
    }
    errors_->setRowCount(result.errorFrames.size());
    for (int row = 0; row < result.errorFrames.size(); ++row) {
        const RuntimeErrorFrame& frame = result.errorFrames.at(row);
        const QStringList cells{frame.schema, frame.objectName, frame.objectType,
            QString::number(frame.line), QString::number(frame.column), frame.message};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells.at(column));
            if (!frame.objectName.isEmpty() && frame.line > 0) {
                item->setData(Qt::UserRole, QVariant::fromValue(frame));
            }
            item->setToolTip(cells.at(column));
            errors_->setItem(row, column, item);
        }
    }
    if (!result.scriptOutput.isEmpty()) {
        scriptOutput_->setPlainText(boundedOutput(result.scriptOutput));
    }
    const QString outputSummary = result.outputs.isEmpty()
        ? QStringLiteral("无") : QStringLiteral("%1 个").arg(result.outputs.size());
    const QString scriptOutputSummary = result.scriptOutput.isEmpty()
        ? QStringLiteral("无")
        : QStringLiteral("%1 个字符").arg(result.scriptOutput.size());
    const QString conciseError = RuntimeErrorParser::messageWithoutFrames(
        result.errorMessage, result.errorFrames);
    if (!result.succeeded) lastHandledTestError_ = result.errorMessage.trimmed();
    messages_->appendPlainText(result.succeeded
        ? QStringLiteral(
            "执行成功，耗时 %1 ms。事务：%2。输出参数：%3；DBMS_OUTPUT：%4。")
            .arg(result.elapsedMs)
            .arg(testTransactionStateText(result.transactionState),
                outputSummary, scriptOutputSummary)
        : QStringLiteral("执行失败（%1），耗时 %2 ms：%3")
            .arg(result.errorCode).arg(result.elapsedMs).arg(conciseError));
    QWidget* resultPage = messagesPage_;
    if (!result.succeeded && !result.errorFrames.isEmpty()) {
        resultPage = errors_;
    } else if (result.succeeded && !result.outputs.isEmpty()) {
        resultPage = variablesPage_;
    } else if (result.succeeded && !result.scriptOutput.isEmpty()) {
        resultPage = dbmsOutputPane_;
    }
    {
        const QSignalBlocker tabsBlocker(resultTabs_);
        const QSignalBlocker splitterBlocker(splitter_);
        resultTabs_->setCurrentWidget(resultPage);
        const QList<int> sizes = splitter_->sizes();
        if (sizes.size() > 1 && sizes.at(1) <= 48) {
            splitter_->setSizes(boundedResultSizes(sizes.at(0), 180));
        }
    }
    if (notifyCompletion) {
        CompletionSounds::Player::instance().notify(CompletionSounds::Event::Test, result.elapsedMs);
    }
    updateActions();
}

void TestDocumentWidget::activateErrorFrame(int row) {
    if (row < 0 || row >= errors_->rowCount()) return;
    const QVariant value = errors_->item(row, 0) == nullptr
        ? QVariant() : errors_->item(row, 0)->data(Qt::UserRole);
    if (value.isValid()) emit errorFrameActivated(value.value<RuntimeErrorFrame>());
}

void TestDocumentWidget::setTransactionState(TestTransactionState state) {
    transactionState_ = state;
    transactionStateLabel_->setText(
        QStringLiteral("事务：%1").arg(testTransactionStateText(state)));
}

bool TestDocumentWidget::confirmExecution(const ProgramTarget& target) const {
    if (profile_.readOnly) {
        QMessageBox::warning(const_cast<TestDocumentWidget*>(this),
            QStringLiteral("只读连接禁止程序测试"),
            QStringLiteral("连接“%1”启用了只读模式，不能调用可能包含 DML 或 DDL 的程序。")
                .arg(profile_.name));
        return false;
    }
    if (profile_.environment != ConnectionEnvironment::Production) return true;
    const QString targetName = target.displayName();
    return MessagePrompt::show(QMessageBox::Warning, const_cast<TestDocumentWidget*>(this),
        QStringLiteral("确认生产环境程序测试"),
        QStringLiteral("连接：%1\n环境：生产\n用户：%2\n目标：%3\n\n"
                       "程序内部可能执行 DML、DDL 或提交事务。是否继续？")
            .arg(profile_.name, profile_.user, targetName),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
            QStringLiteral("执行程序测试"), QStringLiteral("取消测试")) == QMessageBox::Yes;
}

QString TestDocumentWidget::findDmdbg(const QString& applicationDirectory) {
    const QFileInfo bundled(QDir(applicationDirectory).filePath(QStringLiteral("debugger/dmdbg.exe")));
    return bundled.isFile() && bundled.isExecutable() ? bundled.absoluteFilePath() : QString();
}

void TestDocumentWidget::beginDebugging() {
    if (busy_ || pendingDebugRun_ || debugRunConfirmationPending_ || !connected_
            || dmdbgPaused_ || (!dmdbg_->isReady() && dmdbg_->state() != DmdbgProcessState::Stopped)) return;
    debugRunConfirmationPending_ = true;
    updateActions();
    bool confirmed = false;
    try {
        const auto invocation = invocationFromEditor();
        // Validate the current script/bindings before requesting any credentials.
        if (generatedScriptMatchesEditor()) (void)DmdbgCallBuilder::build(invocation);
        else (void)DmdbgCallBuilder::buildScript(invocation);
        confirmed = confirmExecution(invocation.target);
    } catch (const std::exception& error) {
        setError(QString::fromUtf8(error.what()));
    }
    debugRunConfirmationPending_ = false;
    if (!confirmed) { updateActions(); return; }
    pendingDebugRun_ = true;
    pendingEntryBreakpoint_ = breakpoints_.isEmpty();
    entryBreakpointNumber_ = 0;
    if (auto* page = findChild<QWidget*>(QStringLiteral("testDebugPage")))
        resultTabs_->setCurrentWidget(page);
    const auto sizes = splitter_->sizes();
    if (sizes.size() > 1 && sizes.at(1) <= 48) splitter_->setSizes(boundedResultSizes(sizes.at(0), 180));
    updateActions();
    if (dmdbg_->isReady()) {
        if (dmdbgConfigured_) synchronizePendingBreakpoints();
        else configureDmdbgTarget();
    } else if (dmdbgPath_.isEmpty() || !QFileInfo(dmdbgPath_).isExecutable()) {
        setError(QStringLiteral("内置调试器文件缺失，请修复或重新安装完整安装包。"));
    } else {
        emit dmdbgCredentialRequested();
    }
}

void TestDocumentWidget::runPendingDebug() {
    if (!pendingDebugRun_ || !dmdbg_->isReady() || !dmdbgConfigured_
            || pendingEntryBreakpoint_ || !pendingBreakpointLines_.isEmpty()
            || !pendingBreakpointDeletes_.isEmpty()) return;
    if (!dmdbgCapabilities_.start || !dmdbgCapabilities_.breakpoints
            || std::any_of(breakpoints_.cbegin(), breakpoints_.cend(), [](int number) { return number <= 0; })) {
        pendingDebugRun_ = false;
        setError(QStringLiteral("调试器能力不足或断点未确认，未执行用户程序。请查看消息及断点状态。"));
        return;
    }
    pendingDebugRun_ = false;
    scriptOutput_->clear();
    messages_->appendPlainText(QStringLiteral("\n──────── 调试开始 · %1 ────────")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    dmdbg_->run();
}

void TestDocumentWidget::configureDmdbgTarget() {
    if (dmdbg_ == nullptr || !dmdbg_->isReady()) return;
    try {
        const auto invocation = invocationFromEditor();
        const QString sql = generatedScriptMatchesEditor()
            ? DmdbgCallBuilder::build(invocation)
            : DmdbgCallBuilder::buildScript(invocation);
        dmdbgConfigured_ = false;
        dmdbgPaused_ = false;
        pendingBreakpointLines_.clear();
        pendingBreakpointDeletes_.clear();
        deletedBreakpointLines_.clear();
        failedBreakpointDeletes_.clear();
        activeBreakpointDelete_ = 0;
        for (auto iterator = breakpoints_.begin(); iterator != breakpoints_.end(); ++iterator)
            iterator.value() = 0;
        refreshBreakpointTable();
        pendingDmdbgSql_ = sql;
        dmdbgSetupStage_ = DmdbgSetupStage::OutputSql;
        dmdbg_->setSql(DpiSession::scriptOutputControlSql(true));
    } catch (const std::exception& error) {
        pendingDebugRun_ = false;
        dmdbgSetupStage_ = DmdbgSetupStage::None;
        pendingDmdbgSql_.clear();
        dmdbgConfigured_ = false;
        messages_->appendPlainText(QStringLiteral("无法设置 dmdbg 调试目标：%1")
            .arg(QString::fromUtf8(error.what())));
        showDebugMessages();
        updateDebugActions();
    }
}

void TestDocumentWidget::addBreakpoint(int line) {
    if (line <= 0 || breakpoints_.contains(line)) return;
    breakpoints_.insert(line, 0);
    assetModified_ = true;
    refreshBreakpointTable();
    synchronizePendingBreakpoints();
}

void TestDocumentWidget::removeSelectedBreakpoint() {
    const int row = breakpointTable_->currentRow();
    if (row < 0 || breakpointTable_->item(row, 4) == nullptr) return;
    const int line = breakpointTable_->item(row, 4)->text().toInt();
    const int number = breakpoints_.take(line);
    if (number > 0) {
        deletedBreakpointLines_.insert(number, line);
        pendingBreakpointDeletes_.append(number);
    }
    failedBreakpointDeletes_.remove(line);
    assetModified_ = true;
    refreshBreakpointTable();
    synchronizePendingBreakpoints();
}

void TestDocumentWidget::refreshBreakpointTable() {
    breakpointTable_->setUpdatesEnabled(false);
    breakpointTable_->setRowCount(breakpoints_.size());
    int row = 0;
    for (auto iterator = breakpoints_.cbegin(); iterator != breakpoints_.cend();
            ++iterator, ++row) {
        const int targetIndex = targetCombo_->currentIndex();
        const ProgramTarget* target = generatedScriptMatchesEditor()
                && targetIndex >= 0 && targetIndex < targets_.size()
            ? &targets_.at(targetIndex) : nullptr;
        const QString program = target == nullptr ? QStringLiteral("@dbg_main")
            : (target->packageName.isEmpty() ? target->routineName
                                             : target->packageName + QLatin1Char('.')
                                                   + target->routineName);
        const QStringList cells{
            failedBreakpointDeletes_.contains(iterator.key()) ? QStringLiteral("删除失败，仍生效")
                : iterator.value() > 0 ? QStringLiteral("已同步")
                : iterator.value() < 0 ? QStringLiteral("同步失败，请移除后重试")
                                       : QStringLiteral("待同步"),
            iterator.value() > 0 ? QString::number(iterator.value()) : QStringLiteral("—"),
            target == nullptr ? QString() : target->schema,
            program,
            QString::number(iterator.key())};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells.at(column));
            item->setToolTip(cells.at(column));
            breakpointTable_->setItem(row, column, item);
        }
    }
    breakpointTable_->setUpdatesEnabled(true);
}

void TestDocumentWidget::synchronizePendingBreakpoints() {
    if (!dmdbg_->isReady() || !dmdbgConfigured_) return;
    if (!pendingBreakpointDeletes_.isEmpty()) {
        activeBreakpointDelete_ = pendingBreakpointDeletes_.takeFirst();
        dmdbg_->deleteBreakpoint(activeBreakpointDelete_);
        return;
    }
    if (pendingDebugRun_ && pendingEntryBreakpoint_) {
        if (!dmdbgCapabilities_.breakpoints) {
            pendingDebugRun_ = pendingEntryBreakpoint_ = false;
            setError(QStringLiteral("当前调试器不支持入口断点，未执行用户程序。"));
            return;
        }
        const int index = targetCombo_->currentIndex();
        QString method;
        if (generatedScriptMatchesEditor() && index >= 0 && index < targets_.size()) {
            const auto& target = targets_.at(index);
            QStringList parts{target.schema};
            if (!target.packageName.isEmpty()) parts.append(target.packageName);
            parts.append(target.routineName);
            method = parts.join(QLatin1Char('.'));
        }
        // The server moves an anonymous line-1 breakpoint to the first executable line.
        deferredEntryBreakpoint_ = false;
        dmdbg_->setBreakpoint(method, method.isEmpty() ? 1 : 0);
        return;
    }
    for (auto iterator = breakpoints_.cbegin(); iterator != breakpoints_.cend(); ++iterator) {
        if (iterator.value() != 0 || pendingBreakpointLines_.contains(iterator.key())) continue;
        const int targetIndex = targetCombo_->currentIndex();
        if (!generatedScriptMatchesEditor()
                || targetIndex < 0 || targetIndex >= targets_.size()) {
            pendingBreakpointLines_.append(iterator.key());
            dmdbg_->setBreakpoint({}, iterator.key());
            return;
        }
        const ProgramTarget& target = targets_.at(targetIndex);
        if (!target.packageName.isEmpty()) {
            breakpoints_[iterator.key()] = -1;
            pendingDebugRun_ = false;
            setError(QStringLiteral("dmdbg 不支持包方法内部行号断点。请移除该断点后从方法入口开始，再单步进入。"));
            refreshBreakpointTable();
            return;
        }
        QStringList parts{target.schema};
        if (!target.packageName.isEmpty()) parts.append(target.packageName);
        parts.append(target.routineName);
        pendingBreakpointLines_.append(iterator.key());
        dmdbg_->setBreakpoint(parts.join(QLatin1Char('.')), iterator.key());
        return;
    }
    runPendingDebug();
}

void TestDocumentWidget::addWatchExpression() {
    const QString expression = variableName_->text().trimmed();
    if (expression.isEmpty() || watches_.contains(expression)) return;
    try {
        (void)DmdbgProtocol::variableCommand(expression);
    } catch (const std::invalid_argument& error) {
        messages_->appendPlainText(QString::fromUtf8(error.what()));
        return;
    }
    watches_.insert(expression, QStringLiteral("待求值"));
    variableName_->clear();
    assetModified_ = true;
    refreshWatchTable();
    refreshDebugViews();
}

void TestDocumentWidget::removeSelectedWatch() {
    const int row = watchTable_->currentRow();
    if (row < 0 || watchTable_->item(row, 0) == nullptr) return;
    const QString expression = watchTable_->item(row, 0)->text();
    watches_.remove(expression);
    pendingWatchExpressions_.removeAll(expression);
    if (activeWatchExpression_ == expression) activeWatchExpression_.clear();
    assetModified_ = true;
    refreshWatchTable();
}

void TestDocumentWidget::refreshWatchTable() {
    watchTable_->setUpdatesEnabled(false);
    watchRows_.clear();
    watchTable_->setRowCount(watches_.size());
    int row = 0;
    for (auto iterator = watches_.cbegin(); iterator != watches_.cend(); ++iterator, ++row) {
        watchRows_.insert(iterator.key(), row);
        auto* expression = new QTableWidgetItem(iterator.key());
        expression->setToolTip(iterator.key());
        watchTable_->setItem(row, 0, expression);
        watchTable_->setItem(row, 1, new QTableWidgetItem(iterator.value()));
    }
    watchTable_->setUpdatesEnabled(true);
}

void TestDocumentWidget::updateWatchValue(const QString& expression, const QString& value) {
    auto watch = watches_.find(expression);
    const auto row = watchRows_.constFind(expression);
    if (watch == watches_.end() || row == watchRows_.cend()) return;
    watch.value() = value;
    if (auto* item = watchTable_->item(row.value(), 1)) item->setText(value);
}

void TestDocumentWidget::handleDmdbgCommandFailure(
    const QString& command, const QString& detail) {
    pendingDebugRun_ = false;
    pendingEntryBreakpoint_ = false;
    entryBreakpointNumber_ = 0;
    messages_->appendPlainText(QStringLiteral("dmdbg %1：%2").arg(command, detail));
    if (dmdbgSetupStage_ != DmdbgSetupStage::None) {
        dmdbgSetupStage_ = DmdbgSetupStage::None;
        pendingDmdbgSql_.clear();
        dmdbgConfigured_ = false;
        messages_->appendPlainText(QStringLiteral("调试初始化失败，未执行用户程序；请重新设置调试目标。"));
        showDebugMessages();
        updateActions();
        return;
    }
    if (command == QStringLiteral("P")) {
        updateWatchValue(activeWatchExpression_, detail);
        activeWatchExpression_.clear();
        QTimer::singleShot(0, this, &TestDocumentWidget::requestNextWatch);
    } else if (command == QStringLiteral("BT")) {
        QTimer::singleShot(0, this, &TestDocumentWidget::requestNextWatch);
    } else if (command == QStringLiteral("B") || command == QStringLiteral("INFO B")) {
        for (int line : pendingBreakpointLines_) {
            if (breakpoints_.contains(line)) breakpoints_[line] = -1;
        }
        pendingBreakpointLines_.clear();
        refreshBreakpointTable();
        QTimer::singleShot(0, this, &TestDocumentWidget::synchronizePendingBreakpoints);
    } else if (command == QStringLiteral("D")) {
        const int line = deletedBreakpointLines_.take(activeBreakpointDelete_);
        if (line > 0) {
            breakpoints_.insert(line, activeBreakpointDelete_);
            failedBreakpointDeletes_.insert(line);
        }
        activeBreakpointDelete_ = 0;
        refreshBreakpointTable();
        QTimer::singleShot(0, this, &TestDocumentWidget::synchronizePendingBreakpoints);
    } else if (command == QStringLiteral("SQL")) {
        dmdbgConfigured_ = false;
    }
    if (command == QStringLiteral("RUN") || command == QStringLiteral("CONTINUE")
            || command == QStringLiteral("NEXT") || command == QStringLiteral("S")
            || command == QStringLiteral("FINISH")) showDebugMessages();
    updateActions();
}

void TestDocumentWidget::showDebugMessages() {
    const QSignalBlocker tabsBlocker(resultTabs_);
    const QSignalBlocker splitterBlocker(splitter_);
    resultTabs_->setCurrentWidget(messagesPage_);
    const auto sizes = splitter_->sizes();
    if (sizes.size() > 1 && sizes.at(1) <= 48)
        splitter_->setSizes(boundedResultSizes(sizes.at(0), 180));
}

void TestDocumentWidget::showDebugLocation(const DmdbgFrameInfo& location) {
    if (location.line <= 0) return;
    if (location.method.isEmpty()
            || location.method.compare(QStringLiteral("@dbg_main"), Qt::CaseInsensitive) == 0) {
        scriptEditor_->setDebugLine(location.line);
        return;
    }
    scriptEditor_->setDebugLine(0);
    if (location.schema.isEmpty()) return;
    RuntimeErrorFrame frame;
    frame.schema = location.schema;
    frame.objectName = location.method.section(QLatin1Char('.'), 0, 0);
    frame.line = location.line;
    frame.message = QStringLiteral("dmdbg 当前暂停位置");
    if (location.method.contains(QLatin1Char('.'))) {
        frame.objectType = QStringLiteral("PACKAGE BODY");
    } else {
        // Reuse known metadata only for the actual reported object. Unknown
        // nested routines keep an empty type for MainWindow's safe navigation.
        for (const ProgramTarget& target : targets_) {
            if (target.packageName.isEmpty()
                    && target.schema.compare(location.schema, Qt::CaseInsensitive) == 0
                    && target.routineName.compare(location.method, Qt::CaseInsensitive) == 0) {
                frame.objectType = target.function ? QStringLiteral("FUNCTION")
                                                  : QStringLiteral("PROCEDURE");
                break;
            }
        }
    }
    emit errorFrameActivated(frame);
}

void TestDocumentWidget::refreshDebugViews() {
    if (!dmdbg_->isReady() || !dmdbgPaused_) return;
    pendingWatchExpressions_ = watches_.keys();
    activeWatchExpression_.clear();
    if (dmdbgCapabilities_.stack) {
        dmdbg_->requestStack();
    } else {
        requestNextWatch();
    }
}

void TestDocumentWidget::requestNextWatch() {
    if (!dmdbg_->isReady() || !dmdbgPaused_ || !dmdbgCapabilities_.variables
            || pendingWatchExpressions_.isEmpty()) {
        activeWatchExpression_.clear();
        return;
    }
    while (!pendingWatchExpressions_.isEmpty()) {
        const QString expression = pendingWatchExpressions_.takeFirst();
        if (!watches_.contains(expression)) continue;
        try {
            (void)DmdbgProtocol::variableCommand(expression);
        } catch (const std::invalid_argument& error) {
            updateWatchValue(expression, QString::fromUtf8(error.what()));
            continue;
        }
        activeWatchExpression_ = expression;
        dmdbg_->requestVariable(expression);
        return;
    }
    activeWatchExpression_.clear();
}

void TestDocumentWidget::appendDmdbgResponse(
    const QString& command, const QString& response) {
    QString visible = response;
    visible.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    visible.remove(QRegularExpression(
        QStringLiteral(R"((?:^|\n)[ \t]*DBG>[ \t]*$)")));
    visible = visible.trimmed();
    if (command == QStringLiteral("RUN") && dmdbgSetupStage_ == DmdbgSetupStage::OutputRun) {
        if (dmdbg_->runtimeState() != DmdbgRuntimeState::Finished) {
            handleDmdbgCommandFailure(command,
                QStringLiteral("DBMS_OUTPUT 初始化未确认完成。%1").arg(visible));
            return;
        }
        dmdbgSetupStage_ = DmdbgSetupStage::TargetSql;
        messages_->appendPlainText(QStringLiteral("当前 dmdbg 会话已开启 DBMS_OUTPUT，调试输出显示在消息页。"));
        dmdbg_->setSql(std::exchange(pendingDmdbgSql_, {}));
        return;
    }
    if (command == QStringLiteral("SQL")) {
        if (dmdbgSetupStage_ == DmdbgSetupStage::OutputSql) {
            dmdbgSetupStage_ = DmdbgSetupStage::OutputRun;
            dmdbg_->run();
            return;
        }
        if (dmdbgSetupStage_ != DmdbgSetupStage::TargetSql) return;
        dmdbgSetupStage_ = DmdbgSetupStage::None;
        pendingDmdbgSql_.clear();
        dmdbgConfigured_ = true;
        dmdbgPaused_ = false;
        messages_->appendPlainText(QStringLiteral("dmdbg 调试目标已设置。"));
        synchronizePendingBreakpoints();
    } else if (command == QStringLiteral("BT")) {
        const QVector<DmdbgFrameInfo> frames = DmdbgProtocol::parseStack(visible);
        if (!frames.isEmpty()) showDebugLocation(frames.first());
        stackTable_->setRowCount(frames.size());
        for (int row = 0; row < frames.size(); ++row) {
            const DmdbgFrameInfo& frame = frames.at(row);
            const QStringList cells{QStringLiteral("#%1").arg(frame.index), frame.schema,
                frame.method, frame.arguments, QString::number(frame.line), frame.sourceText};
            for (int column = 0; column < cells.size(); ++column) {
                auto* item = new QTableWidgetItem(cells.at(column));
                item->setToolTip(cells.at(column));
                stackTable_->setItem(row, column, item);
            }
        }
        requestNextWatch();
    } else if (command == QStringLiteral("P")) {
        QString error;
        const QString value = DmdbgProtocol::parseVariableValue(visible, &error);
        if (!activeWatchExpression_.isEmpty()) {
            updateWatchValue(activeWatchExpression_, error.isEmpty() ? value : error);
            activeWatchExpression_.clear();
        }
        requestNextWatch();
    } else if (command == QStringLiteral("B") || command == QStringLiteral("D")) {
        if (command == QStringLiteral("B") && pendingEntryBreakpoint_) {
            deferredEntryBreakpoint_ = DmdbgProtocol::isDeferredBreakpoint(visible);
            if (!visible.isEmpty()) messages_->appendPlainText(
                QStringLiteral("dmdbg 入口断点：\n%1").arg(visible));
        }
        if (command == QStringLiteral("D")) {
            deletedBreakpointLines_.remove(activeBreakpointDelete_);
            activeBreakpointDelete_ = 0;
        }
        dmdbg_->requestBreakpoints();
    } else if (command == QStringLiteral("INFO B")) {
        const QVector<DmdbgBreakpointInfo> values = DmdbgProtocol::parseBreakpoints(visible);
        const int targetIndex = targetCombo_->currentIndex();
        const ProgramTarget* target = generatedScriptMatchesEditor()
                && targetIndex >= 0 && targetIndex < targets_.size()
            ? &targets_.at(targetIndex) : nullptr;
        if (pendingEntryBreakpoint_) {
            pendingEntryBreakpoint_ = false;
            for (const auto& value : values) {
                const bool matches = target == nullptr
                    ? (value.schema.isEmpty() || value.schema.compare(profile_.user, Qt::CaseInsensitive) == 0)
                        && value.packageName.isEmpty()
                        && (value.method.isEmpty() || value.method.compare(QStringLiteral("@dbg_main"), Qt::CaseInsensitive) == 0)
                    : value.schema.compare(target->schema, Qt::CaseInsensitive) == 0
                        && value.packageName.compare(target->packageName, Qt::CaseInsensitive) == 0
                        && value.method.compare(target->routineName, Qt::CaseInsensitive) == 0;
                if (matches && value.number > 0 && value.line > 0) entryBreakpointNumber_ = value.number;
            }
            if (entryBreakpointNumber_ <= 0 && !deferredEntryBreakpoint_) {
                pendingDebugRun_ = false;
                setError(QStringLiteral("未确认入口断点，未执行用户程序。断点响应：\n%1")
                    .arg(visible.isEmpty() ? QStringLiteral("INFO B 未返回断点。") : visible));
            } else {
                messages_->appendPlainText(deferredEntryBreakpoint_
                    ? QStringLiteral("调试器已接受延迟入口断点，将在目标加载后生效。")
                    : QStringLiteral("入口断点已确认，将在可执行位置暂停。"));
                runPendingDebug();
            }
            updateActions();
            return;
        }
        for (auto iterator = breakpoints_.begin(); iterator != breakpoints_.end(); ++iterator) {
            if (iterator.value() > 0 || pendingBreakpointLines_.contains(iterator.key()))
                iterator.value() = -1;
        }
        for (const DmdbgBreakpointInfo& value : values) {
            if (value.line <= 0 || value.number <= 0) continue;
            const bool currentTarget = target == nullptr
                ? (value.schema.isEmpty() || value.schema.compare(profile_.user, Qt::CaseInsensitive) == 0)
                    && value.packageName.isEmpty()
                    && (value.method.isEmpty() || value.method.compare(QStringLiteral("@dbg_main"), Qt::CaseInsensitive) == 0)
                : value.schema.compare(target->schema, Qt::CaseInsensitive) == 0
                    && value.packageName.compare(target->packageName, Qt::CaseInsensitive) == 0
                    && value.method.compare(target->routineName, Qt::CaseInsensitive) == 0;
            if (!currentTarget) {
                messages_->appendPlainText(QStringLiteral(
                    "服务器断点 #%1（%2.%3.%4，第 %5 行）的对象归属未确认，未映射到当前脚本。"
                    "该断点可能仍生效，请使用“结束调试”停止目标后重新设置。")
                    .arg(value.number).arg(value.schema, value.packageName, value.method).arg(value.line));
                continue;
            }
            if (!breakpoints_.contains(value.line)) {
                messages_->appendPlainText(QStringLiteral(
                    "服务器返回当前目标的实际断点 #%1，第 %2 行；已保留在断点列表中，可选择并移除。"
                    "未确认的请求行仍标记为同步失败。")
                    .arg(value.number).arg(value.line));
            }
            breakpoints_[value.line] = value.number;
        }
        pendingBreakpointLines_.clear();
        refreshBreakpointTable();
        synchronizePendingBreakpoints();
        if (dmdbgPaused_ && dmdbg_->isReady()) refreshDebugViews();
    } else if (command == QStringLiteral("UP") || command == QStringLiteral("DOWN")) {
        refreshDebugViews();
    } else {
        if (!visible.isEmpty()) {
            messages_->appendPlainText(QStringLiteral("dmdbg %1：\n%2")
                .arg(command, visible));
        }
        if (dmdbg_->runtimeState() == DmdbgRuntimeState::Unknown) {
            messages_->appendPlainText(QStringLiteral(
                "dmdbg 执行响应无法归类：运行和单步已禁用。请结束调试并复核目标状态。"));
        }
        if (dmdbg_->runtimeState() == DmdbgRuntimeState::Debugging) {
            showDebugLocation(DmdbgProtocol::parsePausedLocation(visible));
            if (entryBreakpointNumber_ > 0) {
                pendingBreakpointDeletes_.append(std::exchange(entryBreakpointNumber_, 0));
                synchronizePendingBreakpoints();
            } else refreshDebugViews();
        } else if (dmdbg_->runtimeState() == DmdbgRuntimeState::Finished) {
            showDebugMessages();
        }
    }
    updateActions();
}

void TestDocumentWidget::updateActions() {
    clearMessages_->setEnabled(!isBusy());
    bool supported = !scriptText().trimmed().isEmpty();
    for (int row = 0; supported && row < arguments_->rowCount(); ++row) {
        const QString status = arguments_->item(row, 6) == nullptr
            ? QString() : arguments_->item(row, 6)->text();
        if (status == QStringLiteral("脚本未使用")) continue;
        const QString dataType = arguments_->item(row, 1) == nullptr
            ? QString() : arguments_->item(row, 1)->text();
        auto* useDefault = qobject_cast<QCheckBox*>(arguments_->cellWidget(row, 5));
        if (!isSupportedProgramScalarType(dataType)
                && (useDefault == nullptr || !useDefault->isChecked())) {
            supported = false;
        }
    }
    updateParameterLock();
    run_->setEnabled(connected_ && !isBusy() && supported);
    stop_->setEnabled(busy_);
    stop_->setVisible(busy_);
    const bool canDecide = testTransactionDecisionRequired(transactionState_);
    commit_->setEnabled(connected_ && !busy_ && canDecide);
    rollback_->setEnabled(connected_ && !busy_ && canDecide);
    commit_->setVisible(canDecide);
    rollback_->setVisible(canDecide);
    const ConnectionProfile shownProfile =
        connectingProfile_.id.isEmpty() ? profile_ : connectingProfile_;
    connectionSelector_->setText(shownProfile.name.isEmpty()
        ? QStringLiteral("未连接")
        : QStringLiteral("%1 [%2]")
            .arg(shownProfile.name, connectionEnvironmentLabel(shownProfile.environment)));
    connectionSelector_->setToolTip(shownProfile.name.isEmpty()
        ? QStringLiteral("当前 Test Window 未选择连接")
        : QStringLiteral("连接：%1\n用户：%2\n数据库：%3\n环境：%4\n状态：%5")
            .arg(shownProfile.name, shownProfile.user,
                shownProfile.database.isEmpty() ? QStringLiteral("—") : shownProfile.database,
                connectionEnvironmentLabel(shownProfile.environment), activityDisplay()));
    updateDebugActions();
    emit statusChanged();
}

void TestDocumentWidget::updateDebugActions() {
    updateParameterLock();
    if (dmdbg_ == nullptr) return;
    const DmdbgProcessState state = dmdbg_->state();
    const bool stopped = state == DmdbgProcessState::Stopped;
    const bool ready = state == DmdbgProcessState::Ready;
    const DmdbgRuntimeState runtimeState = dmdbg_->runtimeState();
    const bool scriptMatchesTarget = !scriptText().trimmed().isEmpty();
    const bool pathAvailable = !dmdbgPath_.isEmpty()
        && QFileInfo(dmdbgPath_).isExecutable();
    startDmdbg_->setText(QStringLiteral("开始调试"));
    startDmdbg_->setEnabled(!profile_.readOnly && !busy_ && !pendingDebugRun_ && scriptMatchesTarget
        && ((stopped && connected_ && pathAvailable)
        || (ready && !dmdbgConfigured_)));
    if (!scriptMatchesTarget) {
        startDmdbg_->setToolTip(QStringLiteral("请先输入要调试的 Test Script。"));
    } else if (!pathAvailable) {
        startDmdbg_->setToolTip(QStringLiteral("内置调试器缺失，请重新安装完整安装包。"));
    } else {
        startDmdbg_->setToolTip(QStringLiteral(
            "使用当前 Test Window 的实际物理端点设置调试目标。"));
    }
    runDebug_->setEnabled(!profile_.readOnly && !busy_ && !pendingDebugRun_ && !debugRunConfirmationPending_
        && connected_ && scriptMatchesTarget && ((stopped && pathAvailable) || (ready && !dmdbgPaused_ && dmdbgCapabilities_.start
        && (runtimeState == DmdbgRuntimeState::Waiting || runtimeState == DmdbgRuntimeState::Finished))));
    continueDebug_->setEnabled(
        ready && dmdbgPaused_ && dmdbgCapabilities_.continueExecution);
    stepInto_->setEnabled(ready && dmdbgPaused_ && dmdbgCapabilities_.stepInto);
    stepOver_->setEnabled(ready && dmdbgPaused_ && dmdbgCapabilities_.stepOver);
    stepOut_->setEnabled(ready && dmdbgPaused_ && dmdbgCapabilities_.stepOut);
    stopDebug_->setEnabled(!stopped);
    const auto showDebugControl = [](QPushButton* button, bool visible) {
        auto* toolbar = qobject_cast<QToolBar*>(button->parentWidget());
        if (toolbar != nullptr) {
            for (QAction* action : toolbar->actions()) {
                auto* widgetAction = qobject_cast<QWidgetAction*>(action);
                if (widgetAction != nullptr && widgetAction->defaultWidget() == button) {
                    widgetAction->setVisible(visible);
                    return;
                }
            }
        }
        button->setVisible(visible);
    };
    showDebugControl(startDmdbg_, stopped || !dmdbgConfigured_);
    showDebugControl(continueDebug_, !stopped && dmdbgCapabilities_.continueExecution);
    showDebugControl(stepInto_, !stopped && dmdbgCapabilities_.stepInto);
    showDebugControl(stepOver_, !stopped && dmdbgCapabilities_.stepOver);
    showDebugControl(stepOut_, !stopped && dmdbgCapabilities_.stepOut);
    showDebugControl(stopDebug_, !stopped);
    debugStatus_->setText(stopped
        ? (pendingDebugRun_ ? QStringLiteral("正在准备调试…") : pathAvailable
            ? QStringLiteral("内置调试器就绪") : QStringLiteral("内置调试器缺失"))
        : dmdbgSetupStage_ != DmdbgSetupStage::None ? QStringLiteral("正在初始化调试输出及目标…")
        : dmdbgPaused_ ? QStringLiteral("已暂停，可查看调用栈与变量")
        : runtimeState == DmdbgRuntimeState::Running ? QStringLiteral("正在运行")
        : ready ? QStringLiteral("调试器就绪") : QStringLiteral("正在与调试器通信…"));
    debugStatus_->setToolTip(pathAvailable
        ? QStringLiteral("使用安装包内置调试器：%1\n点击“开始调试”即可，无需配置。")
            .arg(QDir::toNativeSeparators(dmdbgPath_))
        : QStringLiteral("未找到可用调试器，请修复或重新安装完整安装包。"));
    addBreakpoint_->setEnabled(scriptMatchesTarget && state != DmdbgProcessState::WaitingResponse
        && !breakpoints_.contains(breakpointLine_->value()));
    const bool canEditBreakpoints = state != DmdbgProcessState::WaitingResponse
        && runtimeState != DmdbgRuntimeState::Running;
    removeBreakpoint_->setEnabled(
        canEditBreakpoints && breakpointTable_->currentRow() >= 0);
    clearBreakpoints_->setEnabled(canEditBreakpoints && !breakpoints_.isEmpty());
    inspectVariable_->setEnabled(!variableName_->text().trimmed().isEmpty());
    removeWatch_->setEnabled(watchTable_->currentRow() >= 0);
    refreshWatches_->setEnabled(
        ready && dmdbgPaused_ && dmdbgCapabilities_.variables && !watches_.isEmpty());
    frameUp_->setEnabled(ready && dmdbgPaused_ && dmdbgCapabilities_.frameNavigation);
    frameDown_->setEnabled(ready && dmdbgPaused_ && dmdbgCapabilities_.frameNavigation);
    frameUp_->setVisible(!stopped && dmdbgCapabilities_.frameNavigation);
    frameDown_->setVisible(!stopped && dmdbgCapabilities_.frameNavigation);
}

bool TestDocumentWidget::generatedScriptMatchesEditor() const {
    return !lastGeneratedScript_.trimmed().isEmpty()
        && scriptText().trimmed() == lastGeneratedScript_.trimmed();
}

void TestDocumentWidget::updateParameterLock() {
    const bool locked = isBusy();
    if (parametersLocked_ == locked) return;
    parametersLocked_ = locked;
    const QSignalBlocker argumentSignals(arguments_);
    scriptEditor_->setReadOnly(locked);
    targetCombo_->setEnabled(!locked);
    if (auto* scan = findChild<QPushButton*>(QStringLiteral("testScanVariables")))
        scan->setEnabled(!locked);
    if (auto* hint = findChild<QLabel*>(QStringLiteral("testVariableHint")))
        hint->setText(locked ? QStringLiteral("执行中，参数已锁定，可查看和复制。")
            : QStringLiteral("变量来自 :name 绑定；手工变量请确认类型和方向。"));
    for (int row = 0; row < arguments_->rowCount(); ++row) {
        for (int column = 0; column < arguments_->columnCount(); ++column) {
            if (auto* item = arguments_->item(row, column)) {
                if (locked) {
                    item->setData(Qt::UserRole + 97, item->flags().testFlag(Qt::ItemIsEditable));
                    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                } else if (item->data(Qt::UserRole + 97).toBool()) {
                    item->setFlags(item->flags() | Qt::ItemIsEditable);
                }
            }
            if (auto* value = qobject_cast<QLineEdit*>(arguments_->cellWidget(row, column))) {
                value->setReadOnly(locked);
            } else if (QWidget* control = arguments_->cellWidget(row, column)) {
                if (locked) control->setProperty("enabledBeforeExecution", control->isEnabled());
                control->setEnabled(!locked && control->property("enabledBeforeExecution").toBool());
            }
        }
    }
}

void TestDocumentWidget::cancelCurrentTask() {
    if (busy_) {
        executionSoundCancelled_ = true;
        session_->requestCancel();
    }
    else if (isBusy() && stopDebug_->isEnabled()) stopDebug_->click();
}

bool TestDocumentWidget::canExecute() const { return run_ && run_->isEnabled(); }
