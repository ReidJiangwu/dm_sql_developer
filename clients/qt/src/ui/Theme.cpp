#include "Theme.h"
#include "TextTransformMenu.h"
#include "LogView.h"
#include "InterfaceIcons.h"
#include "DiagnosticTextInteraction.h"
#include "MessagePrompt.h"
#include "RuntimeUiScale.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QChildEvent>
#include <QColor>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QLineEdit>
#include <QMainWindow>
#include <QPalette>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QStyle>
#include <QStyleOption>
#include <QStyleHints>
#include <QToolButton>
#include <QTabBar>

namespace {

// Load the fonts available in this edition without installing system fonts.
void loadApplicationFonts() {
    static const bool loaded = [] {
        for (const auto* file : {":/fonts/NotoSerifCJKsc-Regular.otf", ":/fonts/MesloLGS-NF-Regular.ttf",
                ":/fonts/MesloLGS-NF-Bold.ttf", ":/fonts/MesloLGS-NF-Italic.ttf", ":/fonts/MesloLGS-NF-BoldItalic.ttf"}) {
            const QString path = QString::fromLatin1(file);
            if ((path == QStringLiteral(":/fonts/NotoSerifCJKsc-Regular.otf") || QFile::exists(path))
                && QFontDatabase::addApplicationFont(path) < 0)
                qWarning("Could not load application font: %s", file);
        }
        return true;
    }();
    Q_UNUSED(loaded);
}

class WorkbenchStyle final : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    QIcon standardIcon(StandardPixmap icon, const QStyleOption* option = nullptr,
            const QWidget* widget = nullptr) const override {
        const char* name = nullptr;
        switch (icon) {
        case SP_TitleBarCloseButton: case SP_DialogCloseButton: name = "close"; break;
        case SP_FileIcon: name = "file"; break;
        case SP_DirIcon: case SP_DirClosedIcon: name = "folder"; break;
        case SP_DirOpenIcon: name = "open"; break;
        case SP_TrashIcon: name = "trash"; break;
        case SP_BrowserReload: name = "refresh"; break;
        case SP_ArrowUp: name = "arrow-up"; break;
        case SP_ArrowDown: name = "arrow-down"; break;
        case SP_ArrowLeft: name = "arrow-left"; break;
        case SP_ArrowRight: name = "arrow-right"; break;
        case SP_MessageBoxInformation: name = "about"; break;
        case SP_MessageBoxWarning: name = "warning"; break;
        case SP_MessageBoxCritical: name = "critical"; break;
        case SP_MessageBoxQuestion: name = "question"; break;
        case SP_DialogApplyButton: case SP_DialogOkButton: name = "commit"; break;
        case SP_DialogCancelButton: name = "close"; break;
        default: return QProxyStyle::standardIcon(icon, option, widget);
        }
        return InterfaceIcons::icon(QString::fromLatin1(name));
    }

    int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
            const QWidget* widget = nullptr) const override {
        const int base = QProxyStyle::pixelMetric(metric, option, widget);
        switch (metric) {
        case PM_ScrollBarExtent:
        case PM_ScrollBarSliderMin:
        case PM_IndicatorWidth:
        case PM_IndicatorHeight:
        case PM_ExclusiveIndicatorWidth:
        case PM_ExclusiveIndicatorHeight:
        case PM_MenuButtonIndicator:
            return qMax(1, qRound(base * RuntimeUiScale::factor()));
        default:
            return base;
        }
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
            QPainter* painter, const QWidget* widget = nullptr) const override {
        // The stylesheet supplies the keyboard focus border on buttons/tabs.
        // Suppress the native dotted rectangle, keeping focus on other controls.
        if (element == PE_FrameFocusRect && widget
                && (qobject_cast<const QAbstractButton*>(widget) || qobject_cast<const QTabBar*>(widget))) return;
        if (element != PE_IndicatorArrowDown && element != PE_IndicatorArrowUp
                && element != PE_IndicatorArrowLeft && element != PE_IndicatorArrowRight) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        if (!option || option->rect.isEmpty()) return;
        const bool vertical = element == PE_IndicatorArrowDown || element == PE_IndicatorArrowUp;
        const qreal scale = RuntimeUiScale::factor();
        const qreal width = qMin<qreal>(4 * scale, (vertical ? option->rect.width() : option->rect.height()) * 0.35);
        const qreal height = qMin<qreal>(2 * scale, (vertical ? option->rect.height() : option->rect.width()) * 0.25);
        const auto group = option->state.testFlag(State_Enabled) ? QPalette::Active : QPalette::Disabled;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->translate(QRectF(option->rect).center());
        if (element == PE_IndicatorArrowUp) painter->rotate(180);
        else if (element == PE_IndicatorArrowRight) painter->rotate(-90);
        else if (element == PE_IndicatorArrowLeft) painter->rotate(90);
        painter->setPen(QPen(option->palette.color(group, QPalette::ButtonText),
            1.4 * scale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        QPainterPath path;
        path.moveTo(-width, -height);
        path.lineTo(0, height);
        path.lineTo(width, -height);
        painter->drawPath(path);
        painter->restore();
    }
};

QString localizedStandardButtonText(QDialogButtonBox::StandardButton button) {
    switch (button) {
        case QDialogButtonBox::Ok: return QStringLiteral("确定");
        case QDialogButtonBox::Save: return QStringLiteral("保存");
        case QDialogButtonBox::SaveAll: return QStringLiteral("全部保存");
        case QDialogButtonBox::Open: return QStringLiteral("打开");
        case QDialogButtonBox::Yes: return QStringLiteral("是");
        case QDialogButtonBox::YesToAll: return QStringLiteral("全部是");
        case QDialogButtonBox::No: return QStringLiteral("否");
        case QDialogButtonBox::NoToAll: return QStringLiteral("全部否");
        case QDialogButtonBox::Abort: return QStringLiteral("中止");
        case QDialogButtonBox::Retry: return QStringLiteral("重试");
        case QDialogButtonBox::Ignore: return QStringLiteral("忽略");
        case QDialogButtonBox::Close: return QStringLiteral("关闭");
        case QDialogButtonBox::Cancel: return QStringLiteral("取消");
        case QDialogButtonBox::Discard: return QStringLiteral("放弃");
        case QDialogButtonBox::Help: return QStringLiteral("帮助");
        case QDialogButtonBox::Apply: return QStringLiteral("应用");
        case QDialogButtonBox::Reset: return QStringLiteral("重置");
        case QDialogButtonBox::RestoreDefaults: return QStringLiteral("恢复默认");
        default: return {};
    }
}

bool containsCjk(const QString& text) {
    for (const QChar character : text) {
        const ushort code = character.unicode();
        if (code >= 0x3400 && code <= 0x9FFF) return true;
    }
    return false;
}

class DialogButtonLocalizer final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::Polish && event->type() != QEvent::Show) {
            return QObject::eventFilter(watched, event);
        }
        auto* buttons = qobject_cast<QDialogButtonBox*>(watched);
        if (auto* prompt = qobject_cast<QMessageBox*>(watched)) {
            MessagePrompt::structure(prompt);
            MessagePrompt::constrainDialog(prompt);
        } else if (auto* dialog = qobject_cast<QDialog*>(watched);
                   dialog && !qobject_cast<QFileDialog*>(dialog)) {
            if (qobject_cast<QInputDialog*>(dialog) || dialog->property("confirmationForm").toBool())
                MessagePrompt::styleConfirmationForm(dialog);
            MessagePrompt::constrainDialog(dialog);
        }
        if (buttons == nullptr) return QObject::eventFilter(watched, event);
        for (QAbstractButton* button : buttons->buttons()) {
            if (containsCjk(button->text())) continue;
            const QString localized = localizedStandardButtonText(buttons->standardButton(button));
            if (!localized.isEmpty()) button->setText(localized);
        }
        return QObject::eventFilter(watched, event);
    }
};

class LineEditClearButtonAccessibility final : public QObject {
public:
    using QObject::QObject;

protected:
    static bool isClearButton(QToolButton* button) {
        if (button == nullptr) return false;
        const QAction* action = button->defaultAction();
        return action != nullptr
            && action->objectName() == QStringLiteral("_q_qlineeditclearaction");
    }

    static void configureClearButton(QToolButton* button) {
        if (!isClearButton(button)) return;
        auto* lineEdit = qobject_cast<QLineEdit*>(button->parentWidget());
        auto* action = button->defaultAction();
        // Qt normally wires this private action to QLineEdit::clear(). Some
        // Windows styles expose the action but do not forward QAction::trigger
        // reliably, so retain the native connection and add an idempotent
        // fallback for keyboard/accessibility activation.
        if (lineEdit != nullptr && action != nullptr
            && !action->property("reidClearActionConnected").toBool()) {
            QObject::connect(action, &QAction::triggered, lineEdit,
                [lineEdit](bool) { lineEdit->clear(); });
            action->setProperty("reidClearActionConnected", true);
        }
        button->setProperty("lineEditClearButton", true);
        button->setAccessibleName(QStringLiteral("清除文本"));
        button->setToolTip(QStringLiteral("清除文本"));
        // Ask the active style for its palette-aware clear glyph. This keeps
        // the icon legible when Theme::apply switches between light and dark.
        button->setIcon(button->style()->standardIcon(
            QStyle::SP_LineEditClearButton, nullptr, button));
        button->update();
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Polish || event->type() == QEvent::Show) {
            auto* button = qobject_cast<QToolButton*>(watched);
            if (button != nullptr && qobject_cast<QLineEdit*>(button->parentWidget()) != nullptr) {
                configureClearButton(button);
            }
        }
        auto* lineEdit = qobject_cast<QLineEdit*>(watched);
        if (lineEdit == nullptr) return QObject::eventFilter(watched, event);

        if (event->type() == QEvent::ChildAdded) {
            auto* childEvent = static_cast<QChildEvent*>(event);
            configureClearButton(qobject_cast<QToolButton*>(childEvent->child()));
        } else if (event->type() == QEvent::Polish || event->type() == QEvent::Show
                   || event->type() == QEvent::StyleChange) {
            for (auto* clearButton : lineEdit->findChildren<QToolButton*>()) {
                configureClearButton(clearButton);
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

struct Colors {
    // Semantic foundation: canvas, surfaces and content areas.
    QString window;
    QString surface;
    QString surfaceMuted;
    QString text;
    QString textMuted;
    QString border;
    QString accent;
    QString danger;
    QString focusSurface;
    QString warning;
    QString warningText;
    QString warningSurface;
    QString dangerSurface;
    QString selection;
    QString selectionText;
    QString success;
    QString successSurface;
    QString info;
    QString infoSurface;
    QString sidebar;
    QString content;
    QString raised;
    QString control;
    QString editor;
    QString result;
    QString syntaxKeyword;
    QString syntaxComment;
    QString syntaxString;
    QString syntaxNumber;
    QString syntaxSymbol;
    QString syntaxCustom;
    QString progressFill;
};

Colors colorsForTheme(bool dark) {
    // Keep all Windows Widgets colours in this one semantic-token table.  A
    // component should use a semantic role rather than introduce a local hex.
    return dark
        ? Colors{
            "#1C1C1E", "#2C2C2E", "#242426", "#F5F5F7", "#AEAEB2",
            "#48484A", "#0A84FF", "#FF453A", "#003A70", "#FF9F0A",
            "#FF9F0A", "#4A2F00", "#4A1117", "#0A84FF", "#1C1C1E",
            "#30D158", "#123D22", "#64D2FF", "#123B4A", "#242426",
            "#1C1C1E", "#2C2C2E", "#2C2C2E", "#242426", "#2C2C2E",
            "#64D2FF", "#87B78B", "#F2B66D", "#64D2FF", "#AEAEB2", "#64D2FF", "#244D78"}
        : Colors{
            "#F6F8FA", "#FFFFFF", "#F6F8FA", "#243247", "#5F6E82",
            "#E3E8EE", "#2B65D9", "#C83532", "#EDF3FF", "#8A5300",
            "#8A5300", "#FFF3E8", "#FDEBED", "#2B65D9", "#FFFFFF",
            "#16794A", "#EBF7F1", "#2B65D9", "#EDF3FF", "#F6F8FA",
            "#FFFFFF", "#FFFFFF", "#FFFFFF", "#FFFFFF", "#FFFFFF",
            "#2F5FC7", "#39704A", "#A43C26", "#2B65D9", "#5F6E82", "#2F5FC7", "#BDD1F2"};
}

bool systemUsesDarkColors(const QApplication& application) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme colorScheme = QGuiApplication::styleHints()->colorScheme();
    if (colorScheme != Qt::ColorScheme::Unknown) {
        return colorScheme == Qt::ColorScheme::Dark;
    }
#endif
    return application.style()->standardPalette().color(QPalette::Window).lightness() < 128;
}

QPalette createPalette(bool dark) {
    const Colors colors = colorsForTheme(dark);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(colors.window));
    palette.setColor(QPalette::WindowText, QColor(colors.text));
    palette.setColor(QPalette::Base, QColor(colors.editor));
    palette.setColor(QPalette::AlternateBase, QColor(colors.surfaceMuted));
    palette.setColor(QPalette::Text, QColor(colors.text));
    palette.setColor(QPalette::Button, QColor(colors.control));
    palette.setColor(QPalette::ButtonText, QColor(colors.text));
    palette.setColor(QPalette::Mid, QColor(colors.border));
    palette.setColor(QPalette::Highlight, QColor(colors.selection));
    palette.setColor(QPalette::HighlightedText, QColor(colors.selectionText));
    palette.setColor(QPalette::ToolTipBase, QColor(colors.raised));
    palette.setColor(QPalette::ToolTipText, QColor(colors.text));
    palette.setColor(QPalette::PlaceholderText, QColor(colors.textMuted));

    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(colors.textMuted));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(colors.textMuted));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(colors.textMuted));
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(colors.textMuted));
    return palette;
}

QString createStyleSheet(bool dark) {
    const Colors colors = colorsForTheme(dark);
    QFont progressFont(QStringLiteral("Noto Serif CJK SC"));
    progressFont.setPixelSize(12);
    const int progressHeight = qMax(22, QFontMetrics(progressFont).height() + 6);

    return QStringLiteral(R"(
        QWidget {
            font-size: 13px;
        }
        /* Workbench chrome has its own readable type size. Do not apply this
           to editors, result cells or dialog forms with independent sizing. */
        QMenuBar, QMenu, QToolBar QToolButton, QToolBar QComboBox,
        QToolBar QLabel, QToolBar QCheckBox,
        QTabBar#documentTabBar, QWidget#navigationDockTitle QTabBar,
        QTabWidget#navigationTabs QTabBar,
        QTabWidget#documentOutputTabs QTabBar,
        QTabWidget#commandOutputTabs QTabBar,
        QTabWidget#queryResultTabs QTabBar,
        QTreeWidget#objectTree, QTreeWidget#connectionNavigationList,
        QTreeWidget#workspaceTree, QListWidget#windowList, QListWidget#snippetList,
        QStatusBar QLabel {
            font-size: 14px;
        }
        QMainWindow {
            background: %1;
            color: %4;
        }
        QDialog {
            background: %22;
            color: %4;
        }
        QDialog, QLabel, QAbstractButton, QComboBox, QLineEdit,
        QSpinBox, QDoubleSpinBox, QDateTimeEdit, QKeySequenceEdit,
        QHeaderView, QTabBar, QMenuBar, QMenu,
        QTreeView, QListView, QTableView {
            font-family: "Noto Serif CJK SC";
        }
        QMenuBar, QMenu, QToolBar, QStatusBar {
            background: %2;
            color: %4;
        }
        QMenuBar {
            border-bottom: 1px solid %6;
            padding: 2px 8px;
        }
        QMenuBar::item {
            padding: 3px 8px;
            border-radius: 3px;
        }
        QMenu::item {
            min-width: 170px;
            padding: 6px 28px 6px 12px;
            margin: 1px 4px;
            border-radius: 5px;
        }
        QMenu::item:disabled {
            color: %5;
        }
        QMenu::separator {
            height: 1px;
            margin: 5px 9px;
            background: %6;
        }
        QMenu::icon {
            padding-left: 4px;
        }
        QMenuBar::item:selected, QMenu::item:selected {
            background: %9;
            color: %4;
        }
        QToolBar {
            border: 0;
            border-bottom: 1px solid %6;
            padding: 2px 6px;
            spacing: 3px;
        }
        QToolBar#primaryToolBar {
            background: %2;
            min-height: 32px;
            padding: 2px 8px;
            spacing: 4px;
        }
        QToolBar#primaryToolBar::separator {
            width: 1px;
            margin: 6px 6px;
            background: %6;
        }
        QToolBar#documentToolBar {
            background: %2;
            min-height: 32px;
            padding: 1px 6px;
            spacing: 3px;
        }
        QToolBar#commandContextToolBar {
            background: %2;
            min-height: 32px;
            padding: 1px 6px;
            spacing: 2px;
        }
        QToolBar#testContextToolBar {
            background: %2;
            min-height: 32px;
            padding: 1px 6px;
            spacing: 2px;
        }
        QToolBar[documentContext="true"] {
            background: %2;
            min-height: 32px;
            padding: 1px 6px;
            spacing: 3px;
        }
        QToolBar#programContextToolBar {
            min-height: 32px;
            padding: 1px 6px;
            spacing: 2px;
        }
        QToolBar#programContextToolBar QPushButton,
        QToolBar#programContextToolBar QToolButton {
            min-height: 24px;
            max-height: 26px;
            margin: 0;
            padding: 0 8px;
            font-weight: 400;
        }
        QToolBar#programContextToolBar QPushButton[role="primary"] {
            font-weight: 600;
        }
        QToolBar[documentContext="true"] QLabel#programState {
            color: %5;
            padding: 0 4px;
        }
        QWidget#programMemberPane {
            background: %2;
            border-right: 1px solid %6;
        }
        QWidget#programMemberHeader {
            min-height: 34px;
            max-height: 34px;
            background: %3;
            border-bottom: 1px solid %6;
        }
        QWidget#programMemberControls {
            background: %3;
            border-bottom: 1px solid %6;
        }
        QWidget#programViewIdentity {
            background: %3;
            border: 1px solid %6;
            border-radius: 7px;
        }
        QLabel#programViewCommentTitle {
            color: %5;
            font-weight: 600;
        }
        QWidget#objectDetailsHeader,
        QWidget#objectDdlHeader {
            background: %2;
            border-bottom: 1px solid %6;
        }
        QLabel#objectDdlTitle,
        QLabel#objectDetailsTitle {
            color: %4;
            font-size: 15px;
            font-weight: 600;
        }
        QLabel#objectDdlKind {
            color: %5;
        }
        QLabel#objectDetailsKind[statusBadge="true"] {
            color: %5;
            background: %3;
            border: 1px solid %6;
            border-radius: 5px;
            padding: 2px 7px;
        }
        QWidget#objectDetailsHeader QPushButton,
        QWidget#objectDdlHeader QPushButton {
            min-height: 26px;
            padding: 0 8px;
            background: transparent;
            border: 1px solid transparent;
        }
        QWidget#objectDetailsHeader QPushButton:hover,
        QWidget#objectDdlHeader QPushButton:hover {
            background: %9;
        }
        QWidget#objectDetailsHeader QPushButton:focus,
        QWidget#objectDdlHeader QPushButton:focus {
            border: 2px solid %7;
        }
        QLabel#objectDdlState,
        QLabel#objectDetailsState {
            color: %5;
        }
        QPlainTextEdit#objectDdlEditor {
            background: %2;
            border: 0;
            padding: 10px 12px;
        }
        QProgressBar#objectDdlProgress,
        QProgressBar#objectDetailsProgress {
            min-height: 8px;
            max-height: 8px;
        }
        QPlainTextEdit#programViewComment {
            background: %3;
            border: 1px solid %6;
            border-radius: 6px;
            padding: 6px;
        }
        QWidget#objectBrowserControls {
            background: %3;
            border-bottom: 1px solid %6;
        }
        QWidget#objectBrowserControls QPushButton[sidebarAction="true"] {
            min-height: 22px;
            max-height: 22px;
            padding: 0 5px;
            border-radius: 4px;
        }
        QWidget#workspaceSearchRow QComboBox,
        QWidget#workspaceSearchRow QLineEdit {
            min-height: 26px;
            max-height: 26px;
        }
        QWidget#workspaceSearchRow QComboBox {
            min-width: 76px;
            max-width: 92px;
        }
        QWidget#workspaceSearchRow QCheckBox {
            spacing: 4px;
        }
        QLineEdit#workspaceSearch[invalidInput="true"] {
            color: %8;
            border-color: %8;
            background: %13;
        }
        QLabel#workspaceSearchStatus {
            color: %5;
            padding: 0 3px;
        }
        QWidget#invalidObjectControls {
            background: %2;
            border-bottom: 1px solid %6;
        }
        QWidget#invalidObjectControls QPushButton[compactAction="true"] {
            min-height: 24px;
            max-height: 24px;
            padding: 0 7px;
            border-radius: 3px;
        }
        QWidget#invalidObjectControls QLineEdit,
        QWidget#invalidObjectControls QComboBox,
        QWidget#invalidObjectFooter QComboBox {
            min-height: 24px;
            padding-top: 0;
            padding-bottom: 0;
            border-radius: 3px;
        }
        QWidget#invalidObjectFooter QPushButton,
        QPushButton#cancelInvalidCompile {
            min-height: 22px;
            padding: 0 6px;
        }
        QFrame#invalidObjectActionGroup {
            background: transparent;
            border: 0;
        }
        QFrame#invalidObjectActionGroup QPushButton[quietAction="true"] {
            background: transparent;
            border-color: transparent;
        }
        QFrame#invalidObjectActionGroup QPushButton[highRiskAction="true"]:enabled {
            color: %8;
            background: %13;
            border-color: %8;
            font-weight: 600;
        }
        QFrame#invalidObjectActionGroup QPushButton[highRiskAction="true"]:hover {
            background: %13;
            border-width: 2px;
        }
        QWidget#invalidObjectFooter {
            background: %3;
            border-top: 1px solid %6;
        }
        QWidget#invalidObjectFooter QLabel#invalidObjectSummary,
        QWidget#invalidObjectFooter QLabel#invalidObjectPageSummary {
            color: %5;
        }
        QWidget#missingPackageBodyControls {
            background: %2;
            border-bottom: 1px solid %6;
        }
        QWidget#missingPackageBodyControls QPushButton[compactAction="true"],
        QWidget#missingPackageBodyFooter QPushButton[compactAction="true"] {
            min-height: 28px;
            max-height: 28px;
            padding: 0 10px;
            border-radius: 5px;
        }
        QWidget#missingPackageBodyControls QPushButton[quietAction="true"] {
            background: transparent;
            border-color: transparent;
        }
        QTableWidget#missingPackageBodyTable {
            background: %2;
            border: 0;
        }
        QWidget#missingPackageBodyFooter {
            min-height: 34px;
            max-height: 38px;
            background: %3;
            border-top: 1px solid %6;
        }
        QLabel#missingPackageBodySummary,
        QWidget#missingPackageBodyFooter QLabel#missingPackageBodyPageSummary {
            color: %5;
        }
        QToolBar[documentContext="true"] QLabel#testActivityState {
            color: %5;
            min-height: 26px;
            max-height: 26px;
            padding: 0 7px;
        }
        QToolBar#testContextToolBar QPushButton,
        QToolBar#testContextToolBar QToolButton {
            min-height: 26px;
            max-height: 26px;
            margin: 0;
            padding-top: 0;
            padding-bottom: 0;
        }
        QWidget#testTargetBar {
            min-height: 32px;
            max-height: 32px;
            background: %2;
            border-bottom: 1px solid %6;
        }
        QLabel#testTargetCaption {
            color: %5;
        }
        QComboBox#testTargetSelector {
            min-height: 26px;
            max-height: 26px;
            padding-top: 0;
            padding-bottom: 0;
            background: %3;
            border-color: transparent;
        }
        QTabWidget#testResultTabs::pane {
            border-top: 1px solid %6;
        }
        QWidget#testVariableControls {
            background: %3;
            border-bottom: 1px solid %6;
        }
        QWidget#testDocumentWidget QTableWidget {
            background: %2;
        }
        QLabel#programMemberHeading {
            color: %4;
            font-weight: 600;
        }
        QLabel#programMemberCount {
            color: %5;
            background: transparent;
            border: 0;
            padding: 0;
        }
        QLabel#programMemberSourceCaption,
        QLabel#programMemberSortCaption {
            color: %5;
            padding-left: 1px;
            font-weight: 400;
        }
        QWidget#programMemberControls QLineEdit#programMemberFilter,
        QWidget#programMemberControls QComboBox[memberControl="true"] {
            min-height: 28px;
            max-height: 28px;
            background: %2;
            border: 1px solid %6;
            border-radius: 6px;
        }
        QWidget#programMemberControls QLineEdit#programMemberFilter:focus,
        QWidget#programMemberControls QComboBox[memberControl="true"]:focus {
            border: 2px solid %7;
        }
        QTreeWidget#programOutline {
            background: %2;
            alternate-background-color: %3;
            border: 0;
            padding: 4px 5px;
        }
        QTreeWidget#programOutline::item {
            min-height: 28px;
            padding: 0 5px;
            border-radius: 5px;
        }
        QTreeWidget#programOutline::item:hover {
            background: %3;
        }
        QTreeWidget#programOutline::item:selected {
            background: %9;
            color: %4;
        }
        QTableWidget#programCompileErrors {
            background: %2;
        }
        QToolBar[secondaryToolBar="true"] {
            background: %3;
            min-height: 32px;
            max-height: 34px;
            padding: 2px 7px;
            spacing: 4px;
        }
        QWidget[contextBar="true"] {
            min-height: 32px;
            max-height: 32px;
            background: %2;
            border-bottom: 1px solid %6;
        }
        QWidget[controlBar="true"] {
            min-height: 32px;
            max-height: 32px;
            background: %2;
            border-bottom: 1px solid %6;
        }
        QWidget#sqlConnectionBar {
            min-height: 32px;
            max-height: 32px;
            background: %2;
            border-bottom: 1px solid %6;
        }
        QToolButton#sqlWindowConnectionSelector,
        QToolButton#commandWindowConnectionSelector,
        QToolButton#testWindowConnectionSelector,
        QToolButton#dbaWindowConnectionSelector,
        QToolButton#roleEditorConnectionSelector,
        QToolButton#userEditorConnectionSelector {
            min-width: 0;
            min-height: 26px;
            max-height: 26px;
            padding: 0 22px 0 8px;
            color: %4;
            background: %3;
            border: 1px solid transparent;
            border-radius: 4px;
        }
        QToolButton#sqlWindowConnectionSelector:hover,
        QToolButton#sqlWindowConnectionSelector:pressed,
        QToolButton#commandWindowConnectionSelector:hover,
        QToolButton#commandWindowConnectionSelector:pressed,
        QToolButton#testWindowConnectionSelector:hover,
        QToolButton#testWindowConnectionSelector:pressed,
        QToolButton#dbaWindowConnectionSelector:hover,
        QToolButton#dbaWindowConnectionSelector:pressed,
        QToolButton#roleEditorConnectionSelector:hover,
        QToolButton#roleEditorConnectionSelector:pressed,
        QToolButton#userEditorConnectionSelector:hover,
        QToolButton#userEditorConnectionSelector:pressed {
            background: %9;
            border-color: transparent;
        }
        QToolButton {
            min-width: 26px;
            min-height: 26px;
            padding: 1px 3px;
            border: 1px solid transparent;
            border-radius: 4px;
        }
        QToolButton[textButton="true"] {
            min-width: 0;
            min-height: 26px;
            padding: 1px 9px;
            color: %4;
            background: transparent;
            border: 1px solid transparent;
            border-radius: 4px;
        }
        QToolButton[menuButton="true"] {
            padding-right: 24px;
        }
        QToolButton[menuButton="true"]::menu-button {
            subcontrol-origin: border;
            subcontrol-position: center right;
            width: 20px;
            border: 0;
            border-left: 0;
        }
        QToolButton[menuButton="true"]::menu-arrow {
            subcontrol-origin: content;
            subcontrol-position: center;
            width: 12px;
            height: 10px;
        }
        QToolButton[menuButton="true"]::menu-indicator {
            subcontrol-origin: border;
            subcontrol-position: center right;
            right: 6px;
            width: 12px;
            height: 10px;
        }
        QToolButton[instantMenuButton="true"]::menu-button {
            border-left: 0;
        }
        QToolButton#commandCopyErrorPathsButton {
            min-width: 0;
            min-height: 26px;
            max-height: 26px;
            padding: 0 22px 0 8px;
            margin: 0;
        }
        QComboBox {
            min-height: 26px;
            padding: 0 27px 0 8px;
            color: %4;
            background: %23;
            border: 1px solid %6;
            border-radius: 6px;
        }
        QComboBox:hover {
            border-color: %5;
        }
        QComboBox:focus {
            border-color: %7;
        }
        QComboBox::drop-down {
            subcontrol-origin: border;
            subcontrol-position: center right;
            width: 22px;
            border: 0;
            border-left: 0;
        }
        QComboBox::down-arrow {
            image: url(:/theme/chevron-@theme@.svg);
            width: 12px;
            height: 10px;
        }
        QComboBox::down-arrow:disabled {
            image: url(:/theme/chevron-@theme@-disabled.svg);
        }
        QPushButton[menuButton="true"] {
            padding-right: 24px;
        }
        QPushButton[menuButton="true"]::menu-indicator {
            subcontrol-origin: border;
            subcontrol-position: center right;
            right: 6px;
            width: 12px;
            height: 10px;
        }
        QToolButton#connectionAdvancedToggle {
            min-height: 28px;
            padding: 2px 6px;
            background: transparent;
            border-color: transparent;
        }
        QPushButton[iconOnlyAction="true"] {
            min-width: 26px;
            max-width: 26px;
            padding-left: 2px;
            padding-right: 2px;
        }
        QToolBar#primaryToolBar QToolButton {
            min-height: 28px;
            padding: 0;
        }
        QToolBar#primaryToolBar QToolButton[compactToolbarAction="true"] {
            max-height: 28px;
        }
        QToolBar#primaryToolBar QToolButton[compactToolbarAction="true"][toolbarTextAction="false"] {
            min-width: 28px;
            max-width: 28px;
        }
        QToolBar#primaryToolBar QToolButton[compactToolbarAction="true"][toolbarTextAction="true"] {
            min-width: 0;
            padding: 0 7px;
        }
        QToolBar#primaryToolBar QToolButton#activeWindowConnection {
            min-width: 0;
            max-height: 28px;
            padding: 0 22px 0 8px;
            color: %4;
            background: %23;
            border: 1px solid %6;
            border-radius: 4px;
        }
        QToolBar#primaryToolBar QToolButton#activeWindowConnection:hover,
        QToolBar#primaryToolBar QToolButton#activeWindowConnection:pressed {
            background: %9;
            border-color: %5;
        }
        QToolBar#documentToolBar QToolButton {
            min-width: 30px;
            min-height: 30px;
        }
        QToolButton:hover {
            background: %9;
            border-color: transparent;
        }
        QToolButton:pressed, QToolButton:checked {
            background: %9;
            border-color: transparent;
        }
        /* QLineEdit's native clear action is a compact inline control, not a
           toolbar button. Keep it centered and visually quiet in every view. */
        QLineEdit > QToolButton {
            min-width: 20px;
            max-width: 20px;
            min-height: 20px;
            max-height: 20px;
            margin: 0 3px 0 0;
            padding: 0;
            border: 0;
            border-radius: 10px;
            background: transparent;
        }
        QLineEdit > QToolButton[lineEditClearButton="true"]:hover {
            background: %9;
            border: 0;
        }
        QLineEdit > QToolButton[lineEditClearButton="true"]:pressed {
            background: %6;
            border: 0;
        }
        QLineEdit > QToolButton[lineEditClearButton="true"]:focus {
            border: 2px solid %7;
            background: transparent;
        }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QDateTimeEdit:focus, QKeySequenceEdit:focus {
            border-color: %7;
        }
        QSpinBox > QLineEdit, QDoubleSpinBox > QLineEdit, QDateTimeEdit > QLineEdit {
            border: 0;
            border-radius: 0;
            padding: 0;
            background: transparent;
        }
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow, QDateTimeEdit::up-arrow {
            image: url(:/theme/chevron-up-@theme@.svg);
            width: 10px;
            height: 8px;
        }
        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow, QDateTimeEdit::down-arrow {
            image: url(:/theme/chevron-@theme@.svg);
            width: 10px;
            height: 8px;
        }
        QSpinBox::up-arrow:disabled, QDoubleSpinBox::up-arrow:disabled, QDateTimeEdit::up-arrow:disabled {
            image: url(:/theme/chevron-up-@theme@-disabled.svg);
        }
        QSpinBox::down-arrow:disabled, QDoubleSpinBox::down-arrow:disabled, QDateTimeEdit::down-arrow:disabled {
            image: url(:/theme/chevron-@theme@-disabled.svg);
        }
        
        QCheckBox::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid %6;
            border-radius: 3px;
            background: %23;
        }
        QCheckBox::indicator:checked, QCheckBox::indicator:indeterminate {
            border-color: %7;
            background: %7;
        }
        QCheckBox::indicator:checked { image: url(:/theme/check-@theme@.svg); }
        QCheckBox::indicator:indeterminate { image: url(:/theme/mixed-@theme@.svg); }
        QCheckBox::indicator:hover { border-color: %7; }
        QCheckBox::indicator:disabled { border-color: %6; background: %3; }
        QCheckBox::indicator:checked:disabled, QCheckBox::indicator:indeterminate:disabled {
            border-color: %5;
            background: %5;
        }
        QAbstractItemView:focus, QTabBar::tab:focus {
            border: 1px solid %7;
        }
        QDockWidget {
            color: %4;
            background: %20;
        }
        QDockWidget::title {
            background: %20;
            border-bottom: 1px solid %6;
            padding: 5px 8px;
            font-size: 13px;
            font-weight: 600;
        }
        QTabWidget#navigationTabs::pane {
            background: %20;
            border: 0;
        }
        QTabWidget#navigationTabs QTabBar::tab {
            min-height: 26px;
            padding: 0 8px;
            background: %20;
        }
        QTabWidget#navigationTabs QTabBar::tab:selected {
            background: %21;
            border-top-color: %7;
        }
        QTabWidget::pane {
            border: 0;
            background: %21;
        }
        QTabWidget#commandOutputTabs::pane,
        QTabWidget#documentOutputTabs::pane,
        QTabWidget#queryResultTabs::pane {
            border-top: 1px solid %6;
        }
        QTabBar::tab {
            min-height: 26px;
            padding: 0 9px;
            background: %3;
            color: %4;
            border: 0;
            border-top: 2px solid transparent;
        }
        QTabBar::tab:disabled, QTabBar::tab:selected:disabled,
        QTabBar::tab:hover:disabled {
            color: %5;
        }
        QTabBar::scroller {
            width: 36px;
        }
        QTabBar QToolButton {
            min-width: 16px;
            max-width: 16px;
            min-height: 0;
            padding: 0;
            margin: 0;
            border: 1px solid %6;
            border-radius: 0;
            background: %3;
        }
        QToolButton#tabOverflowButton {
            min-width: 52px;
            max-width: 64px;
            min-height: 27px;
            max-height: 31px;
            padding: 0 22px 0 4px;
            border: 0;
            border-radius: 0;
            background: %3;
            color: %5;
        }
        QTabBar::tab:selected {
            background: %2;
            color: %4;
            border-top: 2px solid %7;
        }
        QTabBar::tab:hover:!selected {
            color: %4;
            background: %1;
        }
        QTabBar#documentTabBar::tab {
            min-height: 28px;
            max-width: 220px;
            padding: 0 10px 0 12px;
        }
        QTabBar#documentTabBar::close-button {
            image: url(:/icons/close.svg);
            width: 14px;
            height: 14px;
            margin-left: 5px;
            margin-right: 1px;
            border-radius: 4px;
        }
        QTabBar#documentTabBar::close-button:hover {
            background: %9;
        }
        QPlainTextEdit, QTextEdit, QTextBrowser, QLineEdit {
            background: %24;
            color: %4;
            border: 0;
            selection-background-color: %9;
            selection-color: %4;
        }
        QTreeWidget, QListWidget {
            background: %23;
            color: %4;
            border: 0;
            selection-background-color: %9;
            selection-color: %4;
        }
        QTreeWidget#objectTree, QTreeWidget#connectionNavigationList,
        QTreeWidget#workspaceTree, QListWidget#windowList, QListWidget#snippetList {
            background: %20;
            padding: 2px 4px;
        }
        QTreeWidget#objectTree::item, QTreeWidget#workspaceTree::item,
        QListWidget#windowList::item, QListWidget#snippetList::item {
            min-height: 26px;
        }
        QWidget#navigationDockTitle {
            background: %20;
            border-bottom: 1px solid %6;
        }
        QWidget#navigationDockTitle QTabBar::tab {
            background: %20;
        }
        QWidget#navigationDockTitle QTabBar::tab:selected {
            background: %2;
        }
        QTableWidget, QTableView {
            background: %25;
            color: %4;
            border: 0;
            selection-background-color: %9;
            selection-color: %4;
        }
        QPlainTextEdit, QTextEdit, QTextBrowser {
            selection-background-color: %16;
            selection-color: %17;
        }
        QPlainTextEdit[logContent="true"], QTableView[logContent="true"] {
            font-family: "MesloLGS NF", "@fixedFontFamily@", "Noto Serif CJK SC";
            font-size: @logFontPx@px;
        }
        QPlainTextEdit[logContent="true"] {
            padding: 4px 6px;
        }
        QPlainTextEdit#sqlEditor,
        QPlainTextEdit#commandEditor,
        QPlainTextEdit#testScriptEditor,
        QPlainTextEdit#programSpecificationEditor,
        QPlainTextEdit#programBodyEditor {
            border-left: 2px solid transparent;
        }
        QPlainTextEdit#sqlEditor:focus,
        QPlainTextEdit#commandEditor:focus,
        QPlainTextEdit#testScriptEditor:focus,
        QPlainTextEdit#programSpecificationEditor:focus,
        QPlainTextEdit#programBodyEditor:focus {
            border-left-color: %7;
        }
        QTreeView#sqlCompletionPopup {
            background: %2;
            border: 1px solid %6;
            border-radius: 4px;
        }
        QTreeView#sqlCompletionPopup::item {
            min-height: 24px;
            padding: 1px 6px;
        }
        QTreeView#sqlCompletionPopup::item:selected {
            background: %9;
            color: %4;
        }
        QTreeView#sqlCompletionPopup QHeaderView::section {
            min-height: 24px;
            padding: 2px 6px;
        }
        QTextBrowser {
            padding: 6px;
        }
        QLineEdit {
            min-height: 26px;
            padding: 0 8px;
            border: 1px solid %6;
            border-radius: 6px;
        }
        QSpinBox, QDoubleSpinBox, QDateTimeEdit, QKeySequenceEdit {
            min-height: 26px;
            padding: 0 25px 0 8px;
            color: %4;
            background: %2;
            border: 1px solid %6;
            border-radius: 6px;
            selection-background-color: %9;
            selection-color: %4;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button, QDateTimeEdit::up-button, QSpinBox::down-button, QDoubleSpinBox::down-button, QDateTimeEdit::down-button {
            subcontrol-origin: padding;
            width: 20px;
            background: %3;
            border: 0;
            border-left: 1px solid %6;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button, QDateTimeEdit::up-button {
            subcontrol-position: top right;
            border-top-right-radius: 4px;
        }
        QSpinBox::down-button, QDoubleSpinBox::down-button, QDateTimeEdit::down-button {
            subcontrol-position: bottom right;
            border-bottom-right-radius: 4px;
        }
        QCheckBox, QRadioButton {
            min-height: 24px;
            spacing: 7px;
            color: %4;
        }
        QCheckBox::indicator, QRadioButton::indicator {
            width: 16px;
            height: 16px;
        }
        QGroupBox {
            margin-top: 12px;
            padding-top: 10px;
            border: 1px solid %6;
            border-radius: 7px;
            font-weight: 600;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 5px;
            background: %2;
        }
        QTreeWidget::item {
            min-height: 27px;
            border-radius: 4px;
        }
        QTreeWidget#connectionNavigationList::item {
            min-height: 26px;
            padding: 0 4px;
        }
        QListWidget::item {
            min-height: 27px;
            padding: 0 5px;
            border-radius: 3px;
        }
        QTreeWidget::item:selected, QListWidget::item:selected {
            background: %9;
            color: %4;
        }
        QTreeWidget::item:hover:!selected, QListWidget::item:hover:!selected {
            background: %9;
        }
        QHeaderView, QTableCornerButton::section {
            background: %3;
            border: 0;
        }
        QHeaderView::section {
            background: %3;
            color: %5;
            border: 0;
            border-bottom: 1px solid %6;
            padding: 4px 7px;
            font-weight: 600;
        }
        QLabel#contextHeading, QLabel#secondaryText, QLabel#settingsCompletionSoundNote {
            color: %5;
        }
        QLabel#menuVersionLabel {
            color: %5;
        }
        QLabel[dialogLead="true"] {
            color: %5;
            padding: 2px 0 6px 0;
        }
        QLabel[callout="info"] {
            color: %18;
            background: %19;
            border: 1px solid %6;
            border-radius: 6px;
            padding: 8px 10px;
        }
        QLabel[callout="warning"] {
            color: %11;
            background: %12;
            border: 1px solid %10;
            border-radius: 6px;
            padding: 8px 10px;
        }
        QLabel[callout="danger"] {
            color: %8;
            background: %13;
            border: 1px solid %8;
            border-radius: 6px;
            padding: 8px 10px;
        }
        QLabel[emptyState="true"] {
            color: %5;
            padding: 10px;
        }
        QLabel[sectionHeading="true"] {
            color: %4;
            font-weight: 600;
        }
        QLabel#documentSessionState, QLabel#documentUserState,
        QLabel#documentTransactionState, QLabel#documentEnvironmentState, QLabel#resultSummary,
        QLabel#resultLoadState {
            color: %5;
        }
        QLabel#documentTransactionState[pending="true"] {
            color: %8;
            font-weight: 600;
        }
        QLabel#documentEnvironmentState[environment="test"] {
            color: %10;
            font-weight: 600;
        }
        QLabel#documentEnvironmentState[environment="production"] {
            color: %8;
            font-weight: 700;
        }
        QLabel#environmentBanner {
            border-bottom: 1px solid %6;
            font-weight: 600;
        }
        QLabel#environmentBanner[environment="test"] {
            color: %11;
            background: %12;
        }
        QLabel#environmentBanner[environment="production"] {
            color: %8;
            background: %13;
        }
        QLabel#resultSummary {
            background: %3;
            color: %4;
            border-bottom: 1px solid %6;
            font-weight: 600;
        }
        QLabel#connectionState {
            color: %4;
            font-weight: 600;
        }
        QLabel#userState, QLabel#resultState {
            color: %5;
        }
        QLabel#transactionState[pending="true"] {
            color: %8;
            font-weight: 600;
        }
        QPushButton {
            min-height: 26px;
            padding: 0 9px;
            color: %4;
            background: %23;
            border: 1px solid %6;
            border-radius: 6px;
        }
        QPushButton:hover {
            background: %9;
            border-color: %6;
        }
        QPushButton:pressed {
            background: %9;
        }
        QPushButton:focus, QToolButton:focus {
            border: 1px solid %7;
        }
        QPushButton:disabled, QToolButton:disabled, QComboBox:disabled {
            color: %5;
        }
        QPushButton:default:enabled {
            color: %17;
            background: %7;
            border-color: %7;
            font-weight: 600;
        }
        QPushButton[dbaNavigationAction="true"] {
            text-align: left;
            padding: 5px 8px;
        }
        QDialogButtonBox {
            min-height: 32px;
        }
        QListWidget#settingsCategoryList {
            background: %20;
            border: 1px solid %6;
            border-radius: 6px;
            padding: 4px;
        }
        QListWidget#settingsCategoryList::item {
            padding: 5px 9px;
        }
        QLabel#settingsCategoryHeading {
            padding: 0 6px 6px 6px;
            border-bottom: 1px solid %6;
        }
        QDialogButtonBox QPushButton {
            min-width: 76px;
        }
        QMessageBox {
            background: %22;
        }
        QMessageBox QLabel {
            padding: 4px 2px;
        }
        QMessageBox QLabel#qt_msgbox_label,
        QMessageBox QLabel#qt_msgbox_informativelabel {
            min-width: 260px;
        }
        QMessageBox QLabel#qt_msgbox_label {
            font-weight: 600;
        }
        QMessageBox QLabel#qt_msgboxex_icon_label {
            min-width: 0px;
            padding: 0px;
        }
        QMessageBox QPushButton {
            min-width: 84px;
        }
        QDialog#commandVariableDialog {
            background: %22;
        }
        QDialog#commandVariableDialog QLabel#commandVariableDescription,
        QDialog#commandVariableDialog QLabel#commandVariableSource {
            color: %5;
        }
        QDialog#commandVariableDialog QLineEdit {
            min-height: 26px;
            padding: 1px 7px;
            border-radius: 4px;
        }
        QDialog#commandVariableDialog QPlainTextEdit {
            padding: 4px;
            border: 1px solid %6;
            border-radius: 4px;
        }
        QDialog#commandVariableDialog QPushButton {
            min-width: 58px;
            min-height: 26px;
            padding: 0 9px;
            border-radius: 4px;
        }
        QDialog#commandVariableDialog QDialogButtonBox {
            min-height: 28px;
        }
        QDialog#findReplaceDialog {
            min-width: 460px;
            background: %22;
        }
        QDialog#findReplaceDialog QLabel#findReplaceStatus {
            color: %5;
        }
        QDialog#findReplaceDialog QWidget#findReplaceFields,
        QDialog#findReplaceDialog QWidget#findReplaceOptions {
            background: transparent;
            border: 0;
        }
        QDialog#findReplaceDialog QWidget#findReplaceFields QLabel {
            color: %5;
            font-weight: normal;
        }
        QDialog#findReplaceDialog QWidget#findReplaceFields QLineEdit {
            min-height: 26px;
            padding: 1px 7px;
            border-radius: 4px;
            background: %2;
        }
        QDialog#findReplaceDialog QPushButton {
            min-width: 58px;
            min-height: 26px;
            padding: 0 9px;
            border-radius: 4px;
            font-weight: normal;
        }
        QDialog#findReplaceDialog QDialogButtonBox#findReplaceButtonBox {
            min-height: 28px;
            border: 0;
            padding: 0;
        }
        QWidget#editorFindBar,
        QWidget#resultFindBar {
            background: %3;
            border-top: 1px solid %6;
            border-bottom: 1px solid %6;
        }
        QWidget#editorFindBar QLabel,
        QWidget#resultFindBar QLabel {
            color: %5;
            font-weight: 600;
        }
        QWidget#editorFindBar QLineEdit,
        QWidget#resultFindBar QLineEdit {
            min-height: 28px;
            background: %2;
        }
        QWidget#editorFindBar QPushButton,
        QWidget#resultFindBar QPushButton {
            min-height: 28px;
            min-width: 74px;
        }
        QWidget#editorFindBar QToolButton,
        QWidget#resultFindBar QToolButton {
            min-width: 28px;
            max-width: 28px;
            min-height: 28px;
            max-height: 28px;
            font-size: 16px;
        }
        QDialog#parameterDetailsDialog {
            background: %22;
        }
        QDialog#parameterDetailsDialog QWidget#parameterDetailsHeading {
            background: %22;
            border-bottom: 1px solid %6;
            padding-bottom: 8px;
        }
        QDialog#parameterDetailsDialog QLabel#parameterDetailsTitle {
            color: %4;
            font-size: 16px;
            font-weight: 600;
        }
        QDialog#parameterDetailsDialog QLabel#parameterDetailsSubtitle,
        QDialog#parameterDetailsDialog QLabel#parameterDetailsFeedback {
            color: %5;
        }
        QDialog#parameterDetailsDialog QScrollArea#parameterDetailsScroll {
            background: %22;
            border: 1px solid %6;
            border-radius: 6px;
        }
        QDialog#parameterDetailsDialog QWidget#parameterDetailsSummary {
            background: %3;
            border: 1px solid %6;
            border-radius: 6px;
        }
        QDialog#parameterDetailsDialog QLabel#parameterDetailsMetaLabel,
        QDialog#parameterDetailsDialog QLabel#parameterDetailsSectionHeading {
            color: %5;
            font-weight: 600;
        }
        QDialog#parameterDetailsDialog QLabel#parameterDetailsMetaValue,
        QDialog#parameterDetailsDialog QLabel#parameterDetailsBody {
            color: %4;
        }
        QDialog#parameterDetailsDialog QLabel#parameterDetailsTechnical {
            background: %3;
            color: %4;
            border: 1px solid %6;
            border-radius: 5px;
            padding: 8px;
        }
        QDialog#parameterDetailsDialog QDialogButtonBox#parameterDetailsButtonBox {
            border-top: 1px solid %6;
            padding-top: 10px;
        }
        QDialog#parameterDetailsDialog QDialogButtonBox QPushButton {
            min-width: 108px;
            min-height: 30px;
        }
        QProgressBar {
            min-height: @progress-height@px;
            max-height: @progress-height@px;
            font-family: "Noto Serif CJK SC";
            font-size: 12px;
            color: %4;
            background: %3;
            border: 0;
            border-radius: 4px;
            text-align: center;
        }
        QProgressBar::chunk {
            background: %26;
            border-radius: 4px;
        }
        QProgressBar:disabled {
            color: %5;
        }
        QProgressBar::chunk:disabled {
            background: %6;
        }
        QToolTip {
            padding: 6px 8px;
            color: %4;
            background: %22;
            border: 1px solid %6;
            border-radius: 5px;
        }
        QScrollArea {
            background: %21;
            border: 0;
        }
        QDialog#connectionDialog QWidget#connectionProfileBar {
            min-height: 38px;
            background: %3;
            border: 1px solid %6;
            border-radius: 7px;
        }
        QDialog#connectionDialog QWidget#connectionAdvancedPanel {
            background: %3;
            border: 1px solid %6;
            border-radius: 7px;
        }
        QDialog#connectionDialog QLabel#connectionStatus,
        QDialog#updateDialog QLabel#updateStatusLabel,
        QDialog#versionNotesDialog QLabel#versionNotesMessage {
            color: %5;
        }
        QDialog#connectionDialog QLabel#connectionStatus[callout="danger"] {
            color: %8;
        }
        QDialog#connectionFailureDialog QLabel#connectionFailureHeading {
            font-weight: 600;
        }
        QDialog#connectionFailureDialog QLabel#connectionFailureDetail {
            color: %4;
        }
        QDialog#updateDialog QLabel#updateCurrentVersion,
        QDialog#updateDialog QLabel#updateStatusTitle {
            font-weight: 600;
        }
        QDialog#updateDialog QLabel#updateVersionCaption,
        QDialog#updateDialog QLabel#updateChannelLabel,
        QDialog#updateDialog QLabel#updateDetailsLabel {
            color: %5;
        }
        QDialog#updateDialog QWidget#updateVersionPanel {
            background: transparent;
            border: 0;
        }
        QDialog#updateDialog QLabel#updateCurrentVersion {
            background: %9;
            border-radius: 4px;
            padding: 3px 8px;
        }
        QDialog#updateDialog QWidget#updateStatusPanel {
            background: %3;
            border: 1px solid %6;
            border-radius: 6px;
        }
        QDialog#updateDialog QWidget#updateAdvancedPanel {
            background: %3;
            border: 1px solid %6;
            border-radius: 6px;
        }
        QDialog#updateDialog QWidget#updateActionBar {
            background: transparent;
            border: 0;
        }
        QDialog#updateDialog QWidget#updateActionBar QPushButton {
            min-width: 76px;
            min-height: 28px;
            padding: 0 10px;
        }
        QDialog#updateDialog QToolButton#updateAdvancedToggle,
        QDialog#updateDialog QToolButton#updateDetailsButton {
            min-height: 24px;
            padding: 2px 6px;
            background: transparent;
            border: 1px solid transparent;
        }
        QDialog#updateDialog QWidget#updateStatusPanel[statusKind="success"] {
            background: %9;
            border-color: %7;
        }
        QDialog#updateDialog QWidget#updateStatusPanel[statusKind="error"] {
            background: %13;
            border-color: %8;
        }
        QDialog#updateDialog QWidget#updateStatusPanel[statusKind="error"] QLabel#updateStatusTitle {
            color: %8;
        }
        QDialog#updateDialog QTextBrowser#updateReleaseNotes {
            background: %3;
            border: 1px solid %6;
            border-radius: 6px;
            padding: 6px 8px;
        }
        QDialog#versionNotesDialog QListWidget#versionNotesList {
            background: %3;
            border: 1px solid %6;
            border-radius: 6px;
            padding: 4px;
        }
        QDialog#versionNotesDialog QListWidget#versionNotesList::item {
            min-height: 32px;
            padding: 0 8px;
        }
        QDialog#versionNotesDialog QTextBrowser#versionNotesDetails {
            background: %2;
            border: 1px solid %6;
            border-radius: 4px;
            padding: 10px 12px;
        }
        QDialog#versionNotesDialog QLabel#versionNotesHeading {
            font-size: 16px;
            font-weight: 600;
        }
        QDialog#versionNotesDialog QLabel#versionNotesListCaption {
            font-weight: 600;
        }
        QDialog#helpDocumentDialog QWidget {
            font-size: 11pt;
        }
        QDialog#helpDocumentDialog QLabel#helpDocumentHeading {
            font-size: 18pt;
            font-weight: 600;
        }
        QDialog#versionNotesDialog QLabel#versionNotesCurrentVersion {
            font-weight: 600;
            background: %9;
            border-radius: 5px;
            padding: 3px 8px;
        }
        QDialog#versionNotesDialog QPushButton#closeVersionNotesButton,
        QDialog#updateDialog QPushButton#closeUpdateButton,
        QDialog#updateDialog QPushButton#openUpdateFolderButton,
        QDialog#updateDialog QPushButton#cancelUpdateButton {
            color: %4;
            background: %2;
            border: 1px solid %6;
            border-radius: 4px;
            min-height: 28px;
            padding: 0 12px;
            font-weight: 400;
        }
        QDialog#versionNotesDialog QPushButton#closeVersionNotesButton:hover,
        QDialog#updateDialog QPushButton#closeUpdateButton:hover,
        QDialog#updateDialog QPushButton#openUpdateFolderButton:hover,
        QDialog#updateDialog QPushButton#cancelUpdateButton:hover {
            background: %9;
            border-color: %7;
        }
        QDialog#versionNotesDialog QLabel#versionNotesCurrentVersionCaption,
        QDialog#versionNotesDialog QLabel#versionNotesResultCount {
            color: %5;
        }
        QDialog#testDataDialog QTableWidget#testDataColumnsTable,
        QDialog#testDataDialog QPlainTextEdit#testDataSqlPreview {
            border: 1px solid %6;
            border-radius: 6px;
        }
        QDialog#testDataDialog QLabel#testDataState {
            min-height: 20px;
        }
        QDialog#systemInfoDialog QLabel#systemInfoContext,
        QDialog#systemInfoDialog QLabel#systemInfoSource {
            color: %5;
        }
        QDialog#systemInfoDialog QWidget#systemInfoProperties {
            background: transparent;
        }
        QDialog#systemInfoDialog QLabel[systemInfoPropertyCaption="true"] {
            color: %5;
            font-weight: 600;
        }
        QDialog#systemInfoDialog QLabel#systemInfoPropertyValue {
            color: %4;
        }
        QDialog#systemInfoDialog QTableWidget#systemInfoTable {
            background: %2;
            border: 1px solid %6;
            border-radius: 4px;
        }
        QDialog#systemInfoDialog QTableWidget#systemInfoTable QHeaderView::section {
            min-height: 26px;
            padding: 3px 7px;
        }
        QPushButton#closeSystemInfo {
            font-weight: normal;
        }
        QPushButton[quietAction="true"],
        QToolButton[quietAction="true"] {
            min-height: 26px;
            padding: 0 8px;
            background: transparent;
            border: 1px solid transparent;
            border-radius: 4px;
        }
        QPushButton[quietAction="true"]:hover,
        QToolButton[quietAction="true"]:hover {
            background: %9;
            border-color: transparent;
        }
        QToolButton[quietAction="true"][menuButton="true"] {
            padding-right: 24px;
        }
        QToolBar[documentContext="true"] QPushButton,
        QToolBar[secondaryToolBar="true"] QPushButton {
            background: transparent;
            border-color: transparent;
        }
        QToolBar[documentContext="true"] QPushButton:hover,
        QToolBar[secondaryToolBar="true"] QPushButton:hover {
            background: %9;
            border-color: transparent;
        }
        QToolButton[contextControl="true"] {
            background: %3;
            border-color: transparent;
        }
        QToolBar#commandContextToolBar QToolButton {
            min-height: 26px;
            max-height: 26px;
            margin: 0;
            padding-top: 0;
            padding-bottom: 0;
        }
        QToolBar#commandContextToolBar QComboBox {
            background: %3;
            border-color: transparent;
            min-height: 26px;
            max-height: 26px;
            margin: 0;
            padding-top: 0;
            padding-bottom: 0;
        }
        QToolBar#commandContextToolBar QComboBox QLineEdit {
            min-height: 0;
            padding-top: 0;
            padding-bottom: 0;
            background: transparent;
            border: 0;
        }
        QToolBar#commandContextToolBar QComboBox:hover,
        QToolBar#commandContextToolBar QComboBox:focus {
            background: %9;
            border-color: transparent;
        }
        QWidget#resultControlBar {
            min-height: 32px;
            background: %3;
            border-bottom: 1px solid %6;
        }
        QLabel#resultWorkspaceLabel {
            color: %4;
            font-size: 13px;
            font-weight: 700;
            padding-right: 2px;
        }
        QLabel#resultSummary {
            color: %4;
            background: transparent;
            border: 0;
            font-weight: 500;
        }
        QLabel#resultModeBadge[statusBadge="true"] {
            color: %7;
            background: %9;
            border: 1px solid %6;
            border-radius: 8px;
            padding: 2px 7px;
            font-size: 11px;
            font-weight: 700;
        }
        QLabel#resultModeBadge[editableResult="true"] {
            color: %14;
            background: %15;
            border-color: %15;
        }
        QLabel#resultLoadState,
        QLabel#resultDisplayCaption,
        QLabel#resultFilterCaption {
            color: %5;
        }
        QLabel#resultPageState {
            color: %4;
            font-weight: 600;
            padding: 0 4px;
        }
        QWidget#resultGridToolbar {
            min-height: 32px;
            background: %2;
            border-bottom: 1px solid %6;
        }
        QWidget#resultGridToolbar QLineEdit#resultGridFilter,
        QWidget#resultGridToolbar QComboBox#resultDisplayPreset {
            min-height: 26px;
            background: %2;
            border: 1px solid %6;
            border-radius: 4px;
        }
        QWidget#resultGridToolbar QLineEdit#resultGridFilter:focus,
        QWidget#resultGridToolbar QComboBox#resultDisplayPreset:focus {
            border: 1px solid %7;
        }
        QWidget#resultEditActions {
            background: transparent;
        }
        QLabel#resultEditState[statusBadge="true"] {
            color: %14;
            background: %15;
            border: 0;
            border-radius: 4px;
            padding: 2px 5px;
            font-weight: 600;
        }
        QPushButton#deleteResultRows:enabled {
            color: %8;
        }
        QWidget#resultGridPane,
        QTableView[resultGrid="true"] {
            background: %2;
            border: 1px solid transparent;
        }
        QTableView[resultGrid="true"]:focus {
            border: 1px solid transparent;
            border-left-color: %7;
        }
        QTableView[resultGrid="true"] QHeaderView::section {
            min-height: 28px;
            padding: 3px 7px;
            background: %3;
            border-bottom: 1px solid %6;
        }
        QTableView[resultGrid="true"]::item {
            padding: 2px 6px;
        }
        QLabel#resultEmptyTitle, QLabel[resultPaneEmptyTitle="true"] {
            color: %4;
            font-size: 15px;
            font-weight: 600;
        }
        QLabel#resultEmptyDetail, QLabel[resultPaneEmptyDetail="true"] {
            color: %5;
        }
        QAbstractItemView {
            gridline-color: %6;
        }
        QScrollBar:vertical {
            width: 10px;
            margin: 1px;
            background: transparent;
        }
        QScrollBar:horizontal {
            height: 10px;
            margin: 1px;
            background: transparent;
        }
        QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
            min-height: 28px;
            min-width: 28px;
            background: %6;
            border-radius: 4px;
        }
        QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover {
            background: %5;
        }
        QScrollBar::add-line, QScrollBar::sub-line,
        QScrollBar::add-page, QScrollBar::sub-page {
            width: 0;
            height: 0;
            background: transparent;
        }
        QPushButton[role="primary"]:enabled,
        QToolButton[role="primary"]:enabled {
            color: %17;
            background: %7;
            border-color: %7;
            font-weight: 600;
        }
        QPushButton[role="danger"]:enabled,
        QToolButton[role="danger"]:enabled {
            color: %17;
            background: %8;
            border-color: %8;
            font-weight: 600;
        }
        QMessageBox[unifiedPrompt="true"] QLabel {
            padding: 0;
        }
        QMessageBox[unifiedPrompt="true"] QLabel#qt_msgbox_label {
            font-size: 15px;
            font-weight: 600;
        }
        QMessageBox[unifiedPrompt="true"] QPushButton {
            min-width: 72px;
            min-height: 24px;
            padding: 5px 14px;
            border-radius: 6px;
            font-weight: normal;
        }
        QMessageBox[unifiedPrompt="true"] QPushButton[role="secondary"]:enabled,
        QMessageBox[unifiedPrompt="true"] QPushButton[role="secondary"]:default:enabled,
        QDialog[confirmationForm="true"] QPushButton[role="secondary"]:enabled,
        QInputDialog QPushButton[role="secondary"]:enabled {
            background: %23;
            color: %4;
            border: 1px solid %6;
        }
        QMessageBox[unifiedPrompt="true"] QPushButton[role="secondary"]:focus {
            border-color: %7;
        }
        QMessageBox[unifiedPrompt="true"] QPushButton[role="secondary"]:hover {
            background: %9;
        }
        QMessageBox[unifiedPrompt="true"] QPushButton[role="primary"]:enabled {
            background: %7;
            color: %17;
            border-color: %7;
        }
        QMessageBox[unifiedPrompt="true"] QPushButton[role="danger"]:enabled {
            background: %23;
            color: %8;
            border: 1px solid %6;
            font-weight: normal;
        }
        QMessageBox[unifiedPrompt="true"] QPushButton[role="danger"]:hover {
            background: %13;
            border-color: %8;
        }
        QMessageBox[unifiedPrompt="true"] QPushButton[role="danger"]:focus {
            border-color: %8;
        }
        QStatusBar {
            background: %3;
            border-top: 1px solid %6;
            min-height: 24px;
        }
        QStatusBar::item {
            border: 0;
        }
        QStatusBar QLabel[statusSegment="true"] {
            min-height: 20px;
            padding: 0 7px;
            border-right: 0;
        }
        QStatusBar QLabel[statusSegment="last"] {
            border-right: 0;
        }
        QMainWindow::separator {
            width: 5px;
            height: 5px;
            background: %3;
        }
        QMainWindow::separator:vertical {
            border-left: 1px solid %6;
        }
        QMainWindow::separator:horizontal {
            border-top: 1px solid %6;
        }
        QMainWindow::separator:hover {
            background: %7;
        }
        QSplitter::handle {
            background: transparent;
        }
        QSplitter::handle:horizontal {
            width: 3px;
            border-left: 1px solid %6;
        }
        QSplitter::handle:vertical {
            height: 3px;
            border-top: 1px solid %6;
        }
        QSplitter::handle:hover {
            background: %9;
        }
    )")
        .arg(colors.window, colors.surface, colors.surfaceMuted, colors.text,
             colors.textMuted, colors.border, colors.accent, colors.danger,
             colors.focusSurface)
        .arg(colors.warning, colors.warningText, colors.warningSurface,
             colors.dangerSurface, colors.success, colors.successSurface,
             colors.selection, colors.selectionText, colors.info)
        .arg(colors.infoSurface, colors.sidebar, colors.content, colors.raised,
             colors.control, colors.editor, colors.result)
        .arg(colors.progressFill)
        .replace(QStringLiteral("@progress-height@"), QString::number(progressHeight))
        .replace(QStringLiteral("@theme@"), dark ? QStringLiteral("dark") : QStringLiteral("light"))
        .replace(QStringLiteral("@fixedFontFamily@"), QFontDatabase::systemFont(QFontDatabase::FixedFont).family())
        .replace(QStringLiteral("@logFontPx@"), QString::number(LogView::fontPixels));
}

}  // namespace

void Theme::applyStartupAppearance(QWidget& widget) {
    // Startup keeps its night palette in either workspace theme. Controls use
    // the same semantic colors and states as the shared workbench controls.
    const auto color = [](SemanticColor role) { return semanticColor(role, true).name(); };
    widget.setPalette(createPalette(true));
    widget.setStyleSheet(QStringLiteral(R"(
        QDialog#startupSplash { background: transparent; }
        QDialog#startupSplash QFrame#startupRecoveryPanel {
            background: transparent; color: %2; border: none;
        }
        QDialog#startupSplash QLabel { color: %2; background: transparent; }
        QDialog#startupSplash QLabel#startupRecoveryTitle { font-size: 17px; font-weight: 600; }
        QDialog#startupSplash QLabel#workspaceHistoryDescription { color: %4; }
        QDialog#startupSplash QTreeWidget {
            background: %1; alternate-background-color: %1;
            color: %2; border: 1px solid %3; border-radius: 8px;
            show-decoration-selected: 1;
            selection-background-color: %6; selection-color: %2;
        }
        QDialog#startupSplash QTreeWidget::item {
            padding: 8px 12px; min-height: 20px; margin: 0;
            border: none; border-radius: 0;
        }
        QDialog#startupSplash QTreeWidget::item:hover { background: %5; }
        QDialog#startupSplash QTreeWidget::item:selected { background: %6; color: %2; }
        QDialog#startupSplash QTreeWidget::item:disabled { color: %4; }
        QDialog#startupSplash QHeaderView { background: %5; border: none; }
        QDialog#startupSplash QHeaderView::section {
            background: %5; color: %4; border: none;
            border-bottom: 1px solid %3; padding: 8px 12px;
        }
        QDialog#startupSplash QScrollBar { background: %1; border: none; }
        QDialog#startupSplash QScrollBar:vertical { width: 9px; }
        QDialog#startupSplash QScrollBar:horizontal { height: 9px; }
        QDialog#startupSplash QScrollBar::handle { background: %3; border-radius: 4px; }
        QDialog#startupSplash QScrollBar::handle:vertical { min-height: 24px; }
        QDialog#startupSplash QScrollBar::handle:horizontal { min-width: 24px; }
        QDialog#startupSplash QScrollBar::handle:hover { background: %4; }
        QDialog#startupSplash QScrollBar::add-line, QDialog#startupSplash QScrollBar::sub-line {
            width: 0; height: 0;
        }
        QDialog#startupSplash QScrollBar::add-page, QDialog#startupSplash QScrollBar::sub-page { background: transparent; }
        QDialog#startupSplash QPushButton {
            color: %2; background: %5; border: 1px solid %3; border-radius: 6px;
            padding: 6px 14px; min-height: 26px;
        }
        QDialog#startupSplash QPushButton:hover { background: %6; }
        QDialog#startupSplash QPushButton:pressed { background: %3; }
        QDialog#startupSplash QPushButton:disabled { color: %4; }
        QDialog#startupSplash QPushButton:focus { border-color: %7; }
        QDialog#startupSplash QPushButton#restoreWorkspaceInstances {
            background: %7; color: %8; border-color: %7;
        }
        QDialog#startupSplash QPushButton#restoreWorkspaceInstances:hover { background: %6; color: %2; }
        QDialog#startupSplash QPushButton#restoreWorkspaceInstances:disabled { background: %5; color: %4; border-color: %3; }
        QDialog#startupSplash QPushButton#startupDismiss { background: transparent; border-color: transparent; }
        QDialog#startupSplash QPushButton#startupDismiss:hover { background: %5; }
    )").arg(color(SemanticColor::Content), color(SemanticColor::Text), color(SemanticColor::Control),
        color(SemanticColor::MutedText), color(SemanticColor::Raised), color(SemanticColor::AccentSurface),
        color(SemanticColor::Accent), color(SemanticColor::SelectionText)));
}

QFont Theme::editorFont() {
    loadApplicationFonts();
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setFamilies({QStringLiteral("MesloLGS NF"), font.family(), QStringLiteral("Noto Serif CJK SC")});
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    if (font.pixelSize() > 0) font.setPixelSize(qMax(1, qRound(font.pixelSize() * runtimeScaleFactor())));
    else if (font.pointSizeF() > 0) font.setPointSizeF(runtimeFontSize(font.pointSizeF()));
    return font;
}

bool Theme::applyUiScale(QApplication& application, int percent) {
    return MacDisplayScale::enabled() && RuntimeUiScale::get(application).apply(percent);
}

int Theme::uiScalePercent() { return RuntimeUiScale::percent(); }
qreal Theme::runtimeScaleFactor() { return RuntimeUiScale::factor(); }
qreal Theme::runtimeFontSize(qreal size) { return size * runtimeScaleFactor(); }
bool Theme::isUiScaleChangeEvent(const QEvent* event) {
    return event && event->type() == RuntimeUiScale::changedEventType();
}

int Theme::uiScaleBasisPercent() {
    return MacDisplayScale::enabled() ? RuntimeUiScale::startupBasisPercent() : 100;
}

qreal Theme::restoredUiScaleRatio(int savedBasis) {
    if (!MacDisplayScale::enabled() || savedBasis < MacDisplayScale::minimumPercent
            || savedBasis > MacDisplayScale::maximumPercent) return 1;
    return qreal(savedBasis) / uiScaleBasisPercent();
}

QVariantMap Theme::windowScaleState(const QMainWindow& window) {
    QVariantMap docks;
    for (auto* dock : window.findChildren<QDockWidget*>(QString(), Qt::FindDirectChildrenOnly))
        if (!dock->objectName().isEmpty()) docks.insert(dock->objectName(), dock->size());
    return {{QStringLiteral("basisPercent"), uiScaleBasisPercent()},
        {QStringLiteral("normalGeometry"), window.isMaximized() || window.isFullScreen()
            ? window.normalGeometry() : window.geometry()},
        {QStringLiteral("maximized"), window.isMaximized()},
        {QStringLiteral("fullScreen"), window.isFullScreen()},
        {QStringLiteral("docks"), docks}};
}

void Theme::restoreWindowScaleState(QMainWindow& window, const QVariantMap& state) {
    const qreal ratio = restoredUiScaleRatio(state.value(QStringLiteral("basisPercent")).toInt());
    if (qFuzzyCompare(ratio, 1.0)) return; // Legacy records keep their original restoration path.
    const QRect saved = state.value(QStringLiteral("normalGeometry")).toRect();
    if (saved.isValid()) {
        QRect geometry(qRound(saved.x() * ratio), qRound(saved.y() * ratio),
            qRound(saved.width() * ratio), qRound(saved.height() * ratio));
        if (auto* screen = window.screen()) {
            const QRect room = screen->availableGeometry().adjusted(12, 12, -12, -12);
            geometry.setSize(geometry.size().boundedTo(room.size()));
            geometry.moveLeft(qBound(room.left(), geometry.left(), qMax(room.left(), room.right() - geometry.width() + 1)));
            geometry.moveTop(qBound(room.top(), geometry.top(), qMax(room.top(), room.bottom() - geometry.height() + 1)));
        }
        window.setGeometry(geometry);
    }
    const auto docks = state.value(QStringLiteral("docks")).toMap();
    for (auto* dock : window.findChildren<QDockWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        const QSize savedSize = docks.value(dock->objectName()).toSize();
        if (!savedSize.isValid()) continue;
        const QSize size(qMax(1, qRound(savedSize.width() * ratio)), qMax(1, qRound(savedSize.height() * ratio)));
        if (dock->isFloating()) dock->resize(size);
        else {
            window.resizeDocks({dock}, {size.width()}, Qt::Horizontal);
            window.resizeDocks({dock}, {size.height()}, Qt::Vertical);
        }
    }
    auto states = window.windowState();
    states.setFlag(Qt::WindowMaximized, state.value(QStringLiteral("maximized")).toBool());
    states.setFlag(Qt::WindowFullScreen, state.value(QStringLiteral("fullScreen")).toBool());
    window.setWindowState(states);
}

void Theme::apply(QApplication& application, ThemeMode mode) {
    TextTransformMenu::install(application);
    loadApplicationFonts();
    QFont uiFont = application.font();
    uiFont.setFamily(QStringLiteral("Noto Serif CJK SC"));
    uiFont.setStyleHint(QFont::Serif);
    if (!application.property("diagnosticTextInteractionInstalled").toBool()) {
        application.installEventFilter(new DiagnosticTextInteraction(&application));
        application.setProperty("diagnosticTextInteractionInstalled", true);
    }
    if (!application.property("workbenchArrowStyleInstalled").toBool()) {
        application.setStyle(new WorkbenchStyle(application.style()->objectName()));
        application.setProperty("workbenchArrowStyleInstalled", true);
    }
    // Installing a native style can reset class-specific fonts on Wine.
    application.setFont(uiFont);
    application.setFont(uiFont, "QMenu");
    application.setFont(uiFont, "QMenuBar");
    if (!application.property("dialogButtonLocalizerInstalled").toBool()) {
        application.installEventFilter(new DialogButtonLocalizer(&application));
        application.setProperty("dialogButtonLocalizerInstalled", true);
    }
    if (!application.property("lineEditClearButtonAccessibilityInstalled").toBool()) {
        application.installEventFilter(new LineEditClearButtonAccessibility(&application));
        application.setProperty("lineEditClearButtonAccessibilityInstalled", true);
    }
    const bool dark = mode == ThemeMode::Dark
        || (mode == ThemeMode::System && systemUsesDarkColors(application));
    application.setPalette(createPalette(dark));
    if (MacDisplayScale::enabled()) RuntimeUiScale::get(application).setApplicationStyleSheet(createStyleSheet(dark));
    else application.setStyleSheet(createStyleSheet(dark));
}

QColor Theme::semanticColor(SemanticColor role, bool dark) {
    const Colors colors = colorsForTheme(dark);
    switch (role) {
        case SemanticColor::Window: return QColor(colors.window);
        case SemanticColor::Sidebar: return QColor(colors.sidebar);
        case SemanticColor::Content: return QColor(colors.content);
        case SemanticColor::Raised: return QColor(colors.raised);
        case SemanticColor::Control: return QColor(colors.control);
        case SemanticColor::Editor: return QColor(colors.editor);
        case SemanticColor::Result: return QColor(colors.result);
        case SemanticColor::Text: return QColor(colors.text);
        case SemanticColor::ProgressFill: return QColor(colors.progressFill);
        case SemanticColor::Accent:
            return QColor(colors.accent);
        case SemanticColor::AccentSurface:
            return QColor(colors.focusSurface);
        case SemanticColor::Selection:
            return QColor(colors.selection);
        case SemanticColor::SelectionText:
            return QColor(colors.selectionText);
        case SemanticColor::MutedText:
            return QColor(colors.textMuted);
        case SemanticColor::Success:
            return QColor(colors.success);
        case SemanticColor::SuccessSurface:
            return QColor(colors.successSurface);
        case SemanticColor::Warning:
            return QColor(colors.warning);
        case SemanticColor::WarningSurface:
            return QColor(colors.warningSurface);
        case SemanticColor::Danger:
            return QColor(colors.danger);
        case SemanticColor::DangerSurface:
            return QColor(colors.dangerSurface);
        case SemanticColor::Info: return QColor(colors.info);
        case SemanticColor::InfoSurface: return QColor(colors.infoSurface);
        case SemanticColor::SyntaxKeyword: return QColor(colors.syntaxKeyword);
        case SemanticColor::SyntaxComment: return QColor(colors.syntaxComment);
        case SemanticColor::SyntaxString: return QColor(colors.syntaxString);
        case SemanticColor::SyntaxNumber: return QColor(colors.syntaxNumber);
        case SemanticColor::SyntaxSymbol: return QColor(colors.syntaxSymbol);
        case SemanticColor::SyntaxCustom: return QColor(colors.syntaxCustom);
    }
    return QColor(colors.text);
}

QColor Theme::semanticColor(SemanticColor role, const QPalette& palette) {
    return semanticColor(role, palette.color(QPalette::Base).lightness() < 128);
}
