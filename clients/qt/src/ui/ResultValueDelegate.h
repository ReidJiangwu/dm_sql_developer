#pragma once

#include <QStyledItemDelegate>

class ResultValueDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit ResultValueDelegate(QObject* parent = nullptr);
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    [[nodiscard]] QWidget* createEditor(QWidget* parent,
        const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    void setModelData(QWidget* editor, QAbstractItemModel* model,
        const QModelIndex& index) const override;
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override;

signals:
    void validationFailed(const QModelIndex& index, const QString& message) const;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override;
};
