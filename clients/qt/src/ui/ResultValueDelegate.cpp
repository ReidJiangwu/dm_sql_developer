#include "ResultValueDelegate.h"

#include "QueryResultModel.h"

extern "C" {
#include <DPI.h>
}

#include <QAbstractProxyModel>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTimeEdit>
#include <QDoubleValidator>
#include <QFontMetrics>
#include <QLineEdit>
#include <QLocale>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QTimeEdit>
#include <QStyleOptionViewItem>
#include <QApplication>
#include <QStyleOptionButton>
#include <QStyle>

void ResultValueDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
    const QModelIndex& index) const {
    const auto reference = index.data(QueryResultModel::DeferredLobRole).value<DeferredLobReference>();
    if (!reference.isValid()) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    QStyleOptionViewItem background(option);
    initStyleOption(&background, index);
    background.text.clear();
    const auto* widget = option.widget;
    QStyle* style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);
    QStyleOptionButton button;
    button.palette = option.palette;
    button.state = option.state & (QStyle::State_Enabled | QStyle::State_MouseOver);
    button.text = reference.binary ? QStringLiteral("BLOB") : QStringLiteral("CLOB");
    button.rect = option.rect.adjusted(4, 2, -4, -2);
    button.rect.setWidth(qMin(button.rect.width(), option.fontMetrics.horizontalAdvance(button.text) + 24));
    style->drawControl(QStyle::CE_PushButton, &button, painter, widget);
}

namespace {

const QueryResultModel* resultModel(const QAbstractItemModel* model) {
    const QAbstractItemModel* current = model;
    while (const auto* proxy = qobject_cast<const QAbstractProxyModel*>(current)) {
        current = proxy->sourceModel();
    }
    return qobject_cast<const QueryResultModel*>(current);
}

QLineEdit* lineEditor(QWidget* parent, const QRegularExpression& expression = {}) {
    auto* editor = new QLineEdit(parent);
    editor->setClearButtonEnabled(true);
    if (expression.isValid() && !expression.pattern().isEmpty()) {
        editor->setValidator(new QRegularExpressionValidator(expression, editor));
    }
    return editor;
}

}

ResultValueDelegate::ResultValueDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {
}

QSize ResultValueDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    // Code and CJK fonts have different ascent/descent at the same pixel size.
    // A cell must still honor the view's line height when the code font is shorter.
    size.setHeight(qMax(size.height(), QFontMetrics(option.font).height() + 2));
    return size;
}

void ResultValueDelegate::initStyleOption(QStyleOptionViewItem* option,
        const QModelIndex& index) const {
    const QFont viewFont = option->font;
    QStyledItemDelegate::initStyleOption(option, index);
    // Keep the model's fixed-width family and NULL/deleted styles at the view's size.
    if (viewFont.pixelSize() > 0) option->font.setPixelSize(viewFont.pixelSize());
    else option->font.setPointSizeF(viewFont.pointSizeF());
    option->fontMetrics = QFontMetrics(option->font);
}

QWidget* ResultValueDelegate::createEditor(QWidget* parent,
        const QStyleOptionViewItem&, const QModelIndex& index) const {
    if (!index.flags().testFlag(Qt::ItemIsEditable)) return nullptr;
    const int type = index.data(QueryResultModel::DriverTypeRole).toInt();
    const bool nullable = index.model()->headerData(index.column(), Qt::Horizontal,
        QueryResultModel::ColumnNullableRole).toBool();
    if (type == DSQL_BOOLEAN) {
        auto* editor = new QComboBox(parent);
        if (nullable) editor->addItem(QStringLiteral("NULL"), QVariant{});
        editor->addItem(QStringLiteral("TRUE"), true);
        editor->addItem(QStringLiteral("FALSE"), false);
        editor->setAccessibleName(QStringLiteral("布尔值编辑器"));
        return editor;
    }
    if (type == DSQL_DATE) {
        auto* editor = new QDateEdit(parent);
        editor->setCalendarPopup(true);
        editor->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
        editor->setAccessibleName(QStringLiteral("日期编辑器"));
        return editor;
    }
    if (type == DSQL_TIME) {
        auto* editor = new QTimeEdit(parent);
        editor->setDisplayFormat(QStringLiteral("HH:mm:ss.zzz"));
        editor->setAccessibleName(QStringLiteral("时间编辑器"));
        return editor;
    }
    if (type == DSQL_TIMESTAMP) {
        auto* editor = new QDateTimeEdit(parent);
        editor->setCalendarPopup(true);
        editor->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
        editor->setAccessibleName(QStringLiteral("时间戳编辑器"));
        return editor;
    }
    if (type == DSQL_TINYINT || type == DSQL_SMALLINT
            || type == DSQL_INT || type == DSQL_BIGINT) {
        auto* editor = lineEditor(parent,
            QRegularExpression(QStringLiteral(R"(^[+-]?\d*$)")));
        editor->setAccessibleName(QStringLiteral("整数编辑器"));
        return editor;
    }
    if (type == DSQL_DEC) {
        auto* editor = lineEditor(parent,
            QRegularExpression(QStringLiteral(R"(^[+-]?\d*(?:\.\d*)?$)")));
        editor->setAccessibleName(QStringLiteral("定点数编辑器"));
        return editor;
    }
    if (type == DSQL_FLOAT || type == DSQL_DOUBLE) {
        auto* editor = lineEditor(parent);
        auto* validator = new QDoubleValidator(editor);
        validator->setNotation(QDoubleValidator::ScientificNotation);
        validator->setLocale(QLocale::c());
        editor->setValidator(validator);
        editor->setAccessibleName(QStringLiteral("浮点数编辑器"));
        return editor;
    }
    auto* editor = lineEditor(parent);
    editor->setAccessibleName(type == DSQL_BINARY || type == DSQL_VARBINARY
            || type == DSQL_BLOB
        ? QStringLiteral("十六进制二进制值编辑器")
        : type == DSQL_TIME_TZ || type == DSQL_TIMESTAMP_TZ
            ? QStringLiteral("带时区时间值编辑器")
            : QStringLiteral("文本值编辑器"));
    if (type == DSQL_BINARY || type == DSQL_VARBINARY || type == DSQL_BLOB) {
        editor->setPlaceholderText(QStringLiteral("十六进制，例如 0x0A1B"));
    }
    return editor;
}

void ResultValueDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const {
    const int type = index.data(QueryResultModel::DriverTypeRole).toInt();
    const bool isNull = index.data(QueryResultModel::NullValueRole).toBool();
    const QString raw = index.data(Qt::EditRole).toString();
    if (auto* combo = qobject_cast<QComboBox*>(editor)) {
        const int target = isNull ? combo->findData(QVariant{})
            : combo->findData(index.data(QueryResultModel::TypedValueRole).toBool());
        combo->setCurrentIndex(qMax(0, target));
        return;
    }
    if (auto* date = qobject_cast<QDateEdit*>(editor)) {
        const QDate value = QDate::fromString(raw.left(10), Qt::ISODate);
        if (value.isValid()) date->setDate(value);
        return;
    }
    if (auto* time = qobject_cast<QTimeEdit*>(editor)) {
        const QTime value = QTime::fromString(raw, Qt::ISODateWithMs);
        if (value.isValid()) time->setTime(value);
        return;
    }
    if (auto* timestamp = qobject_cast<QDateTimeEdit*>(editor)) {
        QString normalized = raw;
        if (normalized.size() > 10 && normalized.at(10) == QLatin1Char(' ')) {
            normalized[10] = QLatin1Char('T');
        }
        const QDateTime value = QDateTime::fromString(normalized, Qt::ISODateWithMs);
        if (value.isValid()) timestamp->setDateTime(value);
        return;
    }
    if (auto* line = qobject_cast<QLineEdit*>(editor)) {
        if (type == DSQL_BINARY || type == DSQL_VARBINARY || type == DSQL_BLOB) {
            line->setText(QStringLiteral("0x") + QString::fromLatin1(
                index.data(QueryResultModel::TypedValueRole).toByteArray().toHex().toUpper()));
        } else {
            line->setText(isNull ? QString{} : raw);
        }
        line->selectAll();
    }
}

void ResultValueDelegate::setModelData(QWidget* editor, QAbstractItemModel* model,
        const QModelIndex& index) const {
    QVariant value;
    if (auto* combo = qobject_cast<QComboBox*>(editor)) {
        value = combo->currentData();
    } else if (auto* date = qobject_cast<QDateEdit*>(editor)) {
        value = date->date();
    } else if (auto* time = qobject_cast<QTimeEdit*>(editor)) {
        value = time->time();
    } else if (auto* timestamp = qobject_cast<QDateTimeEdit*>(editor)) {
        value = timestamp->dateTime();
    } else if (auto* line = qobject_cast<QLineEdit*>(editor)) {
        value = line->text();
    }
    if (model->setData(index, value, Qt::EditRole)) return;
    const QueryResultModel* result = resultModel(model);
    emit validationFailed(index, result == nullptr || result->lastValidationError().isEmpty()
        ? QStringLiteral("输入值与目标字段类型不匹配。")
        : result->lastValidationError());
}

void ResultValueDelegate::updateEditorGeometry(QWidget* editor,
        const QStyleOptionViewItem& option, const QModelIndex&) const {
    editor->setGeometry(option.rect);
}
