#pragma once

#include "Theme.h"
#include "ExecutionLogHighlighter.h"
#include <QPlainTextEdit>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTableView>
#include <QHeaderView>
#include <QStyle>

namespace LogView {
inline constexpr int fontPixels = 12;

inline void configure(QPlainTextEdit* view, ExecutionLog::Level fallback = ExecutionLog::Level::Plain) {
    view->setProperty("logContent", true);
    QFont font = Theme::editorFont();
    font.setPixelSize(qMax(1, qRound(fontPixels * Theme::runtimeScaleFactor())));
    view->setFont(font);
    view->style()->unpolish(view);
    view->style()->polish(view);
    view->document()->setDocumentMargin(4);
    QTextBlockFormat block;
    block.setTopMargin(0);
    block.setBottomMargin(0);
    block.setLineHeight(100, QTextBlockFormat::ProportionalHeight);
    QTextCursor cursor(view->document());
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(block);
    view->setTabStopDistance(view->fontMetrics().horizontalAdvance(QStringLiteral("    ")));
    if (!view->property("logHighlighterInstalled").toBool()) {
        view->setProperty("logHighlighterInstalled", true);
        new ExecutionLog::Highlighter(view, fallback);
    }
}

inline void configure(QTableView* view) {
    view->setProperty("logContent", true);
    QFont font = Theme::editorFont();
    font.setPixelSize(qMax(1, qRound(fontPixels * Theme::runtimeScaleFactor())));
    view->setFont(font);
    view->style()->unpolish(view);
    view->style()->polish(view);
    const int rowHeight = qMax(qRound(22 * Theme::runtimeScaleFactor()),
        view->fontMetrics().height() + qRound(4 * Theme::runtimeScaleFactor()));
    // The native header minimum follows the larger UI font on Wine. Reset it
    // with the log row height so it cannot clamp the compact default upwards.
    view->verticalHeader()->setMinimumSectionSize(rowHeight);
    view->verticalHeader()->setDefaultSectionSize(rowHeight);
}
}
