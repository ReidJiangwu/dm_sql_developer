#include "LogView.h"
#include "SearchField.h"
#include "SaveFileDialog.h"
#include "MessagePrompt.h"
#include "ConnectionDialog.h"
#include "ConnectionProfileTree.h"
#include "ConnectionExport.h"
#include "AdaptivePopupComboBox.h"
#include "WrappingControlLayout.h"
#include "CommandButton.h"

#include <QMenu>
#include "../core/SqlTextEncoding.h"

#include "../data/DpiSession.h"
#include "../security/CredentialStore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QIntValidator>
#include <QInputDialog>
#include <QTreeWidget>
#include <QTabWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QHeaderView>
#include <QSplitter>
#include <QResizeEvent>
#include <QDesktopServices>
#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QUrl>
#include <QUuid>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <limits>

namespace {

// Presets have readable labels; custom entries remain strictly integer values.
class ServiceNumberValidator final : public QValidator {
public:
    ServiceNumberValidator(QComboBox* combo, int minimum)
        : QValidator(combo), combo_(combo), integer_(minimum, std::numeric_limits<int>::max()) {}
    State validate(QString& text, int& position) const override {
        for (int index = 0; index < combo_->count(); ++index)
            if (text == combo_->itemText(index)) return Acceptable;
        QString number = text.trimmed();
        int cursor = qMin(position, static_cast<int>(number.size()));
        const State state = integer_.validate(number, cursor);
        if (state != Invalid) return state;
        for (int index = 0; index < combo_->count(); ++index)
            if (combo_->itemText(index).startsWith(text)) return Intermediate;
        return Invalid;
    }
private:
    QComboBox* combo_;
    QIntValidator integer_;
};

QComboBox* serviceNumberCombo(QWidget* parent, int minimum) {
    auto* combo = new AdaptivePopupComboBox(parent);
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->setValidator(new ServiceNumberValidator(combo, minimum));
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    combo->lineEdit()->setPlaceholderText(QStringLiteral("选择常用值或输入整数"));
    return combo;
}

int serviceNumber(const QComboBox* combo) {
    const int index = combo->currentIndex();
    if (index >= 0 && combo->currentText() == combo->itemText(index))
        return combo->itemData(index).toInt();
    bool valid = false;
    const int value = combo->currentText().trimmed().toInt(&valid);
    return valid ? value : -1; // Never silently replace invalid/empty text with a preset.
}

void setServiceNumber(QComboBox* combo, int value) {
    int index = combo->findData(value);
    if (index < 0) { combo->addItem(QString::number(value), value); index = combo->count() - 1; }
    combo->setCurrentIndex(index);
}

QVector<ConnectionEndpoint> parseEndpointLines(const QString& text, bool service = false) {
    QVector<ConnectionEndpoint> endpoints;
    int priorityFallback = 100;
    for (const QString& rawLine : text.split(u'\n')) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) continue;
        const QStringList fields = line.split(u',');
        if (service && fields.size() != 1) {
            endpoints.append({{}, 0, 100, true, {}});
            continue;
        }
        QString address = fields.value(0).trimmed();
        bool priorityOk = false;
        const int priority = fields.value(1).trimmed().toInt(&priorityOk);
        QString host;
        int port = 0;
        if (address.startsWith(u'[')) {
            const int closing = address.indexOf(u']');
            host = closing > 1 ? address.mid(1, closing - 1) : QString{};
            if (closing >= 0 && address.mid(closing + 1).startsWith(u':')) {
                port = address.mid(closing + 2).toInt();
            }
        } else {
            const int separator = address.lastIndexOf(u':');
            host = separator > 0 ? address.left(separator).trimmed() : QString{};
            port = separator > 0 ? address.mid(separator + 1).toInt() : 0;
        }
        endpoints.append({host, port,
            priorityOk ? priority : priorityFallback, true, {}});
        priorityFallback += 100;
    }
    return endpoints;
}

QString formatEndpointLines(const QVector<ConnectionEndpoint>& endpoints, bool service = false) {
    QStringList lines;
    for (const ConnectionEndpoint& endpoint : endpoints) {
        QString host = endpoint.host;
        if (host.contains(u':') && !host.startsWith(u'[')) host = u'[' + host + u']';
        const QString address = QStringLiteral("%1:%2").arg(host).arg(endpoint.port);
        lines.append(service ? address : address + QStringLiteral(",%1").arg(endpoint.priority));
    }
    return lines.join(u'\n');
}

}  // namespace

ConnectionDialog::ConnectionDialog(
    QWidget* parent, QString storePath, CredentialStore* credentialStore)
    : QDialog(parent)
    , store_(std::move(storePath))
    , credentialStore_(credentialStore != nullptr
              ? credentialStore
              : &CredentialStore::systemStore()) {
    setObjectName(QStringLiteral("connectionDialog"));
    setWindowTitle(QStringLiteral("连接管理"));
    setModal(true);
    setMinimumWidth(620);
    resize(980, 620);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* selectorBar = new QWidget(this);
    selectorBar->setObjectName(QStringLiteral("connectionProfileBar"));
    auto* selectorLayout = new QHBoxLayout(selectorBar);
    selectorLayout->setContentsMargins(10, 5, 10, 5);
    selectorLayout->setSpacing(6);
    auto* selectorLabel = new QLabel(QStringLiteral("已保存连接："), selectorBar);
    profileSelector_ = new AdaptivePopupComboBox(selectorBar);
    profileSelector_->setMinimumContentsLength(12);
    profileSelector_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    profileSelector_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(profileSelector_, &QComboBox::currentTextChanged, profileSelector_, &QWidget::setToolTip);
    profileSelector_->setObjectName(QStringLiteral("connectionProfileSelector"));
    profileSelector_->setAccessibleName(QStringLiteral("已保存连接"));
    selectorLabel->setBuddy(profileSelector_);
    newButton_ = new QPushButton(QStringLiteral("新建"), selectorBar);
    newButton_->setObjectName(QStringLiteral("newConnectionProfile"));
    deleteButton_ = new QPushButton(QStringLiteral("删除"), selectorBar);
    deleteButton_->setObjectName(QStringLiteral("deleteConnectionProfile"));
    deleteButton_->setToolTip(QStringLiteral("删除连接配置及其本机安全凭据"));
    copyButton_ = new QPushButton(QStringLiteral("复制"), selectorBar);
    copyButton_->setObjectName(QStringLiteral("copyConnectionProfile"));
    copyButton_->setToolTip(QStringLiteral("复制连接参数，不复制本机安全凭据"));
    importButton_ = new QPushButton(QStringLiteral("导入"), selectorBar);
    importButton_->setToolTip(QStringLiteral("导入小智达梦管理工具 JSON 或 SQLLark XML 连接"));
    importButton_->setProperty("menuButton", true);
    auto* importMenu = new QMenu(importButton_);
    importMenu->addAction(QStringLiteral("JSON…"), this,
        [this]() { importProfiles(); })->setToolTip(QStringLiteral("从小智达梦管理工具 JSON 导入连接"));
    importMenu->addAction(QStringLiteral("SQLLark XML…"), this,
        [this]() { importProfiles(true); })->setToolTip(QStringLiteral("从 SQLLark XML 导入连接"));
    importButton_->setMenu(importMenu);
    exportButton_ = new QPushButton(QStringLiteral("导出"), selectorBar);
    exportButton_->setToolTip(QStringLiteral("导出全部连接参数；不会包含密码"));
    selectorLayout->addWidget(selectorLabel);
    selectorLayout->addWidget(profileSelector_, 1);
    selectorLayout->addWidget(newButton_);
    selectorLayout->addWidget(copyButton_);
    selectorLayout->addWidget(deleteButton_);
    selectorLayout->addWidget(importButton_);
    selectorLayout->addWidget(exportButton_);
    for (QPushButton* button : {newButton_, copyButton_, deleteButton_,
             importButton_, exportButton_}) {
        button->setProperty("quietAction", true);
    }
    layout->addWidget(selectorBar);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setObjectName(QStringLiteral("connectionFormScroll"));
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    auto* formContent = new QWidget(scrollArea);
    auto* formContentLayout = new QVBoxLayout(formContent);
    formContentLayout->setContentsMargins(0, 0, 0, 0);
    formContentLayout->setSpacing(8);
    auto* basicForm = new QFormLayout();
    basicForm_ = basicForm;
    basicForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    basicForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    nameEdit_ = new QLineEdit(this);
    connectionTypeCombo_ = new QComboBox(this);
    connectionTypeCombo_->setObjectName(QStringLiteral("connectionType"));
    connectionTypeCombo_->addItem(QStringLiteral("单节点"),
        static_cast<int>(ConnectionType::Direct));
    connectionTypeCombo_->addItem(QStringLiteral("集群（dm_svc.conf）"),
        static_cast<int>(ConnectionType::DmService));
    connectionTypeCombo_->insertSeparator(2);
    connectionTypeCombo_->addItem(QStringLiteral("多节点（应用管理）"),
        static_cast<int>(ConnectionType::EndpointGroup));
    hostEdit_ = new QLineEdit(this);
    portEdit_ = new QSpinBox(this);
    portEdit_->setRange(1, 65'535);
    portEdit_->setValue(5236);
    endpointsEdit_ = new QPlainTextEdit(this);
    endpointsEdit_->setObjectName(QStringLiteral("connectionEndpoints"));
    endpointsEdit_->setPlaceholderText(QStringLiteral(
        "每行一个端点：host:port,priority\nIPv6：[2001:db8::1]:5236,100"));
    endpointsEdit_->setFixedHeight(82);
    topologyCombo_ = new QComboBox(this);
    topologyCombo_->setObjectName(QStringLiteral("connectionTopology"));
    topologyCombo_->addItem(QStringLiteral("通用／未确认"),
        static_cast<int>(ConnectionTopology::Generic));
    topologyCombo_->addItem(QStringLiteral("数据守护"),
        static_cast<int>(ConnectionTopology::DataGuard));
    topologyCombo_->addItem(QStringLiteral("DSC"),
        static_cast<int>(ConnectionTopology::Dsc));
    requiredRoleCombo_ = new QComboBox(this);
    requiredRoleCombo_->setObjectName(QStringLiteral("connectionRequiredRole"));
    for (const RequiredDatabaseRole role : {
            RequiredDatabaseRole::AnyOpen, RequiredDatabaseRole::WritableOpen,
            RequiredDatabaseRole::NormalOpen, RequiredDatabaseRole::PrimaryOpen,
            RequiredDatabaseRole::StandbyOpen, RequiredDatabaseRole::DscOpen}) {
        requiredRoleCombo_->addItem(requiredDatabaseRoleLabel(role), static_cast<int>(role));
    }
    selectionPolicyCombo_ = new QComboBox(this);
    selectionPolicyCombo_->setObjectName(QStringLiteral("connectionSelectionPolicy"));
    selectionPolicyCombo_->addItem(QStringLiteral("按优先级"),
        static_cast<int>(EndpointSelectionPolicy::Ordered));
    selectionPolicyCombo_->addItem(QStringLiteral("新会话轮询"),
        static_cast<int>(EndpointSelectionPolicy::RoundRobin));
    selectionPolicyCombo_->addItem(QStringLiteral("最近成功优先"),
        static_cast<int>(EndpointSelectionPolicy::LastSuccessfulFirst));
    expectedDatabaseEdit_ = new QLineEdit(this);
    expectedOguidEdit_ = new QLineEdit(this);
    userEdit_ = new QLineEdit(this);
    groupEdit_ = new AdaptivePopupComboBox(this);
    groupEdit_->setEditable(true);
    groupEdit_->setInsertPolicy(QComboBox::NoInsert);
    groupEdit_->lineEdit()->setPlaceholderText(QStringLiteral("选择已有分组或输入新分组"));
    groupEdit_->setObjectName(QStringLiteral("connectionNavigationGroup"));
    groupEdit_->setAccessibleName(QStringLiteral("导航分组"));
    groupEdit_->setToolTip(QStringLiteral(
        "可下拉选择已保存连接的分组，也可输入新分组；留空表示未分组。"
        "仅用于整理左侧连接导航，与多节点端点组无关。"));
    databaseEdit_ = new QLineEdit(this);
    driverCombo_ = new QComboBox(this);
    driverCombo_->addItems({QStringLiteral("DM8"), QStringLiteral("DM9")});
    timeoutEdit_ = new QSpinBox(this);
    timeoutEdit_->setRange(1, 300);
    timeoutEdit_->setValue(15);
    timeoutEdit_->setSuffix(QStringLiteral(" 秒"));
    timeoutEdit_->setToolTip(QStringLiteral(
        "只限制登录和端点建连等待，不限制 SQL、Command 或 Test Window 的执行时长。"));
    fetchTimeoutEdit_ = new QSpinBox(this);
    fetchTimeoutEdit_->setRange(1, 86'400);
    fetchTimeoutEdit_->setValue(60);
    fetchTimeoutEdit_->setSuffix(QStringLiteral(" 秒"));
    cancelTimeoutEdit_ = new QSpinBox(this);
    cancelTimeoutEdit_->setRange(1, 60);
    cancelTimeoutEdit_->setValue(5);
    cancelTimeoutEdit_->setSuffix(QStringLiteral(" 秒"));
    readOnly_ = new QCheckBox(QStringLiteral("只读连接（禁止写 SQL 和管理操作）"), this);
    readOnly_->setObjectName(QStringLiteral("connectionReadOnly"));
    readOnly_->setToolTip(QStringLiteral("仅按手动选择启用工具只读保护，切换主库或备库不自动改变此选项。"));
    environmentCombo_ = new QComboBox(this);
    environmentCombo_->setObjectName(QStringLiteral("connectionEnvironment"));
    environmentCombo_->setAccessibleName(QStringLiteral("连接环境"));
    environmentCombo_->addItem(QStringLiteral("开发"),
        static_cast<int>(ConnectionEnvironment::Development));
    environmentCombo_->addItem(QStringLiteral("测试"),
        static_cast<int>(ConnectionEnvironment::Test));
    environmentCombo_->addItem(QStringLiteral("生产"),
        static_cast<int>(ConnectionEnvironment::Production));
    passwordEdit_ = new QLineEdit(this);
    passwordEdit_->setEchoMode(QLineEdit::Password);
    descriptionEdit_ = new QLineEdit(this);
    showPassword_ = new QCheckBox(QStringLiteral("显示密码"), this);
    showPassword_->setAccessibleName(QStringLiteral("显示或隐藏数据库密码"));
    savePassword_ = new QCheckBox(QStringLiteral("连接验证成功后安全保存密码"), this);
    savePassword_->setObjectName(QStringLiteral("saveConnectionPassword"));
    savePassword_->setAccessibleName(
        QStringLiteral("连接验证成功后在操作系统安全凭据库中保存数据库密码"));
    removePasswordButton_ = new QPushButton(QStringLiteral("移除已保存密码"), this);
    removePasswordButton_->setObjectName(QStringLiteral("removeStoredPassword"));

    nameEdit_->setObjectName(QStringLiteral("connectionName"));
    hostEdit_->setObjectName(QStringLiteral("connectionHost"));
    portEdit_->setObjectName(QStringLiteral("connectionPort"));
    userEdit_->setObjectName(QStringLiteral("connectionUser"));
    passwordEdit_->setObjectName(QStringLiteral("connectionPassword"));
    descriptionEdit_->setObjectName(QStringLiteral("connectionDescription"));
    basicForm->addRow(QStringLiteral("连接名称："), nameEdit_);
    basicForm->addRow(QStringLiteral("主机："), hostEdit_);
    basicForm->addRow(QStringLiteral("端口："), portEdit_);
    basicForm->addRow(QStringLiteral("用户名："), userEdit_);
    basicForm->addRow(QStringLiteral("密码："), passwordEdit_);
    basicForm->addRow(QStringLiteral("环境："), environmentCombo_);
    basicForm->addRow(QStringLiteral("导航分组："), groupEdit_);
    auto* credentialOptions = new QWidget(this);
    credentialOptions->setObjectName(QStringLiteral("connectionCredentialOptions"));
    auto* credentialOptionsLayout = new QHBoxLayout(credentialOptions);
    credentialOptionsLayout->setContentsMargins(0, 0, 0, 0);
    credentialOptionsLayout->addWidget(showPassword_);
    credentialOptionsLayout->addWidget(savePassword_);
    credentialOptionsLayout->addWidget(removePasswordButton_);
    credentialOptionsLayout->addStretch(1);
    basicForm->addRow(QString(), credentialOptions);
    formContentLayout->addLayout(basicForm);

    advancedToggle_ = new QToolButton(formContent);
    advancedToggle_->setObjectName(QStringLiteral("connectionAdvancedToggle"));
    advancedToggle_->setProperty("textButton", true);
    advancedToggle_->setText(QStringLiteral("高级设置"));
    advancedToggle_->setCheckable(true);
    advancedToggle_->setArrowType(Qt::RightArrow);
    advancedToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedToggle_->setAccessibleName(QStringLiteral("展开或折叠连接高级设置"));
    formContentLayout->addWidget(advancedToggle_, 0, Qt::AlignLeft);

    advancedPanel_ = new QWidget(formContent);
    advancedPanel_->setObjectName(QStringLiteral("connectionAdvancedPanel"));
    auto* advancedForm = new QFormLayout(advancedPanel_);
    advancedForm_ = advancedForm;
    advancedForm->setContentsMargins(10, 10, 10, 10);
    advancedForm->setVerticalSpacing(8);
    advancedForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    advancedForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    basicForm->insertRow(1, QStringLiteral("连接类型："), connectionTypeCombo_);
    basicForm->insertRow(4, QStringLiteral("多节点端点："), endpointsEdit_);
    servicePanel_ = new QWidget(formContent);
    servicePanel_->setObjectName(QStringLiteral("connectionServicePanel"));
    auto* serviceForm = new QGridLayout(servicePanel_);
    serviceForm->setContentsMargins(0, 0, 0, 0);
    serviceForm->setHorizontalSpacing(16);
    serviceForm->setVerticalSpacing(6);
    serviceForm->setColumnStretch(0, 1);
    serviceForm->setColumnStretch(1, 1);
    const auto addServiceField = [this, serviceForm](
            int row, int column, const QString& text, QWidget* field) {
        auto* label = new QLabel(text, servicePanel_);
        label->setBuddy(field);
        label->setWordWrap(true);
        field->setAccessibleName(text);
        field->setMinimumWidth(0);
        field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        serviceForm->addWidget(label, row * 2, column, Qt::AlignTop);
        serviceForm->addWidget(field, row * 2 + 1, column, Qt::AlignTop);
    };
    serviceAliasEdit_ = new QLineEdit(QStringLiteral("GRP1_HA"), servicePanel_);
    serviceAliasEdit_->setObjectName(QStringLiteral("connectionServiceAlias"));
    serviceAliasEdit_->setMaxLength(64);
    addServiceField(0, 0, QStringLiteral("服务名（Service Name）"), serviceAliasEdit_);
    serviceLoginModeCombo_ = new AdaptivePopupComboBox(servicePanel_);
    serviceLoginModeCombo_->setObjectName(QStringLiteral("connectionServiceLoginMode"));
    serviceLoginModeCombo_->addItems({QStringLiteral("0－优先主库"), QStringLiteral("1－只连接主库"),
        QStringLiteral("2－只连接备库"), QStringLiteral("3－优先备库"), QStringLiteral("4－优先普通库")});
    serviceLoginModeCombo_->setCurrentIndex(1);
    serviceLoginModeCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    addServiceField(0, 1, QStringLiteral("登录模式（LOGIN_MODE）"), serviceLoginModeCombo_);
    serviceEpSelectorEdit_ = serviceNumberCombo(servicePanel_, 0);
    serviceEpSelectorEdit_->setObjectName(QStringLiteral("connectionServiceEpSelector"));
    serviceEpSelectorEdit_->addItem(QStringLiteral("0－轮询节点"), 0);
    serviceEpSelectorEdit_->addItem(QStringLiteral("1－优先节点 1"), 1);
    setServiceNumber(serviceEpSelectorEdit_, 1);
    serviceEpSelectorEdit_->setToolTip(QStringLiteral("0：轮询节点；N：优先列表中第 N 个节点。也可输入非负整数，保留已有配置值。"));
    connect(endpointsEdit_, &QPlainTextEdit::textChanged, this, [this]() {
        const QString text = serviceEpSelectorEdit_->currentText();
        const int value = serviceNumber(serviceEpSelectorEdit_);
        const QSignalBlocker blocker(serviceEpSelectorEdit_);
        serviceEpSelectorEdit_->clear();
        serviceEpSelectorEdit_->addItem(QStringLiteral("0－轮询节点"), 0);
        const int count = qMax(1, static_cast<int>(parseEndpointLines(endpointsEdit_->toPlainText(), true).size()));
        for (int node = 1; node <= count; ++node)
            serviceEpSelectorEdit_->addItem(QStringLiteral("%1－优先节点 %1").arg(node), node);
        if (value >= 0) setServiceNumber(serviceEpSelectorEdit_, value);
        else serviceEpSelectorEdit_->setEditText(text);
    });
    addServiceField(1, 0, QStringLiteral("节点选择（EP_SELECTOR）"), serviceEpSelectorEdit_);
    serviceAutoReconnectCombo_ = new AdaptivePopupComboBox(servicePanel_);
    serviceAutoReconnectCombo_->setObjectName(QStringLiteral("connectionServiceAutoReconnect"));
    serviceAutoReconnectCombo_->addItems({QStringLiteral("0－断开连接"), QStringLiteral("1－故障切换"),
        QStringLiteral("2－切换并回切")});
    serviceAutoReconnectCombo_->setCurrentIndex(1);
    serviceAutoReconnectCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    serviceAutoReconnectCombo_->setToolTip(QStringLiteral("0：断开；1：故障时切换；2：切换并在优先节点恢复后回切，要求节点选择大于 0。"));
    addServiceField(1, 1, QStringLiteral("故障重连（AUTO_RECONNECT）"), serviceAutoReconnectCombo_);
    serviceSwitchIntervalEdit_ = serviceNumberCombo(servicePanel_, 1);
    serviceSwitchIntervalEdit_->setObjectName(QStringLiteral("connectionServiceSwitchInterval"));
    for (int value : {100, 200, 500, 1000, 2000, 5000})
        serviceSwitchIntervalEdit_->addItem(QStringLiteral("%1 毫秒").arg(value), value);
    setServiceNumber(serviceSwitchIntervalEdit_, 200);
    serviceSwitchIntervalEdit_->setToolTip(QStringLiteral("单位为毫秒。可选常用值或输入 1～2147483647 的整数。"));
    addServiceField(2, 0, QStringLiteral("切换间隔（SWITCH_INTERVAL）"), serviceSwitchIntervalEdit_);
    serviceSwitchTimesEdit_ = serviceNumberCombo(servicePanel_, 1);
    serviceSwitchTimesEdit_->setObjectName(QStringLiteral("connectionServiceSwitchTimes"));
    for (int value : {1, 2, 3, 5, 10, 20})
        serviceSwitchTimesEdit_->addItem(QStringLiteral("%1 次").arg(value), value);
    setServiceNumber(serviceSwitchTimesEdit_, 3);
    serviceSwitchTimesEdit_->setToolTip(QStringLiteral("可选常用次数或输入 1～2147483647 的整数。"));
    addServiceField(2, 1, QStringLiteral("切换次数（SWITCH_TIMES）"), serviceSwitchTimesEdit_);
    serviceConfigPathEdit_ = new QLineEdit(servicePanel_);
    serviceConfigPathEdit_->setObjectName(QStringLiteral("connectionServiceConfigPath"));
    serviceConfigPathEdit_->setReadOnly(true);
    serviceConfigPathEdit_->setPlaceholderText(QStringLiteral("保存后生成本机 dm_svc.conf"));
    serviceConfigPathEdit_->setAccessibleName(QStringLiteral("配置文件（dm_svc.conf）"));
    serviceConfigPathEdit_->setToolTip(QStringLiteral(
        "保存在当前用户的连接配置目录；时区为 480，语言为 cn。"));
    auto* servicePathLabel = new QLabel(QStringLiteral("配置文件（dm_svc.conf）"), servicePanel_);
    servicePathLabel->setBuddy(serviceConfigPathEdit_);
    serviceForm->addWidget(servicePathLabel, 6, 0, 1, 2);
    serviceForm->addWidget(serviceConfigPathEdit_, 7, 0, 1, 2);
    auto* serviceHint = new QLabel(QStringLiteral(
        "只读由手动选择，不随登录目标改变；修改配置后重新连接。"), servicePanel_);
    serviceHint->setObjectName(QStringLiteral("secondaryText"));
    serviceHint->setToolTip(QStringLiteral(
        "节点按填写顺序选择，不使用应用端点优先级。故障切换不会重放 SQL 或恢复未提交事务。"));
    serviceHint->setWordWrap(true);
    serviceForm->addWidget(serviceHint, 8, 0, 1, 2);
    basicForm->insertRow(5, servicePanel_);
    advancedForm->addRow(QStringLiteral("拓扑："), topologyCombo_);
    advancedForm->addRow(QStringLiteral("实例要求："), requiredRoleCombo_);
    advancedForm->addRow(QStringLiteral("端点策略："), selectionPolicyCombo_);
    advancedForm->addRow(QStringLiteral("预期数据库名："), expectedDatabaseEdit_);
    advancedForm->addRow(QStringLiteral("预期 OGUID："), expectedOguidEdit_);
    advancedForm->addRow(QStringLiteral("数据库："), databaseEdit_);
    advancedForm->addRow(QStringLiteral("驱动标识："), driverCombo_);
    sqlTextEncodingCombo_ = new QComboBox(advancedPanel_);
    sqlTextEncodingCombo_->setObjectName(QStringLiteral("sqlTextEncoding"));
    sqlTextEncodingCombo_->addItems(reid::text::supportedSqlTextEncodings());
    sqlTextEncodingCombo_->setToolTip(QStringLiteral(
        "默认 UTF-8；也支持 UTF-16、GBK 和 ISO-8859-1。\n"
        "SQL 中包含所选编码无法表示的字符时停止发送，不替换为问号。ISO-8859-1 不支持中文。\n"
        "文本参数与结果传输保持 UTF-8；文件读取编码独立识别。修改后重新连接生效。"));
    advancedForm->addRow(QStringLiteral("SQL 发送编码："), sqlTextEncodingCombo_);
    advancedForm->addRow(QStringLiteral("登录／建连超时："), timeoutEdit_);
    advancedForm->addRow(QStringLiteral("结果获取超时："), fetchTimeoutEdit_);
    advancedForm->addRow(QStringLiteral("取消等待超时："), cancelTimeoutEdit_);
    advancedForm->addRow(QString(), readOnly_);
    advancedForm->addRow(QStringLiteral("说明："), descriptionEdit_);
    advancedPanel_->hide();
    formContentLayout->addWidget(advancedPanel_);
    formContentLayout->addStretch(1);
    scrollArea->setWidget(formContent);
    layout->addWidget(scrollArea, 1);

    statusLabel_ = new QLabel(QStringLiteral("填写连接信息后可测试或连接。"), this);
    statusLabel_->setObjectName(QStringLiteral("connectionStatus"));
    statusLabel_->setProperty("callout", QStringLiteral("info"));
    statusLabel_->setWordWrap(true);
    statusLabel_->setTextFormat(Qt::PlainText);
    statusLabel_->setAccessibleName(QStringLiteral("连接操作状态"));
    layout->addWidget(statusLabel_);
    statusToggle_ = new QToolButton(this);
    statusToggle_->setObjectName(QStringLiteral("connectionStatusDetailsToggle"));
    statusToggle_->setText(QStringLiteral("查看详细信息")); statusToggle_->setCheckable(true); statusToggle_->hide();
    statusDetails_ = new QPlainTextEdit(this); LogView::configure(statusDetails_);
    statusDetails_->setObjectName(QStringLiteral("connectionStatusDetails"));
    statusDetails_->setReadOnly(true); statusDetails_->setMaximumHeight(120); statusDetails_->hide();
    connect(statusToggle_, &QToolButton::toggled, statusDetails_, &QPlainTextEdit::setVisible);
    layout->addWidget(statusToggle_); layout->addWidget(statusDetails_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("connectionDialogActions"));
    saveButton_ = buttons->addButton(QStringLiteral("保存"), QDialogButtonBox::ActionRole);
    testButton_ = buttons->addButton(QStringLiteral("测试"), QDialogButtonBox::ActionRole);
    connectButton_ = buttons->addButton(QStringLiteral("连接"), QDialogButtonBox::AcceptRole);
    saveButton_->setObjectName(QStringLiteral("saveConnectionProfile"));
    testButton_->setObjectName(QStringLiteral("testConnectionProfile"));
    connectButton_->setObjectName(QStringLiteral("connectConnectionProfile"));
    connectButton_->setToolTip(
        QStringLiteral("验证成功后关闭窗口，并连接到当前窗口"));
    saveButton_->setProperty("quietAction", true);
    saveButton_->setAccessibleName(QStringLiteral("保存连接配置"));
    saveButton_->setToolTip(QStringLiteral("仅保存连接参数；密码在测试或连接验证成功后按所选设置保存。"));
    testButton_->setProperty("quietAction", true);
    for (auto* button : buttons->buttons())
        if (auto* action = qobject_cast<QPushButton*>(button)) action->setAutoDefault(action == connectButton_);
    connectButton_->setDefault(true);
    layout->addWidget(buttons);

    buildWorkspace();
    const QHash<QString, QString> actionIcons{
        {QStringLiteral("新建"), QStringLiteral("new-sql")}, {QStringLiteral("复制"), QStringLiteral("copy")},
        {QStringLiteral("删除"), QStringLiteral("trash")}, {QStringLiteral("导入"), QStringLiteral("import")},
        {QStringLiteral("导出"), QStringLiteral("export")}, {QStringLiteral("连接"), QStringLiteral("connect")},
        {QStringLiteral("保存"), QStringLiteral("save")}, {QStringLiteral("测试"), QStringLiteral("execute")},
        {QStringLiteral("取消"), QStringLiteral("close")}, {QStringLiteral("添加"), QStringLiteral("new-sql")},
        {QStringLiteral("移除"), QStringLiteral("trash")}, {QStringLiteral("上移"), QStringLiteral("arrow-up")},
        {QStringLiteral("下移"), QStringLiteral("arrow-down")}, {QStringLiteral("批量粘贴…"), QStringLiteral("import")},
        {QStringLiteral("复制配置"), QStringLiteral("copy")}, {QStringLiteral("复制路径"), QStringLiteral("copy")},
        {QStringLiteral("打开目录"), QStringLiteral("folder")}, {QStringLiteral("移除已保存密码"), QStringLiteral("trash")}};
    for (auto* button : findChildren<QPushButton*>()) {
        const auto icon = actionIcons.value(button->text());
        if (icon.isEmpty()) continue;
        const QString name = button->accessibleName().isEmpty() ? button->text() : button->accessibleName();
        const QString detail = button->toolTip();
        CommandButton::iconOnly(button, icon, name);
        if (!detail.isEmpty()) button->setToolTip(detail);
    }
    CommandButton::iconOnly(buttons->button(QDialogButtonBox::Cancel), QStringLiteral("close"), QStringLiteral("取消"));

    connect(profileSelector_, &QComboBox::currentIndexChanged, this, [this]() {
        if (loadingForm_) return;
        const int selected = profileSelector_->currentIndex();
        const QString requested = selected >= 0 && selected < profiles_.size() ? profiles_[selected].id : QString();
        if (!confirmUnsavedChanges()) {
            const QSignalBlocker blocker(profileSelector_);
            for (int index = 0; index < profiles_.size(); ++index)
                if (profiles_[index].id == editingProfileId_) profileSelector_->setCurrentIndex(index);
            syncProfileSelection();
            // The view finishes its mouse selection after currentItemChanged.
            // Reconcile the highlight again once that handler has returned.
            QTimer::singleShot(0, this, &ConnectionDialog::syncProfileSelection);
            return;
        }
        const QSignalBlocker blocker(profileSelector_);
        for (int index = 0; index < profiles_.size(); ++index)
            if (profiles_[index].id == requested) profileSelector_->setCurrentIndex(index);
        loadSelectedProfile();
    });
    connect(connectionTypeCombo_, &QComboBox::currentIndexChanged, this, [this]() {
        updateConnectionTypeControls();
    });
    connect(serviceLoginModeCombo_, &QComboBox::currentIndexChanged, this, [this]() {
        updateConnectionTypeControls();
    });
    connect(advancedToggle_, &QToolButton::toggled, this, [this](bool expanded) {
        if (categories_) categories_->setCurrentIndex(expanded ? 2 : 0);
        advancedToggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    });
    connect(newButton_, &QPushButton::clicked, this, [this]() {
        if (confirmUnsavedChanges()) startNewProfile();
    });
    connect(deleteButton_, &QPushButton::clicked, this, [this]() {
        deleteSelectedProfile();
    });
    connect(copyButton_, &QPushButton::clicked, this, [this]() {
        if (confirmUnsavedChanges()) copySelectedProfile();
    });
    connect(exportButton_, &QPushButton::clicked, this, [this]() {
        exportProfiles();
    });
    connect(saveButton_, &QPushButton::clicked, this, [this]() {
        (void)saveForm(false);
    });
    connect(testButton_, &QPushButton::clicked, this, [this]() {
        testConnection(false);
    });
    connect(connectButton_, &QPushButton::clicked, this, [this]() {
        testConnection(true);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(showPassword_, &QCheckBox::toggled, this, [this](bool visible) {
        passwordEdit_->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
    });
    connect(savePassword_, &QCheckBox::toggled, this, [this](bool checked) {
        if (!suppressSavePasswordToggle_ && !checked && !loadedPassword_.isEmpty()) {
            removeStoredCredential();
        }
    });
    connect(removePasswordButton_, &QPushButton::clicked, this, [this]() {
        removeStoredCredential();
    });

    setTabOrder(profileSelector_, newButton_);
    setTabOrder(newButton_, deleteButton_);
    setTabOrder(deleteButton_, nameEdit_);
    setTabOrder(nameEdit_, connectionTypeCombo_);
    setTabOrder(connectionTypeCombo_, environmentCombo_);
    setTabOrder(environmentCombo_, groupEdit_);
    setTabOrder(groupEdit_, hostEdit_);
    setTabOrder(hostEdit_, portEdit_);
    setTabOrder(portEdit_, serviceAliasEdit_);
    setTabOrder(serviceAliasEdit_, serviceLoginModeCombo_);
    setTabOrder(serviceLoginModeCombo_, userEdit_);
    setTabOrder(userEdit_, passwordEdit_);
    setTabOrder(passwordEdit_, showPassword_);
    setTabOrder(showPassword_, savePassword_);
    setTabOrder(savePassword_, removePasswordButton_);
    setTabOrder(removePasswordButton_, endpointTable_);
    setTabOrder(endpointTable_, descriptionEdit_);
    setTabOrder(descriptionEdit_, saveButton_);
    setTabOrder(saveButton_, testButton_);
    setTabOrder(testButton_, connectButton_);

    QString unavailableReason;
    credentialStoreAvailable_ = credentialStore_->isAvailable(&unavailableReason);
    savePassword_->setEnabled(credentialStoreAvailable_);
    savePassword_->setToolTip(credentialStoreAvailable_
            ? QStringLiteral("密码只写入操作系统安全凭据库")
            : unavailableReason);
    const auto changed = [this]() { markDraftChanged(); };
    for (auto* edit : {nameEdit_, hostEdit_, userEdit_, passwordEdit_, descriptionEdit_, databaseEdit_, expectedDatabaseEdit_, expectedOguidEdit_, serviceAliasEdit_})
        connect(edit, &QLineEdit::textChanged, this, changed);
    for (auto* combo : {connectionTypeCombo_, environmentCombo_, groupEdit_, topologyCombo_, requiredRoleCombo_, selectionPolicyCombo_, driverCombo_, sqlTextEncodingCombo_,
            serviceLoginModeCombo_, serviceEpSelectorEdit_, serviceAutoReconnectCombo_, serviceSwitchIntervalEdit_, serviceSwitchTimesEdit_})
        connect(combo, &QComboBox::currentTextChanged, this, changed);
    for (auto* spin : {portEdit_, timeoutEdit_, fetchTimeoutEdit_, cancelTimeoutEdit_}) connect(spin, &QSpinBox::valueChanged, this, changed);
    connect(readOnly_, &QCheckBox::toggled, this, changed);
    loadProfiles();
    updateConnectionTypeControls();
}

void ConnectionDialog::buildWorkspace() {
    auto* root = qobject_cast<QVBoxLayout*>(layout());
    auto* oldScroll = findChild<QScrollArea*>(QStringLiteral("connectionFormScroll"));
    profileBar_ = findChild<QWidget*>(QStringLiteral("connectionProfileBar"));
    root->removeWidget(oldScroll);
    root->removeWidget(profileBar_);

    auto* barLayout = qobject_cast<QHBoxLayout*>(profileBar_->layout());
    while (auto* item = barLayout->takeAt(0)) {
        if (auto* label = qobject_cast<QLabel*>(item->widget())) delete label;
        delete item;
    }
    auto* moreMenu = new QMenu(this);
    const auto addButtonAction = [moreMenu](QPushButton* button, const QString& name, const QString& icon) {
        auto* action = moreMenu->addAction(InterfaceIcons::icon(icon), name, button, &QPushButton::click);
        QObject::connect(moreMenu, &QMenu::aboutToShow, action, [action, button]() { action->setEnabled(button->isEnabled()); });
        button->hide();
    };
    addButtonAction(copyButton_, QStringLiteral("复制"), QStringLiteral("copy"));
    addButtonAction(deleteButton_, QStringLiteral("删除…"), QStringLiteral("trash"));
    moreMenu->addSeparator();
    auto* importAction = moreMenu->addMenu(importButton_->menu());
    importAction->setText(QStringLiteral("导入")); importAction->setIcon(InterfaceIcons::icon(QStringLiteral("import")));
    for (auto* action : importButton_->menu()->actions()) action->setIcon(InterfaceIcons::icon(QStringLiteral("import")));
    importButton_->hide();
    exportButton_->hide();
    auto* exportMenu = moreMenu->addMenu(InterfaceIcons::icon(QStringLiteral("export")), QStringLiteral("导出"));
    connect(moreMenu, &QMenu::aboutToShow, exportMenu, [this, exportMenu]() {
        exportMenu->menuAction()->setEnabled(!busy_ && !profiles_.isEmpty());
    });
    connect(exportMenu, &QMenu::aboutToShow, this, [this, exportMenu]() {
        exportMenu->clear();
        auto* item = profileList_ ? profileList_->currentItem() : nullptr;
        std::optional<QString> group;
        if (item) group = (item->parent() ? item->parent() : item)->data(0, Qt::UserRole + 1).toString();
        ConnectionExport::addActions(exportMenu, item && item->parent()
                ? item->data(0, Qt::UserRole).toString() : QString(), group,
            [this](ConnectionExportScope scope, const QString& key) { exportProfiles(scope, key); });
    });
    const auto moreButton = [moreMenu](QWidget* parent) {
        auto* button = new QToolButton(parent);
        button->setText(QStringLiteral("更多"));
        button->setObjectName(QStringLiteral("connectionMoreButton"));
        button->setIcon(InterfaceIcons::icon(QStringLiteral("more")));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setProperty("iconOnlyAction", true);
        button->setProperty("quietAction", true);
        button->setProperty("menuButton", true);
        button->setProperty("instantMenuButton", true);
        button->setAccessibleName(QStringLiteral("更多连接操作"));
        button->setToolTip(QStringLiteral("更多连接操作"));
        button->setPopupMode(QToolButton::InstantPopup);
        button->setMenu(moreMenu);
        return button;
    };
    barLayout->addWidget(new QLabel(QStringLiteral("连接："), profileBar_));
    barLayout->addWidget(profileSelector_, 1);
    barLayout->addWidget(newButton_);
    barLayout->addWidget(moreButton(profileBar_));

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("connectionWorkspace"));
    connect(splitter, &QSplitter::splitterMoved, this, [this]() { updateResponsiveLayout(); });
    profileSidebar_ = new QWidget(splitter);
    profileSidebar_->setObjectName(QStringLiteral("connectionSidebar"));
    auto* side = new QVBoxLayout(profileSidebar_);
    side->setContentsMargins(0, 0, 10, 0);
    profileSearch_ = new QLineEdit(profileSidebar_);
    profileSearch_->setObjectName(QStringLiteral("connectionProfileSearch"));
    SearchField::setPrompt(profileSearch_, QStringLiteral("连接／地址／账号"), QStringLiteral("搜索连接、地址或账号"));
    profileSearch_->setClearButtonEnabled(true);
    side->addWidget(profileSearch_);
    auto* sideActions = new QHBoxLayout();
    auto* create = new QPushButton(QStringLiteral("新建"), profileSidebar_);
    create->setObjectName(QStringLiteral("connectionCreateButton"));
    create->setProperty("quietAction", true);
    create->setToolTip(QStringLiteral("新建连接"));
    create->setAccessibleName(QStringLiteral("新建连接"));
    connect(create, &QPushButton::clicked, newButton_, &QPushButton::click);
    connect(moreMenu, &QMenu::aboutToShow, create, [this, create]() { create->setEnabled(!busy_); });
    sideActions->addWidget(create);
    sideActions->addStretch();
    sideActions->addWidget(moreButton(profileSidebar_));
    side->addLayout(sideActions);
    auto* profileTree = new ConnectionProfileTree(profileSidebar_);
    profileList_ = profileTree;
    profileTree->moveRequested = [this](const ConnectionListMove& move) { reorderProfiles(move); };
    profileList_->setToolTip(QStringLiteral("拖动组调整组顺序；拖动连接调整组内顺序或移动分组。搜索时暂停排序。"));
    profileList_->setObjectName(QStringLiteral("connectionProfileList"));
    profileList_->setColumnCount(2);
    profileList_->setHeaderLabels({QStringLiteral("连接名称"), QStringLiteral("环境")});
    profileList_->setHeaderHidden(true);
    profileList_->header()->setStretchLastSection(false);
    profileList_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    profileList_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    profileList_->setContextMenuPolicy(Qt::CustomContextMenu);
    profileList_->setSelectionMode(QAbstractItemView::SingleSelection);
    side->addWidget(profileList_, 1);
    const auto rememberExpansion = [this](QTreeWidgetItem* item, bool expanded) {
        if (item && !item->parent())
            profileGroupExpansion_.insert(item->data(0, Qt::UserRole + 1).toString(), expanded);
    };
    connect(profileList_, &QTreeWidget::itemExpanded, this,
        [rememberExpansion](QTreeWidgetItem* item) { rememberExpansion(item, true); });
    connect(profileList_, &QTreeWidget::itemCollapsed, this,
        [rememberExpansion](QTreeWidgetItem* item) { rememberExpansion(item, false); });
    connect(profileSearch_, &QLineEdit::textChanged, this, [this]() { refreshProfileList(); });
    connect(profileList_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (!item || busy_) return;
        const QString id = item->data(0, Qt::UserRole).toString();
        if (id.isEmpty()) return;
        for (int index = 0; index < profiles_.size(); ++index)
            if (profiles_[index].id == id) { profileSelector_->setCurrentIndex(index); return; }
    });
    connect(profileList_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        auto* item = profileList_->itemAt(point);
        if (!item || busy_) return;
        const QString id = item->data(0, Qt::UserRole).toString();
        profileList_->setCurrentItem(item);
        if (!item->parent()) {
            QMenu menu(profileList_);
            ConnectionExport::addActions(&menu, {}, item->data(0, Qt::UserRole + 1).toString(),
                [this](ConnectionExportScope scope, const QString& key) { exportProfiles(scope, key); });
            menu.exec(profileList_->viewport()->mapToGlobal(point));
            return;
        }
        if (editingProfileId_ != id) return;
        QMenu menu(profileList_);
        ConnectionExport::addActions(&menu, id, item->parent()->data(0, Qt::UserRole + 1).toString(),
            [this](ConnectionExportScope scope, const QString& key) { exportProfiles(scope, key); });
        menu.addSeparator();
        menu.addAction(InterfaceIcons::icon(QStringLiteral("connect")), QStringLiteral("连接"), connectButton_, &QPushButton::click);
        menu.addAction(InterfaceIcons::icon(QStringLiteral("open")), QStringLiteral("编辑"), this, [this]() { categories_->setCurrentIndex(0); nameEdit_->setFocus(); });
        menu.addAction(InterfaceIcons::icon(QStringLiteral("copy")), QStringLiteral("复制"), copyButton_, &QPushButton::click);
        menu.addAction(InterfaceIcons::icon(QStringLiteral("folder")), QStringLiteral("移动到分组…"), this, [this]() {
            bool accepted = false;
            const QString group = QInputDialog::getText(this, QStringLiteral("移动到分组"),
                QStringLiteral("分组名称；留空移到未分组"), QLineEdit::Normal, groupEdit_->currentText(), &accepted);
            if (accepted) { groupEdit_->setCurrentText(group); (void)saveForm(); }
        });
        menu.addSeparator();
        menu.addAction(InterfaceIcons::icon(QStringLiteral("trash")), QStringLiteral("删除…"), deleteButton_, &QPushButton::click);
        menu.exec(profileList_->viewport()->mapToGlobal(point));
    });

    auto* right = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(8, 0, 0, 0);
    policySummary_ = new QLabel(right);
    policySummary_->setObjectName(QStringLiteral("connectionPolicySummary"));
    policySummary_->setWordWrap(true);
    policySummary_->setTextFormat(Qt::PlainText);
    policySummary_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    rightLayout->addWidget(policySummary_);
    categories_ = new QTabWidget(right);
    categories_->setObjectName(QStringLiteral("connectionCategories"));
    categories_->tabBar()->setUsesScrollButtons(true); categories_->tabBar()->setElideMode(Qt::ElideNone);
    rightLayout->addWidget(categories_, 1);
    const auto addPage = [this](QWidget* page, const QString& label, const QString& name) {
        auto* scroll = new QScrollArea(categories_);
        scroll->setObjectName(name);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setWidget(page);
        categories_->addTab(scroll, label);
    };
    auto* information = new QWidget();
    informationGrid_ = new QGridLayout(information);
    informationGrid_->setContentsMargins(8, 8, 8, 8);
    informationGrid_->setAlignment(Qt::AlignTop);
    informationGrid_->setHorizontalSpacing(12);
    informationGrid_->setVerticalSpacing(6);
    auto* credentialOptions = findChild<QWidget*>(QStringLiteral("connectionCredentialOptions"));
    while (auto* item = basicForm_->takeAt(0)) {
        if (auto* label = qobject_cast<QLabel*>(item->widget())) delete label;
        delete item;
    }
    delete basicForm_; basicForm_ = nullptr;
    const auto fieldBox = [this](QWidget* field, const QString& text) {
        auto* box = new QWidget();
        auto* vertical = new QVBoxLayout(box);
        vertical->setContentsMargins(0, 0, 0, 0);
        vertical->setSpacing(3);
        if (!text.isEmpty()) {
            auto* label = new QLabel(text, box); label->setBuddy(field); label->setWordWrap(true);
            field->setAccessibleName(text);
            vertical->addWidget(label);
        }
        vertical->addWidget(field);
        fieldGroups_.insert(field, box);
        return box;
    };
    const auto basicField = [&](QWidget* field, const QString& text, int row, int column, int span = 1) {
        auto* box = fieldBox(field, text);
        informationFields_.append({box, QRect(column, row, span, 1)});
        informationGrid_->addWidget(box, row, column, 1, span, Qt::AlignTop);
    };
    connectionTypeCombo_->setItemText(connectionTypeCombo_->findData(static_cast<int>(ConnectionType::Direct)), QStringLiteral("直连"));
    connectionTypeCombo_->setItemText(connectionTypeCombo_->findData(static_cast<int>(ConnectionType::EndpointGroup)), QStringLiteral("多地址连接（工具管理，高级）"));
    connectionTypeCombo_->setItemText(connectionTypeCombo_->findData(static_cast<int>(ConnectionType::DmService)), QStringLiteral("集群服务（dm_svc.conf）"));
    connectionTypeCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    connectionTypeCombo_->setMinimumContentsLength(10);
    connect(connectionTypeCombo_, &QComboBox::currentTextChanged, connectionTypeCombo_, &QWidget::setToolTip);
    for (auto* field : {nameEdit_, hostEdit_, serviceAliasEdit_, userEdit_})
        connect(field, &QLineEdit::textChanged, field, &QWidget::setToolTip);
    basicField(nameEdit_, QStringLiteral("名称"), 0, 0);
    basicField(connectionTypeCombo_, QStringLiteral("方式"), 0, 1);
    basicField(environmentCombo_, QStringLiteral("环境"), 1, 0);
    basicField(groupEdit_, QStringLiteral("分组"), 1, 1);
    basicField(hostEdit_, QStringLiteral("主机"), 2, 0);
    basicField(portEdit_, QStringLiteral("端口"), 2, 1);
    basicField(serviceAliasEdit_, QStringLiteral("服务名"), 3, 0);
    basicField(serviceLoginModeCombo_, QStringLiteral("目标"), 3, 1);
    basicField(userEdit_, QStringLiteral("账号"), 4, 0);
    basicField(passwordEdit_, QStringLiteral("密码"), 4, 1);
    for (const auto& entry : QList<QPair<QWidget*, QString>>{
            {nameEdit_, QStringLiteral("连接名称")}, {connectionTypeCombo_, QStringLiteral("连接方式")},
            {groupEdit_, QStringLiteral("导航分组")}, {serviceAliasEdit_, QStringLiteral("服务名（Service Name）")},
            {serviceLoginModeCombo_, QStringLiteral("登录目标（LOGIN_MODE）")}, {userEdit_, QStringLiteral("数据库用户名")}}) {
        entry.first->setAccessibleName(entry.second);
        for (auto* label : fieldGroups_.value(entry.first)->findChildren<QLabel*>())
            label->setToolTip(entry.second);
    }

    endpointPanel_ = new QWidget();
    endpointPanel_->setObjectName(QStringLiteral("connectionEndpointPanel"));
    auto* nodeLayout = new QVBoxLayout(endpointPanel_); nodeLayout->setContentsMargins(0, 0, 0, 0);
    auto* nodeActions = new WrappingControlLayout(nullptr);
    nodeActions->setContentsMargins(0, 0, 0, 0);
    nodeActions->setSpacing(4);
    auto* nodesLabel = new QLabel(QStringLiteral("节点"), endpointPanel_);
    nodesLabel->setObjectName(QStringLiteral("connectionNodesLabel"));
    nodesLabel->setToolTip(QStringLiteral("节点地址"));
    nodeActions->addWidget(nodesLabel);
    const auto nodeButton = [&](const QString& text, const QString& name, auto callback) {
        auto* button = new QPushButton(text, endpointPanel_); button->setObjectName(name);
        button->setProperty("quietAction", true);
        connect(button, &QPushButton::clicked, this, callback); nodeActions->addWidget(button);
        return button;
    };
    endpointTable_ = new QTableWidget(0, 5, endpointPanel_);
    endpointTable_->setObjectName(QStringLiteral("connectionEndpointTable"));
    endpointTable_->setAccessibleName(QStringLiteral("节点地址"));
    endpointTable_->setHorizontalHeaderLabels({QStringLiteral("主机"), QStringLiteral("端口"), QStringLiteral("优先级"), QStringLiteral("启用"), QStringLiteral("说明")});
    endpointTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    endpointTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    endpointTable_->setWordWrap(false);
    endpointTable_->verticalHeader()->hide();
    endpointTable_->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    endpointTable_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    endpointTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column) endpointTable_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    connect(endpointTable_->verticalHeader(), &QHeaderView::geometriesChanged,
        this, &ConnectionDialog::updateEndpointTableHeight);
    connect(endpointTable_->horizontalHeader(), &QHeaderView::geometriesChanged,
        this, &ConnectionDialog::updateEndpointTableHeight);
    nodeButton(QStringLiteral("添加"), QStringLiteral("addConnectionEndpoint"), [this]() {
        auto endpoints = endpointDrafts_.value(draftMode_);
        endpoints.append({{}, 5236, 100, true, {}}); writeEndpointTable(endpoints); readEndpointTable();
        endpointTable_->setCurrentCell(endpointTable_->rowCount() - 1, 0);
    });
    nodeButton(QStringLiteral("移除"), QStringLiteral("removeConnectionEndpoint"), [this]() {
        if (endpointTable_->currentRow() >= 0) { endpointTable_->removeRow(endpointTable_->currentRow()); readEndpointTable(); }
    });
    nodeButton(QStringLiteral("上移"), QStringLiteral("moveConnectionEndpointUp"), [this]() { moveEndpointRow(-1); });
    nodeButton(QStringLiteral("下移"), QStringLiteral("moveConnectionEndpointDown"), [this]() { moveEndpointRow(1); });
    nodeButton(QStringLiteral("批量粘贴…"), QStringLiteral("pasteConnectionEndpoints"), [this]() {
        QDialog paste(this); paste.setWindowTitle(QStringLiteral("批量填写地址")); paste.resize(560, 360);
        auto* layout = new QVBoxLayout(&paste);
        auto* text = new QPlainTextEdit(&paste); text->setPlainText(endpointsEdit_->toPlainText());
        text->setPlaceholderText(endpointsEdit_->placeholderText()); layout->addWidget(text);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &paste);
        connect(buttons, &QDialogButtonBox::accepted, &paste, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &paste, &QDialog::reject); layout->addWidget(buttons);
        if (paste.exec() == QDialog::Accepted) endpointsEdit_->setPlainText(text->toPlainText());
    });
    copyServiceConfigButton_ = nodeButton(QStringLiteral("复制配置"),
        QStringLiteral("copyConnectionServiceConfig"), [this]() { copyServiceConfig(); });
    copyServiceConfigButton_->setToolTip(QStringLiteral("复制当前表单的 dm_svc.conf 配置内容"));
    copyServiceConfigButton_->setAccessibleName(QStringLiteral("复制 dm_svc.conf 配置内容"));
    nodeLayout->addLayout(nodeActions); nodeLayout->addWidget(endpointTable_);
    while (auto* item = credentialOptions->layout()->takeAt(0)) delete item;
    delete credentialOptions->layout();
    auto* credentialLayout = new WrappingControlLayout(credentialOptions);
    for (QWidget* control : {static_cast<QWidget*>(showPassword_), static_cast<QWidget*>(savePassword_), static_cast<QWidget*>(removePasswordButton_)}) credentialLayout->addWidget(control);
    basicField(credentialOptions, {}, 5, 0, 2);
    basicField(endpointPanel_, {}, 6, 0, 2);
    addPage(information, QStringLiteral("信息"), QStringLiteral("connectionFormScroll"));

    policyPage_ = new QWidget();
    auto* policyLayout = new QVBoxLayout(policyPage_);
    policyLayout->setContentsMargins(8, 12, 8, 12);
    const auto removeFormRow = [](QFormLayout* form, QWidget* field) {
        const auto row = form->takeRow(field);
        if (row.labelItem) { delete row.labelItem->widget(); delete row.labelItem; }
        delete row.fieldItem;
    };
    removeFormRow(advancedForm_, topologyCombo_); removeFormRow(advancedForm_, selectionPolicyCombo_);
    policyForm_ = new QFormLayout();
    policyForm_->setRowWrapPolicy(QFormLayout::WrapLongRows);
    policyForm_->addRow(QStringLiteral("拓扑"), topologyCombo_);
    policyForm_->addRow(QStringLiteral("地址选择策略"), selectionPolicyCombo_);
    policyLayout->addLayout(policyForm_);
    for (auto* label : servicePanel_->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly)) delete label;
    while (auto* item = servicePanel_->layout()->takeAt(0)) delete item;
    delete servicePanel_->layout();
    serviceGrid_ = new QGridLayout(servicePanel_); serviceGrid_->setContentsMargins(0, 0, 0, 0);
    serviceGrid_->setAlignment(Qt::AlignTop);
    serviceGrid_->setHorizontalSpacing(16); serviceGrid_->setVerticalSpacing(10);
    const QVector<QPair<QWidget*, QString>> serviceFields{
        {serviceEpSelectorEdit_, QStringLiteral("节点选择（EP_SELECTOR）")}, {serviceAutoReconnectCombo_, QStringLiteral("故障恢复（AUTO_RECONNECT）")},
        {serviceSwitchIntervalEdit_, QStringLiteral("切换间隔（SWITCH_INTERVAL）")}, {serviceSwitchTimesEdit_, QStringLiteral("切换次数（SWITCH_TIMES）")}};
    for (int index = 0; index < serviceFields.size(); ++index) {
        auto* box = fieldBox(serviceFields[index].first, serviceFields[index].second);
        serviceFields_.append({box, QRect(index % 2, index / 2, 1, 1)});
        serviceGrid_->addWidget(box, index / 2, index % 2, Qt::AlignTop);
    }
    policyLayout->addWidget(servicePanel_); policyLayout->addStretch();
    addPage(policyPage_, QStringLiteral("策略"), QStringLiteral("connectionPolicyScroll"));

    serviceConfigPathEdit_->setParent(advancedPanel_);
    auto* pathRow = new QWidget(advancedPanel_); auto* pathLayout = new WrappingControlLayout(pathRow);
    pathLayout->setContentsMargins(0, 0, 0, 0); pathLayout->addWidget(serviceConfigPathEdit_);
    auto* copyPath = new QPushButton(QStringLiteral("复制路径"), pathRow);
    connect(copyPath, &QPushButton::clicked, this, [this]() { QApplication::clipboard()->setText(serviceConfigPathEdit_->text()); });
    auto* openPath = new QPushButton(QStringLiteral("打开目录"), pathRow);
    connect(openPath, &QPushButton::clicked, this, [this]() {
        if (!serviceConfigPathEdit_->text().isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(serviceConfigPathEdit_->text()).absolutePath()));
    });
    pathLayout->addWidget(copyPath); pathLayout->addWidget(openPath);
    pathRow->setObjectName(QStringLiteral("connectionServicePathRow"));
    auto* pathCaption = new QLabel(QStringLiteral("配置文件（dm_svc.conf）"), advancedPanel_);
    pathCaption->setBuddy(serviceConfigPathEdit_); advancedForm_->addRow(pathCaption, pathRow);
    advancedForm_->setRowWrapPolicy(QFormLayout::WrapLongRows);
    advancedForm_->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto* compatibility = new QWidget(advancedPanel_); auto* compatibilityForm = new QFormLayout(compatibility);
    for (const auto& field : {qMakePair(static_cast<QWidget*>(databaseEdit_), QStringLiteral("数据库")),
            qMakePair(static_cast<QWidget*>(driverCombo_), QStringLiteral("驱动标识"))}) {
        removeFormRow(advancedForm_, field.first); compatibilityForm->addRow(field.second, field.first);
    }
    auto* compatibilityToggle = new QToolButton(advancedPanel_);
    compatibilityToggle->setText(QStringLiteral("兼容设置")); compatibilityToggle->setCheckable(true);
    compatibilityToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); compatibilityToggle->setArrowType(Qt::RightArrow);
    connect(compatibilityToggle, &QToolButton::toggled, compatibility, [compatibility, compatibilityToggle](bool checked) {
        compatibility->setVisible(checked); compatibilityToggle->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
    });
    advancedForm_->addRow(compatibilityToggle); advancedForm_->addRow(compatibility); compatibility->hide();
    advancedPanel_->show();
    addPage(advancedPanel_, QStringLiteral("高级"), QStringLiteral("connectionAdvancedScroll"));
    categories_->setTabToolTip(0, QStringLiteral("连接信息"));
    categories_->setTabToolTip(1, QStringLiteral("连接策略"));
    categories_->setTabToolTip(2, QStringLiteral("高级设置与校验"));
    advancedToggle_->setParent(this); advancedToggle_->hide();
    endpointsEdit_->setParent(this); endpointsEdit_->hide();
    delete oldScroll;
    splitter->setStretchFactor(0, 0); splitter->setStretchFactor(1, 1); splitter->setSizes({230, 700});
    root->insertWidget(0, splitter, 1); root->insertWidget(0, profileBar_);
    connect(endpointTable_, &QTableWidget::itemChanged, this, [this]() { readEndpointTable(); });
    connect(endpointsEdit_, &QPlainTextEdit::textChanged, this, [this]() {
        if (!syncingEndpoints_) { writeEndpointTable(parseEndpointLines(endpointsEdit_->toPlainText(),
            connectionTypeCombo_->currentData().toInt() == static_cast<int>(ConnectionType::DmService))); markDraftChanged(); }
    });
}

void ConnectionDialog::refreshProfileList() {
    if (!profileList_) return;
    const int verticalPosition = profileList_->verticalScrollBar()->value();
    const int horizontalPosition = profileList_->horizontalScrollBar()->value();
    const QSignalBlocker blocker(profileList_);
    profileList_->clear();
    QHash<QString, QTreeWidgetItem*> groups;
    const QString query = profileSearch_->text().trimmed();
    profileList_->setDragEnabled(query.isEmpty() && !busy_);
    profileList_->setAcceptDrops(query.isEmpty() && !busy_);
    QVector<const ConnectionProfile*> matches;
    for (const auto& profile : profiles_) {
        QString searchable = profile.name + u' ' + profile.host + u' ' + profile.user + u' ' + profile.group;
        for (const auto& endpoint : profile.endpoints) searchable += u' ' + endpoint.host;
        if (!query.isEmpty() && !searchable.contains(query, Qt::CaseInsensitive)) continue;
        matches.append(&profile);
    }
    const QStringList groupNames = ConnectionStore::navigationGroups(profiles_);
    for (const auto& name : groupNames) {
        if (std::none_of(matches.cbegin(), matches.cend(), [&name](const auto* profile) {
                return profile->group.compare(name, Qt::CaseInsensitive) == 0;
            })) continue;
        auto* parent = new QTreeWidgetItem(profileList_, {name.isEmpty() ? QStringLiteral("未分组") : name});
        parent->setData(0, Qt::UserRole + 1, name);
        parent->setFlags(parent->flags() | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        parent->setExpanded(!query.isEmpty() || profileGroupExpansion_.value(name, true));
        groups.insert(name, parent);
        parent->setFirstColumnSpanned(true);
    }
    for (const auto* match : matches) {
        const auto& profile = *match;
        QString searchable = profile.name + u' ' + profile.host + u' ' + profile.user + u' ' + profile.group;
        for (const auto& endpoint : profile.endpoints) searchable += u' ' + endpoint.host;
        const auto canonical = std::find_if(groupNames.cbegin(), groupNames.cend(), [&](const QString& name) {
            return name.compare(profile.group, Qt::CaseInsensitive) == 0;
        });
        auto* item = new QTreeWidgetItem(groups[*canonical], {profile.name, connectionEnvironmentLabel(profile.environment)});
        item->setFlags((item->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
        item->setData(0, Qt::UserRole, profile.id); item->setToolTip(0, searchable);
        item->setToolTip(1, connectionEnvironmentLabel(profile.environment));
    }
    syncProfileSelection();
    profileList_->doItemsLayout();
    profileList_->verticalScrollBar()->setValue(verticalPosition);
    profileList_->horizontalScrollBar()->setValue(horizontalPosition);
}

void ConnectionDialog::syncProfileSelection() {
    if (!profileList_) return;
    const QSignalBlocker blocker(profileList_);
    QTreeWidgetItem* selected = nullptr;
    for (int group = 0; group < profileList_->topLevelItemCount() && !selected; ++group) {
        auto* parent = profileList_->topLevelItem(group);
        for (int row = 0; row < parent->childCount(); ++row) {
            auto* item = parent->child(row);
            if (item->data(0, Qt::UserRole).toString() == editingProfileId_) {
                selected = item;
                break;
            }
        }
    }
    // Keep the item and model alive while the view handles a mouse press/release.
    const bool autoScroll = profileList_->hasAutoScroll();
    profileList_->setAutoScroll(false);
    if (profileList_->currentItem() != selected || (selected && !selected->isSelected()))
        profileList_->setCurrentItem(selected);
    if (!selected) profileList_->clearSelection();
    profileList_->setAutoScroll(autoScroll);
}

void ConnectionDialog::markDraftChanged() {
    if (loadingForm_) return;
    dirty_ = true; updatePolicySummary();
    const bool conflict = connectionTypeCombo_->currentData().toInt() == static_cast<int>(ConnectionType::DmService)
        && serviceAutoReconnectCombo_->currentIndex() == 2 && serviceNumber(serviceEpSelectorEdit_) == 0;
    if (conflict) {
        statusLabel_->setProperty("servicePolicyConflict", true);
        setStatusMessage(QStringLiteral("切换并回切要求优先选择一个节点（EP_SELECTOR > 0），请修改节点选择。"), QStringLiteral("danger"));
    } else if (statusLabel_->property("servicePolicyConflict").toBool()) {
        statusLabel_->setProperty("servicePolicyConflict", false);
        setStatusMessage(QStringLiteral("连接参数已修改，保存或测试后生效。"));
    }
}

bool ConnectionDialog::confirmUnsavedChanges() {
    if (!dirty_ || loadingForm_) return true;
    const auto choice = MessagePrompt::show(QMessageBox::Question, this, QStringLiteral("连接参数尚未保存"),
        QStringLiteral("是否保存当前连接参数？密码仅在验证成功后按所选设置保存。"),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Cancel,
        QStringLiteral("保存参数"), QStringLiteral("放弃修改"), QStringLiteral("取消切换"));
    if (choice == QMessageBox::Cancel) return false;
    return choice == QMessageBox::No || saveForm(false);
}

void ConnectionDialog::reject() {
    if (busy_ || confirmUnsavedChanges()) QDialog::reject();
}

void ConnectionDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event); updateResponsiveLayout();
    QTimer::singleShot(0, this, [this]() { updateResponsiveLayout(); });
}

void ConnectionDialog::updateResponsiveLayout() {
    if (!categories_) return;
    const bool wide = width() >= 880;
    profileSidebar_->setVisible(wide); profileBar_->setVisible(!wide);
    const bool two = categories_->width() >= 560;
    const auto arrange = [two](QGridLayout* grid, const QVector<QPair<QWidget*, QRect>>& fields) {
        while (auto* item = grid->takeAt(0)) delete item;
        int row = 0;
        QHash<int, int> captionHeights;
        QVector<QPair<QLabel*, int>> captions;
        const auto margins = grid->contentsMargins();
        const int available = grid->parentWidget()->contentsRect().width() - margins.left() - margins.right();
        for (const auto& field : fields) {
            const QRect p = field.second;
            const int fieldRow = two ? p.y() : row++;
            grid->addWidget(field.first, fieldRow, two ? p.x() : 0,
                1, two ? p.width() : 1, Qt::AlignTop);
            auto* caption = field.first->findChild<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
            if (!caption || field.first->isHidden()) continue;
            const int width = qMax(1, two && p.width() == 1 ? (available - grid->horizontalSpacing()) / 2 : available);
            const int height = qMax(caption->fontMetrics().height(), caption->heightForWidth(width));
            captionHeights[fieldRow] = qMax(captionHeights.value(fieldRow), height);
            captions.append({caption, fieldRow});
        }
        for (const auto& caption : captions) caption.first->setFixedHeight(captionHeights.value(caption.second));
        grid->setColumnStretch(0, 1); grid->setColumnStretch(1, two ? 1 : 0);
    };
    arrange(informationGrid_, informationFields_); arrange(serviceGrid_, serviceFields_);
    updateEndpointTableHeight();
}

void ConnectionDialog::updatePolicySummary() {
    if (!policySummary_) return;
    const bool service = connectionTypeCombo_->currentData().toInt() == static_cast<int>(ConnectionType::DmService);
    QString text = connectionTypeCombo_->currentText();
    if (service) text = QStringLiteral("%1 · %2 · %3 · %4 个节点")
        .arg(QStringLiteral("集群服务"), serviceLoginModeCombo_->currentText(),
            serviceAutoReconnectCombo_->currentText()).arg(endpointTable_->rowCount());
    if (readOnly_->isChecked()) text += QStringLiteral(" · 只读");
    policySummary_->setText(text);
    policySummary_->setToolTip(text);
}

void ConnectionDialog::updateEndpointTableHeight() {
    if (!endpointTable_) return;
    const int rows = qMin(4, endpointTable_->rowCount());
    int height = qMax(endpointTable_->horizontalHeader()->height(),
        endpointTable_->horizontalHeader()->sizeHint().height()) + 2 * endpointTable_->frameWidth();
    for (int row = 0; row < rows; ++row) height += endpointTable_->rowHeight(row);
    if (rows == 0) height += endpointTable_->verticalHeader()->defaultSectionSize();
    // Reserve the horizontal scrollbar without changing height when it appears.
    height += endpointTable_->horizontalScrollBar()->sizeHint().height();
    if (endpointTable_->height() != height || endpointTable_->maximumHeight() != height)
        endpointTable_->setFixedHeight(height);
}

void ConnectionDialog::writeEndpointTable(const QVector<ConnectionEndpoint>& endpoints) {
    if (!endpointTable_) return;
    const bool previous = syncingEndpoints_; syncingEndpoints_ = true;
    const QSignalBlocker blocker(endpointTable_);
    endpointTable_->setRowCount(endpoints.size());
    for (int row = 0; row < endpoints.size(); ++row) {
        const auto& endpoint = endpoints[row];
        const QStringList values{endpoint.host, QString::number(endpoint.port), QString::number(endpoint.priority), {}, endpoint.description};
        for (int column = 0; column < values.size(); ++column) {
            auto* item = new QTableWidgetItem(values[column]);
            item->setToolTip(values[column]);
            if (column == 3) { item->setFlags(item->flags() & ~Qt::ItemIsEditable); item->setCheckState(endpoint.enabled ? Qt::Checked : Qt::Unchecked); }
            endpointTable_->setItem(row, column, item);
        }
    }
    endpointDrafts_.insert(draftMode_, endpoints);
    syncingEndpoints_ = previous;
    updateEndpointTableHeight();
}

void ConnectionDialog::readEndpointTable() {
    if (syncingEndpoints_ || !endpointTable_) return;
    const QSignalBlocker tableBlocker(endpointTable_);
    QVector<ConnectionEndpoint> endpoints;
    const auto text = [this](int row, int column) { auto* item = endpointTable_->item(row, column); return item ? item->text().trimmed() : QString(); };
    for (int row = 0; row < endpointTable_->rowCount(); ++row) {
        for (int column = 0; column < endpointTable_->columnCount(); ++column)
            if (auto* item = endpointTable_->item(row, column)) item->setToolTip(item->text());
        QString host = text(row, 0);
        if (host.startsWith(u'[') && host.endsWith(u']')) host = host.mid(1, host.size() - 2);
        const auto* enabled = endpointTable_->item(row, 3);
        bool validPriority = false;
        const int priority = text(row, 2).toInt(&validPriority);
        endpoints.append({host, text(row, 1).toInt(), validPriority ? priority : -1, enabled && enabled->checkState() == Qt::Checked, text(row, 4)});
    }
    endpointDrafts_.insert(draftMode_, endpoints);
    const QSignalBlocker blocker(endpointsEdit_);
    endpointsEdit_->setPlainText(formatEndpointLines(endpoints, draftMode_ == static_cast<int>(ConnectionType::DmService)));
    const int value = serviceNumber(serviceEpSelectorEdit_);
    const QString raw = serviceEpSelectorEdit_->currentText();
    const QSignalBlocker selectorBlocker(serviceEpSelectorEdit_);
    serviceEpSelectorEdit_->clear(); serviceEpSelectorEdit_->addItem(QStringLiteral("0－轮询节点"), 0);
    for (int node = 1; node <= qMax(1, static_cast<int>(endpoints.size())); ++node)
        serviceEpSelectorEdit_->addItem(QStringLiteral("%1－优先节点 %1").arg(node), node);
    if (value >= 0) setServiceNumber(serviceEpSelectorEdit_, value); else serviceEpSelectorEdit_->setEditText(raw);
    markDraftChanged();
    updateEndpointTableHeight();
}

void ConnectionDialog::moveEndpointRow(int offset) {
    const int from = endpointTable_->currentRow(), to = from + offset;
    auto endpoints = endpointDrafts_.value(draftMode_);
    if (from < 0 || to < 0 || from >= endpoints.size() || to >= endpoints.size()) return;
    endpoints.swapItemsAt(from, to); writeEndpointTable(endpoints); readEndpointTable(); endpointTable_->setCurrentCell(to, 0);
}

void ConnectionDialog::updateConnectionTypeControls() {
    const auto type = static_cast<ConnectionType>(connectionTypeCombo_->currentData().toInt());
    const bool service = type == ConnectionType::DmService;
    const bool group = type == ConnectionType::EndpointGroup;
    if (!loadingForm_ && draftMode_ != static_cast<int>(type)) {
        const auto current = endpointDrafts_.value(draftMode_);
        readOnlyDrafts_.insert(draftMode_, readOnly_->isChecked());
        routingDrafts_.insert(draftMode_, {topologyCombo_->currentData().toInt(), requiredRoleCombo_->currentData().toInt(), selectionPolicyCombo_->currentData().toInt()});
        draftMode_ = static_cast<int>(type);
        if (readOnlyDrafts_.contains(draftMode_)) readOnly_->setChecked(readOnlyDrafts_[draftMode_]);
        if (routingDrafts_.contains(draftMode_)) {
            const auto values = routingDrafts_[draftMode_];
            topologyCombo_->setCurrentIndex(topologyCombo_->findData(values[0]));
            requiredRoleCombo_->setCurrentIndex(requiredRoleCombo_->findData(values[1]));
            selectionPolicyCombo_->setCurrentIndex(selectionPolicyCombo_->findData(values[2]));
        }
        if (!endpointDrafts_.contains(draftMode_)) endpointDrafts_.insert(draftMode_, current);
        writeEndpointTable(endpointDrafts_.value(draftMode_));
    }
    for (auto* field : {static_cast<QWidget*>(hostEdit_), static_cast<QWidget*>(portEdit_)}) fieldGroups_.value(field)->setVisible(type == ConnectionType::Direct);
    fieldGroups_.value(endpointPanel_)->setVisible(group || service);
    fieldGroups_.value(serviceAliasEdit_)->setVisible(service);
    fieldGroups_.value(serviceLoginModeCombo_)->setVisible(service);
    hostEdit_->setEnabled(type == ConnectionType::Direct);
    portEdit_->setEnabled(type == ConnectionType::Direct);
    endpointsEdit_->setEnabled(group || service);
    servicePanel_->setVisible(service);
    policyForm_->setRowVisible(topologyCombo_, group);
    policyForm_->setRowVisible(selectionPolicyCombo_, group);
    advancedForm_->setRowVisible(requiredRoleCombo_, !service);
    advancedForm_->setRowVisible(findChild<QWidget*>(QStringLiteral("connectionServicePathRow")), service);
    topologyCombo_->setEnabled(group);
    selectionPolicyCombo_->setEnabled(group);
    readOnly_->setEnabled(!busy_);
    endpointsEdit_->setPlaceholderText(service
        ? QStringLiteral("每行一个节点：host:port\nIPv6：[2001:db8::1]:5236；按填写顺序选择")
        : QStringLiteral("每行一个端点：host:port,priority\nIPv6：[2001:db8::1]:5236,100"));
    testButton_->setAccessibleName(group ? QStringLiteral("测试全部端点") : QStringLiteral("测试连接"));
    testButton_->setToolTip(group ? QStringLiteral("测试全部端点") : QStringLiteral("测试连接"));
    copyServiceConfigButton_->setVisible(service);
    copyServiceConfigButton_->setEnabled(service && !busy_);
    categories_->setTabVisible(1, group || service);
    endpointTable_->setColumnHidden(2, service); endpointTable_->setColumnHidden(3, service); endpointTable_->setColumnHidden(4, service);
    updatePolicySummary(); updateResponsiveLayout();
}

void ConnectionDialog::loadProfiles(const QString& selectedProfileId) {
    loadingForm_ = true;
    QString error;
    profiles_ = store_.load(&error);
    {
        const QSignalBlocker blocker(profileSelector_);
        profileSelector_->clear();
        for (const ConnectionProfile& profile : profiles_) {
            profileSelector_->addItem(profile.group.isEmpty()
                ? profile.name : QStringLiteral("[%1] %2").arg(profile.group, profile.name));
        }
        int selectedIndex = 0;
        if (!selectedProfileId.isEmpty()) {
            const auto selected = std::find_if(profiles_.cbegin(), profiles_.cend(),
                [&selectedProfileId](const ConnectionProfile& profile) {
                    return profile.id == selectedProfileId;
                });
            if (selected != profiles_.cend()) {
                selectedIndex = static_cast<int>(std::distance(profiles_.cbegin(), selected));
            }
        }
        profileSelector_->setCurrentIndex(profiles_.isEmpty() ? -1 : selectedIndex);
    }
    refreshGroupChoices();
    if (!error.isEmpty()) {
        setStatusMessage(error + QStringLiteral(" 请修复配置文件后重试。"));
    }
    if (profiles_.isEmpty()) {
        startNewProfile();
    } else {
        loadSelectedProfile();
    }
    loadingForm_ = false; dirty_ = false; refreshProfileList();
}

void ConnectionDialog::refreshGroupChoices(const QString& selectedGroup) {
    const QString current = selectedGroup.isNull()
        ? groupEdit_->currentText() : selectedGroup;
    QStringList groups;
    for (const ConnectionProfile& profile : std::as_const(profiles_)) {
        const QString group = normalizedConnectionNavigationGroup(profile.group);
        if (group.isEmpty()) continue;
        const auto existing = std::find_if(groups.cbegin(), groups.cend(),
            [&group](const QString& item) {
                return item.compare(group, Qt::CaseInsensitive) == 0;
            });
        if (existing == groups.cend()) groups.append(group);
    }
    std::sort(groups.begin(), groups.end(), [](const QString& left, const QString& right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    const QSignalBlocker blocker(groupEdit_);
    groupEdit_->clear();
    groupEdit_->addItem(QString());
    groupEdit_->addItems(groups);
    groupEdit_->setCurrentText(current);
}

void ConnectionDialog::loadSelectedProfile() {
    ++credentialLoadGeneration_;
    const int index = profileSelector_->currentIndex();
    if (index < 0 || index >= profiles_.size()) {
        return;
    }
    loadingForm_ = true;
    requireNewPassword_ = false;
    sessionCredentialUnsaved_ = false;
    statusLabel_->setProperty("servicePolicyConflict", false);
    const ConnectionProfile& profile = profiles_[index];
    originalName_ = profile.name;
    editingProfileId_ = draftId_ = profile.id;
    endpointDrafts_.clear(); readOnlyDrafts_.clear(); routingDrafts_.clear(); draftMode_ = static_cast<int>(profile.connectionType);
    nameEdit_->setText(profile.name);
    connectionTypeCombo_->setCurrentIndex(connectionTypeCombo_->findData(
        static_cast<int>(profile.connectionType)));
    advancedToggle_->setChecked(false);
    categories_->setCurrentIndex(0);
    hostEdit_->setText(profile.host);
    portEdit_->setValue(profile.port);
    endpointsEdit_->setPlainText(formatEndpointLines(profile.endpoints,
        profile.connectionType == ConnectionType::DmService));
    writeEndpointTable(profile.endpoints);
    serviceAliasEdit_->setText(profile.serviceAlias);
    serviceLoginModeCombo_->setCurrentIndex(profile.serviceLoginMode);
    setServiceNumber(serviceEpSelectorEdit_, profile.serviceEpSelector);
    serviceAutoReconnectCombo_->setCurrentIndex(profile.serviceAutoReconnect);
    setServiceNumber(serviceSwitchIntervalEdit_, profile.serviceSwitchInterval);
    setServiceNumber(serviceSwitchTimesEdit_, profile.serviceSwitchTimes);
    serviceConfigPathEdit_->setText(profile.connectionType == ConnectionType::DmService
        ? store_.serviceConfigPath(profile) : QString());
    topologyCombo_->setCurrentIndex(topologyCombo_->findData(
        static_cast<int>(profile.topology)));
    requiredRoleCombo_->setCurrentIndex(requiredRoleCombo_->findData(
        static_cast<int>(profile.requiredRole)));
    selectionPolicyCombo_->setCurrentIndex(selectionPolicyCombo_->findData(
        static_cast<int>(profile.selectionPolicy)));
    expectedDatabaseEdit_->setText(profile.expectedDatabaseName);
    expectedOguidEdit_->setText(profile.expectedOguid);
    userEdit_->setText(profile.user);
    refreshGroupChoices(profile.group);
    databaseEdit_->setText(profile.database);
    driverCombo_->setCurrentText(profile.driver);
    sqlTextEncodingCombo_->setCurrentText(reid::text::normalizeSqlTextEncoding(profile.sqlTextEncoding));
    timeoutEdit_->setValue(profile.timeoutSeconds);
    fetchTimeoutEdit_->setValue(profile.fetchTimeoutSeconds);
    cancelTimeoutEdit_->setValue(profile.cancelTimeoutSeconds);
    readOnly_->setChecked(profile.readOnly);
    updateConnectionTypeControls();
    environmentCombo_->setCurrentIndex(environmentCombo_->findData(
        static_cast<int>(profile.environment)));
    passwordEdit_->clear();
    loadedPassword_.fill('\0');
    loadedPassword_.clear();
    descriptionEdit_->setText(profile.description);
    suppressSavePasswordToggle_ = true;
    savePassword_->setChecked(profile.hasStoredCredential);
    suppressSavePasswordToggle_ = false;
    deleteButton_->setEnabled(true);
    copyButton_->setEnabled(true);
    removePasswordButton_->setEnabled(profile.hasStoredCredential);
    if (profile.hasStoredCredential) {
        setStatusMessage(
            QStringLiteral("密码已保存在本机；测试、连接或重连时才会读取。"));
    } else {
        setStatusMessage(credentialStoreAvailable_
                ? QStringLiteral("连接配置未标记已保存密码；测试或连接时将检查本机安全凭据库，也可直接输入密码。")
                : QStringLiteral("系统安全凭据库不可用；密码只用于本次连接。"));
    }
    loadingForm_ = false; dirty_ = false; syncProfileSelection();
}

void ConnectionDialog::startNewProfile() {
    loadingForm_ = true;
    editingProfileId_.clear(); draftId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    statusLabel_->setProperty("servicePolicyConflict", false);
    endpointDrafts_.clear(); readOnlyDrafts_.clear(); routingDrafts_.clear(); draftMode_ = static_cast<int>(ConnectionType::Direct);
    writeEndpointTable({});
    profileSelector_->setCurrentIndex(-1);
    originalName_.clear();
    ++credentialLoadGeneration_;
    nameEdit_->clear();
    connectionTypeCombo_->setCurrentIndex(0);
    advancedToggle_->setChecked(false);
    hostEdit_->clear();
    portEdit_->setValue(5236);
    endpointsEdit_->clear();
    serviceAliasEdit_->setText(QStringLiteral("GRP1_HA"));
    serviceLoginModeCombo_->setCurrentIndex(1);
    setServiceNumber(serviceEpSelectorEdit_, 1);
    serviceAutoReconnectCombo_->setCurrentIndex(1);
    setServiceNumber(serviceSwitchIntervalEdit_, 200);
    setServiceNumber(serviceSwitchTimesEdit_, 3);
    serviceConfigPathEdit_->clear();
    topologyCombo_->setCurrentIndex(0);
    requiredRoleCombo_->setCurrentIndex(0);
    selectionPolicyCombo_->setCurrentIndex(0);
    expectedDatabaseEdit_->clear();
    expectedOguidEdit_->clear();
    userEdit_->clear();
    refreshGroupChoices(QString());
    databaseEdit_->clear();
    driverCombo_->setCurrentText(QStringLiteral("DM8"));
    sqlTextEncodingCombo_->setCurrentText(QStringLiteral("UTF-8"));
    timeoutEdit_->setValue(15);
    fetchTimeoutEdit_->setValue(60);
    cancelTimeoutEdit_->setValue(5);
    readOnly_->setChecked(false);
    environmentCombo_->setCurrentIndex(0);
    passwordEdit_->clear();
    loadedPassword_.fill('\0');
    loadedPassword_.clear();
    descriptionEdit_->clear();
    suppressSavePasswordToggle_ = true;
    savePassword_->setChecked(false);
    suppressSavePasswordToggle_ = false;
    deleteButton_->setEnabled(false);
    copyButton_->setEnabled(false);
    removePasswordButton_->setEnabled(false);
    requireNewPassword_ = false;
    setStatusMessage(QStringLiteral("正在创建新连接配置。"));
    sessionCredentialUnsaved_ = false;
    nameEdit_->setFocus();
    loadingForm_ = false; dirty_ = false; updateConnectionTypeControls(); syncProfileSelection();
}

void ConnectionDialog::startNewProfileInGroup(const QString& group) {
    startNewProfile();
    refreshGroupChoices(normalizedConnectionNavigationGroup(group));
}

bool ConnectionDialog::copySelectedProfile() {
    const int index = profileSelector_->currentIndex();
    if (index < 0 || index >= profiles_.size()) {
        return false;
    }
    const QString profileId = profiles_[index].id;
    QString error;
    auto latest = store_.load(&error);
    const auto source = std::find_if(latest.cbegin(), latest.cend(), [&profileId](const auto& item) { return item.id == profileId; });
    if (!error.isEmpty() || source == latest.cend()) {
        setStatusMessage(error.isEmpty() ? QStringLiteral("原连接配置已不存在，未复制其他连接。") : error);
        return false;
    }
    ConnectionProfile copy = *source;
    const auto previous = profiles_;
    profiles_ = latest;
    copy.id.clear();
    copy.hasStoredCredential = false;
    const QString baseName = copy.name + QStringLiteral(" 副本");
    copy.name = baseName;
    int suffix = 2;
    while (std::any_of(profiles_.cbegin(), profiles_.cend(), [&copy](const auto& item) {
        return item.name.compare(copy.name, Qt::CaseInsensitive) == 0;
    })) {
        copy.name = QStringLiteral("%1 %2").arg(baseName).arg(suffix++);
    }
    copy.ensureId();
    profiles_.push_back(copy);
    if (!store_.save(profiles_, &error)) {
        profiles_ = previous;
        setStatusMessage(error + QStringLiteral(" 连接未复制。"));
        return false;
    }
    loadProfiles(copy.id);
    setStatusMessage(QStringLiteral("连接已复制；密码未复制。"));
    emit profilesChanged();
    return true;
}

bool ConnectionDialog::copySavedProfile(const QString& profileId) {
    if (profileId.isEmpty() || busy_) return false;
    QString error;
    const auto latest = store_.load(&error);
    if (!error.isEmpty()) { setStatusMessage(error, QStringLiteral("danger")); return false; }
    if (std::none_of(latest.cbegin(), latest.cend(), [&profileId](const ConnectionProfile& profile) { return profile.id == profileId; })) {
        setStatusMessage(QStringLiteral("原连接配置已不存在，未复制其他连接。")); return false;
    }
    loadProfiles(profileId);
    if (editingProfileId_ != profileId) { setStatusMessage(QStringLiteral("原连接配置已不存在，未复制其他连接。")); return false; }
    return copySelectedProfile();
}

void ConnectionDialog::importProfiles(bool sqlLark) {
    if (!confirmUnsavedChanges()) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入连接"), {},
        sqlLark ? QStringLiteral("SQLLark 连接 (*.xml)")
                : QStringLiteral("小智达梦管理工具连接 (*.json);;JSON 文件 (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatusMessage(QStringLiteral("无法读取导入文件：%1").arg(file.errorString()));
        return;
    }
    QString error;
    if (sqlLark && file.size() > 16 * 1024 * 1024) {
        setStatusMessage(QStringLiteral("SQLLark 文件超过 16 MiB 限制。"));
        return;
    }
    const QVector<ConnectionProfile> imported = sqlLark
        ? ConnectionStore::importSqlLark(file.read(16 * 1024 * 1024 + 1), &error)
        : ConnectionStore::importPortable(file.readAll(), &error);
    if (!error.isEmpty()) {
        setStatusMessage(error);
        return;
    }
    const auto baseline = store_.load(&error);
    if (!error.isEmpty()) { setStatusMessage(error + QStringLiteral(" 连接未导入。")); return; }
    QVector<ConnectionProfile> merged = baseline;
    int replaced = 0;
    int renamed = 0;
    for (ConnectionProfile profile : imported) {
        if (sqlLark) {
            const QString originalName = profile.name;
            int suffix = 2;
            while (std::any_of(merged.cbegin(), merged.cend(), [&profile](const auto& item) {
                return item.name.compare(profile.name, Qt::CaseInsensitive) == 0;
            })) profile.name = QStringLiteral("%1 (SQLLark %2)").arg(originalName).arg(suffix++);
            if (profile.name != originalName) ++renamed;
        }
        if (!profile.group.isEmpty()) {
            const auto canonical = std::find_if(merged.cbegin(), merged.cend(),
                [&profile](const ConnectionProfile& item) {
                    return item.group.compare(profile.group, Qt::CaseInsensitive) == 0;
                });
            if (canonical != merged.cend()) profile.group = canonical->group;
        }
        const auto existing = std::find_if(merged.begin(), merged.end(), [&profile](const auto& item) {
            return item.name.compare(profile.name, Qt::CaseInsensitive) == 0;
        });
        if (existing == merged.end()) {
            merged.push_back(std::move(profile));
        } else {
            profile.id = existing->id;
            profile.hasStoredCredential = existing->hasStoredCredential;
            *existing = std::move(profile);
            ++replaced;
        }
    }
    if (replaced > 0 && MessagePrompt::show(QMessageBox::Question, this, QStringLiteral("导入连接"),
            QStringLiteral("有 %1 个同名连接。是否用导入参数覆盖，并保留本机已保存密码？")
                .arg(replaced), QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel,
            QStringLiteral("覆盖同名连接"), QStringLiteral("取消导入")) != QMessageBox::Yes) {
        return;
    }
    const auto current = store_.load(&error);
    if (!error.isEmpty() || current != baseline) {
        setStatusMessage(error.isEmpty() ? QStringLiteral("导入期间连接配置已变化，请重新导入；未覆盖修改。") : error);
        return;
    }
    if (!store_.save(merged, &error)) {
        setStatusMessage(error + QStringLiteral(" 连接未导入。"));
        return;
    }
    profiles_ = std::move(merged);
    loadProfiles();
    setStatusMessage(sqlLark
        ? QStringLiteral("已导入 %1 个 SQLLark 连接，%2 个同名连接已重命名；密码未导入，请补填密码。")
            .arg(imported.size()).arg(renamed)
        : QStringLiteral("已导入 %1 个连接；文件中的密码字段不会读取。")
            .arg(imported.size()));
    emit profilesChanged();
}

void ConnectionDialog::exportProfiles(ConnectionExportScope scope, const QString& key) {
    if (busy_) return;
    QString message;
    const bool success = ConnectionExport::save(this, store_.path(), scope, key, &message);
    if (!message.isEmpty()) setStatusMessage(message, success ? QStringLiteral("info") : QStringLiteral("danger"));
}

void ConnectionDialog::reorderProfiles(const ConnectionListMove& move) {
    if (busy_ || !profileSearch_->text().trimmed().isEmpty() || !confirmUnsavedChanges()) return;
    ConnectionStore store(store_.path());
    QString error;
    auto latest = store.load(&error, true);
    if (error.isEmpty() && latest != profiles_)
        error = QStringLiteral("连接配置已被其他窗口修改，请刷新后再排序。排序未保存。");
    if (!error.isEmpty() || !ConnectionStore::reorder(latest, move, &error)
            || !store.save(latest, &error)) {
        setStatusMessage(error, QStringLiteral("danger"));
        return;
    }
    const QString selected = editingProfileId_;
    loadProfiles(selected);
    if (move.profileId.isEmpty()) {
        const QSignalBlocker blocker(profileList_);
        for (int index = 0; index < profileList_->topLevelItemCount(); ++index) {
            auto* group = profileList_->topLevelItem(index);
            if (group->data(0, Qt::UserRole + 1).toString().compare(move.sourceGroup, Qt::CaseInsensitive) == 0)
                profileList_->setCurrentItem(group);
        }
    }
    setStatusMessage(QStringLiteral("连接顺序与分组已保存。"));
    emit profilesChanged();
}

bool ConnectionDialog::copyServiceConfig() {
    if (busy_ || connectionTypeCombo_->currentData().toInt() != static_cast<int>(ConnectionType::DmService))
        return false;
    const auto profile = formProfile();
    const QString error = profile.serviceConfigValidationError();
    if (!error.isEmpty()) {
        setStatusMessage(error, QStringLiteral("warning"));
        return false;
    }
    QApplication::clipboard()->setText(QString::fromUtf8(profile.serviceConfigContent()));
    setStatusMessage(QStringLiteral("已复制 dm_svc.conf 配置。"));
    return true;
}

ConnectionProfile ConnectionDialog::formProfile() {
    ConnectionProfile profile{
        nameEdit_->text().trimmed(),
        hostEdit_->text().trimmed(),
        portEdit_->value(),
        userEdit_->text().trimmed(),
        descriptionEdit_->text().trimmed(),
        static_cast<ConnectionEnvironment>(environmentCombo_->currentData().toInt()),
    };
    const auto existing = std::find_if(profiles_.cbegin(), profiles_.cend(), [this](const auto& item) {
        return item.id == editingProfileId_;
    });
    if (existing != profiles_.cend()) {
        profile.id = existing->id;
        profile.hasStoredCredential = existing->hasStoredCredential;
        profile.executionTimeoutSeconds = existing->executionTimeoutSeconds;
    }
    if (profile.id.isEmpty()) profile.id = draftId_;
    profile.group = normalizedConnectionNavigationGroup(groupEdit_->currentText());
    if (!profile.group.isEmpty()) {
        const auto canonical = std::find_if(profiles_.cbegin(), profiles_.cend(),
            [&profile](const ConnectionProfile& item) {
                return item.group.compare(profile.group, Qt::CaseInsensitive) == 0;
            });
        if (canonical != profiles_.cend()) profile.group = canonical->group;
    }
    profile.database = databaseEdit_->text().trimmed();
    profile.driver = driverCombo_->currentText();
    profile.sqlTextEncoding = sqlTextEncodingCombo_->currentText();
    profile.timeoutSeconds = timeoutEdit_->value();
    profile.fetchTimeoutSeconds = fetchTimeoutEdit_->value();
    profile.cancelTimeoutSeconds = cancelTimeoutEdit_->value();
    profile.readOnly = readOnly_->isChecked();
    profile.connectionType = static_cast<ConnectionType>(
        connectionTypeCombo_->currentData().toInt());
    profile.endpoints = profile.connectionType != ConnectionType::Direct
        ? endpointDrafts_.value(draftMode_) : QVector<ConnectionEndpoint>{};
    if (profile.connectionType == ConnectionType::DmService)
        for (auto& endpoint : profile.endpoints) { endpoint.enabled = true; endpoint.priority = 100; }
    profile.serviceAlias = serviceAliasEdit_->text().trimmed();
    profile.serviceLoginMode = serviceLoginModeCombo_->currentIndex();
    profile.serviceEpSelector = serviceNumber(serviceEpSelectorEdit_);
    profile.serviceAutoReconnect = serviceAutoReconnectCombo_->currentIndex();
    profile.serviceSwitchInterval = serviceNumber(serviceSwitchIntervalEdit_);
    profile.serviceSwitchTimes = serviceNumber(serviceSwitchTimesEdit_);
    profile.topology = static_cast<ConnectionTopology>(
        topologyCombo_->currentData().toInt());
    profile.requiredRole = static_cast<RequiredDatabaseRole>(
        requiredRoleCombo_->currentData().toInt());
    profile.selectionPolicy = static_cast<EndpointSelectionPolicy>(
        selectionPolicyCombo_->currentData().toInt());
    profile.expectedDatabaseName = expectedDatabaseEdit_->text().trimmed();
    profile.expectedOguid = expectedOguidEdit_->text().trimmed();
    profile.ensureId();
    return profile.effectiveProfile();
}

bool ConnectionDialog::validateForm(bool requirePassword) {
    const auto clearDescription = [](QWidget* field) {
        field->setAccessibleDescription(QString());
    };
    clearDescription(endpointTable_);
    clearDescription(nameEdit_);
    clearDescription(hostEdit_);
    clearDescription(portEdit_);
    clearDescription(endpointsEdit_);
    clearDescription(serviceAliasEdit_);
    clearDescription(serviceEpSelectorEdit_);
    clearDescription(serviceSwitchIntervalEdit_);
    clearDescription(serviceSwitchTimesEdit_);
    clearDescription(userEdit_);
    clearDescription(groupEdit_);
    clearDescription(timeoutEdit_);
    clearDescription(fetchTimeoutEdit_);
    clearDescription(cancelTimeoutEdit_);
    clearDescription(readOnly_);
    clearDescription(passwordEdit_);
    const ConnectionProfile profile = formProfile();
    QString error = profile.validationError(profiles_, originalName_);
    if (error.isEmpty()) error = connectionNavigationGroupValidationError(profile.group);
    if (!error.isEmpty()) {
        setStatusMessage(error + QStringLiteral(" 请修改对应字段。"), QStringLiteral("danger"));
        statusLabel_->setAccessibleDescription(error);
        QWidget* target = nameEdit_;
        bool advanced = false;
        if (profile.name.isEmpty()) {
            target = nameEdit_;
        } else if (profile.connectionType == ConnectionType::Direct
                && profile.host.isEmpty()) {
            target = hostEdit_;
        } else if (profile.port < 1 || profile.port > 65'535) {
            target = portEdit_;
        } else if (profile.user.isEmpty()) {
            target = userEdit_;
        } else if (!connectionNavigationGroupValidationError(profile.group).isEmpty()) {
            target = groupEdit_;
        } else if (profile.timeoutSeconds < 1 || profile.timeoutSeconds > 300) {
            target = timeoutEdit_;
            advanced = true;
        } else if (profile.fetchTimeoutSeconds < 1
                || profile.fetchTimeoutSeconds > 86'400) {
            target = fetchTimeoutEdit_;
            advanced = true;
        } else if (profile.cancelTimeoutSeconds < 1
                || profile.cancelTimeoutSeconds > 60) {
            target = cancelTimeoutEdit_;
            advanced = true;
        } else if (profile.requiredRole == RequiredDatabaseRole::StandbyOpen
                && !profile.readOnly) {
            target = readOnly_;
            advanced = true;
        } else if (profile.connectionType == ConnectionType::DmService) {
            if (error.contains(QStringLiteral("服务名"))) target = serviceAliasEdit_;
            else if (profile.serviceEpSelector < 0) target = serviceEpSelectorEdit_;
            else if (profile.serviceSwitchInterval < 1) target = serviceSwitchIntervalEdit_;
            else if (profile.serviceSwitchTimes < 1) target = serviceSwitchTimesEdit_;
            else if (error.contains(QStringLiteral("EP_SELECTOR"))) target = serviceEpSelectorEdit_;
            else target = endpointTable_;
        } else if (profile.connectionType == ConnectionType::EndpointGroup) {
            target = endpointTable_;
        }
        if (advanced) advancedToggle_->setChecked(true);
        for (int index = 0; index < categories_->count(); ++index) {
            auto* scroll = qobject_cast<QScrollArea*>(categories_->widget(index));
            if (scroll && scroll->isAncestorOf(target)) {
                categories_->setCurrentIndex(index); scroll->ensureWidgetVisible(target); break;
            }
        }
        target->setAccessibleDescription(error);
        target->setFocus(Qt::OtherFocusReason);
        return false;
    }
    if (requirePassword && effectivePassword().isEmpty()) {
        const QString passwordError = QStringLiteral(
            "没有可用密码。请输入数据库密码后重试。");
        setStatusMessage(passwordError);
        statusLabel_->setAccessibleDescription(passwordError);
        passwordEdit_->setAccessibleDescription(passwordError);
        passwordEdit_->setFocus(Qt::OtherFocusReason);
        return false;
    }
    return true;
}

bool ConnectionDialog::saveForm(bool requirePassword) {
    QString readError;
    const auto latest = store_.load(&readError);
    if (!readError.isEmpty()) { setStatusMessage(readError + QStringLiteral(" 配置未保存。"), QStringLiteral("danger")); return false; }
    if (!editingProfileId_.isEmpty()) {
        const auto sameId = [this](const ConnectionProfile& item) { return item.id == editingProfileId_; };
        const auto original = std::find_if(profiles_.cbegin(), profiles_.cend(), sameId);
        const auto current = std::find_if(latest.cbegin(), latest.cend(), sameId);
        if (original == profiles_.cend() || current == latest.cend() || !(*original == *current)) {
            setStatusMessage(QStringLiteral("连接配置已被修改或删除，请重新加载后再保存；未覆盖其他修改。"), QStringLiteral("danger"));
            return false;
        }
    }
    if (!validateForm(requirePassword)) {
        return false;
    }
    const ConnectionProfile profile = formProfile();
    const QString validation = profile.validationError(latest, originalName_);
    if (!validation.isEmpty()) { setStatusMessage(validation, QStringLiteral("danger")); return false; }
    const auto previous = profiles_;
    profiles_ = latest;
    const auto existing = std::find_if(profiles_.begin(), profiles_.end(), [this](const auto& item) {
        return item.id == editingProfileId_;
    });
    if (existing == profiles_.end()) {
        profiles_.push_back(profile);
    } else {
        if (existing->group.compare(profile.group, Qt::CaseInsensitive) != 0) {
            QString orderError;
            if (!ConnectionStore::reorder(profiles_, {profile.id, existing->group, profile.group, {}, false}, &orderError)) {
                profiles_ = previous;
                setStatusMessage(orderError, QStringLiteral("danger"));
                return false;
            }
            *std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& item) { return item.id == profile.id; }) = profile;
        } else {
            *existing = profile;
        }
    }
    QString error;
    if (!store_.save(profiles_, &error)) {
        profiles_ = previous;
        setStatusMessage(error + QStringLiteral(" 配置未保存。"));
        return false;
    }
    originalName_ = profile.name;
    editingProfileId_ = draftId_ = profile.id;
    serviceConfigPathEdit_->setText(profile.connectionType == ConnectionType::DmService
        ? store_.serviceConfigPath(profile) : QString());
    const int selected = static_cast<int>(std::distance(profiles_.begin(),
        std::find(profiles_.begin(), profiles_.end(), profile)));
    profileSelector_->blockSignals(true);
    profileSelector_->clear();
    for (const ConnectionProfile& item : profiles_) {
        profileSelector_->addItem(item.group.isEmpty()
            ? item.name : QStringLiteral("[%1] %2").arg(item.group, item.name));
    }
    profileSelector_->setCurrentIndex(selected);
    profileSelector_->blockSignals(false);
    refreshGroupChoices(profile.group);
    deleteButton_->setEnabled(true);
    setStatusMessage(profile.hasStoredCredential
            ? QStringLiteral("连接配置已保存；密码仍保存在本机安全凭据库。")
            : savePassword_->isChecked() && !effectivePassword().isEmpty()
                ? QStringLiteral("连接配置已保存；密码将在测试或连接验证成功后安全保存。")
                : QStringLiteral("连接配置已保存；未保存密码。"));
    dirty_ = false;
    // Save-and-switch can run inside the tree's mouse handler. Rebuild only once
    // that event has finished, using the latest profiles and selected identity.
    QTimer::singleShot(0, this, &ConnectionDialog::refreshProfileList);
    emit profilesChanged();
    return true;
}

QByteArray ConnectionDialog::effectivePassword() const {
    if (!passwordEdit_->text().isEmpty()) {
        return passwordEdit_->text().toUtf8();
    }
    return requireNewPassword_ ? QByteArray{} : loadedPassword_;
}

void ConnectionDialog::testConnection(bool connectAfterSuccess) {
    if (!validateForm(false)) {
        return;
    }
    const ConnectionProfile profile = formProfile();
    QByteArray password = effectivePassword();
    if (password.isEmpty() && !editingProfileId_.isEmpty() && !requireNewPassword_
            && credentialStoreAvailable_) {
        const int generation = ++credentialLoadGeneration_;
        setBusy(true, QStringLiteral("正在从本机安全凭据库读取密码…"));
        auto* credentialWatcher = new QFutureWatcher<CredentialResult>(this);
        connect(credentialWatcher, &QFutureWatcher<CredentialResult>::finished, this,
            [this, credentialWatcher, generation, connectAfterSuccess]() {
            CredentialResult result = credentialWatcher->result();
            credentialWatcher->deleteLater();
            if (generation != credentialLoadGeneration_) {
                result.password.fill('\0');
                return;
            }
            if (!result.succeeded()) {
                result.password.fill('\0');
                const QString status = result.status == CredentialStatus::NotFound
                    ? QStringLiteral("本机安全凭据库中未找到此连接的密码。")
                    : result.status == CredentialStatus::Unavailable
                        ? QStringLiteral("本机安全凭据库不可用，不能确认密码是否已保存。")
                        : QStringLiteral("读取本机密码失败，不能判定为未保存。");
                setBusy(false, status + QStringLiteral(" 请输入密码后重试。"));
                return;
            }
            if (result.password.isEmpty()) {
                setBusy(false, QStringLiteral("本机凭据为空，请输入数据库密码后重试。"));
                return;
            }
            const ConnectionProfile current = formProfile();
            if (!current.hasStoredCredential && !updateStoredCredentialFlag(current.id, true)) {
                result.password.fill('\0');
                setBusy(false, QStringLiteral("密码读取成功，但连接配置状态同步失败。请重新打开连接管理后重试；已保存密码未删除。"));
                return;
            }
            suppressSavePasswordToggle_ = true;
            savePassword_->setChecked(true);
            suppressSavePasswordToggle_ = false;
            loadedPassword_ = std::move(result.password);
            sessionCredentialUnsaved_ = false;
            setBusy(false, QStringLiteral("已读取本机安全凭据，正在测试连接…"));
            testConnection(connectAfterSuccess);
        });
        credentialWatcher->setFuture(QtConcurrent::run(
            [store = credentialStore_, profileId = profile.id]() {
                return store->read(profileId);
            }));
        return;
    }
    if (password.isEmpty()) {
        setStatusMessage(QStringLiteral("没有可用密码。请输入数据库密码后重试。"));
        passwordEdit_->setFocus();
        return;
    }
    setBusy(true, connectAfterSuccess
            ? QStringLiteral("正在验证连接…")
            : QStringLiteral("正在测试连接…"));
    auto* watcher = new QFutureWatcher<ConnectionTestReport>(this);
    connect(watcher, &QFutureWatcher<ConnectionTestReport>::finished, this,
        [this, watcher, connectAfterSuccess]() {
        const ConnectionTestReport report = watcher->result();
        watcher->deleteLater();
        if (report.error.message.isEmpty()) {
            if (!connectAfterSuccess
                && connectionTypeCombo_->currentData().toInt()
                    == static_cast<int>(ConnectionType::EndpointGroup)
                && report.suggestedTopology != ConnectionTopology::Generic
                && report.suggestedTopology != static_cast<ConnectionTopology>(
                    topologyCombo_->currentData().toInt())) {
                const QString suggestion = report.suggestedTopology == ConnectionTopology::Dsc
                    ? QStringLiteral("DSC") : QStringLiteral("数据守护");
                if (MessagePrompt::show(QMessageBox::Question, this, QStringLiteral("拓扑建议"),
                        QStringLiteral("%1\n\n检测结果建议拓扑为“%2”。是否应用到当前配置？")
                            .arg(report.summary(), suggestion),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
            QStringLiteral("应用建议"), QStringLiteral("保留当前配置"))
                    == QMessageBox::Yes) {
                    topologyCombo_->setCurrentIndex(topologyCombo_->findData(
                        static_cast<int>(report.suggestedTopology)));
                }
            }
            const bool completed = finishSuccessfulCheck(connectAfterSuccess);
            if (completed && !connectAfterSuccess && !report.summary().isEmpty()) {
                setStatusMessage(QStringLiteral("测试连接成功。\n%1").arg(report.summary()));
            }
        } else {
            QString detail = QStringLiteral("%1\n原因：%2\n建议：%3")
                .arg(connectAfterSuccess
                        ? QStringLiteral("连接验证失败")
                        : QStringLiteral("测试连接失败"),
                    report.error.message, report.error.recovery);
            if (report.endpoints.size() > 1 && !report.summary().isEmpty()) {
                detail += QStringLiteral("\n端点详情：\n%1").arg(report.summary());
            }
            setBusy(false, detail, QStringLiteral("danger"));
        }
    });
    watcher->setFuture(QtConcurrent::run([profile, password = std::move(password)]() mutable {
        return DpiSession::testEndpoints(profile, std::move(password));
    }));
}

bool ConnectionDialog::finishSuccessfulCheck(bool connectAfterSuccess) {
    const bool saveCredential = savePassword_->isChecked()
        && (!passwordEdit_->text().isEmpty() || sessionCredentialUnsaved_);
    if ((connectAfterSuccess || saveCredential) && !saveForm(false)) {
        setBusy(false, QStringLiteral("验证成功，但连接参数未能保存，未写入密码。\n") + operationStatus(), QStringLiteral("danger"));
        return false;
    }
    requireNewPassword_ = false;
    emit credentialVerified(formProfile().id, effectivePassword());
    if (saveCredential) {
        return storeEnteredCredential(connectAfterSuccess);
    }
    setBusy(false, connectAfterSuccess
            ? QStringLiteral("连接验证成功。")
            : QStringLiteral("测试连接成功。"));
    if (connectAfterSuccess) {
        selectedProfile_ = formProfile();
        accept();
    }
    return true;
}

bool ConnectionDialog::storeEnteredCredential(bool connectAfterSuccess) {
    ConnectionProfile profile = formProfile();
    QByteArray password = effectivePassword();
    setBusy(true, QStringLiteral("连接成功，正在安全保存密码…"));
    const QPointer<ConnectionDialog> guard(this);
    CredentialResult backup = credentialStore_->read(profile.id);
    if (guard.isNull()) { backup.password.fill('\0'); password.fill('\0'); return false; }
    const bool unreadable = backup.status != CredentialStatus::Success
        && backup.status != CredentialStatus::NotFound;
    if (unreadable) {
        backup.password.fill('\0');
        const auto choice = backup.status == CredentialStatus::Error
            ? MessagePrompt::show(QMessageBox::Warning, this, QStringLiteral("替换无法读取的旧密码"),
                QStringLiteral("新密码已通过连接验证，但无法读取旧密码。\n"
                    "替换后旧密码将无法恢复；选择“仅本次使用”会保留旧记录，不保存新密码。\n\n%1")
                    .arg(backup.message),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
                QStringLiteral("替换旧密码"), QStringLiteral("仅本次使用"))
            : QMessageBox::No;
        if (guard.isNull()) { password.fill('\0'); return false; }
        if (choice != QMessageBox::Yes) {
            password.fill('\0');
            useVerifiedSessionCredential(connectAfterSuccess,
                QStringLiteral("连接验证成功；新密码仅本次使用，旧记录已保留。\n%1").arg(backup.message));
            return false;
        }
        // An unreadable secret cannot be rolled back. Commit the profile flag
        // before writing, so a configuration failure never destroys that secret.
        if (!updateStoredCredentialFlag(profile.id, true)) {
            password.fill('\0');
            if (guard.isNull()) return false;
            setBusy(false, QStringLiteral("连接验证成功，但配置状态保存失败，未替换旧密码。\n")
                + operationStatus(), QStringLiteral("warning"));
            return false;
        }
        if (guard.isNull()) { password.fill('\0'); return false; }
    }
    CredentialResult result = credentialStore_->write(profile.id, std::move(password));
    if (guard.isNull()) {
        result.password.fill('\0'); backup.password.fill('\0');
        return false;
    }
    if (!result.succeeded()) {
        backup.password.fill('\0');
        QString detail = result.message;
        if (unreadable && !profile.hasStoredCredential) {
            const bool restored = updateStoredCredentialFlag(profile.id, false);
            if (guard.isNull()) return false;
            if (!restored) detail += QStringLiteral("\n凭据标记恢复失败，请重新打开连接管理核对。");
        }
        useVerifiedSessionCredential(connectAfterSuccess,
            QStringLiteral("连接验证成功；密码未保存，仅本次使用。\n%1").arg(detail));
        return false;
    }
    if (!unreadable && !updateStoredCredentialFlag(profile.id, true)) {
        const auto restored = backup.succeeded()
            ? credentialStore_->write(profile.id, std::move(backup.password))
            : credentialStore_->remove(profile.id);
        backup.password.fill('\0');
        if (guard.isNull()) return false;
        setBusy(false, QStringLiteral("连接配置状态保存失败。 ")
            + (restored.succeeded() || restored.status == CredentialStatus::NotFound
                ? QStringLiteral("已撤销本次密码保存，保留原凭据状态。")
                : QStringLiteral("恢复原凭据状态失败：") + restored.message), QStringLiteral("danger"));
        return false;
    }
    backup.password.fill('\0');
    QByteArray verifiedPassword = effectivePassword();
    loadedPassword_.fill('\0');
    loadedPassword_ = std::move(verifiedPassword);
    sessionCredentialUnsaved_ = false;
    passwordEdit_->clear();
    dirty_ = false;
    removePasswordButton_->setEnabled(true);
    setBusy(false, QStringLiteral("连接成功，密码已安全保存。"));
    if (connectAfterSuccess) {
        selectedProfile_ = formProfile();
        accept();
    }
    return true;
}

void ConnectionDialog::useVerifiedSessionCredential(bool connectAfterSuccess, const QString& message) {
    QByteArray verifiedPassword = effectivePassword();
    loadedPassword_.fill('\0');
    loadedPassword_ = std::move(verifiedPassword);
    sessionCredentialUnsaved_ = true;
    passwordEdit_->clear();
    const QSignalBlocker blocker(savePassword_);
    savePassword_->setChecked(false); // Session-only must not trigger credential removal.
    dirty_ = false;
    setBusy(false, message, QStringLiteral("warning"));
    if (connectAfterSuccess) {
        selectedProfile_ = formProfile();
        accept();
    }
}

void ConnectionDialog::removeStoredCredential() {
    ConnectionProfile profile = formProfile();
    if (!profile.hasStoredCredential && loadedPassword_.isEmpty()) {
        return;
    }
    if (MessagePrompt::show(QMessageBox::Question, this, QStringLiteral("移除已保存密码"),
            QStringLiteral("确定从本机安全凭据库中移除此连接的密码吗？"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel,
            QStringLiteral("移除密码"), QStringLiteral("保留密码")) != QMessageBox::Yes) {
        suppressSavePasswordToggle_ = true;
        savePassword_->setChecked(true);
        suppressSavePasswordToggle_ = false;
        return;
    }
    setBusy(true, QStringLiteral("正在移除已保存密码…"));
    if (!updateStoredCredentialFlag(profile.id, false)) {
        setBusy(false, QStringLiteral("连接配置状态保存失败，密码未移除。"));
        return;
    }
    const QPointer<ConnectionDialog> guard(this);
    const CredentialResult result = credentialStore_->remove(profile.id);
    if (guard.isNull()) {
        return;
    }
    if (result.status != CredentialStatus::Success
        && result.status != CredentialStatus::NotFound) {
        setBusy(false, result.message + QStringLiteral(" 密码未移除。"));
        (void)updateStoredCredentialFlag(profile.id, true);
        suppressSavePasswordToggle_ = true;
        savePassword_->setChecked(true);
        suppressSavePasswordToggle_ = false;
        return;
    }
    loadedPassword_.fill('\0');
    loadedPassword_.clear();
    suppressSavePasswordToggle_ = true;
    sessionCredentialUnsaved_ = false;
    savePassword_->setChecked(false);
    suppressSavePasswordToggle_ = false;
    removePasswordButton_->setEnabled(false);
    emit storedCredentialRemoved(profile.id);
    setBusy(false, QStringLiteral("已移除本机保存的密码。"));
}

void ConnectionDialog::deleteSelectedProfile() {
    const int index = profileSelector_->currentIndex();
    if (index < 0 || index >= profiles_.size()) {
        return;
    }
    (void)deleteSavedProfile(profiles_[index].id, this);
}

QString ConnectionDialog::operationStatus() const { return statusLabel_->accessibleDescription(); }

bool ConnectionDialog::deleteSavedProfile(const QString& profileId, QWidget* confirmationParent) {
    if (profileId.isEmpty() || !newButton_->isEnabled()) return false;
    QString error;
    auto latest = store_.load(&error);
    const auto find = [&profileId](const ConnectionProfile& profile) { return profile.id == profileId; };
    auto target = std::find_if(latest.cbegin(), latest.cend(), find);
    if (!error.isEmpty() || target == latest.cend()) {
        setBusy(false, error.isEmpty() ? QStringLiteral("连接配置已不存在，未删除其他连接。") : error);
        return false;
    }
    const ConnectionProfile profile = *target;
    if (MessagePrompt::show(QMessageBox::Warning, confirmationParent ? confirmationParent : this, QStringLiteral("删除连接"),
            QStringLiteral("确定删除连接“%1”吗？\n环境：%2\n分组：%3\n\n连接配置和本机保存的密码将一并删除。"
                "已打开的会话继续运行，不关闭窗口；下次连接需要重新配置。")
                .arg(profile.name, connectionEnvironmentLabel(profile.environment),
                    profile.group.isEmpty() ? QStringLiteral("未分组") : profile.group),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel,
            QStringLiteral("删除连接及密码"), QStringLiteral("保留连接")) != QMessageBox::Yes) {
        setStatusMessage(QStringLiteral("已取消删除连接。"));
        return false;
    }
    latest = store_.load(&error);
    target = std::find_if(latest.cbegin(), latest.cend(), find);
    if (!error.isEmpty() || target == latest.cend() || !(*target == profile)) {
        setBusy(false, error.isEmpty() ? QStringLiteral("确认期间连接配置已变化，未删除，请重新核对。") : error);
        return false;
    }
    profiles_ = latest; // Preserve unrelated changes made while the confirmation was open.
    if (!profile.hasStoredCredential) {
        const bool removed = finishProfileDeletion(profile.id);
        if (removed) emit storedCredentialRemoved(profile.id);
        return removed;
    }
    setBusy(true, QStringLiteral("正在删除连接及本机凭据…"));
    const QPointer<ConnectionDialog> guard(this);
    CredentialResult backup = credentialStore_->read(profile.id);
    if (guard.isNull()) { backup.password.fill('\0'); return false; }
    if (backup.status != CredentialStatus::Success
            && backup.status != CredentialStatus::NotFound) {
        backup.password.fill('\0');
        setBusy(false, backup.message + QStringLiteral(" 连接未删除。"));
        return false;
    }
    const CredentialResult result = credentialStore_->remove(profile.id);
    if (guard.isNull()) {
        backup.password.fill('\0');
        return false;
    }
    bool removed = false;
    if (result.status == CredentialStatus::Success
        || result.status == CredentialStatus::NotFound) {
        removed = finishProfileDeletion(profile.id);
        if (!removed
                && backup.status == CredentialStatus::Success) {
            CredentialResult restored =
                credentialStore_->write(profile.id, std::move(backup.password));
            if (guard.isNull()) return false;
            if (!restored.succeeded()) {
                setStatusMessage(operationStatus()
                    + QStringLiteral(" 本机凭据恢复失败：") + restored.message);
            }
        }
    } else {
        setBusy(false, result.message + QStringLiteral(" 连接未删除。"));
    }
    backup.password.fill('\0');
    backup.password.clear();
    if (removed) emit storedCredentialRemoved(profile.id);
    return removed;
}

bool ConnectionDialog::finishProfileDeletion(const QString& profileId) {
    const QVector<ConnectionProfile> previous = profiles_;
    QString error;
    const auto current = store_.load(&error);
    const auto sameId = [&profileId](const ConnectionProfile& profile) { return profile.id == profileId; };
    const auto expected = std::find_if(previous.cbegin(), previous.cend(), sameId);
    const auto actual = std::find_if(current.cbegin(), current.cend(), sameId);
    if (!error.isEmpty() || expected == previous.cend() || actual == current.cend() || !(*expected == *actual)) {
        setBusy(false, error.isEmpty() ? QStringLiteral("连接配置已变化，未删除；请重新核对。") : error);
        return false;
    }
    profiles_ = current;
    profiles_.erase(std::remove_if(profiles_.begin(), profiles_.end(),
        [&profileId](const ConnectionProfile& profile) { return profile.id == profileId; }),
        profiles_.end());
    if (!store_.save(profiles_, &error)) {
        profiles_ = previous;
        setBusy(false, error + QStringLiteral(" 连接未删除。"));
        return false;
    }
    setBusy(false, QStringLiteral("连接及其本机凭据已删除。"));
    loadProfiles();
    setStatusMessage(QStringLiteral("连接及其本机凭据已删除；已打开的会话继续运行。"));
    emit profilesChanged();
    return true;
}

bool ConnectionDialog::updateStoredCredentialFlag(const QString& profileId, bool stored) {
    const auto profile = std::find_if(profiles_.begin(), profiles_.end(),
        [&profileId](const ConnectionProfile& item) { return item.id == profileId; });
    if (profile == profiles_.end()) {
        return false;
    }
    QString error;
    auto latest = store_.load(&error);
    auto current = std::find_if(latest.begin(), latest.end(), [&profileId](const auto& item) { return item.id == profileId; });
    if (!error.isEmpty() || current == latest.end() || !(*current == *profile)) {
        setStatusMessage(error.isEmpty() ? QStringLiteral("连接配置已变化，未覆盖修改；请重新打开连接管理。") : error);
        return false;
    }
    current->hasStoredCredential = stored;
    if (!store_.save(latest, &error)) {
        setStatusMessage(error);
        return false;
    }
    profile->hasStoredCredential = stored;
    emit profilesChanged();
    return true;
}

void ConnectionDialog::setBusy(
        bool busy, const QString& status, const QString& callout) {
    busy_ = busy;
    if (profileBar_) profileBar_->setEnabled(!busy);
    if (profileSidebar_) profileSidebar_->setEnabled(!busy);
    if (profileList_) {
        const bool draggable = !busy && profileSearch_->text().trimmed().isEmpty();
        profileList_->setDragEnabled(draggable);
        profileList_->setAcceptDrops(draggable);
    }
    if (categories_) categories_->setEnabled(!busy);
    profileSelector_->setEnabled(!busy);
    newButton_->setEnabled(!busy);
    deleteButton_->setEnabled(!busy && profileSelector_->currentIndex() >= 0);
    copyButton_->setEnabled(!busy && profileSelector_->currentIndex() >= 0);
    importButton_->setEnabled(!busy);
    exportButton_->setEnabled(!busy && !profiles_.isEmpty());
    saveButton_->setEnabled(!busy);
    testButton_->setEnabled(!busy);
    copyServiceConfigButton_->setEnabled(!busy
        && connectionTypeCombo_->currentData().toInt() == static_cast<int>(ConnectionType::DmService));
    connectButton_->setEnabled(!busy);
    nameEdit_->setEnabled(!busy);
    hostEdit_->setEnabled(!busy);
    portEdit_->setEnabled(!busy);
    connectionTypeCombo_->setEnabled(!busy);
    servicePanel_->setEnabled(!busy);
    endpointsEdit_->setEnabled(!busy);
    topologyCombo_->setEnabled(!busy);
    selectionPolicyCombo_->setEnabled(!busy);
    readOnly_->setEnabled(!busy);
    if (!busy) updateConnectionTypeControls();
    requiredRoleCombo_->setEnabled(!busy);
    expectedDatabaseEdit_->setEnabled(!busy);
    expectedOguidEdit_->setEnabled(!busy);
    userEdit_->setEnabled(!busy);
    passwordEdit_->setEnabled(!busy);
    descriptionEdit_->setEnabled(!busy);
    showPassword_->setEnabled(!busy);
    savePassword_->setEnabled(!busy && credentialStoreAvailable_);
    removePasswordButton_->setEnabled(!busy && (!loadedPassword_.isEmpty() || formProfile().hasStoredCredential));
    setStatusMessage(status, callout);
}

void ConnectionDialog::setStatusMessage(const QString& message, const QString& callout) {
    const bool detailed = message.contains(u'\n') || message.size() > 90;
    const QString summary = message.section(u'\n', 0, 0);
    statusLabel_->setText(detailed ? (summary.size() > 90 ? summary.left(90) + QStringLiteral("…") : summary) : message);
    statusLabel_->setToolTip(message);
    if (statusDetails_) {
        statusDetails_->setPlainText(message); statusToggle_->setVisible(detailed);
        if (!detailed) { statusToggle_->setChecked(false); statusDetails_->hide(); }
    }
    statusLabel_->setAccessibleDescription(message);
    if (statusLabel_->property("callout").toString() == callout) return;
    statusLabel_->setProperty("callout", callout);
    statusLabel_->style()->unpolish(statusLabel_);
    statusLabel_->style()->polish(statusLabel_);
}

ConnectionProfile ConnectionDialog::selectedProfile() const {
    return selectedProfile_;
}

QByteArray ConnectionDialog::takePassword() {
    QByteArray password = effectivePassword();
    passwordEdit_->clear();
    loadedPassword_.fill('\0');
    loadedPassword_.clear();
    sessionCredentialUnsaved_ = false;
    return password;
}

void ConnectionDialog::selectProfile(const QString& profileId) {
    loadProfiles(profileId);
    if (profileList_ && profileList_->currentItem())
        profileList_->scrollToItem(profileList_->currentItem());
}

void ConnectionDialog::focusPasswordInput() {
    categories_->setCurrentIndex(0);
    if (auto* scroll = qobject_cast<QScrollArea*>(categories_->widget(0))) scroll->ensureWidgetVisible(passwordEdit_);
    passwordEdit_->setFocus(Qt::OtherFocusReason);
    passwordEdit_->selectAll();
}

void ConnectionDialog::presentCredentialFailure(const QString& message) {
    ++credentialLoadGeneration_;
    loadedPassword_.fill('\0');
    loadedPassword_.clear();
    passwordEdit_->clear();
    sessionCredentialUnsaved_ = false;
    requireNewPassword_ = true;

    const QString guidance = QStringLiteral("认证失败，请重新输入用户名或密码后连接。\n%1")
        .arg(message.trimmed());
    setStatusMessage(guidance, QStringLiteral("danger"));
    passwordEdit_->setAccessibleDescription(
        QStringLiteral("认证失败，请重新输入数据库密码。"));
    QTimer::singleShot(0, this, [this]() { focusPasswordInput(); });
}
