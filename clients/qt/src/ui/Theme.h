#pragma once

#include <QColor>
#include <QFont>
#include <QVariantMap>

class QApplication;
class QPalette;
class QEvent;
class QMainWindow;
class QWidget;

enum class ThemeMode {
    System,
    Light,
    Dark,
};

enum class SemanticColor {
    Window,
    Sidebar,
    Content,
    Raised,
    Control,
    Editor,
    Result,
    Text,
    Accent,
    AccentSurface,
    Selection,
    SelectionText,
    MutedText,
    Success,
    SuccessSurface,
    Warning,
    WarningSurface,
    Danger,
    DangerSurface,
    Info,
    InfoSurface,
    SyntaxKeyword,
    SyntaxComment,
    SyntaxString,
    SyntaxNumber,
    SyntaxSymbol,
    SyntaxCustom,
    ProgressFill,
};

namespace Theme {

using SemanticColor = ::SemanticColor;

void apply(QApplication& application, ThemeMode mode);
void applyStartupAppearance(QWidget& widget);
bool applyUiScale(QApplication& application, int percent);
int uiScalePercent();
qreal runtimeScaleFactor();
qreal runtimeFontSize(qreal size);
bool isUiScaleChangeEvent(const QEvent* event);
int uiScaleBasisPercent();
qreal restoredUiScaleRatio(int savedBasis);
QVariantMap windowScaleState(const QMainWindow& window);
void restoreWindowScaleState(QMainWindow& window, const QVariantMap& state);
QFont editorFont();
QColor semanticColor(SemanticColor role, bool dark);
QColor semanticColor(SemanticColor role, const QPalette& palette);

}  // namespace Theme
