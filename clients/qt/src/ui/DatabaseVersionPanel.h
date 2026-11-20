#pragma once

#include "../data/MetadataTypes.h"
#include "TableCopySupport.h"
#include <QFontMetrics>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

// Presentation only: keep the original query result intact for full-result copy.
class DatabaseVersionPanel final : public QWidget {
public:
    explicit DatabaseVersionPanel(const SystemInfoResult& result, QWidget* parent = nullptr)
        : QWidget(parent) {
        setObjectName(QStringLiteral("databaseVersionPanel"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(10);
        auto* identity = new QLabel(this);
        identity->setObjectName(QStringLiteral("databaseVersionIdentity"));
        identity->setTextFormat(Qt::PlainText);
        identity->setWordWrap(true);
        identity->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        identity->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        identity->setText(QStringLiteral("数据库 IP：%1　｜　端口：%2　｜　实例：%3")
            .arg(result.serverAddress.isEmpty() ? QStringLiteral("未获取") : result.serverAddress,
                result.serverPort > 0 ? QString::number(result.serverPort) : QStringLiteral("未获取"),
                result.instanceName.isEmpty() ? QStringLiteral("未获取") : result.instanceName));
        identity->setToolTip(result.identityWarning.isEmpty()
            ? identity->text() : identity->text() + QLatin1Char('\n') + result.identityWarning);
        layout->addWidget(identity);
        QStringList versions;
        QVector<QPair<QString, QString>> details;
        for (const auto& row : result.rows) {
            for (int column = 0; column < result.columns.size(); ++column) {
                const QString name = result.columns.at(column).trimmed();
                const QString value = row.value(column);
                if (name.compare(QStringLiteral("INNER_VER"), Qt::CaseInsensitive) == 0) {
                    if (!value.trimmed().isEmpty() && !versions.contains(value)) versions.append(value);
                } else {
                    details.append({name.compare(QStringLiteral("BANNER"), Qt::CaseInsensitive) == 0
                        ? bannerLabel(value) : name, value});
                }
            }
        }
        auto* version = new QLabel(this);
        version->setObjectName(QStringLiteral("databaseInternalVersion"));
        version->setProperty("systemInfoPropertyCaption", true);
        version->setTextFormat(Qt::PlainText);
        version->setWordWrap(true);
        version->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        version->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        version->setText(versions.isEmpty() ? QStringLiteral("内部版本（INNER_VER）：未返回")
            : versions.size() == 1 ? QStringLiteral("内部版本（INNER_VER）：%1").arg(versions.first())
            : QStringLiteral("内部版本（INNER_VER）：返回 %1 个不同值，详见下方列表").arg(versions.size()));
        layout->addWidget(version);
        // Unexpected differing values remain visible instead of choosing one silently.
        if (versions.size() > 1) {
            int index = 0;
            for (const auto& value : versions)
                details.insert(index++, {QStringLiteral("内部版本（INNER_VER）"), value});
        }
        auto* empty = new QLabel(QStringLiteral("数据库未返回其他版本明细。"), this);
        empty->setObjectName(QStringLiteral("databaseVersionEmpty"));
        empty->setWordWrap(true);
        empty->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        empty->setVisible(details.isEmpty());
        layout->addWidget(empty);

        auto* table = new QTableWidget(details.size(), 2, this);
        table->setObjectName(QStringLiteral("systemInfoTable"));
        table->setAccessibleName(QStringLiteral("数据库版本明细"));
        table->setHorizontalHeaderLabels({QStringLiteral("项目"), QStringLiteral("值")});
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->setAlternatingRowColors(true);
        table->setWordWrap(true);
        table->setTextElideMode(Qt::ElideNone);
        table->setShowGrid(false);
        table->verticalHeader()->hide();
        table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setMinimumSectionSize(80);
        table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        table->setColumnWidth(0, qMin(240, table->fontMetrics().horizontalAdvance(
            QStringLiteral("数据库版本（DB Version）")) + 24));
        for (int row = 0; row < details.size(); ++row) {
            const QStringList cells{details.at(row).first, details.at(row).second};
            for (int column = 0; column < cells.size(); ++column) {
                auto* item = new QTableWidgetItem(cells.at(column));
                item->setToolTip(item->text());
                item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                table->setItem(row, column, item);
            }
        }
        connect(table->horizontalHeader(), &QHeaderView::sectionResized, table,
            [table](int, int, int) { table->resizeRowsToContents(); });
        TableCopySupport::install(table);
        // Let the view consume spare height; no pre-show visibility/height calculation.
        layout->addWidget(table, 1);
    }

private:
    static QString bannerLabel(const QString& value) {
        const QString banner = value.trimmed();
        if (banner.startsWith(QStringLiteral("DM Database Server"), Qt::CaseInsensitive))
            return QStringLiteral("服务器版本");
        if (banner.startsWith(QStringLiteral("DB Version:"), Qt::CaseInsensitive))
            return QStringLiteral("数据库版本（DB Version）");
        if (banner.startsWith(QStringLiteral("Msg Version:"), Qt::CaseInsensitive))
            return QStringLiteral("通信版本（Msg Version）");
        if (banner.startsWith(QStringLiteral("DMDSC Version:"), Qt::CaseInsensitive)
                || banner.startsWith(QStringLiteral("DSC Version:"), Qt::CaseInsensitive))
            return QStringLiteral("集群版本（DSC）");
        if (banner.startsWith(QStringLiteral("Gsu level"), Qt::CaseInsensitive))
            return QStringLiteral("兼容性标识（Gsu）");
        return QStringLiteral("版本标识（BANNER）");
    }
};
