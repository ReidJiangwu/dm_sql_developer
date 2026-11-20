#include "CommandButton.h"
#include "SearchField.h"
#include "InterfaceIcons.h"
#include "MessagePrompt.h"
#include "DbaDocumentWidget.h"
#include "AdaptivePopupComboBox.h"
#include "Theme.h"
#include "WrappingControlLayout.h"

#include "../core/DbaDiagnostics.h"
#include "../core/DbaManagement.h"
#include "../core/ConnectionStore.h"
#include "../core/RoleEditorModel.h"
#include "../data/DpiSession.h"
#include "../security/CredentialStore.h"

#include <QAction>
#include <QAbstractItemModel>
#include <QCheckBox>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsItem>
#include <QGraphicsItemGroup>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QMap>
#include <QMenu>
#include <QSettings>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPolygonF>
#include <QSplitter>
#include <QStackedWidget>
#include <QScrollBar>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidgetAction>
#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <functional>

namespace {

QIcon parameterStarIcon(const QPalette& palette) {
    QPixmap pixmap(18, 18); pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
    QPolygonF star;
    for (int index = 0; index < 10; ++index) {
        const double angle = -1.5707963267948966 + index * 0.6283185307179586;
        const double radius = index % 2 ? 3.3 : 7.3;
        star << QPointF(9 + std::cos(angle) * radius, 9 + std::sin(angle) * radius);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(Theme::semanticColor(Theme::SemanticColor::Warning, palette));
    painter.drawPolygon(star); return QIcon(pixmap);
}

bool looksLikePermissionDenied(const QString& message) {
    const QString normalized = message.toLower();
    return normalized.contains(QStringLiteral("权限"))
        || normalized.contains(QStringLiteral("permission"))
        || normalized.contains(QStringLiteral("privilege"))
        || normalized.contains(QStringLiteral("not authorized"))
        || normalized.contains(QStringLiteral("not authorised"));
}

QString observedParameterValue(const QString& format, const QString& value) {
    // DatabaseEnvironment currently receives trimmed QString cells from the
    // driver result.  It therefore cannot distinguish SQL NULL, an empty value
    // and a whitespace-only value here.
    return value.isEmpty() ? QStringLiteral("空值／未返回（待核对）")
        : DatabaseEnvironment::formatValue(format, value);
}

QString observedParameterValueToolTip(const QString& value) {
    return value.isEmpty()
        ? QStringLiteral("当前驱动／查询结果无法区分数据库空值、空白值与未返回值；请以当前实例查询结果核对。")
        : QString();
}

QDateTime parsedSessionTime(QString value) {
    value = value.trimmed();
    QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!parsed.isValid()) parsed = QDateTime::fromString(value, Qt::ISODate);
    if (!parsed.isValid() && value.size() > 23) value = value.left(23);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(value, QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    }
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(value, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    }
    return parsed;
}

QString elapsedSessionLabel(const QString& value, const QDateTime& now) {
    const QDateTime time = parsedSessionTime(value);
    if (!time.isValid()) return QStringLiteral("未知");
    qint64 seconds = std::max<qint64>(0, time.secsTo(now));
    const qint64 days = seconds / 86'400;
    seconds %= 86'400;
    const qint64 hours = seconds / 3'600;
    seconds %= 3'600;
    const qint64 minutes = seconds / 60;
    if (days > 0) return QStringLiteral("%1 天 %2 小时").arg(days).arg(hours);
    if (hours > 0) return QStringLiteral("%1 小时 %2 分").arg(hours).arg(minutes);
    if (minutes > 0) return QStringLiteral("%1 分钟").arg(minutes);
    return QStringLiteral("%1 秒").arg(seconds);
}

}  // namespace

class SessionTrendWidget final : public QWidget {
public:
    explicit SessionTrendWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(110);
    }

    void setSamples(QVector<QPair<qint64, qint64>> samples) {
        samples_ = std::move(samples);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area = rect().adjusted(44, 24, -16, -28);
        painter.setPen(palette().text().color());
        painter.drawText(QPointF(area.left(), 16), QStringLiteral("总会话"));
        const QColor totalColor = palette().color(QPalette::Highlight);
        const QColor activeColor = Theme::semanticColor(
            Theme::SemanticColor::Warning, palette());
        painter.setPen(QPen(totalColor, 3));
        painter.drawLine(QPointF(area.left() + 48, 12), QPointF(area.left() + 66, 12));
        painter.setPen(palette().text().color());
        painter.drawText(QPointF(area.left() + 82, 16), QStringLiteral("活动会话"));
        painter.setPen(QPen(activeColor, 3));
        painter.drawLine(QPointF(area.left() + 142, 12), QPointF(area.left() + 160, 12));
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawLine(area.bottomLeft(), area.topLeft());
        painter.drawLine(area.bottomLeft(), area.bottomRight());
        if (samples_.isEmpty()) {
            painter.setPen(palette().text().color());
            painter.drawText(area.adjusted(12, 12, 0, 0), QStringLiteral("尚无会话采样。"));
            return;
        }
        qint64 maximum = 1;
        for (const auto& sample : std::as_const(samples_)) {
            maximum = std::max(maximum, std::max(sample.first, sample.second));
        }
        painter.setPen(palette().text().color());
        painter.drawText(QPointF(8, area.top() + 5), QString::number(maximum));
        painter.drawText(QPointF(26, area.bottom() + 5), QStringLiteral("0"));
        drawSeries(painter, area, maximum, true, totalColor);
        drawSeries(painter, area, maximum, false, activeColor);
    }

private:
    void drawSeries(QPainter& painter, const QRectF& area, qint64 maximum,
        bool total, const QColor& color) const {
        QPainterPath path;
        for (int index = 0; index < samples_.size(); ++index) {
            const qint64 value = total ? samples_.at(index).first : samples_.at(index).second;
            const qsizetype segments = std::max<qsizetype>(1, samples_.size() - 1);
            const qreal x = area.left() + area.width() * index
                / segments;
            const qreal y = area.bottom() - area.height() * std::max<qint64>(0, value) / maximum;
            if (index == 0) path.moveTo(x, y); else path.lineTo(x, y);
        }
        painter.setPen(QPen(color, 2));
        painter.drawPath(path);
        if (samples_.size() == 1) {
            const qint64 value = total ? samples_.first().first : samples_.first().second;
            const qreal y = area.bottom() - area.height() * std::max<qint64>(0, value) / maximum;
            painter.setBrush(color);
            painter.drawEllipse(QPointF(area.left(), y), 4, 4);
        }
    }

    QVector<QPair<qint64, qint64>> samples_;
};

class LockWaitGraphWidget final : public QGraphicsView {
public:
    explicit LockWaitGraphWidget(QWidget* parent = nullptr)
        : QGraphicsView(parent), scene_(new QGraphicsScene(this)) {
        setScene(scene_);
        setMinimumHeight(180);
        setRenderHint(QPainter::Antialiasing);
        setDragMode(QGraphicsView::ScrollHandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setResizeAnchor(QGraphicsView::AnchorViewCenter);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(QStringLiteral("锁等待有向关系图"));
        connect(scene_, &QGraphicsScene::selectionChanged, this, [this]() {
            if (synchronizing_ || scene_->selectedItems().isEmpty()) return;
            const QGraphicsItem* item = scene_->selectedItems().constFirst();
            int recordIndex = -1;
            if (item->data(1).toString() == QStringLiteral("edge")) {
                recordIndex = item->data(0).toInt();
            } else if (item->data(1).toString() == QStringLiteral("node")) {
                const qint64 sessionId = item->data(0).toLongLong();
                for (int index = 0; index < records_.size(); ++index) {
                    if (records_.at(index).blockerSessionId == sessionId
                            || records_.at(index).waiterSessionId == sessionId) {
                        recordIndex = index;
                        break;
                    }
                }
            }
            if (recordIndex >= 0 && relationSelected) relationSelected(recordIndex);
        });
    }

    std::function<void(int)> relationSelected;

    void setRecords(QVector<LockWaitRecord> records) {
        records_ = std::move(records);
        rebuild();
    }

    void selectRecord(int recordIndex) {
        if (recordIndex < 0 || recordIndex >= records_.size()) return;
        synchronizing_ = true;
        scene_->clearSelection();
        for (QGraphicsItem* item : scene_->items()) {
            if (item->data(1).toString() == QStringLiteral("edge")
                    && item->data(0).toInt() == recordIndex) {
                item->setSelected(true);
                centerOn(item);
                break;
            }
        }
        synchronizing_ = false;
    }

    void fitContents() {
        if (!scene_->items().isEmpty()) {
            fitInView(scene_->itemsBoundingRect().adjusted(-24, -24, 24, 24),
                Qt::KeepAspectRatio);
        }
    }

protected:
    void wheelEvent(QWheelEvent* event) override {
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            const qreal factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
            scale(factor, factor);
            event->accept();
            return;
        }
        QGraphicsView::wheelEvent(event);
    }

private:
    void rebuild() {
        scene_->clear();
        if (records_.isEmpty()) {
            QGraphicsTextItem* empty = scene_->addText(QStringLiteral("当前快照未发现等待。"));
            empty->setDefaultTextColor(palette().text().color());
            return;
        }
        const QVector<LockWaitEdge> edges = DbaDiagnostics::lockEdges(records_);
        QVector<qint64> ids = DbaDiagnostics::lockSessionIds(edges);
        const int hiddenNodes = std::max(0, static_cast<int>(ids.size()) - 100);
        if (hiddenNodes > 0) ids.resize(100);
        QSet<qint64> visibleIds;
        for (qint64 id : std::as_const(ids)) visibleIds.insert(id);
        const QHash<qint64, int> depths = DbaDiagnostics::nodeDepths(records_);
        QHash<int, QVector<qint64>> levels;
        QSet<qint64> blockers;
        QSet<qint64> waiters;
        QHash<qint64, QString> users;
        for (const LockWaitRecord& record : std::as_const(records_)) {
            blockers.insert(record.blockerSessionId);
            waiters.insert(record.waiterSessionId);
            if (!record.blockerUser.isEmpty()) users.insert(record.blockerSessionId, record.blockerUser);
            if (!record.waiterUser.isEmpty()) users.insert(record.waiterSessionId, record.waiterUser);
        }
        for (qint64 id : std::as_const(ids)) levels[depths.value(id)].append(id);
        for (auto iterator = levels.begin(); iterator != levels.end(); ++iterator) {
            std::sort(iterator.value().begin(), iterator.value().end());
        }
        QHash<qint64, QPointF> points;
        constexpr qreal nodeWidth = 176.0;
        constexpr qreal nodeHeight = 62.0;
        for (auto iterator = levels.cbegin(); iterator != levels.cend(); ++iterator) {
            for (int row = 0; row < iterator.value().size(); ++row) {
                points.insert(iterator.value().at(row),
                    QPointF(iterator.key() * 264.0, row * 98.0));
            }
        }

        const bool cycle = DbaDiagnostics::containsCycle(edges);
        const QColor edgeColor = cycle
            ? Theme::semanticColor(Theme::SemanticColor::Danger, palette())
            : palette().color(QPalette::PlaceholderText);
        QSet<QString> drawnEdges;
        for (int recordIndex = 0; recordIndex < records_.size(); ++recordIndex) {
            const LockWaitRecord& record = records_.at(recordIndex);
            if (!visibleIds.contains(record.blockerSessionId)
                    || !visibleIds.contains(record.waiterSessionId)) continue;
            const QString edgeKey = QStringLiteral("%1:%2")
                .arg(record.blockerSessionId).arg(record.waiterSessionId);
            if (drawnEdges.contains(edgeKey)) continue;
            drawnEdges.insert(edgeKey);
            const QPointF from = points.value(record.blockerSessionId)
                + QPointF(nodeWidth, nodeHeight / 2);
            const QPointF to = points.value(record.waiterSessionId)
                + QPointF(0, nodeHeight / 2);
            const qreal middle = (from.x() + to.x()) / 2.0;
            QPainterPath path(from);
            path.cubicTo(QPointF(middle, from.y()), QPointF(middle, to.y()), to);
            auto* edgeItem = scene_->addPath(path, QPen(edgeColor, 1.8));
            edgeItem->setFlag(QGraphicsItem::ItemIsSelectable);
            edgeItem->setData(0, recordIndex);
            edgeItem->setData(1, QStringLiteral("edge"));
            auto* arrow = new QGraphicsPolygonItem(edgeItem);
            arrow->setPolygon(QPolygonF{to, to + QPointF(-10, -5), to + QPointF(-10, 5)});
            arrow->setPen(Qt::NoPen);
            arrow->setBrush(edgeColor);
            QString object = record.objectName;
            if (!record.objectSchema.isEmpty() && !object.isEmpty()) {
                object.prepend(record.objectSchema + QLatin1Char('.'));
            } else if (object.isEmpty() && record.tableId > 0) {
                object = QStringLiteral("TABLE_ID %1").arg(record.tableId);
            }
            QString label = record.waiterOperation.operation;
            if (!object.isEmpty()) label = object + QStringLiteral(" · ") + label;
            auto* edgeLabel = new QGraphicsTextItem(label, edgeItem);
            edgeLabel->setDefaultTextColor(palette().text().color());
            edgeLabel->setPos(middle - edgeLabel->boundingRect().width() / 2,
                (from.y() + to.y()) / 2 - 26);
        }

        for (qint64 id : std::as_const(ids)) {
            const bool root = blockers.contains(id) && !waiters.contains(id);
            const bool middle = blockers.contains(id) && waiters.contains(id);
            const QString role = root ? QStringLiteral("根阻塞者")
                : middle ? QStringLiteral("中间阻塞者") : QStringLiteral("等待者");
            const QColor border = cycle
                ? Theme::semanticColor(Theme::SemanticColor::Danger, palette())
                : root ? Theme::semanticColor(Theme::SemanticColor::Warning, palette())
                       : palette().color(QPalette::Highlight);
            const QColor surface = cycle
                ? Theme::semanticColor(Theme::SemanticColor::DangerSurface, palette())
                : root ? Theme::semanticColor(Theme::SemanticColor::WarningSurface, palette())
                       : palette().color(QPalette::AlternateBase);
            auto* group = new QGraphicsItemGroup();
            scene_->addItem(group);
            auto* rectangle = new QGraphicsRectItem(0, 0, nodeWidth, nodeHeight, group);
            rectangle->setPen(QPen(border, 1.6));
            rectangle->setBrush(surface);
            auto* roleText = new QGraphicsTextItem(role, group);
            roleText->setDefaultTextColor(border);
            roleText->setPos(8, 2);
            auto* idText = new QGraphicsTextItem(QString::number(id), group);
            idText->setDefaultTextColor(palette().text().color());
            QFont idFont = Theme::editorFont();
            idFont.setBold(true);
            idText->setFont(idFont);
            idText->setPos(8, 21);
            auto* userText = new QGraphicsTextItem(
                users.value(id).isEmpty() ? QStringLiteral("用户未知") : users.value(id), group);
            userText->setDefaultTextColor(palette().color(QPalette::PlaceholderText));
            userText->setPos(8, 40);
            group->setPos(points.value(id));
            group->setFlag(QGraphicsItem::ItemIsSelectable);
            group->setFlag(QGraphicsItem::ItemIsFocusable);
            group->setData(0, id);
            group->setData(1, QStringLiteral("node"));
        }
        if (hiddenNodes > 0) {
            QGraphicsTextItem* note = scene_->addText(
                QStringLiteral("共 %1 个节点，图中显示前 100 个；关系表保留全部数据。")
                    .arg(ids.size() + hiddenNodes));
            note->setDefaultTextColor(palette().color(QPalette::PlaceholderText));
            note->setPos(0, scene_->itemsBoundingRect().bottom() + 16);
        }
        scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-24, -24, 24, 24));
        fitContents();
    }

    QGraphicsScene* scene_ = nullptr;
    QVector<LockWaitRecord> records_;
    bool synchronizing_ = false;
};

namespace {
QWidget* tableSection(const QString& title, QTableWidget* table, QWidget* parent) {
    auto* section = new QWidget(parent);
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    layout->addWidget(new QLabel(title, section));
    layout->addWidget(table, 1);
    return section;
}

QWidget* pagedTableSection(const QString& title, QTableWidget* table, QWidget* parent,
        QPushButton*& previous, QLabel*& pageLabel, QPushButton*& next,
        QComboBox*& pageSize, QLineEdit* filter = nullptr) {
    auto* section = new QWidget(parent);
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    auto* tools = new QHBoxLayout();
    tools->setContentsMargins(0, 0, 0, 0);
    tools->addWidget(new QLabel(title, section));
    if (filter != nullptr) {
        filter->setParent(section);
        tools->addWidget(filter, 1);
    } else {
        tools->addStretch(1);
    }
    layout->addLayout(tools);
    layout->addWidget(table, 1);
    auto* pagination = new QHBoxLayout();
    pagination->setContentsMargins(0, 0, 0, 0);
    pagination->setSpacing(4);
    pagination->addStretch(1);
    previous = new QPushButton(QStringLiteral("上一页"), section);
    pageLabel = new QLabel(QStringLiteral("第 1 页"), section);
    next = new QPushButton(QStringLiteral("下一页"), section);
    pageSize = new AdaptivePopupComboBox(section);
    for (int size : {50, 100, 200}) {
        pageSize->addItem(QString::number(size), size);
    }
    pageSize->setCurrentIndex(pageSize->findData(100));
    pagination->addWidget(previous);
    pagination->addWidget(pageLabel);
    pagination->addWidget(next);
    pagination->addWidget(new QLabel(QStringLiteral("每页"), section));
    pagination->addWidget(pageSize);
    layout->addLayout(pagination);
    return section;
}

QString dbaDocumentKindLabel(DbaDocumentKind kind) {
    switch (kind) {
    case DbaDocumentKind::Sessions: return QStringLiteral("会话管理");
    case DbaDocumentKind::Locks: return QStringLiteral("锁等待分析");
    case DbaDocumentKind::Performance: return QStringLiteral("性能监控");
    case DbaDocumentKind::Tablespaces: return QStringLiteral("表空间管理");
    case DbaDocumentKind::Users: return QStringLiteral("用户管理");
    case DbaDocumentKind::Roles: return QStringLiteral("角色管理");
    case DbaDocumentKind::Jobs: return QStringLiteral("作业管理");
    case DbaDocumentKind::JobSchedules: return QStringLiteral("调度计划");
    case DbaDocumentKind::JobHistory: return QStringLiteral("执行历史");
    case DbaDocumentKind::Parameters: return QStringLiteral("数据库参数");
    }
    return {};
}

int dbaDocumentPage(DbaDocumentKind kind) {
    switch (kind) {
    case DbaDocumentKind::Sessions: return 0;
    case DbaDocumentKind::Locks: return 1;
    case DbaDocumentKind::Performance: return 2;
    case DbaDocumentKind::Tablespaces: return 3;
    case DbaDocumentKind::Users:
    case DbaDocumentKind::Roles: return 4;
    case DbaDocumentKind::Jobs:
    case DbaDocumentKind::JobSchedules:
    case DbaDocumentKind::JobHistory: return 5;
    case DbaDocumentKind::Parameters: return 6;
    }
    return 0;
}
}

DbaDocumentWidget::DbaDocumentWidget(
        DbaDocumentKind kind, int documentNumber, QWidget* parent)
    : QWidget(parent)
    , title_(QStringLiteral("%1 %2")
          .arg(dbaDocumentKindLabel(kind))
          .arg(documentNumber))
    , kind_(kind) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* contextBar = new QWidget(this);
    contextBar->setProperty("contextBar", true);
    contextBar->setProperty("documentContextContainer", true);
    contextBar->setObjectName(QStringLiteral("DbaDocumentWidgetContext"));
    auto* contextLayout = new QHBoxLayout(contextBar);
    contextLayout->setContentsMargins(8, 3, 8, 3);
    connectionSelector_ = new QToolButton(contextBar);
    connectionSelector_->setObjectName(QStringLiteral("dbaWindowConnectionSelector"));
    connectionSelector_->setProperty("textButton", true);
    connectionSelector_->setText(QStringLiteral("未连接"));
    connectionSelector_->setPopupMode(QToolButton::InstantPopup);
    connectionSelector_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connectionSelector_->setAccessibleName(
        QStringLiteral("%1当前连接").arg(dbaDocumentKindLabel(kind_)));
    refreshPrivileges_ = CommandButton::refresh(contextBar, QStringLiteral("刷新权限"), QStringLiteral("权限"));
    refreshPrivileges_->setObjectName(QStringLiteral("refreshRolePrivileges"));
    refreshPrivileges_->setProperty("quietAction", true);
    refreshPrivileges_->setVisible(kind_ == DbaDocumentKind::Roles);
    context_ = new QLabel(
        QStringLiteral("连接：未连接｜状态：未连接｜独立 Session"), contextBar);
    context_->setContentsMargins(0, 0, 0, 0);
    context_->setWordWrap(true);
    context_->setProperty("secondaryText", true);
    cancelRequest_ = new QPushButton(QStringLiteral("取消查询"), contextBar);
    contextLayout->addWidget(connectionSelector_);
    if (kind_ == DbaDocumentKind::Roles) {
        contextLayout->addWidget(refreshPrivileges_);
    }
    contextLayout->addWidget(context_, 1);
    if (kind_ == DbaDocumentKind::Locks) contextLayout->addStretch(1);
    contextLayout->addWidget(cancelRequest_);
    layout->addWidget(contextBar);
    pages_ = new QTabWidget(this);

    auto* sessionsPage = new QWidget(pages_);
    auto* sessionsLayout = new QVBoxLayout(sessionsPage);
    sessionsLayout->setContentsMargins(8, 8, 8, 8);
    sessionsLayout->setSpacing(6);
    auto* sessionActions = new QToolBar(sessionsPage);
    sessionActions->setObjectName(QStringLiteral("sessionManagerActions"));
    sessionActions->setProperty("secondaryToolBar", true);
    refreshSessions_ = CommandButton::refresh(sessionsPage, QStringLiteral("刷新会话"), QStringLiteral("会话"));
    loadSessionSql_ = new QPushButton(QStringLiteral("完整 SQL"), sessionsPage);
    openSessionLocks_ = new QPushButton(QStringLiteral("锁等待"), sessionsPage);
    sessionActionMenu_ = new QToolButton(sessionsPage);
    sessionActionMenu_->setText(QStringLiteral("会话操作"));
    sessionActionMenu_->setAccessibleName(QStringLiteral("选中会话操作菜单"));
    sessionActionMenu_->setPopupMode(QToolButton::InstantPopup);
    auto* sessionMenu = new QMenu(sessionActionMenu_);
    copySessionSummary_ = sessionMenu->addAction(QStringLiteral("复制会话诊断摘要"));
    copySessionSummary_->setToolTip(QStringLiteral(
        "复制选中会话的身份、客户端、事务和当前／最近 SQL 摘要。"));
    sessionMenu->addSeparator();
    cancelSessionOperation_ = sessionMenu->addAction(QStringLiteral("取消操作"));
    cancelSessionOperation_->setToolTip(QStringLiteral(
        "只取消正在执行的操作，不主动断开连接。"));
    sessionMenu->addSeparator();
    closeSession_ = sessionMenu->addAction(QStringLiteral("终止会话…"));
    closeSession_->setProperty("dangerAction", true);
    closeSession_->setToolTip(QStringLiteral(
        "断开目标连接；未提交事务通常会回滚。"));
    sessionActionMenu_->setMenu(sessionMenu);
    sessionActionMenu_->setProperty("menuButton", true);
    auto* sessionColumnsButton = new QToolButton(sessionsPage);
    sessionColumnsButton->setText(QStringLiteral("选择列"));
    sessionColumnsButton->setAccessibleName(QStringLiteral("选择会话表显示列"));
    sessionColumnsButton->setPopupMode(QToolButton::InstantPopup);
    auto* sessionColumnsMenu = new QMenu(sessionColumnsButton);
    sessionColumnsButton->setMenu(sessionColumnsMenu);
    sessionColumnsButton->setProperty("menuButton", true);
    sessionRefreshInterval_ = new AdaptivePopupComboBox(sessionsPage);
    sessionRefreshInterval_->addItem(QStringLiteral("手动刷新"), 0);
    sessionRefreshInterval_->addItem(QStringLiteral("每 2 秒"), 2'000);
    sessionRefreshInterval_->addItem(QStringLiteral("每 5 秒"), 5'000);
    sessionRefreshInterval_->addItem(QStringLiteral("每 10 秒"), 10'000);
    sessionRefreshInterval_->addItem(QStringLiteral("每 30 秒"), 30'000);
    sessionRefreshInterval_->setCurrentIndex(2);
    sessionRefreshInterval_->setAccessibleName(QStringLiteral("会话自动刷新频率"));
    sessionActions->addWidget(refreshSessions_);
    sessionActions->addWidget(loadSessionSql_);
    sessionActions->addWidget(openSessionLocks_);
    sessionActions->addSeparator();
    sessionActions->addWidget(new QLabel(QStringLiteral("自动刷新："), sessionsPage));
    sessionActions->addWidget(sessionRefreshInterval_);
    sessionActions->addSeparator();
    sessionActions->addWidget(sessionActionMenu_);
    sessionActions->addWidget(sessionColumnsButton);
    sessionsLayout->addWidget(sessionActions);

    sessionSummary_ = new QLabel(
        QStringLiteral("总会话 0｜活动 0｜有事务 0｜空闲有事务 0｜本客户端 0"), sessionsPage);
    sessionSummary_->setObjectName(QStringLiteral("sessionManagerSummary"));
    sessionSummary_->setAccessibleName(QStringLiteral("会话风险摘要"));
    sessionSummary_->setTextFormat(Qt::RichText);
    sessionSummary_->setTextInteractionFlags(
        Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    sessionSummary_->setOpenExternalLinks(false);
    sessionSummary_->setWordWrap(true);
    sessionSummary_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sessionSummary_->setContentsMargins(2, 4, 2, 4);
    sessionPresetFilter_ = new AdaptivePopupComboBox(sessionsPage);
    sessionPresetFilter_->setAccessibleName(QStringLiteral("会话快速筛选"));
    sessionPresetFilter_->addItem(QStringLiteral("全部会话"), QStringLiteral("all"));
    sessionPresetFilter_->addItem(QStringLiteral("活动会话"), QStringLiteral("active"));
    sessionPresetFilter_->addItem(QStringLiteral("有事务"), QStringLiteral("transaction"));
    sessionPresetFilter_->addItem(QStringLiteral("空闲有事务"), QStringLiteral("idleTransaction"));
    sessionPresetFilter_->addItem(QStringLiteral("本次变化"), QStringLiteral("changed"));
    sessionPresetFilter_->addItem(QStringLiteral("本客户端"), QStringLiteral("client"));
    sessionPresetFilter_->addItem(QStringLiteral("当前用户"), QStringLiteral("currentUser"));
    // Risk counters must remain readable, independently of toolbar overflow.
    sessionsLayout->addWidget(sessionSummary_);

    auto* sessionFilters = new QToolBar(sessionsPage);
    sessionFilters->setObjectName(QStringLiteral("sessionManagerFilters"));
    sessionFilters->setProperty("secondaryToolBar", true);
    sessionFilters->addWidget(sessionPresetFilter_);
    sessionUserFilter_ = new AdaptivePopupComboBox(sessionsPage);
    sessionUserFilter_->setAccessibleName(QStringLiteral("会话用户筛选"));
    sessionUserFilter_->addItem(QStringLiteral("全部用户"));
    sessionStateFilter_ = new AdaptivePopupComboBox(sessionsPage);
    sessionStateFilter_->setAccessibleName(QStringLiteral("会话状态筛选"));
    for (const QString& state : {QStringLiteral("全部状态"), QStringLiteral("ACTIVE"),
            QStringLiteral("IDLE"), QStringLiteral("PENDING"), QStringLiteral("CREATE"),
            QStringLiteral("STARTUP"), QStringLiteral("UNKNOWN")}) {
        sessionStateFilter_->addItem(state);
    }
    sessionIpFilter_ = new QLineEdit(sessionsPage);
    sessionIpFilter_->setObjectName(QStringLiteral("sessionIpFilter"));
    sessionIpFilter_->setAccessibleName(QStringLiteral("客户端 IP 筛选"));
    sessionIpFilter_->setPlaceholderText(QStringLiteral("客户端 IP"));
    sessionIpFilter_->setClearButtonEnabled(true);
    sessionAppFilter_ = new QLineEdit(sessionsPage);
    sessionAppFilter_->setAccessibleName(QStringLiteral("应用名称筛选"));
    SearchField::setPrompt(sessionAppFilter_, QStringLiteral("应用"), QStringLiteral("应用名称"));
    sessionAppFilter_->setClearButtonEnabled(true);
    userSessionsOnly_ = new QCheckBox(QStringLiteral("仅用户会话"), sessionsPage);
    currentUserOnly_ = new QCheckBox(QStringLiteral("仅当前用户"), sessionsPage);
    hideClientSessions_ = new QCheckBox(QStringLiteral("隐藏本客户端"), sessionsPage);
    sessionFilters->addWidget(sessionUserFilter_);
    sessionFilters->addWidget(sessionStateFilter_);
    sessionFilters->addWidget(sessionIpFilter_);
    sessionFilters->addWidget(sessionAppFilter_);
    auto* moreFilters = new QToolButton(sessionsPage);
    moreFilters->setText(QStringLiteral("更多筛选"));
    moreFilters->setAccessibleName(QStringLiteral("更多会话筛选条件"));
    moreFilters->setPopupMode(QToolButton::InstantPopup);
    auto* moreFiltersMenu = new QMenu(moreFilters);
    auto* filterOptions = new QWidget(moreFiltersMenu);
    auto* filterOptionsLayout = new QVBoxLayout(filterOptions);
    filterOptionsLayout->setContentsMargins(10, 8, 10, 8);
    filterOptionsLayout->setSpacing(6);
    filterOptionsLayout->addWidget(userSessionsOnly_);
    filterOptionsLayout->addWidget(currentUserOnly_);
    filterOptionsLayout->addWidget(hideClientSessions_);
    auto* filterOptionsAction = new QWidgetAction(moreFiltersMenu);
    filterOptionsAction->setDefaultWidget(filterOptions);
    moreFiltersMenu->addAction(filterOptionsAction);
    moreFilters->setMenu(moreFiltersMenu);
    moreFilters->setProperty("menuButton", true);
    sessionFilters->addWidget(moreFilters);
    for (QComboBox* filter : {sessionPresetFilter_, sessionUserFilter_, sessionStateFilter_}) {
        filter->setMinimumContentsLength(6);
        filter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        filter->setMaximumWidth(160);
    }
    for (QLineEdit* filter : {sessionIpFilter_, sessionAppFilter_}) {
        filter->setMinimumWidth(100);
        filter->setMaximumWidth(200);
    }
    sessionRefreshInterval_->setMaximumWidth(120);
    auto* resetFilters = sessionFilters->addAction(QStringLiteral("重置筛选"));
    resetFilters->setObjectName(QStringLiteral("resetSessionFilters"));
    connect(resetFilters, &QAction::triggered, this, [this]() {
        const QSignalBlocker presetBlock(sessionPresetFilter_);
        const QSignalBlocker userBlock(sessionUserFilter_);
        const QSignalBlocker stateBlock(sessionStateFilter_);
        const QSignalBlocker ipBlock(sessionIpFilter_);
        const QSignalBlocker appBlock(sessionAppFilter_);
        const QSignalBlocker sessionsBlock(userSessionsOnly_);
        const QSignalBlocker currentBlock(currentUserOnly_);
        const QSignalBlocker clientBlock(hideClientSessions_);
        sessionPresetFilter_->setCurrentIndex(0);
        sessionUserFilter_->setCurrentIndex(0);
        sessionStateFilter_->setCurrentIndex(0);
        sessionIpFilter_->clear();
        sessionAppFilter_->clear();
        userSessionsOnly_->setChecked(false);
        currentUserOnly_->setChecked(false);
        hideClientSessions_->setChecked(false);
        applySessionFilters();
    });
    sessionsLayout->addWidget(sessionFilters);
    sessionsState_ = new QLabel(QStringLiteral("尚未刷新会话。"), sessionsPage);
    sessionsLayout->addWidget(sessionsState_);
    sessions_ = new QTableWidget(sessionsPage);
    sessions_->setObjectName(QStringLiteral("sessionManagerTable"));
    sessions_->setAccessibleName(QStringLiteral("数据库会话列表"));
    sessions_->setSelectionBehavior(QAbstractItemView::SelectRows);
    sessions_->setSelectionMode(QAbstractItemView::SingleSelection);
    sessions_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sessions_->setAlternatingRowColors(true);
    sessions_->setWordWrap(false);
    sessions_->setTextElideMode(Qt::ElideRight);
    sessions_->setSortingEnabled(true);
    sessions_->verticalHeader()->setDefaultSectionSize(30);
    connect(sessionColumnsMenu, &QMenu::aboutToShow, this,
        [this, sessionColumnsMenu]() {
            sessionColumnsMenu->clear();
            for (int column = 0; column < sessions_->columnCount(); ++column) {
                const QTableWidgetItem* header = sessions_->horizontalHeaderItem(column);
                if (header == nullptr || header->text() == QStringLiteral("RISK_ORDER")
                        || header->text() == QStringLiteral("IS_SELF")
                        || header->text() == QStringLiteral("CONNECTED")) continue;
                QAction* action = sessionColumnsMenu->addAction(header->text());
                action->setCheckable(true);
                action->setChecked(!sessions_->isColumnHidden(column));
                connect(action, &QAction::toggled, sessions_, [this, column](bool shown) {
                    const QTableWidgetItem* header = sessions_->horizontalHeaderItem(column);
                    if (header != nullptr) {
                        const QString name = header->data(Qt::UserRole).toString();
                        sessionColumnVisibility_.insert(
                            name.isEmpty() ? header->text() : name, shown);
                    }
                    sessions_->setColumnHidden(column, !shown);
                });
            }
        });
    sessionDetails_ = new QLabel(QStringLiteral("选择一个会话查看详情。"), sessionsPage);
    sessionDetails_->setObjectName(QStringLiteral("sessionDetails"));
    sessionDetails_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    sessionDetails_->setWordWrap(true);
    sessionSql_ = new QPlainTextEdit(sessionsPage);
    sessionSql_->setFont(Theme::editorFont());
    sessionSql_->setObjectName(QStringLiteral("sessionFullSql"));
    sessionSql_->setAccessibleName(QStringLiteral("选中会话完整 SQL"));
    sessionSql_->setReadOnly(true);
    sessionSql_->setPlaceholderText(QStringLiteral("选择会话后点击“完整 SQL”。"));
    auto* sqlActions = new QHBoxLayout();
    copySessionSql_ = new QPushButton(QStringLiteral("复制 SQL"), sessionsPage);
    openSessionSql_ = new QPushButton(QStringLiteral("SQL 窗口"), sessionsPage);
    sqlActions->addWidget(copySessionSql_);
    sqlActions->addWidget(openSessionSql_);
    sqlActions->addStretch(1);
    sessionDetailTabs_ = new QTabWidget(sessionsPage);
    sessionDetailTabs_->setObjectName(QStringLiteral("sessionDetailTabs"));
    sessionDetailTabs_->setAccessibleName(QStringLiteral("选中会话详情"));
    auto* overviewPage = new QWidget(sessionDetailTabs_);
    auto* overviewLayout = new QVBoxLayout(overviewPage);
    overviewLayout->setContentsMargins(8, 8, 8, 8);
    overviewLayout->addWidget(sessionDetails_);
    overviewLayout->addStretch(1);
    sessionDetailTabs_->addTab(overviewPage, QStringLiteral("概览"));
    auto* sqlPage = new QWidget(sessionDetailTabs_);
    auto* sqlLayout = new QVBoxLayout(sqlPage);
    sqlLayout->setContentsMargins(8, 8, 8, 8);
    sqlLayout->setSpacing(4);
    sqlLayout->addWidget(sessionSql_, 1);
    sqlLayout->addLayout(sqlActions);
    sessionDetailTabs_->addTab(sqlPage, QStringLiteral("当前／最近 SQL"));
    auto* lockPage = new QWidget(sessionDetailTabs_);
    auto* lockLayout = new QVBoxLayout(lockPage);
    lockLayout->setContentsMargins(8, 8, 8, 8);
    sessionLockDetails_ = new QLabel(QStringLiteral("选择会话后查看事务和锁诊断入口。"), lockPage);
    sessionLockDetails_->setWordWrap(true);
    sessionLockDetails_->setTextInteractionFlags(
        Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    auto* openLockAnalysis = new QPushButton(QStringLiteral("锁等待分析"), lockPage);
    lockLayout->addWidget(sessionLockDetails_);
    lockLayout->addWidget(openLockAnalysis, 0, Qt::AlignLeft);
    lockLayout->addStretch(1);
    sessionDetailTabs_->addTab(lockPage, QStringLiteral("锁与事务"));
    sessionDetailTabs_->hide();
    auto* sessionsSplit = new QSplitter(Qt::Vertical, sessionsPage);
    sessionsSplit->setObjectName(QStringLiteral("sessionManagerSplitter"));
    sessionsSplit->addWidget(sessions_);
    sessionsSplit->addWidget(sessionDetailTabs_);
    sessionsSplit->setStretchFactor(0, 7);
    sessionsSplit->setStretchFactor(1, 3);
    sessionsLayout->addWidget(sessionsSplit, 1);
    connect(openLockAnalysis, &QPushButton::clicked,
        this, [this]() { openSessionLocks_->click(); });
    pages_->addTab(sessionsPage, QStringLiteral("会话"));

    auto* locksPage = new QWidget(pages_);
    auto* locksLayout = new QVBoxLayout(locksPage);
    locksLayout->setContentsMargins(8, 8, 8, 8);
    locksLayout->setSpacing(6);
    auto* lockActions = new QWidget(locksPage);
    lockActions->setObjectName(QStringLiteral("lockWaitActions"));
    auto* lockActionsLayout = new WrappingControlLayout(lockActions);
    lockActionsLayout->setContentsMargins(0, 0, 0, 0);
    lockActionsLayout->setSpacing(6);
    if (kind_ == DbaDocumentKind::Locks) {
        context_->setObjectName(QStringLiteral("lockWaitConnectionState"));
        context_->setContentsMargins(0, 0, 0, 0);
        context_->setWordWrap(false);
        context_->setMaximumWidth(fontMetrics().horizontalAdvance(QLatin1Char('M')) * 48);
        lockActionsLayout->addWidget(context_);
    }
    refreshLocks_ = CommandButton::refresh(locksPage, QStringLiteral("刷新锁等待"), QStringLiteral("锁等待"));
    fitLockGraph_ = new QPushButton(QStringLiteral("适应窗口"), locksPage);
    lockRefreshInterval_ = new AdaptivePopupComboBox(locksPage);
    lockRefreshInterval_->addItem(QStringLiteral("手动刷新"), 0);
    lockRefreshInterval_->addItem(QStringLiteral("每 5 秒"), 5'000);
    lockRefreshInterval_->addItem(QStringLiteral("每 10 秒"), 10'000);
    lockRefreshInterval_->addItem(QStringLiteral("每 30 秒"), 30'000);
    lockRefreshInterval_->setAccessibleName(QStringLiteral("锁等待自动刷新频率"));
    selectedLockChainOnly_ = new QCheckBox(QStringLiteral("仅显示选中链"), locksPage);
    selectedLockChainOnly_->setEnabled(false);
    lockActionsLayout->addWidget(refreshLocks_);
    lockActionsLayout->addWidget(fitLockGraph_);
    auto* lockRefreshGroup = new QWidget(lockActions);
    auto* lockRefreshLayout = new QHBoxLayout(lockRefreshGroup);
    lockRefreshLayout->setContentsMargins(0, 0, 0, 0);
    lockRefreshLayout->setSpacing(6);
    lockRefreshLayout->addWidget(new QLabel(QStringLiteral("自动刷新："), lockRefreshGroup));
    lockRefreshLayout->addWidget(lockRefreshInterval_);
    lockActionsLayout->addWidget(lockRefreshGroup);
    lockActionsLayout->addWidget(selectedLockChainOnly_);
    lockUpdatedAt_ = new QLabel(QStringLiteral("尚未刷新"), locksPage);
    lockUpdatedAt_->setObjectName(QStringLiteral("lockUpdatedAt"));
    lockActionsLayout->addWidget(lockUpdatedAt_);
    locksLayout->addWidget(lockActions);
    lockState_ = new QLabel(QStringLiteral("尚未加载锁等待关系。"), locksPage);
    lockState_->setObjectName(QStringLiteral("lockWaitState"));
    lockState_->setWordWrap(true);
    locksLayout->addWidget(lockState_);
    lockSummary_ = new QLabel(QStringLiteral("关系 0｜会话 0｜根阻塞者 0｜最大深度 0｜循环未检测"), locksPage);
    lockSummary_->setObjectName(QStringLiteral("lockWaitSummary"));
    lockSummary_->hide();
    locksLayout->addWidget(lockSummary_);
    locks_ = new QTableWidget(locksPage);
    locks_->setObjectName(QStringLiteral("lockWaitTable"));
    locks_->setAccessibleName(QStringLiteral("锁等待精确关系表"));
    locks_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    locks_->setSelectionBehavior(QAbstractItemView::SelectRows);
    locks_->setSelectionMode(QAbstractItemView::SingleSelection);
    locks_->setAlternatingRowColors(true);
    locks_->setSortingEnabled(true);
    lockGraph_ = new LockWaitGraphWidget(locksPage);
    lockGraph_->setObjectName(QStringLiteral("lockWaitGraph"));
    lockDetails_ = new QPlainTextEdit(locksPage);
    lockDetails_->setObjectName(QStringLiteral("lockWaitDetails"));
    lockDetails_->setAccessibleName(QStringLiteral("选中锁等待关系详情"));
    lockDetails_->setReadOnly(true);
    lockDetails_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    lockDetails_->setPlaceholderText(QStringLiteral("选择关系表行或图中连线查看详情。"));
    auto* lockDetailActions = new QToolBar(locksPage);
    lockDetailActions->setObjectName(QStringLiteral("lockWaitDetailActions"));
    lockDetailActions->setProperty("secondaryToolBar", true);
    copyLockDetails_ = new QPushButton(QStringLiteral("复制详情"), locksPage);
    openBlockerSql_ = new QPushButton(QStringLiteral("阻塞 SQL"), locksPage);
    openWaiterSql_ = new QPushButton(QStringLiteral("等待 SQL"), locksPage);
    cancelBlockerOperation_ = new QPushButton(QStringLiteral("取消阻塞操作"), locksPage);
    killBlocker_ = new QPushButton(QStringLiteral("Kill 阻塞会话"), locksPage);
    killBlocker_->setProperty("dangerAction", true);
    killWaiter_ = new QPushButton(QStringLiteral("Kill 等待会话"), locksPage);
    killWaiter_->setProperty("quietAction", true);
    lockDetailActions->addWidget(copyLockDetails_);
    lockDetailActions->addWidget(openBlockerSql_);
    lockDetailActions->addWidget(openWaiterSql_);
    lockDetailActions->addSeparator();
    lockDetailActions->addWidget(cancelBlockerOperation_);
    lockDetailActions->addWidget(killBlocker_);
    lockDetailActions->addWidget(killWaiter_);
    auto* lockDetailPanel = new QWidget(locksPage);
    auto* lockDetailLayout = new QVBoxLayout(lockDetailPanel);
    lockDetailLayout->setContentsMargins(0, 0, 0, 0);
    lockDetailLayout->setSpacing(4);
    lockDetailLayout->addWidget(new QLabel(QStringLiteral("诊断详情"), lockDetailPanel));
    lockDetailLayout->addWidget(lockDetails_, 1);
    lockDetailLayout->addWidget(lockDetailActions);
    lockDetailPanel->hide();
    auto* lockLowerSplit = new QSplitter(Qt::Horizontal, locksPage);
    lockLowerSplit->setObjectName(QStringLiteral("lockWaitDetailSplitter"));
    lockLowerSplit->addWidget(locks_);
    lockLowerSplit->addWidget(lockDetailPanel);
    lockLowerSplit->setStretchFactor(0, 7);
    lockLowerSplit->setStretchFactor(1, 3);
    auto* locksSplit = new QSplitter(Qt::Vertical, locksPage);
    locksSplit->setObjectName(QStringLiteral("lockWaitMainSplitter"));
    locksSplit->addWidget(lockGraph_);
    locksSplit->addWidget(lockLowerSplit);
    locksSplit->setStretchFactor(0, 4);
    locksSplit->setStretchFactor(1, 6);
    lockContent_ = new QStackedWidget(locksPage);
    lockContent_->setObjectName(QStringLiteral("lockWaitContent"));
    auto* lockEmpty = new QWidget(lockContent_);
    lockEmpty->setObjectName(QStringLiteral("lockWaitEmptyState"));
    auto* lockEmptyLayout = new QVBoxLayout(lockEmpty);
    lockEmptyLayout->setContentsMargins(24, 24, 24, 24);
    lockEmptyLayout->setSpacing(6);
    lockEmptyLayout->addStretch();
    lockEmptyTitle_ = new QLabel(QStringLiteral("尚未查询锁等待"), lockEmpty);
    lockEmptyTitle_->setObjectName(QStringLiteral("lockWaitEmptyTitle"));
    lockEmptyTitle_->setProperty("resultPaneEmptyTitle", true);
    lockEmptyDetail_ = new QLabel(QStringLiteral("连接后点击“刷新锁等待”，查看当前等待关系。"), lockEmpty);
    lockEmptyDetail_->setObjectName(QStringLiteral("lockWaitEmptyDetail"));
    lockEmptyDetail_->setProperty("resultPaneEmptyDetail", true);
    for (auto* label : {lockEmptyTitle_, lockEmptyDetail_}) {
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        lockEmptyLayout->addWidget(label);
    }
    lockEmptyLayout->addStretch();
    lockContent_->addWidget(lockEmpty);
    lockContent_->addWidget(locksSplit);
    locksLayout->addWidget(lockContent_, 1);
    pages_->addTab(locksPage, QStringLiteral("锁等待"));

    auto* performancePage = new QWidget(pages_);
    auto* performanceLayout = new QVBoxLayout(performancePage);
    auto* performanceTools = new QWidget(performancePage);
    auto* performanceActions = new WrappingControlLayout(performanceTools);
    performanceActions->setContentsMargins(0, 0, 0, 0);
    performanceActions->setSpacing(4);
    refreshPerformance_ = new QPushButton(QStringLiteral("采样会话数"), performancePage);
    refreshSlowSql_ = CommandButton::refresh(performancePage, QStringLiteral("刷新慢 SQL"), QStringLiteral("慢 SQL"));
    auto* autoPerformance = new QCheckBox(QStringLiteral("每 5 秒采样"), performancePage);
    performanceActions->addWidget(refreshPerformance_);
    performanceActions->addWidget(refreshSlowSql_);
    performanceActions->addWidget(autoPerformance);
    performanceLayout->addWidget(performanceTools);
    trend_ = new QLabel(QStringLiteral(
        "会话趋势尚无样本。CPU／内存单位尚未从目标 DM8 版本确认，不显示猜测值。"),
        performancePage);
    performanceLayout->addWidget(trend_);
    slowSource_ = new QLabel(QStringLiteral("慢 SQL 数据源：尚未刷新。"), performancePage);
    performanceLayout->addWidget(slowSource_);
    slowSql_ = new QTableWidget(performancePage);
    slowSql_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    trendChart_ = new SessionTrendWidget(performancePage);
    auto* performanceSplit = new QSplitter(Qt::Vertical, performancePage);
    performanceSplit->addWidget(trendChart_);
    performanceSplit->addWidget(slowSql_);
    performanceSplit->setStretchFactor(0, 2);
    performanceSplit->setStretchFactor(1, 3);
    performanceLayout->addWidget(performanceSplit, 1);
    pages_->addTab(performancePage, QStringLiteral("性能"));

    auto* tablespacesPage = new QWidget(pages_);
    auto* tablespacesLayout = new QVBoxLayout(tablespacesPage);
    auto* tablespacesTools = new QWidget(tablespacesPage);
    auto* tablespacesActions = new WrappingControlLayout(tablespacesTools);
    tablespacesActions->setContentsMargins(0, 0, 0, 0);
    tablespacesActions->setSpacing(4);
    refreshTablespaces_ = CommandButton::refresh(tablespacesPage, QStringLiteral("刷新表空间"), QStringLiteral("表空间"));
    refreshDataFiles_ = CommandButton::refresh(tablespacesPage, QStringLiteral("刷新数据文件"), QStringLiteral("数据文件"));
    tablespaceFilter_ = new QLineEdit(tablespacesPage);
    SearchField::setPrompt(tablespaceFilter_, QStringLiteral("名称／状态／路径"), QStringLiteral("筛选名称、状态或路径"));
    tablespacesActions->addWidget(refreshTablespaces_);
    tablespacesActions->addWidget(refreshDataFiles_);
    tablespacesActions->addWidget(tablespaceFilter_, 1);
    tablespacesLayout->addWidget(tablespacesTools);
    tablespaceState_ = new QLabel(QStringLiteral(
        "尚未加载。只读查询；表空间和数据文件变更未开放。"), tablespacesPage);
    tablespacesLayout->addWidget(tablespaceState_);
    tablespaces_ = new QTableWidget(tablespacesPage);
    tablespaces_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    dataFiles_ = new QTableWidget(tablespacesPage);
    dataFiles_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auto* tablespaceSplit = new QSplitter(Qt::Vertical, tablespacesPage);
    tablespaceSplit->addWidget(
        tableSection(QStringLiteral("表空间摘要"), tablespaces_, tablespaceSplit));
    tablespaceSplit->addWidget(
        tableSection(QStringLiteral("数据文件详情"), dataFiles_, tablespaceSplit));
    tablespaceSplit->setStretchFactor(0, 1);
    tablespaceSplit->setStretchFactor(1, 1);
    tablespacesLayout->addWidget(tablespaceSplit, 1);
    pages_->addTab(tablespacesPage, QStringLiteral("表空间"));

    auto* usersPage = new QWidget(pages_);
    auto* usersLayout = new QVBoxLayout(usersPage);
    auto* usersTools = new QWidget(usersPage);
    auto* usersActions = new WrappingControlLayout(usersTools);
    usersActions->setContentsMargins(0, 0, 0, 0);
    usersActions->setSpacing(4);
    refreshUsers_ = CommandButton::refresh(usersPage, QStringLiteral("刷新用户"), QStringLiteral("用户"));
    createUser_ = new QPushButton(QStringLiteral("新建用户…"), usersPage);
    usersFilter_ = new QLineEdit(usersPage);
    SearchField::setPrompt(usersFilter_, QStringLiteral("用户"), QStringLiteral("筛选用户"));
    privilegesFilter_ = new QLineEdit(usersPage);
    SearchField::setPrompt(privilegesFilter_, QStringLiteral("已加载权限"), QStringLiteral("筛选已加载权限"));
    roleMembersFilter_ = new QLineEdit(usersPage);
    SearchField::setPrompt(roleMembersFilter_, QStringLiteral("已加载用户"), QStringLiteral("筛选已加载用户"));
    if (kind_ != DbaDocumentKind::Roles) {
        usersActions->addWidget(createUser_);
        usersActions->addWidget(refreshUsers_);
        usersLayout->addWidget(usersTools);
    } else {
        usersTools->hide();
        refreshUsers_->hide();
        createUser_->hide();
    }
    usersState_ = new QLabel(QStringLiteral(
        "尚未加载。只显示非敏感用户字段；用户和授权变更未开放。"), usersPage);
    usersLayout->addWidget(usersState_);
    users_ = new QTableWidget(usersPage);
    users_->setSelectionBehavior(QAbstractItemView::SelectRows);
    users_->setSelectionMode(QAbstractItemView::SingleSelection);
    users_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    privileges_ = new QTableWidget(usersPage);
    privileges_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    roleMembers_ = new QTableWidget(usersPage);
    roleMembers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auto* usersSplit = new QSplitter(
        kind_ == DbaDocumentKind::Roles ? Qt::Horizontal : Qt::Vertical,
        usersPage);
    QWidget* usersSection = kind_ == DbaDocumentKind::Roles
            || kind_ == DbaDocumentKind::Users
        ? pagedTableSection(kind_ == DbaDocumentKind::Roles
                ? QStringLiteral("角色") : QStringLiteral("用户"), users_, usersSplit,
            rolesPrevious_, rolesPageLabel_, rolesNext_, rolesPageSize_, usersFilter_)
        : tableSection(QStringLiteral("用户"), users_, usersSplit);
    QWidget* privilegesSection = kind_ == DbaDocumentKind::Roles
        ? pagedTableSection(QStringLiteral("角色拥有的权限"), privileges_, usersSplit,
            rolePrivilegesPrevious_, rolePrivilegesPageLabel_,
            rolePrivilegesNext_, rolePrivilegesPageSize_, privilegesFilter_)
        : tableSection(
            QStringLiteral("角色、系统权限和对象权限"), privileges_, usersSplit);
    QWidget* roleMembersSection = kind_ == DbaDocumentKind::Roles
        ? pagedTableSection(QStringLiteral("角色下的用户"), roleMembers_, usersSplit,
            roleMembersPrevious_, roleMembersPageLabel_,
            roleMembersNext_, roleMembersPageSize_, roleMembersFilter_)
        : tableSection(QStringLiteral("角色下的用户"), roleMembers_, usersSplit);
    usersSplit->addWidget(usersSection);
    if (kind_ == DbaDocumentKind::Roles) {
        auto* roleDetailsSplit = new QSplitter(Qt::Vertical, usersSplit);
        roleDetailsSplit->addWidget(privilegesSection);
        roleDetailsSplit->addWidget(roleMembersSection);
        roleDetailsSplit->setStretchFactor(0, 1);
        roleDetailsSplit->setStretchFactor(1, 1);
        usersSplit->addWidget(roleDetailsSplit);
        usersSplit->setStretchFactor(0, 1);
        usersSplit->setStretchFactor(1, 2);
    } else {
        privilegesSection->hide();
        roleMembersSection->hide();
        usersSplit->setStretchFactor(0, 1);
    }
    usersLayout->addWidget(usersSplit, 1);
    pages_->addTab(usersPage, QStringLiteral("用户权限"));

    auto* jobsPage = new QWidget(pages_);
    auto* jobsLayout = new QVBoxLayout(jobsPage);
    auto* jobsTools = new QWidget(jobsPage);
    auto* jobsActions = new WrappingControlLayout(jobsTools);
    jobsActions->setContentsMargins(0, 0, 0, 0);
    jobsActions->setSpacing(4);
    refreshJobs_ = CommandButton::refresh(jobsPage, QStringLiteral("刷新作业"), QStringLiteral("作业"));
    refreshJobSchedules_ = CommandButton::refresh(jobsPage, QStringLiteral("刷新调度计划"), QStringLiteral("调度计划"));
    refreshJobHistory_ = CommandButton::refresh(jobsPage, QStringLiteral("刷新执行历史"), QStringLiteral("执行历史"));
    jobsFilter_ = new QLineEdit(jobsPage);
    SearchField::setPrompt(jobsFilter_, QStringLiteral("作业／调度／状态"), QStringLiteral("筛选作业、调度或状态"));
    jobsActions->addWidget(refreshJobs_);
    jobsActions->addWidget(refreshJobSchedules_);
    jobsActions->addWidget(refreshJobHistory_);
    jobsActions->addWidget(jobsFilter_, 1);
    jobsLayout->addWidget(jobsTools);
    jobsState_ = new QLabel(QStringLiteral(
        "尚未加载。不会初始化作业环境，也不会创建、启停或删除作业。"), jobsPage);
    jobsLayout->addWidget(jobsState_);
    jobs_ = new QTableWidget(jobsPage);
    jobs_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    jobSchedules_ = new QTableWidget(jobsPage);
    jobSchedules_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    jobHistory_ = new QTableWidget(jobsPage);
    jobHistory_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auto* jobsSplit = new QSplitter(Qt::Vertical, jobsPage);
    auto* jobsSection =
        tableSection(QStringLiteral("当前用户作业"), jobs_, jobsSplit);
    auto* schedulesSection =
        tableSection(QStringLiteral("调度计划"), jobSchedules_, jobsSplit);
    auto* historySection =
        tableSection(QStringLiteral("执行历史"), jobHistory_, jobsSplit);
    jobsSplit->addWidget(jobsSection);
    jobsSplit->addWidget(schedulesSection);
    jobsSplit->addWidget(historySection);
    for (int section = 0; section < 3; ++section) jobsSplit->setStretchFactor(section, 1);
    jobsLayout->addWidget(jobsSplit, 1);
    pages_->addTab(jobsPage, QStringLiteral("作业"));

    auto* parametersPage = new QWidget(pages_);
    auto* parametersLayout = new QVBoxLayout(parametersPage);
    parametersLayout->setContentsMargins(4, 0, 4, 4);
    parametersLayout->setSpacing(4);
    auto* parametersTools = new QToolBar(parametersPage);
    parametersTools->setProperty("secondaryToolBar", true);
    refreshParameters_ = CommandButton::refresh(parametersPage, QStringLiteral("刷新环境快照"), QStringLiteral("环境快照"));
    parametersTools->addWidget(refreshParameters_);
    parametersTools->addSeparator();
    parametersState_ = new QLabel(QStringLiteral(
        "尚未加载数据库环境。所有查询均为只读。"), parametersPage);
    parametersTools->addWidget(parametersState_);
    parametersTools->hide();

    auto setupEnvironmentTable = [](QTableWidget* table, const QString& name) {
        table->setObjectName(name);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSortingEnabled(true);
        table->setAlternatingRowColors(true);
        const int rowHeight = qMax(26, table->fontMetrics().height() + 8);
        table->verticalHeader()->setMinimumSectionSize(rowHeight);
        table->verticalHeader()->setDefaultSectionSize(rowHeight);
        table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table->horizontalHeader()->setFixedHeight(rowHeight + 2);
        table->setWordWrap(false);
        table->setShowGrid(false);
        table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    };
    auto* environmentTabs = new QTabWidget(parametersPage);
    environmentTabs->setObjectName(QStringLiteral("databaseEnvironmentTabs"));

    environmentOverview_ = new QTableWidget(environmentTabs);
    setupEnvironmentTable(environmentOverview_, QStringLiteral("environmentOverviewTable"));
    environmentTabs->addTab(environmentOverview_, QStringLiteral("概览"));

    auto* optionsPage = new QWidget(environmentTabs);
    auto* optionsLayout = new QVBoxLayout(optionsPage);
    optionsLayout->setContentsMargins(8, 8, 8, 8);
    optionsLayout->setSpacing(6);
    auto* optionsTools = new QHBoxLayout();
    databaseOptionsFilter_ = new QLineEdit(optionsPage);
    databaseOptionsFilter_->setObjectName(QStringLiteral("databaseOptionsFilter"));
    databaseOptionsFilter_->setClearButtonEnabled(true);
    SearchField::setPrompt(databaseOptionsFilter_, QStringLiteral("选项／值／说明"), QStringLiteral("搜索选项名称、当前值或说明"));
    databaseOptionsFilter_->setAccessibleName(QStringLiteral("搜索数据库选项"));
    optionsTools->addWidget(databaseOptionsFilter_, 1);
    optionsLayout->addLayout(optionsTools);
    databaseOptionsState_ = new QLabel(QStringLiteral(
        "尚未读取 V$OPTION；刷新环境快照后自动加载。"), optionsPage);
    databaseOptionsState_->setObjectName(QStringLiteral("databaseOptionsState"));
    databaseOptionsState_->setAccessibleName(QStringLiteral("数据库选项加载状态"));
    databaseOptionsState_->setWordWrap(true);
    optionsLayout->addWidget(databaseOptionsState_);
    databaseOptions_ = new QTableWidget(optionsPage);
    setupEnvironmentTable(databaseOptions_, QStringLiteral("databaseOptionsTable"));
    optionsLayout->addWidget(databaseOptions_, 1);
    environmentTabs->addTab(optionsPage, QStringLiteral("数据库选项"));

    auto* commonPage = new QWidget(environmentTabs);
    auto* commonLayout = new QVBoxLayout(commonPage);
    commonLayout->setContentsMargins(8, 8, 8, 8);
    commonLayout->setSpacing(6);
    commonParametersSummary_ = new QLabel(
        QStringLiteral("常用参数 0｜非默认 0｜待生效 0｜显示 0"), commonPage);
    commonParametersSummary_->setObjectName(QStringLiteral("commonParametersSummary"));
    commonParametersSummary_->setAccessibleName(QStringLiteral("常用参数状态摘要"));
    commonParametersPresetFilter_ = new AdaptivePopupComboBox(commonPage);
    commonParametersPresetFilter_->setObjectName(
        QStringLiteral("commonParametersPresetFilter"));
    commonParametersPresetFilter_->setAccessibleName(QStringLiteral("常用参数状态筛选"));
    commonParametersPresetFilter_->addItem(QStringLiteral("全部状态"), QStringLiteral("all"));
    commonParametersPresetFilter_->addItem(
        QStringLiteral("仅非默认值"), QStringLiteral("nonDefault"));
    commonParametersPresetFilter_->addItem(
        QStringLiteral("仅待生效"), QStringLiteral("pending"));
    auto* commonTools = new QHBoxLayout();
    commonParametersFilter_ = new QLineEdit(commonPage);
    commonParametersFilter_->setObjectName(QStringLiteral("commonParametersFilter"));
    commonParametersFilter_->setClearButtonEnabled(true);
    SearchField::setPrompt(commonParametersFilter_, QStringLiteral("参数／分类／说明"), QStringLiteral("搜索常用参数、分类或说明"));
    commonParametersFilter_->setAccessibleName(QStringLiteral("搜索常用参数"));
    commonTools->addWidget(commonParametersFilter_, 1);
    commonTools->addWidget(commonParametersPresetFilter_);
    commonTools->addWidget(commonParametersSummary_);
    commonLayout->addLayout(commonTools);
    commonParameters_ = new QTableWidget(commonPage);
    setupEnvironmentTable(commonParameters_, QStringLiteral("commonParametersTable"));
    commonLayout->addWidget(commonParameters_);
    environmentTabs->addTab(commonPage, QStringLiteral("常用参数"));

    auto* allPage = new QWidget(environmentTabs);
    auto* allLayout = new QVBoxLayout(allPage);
    auto* allTools = new QHBoxLayout();
    parametersFilter_ = new QLineEdit(allPage);
    parametersFilter_->setObjectName(QStringLiteral("allParametersFilter"));
    parametersFilter_->setClearButtonEnabled(true);
    SearchField::setPrompt(parametersFilter_, QStringLiteral("参数／值／说明"), QStringLiteral("搜索名称、值、类型、模块或说明"));
    parametersFilter_->setAccessibleName(QStringLiteral("搜索全部参数"));
    nonDefaultOnly_ = new QCheckBox(QStringLiteral("仅非默认值"), allPage);
    pendingOnly_ = new QCheckBox(QStringLiteral("仅待生效"), allPage);
    allTools->addWidget(parametersFilter_, 1);
    allTools->addWidget(nonDefaultOnly_);
    allTools->addWidget(pendingOnly_);
    allLayout->addLayout(allTools);
    parameters_ = new QTableWidget(allPage);
    setupEnvironmentTable(parameters_, QStringLiteral("allParametersTable"));
    allLayout->addWidget(parameters_);
    environmentTabs->addTab(allPage, QStringLiteral("全部参数"));
    auto* watchedPage = new QWidget(environmentTabs);
    auto* watchedLayout = new QVBoxLayout(watchedPage);
    watchedParametersFilter_ = new QLineEdit(watchedPage);
    SearchField::setPrompt(watchedParametersFilter_, QStringLiteral("关注参数"), QStringLiteral("搜索关注参数名称、说明、值…"));
    watchedParametersFilter_->setClearButtonEnabled(true);
    watchedLayout->addWidget(watchedParametersFilter_);
    watchedParameters_ = new QTableWidget(watchedPage);
    watchedLayout->addWidget(watchedParameters_, 1);
    connect(watchedParametersFilter_, &QLineEdit::textChanged, this, [this](const QString& text) {
        filterTable(watchedParameters_, DatabaseEnvironment::canonicalParameterName(text));
    });
    setupEnvironmentTable(watchedParameters_, QStringLiteral("watchedParametersTable"));
    environmentTabs->addTab(watchedPage, QStringLiteral("关注参数"));
    parameterMetadata_ = DatabaseEnvironment::loadMetadata();
    refreshWatchedParameters();
    connect(environmentTabs, &QTabWidget::currentChanged, this, [this]() {
        refreshWatchedParameters();
    });

    auto* comparePage = new QWidget(environmentTabs);
    auto* compareLayout = new QVBoxLayout(comparePage);
    auto* compareTools = new QHBoxLayout();
    compareTools->setSpacing(8);
    compareTools->addWidget(new QLabel(QStringLiteral("当前环境："), comparePage));
    comparisonCurrent_ = new QLabel(QStringLiteral("未连接"), comparePage);
    comparisonCurrent_->setObjectName(QStringLiteral("environmentComparisonCurrent"));
    comparisonCurrent_->setAccessibleName(QStringLiteral("当前数据库连接"));
    compareTools->addWidget(comparisonCurrent_);
    compareTools->addWidget(new QLabel(QStringLiteral("目标环境："), comparePage));
    comparisonProfile_ = new AdaptivePopupComboBox(comparePage);
    comparisonProfile_->setObjectName(QStringLiteral("environmentComparisonProfile"));
    comparisonProfile_->setAccessibleName(QStringLiteral("目标数据库连接"));
    compareTools->addWidget(comparisonProfile_, 1);
    refreshComparisonProfiles_ = CommandButton::refresh(comparePage, QStringLiteral("刷新连接"), QStringLiteral("连接"));
    refreshComparisonProfiles_->setObjectName(
        QStringLiteral("environmentComparisonRefreshProfiles"));
    refreshComparisonProfiles_->setAccessibleName(QStringLiteral("刷新目标连接列表"));
    refreshComparisonProfiles_->setToolTip(QStringLiteral(
        "重新读取连接配置和凭据保存状态"));
    compareTools->addWidget(refreshComparisonProfiles_);
    compareEnvironment_ = new QPushButton(QStringLiteral("开始只读对比"), comparePage);
    compareEnvironment_->setObjectName(QStringLiteral("environmentComparisonStart"));
    compareEnvironment_->setAccessibleName(QStringLiteral("开始只读环境对比"));
    compareEnvironment_->setEnabled(false);
    compareTools->addWidget(compareEnvironment_);
    compareLayout->addLayout(compareTools);
    comparisonState_ = new QLabel(QStringLiteral(
        "先刷新当前环境快照，再选择目标连接进行只读对比。"), comparePage);
    comparisonState_->setObjectName(QStringLiteral("environmentComparisonState"));
    comparisonState_->setAccessibleName(QStringLiteral("环境对比状态"));
    comparisonState_->setWordWrap(true);
    comparisonState_->setTextInteractionFlags(
        Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    auto* comparisonStatus = new QHBoxLayout();
    comparisonStatus->addWidget(comparisonState_, 1);
    comparisonMode_ = new AdaptivePopupComboBox(comparePage);
    comparisonMode_->setObjectName(QStringLiteral("environmentComparisonMode"));
    comparisonMode_->setAccessibleName(QStringLiteral("参数对比方式"));
    comparisonMode_->addItem(QStringLiteral("环境差异对比"), false);
    comparisonMode_->addItem(QStringLiteral("主备参数对比"), true);
    comparisonMode_->setToolTip(QStringLiteral("主备模式同时核对文件值及特殊重演同步项；不自动认定两个连接属于同一守护组。"));
    comparisonStatus->addWidget(comparisonMode_);
    auto* severityLabel = new QLabel(QStringLiteral("级别："), comparePage);
    comparisonStatus->addWidget(severityLabel);
    comparisonSeverityFilter_ = new AdaptivePopupComboBox(comparePage);
    comparisonSeverityFilter_->setObjectName(QStringLiteral("environmentComparisonSeverityFilter"));
    comparisonSeverityFilter_->setAccessibleName(QStringLiteral("比较结果级别筛选"));
    comparisonSeverityFilter_->addItem(QStringLiteral("全部级别"), -1);
    for (EnvironmentSeverity severity : {EnvironmentSeverity::Critical,
            EnvironmentSeverity::Warning, EnvironmentSeverity::Info}) {
        comparisonSeverityFilter_->addItem(DatabaseEnvironment::severityLabel(severity),
            static_cast<int>(severity));
    }
    severityLabel->setBuddy(comparisonSeverityFilter_);
    comparisonStatus->addWidget(comparisonSeverityFilter_);
    comparisonFilterState_ = new QLabel(QStringLiteral("显示 0／0 项"), comparePage);
    comparisonFilterState_->setObjectName(QStringLiteral("environmentComparisonFilterState"));
    comparisonFilterState_->setAccessibleName(QStringLiteral("比较结果筛选数量"));
    comparisonStatus->addWidget(comparisonFilterState_);
    compareLayout->addLayout(comparisonStatus);
    environmentComparison_ = new QTableWidget(comparePage);
    setupEnvironmentTable(environmentComparison_, QStringLiteral("environmentComparisonTable"));
    environmentComparison_->setColumnCount(7);
    environmentComparison_->setHorizontalHeaderLabels({QStringLiteral("级别"),
        QStringLiteral("参数"), QStringLiteral("说明"), QStringLiteral("当前环境"),
        QStringLiteral("目标环境"), QStringLiteral("变更方式"), QStringLiteral("判断")});
    environmentComparison_->horizontalHeader()->setStretchLastSection(true);
    compareLayout->addWidget(environmentComparison_);
    comparisonDetails_ = new QPlainTextEdit(comparePage);
    comparisonDetails_->setObjectName(QStringLiteral("environmentComparisonDetails"));
    comparisonDetails_->setAccessibleName(QStringLiteral("所选对比参数的完整说明"));
    comparisonDetails_->setReadOnly(true);
    comparisonDetails_->setMaximumHeight(180);
    comparisonDetails_->setPlaceholderText(QStringLiteral("选择一项查看完整说明、原始值与主备同步规则；右键可查看详情或修改当前环境参数。"));
    compareLayout->addWidget(comparisonDetails_);
    connect(comparisonMode_, &QComboBox::currentIndexChanged, this, [this]() { renderEnvironmentComparison(); });
    connect(environmentComparison_, &QTableWidget::itemSelectionChanged, this, [this]() {
        const auto* name = environmentComparison_->item(environmentComparison_->currentRow(), 1);
        comparisonDetails_->setPlainText(name ? comparisonDescriptions_.value(name->text()) : QString());
    });
    connect(environmentComparison_, &QTableWidget::cellDoubleClicked, this,
        [this](int row, int) { showEnvironmentDetails(environmentComparison_, row); });
    connect(comparisonSeverityFilter_, qOverload<int>(&QComboBox::currentIndexChanged),
        this, &DbaDocumentWidget::applyComparisonFilter);
    connect(environmentComparison_->model(), &QAbstractItemModel::layoutChanged,
        this, &DbaDocumentWidget::applyComparisonFilter);
    environmentTabs->addTab(comparePage, QStringLiteral("环境对比"));
    parametersLayout->addWidget(environmentTabs, 1);
    pages_->addTab(parametersPage, QStringLiteral("数据库参数"));
    for (QWidget* page : {optionsPage, commonPage, allPage, comparePage, watchedPage}) {
        if (page->layout()) {
            page->layout()->setContentsMargins(4, 4, 4, 0);
            page->layout()->setSpacing(4);
        }
    }
    const int controlHeight = qMax(28, fontMetrics().height() + 10);
    for (QLineEdit* filter : {databaseOptionsFilter_, commonParametersFilter_, parametersFilter_, watchedParametersFilter_}) {
        filter->setFixedHeight(controlHeight);
    }
    commonParametersPresetFilter_->setFixedHeight(controlHeight);
    commonParametersPresetFilter_->setMaximumWidth(150);
    comparisonSeverityFilter_->setFixedHeight(controlHeight);
    // Reorder pages without changing their identity or their signal connections.
    environmentTabs->tabBar()->moveTab(environmentTabs->indexOf(watchedPage), 0);
    environmentTabs->tabBar()->moveTab(environmentTabs->indexOf(commonPage), 1);
    environmentTabs->tabBar()->moveTab(environmentTabs->indexOf(allPage), 2);
    environmentTabs->setCurrentIndex(0);

    const auto installWatchMenu = [this](QTableWidget* table, int nameColumn) {
        table->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(table, &QWidget::customContextMenuRequested, this,
            [this, table, nameColumn](const QPoint& point) {
                const auto* clicked = table->itemAt(point);
                if (clicked == nullptr) return;
                const auto* nameItem = table->item(clicked->row(), nameColumn);
                if (nameItem == nullptr) return;
                const QString name = DatabaseEnvironment::canonicalParameterName(nameItem->text());
                if (name.isEmpty()) return;
                const bool remove = watchedParameterNames_.contains(name);
                QMenu menu(table);
                menu.addAction(remove ? QStringLiteral("取消关注") : QStringLiteral("关注"),
                    this, [this, name, remove]() {
                        QSettings settings;
                        QStringList names = settings.value(QStringLiteral("settings/databaseParameters/watched")).toStringList();
                        if (remove) names.removeAll(name);
                        else if (!names.contains(name)) names.append(name);
                        settings.setValue(QStringLiteral("settings/databaseParameters/watched"), names);
                        refreshWatchedParameters();
                    });
                if (table != databaseOptions_) {
                    menu.addAction(QStringLiteral("完整参数说明…"), this,
                        [this, table, name, nameColumn]() {
                            for (int row = 0; row < table->rowCount(); ++row) {
                                const auto* item = table->item(row, nameColumn);
                                if (item && item->text() == name) { showEnvironmentDetails(table, row); return; }
                            }
                        });
                    auto* edit = menu.addAction(QStringLiteral("修改当前环境参数…"), this,
                        [this, name]() { editEnvironmentParameter(name); });
                    const auto found = std::find_if(environmentSnapshot_.cbegin(), environmentSnapshot_.cend(),
                        [&name](const auto& item) { return item.name == name; });
                    edit->setObjectName(QStringLiteral("modifyEnvironmentParameter"));
                    edit->setEnabled(!isBusy() && !comparisonBusy_ && isConnected() && !profile_.readOnly && found != environmentSnapshot_.cend()
                        && DatabaseEnvironment::editBlockReason(*found).isEmpty());
                    edit->setToolTip(profile_.readOnly ? QStringLiteral("当前连接为只读，不能修改参数。")
                        : found == environmentSnapshot_.cend() ? QStringLiteral("当前环境未返回此参数，不能用目标侧参数修改当前侧。")
                        : !DatabaseEnvironment::editBlockReason(*found).isEmpty() ? DatabaseEnvironment::editBlockReason(*found)
                        : QStringLiteral("只修改当前窗口所连接的实例，不向目标环境或备库批量覆盖参数。"));
                }
                menu.exec(table->viewport()->mapToGlobal(point));
            });
    };
    installWatchMenu(parameters_, 2);
    installWatchMenu(commonParameters_, 2);
    installWatchMenu(databaseOptions_, 0);
    installWatchMenu(environmentComparison_, 1);
    installWatchMenu(watchedParameters_, 0);

    const auto openEnvironmentDetails = [this](QTableWidget* table, int row, int column,
                                                bool detailsColumnOnly) {
        const QTableWidgetItem* header = table->horizontalHeaderItem(column);
        const bool detailsColumn = header != nullptr
            && header->data(Qt::UserRole).toString() == QStringLiteral("PARAMETER_DETAILS");
        if (detailsColumnOnly != detailsColumn) return;
        showEnvironmentDetails(table, row);
    };
    for (QTableWidget* table : {parameters_, commonParameters_}) {
        connect(table, &QTableWidget::cellClicked, this,
            [openEnvironmentDetails, table](int row, int column) {
                openEnvironmentDetails(table, row, column, true);
            });
        connect(table, &QTableWidget::cellActivated, this,
            [openEnvironmentDetails, table](int row, int column) {
                openEnvironmentDetails(table, row, column, true);
            });
        connect(table, &QTableWidget::cellDoubleClicked, this,
            [openEnvironmentDetails, table](int row, int column) {
                openEnvironmentDetails(table, row, column, false);
            });
    }

    pages_->setCurrentIndex(dbaDocumentPage(kind_));
    pages_->tabBar()->hide();
    if (kind_ == DbaDocumentKind::Roles) {
        refreshUsers_->setText(QStringLiteral("角色"));
        refreshUsers_->setToolTip(QStringLiteral("刷新角色列表"));
        refreshUsers_->setAccessibleName(QStringLiteral("刷新角色列表"));
        SearchField::setPrompt(usersFilter_, QStringLiteral("角色"), QStringLiteral("筛选角色"));
        SearchField::setPrompt(privilegesFilter_, QStringLiteral("权限"), QStringLiteral("筛选权限"));
        SearchField::setPrompt(roleMembersFilter_, QStringLiteral("用户"), QStringLiteral("筛选用户"));
        usersState_->setText(QStringLiteral(
            "连接后自动加载角色第一页，并自动加载首个角色的权限和用户；授权变更未开放。"));
    } else {
        privilegesSection->hide();
        privilegesFilter_->hide();
        roleMembersFilter_->hide();
        roleMembersSection->hide();
        usersState_->setText(QStringLiteral(
            "连接后自动加载用户第一页；双击用户进入独立用户编辑器。"));
    }
    if (kind_ == DbaDocumentKind::Jobs
            || kind_ == DbaDocumentKind::JobSchedules
            || kind_ == DbaDocumentKind::JobHistory) {
        const bool showJobs = kind_ == DbaDocumentKind::Jobs;
        const bool showSchedules = kind_ == DbaDocumentKind::JobSchedules;
        const bool showHistory = kind_ == DbaDocumentKind::JobHistory;
        refreshJobs_->setVisible(showJobs);
        refreshJobSchedules_->setVisible(showSchedules);
        refreshJobHistory_->setVisible(showHistory);
        jobsSection->setVisible(showJobs);
        schedulesSection->setVisible(showSchedules);
        historySection->setVisible(showHistory);
        SearchField::setPrompt(jobsFilter_, showJobs ? QStringLiteral("作业")
            : showSchedules ? QStringLiteral("调度计划") : QStringLiteral("执行历史"),
            showJobs ? QStringLiteral("筛选作业") : showSchedules
                ? QStringLiteral("筛选调度计划") : QStringLiteral("筛选执行历史"));
    }
    // All management pages share the same workspace rhythm, including pages
    // that are hidden for the current document kind.
    for (int page = 0; page < pages_->count(); ++page) {
        if (auto* pageLayout = pages_->widget(page)->layout()) {
            pageLayout->setContentsMargins(8, 6, 8, 6);
            pageLayout->setSpacing(6);
        }
    }
    for (auto* label : {trend_, slowSource_, tablespaceState_, usersState_, jobsState_}) {
        label->setWordWrap(true);
        label->setProperty("secondaryText", true);
    }
    for (auto* filter : {tablespaceFilter_, usersFilter_, privilegesFilter_, roleMembersFilter_, jobsFilter_}) {
        filter->setClearButtonEnabled(true);
        filter->setAccessibleName(filter->placeholderText());
    }
    for (auto* table : pages_->findChildren<QTableWidget*>()) {
        table->setAlternatingRowColors(true);
        table->setTextElideMode(Qt::ElideRight);
        table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }
    layout->addWidget(pages_, 1);
    sampleTimer_ = new QTimer(this);
    sampleTimer_->setInterval(5'000);
    connect(sampleTimer_, &QTimer::timeout, this, &DbaDocumentWidget::refreshCounts);
    connect(autoPerformance, &QCheckBox::toggled, sampleTimer_, [this](bool enabled) {
        if (enabled) sampleTimer_->start(); else sampleTimer_->stop();
    });
    sessionsTimer_ = new QTimer(this);
    sessionsTimer_->setInterval(5'000);
    connect(sessionsTimer_, &QTimer::timeout, this, &DbaDocumentWidget::refreshSessions);
    connect(sessionRefreshInterval_, &QComboBox::currentIndexChanged, this, [this](int) {
        const int interval = sessionRefreshInterval_->currentData().toInt();
        sessionsTimer_->stop();
        if (interval > 0) {
            sessionsTimer_->setInterval(interval);
            sessionsTimer_->start();
        }
    });
    locksTimer_ = new QTimer(this);
    locksTimer_->setInterval(5'000);
    connect(locksTimer_, &QTimer::timeout, this, [this]() {
        if (kind_ == DbaDocumentKind::Locks && !isBusy()) refreshLocks();
    });
    connect(lockRefreshInterval_, &QComboBox::currentIndexChanged, this, [this](int) {
        const int interval = lockRefreshInterval_->currentData().toInt();
        locksTimer_->stop();
        if (interval > 0 && state_ == SessionState::Connected
                && kind_ == DbaDocumentKind::Locks) {
            locksTimer_->setInterval(interval);
            locksTimer_->start();
        }
    });

    sessionThread_ = new QThread(this);
    session_ = new DpiSession();
    session_->moveToThread(sessionThread_);
    connect(sessionThread_, &QThread::finished, session_, &QObject::deleteLater);
    connect(session_, &DpiSession::stateChanged, this, &DbaDocumentWidget::handleState);
    connect(session_, &DpiSession::connected, this, [this](const ConnectionProfile& profile) {
        hasSessionSnapshot_ = false;
        compareSessionSnapshot_ = false;
        previousSessionSnapshot_.clear();
        pendingSessionSnapshot_.clear();
        pendingSessionKeys_.clear();
        pendingSessionColumns_.clear();
        pendingSessionRows_.clear();
        sessionNewCount_ = 0;
        sessionChangedCount_ = 0;
        sessionEndedCount_ = 0;
        sessionLastRefresh_.clear();
        verifyingSessionId_ = 0;
        verifyingSessionSeq_ = 0;
        verifiedSessionSeq_ = 0;
        verifyingSessionFromLocks_ = false;
        verifiedSessionExists_ = false;
        verifiedSessionIdentityValid_ = false;
        pendingSessionActionOutcome_.clear();
        pendingSessionOutcomeForLocks_ = false;
        environmentSnapshot_.clear(); comparisonSnapshot_.clear();
        comparisonParameters_.clear(); comparisonDescriptions_.clear();
        environmentComparison_->setRowCount(0); comparisonDetails_->clear();
        parameterUpdateNotice_.clear();
        parameterRefreshPending_ = false;
        profile_ = profile;
        pendingProfile_ = {};
        refreshComparisonProfiles();
        refreshPrimaryView();
        const int interval = sessionRefreshInterval_->currentData().toInt();
        if (interval > 0) {
            sessionsTimer_->setInterval(interval);
            sessionsTimer_->start();
        }
        const int lockInterval = lockRefreshInterval_->currentData().toInt();
        if (kind_ == DbaDocumentKind::Locks && lockInterval > 0) {
            locksTimer_->setInterval(lockInterval);
            locksTimer_->start();
        }
    });
    connect(session_, &DpiSession::databaseVersionDetected, this,
        [this](const QString& version) { databaseVersion_ = version; });
    connect(session_, &DpiSession::activeEndpointChanged, this,
        [this](const ConnectionEndpoint&, const ObservedDatabaseIdentity& identity) {
            databaseIdentity_ = identity;
        });
    connect(session_, &DpiSession::batchReady, this, &DbaDocumentWidget::handleBatch);
    connect(session_, &DpiSession::failed, this, [this](const SessionError& error) {
        if (request_ == Request::ParameterUpdate) return;
        if (request_ == Request::CreateUser) return;
        if (request_ == Request::Parameters && !parameterCompatibilityAttempted_) return;
        if (request_ == Request::DatabaseVersion
                && !databaseVersionCompatibilityAttempted_) return;
        if (request_ == Request::DatabaseVersion
                || request_ == Request::DatabaseOptions) return;
        if (request_ == Request::LocksDetailed && !lockCompatibilityAttempted_) return;
        if (suppressNextErrorDialog_) {
            suppressNextErrorDialog_ = false;
            return;
        }
        QMessageBox::warning(this, QStringLiteral("DBA 查询失败"), error.message);
    });
    connect(session_, &DpiSession::executionFinished, this,
        [this](const QString&, const QString&, qint64, qint64, bool success,
               int, const QString& error, bool) {
            const Request completed = request_;
            if (completed == Request::ParameterUpdate) {
                request_ = Request::None;
                parameterUpdateNotice_ = success
                    ? QStringLiteral("参数 %1 修改请求成功；正在重新读取当前值和文件值，静态参数仍需重启。自带主备同步规则的参数需等待同步／重演后重新对比。凡未确认生效的项不得视为已完成。").arg(pendingParameterName_)
                    : QStringLiteral("参数 %1 修改未确认成功：%2。不会自动重试；断线时请先核对实际结果。").arg(pendingParameterName_, error);
                pendingParameterName_.clear();
                parametersState_->setText(parameterUpdateNotice_);
                comparisonState_->setText(QStringLiteral("当前环境发生参数修改请求；上次对比为历史结果，请重新对比。"));
                updateActions();
                parameterRefreshPending_ = success;
                refreshParametersAfterUpdate();
                return;
            }
            if (success && isManagementRequest(completed)) {
                commitManagementBatch(completed);
            }
            if (success && (completed == Request::LocksDetailed
                    || completed == Request::Locks)) {
                commitLockSnapshot();
            }
            const bool loadRoleDetails =
                success && completed == Request::Roles && !selectedRole().isEmpty();
            const bool loadRoleMembers =
                success && completed == Request::RolePrivileges
                && loadRoleMembersAfterPrivileges_;
            if (completed == Request::RolePrivileges) {
                loadRoleMembersAfterPrivileges_ = false;
            }
            if ((completed == Request::CancelSessionOperation
                    || completed == Request::CloseSession) && success) {
                closeCommitPending_ = true;
                QMetaObject::invokeMethod(session_, "commit", Qt::QueuedConnection);
                return;
            }
            if (completed == Request::CreateUser) {
                request_ = Request::None;
                const QString created = pendingCreatedUser_;
                pendingCreatedUser_.clear();
                if (success) {
                    QMessageBox::information(this, QStringLiteral("用户已创建"),
                        QStringLiteral("用户“%1”已创建，密码未写入预览或日志。")
                            .arg(created));
                    loadUserPage(0);
                    emit userEditRequested(created);
                } else {
                    QMessageBox::critical(this, QStringLiteral("创建用户失败"), error);
                }
                updateActions();
                return;
            }
            request_ = Request::None;
            if (completed == Request::Sessions && success) {
                commitSessionSnapshot();
            }
            if (completed == Request::VerifyClosedSession) {
                if (success) {
                    pendingSessionActionOutcome_ = verifiedSessionExists_
                            && !verifiedSessionIdentityValid_
                        ? QStringLiteral(
                            "终止请求已发送；复查返回的会话身份不完整，目标状态未知。")
                        : DbaDiagnostics::closeVerificationMessage(
                            verifyingSessionId_, verifyingSessionSeq_,
                            verifiedSessionExists_, verifiedSessionSeq_);
                    pendingSessionOutcomeForLocks_ = verifyingSessionFromLocks_;
                    const bool refreshLocksPage = verifyingSessionFromLocks_;
                    verifyingSessionId_ = 0;
                    verifyingSessionSeq_ = 0;
                    verifiedSessionSeq_ = 0;
                    verifiedSessionExists_ = false;
                    verifiedSessionIdentityValid_ = false;
                    verifyingSessionFromLocks_ = false;
                    QTimer::singleShot(0, this, refreshLocksPage
                        ? &DbaDocumentWidget::refreshLocks
                        : &DbaDocumentWidget::refreshSessions);
                } else {
                    showRequestFailure(completed, error);
                    verifyingSessionId_ = 0;
                    verifyingSessionSeq_ = 0;
                    verifiedSessionSeq_ = 0;
                    verifiedSessionExists_ = false;
                    verifiedSessionIdentityValid_ = false;
                    verifyingSessionFromLocks_ = false;
                    updateActions();
                }
                return;
            }
            if (completed == Request::ValidateSession && success) {
                if (sessionValidationMatched_) {
                    runValidatedSessionAction();
                } else {
                    pendingSessionAction_ = PendingSessionAction::None;
                    QLabel* stateLabel = pendingActionFromLocks_ ? lockState_ : sessionsState_;
                    stateLabel->setText(pendingActionFromLocks_
                        ? QStringLiteral("等待关系已变化，或目标会话已结束；未执行操作，正在刷新。")
                        : QStringLiteral("目标会话已结束或序列已变化；未执行操作，请刷新后重试。"));
                    const bool refreshLockPage = pendingActionFromLocks_;
                    pendingActionFromLocks_ = false;
                    if (refreshLockPage) refreshLocks(); else refreshSessions();
                }
                return;
            }
            if (!success) {
                if (completed == Request::Sessions) {
                    pendingSessionColumns_.clear();
                    pendingSessionRows_.clear();
                }
                if (completed == Request::LocksDetailed
                        && !lockCompatibilityAttempted_) {
                    lockCompatibilityAttempted_ = true;
                    suppressNextErrorDialog_ = true;
                    pendingLockColumns_.clear();
                    pendingLockRows_.clear();
                    lockState_->setText(QStringLiteral(
                        "锁对象详细字段不可用，正在回退基础等待关系…"));
                    QTimer::singleShot(0, this, [this]() {
                        execute(Request::Locks, DbaDiagnostics::lockWaitsSql());
                    });
                    return;
                }
                if (completed == Request::Parameters
                        && !parameterCompatibilityAttempted_) {
                    parameterCompatibilityAttempted_ = true;
                    parameterResult_.fail();
                    parametersState_->setText(QStringLiteral(
                        "完整参数字段不可用，正在使用兼容模式读取…"));
                    QTimer::singleShot(0, this, [this]() {
                        execute(Request::Parameters,
                            DbaManagement::parametersCompatibilitySql());
                    });
                    return;
                }
                if (completed == Request::DatabaseVersion
                        && !databaseVersionCompatibilityAttempted_) {
                    databaseVersionCompatibilityAttempted_ = true;
                    databaseVersionNotice_ = QStringLiteral(
                        "ID_CODE 解析不可用，已回退 V$INSTANCE 版本信息");
                    databaseVersionResult_.fail();
                    databaseOptionsState_->setText(QStringLiteral(
                        "ID_CODE 版本解析不可用，正在读取 V$INSTANCE 兼容信息…"));
                    QTimer::singleShot(0, this, [this]() {
                        execute(Request::DatabaseVersion,
                            DbaManagement::databaseVersionCompatibilitySql());
                    });
                    return;
                }
                if (DbaManagementAccumulator* accumulator =
                        managementAccumulator(completed)) {
                    accumulator->fail();
                }
                showRequestFailure(completed, error);
                if (completed == Request::ValidateSession
                        || completed == Request::CancelSessionOperation
                        || completed == Request::CloseSession) {
                    pendingSessionAction_ = PendingSessionAction::None;
                    pendingActionFromLocks_ = false;
                    const int interval = lockRefreshInterval_->currentData().toInt();
                    if (kind_ == DbaDocumentKind::Locks && interval > 0
                            && state_ == SessionState::Connected) {
                        locksTimer_->setInterval(interval);
                        locksTimer_->start();
                    }
                }
                if (completed == Request::SlowSql && !slowFallbackAttempted_) {
                    slowFallbackAttempted_ = true;
                    suppressNextErrorDialog_ = true;
                    QTimer::singleShot(0, this, [this]() {
                        slowSource_->setText(QStringLiteral(
                            "慢 SQL 数据源：V$SQL_HISTORY 不可用，已回退活动 V$SESSIONS。"));
                        execute(Request::SlowSqlFallback, DbaDiagnostics::activeSqlFallback());
                    });
                }
            }
            updateActions();
            if (completed == Request::Parameters) {
                QTimer::singleShot(0, this,
                    [this]() { refreshDatabaseVersion(); });
            } else if (completed == Request::DatabaseVersion) {
                QTimer::singleShot(0, this,
                    [this]() { refreshDatabaseOptions(); });
            } else if (loadRoleDetails) {
                QTimer::singleShot(0, this,
                    [this]() { loadSelectedRoleDetails(); });
            } else if (loadRoleMembers) {
                QTimer::singleShot(0, this,
                    [this]() { loadRoleMemberPage(0); });
            }
        });
    connect(session_, &DpiSession::commitCompleted, this, [this](bool success) {
        if (!closeCommitPending_) return;
        closeCommitPending_ = false;
        request_ = Request::None;
        if (success) {
            const bool closing = pendingSessionAction_ == PendingSessionAction::CloseSession;
            sessionsState_->setText(closing
                ? QStringLiteral("终止请求已发送；正在等待数据库释放会话…")
                : QStringLiteral("取消操作请求已发送；正在刷新会话状态…"));
            if (pendingActionFromLocks_) {
                lockState_->setText(closing
                    ? QStringLiteral("Kill 请求已发送；正在等待数据库释放会话…")
                    : QStringLiteral("取消操作请求已发送；正在刷新阻塞关系…"));
            }
            const bool refreshLockPage = pendingActionFromLocks_;
            const qint64 completedSessionId = pendingSessionId_;
            const qint64 completedSessionSeq = pendingSessionSeq_;
            pendingSessionAction_ = PendingSessionAction::None;
            pendingActionFromLocks_ = false;
            pendingSessionId_ = 0;
            pendingSessionSeq_ = 0;
            if (closing) {
                verifyingSessionId_ = completedSessionId;
                verifyingSessionSeq_ = completedSessionSeq;
                verifyingSessionFromLocks_ = refreshLockPage;
                verifiedSessionExists_ = false;
                verifiedSessionSeq_ = 0;
                verifiedSessionIdentityValid_ = false;
                QTimer::singleShot(400, this, [this]() {
                    if (state_ != SessionState::Connected || request_ != Request::None) {
                        QLabel* stateLabel = verifyingSessionFromLocks_
                            ? lockState_ : sessionsState_;
                        stateLabel->setText(QStringLiteral(
                            "终止请求已发送，但当前连接状态已变化，无法确认目标是否已释放。"));
                        verifyingSessionId_ = 0;
                        verifyingSessionSeq_ = 0;
                        verifiedSessionSeq_ = 0;
                        verifiedSessionExists_ = false;
                        verifiedSessionIdentityValid_ = false;
                        verifyingSessionFromLocks_ = false;
                        updateActions();
                        return;
                    }
                    execute(Request::VerifyClosedSession,
                        DbaDiagnostics::verifyClosedSessionSql(verifyingSessionId_));
                });
            } else {
                QTimer::singleShot(400, this, refreshLockPage
                    ? &DbaDocumentWidget::refreshLocks
                    : &DbaDocumentWidget::refreshSessions);
            }
        } else {
            QLabel* stateLabel = pendingActionFromLocks_ ? lockState_ : sessionsState_;
            stateLabel->setText(QStringLiteral(
                "会话操作调用已返回，但提交失败；目标状态未知，请手工刷新确认。"));
            pendingSessionAction_ = PendingSessionAction::None;
            pendingActionFromLocks_ = false;
            pendingSessionId_ = 0;
            pendingSessionSeq_ = 0;
        }
        updateActions();
    });
    sessionThread_->start();

    comparisonThread_ = new QThread(this);
    comparisonSession_ = new DpiSession();
    comparisonSession_->moveToThread(comparisonThread_);
    connect(comparisonThread_, &QThread::finished,
        comparisonSession_, &QObject::deleteLater);
    connect(comparisonSession_, &DpiSession::activeEndpointChanged, this,
        [this](const ConnectionEndpoint&, const ObservedDatabaseIdentity& identity) { comparisonIdentity_ = identity; });
    connect(comparisonSession_, &DpiSession::connected, this,
        [this](const ConnectionProfile& target) {
            comparisonProfileValue_ = target;
            if (comparisonCancelRequested_) {
                comparisonBusy_ = false;
                comparisonCancelRequested_ = false;
                comparisonState_->setText(QStringLiteral(
                    "环境对比已取消，已保留上次成功结果。"));
                updateComparisonAction(false);
                updateActions();
                QMetaObject::invokeMethod(comparisonSession_, "disconnectFromDatabase",
                    Qt::QueuedConnection);
                return;
            }
            comparisonResult_.begin(0);
            comparisonState_->setText(QStringLiteral(
                "已连接目标环境 %1，正在读取完整参数…").arg(target.name));
            QMetaObject::invokeMethod(comparisonSession_, "execute", Qt::QueuedConnection,
                Q_ARG(QString, DbaManagement::parametersSql()));
        });
    connect(comparisonSession_, &DpiSession::batchReady, this,
        [this](const QueryBatch& batch) {
            if (!comparisonResult_.append(batch.columns, batch.rows)) {
                comparisonResult_.fail();
                comparisonState_->setText(QStringLiteral(
                    "目标环境返回了不兼容的结果结构。请检查数据库版本后重试。"));
                comparisonSession_->requestCancel();
                return;
            }
            comparisonState_->setText(QStringLiteral("正在读取目标环境：已接收 %1 项参数…")
                .arg(comparisonResult_.pendingRowCount()));
            if (batch.hasMore) {
                QMetaObject::invokeMethod(comparisonSession_, "fetchMore", Qt::QueuedConnection);
            }
        });
    connect(comparisonSession_, &DpiSession::failed, this,
        [this](const SessionError& error) {
            if (!comparisonResult_.active()) {
                comparisonBusy_ = false;
                comparisonState_->setText(comparisonCancelRequested_
                    ? QStringLiteral("环境对比已取消，已保留上次成功结果。")
                    : QStringLiteral(
                        "环境对比失败：%1。请检查目标地址、网络和保存的凭据后重试。")
                        .arg(error.message));
                comparisonCancelRequested_ = false;
                updateComparisonAction(false);
                updateActions();
            }
        });
    connect(comparisonSession_, &DpiSession::executionFinished, this,
        [this](const QString&, const QString&, qint64 elapsedMs, qint64, bool success,
               int, const QString& error, bool) {
            if (!success && !comparisonCancelRequested_
                    && !comparisonCompatibilityAttempted_) {
                comparisonCompatibilityAttempted_ = true;
                comparisonResult_.begin(0);
                comparisonState_->setText(QStringLiteral(
                    "目标版本不支持完整参数视图，正在使用兼容查询重试…"));
                QMetaObject::invokeMethod(comparisonSession_, "execute", Qt::QueuedConnection,
                    Q_ARG(QString, DbaManagement::parametersCompatibilitySql()));
                return;
            }
            if (comparisonCancelRequested_) {
                comparisonResult_.fail();
                comparisonState_->setText(QStringLiteral(
                    "环境对比已取消，已保留上次成功结果。"));
            } else if (!success) {
                comparisonResult_.fail();
                comparisonState_->setText(QStringLiteral(
                    "环境对比失败：%1。已保留上次成功结果，请检查目标数据库权限和版本后重试。")
                    .arg(error));
            } else {
                comparisonSnapshot_ = DatabaseEnvironment::snapshot(comparisonResult_.complete(), parameterMetadata_);
                renderEnvironmentComparison();
                comparisonState_->setText(comparisonState_->text() + QStringLiteral("；耗时 %1 ms。").arg(elapsedMs));
            }
            comparisonBusy_ = false;
            comparisonCancelRequested_ = false;
            updateComparisonAction(false);
            updateActions();
            QMetaObject::invokeMethod(comparisonSession_, "disconnectFromDatabase",
                Qt::QueuedConnection);
        });
    comparisonThread_->start();

    connect(refreshSessions_, &QPushButton::clicked, this, &DbaDocumentWidget::refreshSessions);
    connect(loadSessionSql_, &QPushButton::clicked,
        this, &DbaDocumentWidget::loadSelectedSessionSql);
    connect(copySessionSummary_, &QAction::triggered, this, [this]() {
        const qint64 sessionId = selectedSessionNumber(QStringLiteral("SESS_ID"));
        if (sessionId <= 0) return;
        QApplication::clipboard()->setText(QStringLiteral(
            "%1\n\n%2\n\n当前／最近 SQL：\n%3")
            .arg(selectedSessionContext(sessionId), sessionDetails_->text(),
                selectedSessionValue(QStringLiteral("SQL_TEXT")).trimmed().isEmpty()
                    ? QStringLiteral("—")
                    : selectedSessionValue(QStringLiteral("SQL_TEXT"))));
        sessionsState_->setText(QStringLiteral("会话 %1 的诊断摘要已复制。").arg(sessionId));
    });
    connect(cancelSessionOperation_, &QAction::triggered,
        this, &DbaDocumentWidget::cancelSelectedSessionOperation);
    connect(closeSession_, &QAction::triggered,
        this, &DbaDocumentWidget::closeSelectedSession);
    connect(openSessionLocks_, &QPushButton::clicked, this, [this]() {
        pages_->setCurrentIndex(dbaDocumentPage(DbaDocumentKind::Locks));
        if (state_ == SessionState::Connected && !isBusy()) refreshLocks();
    });
    connect(copySessionSql_, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(sessionSql_->toPlainText());
        sessionsState_->setText(QStringLiteral("完整 SQL 已复制。"));
    });
    connect(openSessionSql_, &QPushButton::clicked, this, [this]() {
        const QString sql = sessionSql_->toPlainText().trimmed();
        if (!sql.isEmpty()) emit openSqlRequested(sql, connectionProfileId());
    });
    connect(refreshLocks_, &QPushButton::clicked, this, &DbaDocumentWidget::refreshLocks);
    connect(fitLockGraph_, &QPushButton::clicked, lockGraph_,
        &LockWaitGraphWidget::fitContents);
    connect(selectedLockChainOnly_, &QCheckBox::toggled,
        this, [this](bool) { applyLockFilter(); });
    connect(locks_, &QTableWidget::itemSelectionChanged,
        this, &DbaDocumentWidget::updateLockDetails);
    lockGraph_->relationSelected = [this](int recordIndex) {
        applyLockSelection(recordIndex);
    };
    connect(copyLockDetails_, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(lockDetails_->toPlainText());
        lockState_->setText(QStringLiteral("诊断详情已复制。"));
    });
    connect(openBlockerSql_, &QPushButton::clicked, this, [this]() {
        if (const LockWaitRecord* record = selectedLockRecord();
                record != nullptr && !record->blockerSql.trimmed().isEmpty()) {
            emit openSqlRequested(record->blockerSql, connectionProfileId());
        }
    });
    connect(openWaiterSql_, &QPushButton::clicked, this, [this]() {
        if (const LockWaitRecord* record = selectedLockRecord();
                record != nullptr && !record->waiterSql.trimmed().isEmpty()) {
            emit openSqlRequested(record->waiterSql, connectionProfileId());
        }
    });
    connect(cancelBlockerOperation_, &QPushButton::clicked, this, [this]() {
        beginLockSessionAction(PendingSessionAction::CancelOperation, true);
    });
    connect(killBlocker_, &QPushButton::clicked, this, [this]() {
        beginLockSessionAction(PendingSessionAction::CloseSession, true);
    });
    connect(killWaiter_, &QPushButton::clicked, this, [this]() {
        beginLockSessionAction(PendingSessionAction::CloseSession, false);
    });
    connect(refreshPerformance_, &QPushButton::clicked, this, &DbaDocumentWidget::refreshCounts);
    connect(refreshSlowSql_, &QPushButton::clicked, this, &DbaDocumentWidget::refreshSlowSql);
    connect(refreshTablespaces_, &QPushButton::clicked,
        this, &DbaDocumentWidget::refreshTablespaces);
    connect(refreshDataFiles_, &QPushButton::clicked,
        this, &DbaDocumentWidget::refreshDataFiles);
    if (kind_ == DbaDocumentKind::Roles) {
        connect(refreshPrivileges_, &QPushButton::clicked,
            this, &DbaDocumentWidget::loadSelectedRoleDetails);
        connect(rolesPrevious_, &QPushButton::clicked, this,
            [this]() { loadRolePage(rolePage_ - 1); });
        connect(rolesNext_, &QPushButton::clicked, this,
            [this]() { loadRolePage(rolePage_ + 1); });
        connect(rolePrivilegesPrevious_, &QPushButton::clicked, this,
            [this]() { loadRolePrivilegePage(rolePrivilegePage_ - 1); });
        connect(rolePrivilegesNext_, &QPushButton::clicked, this,
            [this]() { loadRolePrivilegePage(rolePrivilegePage_ + 1); });
        connect(roleMembersPrevious_, &QPushButton::clicked, this,
            [this]() { loadRoleMemberPage(roleMemberPage_ - 1); });
        connect(roleMembersNext_, &QPushButton::clicked, this,
            [this]() { loadRoleMemberPage(roleMemberPage_ + 1); });
        connect(rolesPageSize_, &QComboBox::currentIndexChanged, this,
            [this](int) { loadRolePage(0); });
        connect(rolePrivilegesPageSize_, &QComboBox::currentIndexChanged, this,
            [this](int) { loadRolePrivilegePage(0); });
        connect(roleMembersPageSize_, &QComboBox::currentIndexChanged, this,
            [this](int) { loadRoleMemberPage(0); });
    } else {
        connect(refreshUsers_, &QPushButton::clicked,
            this, &DbaDocumentWidget::refreshUsers);
        connect(createUser_, &QPushButton::clicked,
            this, &DbaDocumentWidget::createUser);
        connect(rolesPrevious_, &QPushButton::clicked, this,
            [this]() { loadUserPage(userPage_ - 1); });
        connect(rolesNext_, &QPushButton::clicked, this,
            [this]() { loadUserPage(userPage_ + 1); });
        connect(rolesPageSize_, &QComboBox::currentIndexChanged, this,
            [this](int) { loadUserPage(0); });
    }
    connect(refreshJobs_, &QPushButton::clicked, this, &DbaDocumentWidget::refreshJobs);
    connect(refreshJobSchedules_, &QPushButton::clicked,
        this, &DbaDocumentWidget::refreshJobSchedules);
    connect(refreshJobHistory_, &QPushButton::clicked,
        this, &DbaDocumentWidget::refreshJobHistory);
    connect(refreshParameters_, &QPushButton::clicked,
        this, &DbaDocumentWidget::refreshParameters);
    connect(compareEnvironment_, &QPushButton::clicked,
        this, &DbaDocumentWidget::startEnvironmentComparison);
    connect(refreshComparisonProfiles_, &QPushButton::clicked,
        this, &DbaDocumentWidget::refreshComparisonProfiles);
    connect(comparisonProfile_, &QComboBox::currentIndexChanged, this,
        [this](int) { updateComparisonAction(); });
    connect(cancelRequest_, &QPushButton::clicked, this, [this]() {
        if (kind_ == DbaDocumentKind::Parameters && request_ == Request::None && !comparisonBusy_) {
            refreshParameters();
        } else {
            cancelRequest();
        }
    });
    connect(tablespaceFilter_, &QLineEdit::textChanged, this, [this](const QString& text) {
        filterTable(tablespaces_, text);
        filterTable(dataFiles_, text);
    });
    userSearchTimer_ = new QTimer(this);
    userSearchTimer_->setSingleShot(true);
    userSearchTimer_->setInterval(300);
    connect(userSearchTimer_, &QTimer::timeout, this, [this]() {
        if (kind_ == DbaDocumentKind::Users && state_ == SessionState::Connected
                && !isBusy()) loadUserPage(0);
    });
    connect(usersFilter_, &QLineEdit::textChanged, this,
        [this](const QString& text) {
            if (kind_ == DbaDocumentKind::Users) userSearchTimer_->start();
            else filterTable(users_, text);
        });
    connect(users_, &QTableWidget::itemDoubleClicked, this,
        [this](QTableWidgetItem*) {
            if (kind_ == DbaDocumentKind::Roles) emit roleEditRequested(selectedRole());
            else if (kind_ == DbaDocumentKind::Users) emit userEditRequested(selectedUser());
        });
    connect(privilegesFilter_, &QLineEdit::textChanged, this,
        [this](const QString& text) { filterTable(privileges_, text); });
    connect(roleMembersFilter_, &QLineEdit::textChanged, this,
        [this](const QString& text) { filterTable(roleMembers_, text); });
    connect(jobsFilter_, &QLineEdit::textChanged, this, [this](const QString& text) {
        filterTable(jobs_, text);
        filterTable(jobSchedules_, text);
        filterTable(jobHistory_, text);
    });
    connect(parametersFilter_, &QLineEdit::textChanged, this,
        [this](const QString&) { filterEnvironmentTables(); });
    connect(databaseOptionsFilter_, &QLineEdit::textChanged, this,
        [this](const QString& text) { filterTable(databaseOptions_, text); });
    connect(commonParametersFilter_, &QLineEdit::textChanged, this,
        [this](const QString&) { filterEnvironmentTables(); });
    connect(commonParametersPresetFilter_, &QComboBox::currentIndexChanged, this,
        [this](int) { filterEnvironmentTables(); });
    connect(nonDefaultOnly_, &QCheckBox::toggled, this,
        [this](bool) { filterEnvironmentTables(); });
    connect(pendingOnly_, &QCheckBox::toggled, this,
        [this](bool) { filterEnvironmentTables(); });
    connect(users_, &QTableWidget::itemSelectionChanged, this, [this]() {
        if (kind_ == DbaDocumentKind::Roles) {
            const QString role = selectedRole();
            privileges_->clear();
            privileges_->setRowCount(0);
            privileges_->setColumnCount(0);
            roleMembers_->clear();
            roleMembers_->setRowCount(0);
            roleMembers_->setColumnCount(0);
            privilegesFilter_->clear();
            roleMembersFilter_->clear();
            rolePrivilegePage_ = 0;
            roleMemberPage_ = 0;
            rolePrivilegeHasNext_ = false;
            roleMemberHasNext_ = false;
            rolePrivilegesPageLabel_->setText(QStringLiteral("第 1 页"));
            roleMembersPageLabel_->setText(QStringLiteral("第 1 页"));
            usersState_->setText(role.isEmpty()
                ? QStringLiteral("请选择角色。")
                : QStringLiteral("已选择角色 %1；正在加载权限和用户。").arg(role));
            const bool shouldLoad = !role.isEmpty() && !isBusy()
                && state_ == SessionState::Connected;
            updateActions();
            if (shouldLoad) {
                QTimer::singleShot(0, this,
                    [this]() { loadSelectedRoleDetails(); });
            }
            return;
        }
    });
    connect(sessions_, &QTableWidget::itemSelectionChanged, this, [this]() {
        updateSessionDetails();
        updateActions();
    });
    connect(sessions_, &QTableWidget::itemDoubleClicked, this,
        [this](QTableWidgetItem*) {
            if (!selectedSessionValue(QStringLiteral("SQL_TEXT")).trimmed().isEmpty()) {
                loadSelectedSessionSql();
                sessionDetailTabs_->setCurrentIndex(1);
            }
        });
    connect(sessionPresetFilter_, &QComboBox::currentIndexChanged,
        this, [this](int) { applySessionFilters(); });
    connect(sessionSummary_, &QLabel::linkActivated, this, [this](const QString& preset) {
        const int index = sessionPresetFilter_->findData(preset);
        if (index >= 0) sessionPresetFilter_->setCurrentIndex(index);
    });
    connect(sessionUserFilter_, &QComboBox::currentIndexChanged,
        this, [this](int) { applySessionFilters(); });
    connect(sessionStateFilter_, &QComboBox::currentIndexChanged,
        this, [this](int) { applySessionFilters(); });
    connect(sessionIpFilter_, &QLineEdit::textChanged,
        this, [this](const QString&) { applySessionFilters(); });
    connect(sessionAppFilter_, &QLineEdit::textChanged,
        this, [this](const QString&) { applySessionFilters(); });
    connect(userSessionsOnly_, &QCheckBox::toggled,
        this, [this](bool) { applySessionFilters(); });
    connect(currentUserOnly_, &QCheckBox::toggled,
        this, [this](bool) { applySessionFilters(); });
    connect(hideClientSessions_, &QCheckBox::toggled,
        this, [this](bool) { applySessionFilters(); });
    updateActions();
}

DbaDocumentWidget::~DbaDocumentWidget() {
    sampleTimer_->stop();
    sessionsTimer_->stop();
    locksTimer_->stop();
    if (session_ != nullptr && sessionThread_->isRunning()) {
        session_->requestCancel();
        QMetaObject::invokeMethod(session_, [session = session_, thread = sessionThread_]() {
            session->disconnectFromDatabase();
            thread->quit();
        }, Qt::QueuedConnection);
        if (!sessionThread_->wait(5'000)) {
            sessionThread_->terminate();
            sessionThread_->wait(1'000);
        }
    }
    if (comparisonSession_ != nullptr && comparisonThread_->isRunning()) {
        comparisonSession_->requestCancel();
        QMetaObject::invokeMethod(comparisonSession_,
            [session = comparisonSession_, thread = comparisonThread_]() {
                session->disconnectFromDatabase();
                thread->quit();
            }, Qt::QueuedConnection);
        if (!comparisonThread_->wait(5'000)) {
            comparisonThread_->terminate();
            comparisonThread_->wait(1'000);
        }
    }
}

QString DbaDocumentWidget::displayName() const { return title_; }
DbaDocumentKind DbaDocumentWidget::kind() const { return kind_; }
QToolButton* DbaDocumentWidget::connectionSelector() const { return connectionSelector_; }
QString DbaDocumentWidget::connectionProfileId() const {
    return pendingProfile_.id.isEmpty() ? profile_.id : pendingProfile_.id;
}
QString DbaDocumentWidget::connectionDisplay() const {
    return profile_.name.isEmpty() ? QStringLiteral("未连接") : profile_.name;
}
bool DbaDocumentWidget::isConnected() const { return state_ == SessionState::Connected; }
SessionState DbaDocumentWidget::sessionState() const { return state_; }
bool DbaDocumentWidget::isBusy() const {
    return request_ != Request::None || comparisonBusy_ || state_ == SessionState::Connecting
        || state_ == SessionState::Executing || state_ == SessionState::Cancelling
        || verifyingSessionId_ > 0;
}

void DbaDocumentWidget::setConnectionPending(ConnectionProfile profile, QString detail) {
    pendingProfile_ = std::move(profile);
    const QString active = connectionDisplay();
    context_->setText(QStringLiteral("连接：%1｜目标：%2｜状态：%3｜独立 Session")
        .arg(active, pendingProfile_.name, detail));
    updateActions();
    emit statusChanged(active, detail);
}

void DbaDocumentWidget::connectDatabase(ConnectionProfile profile, QByteArray password) {
    pendingProfile_ = profile;
    QMetaObject::invokeMethod(session_, "connectToDatabase", Qt::QueuedConnection,
        Q_ARG(ConnectionProfile, profile), Q_ARG(QByteArray, password));
}

void DbaDocumentWidget::disconnectDatabase() {
    if (isBusy()) return;
    sampleTimer_->stop();
    sessionsTimer_->stop();
    locksTimer_->stop();
    QMetaObject::invokeMethod(session_, "disconnectFromDatabase", Qt::QueuedConnection);
}

void DbaDocumentWidget::setLockIncidentContext(QString sql, QString error) {
    lockIncidentSql_ = std::move(sql);
    lockIncidentError_ = std::move(error);
    const SqlOperationInfo operation = DbaDiagnostics::analyzeSqlOperation(
        lockIncidentSql_, profile_.user);
    lockState_->setText(QStringLiteral(
        "来自执行错误的锁诊断：%1；请求操作 %2，目标 %3。正在采集当前等待快照。")
        .arg(lockIncidentError_.simplified(), operation.operation,
            operation.objectName.isEmpty() ? QStringLiteral("未知")
                                           : operation.objectName));
    if (state_ == SessionState::Connected && !isBusy()) refreshLocks();
}

void DbaDocumentWidget::execute(Request request, const QString& sql) {
    if (state_ != SessionState::Connected || request_ != Request::None) return;
    if (request == Request::ParameterUpdate && (profile_.readOnly || comparisonBusy_)) return;
    if (isManagementRequest(request) && !DbaManagement::isReadOnlyQuery(sql)) {
        QMessageBox::critical(this, QStringLiteral("F8 只读保护"),
            QStringLiteral("拒绝执行非只读 DBA 管理请求。"));
        return;
    }
    request_ = request;
    firstBatch_ = true;
    if (isManagementRequest(request)) {
        int displayLimit = DbaManagement::displayLimit(managementQuery(request));
        if (request == Request::Roles || request == Request::Users) {
            displayLimit = rolesPageSize_->currentData().toInt();
        } else if (request == Request::RolePrivileges) {
            displayLimit = rolePrivilegesPageSize_->currentData().toInt();
        } else if (request == Request::RoleMembers) {
            displayLimit = roleMembersPageSize_->currentData().toInt();
        }
        managementAccumulator(request)->begin(displayLimit);
    }
    if (request == Request::LocksDetailed || request == Request::Locks) {
        pendingLockColumns_.clear();
        pendingLockRows_.clear();
    }
    updateActions();
    QMetaObject::invokeMethod(session_, "execute", Qt::QueuedConnection, Q_ARG(QString, sql));
}

void DbaDocumentWidget::refreshPrimaryView() {
    switch (kind_) {
    case DbaDocumentKind::Sessions: refreshSessions(); break;
    case DbaDocumentKind::Locks: refreshLocks(); break;
    case DbaDocumentKind::Performance: refreshCounts(); break;
    case DbaDocumentKind::Tablespaces: refreshTablespaces(); break;
    case DbaDocumentKind::Users: refreshUsers(); break;
    case DbaDocumentKind::Roles:
        rolePage_ = 0;
        refreshRoles();
        break;
    case DbaDocumentKind::Jobs: refreshJobs(); break;
    case DbaDocumentKind::JobSchedules: refreshJobSchedules(); break;
    case DbaDocumentKind::JobHistory: refreshJobHistory(); break;
    case DbaDocumentKind::Parameters: refreshParameters(); break;
    }
}

void DbaDocumentWidget::refreshSessions() {
    if (isBusy()) return;
    sessionSelectionKey_ = selectedSessionKey();
    sessionScrollValue_ = sessions_->verticalScrollBar()->value();
    sessionSortColumn_ = sessions_->rowCount() > 0 && sessions_->isSortingEnabled()
        ? sessions_->horizontalHeader()->sortIndicatorSection() : -1;
    sessionSortOrder_ = sessions_->horizontalHeader()->sortIndicatorOrder();
    compareSessionSnapshot_ = hasSessionSnapshot_;
    sessionSnapshotIdentityAvailable_ = true;
    pendingSessionColumns_.clear();
    pendingSessionRows_.clear();
    pendingSessionSnapshot_.clear();
    pendingSessionKeys_.clear();
    sessionMissingFields_.clear();
    sessionNewCount_ = 0;
    sessionChangedCount_ = 0;
    sessionEndedCount_ = 0;
    execute(Request::Sessions, DbaDiagnostics::sessionsSql());
}
void DbaDocumentWidget::refreshLocks() {
    if (isBusy() || state_ != SessionState::Connected) return;
    const LockWaitRecord* selected = selectedLockRecord();
    lockSelectionKey_ = selected == nullptr ? QString() : QStringLiteral("%1:%2:%3")
        .arg(selected->blockerSessionId).arg(selected->waiterSessionId).arg(selected->tableId);
    lockCompatibilityAttempted_ = false;
    lockState_->setText(lockRecords_.isEmpty()
        ? QStringLiteral("正在采集锁等待、会话和对象信息…")
        : QStringLiteral("正在刷新；完成前保留上次快照。"));
    if (lockRecords_.isEmpty()) {
        lockEmptyTitle_->setText(QStringLiteral("正在查询锁等待"));
        lockEmptyDetail_->setText(QStringLiteral("正在采集当前等待关系；结果返回后更新。"));
    }
    execute(Request::LocksDetailed, DbaDiagnostics::detailedLockWaitsSql());
}

void DbaDocumentWidget::commitLockSnapshot() {
    const bool compatibilityMode = !pendingLockColumns_.contains(
        QStringLiteral("BLOCKER_SESS_SEQ"));
    QVector<LockWaitRecord> records = DbaDiagnostics::lockWaitRecords(
        pendingLockColumns_, pendingLockRows_, profile_.user);
    const QVector<LockWaitEdge> edges = DbaDiagnostics::lockEdges(records);
    const LockTopologySummary summary = DbaDiagnostics::topologySummary(records);
    QSet<qint64> waiterIds;
    for (const LockWaitEdge& edge : edges) waiterIds.insert(edge.waiterSessionId);

    const bool sorting = locks_->isSortingEnabled();
    locks_->setSortingEnabled(false);
    locks_->clear();
    locks_->setColumnCount(11);
    locks_->setHorizontalHeaderLabels({QStringLiteral("风险"), QStringLiteral("阻塞会话"),
        QStringLiteral("阻塞用户"), QStringLiteral("等待会话"), QStringLiteral("等待用户"),
        QStringLiteral("冲突／候选对象"), QStringLiteral("等待操作"),
        QStringLiteral("阻塞当前／最近操作"), QStringLiteral("锁类型"),
        QStringLiteral("锁模式"), QStringLiteral("可信度")});
    locks_->setRowCount(records.size());
    for (int row = 0; row < records.size(); ++row) {
        const LockWaitRecord& record = records.at(row);
        QString object;
        if (!record.objectSchema.isEmpty() && !record.objectName.isEmpty()) {
            object = record.objectSchema + QLatin1Char('.') + record.objectName;
        } else if (!record.objectName.isEmpty()) {
            object = record.objectName;
        } else if (record.tableId > 0) {
            object = QStringLiteral("TABLE_ID %1").arg(record.tableId);
        } else {
            object = QStringLiteral("未知");
        }
        const bool root = record.blockerSessionId > 0
            && !waiterIds.contains(record.blockerSessionId);
        const QString risk = summary.cycle ? QStringLiteral("循环")
            : root ? QStringLiteral("根阻塞") : QStringLiteral("链中");
        const QStringList values = {risk, QString::number(record.blockerSessionId),
            record.blockerUser, QString::number(record.waiterSessionId), record.waiterUser,
            object, record.waiterOperation.operation, record.blockerOperation.operation,
            record.lockType, record.lockMode,
            DbaDiagnostics::confidenceLabel(record.objectConfidence)};
        for (int column = 0; column < values.size(); ++column) {
            auto* item = new QTableWidgetItem(values.at(column));
            item->setData(Qt::UserRole, row);
            if (column == 0 && (summary.cycle || root)) {
                item->setForeground(Theme::semanticColor(summary.cycle
                    ? Theme::SemanticColor::Danger : Theme::SemanticColor::Warning,
                    palette()));
            }
            locks_->setItem(row, column, item);
        }
    }
    locks_->horizontalHeader()->setStretchLastSection(false);
    locks_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    locks_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    locks_->setSortingEnabled(sorting);

    lockRecords_ = std::move(records);
    lockEdges_ = edges;
    lockGraph_->setRecords(lockRecords_);
    lockSummary_->setText(QStringLiteral(
        "关系 %1｜会话 %2｜根阻塞者 %3｜最大深度 %4｜%5")
        .arg(summary.relationCount).arg(summary.sessionCount)
        .arg(summary.rootBlockerCount).arg(summary.maximumDepth)
        .arg(summary.cycle ? QStringLiteral("检测到循环") : QStringLiteral("未检测到循环")));
    lockUpdatedAt_->setText(QStringLiteral("更新于 %1")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    lockContent_->setCurrentIndex(lockRecords_.isEmpty() ? 0 : 1);
    if (lockRecords_.isEmpty()) {
        lockEmptyTitle_->setText(QStringLiteral("本次快照未发现锁等待"));
        lockEmptyDetail_->setText(QStringLiteral("本次快照未返回事务等待关系；可手动刷新或设置自动刷新。"));
    }
    if (lockRecords_.isEmpty()) {
        lockGraph_->hide();
        locks_->hide();
        lockSummary_->hide();
        selectedLockChainOnly_->setEnabled(false);
        lockState_->setText(QStringLiteral("当前快照未发现事务等待关系。"));
    } else if (summary.cycle) {
        lockGraph_->show();
        locks_->show();
        lockSummary_->show();
        selectedLockChainOnly_->setEnabled(true);
        lockState_->setText(QStringLiteral(
            "检测到循环等待。请先核对对象、SQL 和事务来源，再处理根因会话。"));
    } else if (compatibilityMode) {
        lockGraph_->show();
        locks_->show();
        lockSummary_->show();
        selectedLockChainOnly_->setEnabled(true);
        lockState_->setText(QStringLiteral(
            "已加载基础等待关系；当前达梦版本未返回对象和会话序列，危险操作已禁用。"));
    } else {
        lockGraph_->show();
        locks_->show();
        lockSummary_->show();
        selectedLockChainOnly_->setEnabled(true);
        lockState_->setText(QStringLiteral(
            "已生成诊断快照。对象来自阻塞事务持锁记录；“候选”不等同于唯一根因。"));
    }
    if (!lockIncidentSql_.trimmed().isEmpty()) {
        const SqlOperationInfo incident = DbaDiagnostics::analyzeSqlOperation(
            lockIncidentSql_, profile_.user);
        lockState_->setText(QStringLiteral("%1 请求操作：%2；目标：%3。")
            .arg(lockState_->text(), incident.operation,
                incident.objectName.isEmpty() ? QStringLiteral("未知")
                                              : incident.objectName));
    }

    int selectedIndex = -1;
    for (int index = 0; index < lockRecords_.size(); ++index) {
        const LockWaitRecord& record = lockRecords_.at(index);
        const QString key = QStringLiteral("%1:%2:%3")
            .arg(record.blockerSessionId).arg(record.waiterSessionId).arg(record.tableId);
        if (key == lockSelectionKey_) {
            selectedIndex = index;
            break;
        }
    }
    if (selectedIndex < 0 && !lockRecords_.isEmpty()) selectedIndex = 0;
    if (selectedIndex >= 0) applyLockSelection(selectedIndex);
    else updateLockDetails();
    pendingLockColumns_.clear();
    pendingLockRows_.clear();
    const int interval = lockRefreshInterval_->currentData().toInt();
    if (kind_ == DbaDocumentKind::Locks && interval > 0
            && state_ == SessionState::Connected) {
        locksTimer_->setInterval(interval);
        locksTimer_->start();
    }
    applyLockFilter();
    applyPendingSessionActionOutcome(true);
    updateActions();
}

const LockWaitRecord* DbaDocumentWidget::selectedLockRecord() const {
    const int row = locks_ == nullptr ? -1 : locks_->currentRow();
    if (row < 0 || locks_->item(row, 0) == nullptr) return nullptr;
    const int recordIndex = locks_->item(row, 0)->data(Qt::UserRole).toInt();
    return recordIndex >= 0 && recordIndex < lockRecords_.size()
        ? &lockRecords_.at(recordIndex) : nullptr;
}

void DbaDocumentWidget::applyLockSelection(int recordIndex) {
    if (recordIndex < 0 || recordIndex >= lockRecords_.size()) return;
    for (int row = 0; row < locks_->rowCount(); ++row) {
        const QTableWidgetItem* item = locks_->item(row, 0);
        if (item != nullptr && item->data(Qt::UserRole).toInt() == recordIndex) {
            locks_->selectRow(row);
            locks_->scrollToItem(item);
            break;
        }
    }
}

void DbaDocumentWidget::applyLockFilter() {
    const LockWaitRecord* selected = selectedLockRecord();
    const bool filter = selectedLockChainOnly_->isChecked() && selected != nullptr;
    QSet<qint64> chain;
    if (filter) {
        chain.insert(selected->blockerSessionId);
        chain.insert(selected->waiterSessionId);
        bool changed = true;
        while (changed) {
            changed = false;
            for (const LockWaitRecord& record : std::as_const(lockRecords_)) {
                if (chain.contains(record.blockerSessionId)
                        || chain.contains(record.waiterSessionId)) {
                    const int before = chain.size();
                    chain.insert(record.blockerSessionId);
                    chain.insert(record.waiterSessionId);
                    changed = changed || chain.size() != before;
                }
            }
        }
    }
    for (int row = 0; row < locks_->rowCount(); ++row) {
        const int index = locks_->item(row, 0)->data(Qt::UserRole).toInt();
        const LockWaitRecord& record = lockRecords_.at(index);
        locks_->setRowHidden(row, filter
            && !chain.contains(record.blockerSessionId)
            && !chain.contains(record.waiterSessionId));
    }
}

void DbaDocumentWidget::updateLockDetails() {
    const LockWaitRecord* record = selectedLockRecord();
    if (record == nullptr) {
        lockDetails_->clear();
        lockDetails_->parentWidget()->hide();
        updateActions();
        return;
    }
    lockDetails_->parentWidget()->show();
    QString object = QStringLiteral("未知");
    if (!record->objectName.isEmpty()) {
        object = record->objectSchema.isEmpty() ? record->objectName
            : record->objectSchema + QLatin1Char('.') + record->objectName;
    } else if (record->tableId > 0) {
        object = QStringLiteral("TABLE_ID %1").arg(record->tableId);
    }
    lockDetails_->setPlainText(QStringLiteral(
        "等待关系\n"
        "  阻塞会话：%1／序列 %2／事务 %3\n"
        "  等待会话：%4／序列 %5／事务 %6\n\n"
        "锁与对象\n"
        "  对象：%7\n"
        "  TABLE_ID：%8\n"
        "  锁类型／模式：%9／%10\n"
        "  可信度：%11\n"
        "  说明：对象来自阻塞事务持锁记录；候选对象可能不是唯一根因。\n\n"
        "等待操作\n"
        "  %12｜用户 %13｜应用 %14｜客户端 %15／%16\n"
        "  SQL：%17\n\n"
        "阻塞当前／最近操作\n"
        "  %18｜用户 %19｜应用 %20｜客户端 %21／%22\n"
        "  SQL：%23")
        .arg(record->blockerSessionId).arg(record->blockerSessionSeq)
        .arg(record->blockerTransactionId).arg(record->waiterSessionId)
        .arg(record->waiterSessionSeq).arg(record->waiterTransactionId)
        .arg(object).arg(record->tableId).arg(record->lockType, record->lockMode,
            DbaDiagnostics::confidenceLabel(record->objectConfidence),
            record->waiterOperation.operation, record->waiterUser,
            record->waiterApplication, record->waiterIp, record->waiterHost,
            record->waiterSql.isEmpty() ? QStringLiteral("—") : record->waiterSql,
            record->blockerOperation.operation, record->blockerUser,
            record->blockerApplication, record->blockerIp, record->blockerHost,
            record->blockerSql.isEmpty() ? QStringLiteral("—") : record->blockerSql));
    const int index = locks_->item(locks_->currentRow(), 0)->data(Qt::UserRole).toInt();
    lockGraph_->selectRecord(index);
    applyLockFilter();
    updateActions();
}
void DbaDocumentWidget::refreshCounts() {
    execute(Request::Counts, DbaDiagnostics::sessionCountsSql());
}
void DbaDocumentWidget::refreshSlowSql() {
    slowFallbackAttempted_ = false;
    slowSource_->setText(QStringLiteral("慢 SQL 数据源：V$SQL_HISTORY。"));
    execute(Request::SlowSql, DbaDiagnostics::slowSqlHistorySql());
}
void DbaDocumentWidget::refreshTablespaces() {
    execute(Request::Tablespaces, DbaManagement::tablespacesSql());
}
void DbaDocumentWidget::refreshDataFiles() {
    execute(Request::DataFiles, DbaManagement::dataFilesSql());
}

void DbaDocumentWidget::refreshParameters() {
    parameterCompatibilityAttempted_ = false;
    databaseVersionCompatibilityAttempted_ = false;
    databaseVersionNotice_.clear();
    databaseOptionsState_->setText(QStringLiteral(
        "环境快照加载完成后将继续读取版本和 V$OPTION；旧结果暂时保留。"));
    execute(Request::Parameters, DbaManagement::parametersSql());
}

void DbaDocumentWidget::refreshDatabaseVersion() {
    databaseVersionCompatibilityAttempted_ = false;
    databaseOptionsState_->setText(QStringLiteral(
        "参数快照已完成，正在解析 ID_CODE、发行类型和内部版本…"));
    execute(Request::DatabaseVersion, DbaManagement::databaseVersionSql());
}

void DbaDocumentWidget::refreshDatabaseOptions() {
    databaseOptionsState_->setText(QStringLiteral(
        "版本信息已完成，正在读取 V$OPTION 全量选项…"));
    execute(Request::DatabaseOptions, DbaManagement::databaseOptionsSql());
}
void DbaDocumentWidget::refreshUsers() {
    loadUserPage(userPage_);
}
void DbaDocumentWidget::createUser() {
    if (state_ != SessionState::Connected || request_ != Request::None
            || profile_.readOnly) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("新建用户"));
    dialog.setProperty("confirmationForm", true);
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(QStringLiteral("创建用户：立即在当前数据库创建用户。\n取消：不创建用户。"), &dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* form = new QFormLayout();
    auto* name = new QLineEdit(&dialog);
    auto* password = new QLineEdit(&dialog);
    auto* confirmation = new QLineEdit(&dialog);
    auto* tablespace = new QLineEdit(&dialog);
    auto* profile = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    confirmation->setEchoMode(QLineEdit::Password);
    tablespace->setPlaceholderText(QStringLiteral("可选"));
    profile->setPlaceholderText(QStringLiteral("可选"));
    form->addRow(QStringLiteral("用户名："), name);
    form->addRow(QStringLiteral("密码："), password);
    form->addRow(QStringLiteral("确认密码："), confirmation);
    form->addRow(QStringLiteral("默认表空间："), tablespace);
    form->addRow(QStringLiteral("Profile："), profile);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("创建用户"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString userName = name->text().trimmed();
    QString secret = password->text();
    QString repeated = confirmation->text();
    password->clear();
    confirmation->clear();
    if (userName.isEmpty() || secret.isEmpty() || secret != repeated) {
        secret.fill(QChar(u'\0'));
        repeated.fill(QChar(u'\0'));
        QMessageBox::warning(this, QStringLiteral("未创建用户"),
            QStringLiteral("用户名和密码不能为空，且两次密码必须一致。"));
        return;
    }
    repeated.fill(QChar(u'\0'));
    QString escaped = secret;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    QString sql = QStringLiteral("CREATE USER %1 IDENTIFIED BY \"%2\"")
        .arg(RoleEditorModel::quoteIdentifier(userName), escaped);
    if (!tablespace->text().trimmed().isEmpty()) {
        sql += QStringLiteral(" DEFAULT TABLESPACE %1")
            .arg(RoleEditorModel::quoteIdentifier(tablespace->text().trimmed()));
    }
    if (!profile->text().trimmed().isEmpty()) {
        sql += QStringLiteral(" PROFILE %1")
            .arg(RoleEditorModel::quoteIdentifier(profile->text().trimmed()));
    }
    const QString displaySql = QStringLiteral(
        "CREATE USER %1 IDENTIFIED BY <redacted>")
        .arg(RoleEditorModel::quoteIdentifier(userName));
    secret.fill(QChar(u'\0'));
    escaped.fill(QChar(u'\0'));
    pendingCreatedUser_ = userName;
    request_ = Request::CreateUser;
    updateActions();
    QMetaObject::invokeMethod(session_, "executeBound", Qt::QueuedConnection,
        Q_ARG(QString, sql), Q_ARG(QString, displaySql),
        Q_ARG(QVector<EditCellValue>, QVector<EditCellValue>{}));
}
void DbaDocumentWidget::loadUserPage(int page) {
    if (page < 0) return;
    userPage_ = page;
    const int pageSize = rolesPageSize_->currentData().toInt();
    execute(Request::Users, DbaManagement::usersSql(
        userPage_ * pageSize, pageSize, usersFilter_->text()));
}
void DbaDocumentWidget::refreshRoles() {
    loadRolePage(rolePage_);
}
void DbaDocumentWidget::loadRolePage(int page) {
    if (page < 0) return;
    rolePage_ = page;
    const int pageSize = rolesPageSize_->currentData().toInt();
    execute(Request::Roles,
        DbaManagement::rolesSql(rolePage_ * pageSize, pageSize));
}
void DbaDocumentWidget::loadRolePrivilegePage(int page) {
    const QString role = selectedRole();
    if (role.isEmpty()) return;
    rolePrivilegePage_ = qMax(0, page);
    const int pageSize = rolePrivilegesPageSize_->currentData().toInt();
    execute(Request::RolePrivileges,
        DbaManagement::rolePrivilegesSql(
            role, rolePrivilegePage_ * pageSize, pageSize));
}
void DbaDocumentWidget::loadRoleMemberPage(int page) {
    const QString role = selectedRole();
    if (role.isEmpty()) return;
    roleMemberPage_ = qMax(0, page);
    const int pageSize = roleMembersPageSize_->currentData().toInt();
    execute(Request::RoleMembers,
        DbaManagement::roleMembersSql(
            role, roleMemberPage_ * pageSize, pageSize));
}
void DbaDocumentWidget::loadSelectedRoleDetails() {
    if (selectedRole().isEmpty() || isBusy()) return;
    rolePrivilegePage_ = 0;
    roleMemberPage_ = 0;
    loadRoleMembersAfterPrivileges_ = true;
    loadRolePrivilegePage(0);
}
void DbaDocumentWidget::refreshPrivileges() {
    execute(Request::Privileges, DbaManagement::privilegesSql());
}
void DbaDocumentWidget::refreshJobs() {
    execute(Request::Jobs, DbaManagement::jobsSql());
}
void DbaDocumentWidget::refreshJobSchedules() {
    execute(Request::JobSchedules, DbaManagement::jobSchedulesSql());
}
void DbaDocumentWidget::refreshJobHistory() {
    execute(Request::JobHistory, DbaManagement::jobHistorySql());
}

void DbaDocumentWidget::cancelRequest() {
    if (comparisonBusy_) {
        comparisonCancelRequested_ = true;
        cancelRequest_->setEnabled(false);
        comparisonState_->setText(QStringLiteral(
            "正在取消目标环境读取；取消后将保留上次成功结果…"));
        comparisonSession_->requestCancel();
        return;
    }
    if (request_ == Request::None) return;
    cancelRequest_->setEnabled(false);
    context_->setText(QStringLiteral("连接：%1｜状态：正在取消｜独立 Session")
        .arg(connectionDisplay()));
    session_->requestCancel();
}

void DbaDocumentWidget::loadSelectedSessionSql() {
    const qint64 sessionId = selectedSessionNumber(QStringLiteral("SESS_ID"));
    if (sessionId <= 0 || isBusy()) return;
    sessionSql_->clear();
    sessionSql_->setPlaceholderText(QStringLiteral("正在读取完整 SQL…"));
    sessionDetailTabs_->setCurrentIndex(1);
    sessionsState_->setText(QStringLiteral("正在按需读取会话 %1 的完整 SQL…")
        .arg(sessionId));
    execute(Request::FullSessionSql, DbaDiagnostics::fullSessionSql(sessionId));
}

void DbaDocumentWidget::cancelSelectedSessionOperation() {
    beginSessionAction(PendingSessionAction::CancelOperation);
}

void DbaDocumentWidget::closeSelectedSession() {
    beginSessionAction(PendingSessionAction::CloseSession);
}

void DbaDocumentWidget::beginSessionAction(PendingSessionAction action) {
    const qint64 sessionId = selectedSessionNumber(QStringLiteral("SESS_ID"));
    const qint64 sessionSeq = selectedSessionNumber(QStringLiteral("SESS_SEQ"));
    if (profile_.readOnly) {
        QMessageBox::warning(this, QStringLiteral("只读连接"),
            QStringLiteral("当前连接设置为只读，不能执行会话取消或终止操作。"));
        return;
    }
    if (sessionId <= 0 || sessionSeq <= 0
            || selectedSessionValue(QStringLiteral("IS_SELF")) == QStringLiteral("1")) {
        QMessageBox::warning(this, QStringLiteral("禁止操作会话"),
            QStringLiteral("不能操作当前管理连接，且会话 ID 与序列号必须有效。"));
        return;
    }
    const bool closing = action == PendingSessionAction::CloseSession;
    const QString target = selectedSessionContext(sessionId);
    const QString sql = selectedSessionValue(QStringLiteral("SQL_TEXT"));
    const QString impact = closing
        ? QStringLiteral("终止会话会断开连接，未提交事务通常会回滚。")
        : QStringLiteral("只取消当前正在执行的操作，连接可以继续使用。");
    if (MessagePrompt::show(QMessageBox::Question, this,
            closing ? QStringLiteral("确认终止会话") : QStringLiteral("确认取消当前操作"),
            QStringLiteral("%1\n%2\n\n当前／最近 SQL：\n%3\n\n继续？")
                .arg(target, impact, sql.isEmpty() ? QStringLiteral("—") : sql),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No,
            closing ? QStringLiteral("终止会话") : QStringLiteral("取消操作"), QStringLiteral("保持会话不变"), QStringLiteral("返回")) != QMessageBox::Yes) return;
    if (profile_.environment == ConnectionEnvironment::Production
        && MessagePrompt::show(QMessageBox::Warning, this, QStringLiteral("生产环境二次确认"),
            QStringLiteral("%1\n这是生产环境。再次确认%2？")
                .arg(target, closing ? QStringLiteral("终止该会话")
                                     : QStringLiteral("取消该会话的当前操作")),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
            closing ? QStringLiteral("终止会话") : QStringLiteral("取消操作"), QStringLiteral("保持会话不变"), QStringLiteral("返回")) != QMessageBox::Yes) return;
    pendingSessionAction_ = action;
    pendingSessionId_ = sessionId;
    pendingSessionSeq_ = sessionSeq;
    sessionValidationMatched_ = false;
    sessionsState_->setText(QStringLiteral("正在复核会话 %1／序列 %2…")
        .arg(sessionId).arg(sessionSeq));
    execute(Request::ValidateSession,
        DbaDiagnostics::validateSessionSql(sessionId, sessionSeq));
}

void DbaDocumentWidget::beginLockSessionAction(
        PendingSessionAction action, bool blocker) {
    const LockWaitRecord* record = selectedLockRecord();
    if (record == nullptr || isBusy()) return;
    if (profile_.readOnly) {
        QMessageBox::warning(this, QStringLiteral("只读连接"),
            QStringLiteral("当前连接设置为只读，不能取消操作或 Kill 会话。"));
        return;
    }
    const qint64 sessionId = blocker
        ? record->blockerSessionId : record->waiterSessionId;
    const qint64 sessionSeq = blocker
        ? record->blockerSessionSeq : record->waiterSessionSeq;
    const bool self = blocker ? record->blockerIsSelf : record->waiterIsSelf;
    if (sessionId <= 0 || sessionSeq <= 0 || self
            || record->blockerTransactionId <= 0
            || record->waiterTransactionId <= 0) {
        QMessageBox::warning(this, QStringLiteral("禁止操作会话"),
            QStringLiteral("目标会话身份或等待关系不完整，或目标是当前管理连接。"
                           "为避免误杀，本次操作已拒绝。"));
        return;
    }
    QString object = record->objectName;
    if (!record->objectSchema.isEmpty() && !object.isEmpty()) {
        object.prepend(record->objectSchema + QLatin1Char('.'));
    } else if (object.isEmpty() && record->tableId > 0) {
        object = QStringLiteral("TABLE_ID %1").arg(record->tableId);
    } else if (object.isEmpty()) {
        object = QStringLiteral("未知");
    }
    const bool closing = action == PendingSessionAction::CloseSession;
    const QString role = blocker ? QStringLiteral("阻塞") : QStringLiteral("等待");
    const QString user = blocker ? record->blockerUser : record->waiterUser;
    const QString application = blocker
        ? record->blockerApplication : record->waiterApplication;
    const QString sql = blocker ? record->blockerSql : record->waiterSql;
    const QString impact = closing
        ? QStringLiteral("Kill 会断开连接，目标会话的未提交事务通常会回滚。")
        : QStringLiteral("仅请求取消当前操作，不主动断开连接。若阻塞来自未提交事务，取消操作可能不能释放锁。");
    const QString prompt = QStringLiteral(
        "连接：%1｜环境：%2\n"
        "目标：%3会话 %4／序列 %5｜用户 %6｜应用 %7\n"
        "等待关系：阻塞事务 %8 → 等待事务 %9\n"
        "冲突／候选对象：%10（%11）\n\n"
        "%12\n\n当前／最近 SQL：\n%13\n\n继续？")
        .arg(connectionDisplay(), connectionEnvironmentLabel(profile_.environment), role)
        .arg(sessionId).arg(sessionSeq).arg(user, application)
        .arg(record->blockerTransactionId).arg(record->waiterTransactionId)
        .arg(object, DbaDiagnostics::confidenceLabel(record->objectConfidence), impact,
            sql.trimmed().isEmpty() ? QStringLiteral("—") : sql);
    if (MessagePrompt::show(QMessageBox::Question, this,
            closing ? QStringLiteral("确认 Kill %1会话").arg(role)
                    : QStringLiteral("确认取消阻塞操作"),
            prompt, QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No,
            closing ? QStringLiteral("终止会话") : QStringLiteral("取消操作"), QStringLiteral("保持会话不变"), QStringLiteral("返回")) != QMessageBox::Yes) return;
    if (profile_.environment == ConnectionEnvironment::Production
            && MessagePrompt::show(QMessageBox::Warning, this, QStringLiteral("生产环境二次确认"),
                QStringLiteral("这是生产环境。再次确认%1会话 %2？")
                    .arg(closing ? QStringLiteral("Kill ") : QStringLiteral("取消"))
                    .arg(sessionId),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No,
            closing ? QStringLiteral("终止会话") : QStringLiteral("取消操作"), QStringLiteral("保持会话不变"), QStringLiteral("返回")) != QMessageBox::Yes) return;

    locksTimer_->stop();
    pendingActionFromLocks_ = true;
    pendingSessionAction_ = action;
    pendingSessionId_ = sessionId;
    pendingSessionSeq_ = sessionSeq;
    pendingBlockerTransactionId_ = record->blockerTransactionId;
    pendingWaiterTransactionId_ = record->waiterTransactionId;
    sessionValidationMatched_ = false;
    lockState_->setText(QStringLiteral(
        "正在复核%1会话 %2／序列 %3 及当前等待关系…")
        .arg(role).arg(sessionId).arg(sessionSeq));
    execute(Request::ValidateSession, blocker
        ? DbaDiagnostics::validateBlockingSessionSql(sessionId, sessionSeq,
            pendingBlockerTransactionId_, pendingWaiterTransactionId_)
        : DbaDiagnostics::validateWaitingSessionSql(sessionId, sessionSeq,
            pendingBlockerTransactionId_, pendingWaiterTransactionId_));
}

void DbaDocumentWidget::runValidatedSessionAction() {
    QLabel* stateLabel = pendingActionFromLocks_ ? lockState_ : sessionsState_;
    if (pendingSessionAction_ == PendingSessionAction::CancelOperation) {
        stateLabel->setText(QStringLiteral("正在发送取消操作请求…"));
        execute(Request::CancelSessionOperation,
            DbaDiagnostics::cancelSessionOperationSql(pendingSessionId_));
    } else if (pendingSessionAction_ == PendingSessionAction::CloseSession) {
        stateLabel->setText(QStringLiteral("正在发送 Kill 会话请求…"));
        execute(Request::CloseSession, DbaDiagnostics::closeSessionSql(pendingSessionId_));
    }
}

void DbaDocumentWidget::handleState(SessionState state, const QString& detail) {
    state_ = state;
    const QString shownConnection =
        state == SessionState::Connecting && !pendingProfile_.name.isEmpty()
        ? pendingProfile_.name : connectionDisplay();
    context_->setText(QStringLiteral("连接：%1｜状态：%2｜独立 Session")
        .arg(shownConnection, detail));
    emit statusChanged(shownConnection, detail);
    updateActions();
    updateComparisonAction(false);
    refreshParametersAfterUpdate();
}

QueryBatch DbaDocumentWidget::presentSessionBatch(const QueryBatch& batch) {
    QueryBatch presented = batch;
    const QStringList primary = {QStringLiteral("SESS_ID"), QStringLiteral("USER_NAME"),
        QStringLiteral("STATE"), QStringLiteral("SQL_TEXT"), QStringLiteral("APPNAME")};
    const QStringList derived = {QStringLiteral("RISK"), QStringLiteral("CHANGE"),
        QStringLiteral("SESS_ID"), QStringLiteral("USER_NAME"), QStringLiteral("STATE"),
        QStringLiteral("TRANSACTION"),
        QStringLiteral("OPERATION"), QStringLiteral("SQL_TEXT"), QStringLiteral("CLIENT"),
        QStringLiteral("APPNAME"), QStringLiteral("IDLE_FOR"),
        QStringLiteral("CONNECTED_FOR"), QStringLiteral("RISK_ORDER")};
    QStringList trailing;
    for (const QString& name : batch.columns) {
        if (!primary.contains(name)) trailing.append(name);
    }
    presented.columns = derived + trailing;
    presented.columnMetadata.clear();
    presented.typedRows.clear();
    presented.nullCells.clear();
    presented.rows.clear();
    const QDateTime now = QDateTime::currentDateTime();
    auto source = [&batch](const QStringList& row, const QString& name) {
        const int column = batch.columns.indexOf(name);
        return column >= 0 ? row.value(column) : QString();
    };
    for (const QStringList& row : batch.rows) {
        const QString state = source(row, QStringLiteral("STATE")).trimmed().toUpper();
        const QString transactionId = source(row, QStringLiteral("TRX_ID"));
        const bool transaction = DbaDiagnostics::hasTransaction(transactionId);
        const bool self = source(row, QStringLiteral("IS_SELF")) == QStringLiteral("1");
        const QString risk = DbaDiagnostics::sessionRisk(state, transactionId, self);
        const QString riskOrder = risk == QStringLiteral("空闲有事务")
            ? QStringLiteral("10") : risk == QStringLiteral("活动事务")
            ? QStringLiteral("20") : risk == QStringLiteral("等待中")
            ? QStringLiteral("30") : risk == QStringLiteral("活动")
            ? QStringLiteral("40") : risk == QStringLiteral("正常")
            ? QStringLiteral("80") : QStringLiteral("90");
        const QString sql = source(row, QStringLiteral("SQL_TEXT"));
        const QString sessionId = source(row, QStringLiteral("SESS_ID"));
        const QString sessionSequence = source(row, QStringLiteral("SESS_SEQ"));
        const QString sessionKey = sessionId.isEmpty() || sessionSequence.isEmpty()
            ? QString() : QStringLiteral("%1:%2").arg(sessionId, sessionSequence);
        const QString signature = DbaDiagnostics::sessionSnapshotSignature(
            state, transactionId, sql);
        QString change = QStringLiteral("—");
        if (!sessionKey.isEmpty()) {
            pendingSessionKeys_.insert(sessionKey);
            pendingSessionSnapshot_.insert(sessionKey, signature);
            change = DbaDiagnostics::sessionSnapshotChange(
                previousSessionSnapshot_.value(sessionKey), signature,
                compareSessionSnapshot_);
            if (change == QStringLiteral("新增")) {
                ++sessionNewCount_;
            } else if (change == QStringLiteral("已变化")) {
                ++sessionChangedCount_;
            }
        }
        const SqlOperationInfo operation = DbaDiagnostics::analyzeSqlOperation(
            sql, source(row, QStringLiteral("CURR_SCH")));
        const QString ip = DbaDiagnostics::normalizeClientIp(
            source(row, QStringLiteral("CLNT_IP")));
        const QString host = source(row, QStringLiteral("CLNT_HOST")).trimmed();
        const QString client = host.isEmpty() ? ip
            : ip.isEmpty() ? host : ip + QStringLiteral("／") + host;
        QStringList values = {risk, change, sessionId,
            source(row, QStringLiteral("USER_NAME")), state,
            transaction ? transactionId : QStringLiteral("无事务"),
            operation.operation, sql, client, source(row, QStringLiteral("APPNAME")),
            elapsedSessionLabel(source(row, QStringLiteral("LAST_RECV_TIME")), now),
            elapsedSessionLabel(source(row, QStringLiteral("CREATE_TIME")), now), riskOrder};
        for (const QString& name : trailing) values.append(source(row, name));
        presented.rows.append(std::move(values));
    }
    return presented;
}

void DbaDocumentWidget::handleBatch(const QueryBatch& batch) {
    if (!batch.query) return;
    bool continueFetching = true;
    if (request_ == Request::Sessions) {
        if (!DbaDiagnostics::appendSessionSnapshotBatch(
                pendingSessionColumns_, pendingSessionRows_,
                batch.columns, batch.rows)) {
            pendingSessionColumns_.clear();
            pendingSessionRows_.clear();
            sessionsState_->setText(QStringLiteral(
                "分页结果列结构发生变化；保留上次成功快照。"));
            session_->requestCancel();
            continueFetching = false;
        }
        if (batch.hasMore) {
            sessionsState_->setText(QStringLiteral(
                "正在加载会话：%1 条；完成前保留上次成功快照…")
                .arg(pendingSessionRows_.size()));
        }
    } else if (request_ == Request::FullSessionSql) {
        if (!batch.rows.isEmpty() && !batch.rows.first().isEmpty()) {
            const QString fullSql = batch.rows.first().first();
            if (firstBatch_) sessionSql_->setPlainText(fullSql);
            else sessionSql_->appendPlainText(fullSql);
            sessionsState_->setText(fullSql.isEmpty()
                ? QStringLiteral("该会话没有可读取的完整 SQL。")
                : QStringLiteral("已加载会话完整 SQL。"));
        }
    } else if (request_ == Request::ValidateSession) {
        sessionValidationMatched_ = sessionValidationMatched_ || !batch.rows.isEmpty();
    } else if (request_ == Request::VerifyClosedSession) {
        if (!batch.rows.isEmpty()) {
            verifiedSessionExists_ = true;
            const int sequenceColumn = batch.columns.indexOf(QStringLiteral("SESS_SEQ"));
            if (sequenceColumn >= 0) {
                bool validSequence = false;
                verifiedSessionSeq_ = batch.rows.first().value(sequenceColumn)
                    .toLongLong(&validSequence);
                verifiedSessionIdentityValid_ = validSequence
                    && verifiedSessionSeq_ > 0;
            }
        }
    } else if (request_ == Request::LocksDetailed || request_ == Request::Locks) {
        if (firstBatch_) {
            pendingLockColumns_ = batch.columns;
        } else if (pendingLockColumns_ != batch.columns) {
            pendingLockColumns_.clear();
            pendingLockRows_.clear();
            lockState_->setText(QStringLiteral("分页结果列结构发生变化；保留上次快照。"));
            session_->requestCancel();
            continueFetching = false;
        }
        if (continueFetching) pendingLockRows_ += batch.rows;
        if (batch.hasMore) {
            lockState_->setText(QStringLiteral("正在采集锁等待：%1 条原始记录…")
                .arg(pendingLockRows_.size()));
        }
    } else if (request_ == Request::Counts) {
        if (!batch.rows.isEmpty() && batch.rows.first().size() >= 2) {
            samples_.append({batch.rows.first().at(0).toLongLong(),
                batch.rows.first().at(1).toLongLong()});
            while (samples_.size() > DbaDiagnostics::kMaximumSamples) samples_.removeFirst();
            trendChart_->setSamples(samples_);
            trend_->setText(QStringLiteral("会话趋势：%1 个样本，最新总数 %2，活动 %3。"
                "CPU／内存单位待目标版本确认。")
                .arg(samples_.size()).arg(samples_.last().first).arg(samples_.last().second));
        }
    } else if (request_ == Request::SlowSql || request_ == Request::SlowSqlFallback) {
        populate(slowSql_, batch, !firstBatch_);
    } else if (isManagementRequest(request_)) {
        continueFetching = collectManagementBatch(batch);
    }
    firstBatch_ = false;
    if (batch.hasMore && continueFetching) {
        QMetaObject::invokeMethod(session_, "fetchMore", Qt::QueuedConnection);
    }
}

bool DbaDocumentWidget::collectManagementBatch(const QueryBatch& batch) {
    DbaManagementAccumulator* accumulator = managementAccumulator(request_);
    if (accumulator == nullptr || !accumulator->append(batch.columns, batch.rows)) {
        if (accumulator != nullptr) accumulator->fail();
        showRequestFailure(request_, QStringLiteral("分页结果列结构发生变化"));
        session_->requestCancel();
        return false;
    }
    const int count = accumulator->pendingRowCount();
    if (request_ == Request::Tablespaces || request_ == Request::DataFiles) {
        tablespaceState_->setText(QStringLiteral("正在读取只读结果：%1…").arg(count));
    } else if (request_ == Request::Users || request_ == Request::Roles
            || request_ == Request::RolePrivileges
            || request_ == Request::RoleMembers
            || request_ == Request::Privileges) {
        usersState_->setText(QStringLiteral("正在读取只读结果：%1…").arg(count));
    } else if (request_ == Request::Parameters) {
        parametersState_->setText(QStringLiteral("正在读取只读结果：%1…").arg(count));
    } else if (request_ == Request::DatabaseVersion) {
        databaseOptionsState_->setText(QStringLiteral("正在读取数据库版本信息…"));
    } else if (request_ == Request::DatabaseOptions) {
        databaseOptionsState_->setText(QStringLiteral(
            "正在读取 V$OPTION：已接收 %1 项…").arg(count));
    } else {
        jobsState_->setText(QStringLiteral("正在读取只读结果：%1…").arg(count));
    }
    return true;
}

void DbaDocumentWidget::commitManagementBatch(Request request) {
    DbaManagementAccumulator* accumulator = managementAccumulator(request);
    if (accumulator == nullptr) return;
    const DbaManagementQuery query = managementQuery(request);
    const DbaManagementResult& result = accumulator->complete();
    if (request == Request::Parameters) populateEnvironment(result);
    else if (request == Request::DatabaseVersion) populateEnvironmentVersion(result);
    else populateManagement(managementTable(request), result, query);
    QString summary = DbaManagement::resultSummary(
        query, result.rows.size(), result.truncated);
    if (request == Request::Users) summary += QStringLiteral(" 未读取认证字段。");
    if (request == Request::Parameters) {
        summary += QStringLiteral("；点击说明图标或双击参数行查看完整说明。");
    } else if (request == Request::DatabaseVersion) {
        summary += databaseVersionCompatibilityAttempted_
            ? QStringLiteral("；ID_CODE 解析不可用，已显示 V$INSTANCE 兼容信息。")
            : QStringLiteral("；已解析 ID_CODE、发行类型和内部版本。");
    } else if (request == Request::DatabaseOptions && !databaseVersionNotice_.isEmpty()) {
        summary += QStringLiteral("；%1。").arg(databaseVersionNotice_);
    }
        if (request == Request::Users) {
            userHasNext_ = result.truncated;
            rolesPageLabel_->setText(QStringLiteral("第 %1 页").arg(userPage_ + 1));
            summary = QStringLiteral("用户第 %1 页已加载 %2 项。")
                .arg(userPage_ + 1).arg(result.rows.size());
        }
        if (request == Request::Roles) {
        roleHasNext_ = result.truncated;
        rolesPageLabel_->setText(
            QStringLiteral("第 %1 页").arg(rolePage_ + 1));
        summary = QStringLiteral("角色第 %1 页已加载 %2 项。")
            .arg(rolePage_ + 1)
            .arg(result.rows.size());
        privileges_->clear();
        privileges_->setRowCount(0);
        privileges_->setColumnCount(0);
        roleMembers_->clear();
        roleMembers_->setRowCount(0);
        roleMembers_->setColumnCount(0);
        if (!result.rows.isEmpty()) users_->selectRow(0);
        summary += QStringLiteral(" 已自动选择首项并加载右侧数据。");
    }
    if (request == Request::RolePrivileges) {
        rolePrivilegeHasNext_ = result.truncated;
        rolePrivilegesPageLabel_->setText(
            QStringLiteral("第 %1 页").arg(rolePrivilegePage_ + 1));
        summary = QStringLiteral("角色 %1：权限第 %2 页已加载 %3 项。")
            .arg(selectedRole())
            .arg(rolePrivilegePage_ + 1)
            .arg(result.rows.size());
    }
    if (request == Request::RoleMembers) {
        roleMemberHasNext_ = result.truncated;
        roleMembersPageLabel_->setText(
            QStringLiteral("第 %1 页").arg(roleMemberPage_ + 1));
        summary = QStringLiteral("角色 %1：成员第 %2 页已加载 %3 项。")
            .arg(selectedRole())
            .arg(roleMemberPage_ + 1)
            .arg(result.rows.size());
    }
    if (request == Request::Tablespaces || request == Request::DataFiles) {
        tablespaceState_->setText(summary);
    } else if (request == Request::Users || request == Request::Roles
            || request == Request::RolePrivileges
            || request == Request::RoleMembers
            || request == Request::Privileges) {
        usersState_->setText(summary);
    } else if (request == Request::Parameters) {
        parametersState_->setText(parameterUpdateNotice_.isEmpty() ? summary : parameterUpdateNotice_ + QLatin1Char('\n') + summary);
        parameterUpdateNotice_.clear();
    } else if (request == Request::DatabaseVersion
            || request == Request::DatabaseOptions) {
        databaseOptionsState_->setText(summary);
    } else {
        jobsState_->setText(summary);
    }
}

bool DbaDocumentWidget::isManagementRequest(Request request) {
    return request == Request::Tablespaces || request == Request::DataFiles
        || request == Request::Users || request == Request::Roles
        || request == Request::RolePrivileges || request == Request::RoleMembers
        || request == Request::Privileges
        || request == Request::Jobs || request == Request::JobSchedules
        || request == Request::JobHistory || request == Request::Parameters
        || request == Request::DatabaseVersion || request == Request::DatabaseOptions;
}

DbaManagementQuery DbaDocumentWidget::managementQuery(Request request) {
    switch (request) {
    case Request::Tablespaces: return DbaManagementQuery::Tablespaces;
    case Request::DataFiles: return DbaManagementQuery::DataFiles;
    case Request::Users: return DbaManagementQuery::Users;
    case Request::Roles: return DbaManagementQuery::Roles;
    case Request::RolePrivileges: return DbaManagementQuery::RolePrivileges;
    case Request::RoleMembers: return DbaManagementQuery::RoleMembers;
    case Request::Privileges: return DbaManagementQuery::Privileges;
    case Request::Jobs: return DbaManagementQuery::Jobs;
    case Request::JobSchedules: return DbaManagementQuery::JobSchedules;
    case Request::JobHistory: return DbaManagementQuery::JobHistory;
    case Request::Parameters: return DbaManagementQuery::Parameters;
    case Request::DatabaseVersion: return DbaManagementQuery::DatabaseVersion;
    case Request::DatabaseOptions: return DbaManagementQuery::DatabaseOptions;
    default: return DbaManagementQuery::Tablespaces;
    }
}

DbaManagementAccumulator* DbaDocumentWidget::managementAccumulator(Request request) {
    switch (request) {
    case Request::Tablespaces: return &tablespaceResult_;
    case Request::DataFiles: return &dataFileResult_;
    case Request::Users: return &userResult_;
    case Request::Roles: return &roleResult_;
    case Request::RolePrivileges: return &rolePrivilegeResult_;
    case Request::RoleMembers: return &roleMemberResult_;
    case Request::Privileges: return &privilegeResult_;
    case Request::Jobs: return &jobResult_;
    case Request::JobSchedules: return &jobScheduleResult_;
    case Request::JobHistory: return &jobHistoryResult_;
    case Request::Parameters: return &parameterResult_;
    case Request::DatabaseVersion: return &databaseVersionResult_;
    case Request::DatabaseOptions: return &databaseOptionsResult_;
    default: return nullptr;
    }
}

QTableWidget* DbaDocumentWidget::managementTable(Request request) const {
    switch (request) {
    case Request::Tablespaces: return tablespaces_;
    case Request::DataFiles: return dataFiles_;
    case Request::Users: return users_;
    case Request::Roles: return users_;
    case Request::RolePrivileges: return privileges_;
    case Request::RoleMembers: return roleMembers_;
    case Request::Privileges: return privileges_;
    case Request::Jobs: return jobs_;
    case Request::JobSchedules: return jobSchedules_;
    case Request::JobHistory: return jobHistory_;
    case Request::Parameters: return parameters_;
    case Request::DatabaseOptions: return databaseOptions_;
    default: return nullptr;
    }
}

void DbaDocumentWidget::populateManagement(
    QTableWidget* table, const DbaManagementResult& result, DbaManagementQuery query) {
    if (table == nullptr) return;
    table->setSortingEnabled(false);
    table->clear();
    table->setColumnCount(result.columns.size());
    table->setHorizontalHeaderLabels(result.columns);
    table->setRowCount(result.rows.size());
    for (int row = 0; row < result.rows.size(); ++row) {
        for (int column = 0; column < result.columns.size(); ++column) {
            table->setItem(row, column, new QTableWidgetItem(
                DbaManagement::formatValue(
                    query, result.columns.at(column), result.rows.at(row).value(column))));
        }
    }
    for (int column = 0; column < result.columns.size(); ++column) {
        const int headerWidth = result.columns.at(column).size() * 9 + 28;
        table->setColumnWidth(column, qBound(90, headerWidth, 240));
    }
    table->horizontalHeader()->setStretchLastSection(true);
    if (kind_ == DbaDocumentKind::Roles && table == privileges_) {
        const int privilegeColumn = result.columns.indexOf(QStringLiteral("PRIVILEGE"));
        if (privilegeColumn >= 0 && result.columns.size() == 3) {
            table->horizontalHeader()->setStretchLastSection(false);
            const auto applyRolePrivilegeWidths = [table, privilegeColumn]() {
                const int available = table->viewport()->width();
                if (available <= 0) return;
                const int privilegeWidth = available / 2;
                const int sideWidth = (available - privilegeWidth) / 2;
                for (int column = 0; column < table->columnCount(); ++column) {
                    table->setColumnWidth(column,
                        column == privilegeColumn ? privilegeWidth : sideWidth);
                }
            };
            applyRolePrivilegeWidths();
            QTimer::singleShot(0, table, applyRolePrivilegeWidths);
        }
    }
    table->setSortingEnabled(true);
    if (table == tablespaces_ || table == dataFiles_) {
        filterTable(table, tablespaceFilter_->text());
    } else if (table == users_) {
        filterTable(table, usersFilter_->text());
    } else if (table == privileges_) {
        filterTable(table, privilegesFilter_->text());
    } else if (table == roleMembers_) {
        filterTable(table, roleMembersFilter_->text());
    } else if (table == parameters_) {
        filterTable(table, parametersFilter_->text());
    } else if (table == databaseOptions_) {
        filterTable(table, databaseOptionsFilter_->text());
    } else {
        filterTable(table, jobsFilter_->text());
    }
}

void DbaDocumentWidget::refreshWatchedParameters() {
    if (watchedParameters_ == nullptr) return;
    QSettings settings;
    const QString key = QStringLiteral("settings/databaseParameters/watched");
    const QString revisionKey = QStringLiteral("settings/databaseParameters/defaultWatchedRevision");
    const bool addDefaults = !settings.contains(key) || settings.value(revisionKey, 0).toInt() < 1;
    const auto names = DatabaseEnvironment::watchedParameters(settings.value(key).toStringList(), parameterMetadata_, addDefaults);
    if (settings.value(key).toStringList() != names || !settings.contains(key)) settings.setValue(key, names);
    if (addDefaults) settings.setValue(revisionKey, 1);
    watchedParameterNames_ = names;
    watchedParameters_->setSortingEnabled(false);
    watchedParameters_->setColumnCount(5);
    watchedParameters_->setHorizontalHeaderLabels({QStringLiteral("参数"), QStringLiteral("当前值"),
        QStringLiteral("会话值"), QStringLiteral("文件值"), QStringLiteral("参数说明")});
    for (int column = 1; column <= 3; ++column) {
        if (auto* header = watchedParameters_->horizontalHeaderItem(column)) {
            header->setToolTip(QStringLiteral("“未获取”表示当前快照没有该参数；“空值／未返回（待核对）”表示当前驱动无法区分该层的空值、空白值和未返回值。"));
        }
    }
    watchedParameters_->setRowCount(names.size());
    for (int row = 0; row < names.size(); ++row) {
        const QString name = names.at(row);
        QStringList values{name, QStringLiteral("未获取"), QString(), QString(),
            QStringLiteral("当前快照未提供此参数；不代表参数不存在。")};
        for (const auto& parameter : environmentSnapshot_) {
            if (parameter.name.compare(name, Qt::CaseInsensitive) != 0) continue;
            values = {name, observedParameterValue(parameter.metadata.format, parameter.currentValue),
                observedParameterValue(parameter.metadata.format, parameter.sessionValue),
                observedParameterValue(parameter.metadata.format, parameter.fileValue),
                parameter.metadata.description.isEmpty() ? parameter.description : parameter.metadata.description};
            break;
        }
        if (values.at(1) == QStringLiteral("未获取") && databaseOptions_ != nullptr) {
            for (int optionRow = 0; optionRow < databaseOptions_->rowCount(); ++optionRow) {
                const auto* option = databaseOptions_->item(optionRow, 0);
                const auto* value = databaseOptions_->item(optionRow, 1);
                if (option && value && option->text().compare(name, Qt::CaseInsensitive) == 0) {
                    values[1] = value->text();
                    values[4] = QStringLiteral("数据库选项");
                    break;
                }
            }
        }
        for (int column = 0; column < values.size(); ++column) {
            watchedParameters_->setItem(row, column, new QTableWidgetItem(values.at(column)));
        }
    }
    watchedParameters_->setColumnWidth(0, 240);
    for (int column = 1; column < 4; ++column) watchedParameters_->setColumnWidth(column, 180);
    watchedParameters_->horizontalHeader()->setStretchLastSection(true);
    watchedParameters_->setSortingEnabled(true);
    filterTable(watchedParameters_, DatabaseEnvironment::canonicalParameterName(watchedParametersFilter_->text()));
    refreshParameterStars();
}

void DbaDocumentWidget::refreshParameterStars() {
    const QIcon star = parameterStarIcon(palette());
    const QVector<QPair<QTableWidget*, int>> tables{{watchedParameters_, 0}, {commonParameters_, 2},
        {parameters_, 2}, {environmentComparison_, 1}};
    for (const auto& entry : tables) {
        if (!entry.first) continue;
        for (int row = 0; row < entry.first->rowCount(); ++row) {
            auto* item = entry.first->item(row, entry.second); if (!item) continue;
            const bool watched = watchedParameterNames_.contains(DatabaseEnvironment::canonicalParameterName(item->text()));
            item->setIcon(watched ? star : QIcon()); item->setData(Qt::UserRole + 3, watched);
            item->setData(Qt::AccessibleTextRole, item->text() + (watched ? QStringLiteral("，已星标关注") : QString()));
            item->setToolTip(item->text() + (item->text() == QStringLiteral("UNICODE_FLAG") ? QStringLiteral("（建库名称 CHARSET）") : QString())
                + (watched ? QStringLiteral("\n已星标关注，可右键取消") : QStringLiteral("\n可右键关注并加星标")));
        }
    }
}

void DbaDocumentWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        refreshParameterStars();
}

void DbaDocumentWidget::populateEnvironment(const DbaManagementResult& result) {
    environmentSnapshot_ = DatabaseEnvironment::snapshot(result, parameterMetadata_);
    refreshWatchedParameters();
    const DatabaseBuildInfo buildInfo = DatabaseEnvironment::buildInfo(result);
    auto populateParameters = [this](QTableWidget* table,
            const QVector<EnvironmentParameter>& source, bool commonOnly) {
        const QString selectedName = table->currentRow() >= 0 && table->item(table->currentRow(), 2)
            ? table->item(table->currentRow(), 2)->text() : QString();
        const int scrollPosition = table->verticalScrollBar()->value();
        table->setSortingEnabled(false);
        table->clear();
        const QStringList headers = commonOnly
            ? QStringList{QStringLiteral("状态"), QStringLiteral("分类"),
                QStringLiteral("参数"), QStringLiteral("参数说明"),
                QStringLiteral("当前值"), QStringLiteral("默认值"), QStringLiteral("变更方式")}
            : QStringList{QStringLiteral("状态"), QStringLiteral("分类"),
                QStringLiteral("参数"), QStringLiteral("参数说明"),
                QStringLiteral("当前值"), QStringLiteral("会话值"), QStringLiteral("文件值"),
                QStringLiteral("默认值"), QStringLiteral("变更方式")};
        table->setColumnCount(headers.size());
        table->setHorizontalHeaderLabels(headers);
        for (int column = 4; column < table->columnCount() - 1; ++column) {
            if (auto* header = table->horizontalHeaderItem(column)) {
                header->setToolTip(QStringLiteral("“空值／未返回（待核对）”表示当前驱动无法区分该层的空值、空白值和未返回值；不代表参数不支持。"));
            }
        }
        if (QTableWidgetItem* detailsHeader = table->horizontalHeaderItem(3)) {
            detailsHeader->setData(Qt::UserRole, QStringLiteral("PARAMETER_DETAILS"));
            detailsHeader->setToolTip(QStringLiteral("点击参数说明打开完整说明"));
        }
        QVector<EnvironmentParameter> rows;
        for (const EnvironmentParameter& item : source) {
            if (!commonOnly || item.common) rows.append(item);
        }
        if (commonOnly) for (const auto& metadata : parameterMetadata_) {
            if (!metadata.defaultWatched || std::any_of(rows.cbegin(), rows.cend(),
                    [&metadata](const auto& item) { return item.name == metadata.name; })) continue;
            EnvironmentParameter missing;
            missing.name = metadata.name; missing.metadata = metadata; missing.common = true;
            missing.description = QStringLiteral("当前快照未提供此参数；未获取到实际值，不代表该版本支持或不支持。");
            rows.append(std::move(missing));
        }
        table->setRowCount(rows.size());
        for (int row = 0; row < rows.size(); ++row) {
            const EnvironmentParameter& item = rows.at(row);
            const bool unavailable = commonOnly && !std::any_of(source.cbegin(), source.cend(),
                [&item](const auto& value) { return value.name == item.name; });
            const QString description = !item.metadata.description.isEmpty()
                ? item.metadata.description : (!item.description.isEmpty()
                    ? item.description
                    : QStringLiteral("当前达梦版本未提供参数说明，请以该版本《配置文件说明》为准。"));
            const QString state = item.statusLabel();
            const QStringList values = commonOnly
                ? QStringList{state, item.metadata.category, item.name,
                    description, unavailable ? QStringLiteral("未获取") : observedParameterValue(item.metadata.format, item.currentValue),
                    unavailable ? QStringLiteral("未获取") : observedParameterValue(item.metadata.format, item.defaultValue),
                    DatabaseEnvironment::effectiveChangePolicyLabel(item)}
                : QStringList{state, item.metadata.category, item.name,
                    description, observedParameterValue(item.metadata.format, item.currentValue),
                    observedParameterValue(item.metadata.format, item.sessionValue),
                    observedParameterValue(item.metadata.format, item.fileValue),
                    observedParameterValue(item.metadata.format, item.defaultValue),
                    DatabaseEnvironment::effectiveChangePolicyLabel(item)};
            for (int column = 0; column < values.size(); ++column) {
                auto* cell = column == 3
                    ? new QTableWidgetItem(InterfaceIcons::icon(QStringLiteral(":/icons/about.svg")),
                        values.at(column))
                    : new QTableWidgetItem(values.at(column));
                cell->setData(Qt::UserRole, item.isDefault());
                cell->setData(Qt::UserRole + 1, item.pendingRestart());
                cell->setData(Qt::UserRole + 2, !item.defaultValue.isEmpty());
                cell->setToolTip(values.at(column));
                if (column >= 4 && column < table->columnCount() - 1) {
                    const int valueIndex = column - 4;
                    const QStringList rawValues = commonOnly
                        ? QStringList{item.currentValue, item.defaultValue}
                        : QStringList{item.currentValue, item.sessionValue, item.fileValue,
                            item.defaultValue};
                    const QString availability = observedParameterValueToolTip(
                        rawValues.value(valueIndex));
                    if (!availability.isEmpty()) cell->setToolTip(availability);
                }
                if (column == 3) {
                    const QString accessible = QStringLiteral("查看 %1（%2）的完整参数说明")
                        .arg(item.metadata.title.isEmpty() ? item.name : item.metadata.title,
                            item.name);
                    cell->setData(Qt::AccessibleTextRole, accessible);
                    cell->setToolTip(QStringLiteral("%1\n点击打开完整参数说明").arg(description));
                }
                if (column == 0) {
                    const Theme::SemanticColor color = item.pendingRestart()
                        ? Theme::SemanticColor::Warning
                        : item.currentValue.isEmpty()
                            ? Theme::SemanticColor::MutedText
                            : item.isDefault()
                                ? Theme::SemanticColor::MutedText
                                : Theme::SemanticColor::Success;
                    cell->setForeground(Theme::semanticColor(color, table->palette()));
                }
                table->setItem(row, column, cell);
            }
        }
        table->horizontalHeader()->setStretchLastSection(false);
        table->setColumnWidth(0, 86);
        table->setColumnWidth(1, 120);
        table->setColumnWidth(2, 210);
        table->setColumnWidth(3, commonOnly ? 430 : 390);
        for (int column = 4; column < table->columnCount() - 1; ++column) {
            table->setColumnWidth(column, 145);
        }
        table->setColumnWidth(table->columnCount() - 1, 104);
        auto* header = table->horizontalHeader();
        const QList<int> order = commonOnly ? QList<int>{2, 4, 3, 5, 6, 1, 0}
            : QList<int>{2, 4, 3, 5, 6, 7, 8, 1, 0};
        for (int visual = 0; visual < order.size(); ++visual) {
            header->moveSection(header->visualIndex(order.at(visual)), visual);
        }
        header->setSectionResizeMode(3, QHeaderView::Stretch);
        table->setColumnWidth(2, qMax(210, table->fontMetrics().horizontalAdvance(
            QStringLiteral("NLS_LENGTH_SEMANTICS")) + 28));
        table->setSortingEnabled(true);
        for (int row = 0; row < table->rowCount(); ++row) {
            if (!selectedName.isEmpty() && table->item(row, 2)->text() == selectedName) {
                table->setCurrentCell(row, 2);
                break;
            }
        }
        table->verticalScrollBar()->setValue(scrollPosition);
    };
    populateParameters(commonParameters_, environmentSnapshot_, true);
    populateParameters(parameters_, environmentSnapshot_, false);
    refreshParameterStars();

    QMap<QString, EnvironmentParameter> byName;
    for (const EnvironmentParameter& item : environmentSnapshot_) byName.insert(item.name, item);
    auto parameterValue = [&byName](const QString& name) {
        if (!byName.contains(name)) return QStringLiteral("未获取");
        const EnvironmentParameter& parameter = byName.value(name);
        return observedParameterValue(parameter.metadata.format, parameter.currentValue);
    };
    const QVector<QStringList> overview = {
        {QStringLiteral("数据库版本"), databaseVersion_.isEmpty()
            ? databaseIdentity_.serverVersion : databaseVersion_, QStringLiteral("连接握手")},
        {QStringLiteral("实例名称"), databaseIdentity_.instanceName.isEmpty()
            ? parameterValue(QStringLiteral("INSTANCE_NAME")) : databaseIdentity_.instanceName,
            QStringLiteral("连接身份／V$DM_INI")},
        {QStringLiteral("字符集"), parameterValue(QStringLiteral("UNICODE_FLAG")),
            QStringLiteral("数据库建库属性")},
        {QStringLiteral("大小写敏感"), parameterValue(QStringLiteral("CASE_SENSITIVE")),
            QStringLiteral("数据库建库属性")},
        {QStringLiteral("Page Size"), parameterValue(QStringLiteral("PAGE_SIZE")),
            QStringLiteral("数据库建库属性")},
        {QStringLiteral("Extent Size"), parameterValue(QStringLiteral("EXTENT_SIZE")),
            QStringLiteral("数据库建库属性")},
        {QStringLiteral("兼容模式"), parameterValue(QStringLiteral("COMPATIBLE_MODE")),
            QStringLiteral("V$DM_INI")},
        {QStringLiteral("Blank Pad Mode"), parameterValue(QStringLiteral("BLANK_PAD_MODE")),
            QStringLiteral("V$DM_INI")},
        {QStringLiteral("数据库目录"), parameterValue(QStringLiteral("SYSTEM_PATH")),
            QStringLiteral("V$DM_INI")},
        {QStringLiteral("归档状态"),
            DatabaseEnvironment::archiveModeLabel(buildInfo.archiveMode),
            QStringLiteral("V$DATABASE")}};
    environmentOverview_->setSortingEnabled(false);
    environmentOverview_->clear();
    environmentOverview_->setColumnCount(3);
    environmentOverview_->setHorizontalHeaderLabels(
        {QStringLiteral("项目"), QStringLiteral("值"), QStringLiteral("数据来源")});
    environmentOverview_->setRowCount(overview.size());
    for (int row = 0; row < overview.size(); ++row) {
        for (int column = 0; column < overview.at(row).size(); ++column) {
            environmentOverview_->setItem(row, column,
                new QTableWidgetItem(overview.at(row).at(column).isEmpty()
                    ? QStringLiteral("不可用") : overview.at(row).at(column)));
        }
    }
    environmentOverview_->horizontalHeader()->setStretchLastSection(true);
    environmentOverview_->resizeColumnsToContents();
    environmentOverview_->setSortingEnabled(true);
    filterEnvironmentTables();
    refreshComparisonProfiles();
}

void DbaDocumentWidget::populateEnvironmentVersion(const DbaManagementResult& result) {
    if (result.rows.isEmpty()) return;
    const QStringList& values = result.rows.first();
    const auto value = [&result, &values](const QString& column) {
        const int index = result.columns.indexOf(column);
        return index < 0 ? QString() : values.value(index).trimmed();
    };
    const auto upsert = [this](const QString& name, const QString& text,
                               const QString& source) {
        if (text.isEmpty()) return;
        int row = -1;
        for (int candidate = 0; candidate < environmentOverview_->rowCount(); ++candidate) {
            const QTableWidgetItem* item = environmentOverview_->item(candidate, 0);
            if (item != nullptr && item->text() == name) {
                row = candidate;
                break;
            }
        }
        if (row < 0) {
            row = environmentOverview_->rowCount();
            environmentOverview_->insertRow(row);
        }
        environmentOverview_->setItem(row, 0, new QTableWidgetItem(name));
        environmentOverview_->setItem(row, 1, new QTableWidgetItem(text));
        environmentOverview_->setItem(row, 2, new QTableWidgetItem(source));
    };

    environmentOverview_->setSortingEnabled(false);
    if (result.columns.contains(QStringLiteral("ID_CODE"))) {
        upsert(QStringLiteral("产品标识 ID_CODE"), value(QStringLiteral("ID_CODE")),
            QStringLiteral("ID_CODE"));
        upsert(QStringLiteral("发行类型"), value(QStringLiteral("BUILD_TYPE")),
            QStringLiteral("ID_CODE 解析"));
        upsert(QStringLiteral("内部版本"), value(QStringLiteral("INNER_VER")),
            QStringLiteral("ID_CODE 解析"));
    } else {
        upsert(QStringLiteral("服务器版本"), value(QStringLiteral("SVR_VERSION")),
            QStringLiteral("V$INSTANCE"));
        upsert(QStringLiteral("构建版本"), value(QStringLiteral("BUILD_VERSION")),
            QStringLiteral("V$INSTANCE"));
        upsert(QStringLiteral("构建时间"), value(QStringLiteral("BUILD_TIME")),
            QStringLiteral("V$INSTANCE"));
    }
    environmentOverview_->resizeColumnsToContents();
    environmentOverview_->setSortingEnabled(true);
}

void DbaDocumentWidget::filterEnvironmentTables() {
    const QString commonNeedle = DatabaseEnvironment::canonicalParameterName(commonParametersFilter_->text());
    const QString preset = commonParametersPresetFilter_->currentData().toString();
    int commonCount = 0;
    int nonDefaultCount = 0;
    int pendingCount = 0;
    int visibleCount = 0;
    for (int row = 0; row < commonParameters_->rowCount(); ++row) {
        ++commonCount;
        bool textMatches = commonNeedle.isEmpty();
        for (int column = 0; !textMatches && column < commonParameters_->columnCount(); ++column) {
            const QTableWidgetItem* item = commonParameters_->item(row, column);
            textMatches = item != nullptr
                && item->text().contains(commonNeedle, Qt::CaseInsensitive);
        }
        const QTableWidgetItem* marker = commonParameters_->item(row, 0);
        const bool isDefault = marker != nullptr && marker->data(Qt::UserRole).toBool();
        const bool pending = marker != nullptr && marker->data(Qt::UserRole + 1).toBool();
        const bool hasDefault = marker != nullptr
            && marker->data(Qt::UserRole + 2).toBool();
        const bool nonDefault = hasDefault && !isDefault;
        if (nonDefault) ++nonDefaultCount;
        if (pending) ++pendingCount;
        const bool presetMatches = preset == QStringLiteral("all")
            || (preset == QStringLiteral("nonDefault") && nonDefault)
            || (preset == QStringLiteral("pending") && pending);
        const bool visible = textMatches && presetMatches;
        commonParameters_->setRowHidden(row, !visible);
        if (visible) ++visibleCount;
    }
    commonParametersSummary_->setToolTip(QStringLiteral(
        "常用参数 %1｜非默认 %2｜待生效 %3｜显示 %4")
        .arg(commonCount).arg(nonDefaultCount).arg(pendingCount).arg(visibleCount));
    commonParametersSummary_->setText(QStringLiteral("显示 %1 项").arg(visibleCount));
    const QString needle = DatabaseEnvironment::canonicalParameterName(parametersFilter_->text());
    for (int row = 0; row < parameters_->rowCount(); ++row) {
        bool textMatches = needle.isEmpty();
        for (int column = 0; !textMatches && column < parameters_->columnCount(); ++column) {
            const QTableWidgetItem* item = parameters_->item(row, column);
            textMatches = item != nullptr
                && item->text().contains(needle, Qt::CaseInsensitive);
        }
        const QTableWidgetItem* marker = parameters_->item(row, 0);
        const bool isDefault = marker != nullptr && marker->data(Qt::UserRole).toBool();
        const bool pending = marker != nullptr && marker->data(Qt::UserRole + 1).toBool();
        const bool hasDefault = marker != nullptr
            && marker->data(Qt::UserRole + 2).toBool();
        parameters_->setRowHidden(row, !textMatches
            || (nonDefaultOnly_->isChecked() && (!hasDefault || isDefault))
            || (pendingOnly_->isChecked() && !pending));
    }
}

void DbaDocumentWidget::refreshParametersAfterUpdate() {
    if (!parameterRefreshPending_) return;
    QTimer::singleShot(0, this, [this]() {
        if (!parameterRefreshPending_ || !isConnected() || isBusy()) return;
        parameterRefreshPending_ = false;
        refreshParameters();
    });
}

void DbaDocumentWidget::editEnvironmentParameter(const QString& name) {
    if (!isConnected() || isBusy() || comparisonBusy_ || profile_.readOnly) return;
    const auto found = std::find_if(environmentSnapshot_.cbegin(), environmentSnapshot_.cend(),
        [&name](const auto& item) { return item.name == name; });
    if (found == environmentSnapshot_.cend()) return;
    const EnvironmentParameter parameter = *found;
    const auto identity = databaseIdentity_;
    const auto profile = profile_;
    const QString reason = DatabaseEnvironment::editBlockReason(parameter);
    if (!reason.isEmpty()) { QMessageBox::information(this, QStringLiteral("不能直接修改"), reason); return; }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("environmentParameterEditor"));
    dialog.setWindowTitle(QStringLiteral("修改当前环境参数：%1").arg(name)); dialog.resize(700, 580);
    dialog.setProperty("confirmationForm", true);
    auto* outer = new QVBoxLayout(&dialog);
    auto* scroll = new QScrollArea(&dialog); scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll); auto* root = new QVBoxLayout(content);
    scroll->setWidget(content); outer->addWidget(scroll, 1);
    auto* context = new QLabel(QStringLiteral("连接：%1\n实例：%2；模式：%3；OGUID：%4\n当前值：%5；文件值：%6\n%7")
        .arg(profile.name, identity.instanceName, identity.instanceMode, identity.oguid,
            parameter.currentValue, parameter.fileValue, DatabaseEnvironment::parameterSummary(parameter)), &dialog);
    context->setTextFormat(Qt::PlainText); context->setWordWrap(true);
    context->setTextInteractionFlags(Qt::TextSelectableByMouse); root->addWidget(context);
    auto* form = new QFormLayout(); form->setRowWrapPolicy(QFormLayout::WrapLongRows); root->addLayout(form);
    auto* options = new AdaptivePopupComboBox(&dialog); options->setObjectName(QStringLiteral("parameterValueChoices"));
    auto* input = new QLineEdit(&dialog); input->setObjectName(QStringLiteral("parameterValueInput")); input->setMaxLength(8187);
    const bool enumeration = DatabaseEnvironment::valueKind(parameter) == QStringLiteral("enum");
    const QStringList choices = parameter.metadata.choices.isEmpty()
        ? QStringList{QStringLiteral("0"), QStringLiteral("1")} : parameter.metadata.choices;
    if (enumeration) {
        for (int index = 0; index < choices.size(); ++index) {
            if (!DatabaseEnvironment::validateParameterValue(parameter, choices[index]).isEmpty()) continue;
            options->addItem(
            QStringLiteral("%1 — %2").arg(choices[index], parameter.metadata.choiceLabels.value(index,
                parameter.metadata.format == QStringLiteral("boolean")
                    ? (choices[index] == QStringLiteral("0") ? QStringLiteral("否／关闭") : QStringLiteral("是／开启"))
                    : QStringLiteral("取值 %1").arg(choices[index]))), choices[index]);
        }
        options->setCurrentIndex(options->findData(parameter.currentValue));
        form->addRow(QStringLiteral("新值"), options); input->hide();
    } else {
        input->setText(parameter.currentValue); form->addRow(QStringLiteral("新值"), input); options->hide();
        const QString kind = DatabaseEnvironment::valueKind(parameter);
        input->setPlaceholderText(kind == QStringLiteral("integer") ? QStringLiteral("填写整数，不带单位或千分位")
            : kind == QStringLiteral("decimal") ? QStringLiteral("填写小数，使用英文小数点")
            : QStringLiteral("填写路径或原始文本，不加 SQL 引号"));
    }
    auto* scope = new AdaptivePopupComboBox(&dialog); scope->setObjectName(QStringLiteral("parameterChangeScope"));
    if (parameter.parameterType.toUpper() != QStringLiteral("IN FILE")) {
        scope->addItem(QStringLiteral("内存和文件（立即生效并保存）"), 1);
        scope->addItem(QStringLiteral("仅内存（重启后不保留）"), 0);
    }
    scope->addItem(QStringLiteral("仅配置文件（内存不变，重启生效）"), 2);
    form->addRow(QStringLiteral("作用范围"), scope);
    auto* rules = new QLabel(QStringLiteral("类型：%1；范围：%2 ～ %3\n%4\n"
        "本操作仅针对上方当前实例，不批量覆盖目标侧；参数变更可能立即影响系统，不能保证用普通事务回滚。服务器仍会校验版本、依赖和权限。")
        .arg(parameter.parameterType, parameter.minimumValue.isEmpty() ? QStringLiteral("未返回") : parameter.minimumValue,
            parameter.maximumValue.isEmpty() ? QStringLiteral("未返回") : parameter.maximumValue,
            DatabaseEnvironment::synchronizationDescription(parameter)), &dialog);
    rules->setTextFormat(Qt::PlainText); rules->setWordWrap(true); root->addWidget(rules);
    auto* help = new QPlainTextEdit(&dialog); help->setObjectName(QStringLiteral("parameterEditDetails"));
    help->setReadOnly(true); help->setPlainText(DatabaseEnvironment::parameterDetails(parameter));
    help->setMinimumHeight(80); help->setMaximumHeight(160); root->addWidget(help);
    auto* error = new QLabel(&dialog); error->setObjectName(QStringLiteral("parameterChangeValidation"));
    error->setWordWrap(true); error->setProperty("callout", QStringLiteral("danger")); root->addWidget(error);
    auto* preview = new QPlainTextEdit(&dialog); preview->setObjectName(QStringLiteral("parameterChangePreview")); preview->setFont(Theme::editorFont());
    preview->setReadOnly(true); preview->setMinimumHeight(120); root->addWidget(preview, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* apply = buttons->addButton(QStringLiteral("确认修改…"), QDialogButtonBox::ActionRole);
    apply->setObjectName(QStringLiteral("applyEnvironmentParameter")); outer->addWidget(buttons);
    QString generatedSql;
    const auto update = [&]() {
        const QString value = enumeration ? options->currentData().toString() : input->text();
        QString failure;
        generatedSql = DatabaseEnvironment::parameterChangeSql(parameter, value, scope->currentData().toInt(),
            identity.instanceName, identity.instanceMode, identity.oguid, &failure);
        error->setText(failure); apply->setEnabled(failure.isEmpty()); preview->setPlainText(generatedSql);
    };
    connect(input, &QLineEdit::textChanged, &dialog, update);
    connect(options, &QComboBox::currentIndexChanged, &dialog, update);
    connect(scope, &QComboBox::currentIndexChanged, &dialog, update);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(apply, &QPushButton::clicked, &dialog, [&]() {
        update(); if (generatedSql.isEmpty()) return;
        if (profile_.id != profile.id || !isConnected() || isBusy() || comparisonBusy_ || profile_.readOnly) {
            error->setText(QStringLiteral("当前连接已变化或正在忙碌，未执行，请关闭后重新打开。")); return;
        }
        if (MessagePrompt::show(QMessageBox::Warning, &dialog, QStringLiteral("确认修改数据库参数"),
                QStringLiteral("连接：%1\n实例：%2\n参数：%3\n当前值：%4；文件值：%5\n新值：%6\n范围：%7\n%8\n\n是否执行上方预览中的修改？")
                    .arg(profile.name, identity.instanceName, name, parameter.currentValue, parameter.fileValue,
                        enumeration ? options->currentData().toString() : input->text(), scope->currentText(),
                        DatabaseEnvironment::synchronizationDescription(parameter)),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel,
                QStringLiteral("执行修改"), QStringLiteral("取消"), QStringLiteral("保持原值")) != QMessageBox::Yes) return;
        if (profile.environment == ConnectionEnvironment::Production && MessagePrompt::show(QMessageBox::Warning,
                &dialog, QStringLiteral("生产环境二次确认"), QStringLiteral("再次确认修改生产实例 %1 的参数 %2？可能立即影响业务，不能依赖普通事务撤销。")
                    .arg(identity.instanceName, name), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel,
                QStringLiteral("修改生产参数"), QStringLiteral("取消"), QStringLiteral("保持原值")) != QMessageBox::Yes) return;
        dialog.accept();
    });
    update();
    if (dialog.exec() != QDialog::Accepted || generatedSql.isEmpty() || profile_.id != profile.id
            || !isConnected() || isBusy() || comparisonBusy_ || profile_.readOnly) return;
    pendingParameterName_ = name;
    execute(Request::ParameterUpdate, generatedSql);
}

void DbaDocumentWidget::showEnvironmentDetails(QTableWidget* table, int row) {
    if (table == nullptr || row < 0) return;
    const int nameColumn = table == environmentComparison_ ? 1 : table == watchedParameters_ ? 0 : 2;
    const QTableWidgetItem* nameItem = table->item(row, nameColumn);
    if (nameItem == nullptr) return;
    const auto found = std::find_if(environmentSnapshot_.cbegin(), environmentSnapshot_.cend(),
        [&nameItem](const EnvironmentParameter& item) { return item.name == nameItem->text(); });
    EnvironmentParameter item;
    if (table == environmentComparison_) {
        if (!comparisonParameters_.contains(nameItem->text())) return;
        item = comparisonParameters_.value(nameItem->text());
    } else if (found != environmentSnapshot_.cend()) item = *found;
    else {
        const auto metadata = std::find_if(parameterMetadata_.cbegin(), parameterMetadata_.cend(),
            [&nameItem](const auto& value) { return value.name == nameItem->text() && value.defaultWatched; });
        if (metadata == parameterMetadata_.cend()) return;
        item.name = metadata->name; item.metadata = *metadata;
        item.description = QStringLiteral("当前快照未返回此参数的实际值，不能直接修改；请核对目标版本和采集权限。");
    }
    const QString title = item.metadata.title.isEmpty() ? item.name : item.metadata.title;
    const QString summary = DatabaseEnvironment::parameterSummary(item);
    const QString details = table == environmentComparison_ ? comparisonDescriptions_.value(item.name)
        : DatabaseEnvironment::parameterDetails(item);
    const QString recommendation = item.metadata.recommendation.isEmpty()
        ? QStringLiteral("请以当前达梦版本《配置文件说明》和实际负载验证结果为准。")
        : item.metadata.recommendation;
    const QString originalDescription = item.description.isEmpty()
        ? QStringLiteral("当前版本未返回原始说明") : item.description;
    const QString source = DatabaseEnvironment::parameterSourceDescription(item);
    const QString sourceSummary = item.metadata.source.trimmed().isEmpty()
        ? QStringLiteral("达梦 V$DM_INI（当前值与配置层级）")
        : item.metadata.source.section(QChar(0xFF1B), 0, 0).trimmed();
    const QString modification =
        DatabaseEnvironment::parameterModificationDescription(item);
    const QString modificationTemplate =
        DatabaseEnvironment::parameterModificationTemplate(item);
    const QString state = item.statusLabel();
    QString body = summary;
    if (!details.isEmpty() && details != summary) body += QStringLiteral("\n\n%1").arg(details);
    const auto rawValue = [](const QString& value) {
        return value.isEmpty() ? QStringLiteral("空值／未返回（待核对）") : value;
    };
    const QString informativeText = QStringLiteral(
        "状态：%1　分类：%2　变更方式：%3\n当前值：%4\n\n"
        "参数来源：%5\n\n修改方法：%6\n\n调整建议：%7")
        .arg(state, item.metadata.category,
            DatabaseEnvironment::effectiveChangePolicyLabel(item),
            observedParameterValue(item.metadata.format, item.currentValue),
            sourceSummary, modification, recommendation);
    const QString technicalDetails = QStringLiteral(
        "数据库原始说明：%1\n\n原始当前值：%2\n会话值：%3\n文件值：%4\n默认值：%5\n"
        "最小值：%6\n最大值：%7\n参数类型：%8\n原始模块：%9")
        .arg(originalDescription, rawValue(item.currentValue), rawValue(item.sessionValue),
             rawValue(item.fileValue), rawValue(item.defaultValue),
             rawValue(item.minimumValue), rawValue(item.maximumValue),
             rawValue(item.parameterType), rawValue(item.module))
        + QStringLiteral(
            "\n\n空值说明：当前驱动／查询结果无法区分数据库空值、空白值和未返回值，请以当前实例查询结果核对。"
            "\n\n来源明细：\n%1\n\n修改模板（仅复制，不会自动执行）：\n%2\n\n"
            "执行前检查：替换 <NEW_VALUE>；核对取值范围、DBA 权限、实例版本和重启要求；"
            "生产环境先备份并走变更审批。")
            .arg(source, modificationTemplate);
    const QString copyableDetails = QStringLiteral(
        "参数说明：%1（%2）\n\n%3\n\n%4\n\n技术明细：\n%5")
        .arg(title, item.name, body, informativeText, technicalDetails);
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("parameterDetailsDialog"));
    dialog.setWindowTitle(QStringLiteral("参数说明：%1（%2）").arg(title, item.name));
    dialog.setModal(true);
    dialog.setWindowFlags(dialog.windowFlags() | Qt::WindowCloseButtonHint);
    dialog.resize(820, 700);
    dialog.setMinimumSize(620, 460);

    auto* dialogLayout = new QVBoxLayout(&dialog);
    dialogLayout->setContentsMargins(20, 16, 20, 16);
    dialogLayout->setSpacing(12);
    auto* heading = new QWidget(&dialog);
    heading->setObjectName(QStringLiteral("parameterDetailsHeading"));
    auto* headingLayout = new QHBoxLayout(heading);
    headingLayout->setContentsMargins(0, 0, 0, 0);
    headingLayout->setSpacing(12);
    auto* iconLabel = new QLabel(heading);
    iconLabel->setObjectName(QStringLiteral("parameterDetailsIcon"));
    iconLabel->setPixmap(InterfaceIcons::icon(QStringLiteral(":/icons/about.svg")).pixmap(32, 32));
    iconLabel->setFixedSize(36, 36);
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setAccessibleName(QStringLiteral("参数说明"));
    headingLayout->addWidget(iconLabel);
    auto* titleLayout = new QVBoxLayout();
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(2);
    auto* titleLabel = new QLabel(title, heading);
    titleLabel->setObjectName(QStringLiteral("parameterDetailsTitle"));
    titleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titleLayout->addWidget(titleLabel);
    auto* nameLabel = new QLabel(
        QStringLiteral("%1 · %2").arg(item.name, item.metadata.category), heading);
    nameLabel->setObjectName(QStringLiteral("parameterDetailsSubtitle"));
    nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titleLayout->addWidget(nameLabel);
    headingLayout->addLayout(titleLayout, 1);
    dialogLayout->addWidget(heading);

    auto* contentScroll = new QScrollArea(&dialog);
    contentScroll->setObjectName(QStringLiteral("parameterDetailsScroll"));
    contentScroll->setWidgetResizable(true);
    contentScroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(contentScroll);
    content->setObjectName(QStringLiteral("parameterDetailsContent"));
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(4, 4, 12, 4);
    contentLayout->setSpacing(18);

    auto* summaryPanel = new QWidget(content);
    summaryPanel->setObjectName(QStringLiteral("parameterDetailsSummary"));
    auto* summaryLayout = new QGridLayout(summaryPanel);
    summaryLayout->setContentsMargins(12, 10, 12, 10);
    summaryLayout->setHorizontalSpacing(16);
    summaryLayout->setVerticalSpacing(6);
    const auto addMeta = [&summaryLayout](int rowIndex, const QString& label,
                                            const QString& value) {
        auto* keyLabel = new QLabel(label);
        keyLabel->setObjectName(QStringLiteral("parameterDetailsMetaLabel"));
        auto* valueLabel = new QLabel(value.isEmpty() ? QStringLiteral("—") : value);
        valueLabel->setObjectName(QStringLiteral("parameterDetailsMetaValue"));
        valueLabel->setWordWrap(true);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        summaryLayout->addWidget(keyLabel, rowIndex, 0);
        summaryLayout->addWidget(valueLabel, rowIndex, 1);
    };
    addMeta(0, QStringLiteral("状态"), state);
    addMeta(1, QStringLiteral("当前值"),
        observedParameterValue(item.metadata.format, item.currentValue));
    addMeta(2, QStringLiteral("变更方式"),
        DatabaseEnvironment::effectiveChangePolicyLabel(item));
    addMeta(3, QStringLiteral("参数来源"), sourceSummary);
    summaryLayout->setColumnStretch(1, 1);
    contentLayout->addWidget(summaryPanel);

    const auto addSection = [&contentLayout](const QString& headingText,
                                               const QString& text) {
        auto* section = new QWidget();
        section->setObjectName(QStringLiteral("parameterDetailsSection"));
        auto* sectionLayout = new QVBoxLayout(section);
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setSpacing(4);
        auto* headingLabel = new QLabel(headingText, section);
        headingLabel->setObjectName(QStringLiteral("parameterDetailsSectionHeading"));
        sectionLayout->addWidget(headingLabel);
        auto* textLabel = new QLabel(text, section);
        textLabel->setTextFormat(Qt::PlainText);
        textLabel->setObjectName(QStringLiteral("parameterDetailsBody"));
        textLabel->setWordWrap(true);
        textLabel->setTextInteractionFlags(
            Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        sectionLayout->addWidget(textLabel);
        contentLayout->addWidget(section);
    };
    addSection(QStringLiteral("参数说明"), body);
    addSection(QStringLiteral("修改方法"), modification);
    addSection(QStringLiteral("调整建议"), recommendation);

    auto* technicalPanel = new QWidget(content);
    technicalPanel->setObjectName(QStringLiteral("parameterDetailsTechnicalPanel"));
    auto* technicalLayout = new QVBoxLayout(technicalPanel);
    technicalLayout->setContentsMargins(0, 0, 0, 0);
    technicalLayout->setSpacing(4);
    auto* technicalHeading = new QLabel(QStringLiteral("技术明细"), technicalPanel);
    technicalHeading->setObjectName(QStringLiteral("parameterDetailsSectionHeading"));
    technicalLayout->addWidget(technicalHeading);
    auto* technicalGrid = new QGridLayout();
    technicalGrid->setHorizontalSpacing(16);
    technicalGrid->setVerticalSpacing(8);
    const QList<QPair<QString, QString>> technicalValues{
        {QStringLiteral("数据库原始说明"), originalDescription},
        {QStringLiteral("原始当前值"), rawValue(item.currentValue)},
        {QStringLiteral("会话值"), rawValue(item.sessionValue)},
        {QStringLiteral("文件值"), rawValue(item.fileValue)},
        {QStringLiteral("默认值"), rawValue(item.defaultValue)},
        {QStringLiteral("最小值"), rawValue(item.minimumValue)},
        {QStringLiteral("最大值"), rawValue(item.maximumValue)},
        {QStringLiteral("参数类型"), rawValue(item.parameterType)},
        {QStringLiteral("原始模块"), rawValue(item.module)}};
    for (int rowIndex = 0; rowIndex < technicalValues.size(); ++rowIndex) {
        auto* label = new QLabel(technicalValues.at(rowIndex).first, technicalPanel);
        label->setObjectName(QStringLiteral("parameterDetailsMetaLabel"));
        label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        auto* value = new QLabel(technicalValues.at(rowIndex).second, technicalPanel);
        value->setTextFormat(Qt::PlainText);
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        technicalGrid->addWidget(label, rowIndex, 0);
        technicalGrid->addWidget(value, rowIndex, 1);
    }
    technicalGrid->setColumnStretch(1, 1);
    technicalLayout->addLayout(technicalGrid);
    contentLayout->addWidget(technicalPanel);
    addSection(QStringLiteral("来源明细"), source);
    auto* templateHeading = new QLabel(QStringLiteral("查询／修改模板（仅复制，不自动执行）"), content);
    templateHeading->setObjectName(QStringLiteral("parameterDetailsSectionHeading"));
    contentLayout->addWidget(templateHeading);
    auto* technicalText = new QLabel(modificationTemplate, content);
    technicalText->setObjectName(QStringLiteral("parameterDetailsTechnical"));
    technicalText->setTextFormat(Qt::PlainText);
    technicalText->setFont(Theme::editorFont());
    technicalText->setWordWrap(true);
    technicalText->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    contentLayout->addWidget(technicalText);
    addSection(QStringLiteral("执行前检查"), QStringLiteral(
        "替换 <NEW_VALUE>；核对取值范围、DBA 权限、实例版本和重启要求；生产环境先备份并走变更审批。"));
    contentLayout->addStretch(1);
    contentScroll->setWidget(content);
    dialogLayout->addWidget(contentScroll, 1);

    auto* feedback = new QLabel(&dialog);
    feedback->setObjectName(QStringLiteral("parameterDetailsFeedback"));
    feedback->setAccessibleName(QStringLiteral("参数说明操作状态"));
    feedback->setWordWrap(true);
    dialogLayout->addWidget(feedback);

    auto* buttonBox = new QDialogButtonBox(&dialog);
    buttonBox->setObjectName(QStringLiteral("parameterDetailsButtonBox"));
    auto* okButton = buttonBox->addButton(QStringLiteral("关闭"), QDialogButtonBox::AcceptRole);
    okButton->setDefault(true);
    okButton->setProperty("role", "primary");
    okButton->setAccessibleName(QStringLiteral("关闭参数说明"));
    auto* copyDetails = buttonBox->addButton(
        QStringLiteral("复制参数说明"), QDialogButtonBox::ActionRole);
    copyDetails->setToolTip(QStringLiteral("复制当前弹窗中的完整参数说明"));
    copyDetails->setAccessibleName(QStringLiteral("复制完整参数说明"));
    auto* copyTemplate = buttonBox->addButton(
        QStringLiteral("复制查询／修改模板"), QDialogButtonBox::ActionRole);
    copyTemplate->setToolTip(QStringLiteral("复制参数来源查询和修改模板，不执行任何 SQL"));
    copyTemplate->setAccessibleName(QStringLiteral("复制参数查询和修改模板"));
    dialogLayout->addWidget(buttonBox);
    QObject::connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(copyDetails, &QPushButton::clicked, &dialog, [&, feedback]() {
        QApplication::clipboard()->setText(copyableDetails);
        feedback->setText(QStringLiteral("已复制 %1 的完整参数说明。").arg(item.name));
        parametersState_->setText(QStringLiteral("已复制 %1 的完整参数说明。").arg(item.name));
    });
    QObject::connect(copyTemplate, &QPushButton::clicked, &dialog, [&, feedback]() {
        QApplication::clipboard()->setText(modificationTemplate);
        feedback->setText(QStringLiteral("已复制 %1 的查询／修改模板；未执行 SQL。")
            .arg(item.name));
        parametersState_->setText(QStringLiteral("已复制 %1 的查询／修改模板；未执行 SQL。")
            .arg(item.name));
    });
    dialog.exec();
}

void DbaDocumentWidget::refreshComparisonProfiles() {
    if (comparisonProfile_ == nullptr) return;
    const QString selected = comparisonProfile_->currentData().toString();
    const QSignalBlocker blocker(comparisonProfile_);
    comparisonProfile_->clear();
    QString error;
    const QVector<ConnectionProfile> profiles = ConnectionStore().load(&error);
    for (const bool credentialed : {true, false}) {
        for (const ConnectionProfile& candidate : profiles) {
            if (candidate.id == profile_.id
                    || candidate.hasStoredCredential != credentialed) continue;
            QString label = QStringLiteral("%1（%2@%3:%4）")
                .arg(candidate.name, candidate.user, candidate.host).arg(candidate.port);
            if (!candidate.hasStoredCredential) label += QStringLiteral("（未保存凭据）");
            comparisonProfile_->addItem(label, candidate.id);
            const int index = comparisonProfile_->count() - 1;
            comparisonProfile_->setItemData(
                index, candidate.hasStoredCredential, Qt::UserRole + 1);
            comparisonProfile_->setItemData(index,
                candidate.hasStoredCredential
                    ? QStringLiteral("可使用系统凭据存储中的密码进行只读对比")
                    : QStringLiteral("未保存凭据；请先在连接管理中保存密码"),
                Qt::ToolTipRole);
        }
    }
    const int oldIndex = comparisonProfile_->findData(selected);
    if (oldIndex >= 0) comparisonProfile_->setCurrentIndex(oldIndex);
    comparisonCurrent_->setText(connectionDisplay());
    comparisonCurrent_->setToolTip(profile_.name.isEmpty()
        ? QStringLiteral("当前数据库尚未连接")
        : QStringLiteral("当前环境：%1（%2@%3:%4）")
            .arg(profile_.name, profile_.user, profile_.host).arg(profile_.port));
    comparisonProfile_->setToolTip(error.isEmpty()
        ? QStringLiteral("选择另一个连接；只读对比要求目标连接已保存凭据") : error);
    if (!error.isEmpty()) {
        comparisonState_->setText(QStringLiteral(
            "无法读取目标连接列表：%1。请检查连接配置后重试。").arg(error));
        updateComparisonAction(false);
        return;
    }
    updateComparisonAction();
}

void DbaDocumentWidget::updateComparisonAction(bool updateStatus) {
    if (comparisonProfile_ == nullptr || compareEnvironment_ == nullptr) return;
    const bool connected = state_ == SessionState::Connected;
    const bool snapshotReady = !environmentSnapshot_.isEmpty();
    const bool targetSelected = comparisonProfile_->currentIndex() >= 0
        && !comparisonProfile_->currentData().toString().isEmpty();
    const bool credentialReady = targetSelected
        && comparisonProfile_->currentData(Qt::UserRole + 1).toBool();
    const bool ready = !comparisonBusy_ && connected && snapshotReady
        && targetSelected && credentialReady;
    compareEnvironment_->setEnabled(ready);
    comparisonProfile_->setEnabled(!comparisonBusy_);
    comparisonMode_->setEnabled(!comparisonBusy_);
    refreshComparisonProfiles_->setEnabled(!comparisonBusy_);

    QString hint;
    if (comparisonBusy_) {
        hint = QStringLiteral("环境对比正在执行，请等待当前操作完成。");
    } else if (!connected) {
        hint = QStringLiteral("当前数据库连接不可用，请重新连接后再对比。");
    } else if (!snapshotReady) {
        hint = QStringLiteral("正在等待当前环境快照；也可以点击“刷新环境快照”重新读取。");
    } else if (!targetSelected) {
        hint = QStringLiteral("没有其他已保存连接，请先在连接管理中新增目标连接。");
    } else if (!credentialReady) {
        hint = QStringLiteral("目标连接未保存凭据，请先在连接管理中保存密码后重试。");
    } else {
        hint = QStringLiteral("将使用独立 Session 只读读取目标环境，不会修改任何数据库参数。");
    }
    compareEnvironment_->setToolTip(hint);
    if (updateStatus) comparisonState_->setText(hint);
}

void DbaDocumentWidget::renderEnvironmentComparison() {
    if (comparisonSnapshot_.isEmpty()) return;
    const bool standby = comparisonMode_->currentData().toBool();
    const auto differences = DatabaseEnvironment::compare(environmentSnapshot_, comparisonSnapshot_, standby);
    const QSignalBlocker blocker(environmentComparison_);
    environmentComparison_->setSortingEnabled(false);
    environmentComparison_->clear(); comparisonParameters_.clear(); comparisonDescriptions_.clear();
    environmentComparison_->setColumnCount(standby ? 8 : 7);
    QStringList headers{QStringLiteral("级别"), QStringLiteral("参数"), QStringLiteral("参数说明"), profile_.name,
        comparisonProfileValue_.name, QStringLiteral("变更方式"), QStringLiteral("判断")};
    if (standby) headers.append(QStringLiteral("主备同步规则"));
    environmentComparison_->setHorizontalHeaderLabels(headers);
    environmentComparison_->setRowCount(differences.size());
    int unequal = 0;
    for (int row = 0; row < differences.size(); ++row) {
        const auto& difference = differences[row];
        if (difference.judgement != QStringLiteral("当前值一致")) ++unequal;
        comparisonParameters_.insert(difference.name, difference.parameter);
        comparisonDescriptions_.insert(difference.name, difference.details);
        QStringList values{DatabaseEnvironment::severityLabel(difference.severity), difference.name, difference.description,
            difference.leftValue, difference.rightValue, difference.policy, difference.judgement};
        if (standby) values.append(difference.synchronization);
        for (int column = 0; column < values.size(); ++column) {
            auto* item = new QTableWidgetItem(values[column]);
            item->setToolTip(column == 2 || column == 7 ? difference.details : values[column]);
            if (column == 0) item->setData(Qt::UserRole, static_cast<int>(difference.severity));
            environmentComparison_->setItem(row, column, item);
        }
    }
    auto* header = environmentComparison_->horizontalHeader();
    header->setStretchLastSection(false);
    for (int column = 0; column < headers.size(); ++column) header->setSectionResizeMode(column, QHeaderView::Interactive);
    const int em = fontMetrics().horizontalAdvance(QStringLiteral("中"));
    const QVector<int> widths{4, 18, 26, 18, 18, 10, 20, 24};
    for (int column = 0; column < headers.size(); ++column) environmentComparison_->setColumnWidth(column, widths[column] * em);
    environmentComparison_->setWordWrap(false);
    environmentComparison_->setSortingEnabled(true); applyComparisonFilter();
    refreshParameterStars();
    comparisonState_->setText(QStringLiteral("%1完成：显示 %2 项，%3 项值或存在性差异。")
        .arg(standby ? QStringLiteral("主备规则对比") : QStringLiteral("环境对比")).arg(differences.size()).arg(unequal));
    if (standby) comparisonState_->setText(comparisonState_->text() + QStringLiteral(
        " 当前侧角色：%1，目标侧角色：%2。特殊项即使值一致也展示；普通同步读取 PRIMARY_SYNC，旧版本缺失则标为未知。"
        "角色信息不证明属于同一守护组；跨版本可能不同步，需结合归档、日志重演及 V$INI_SYNC_INFO 核对。")
        .arg(databaseIdentity_.instanceMode.isEmpty() ? QStringLiteral("未验证") : databaseIdentity_.instanceMode,
            comparisonIdentity_.instanceMode.isEmpty() ? QStringLiteral("未验证") : comparisonIdentity_.instanceMode));
    if (standby) {
        const QString left = databaseIdentity_.instanceMode.toUpper(), right = comparisonIdentity_.instanceMode.toUpper();
        if (!((left == QStringLiteral("PRIMARY") && right == QStringLiteral("STANDBY"))
                || (right == QStringLiteral("PRIMARY") && left == QStringLiteral("STANDBY"))))
            comparisonState_->setText(comparisonState_->text() + QStringLiteral(" 两侧未识别为一主一备，请核对选择的实际节点。"));
        if (!databaseIdentity_.oguid.isEmpty() && !comparisonIdentity_.oguid.isEmpty()
                && databaseIdentity_.oguid != comparisonIdentity_.oguid)
            comparisonState_->setText(comparisonState_->text() + QStringLiteral(" 两侧 OGUID 不同，不能认定为同一守护组。"));
    }
    comparisonDetails_->clear();
    if (!differences.isEmpty()) {
        environmentComparison_->setCurrentCell(0, 2);
        const auto* name = environmentComparison_->item(0, 1);
        comparisonDetails_->setPlainText(comparisonDescriptions_.value(name->text()));
    }
}

void DbaDocumentWidget::applyComparisonFilter() {
    const int severity = comparisonSeverityFilter_->currentData().toInt();
    int visible = 0;
    for (int row = 0; row < environmentComparison_->rowCount(); ++row) {
        const auto* level = environmentComparison_->item(row, 0);
        const bool show = severity < 0 || (level != nullptr
            && level->data(Qt::UserRole).isValid()
            && level->data(Qt::UserRole).toInt() == severity);
        environmentComparison_->setRowHidden(row, !show);
        if (show) ++visible;
    }
    comparisonFilterState_->setText(QStringLiteral("显示 %1／%2 项")
        .arg(visible).arg(environmentComparison_->rowCount()));
}

void DbaDocumentWidget::startEnvironmentComparison() {
    if (environmentSnapshot_.isEmpty()) {
        comparisonState_->setText(QStringLiteral(
            "当前环境快照尚未就绪，请点击“刷新环境快照”后重试。"));
        updateComparisonAction(false);
        return;
    }
    refreshComparisonProfiles();
    const QString profileId = comparisonProfile_->currentData().toString();
    const QVector<ConnectionProfile> profiles = ConnectionStore().load();
    const auto target = std::find_if(profiles.cbegin(), profiles.cend(),
        [&profileId](const ConnectionProfile& item) { return item.id == profileId; });
    if (target == profiles.cend()) {
        comparisonState_->setText(QStringLiteral(
            "目标连接不可用，请重新选择或在连接管理中保存连接。"));
        updateComparisonAction(false);
        return;
    }
    if (!target->hasStoredCredential) {
        comparisonState_->setText(QStringLiteral(
            "目标连接未保存凭据，请先在连接管理中保存密码后重试。"));
        updateComparisonAction(false);
        return;
    }
    CredentialStore& store = CredentialStore::systemStore();
    QString unavailable;
    if (!store.isAvailable(&unavailable)) {
        comparisonState_->setText(QStringLiteral(
            "无法读取系统凭据存储：%1。请修复凭据存储后重试。").arg(unavailable));
        updateComparisonAction(false);
        return;
    }
    CredentialResult credential = store.read(target->id);
    if (!credential.succeeded()) {
        comparisonState_->setText(QStringLiteral(
            "无法读取目标连接凭据：%1。请在连接管理中重新保存密码后重试。")
            .arg(credential.message));
        credential.password.fill('\0');
        updateComparisonAction(false);
        return;
    }
    comparisonCompatibilityAttempted_ = false;
    comparisonIdentity_ = {};
    comparisonCancelRequested_ = false;
    comparisonProfileValue_ = *target;
    comparisonBusy_ = true;
    comparisonState_->setText(QStringLiteral("正在连接目标环境 %1…").arg(target->name));
    updateComparisonAction(false);
    updateActions();
    QMetaObject::invokeMethod(comparisonSession_, "connectToDatabase", Qt::QueuedConnection,
        Q_ARG(ConnectionProfile, *target), Q_ARG(QByteArray, credential.password));
    credential.password.fill('\0');
}

void DbaDocumentWidget::filterTable(QTableWidget* table, const QString& filter) {
    const QString needle = filter.trimmed();
    for (int row = 0; row < table->rowCount(); ++row) {
        bool visible = needle.isEmpty();
        for (int column = 0; !visible && column < table->columnCount(); ++column) {
            if (const QTableWidgetItem* item = table->item(row, column)) {
                visible = item->text().contains(needle, Qt::CaseInsensitive);
            }
        }
        table->setRowHidden(row, !visible);
    }
}

void DbaDocumentWidget::populate(QTableWidget* table, const QueryBatch& batch, bool append) {
    const bool sorting = table->isSortingEnabled();
    table->setSortingEnabled(false);
    int firstRow = 0;
    if (!append) {
        table->clear();
        table->setColumnCount(batch.columns.size());
        table->setHorizontalHeaderLabels(batch.columns);
        table->setRowCount(0);
    } else if (table->columnCount() != batch.columns.size()) {
        return;
    }
    firstRow = table->rowCount();
    table->setRowCount(firstRow + batch.rows.size());
    for (int row = 0; row < batch.rows.size(); ++row) {
        for (int column = 0; column < batch.columns.size(); ++column) {
            table->setItem(firstRow + row, column,
                new QTableWidgetItem(batch.rows.at(row).value(column)));
        }
    }
    table->horizontalHeader()->setStretchLastSection(true);
    for (int column = 0; column < batch.columns.size(); ++column) {
        const int headerWidth = batch.columns.at(column).size() * 9 + 28;
        table->setColumnWidth(column, qBound(90, headerWidth, 240));
    }
    table->setSortingEnabled(sorting || !append);
    updateActions();
}

void DbaDocumentWidget::showRequestFailure(Request request, const QString& message) {
    const QString failure = message.trimmed().isEmpty()
        ? QStringLiteral("数据库请求失败") : message;
    const QString detail = request == Request::VerifyClosedSession
        ? QStringLiteral("%1；终止请求已经发送，但目标会话状态未知，请手工刷新确认。")
            .arg(failure)
        : QStringLiteral("%1；保留上次成功结果。").arg(failure);
    if (request == Request::VerifyClosedSession && verifyingSessionFromLocks_) {
        lockState_->setText(detail);
    } else if ((request == Request::ValidateSession
            || request == Request::CancelSessionOperation
            || request == Request::CloseSession) && pendingActionFromLocks_) {
        lockState_->setText(detail);
    } else if (request == Request::Sessions || request == Request::FullSessionSql
            || request == Request::ValidateSession
            || request == Request::CancelSessionOperation
            || request == Request::CloseSession
            || request == Request::VerifyClosedSession) {
        sessionsState_->setText(detail);
    } else if (request == Request::LocksDetailed || request == Request::Locks) {
        lockState_->setText(detail);
        if (lockRecords_.isEmpty()) {
            lockEmptyTitle_->setText(QStringLiteral("锁等待查询失败"));
            lockEmptyDetail_->setText(QStringLiteral("%1\n本次未取得新快照，不能据此判断没有等待；请重试。").arg(failure));
        }
    } else if (request == Request::Counts) {
        trend_->setText(detail);
    } else if (request == Request::SlowSql || request == Request::SlowSqlFallback) {
        slowSource_->setText(detail);
    } else if (request == Request::Tablespaces || request == Request::DataFiles) {
        tablespaceState_->setText(detail);
    } else if (request == Request::Users || request == Request::Roles
            || request == Request::RolePrivileges
            || request == Request::RoleMembers
            || request == Request::Privileges) {
        usersState_->setText(detail);
    } else if (request == Request::Jobs || request == Request::JobSchedules
            || request == Request::JobHistory) {
        jobsState_->setText(detail);
    } else if (request == Request::Parameters) {
        if (looksLikePermissionDenied(message)) {
            parametersState_->setText(QStringLiteral(
                "没有权限读取 V$DM_INI；保留上次成功结果。"));
            QMessageBox prompt(QMessageBox::Warning, QStringLiteral("没有权限"),
                QStringLiteral("当前用户没有读取 V$DM_INI 的权限。"),
                QMessageBox::NoButton, this);
            prompt.setInformativeText(QStringLiteral(
                "请切换到有权限的连接，或联系数据库管理员授予查询权限。"));
            prompt.setDetailedText(message);
            prompt.addButton(QStringLiteral("关闭"), QMessageBox::RejectRole);
            prompt.exec();
        } else {
            parametersState_->setText(detail);
        }
    } else if (request == Request::DatabaseVersion) {
        databaseVersionNotice_ = QStringLiteral("数据库版本信息读取失败");
        databaseOptionsState_->setText(QStringLiteral(
            "%1；将继续读取 V$OPTION，现有概览保持不变。错误：%2")
            .arg(databaseVersionNotice_, failure));
    } else if (request == Request::DatabaseOptions) {
        databaseOptionsState_->setText(looksLikePermissionDenied(message)
            ? QStringLiteral(
                "没有权限读取 V$OPTION；请切换连接或联系管理员授权，已保留上次成功结果。")
            : QStringLiteral("V$OPTION 读取失败：%1；已保留上次成功结果。")
                .arg(failure));
    }
}

QString DbaDocumentWidget::selectedSessionContext(qint64 sessionId) const {
    const QString transaction = selectedSessionValue(QStringLiteral("TRX_ID")).trimmed();
    return QStringLiteral(
        "连接：%1｜环境：%2\n"
        "会话：%3｜序列：%4｜用户：%5｜状态：%6｜事务：%7\n"
        "客户端：%8／%9｜应用：%10")
        .arg(connectionDisplay(), connectionEnvironmentLabel(profile_.environment))
        .arg(sessionId)
        .arg(selectedSessionValue(QStringLiteral("SESS_SEQ")),
            selectedSessionValue(QStringLiteral("USER_NAME")),
            selectedSessionValue(QStringLiteral("STATE")),
            transaction.isEmpty() || transaction == QStringLiteral("0")
                ? QStringLiteral("无") : transaction,
            DbaDiagnostics::normalizeClientIp(
                selectedSessionValue(QStringLiteral("CLNT_IP"))),
            selectedSessionValue(QStringLiteral("CLNT_HOST")),
            selectedSessionValue(QStringLiteral("APPNAME")));
}

QString DbaDocumentWidget::selectedSessionValue(const QString& name) const {
    const int row = sessions_->currentRow();
    return row < 0 ? QString() : sessionValue(row, name);
}

int DbaDocumentWidget::sessionColumn(const QString& name) const {
    for (int column = 0; column < sessions_->columnCount(); ++column) {
        const QTableWidgetItem* header = sessions_->horizontalHeaderItem(column);
        if (header != nullptr && (header->text() == name
                || header->data(Qt::UserRole).toString() == name)) return column;
    }
    return -1;
}

QString DbaDocumentWidget::sessionValue(int row, const QString& name) const {
    const int column = sessionColumn(name);
    if (row < 0 || column < 0) return {};
    const QTableWidgetItem* item = sessions_->item(row, column);
    return item == nullptr ? QString() : item->text();
}

qint64 DbaDocumentWidget::selectedSessionNumber(const QString& name) const {
    bool valid = false;
    const qint64 value = selectedSessionValue(name).toLongLong(&valid);
    return valid ? value : 0;
}

QString DbaDocumentWidget::selectedSessionKey() const {
    const qint64 id = selectedSessionNumber(QStringLiteral("SESS_ID"));
    const qint64 sequence = selectedSessionNumber(QStringLiteral("SESS_SEQ"));
    return id > 0 && sequence > 0
        ? QStringLiteral("%1:%2").arg(id).arg(sequence) : QString();
}

void DbaDocumentWidget::updateSessionDetails() {
    if (sessions_->currentRow() < 0) {
        sessionDetailTabs_->hide();
        sessionDetails_->setText(QStringLiteral("选择一个会话查看详情。"));
        sessionSql_->clear();
        sessionSql_->setPlaceholderText(QStringLiteral("选择会话后点击“完整 SQL”。"));
        sessionLockDetails_->setText(QStringLiteral("选择会话后查看事务和锁诊断入口。"));
        return;
    }
    sessionDetailTabs_->show();
    const QString transactionId = selectedSessionValue(QStringLiteral("TRX_ID"));
    const QString transaction = DbaDiagnostics::hasTransaction(transactionId)
        ? transactionId : QStringLiteral("无事务");
    const QString originalIp = selectedSessionValue(QStringLiteral("CLNT_IP"));
    const QString shownIp = DbaDiagnostics::normalizeClientIp(originalIp);
    sessionDetails_->setText(QStringLiteral(
        "身份\n"
        "  会话：%1／序列 %2    用户：%3    Schema：%4\n"
        "  状态：%5    风险：%6    事务：%7\n\n"
        "客户端\n"
        "  IP：%8    主机：%9    类型：%10\n"
        "  应用：%11    系统：%12\n\n"
        "活动\n"
        "  当前／最近操作：%13    空闲时长：%14    连接时长：%15\n"
        "  连接时间：%16    最近活动：%17")
        .arg(selectedSessionValue(QStringLiteral("SESS_ID")),
            selectedSessionValue(QStringLiteral("SESS_SEQ")),
            selectedSessionValue(QStringLiteral("USER_NAME")),
            selectedSessionValue(QStringLiteral("CURR_SCH")),
            selectedSessionValue(QStringLiteral("STATE")),
            selectedSessionValue(QStringLiteral("RISK")), transaction, shownIp,
            selectedSessionValue(QStringLiteral("CLNT_HOST")),
            selectedSessionValue(QStringLiteral("CLNT_TYPE")),
            selectedSessionValue(QStringLiteral("APPNAME")),
            selectedSessionValue(QStringLiteral("OSNAME")),
            selectedSessionValue(QStringLiteral("OPERATION")),
            selectedSessionValue(QStringLiteral("IDLE_FOR")),
            selectedSessionValue(QStringLiteral("CONNECTED_FOR")),
            selectedSessionValue(QStringLiteral("CREATE_TIME")),
            selectedSessionValue(QStringLiteral("LAST_RECV_TIME"))));
    sessionSql_->setPlainText(selectedSessionValue(QStringLiteral("SQL_TEXT")));
    sessionSql_->setPlaceholderText(QStringLiteral("当前会话没有可显示的 SQL。"));
    sessionLockDetails_->setText(DbaDiagnostics::hasTransaction(transactionId)
        ? QStringLiteral("事务 %1 当前仍存在。可打开锁等待分析，确认该事务是否正在等待或阻塞其他会话。")
            .arg(transactionId)
        : QStringLiteral("当前快照未显示活动事务。仍可打开锁等待分析查看全库等待关系。"));
}

void DbaDocumentWidget::refreshSessionFilterValues() {
    const QString selected = sessionUserFilter_->currentText();
    QStringList users;
    const int column = sessionColumn(QStringLiteral("USER_NAME"));
    for (int row = 0; row < sessions_->rowCount(); ++row) {
        if (column >= 0 && sessions_->item(row, column) != nullptr) {
            const QString user = sessions_->item(row, column)->text().trimmed();
            if (!user.isEmpty() && !users.contains(user, Qt::CaseInsensitive)) users.append(user);
        }
    }
    users.sort(Qt::CaseInsensitive);
    sessionUserFilter_->blockSignals(true);
    sessionUserFilter_->clear();
    sessionUserFilter_->addItem(QStringLiteral("全部用户"));
    sessionUserFilter_->addItems(users);
    const int index = sessionUserFilter_->findText(selected, Qt::MatchFixedString);
    sessionUserFilter_->setCurrentIndex(index >= 0 ? index : 0);
    sessionUserFilter_->blockSignals(false);
}

void DbaDocumentWidget::applySessionFilters() {
    auto value = [this](int row, int col) {
        return col >= 0 && sessions_->item(row, col) != nullptr
            ? sessions_->item(row, col)->text() : QString();
    };
    const int userColumn = sessionColumn(QStringLiteral("USER_NAME"));
    const int stateColumn = sessionColumn(QStringLiteral("STATE"));
    const int ipColumn = sessionColumn(QStringLiteral("CLNT_IP"));
    const int appColumn = sessionColumn(QStringLiteral("APPNAME"));
    const int portColumn = sessionColumn(QStringLiteral("PORT_TYPE"));
    const int transactionColumn = sessionColumn(QStringLiteral("TRX_ID"));
    const int changeColumn = sessionColumn(QStringLiteral("CHANGE"));
    const QString user = sessionUserFilter_->currentIndex() > 0
        ? sessionUserFilter_->currentText() : QString();
    const QString state = sessionStateFilter_->currentIndex() > 0
        ? sessionStateFilter_->currentText() : QString();
    const QString preset = sessionPresetFilter_->currentData().toString();
    int visible = 0;
    for (int row = 0; row < sessions_->rowCount(); ++row) {
        const QString rowUser = value(row, userColumn);
        const QString rowState = value(row, stateColumn);
        const QString rowIp = value(row, ipColumn);
        const QString rowApp = value(row, appColumn);
        const bool transaction = DbaDiagnostics::hasTransaction(
            value(row, transactionColumn));
        const bool presetMatches = preset == QStringLiteral("all")
            || (preset == QStringLiteral("active")
                && (rowState == QStringLiteral("ACTIVE")
                    || rowState == QStringLiteral("PENDING")))
            || (preset == QStringLiteral("transaction") && transaction)
            || (preset == QStringLiteral("idleTransaction")
                && rowState == QStringLiteral("IDLE") && transaction)
            || (preset == QStringLiteral("changed")
                && value(row, changeColumn) != QStringLiteral("—"))
            || (preset == QStringLiteral("client")
                && DbaDiagnostics::isGzydDmToolsApplication(rowApp))
            || (preset == QStringLiteral("currentUser")
                && rowUser.compare(profile_.user, Qt::CaseInsensitive) == 0);
        bool show = (user.isEmpty() || rowUser.compare(user, Qt::CaseInsensitive) == 0)
            && (state.isEmpty() || rowState.compare(state, Qt::CaseInsensitive) == 0)
            && rowIp.contains(sessionIpFilter_->text().trimmed(), Qt::CaseInsensitive)
            && rowApp.contains(sessionAppFilter_->text().trimmed(), Qt::CaseInsensitive)
            && presetMatches
            && (!userSessionsOnly_->isChecked()
                || (!rowUser.trimmed().isEmpty() && value(row, portColumn) != QStringLiteral("12")))
            && (!currentUserOnly_->isChecked()
                || rowUser.compare(profile_.user, Qt::CaseInsensitive) == 0)
            && (!hideClientSessions_->isChecked()
                || !DbaDiagnostics::isGzydDmToolsApplication(rowApp));
        sessions_->setRowHidden(row, !show);
        if (show) ++visible;
    }
    updateSessionSummary();
    if (request_ == Request::None || request_ == Request::Sessions) {
        const QString changes = compareSessionSnapshot_
            ? QStringLiteral("｜本次新增 %1、变化 %2、结束 %3。")
                .arg(sessionNewCount_).arg(sessionChangedCount_).arg(sessionEndedCount_)
            : QString();
        sessionsState_->setText(sessions_->rowCount() == 0
            ? QStringLiteral("当前快照无会话。%1").arg(changes)
            : QStringLiteral("显示 %1／%2 个会话。%3")
                .arg(visible).arg(sessions_->rowCount()).arg(changes));
    }
}

void DbaDocumentWidget::updateSessionSummary() {
    int active = 0;
    int transactions = 0;
    int idleTransactions = 0;
    int client = 0;
    for (int row = 0; row < sessions_->rowCount(); ++row) {
        const QString state = sessionValue(row, QStringLiteral("STATE"));
        const bool transaction = DbaDiagnostics::hasTransaction(
            sessionValue(row, QStringLiteral("TRX_ID")));
        if (state == QStringLiteral("ACTIVE") || state == QStringLiteral("PENDING")) {
            ++active;
        }
        if (transaction) ++transactions;
        if (transaction && state == QStringLiteral("IDLE")) ++idleTransactions;
        if (DbaDiagnostics::isGzydDmToolsApplication(
                sessionValue(row, QStringLiteral("APPNAME")))) {
            ++client;
        }
    }
    sessionSummary_->setText(QStringLiteral(
        "<a href=\"all\">总会话 %1</a>｜<a href=\"active\">活动 %2</a>｜"
        "<a href=\"transaction\">有事务 %3</a>｜"
        "<a href=\"idleTransaction\">空闲有事务 %4</a>｜"
        "<a href=\"client\">本客户端 %5</a>｜"
        "<a href=\"changed\">本次变化 %6</a>%7")
        .arg(sessions_->rowCount()).arg(active).arg(transactions)
        .arg(idleTransactions).arg(client)
        .arg(sessionNewCount_ + sessionChangedCount_)
        .arg(sessionLastRefresh_.isEmpty()
            ? QString() : QStringLiteral("｜更新 %1").arg(sessionLastRefresh_)));
}

void DbaDocumentWidget::commitSessionSnapshot() {
    if (pendingSessionColumns_.isEmpty()) {
        pendingSessionRows_.clear();
        sessionsState_->setText(QStringLiteral(
            "会话查询未返回可识别的列结构；保留上次成功快照。"));
        return;
    }
    QueryBatch raw;
    raw.query = true;
    raw.columns = pendingSessionColumns_;
    raw.rows = pendingSessionRows_;
    const QueryBatch presented = presentSessionBatch(raw);
    populate(sessions_, presented, false);

    const QHash<QString, QString> sessionHeaders = {
        {QStringLiteral("RISK"), QStringLiteral("风险")},
        {QStringLiteral("CHANGE"), QStringLiteral("变化")},
        {QStringLiteral("SESS_ID"), QStringLiteral("会话")},
        {QStringLiteral("USER_NAME"), QStringLiteral("用户")},
        {QStringLiteral("STATE"), QStringLiteral("状态")},
        {QStringLiteral("TRANSACTION"), QStringLiteral("事务")},
        {QStringLiteral("OPERATION"), QStringLiteral("当前／最近操作")},
        {QStringLiteral("SQL_TEXT"), QStringLiteral("SQL 摘要")},
        {QStringLiteral("CLIENT"), QStringLiteral("客户端")},
        {QStringLiteral("APPNAME"), QStringLiteral("应用")},
        {QStringLiteral("IDLE_FOR"), QStringLiteral("空闲时长")},
        {QStringLiteral("CONNECTED_FOR"), QStringLiteral("连接时长")},
    };
    for (int column = 0; column < presented.columns.size(); ++column) {
        if (QTableWidgetItem* header = sessions_->horizontalHeaderItem(column)) {
            const QString name = presented.columns.at(column);
            header->setData(Qt::UserRole, name);
            if (sessionHeaders.contains(name)) header->setText(sessionHeaders.value(name));
        }
    }

    const QStringList required = {QStringLiteral("SESS_ID"), QStringLiteral("SESS_SEQ"),
        QStringLiteral("USER_NAME"), QStringLiteral("CURR_SCH"),
        QStringLiteral("STATE"), QStringLiteral("CLNT_IP"), QStringLiteral("CLNT_HOST"),
        QStringLiteral("APPNAME"), QStringLiteral("SQL_TEXT"),
        QStringLiteral("CREATE_TIME"), QStringLiteral("LAST_RECV_TIME"),
        QStringLiteral("TRX_ID"), QStringLiteral("IS_SELF")};
    sessionMissingFields_.clear();
    for (const QString& name : required) {
        if (!raw.columns.contains(name)) sessionMissingFields_.append(name);
    }
    sessionSnapshotIdentityAvailable_ = !sessionMissingFields_.contains(
        QStringLiteral("SESS_ID")) && !sessionMissingFields_.contains(
        QStringLiteral("SESS_SEQ"));

    refreshSessionFilterValues();
    const QStringList defaultHidden = {QStringLiteral("CHANGE"),
        QStringLiteral("OPERATION"), QStringLiteral("CONNECTED_FOR"),
        QStringLiteral("APPNAME"), QStringLiteral("SESSION_MARK"),
        QStringLiteral("CLNT_IP"), QStringLiteral("CLNT_HOST"),
        QStringLiteral("CREATE_TIME"), QStringLiteral("LAST_RECV_TIME"),
        QStringLiteral("TRX_ID"), QStringLiteral("SESS_SEQ"),
        QStringLiteral("CURR_SCH"), QStringLiteral("CLNT_TYPE"),
        QStringLiteral("OSNAME"), QStringLiteral("CONN_TYPE"),
        QStringLiteral("PORT_TYPE"), QStringLiteral("CONNECTED"),
        QStringLiteral("IS_SELF"), QStringLiteral("RISK_ORDER")};
    for (int column = 0; column < presented.columns.size(); ++column) {
        const QString name = presented.columns.at(column);
        const bool internal = name == QStringLiteral("IS_SELF")
            || name == QStringLiteral("CONNECTED")
            || name == QStringLiteral("RISK_ORDER");
        const bool shown = sessionColumnVisibility_.contains(name)
            ? sessionColumnVisibility_.value(name) : !defaultHidden.contains(name);
        sessions_->setColumnHidden(column, internal || !shown);
    }

    const int riskColumn = presented.columns.indexOf(QStringLiteral("RISK"));
    const int changeColumn = presented.columns.indexOf(QStringLiteral("CHANGE"));
    const int sqlColumn = presented.columns.indexOf(QStringLiteral("SQL_TEXT"));
    const int sessionColumnIndex = presented.columns.indexOf(QStringLiteral("SESS_ID"));
    for (int row = 0; row < sessions_->rowCount(); ++row) {
        if (riskColumn >= 0) {
            QTableWidgetItem* riskItem = sessions_->item(row, riskColumn);
            if (riskItem != nullptr) {
                const bool danger = riskItem->text() == QStringLiteral("空闲有事务");
                const bool warning = riskItem->text() == QStringLiteral("活动事务")
                    || riskItem->text() == QStringLiteral("等待中");
                if (danger || warning) {
                    riskItem->setForeground(Theme::semanticColor(danger
                        ? Theme::SemanticColor::Danger : Theme::SemanticColor::Warning,
                        palette()));
                }
            }
        }
        if (changeColumn >= 0 && sessions_->item(row, changeColumn) != nullptr) {
            QTableWidgetItem* changeItem = sessions_->item(row, changeColumn);
            if (changeItem->text() != QStringLiteral("—")) {
                changeItem->setForeground(Theme::semanticColor(
                    Theme::SemanticColor::Accent, palette()));
            }
        }
        for (int column = 0; column < sessions_->columnCount(); ++column) {
            if (column >= 0 && sessions_->item(row, column) != nullptr) {
                sessions_->item(row, column)->setToolTip(
                    sessions_->item(row, column)->text());
            }
        }
    }
    if (sessionColumnIndex >= 0) sessions_->setColumnWidth(sessionColumnIndex, 150);
    for (const QString& name : {QStringLiteral("RISK"), QStringLiteral("USER_NAME"),
            QStringLiteral("STATE"), QStringLiteral("TRANSACTION"), QStringLiteral("IDLE_FOR")}) {
        const int column = presented.columns.indexOf(name);
        if (column >= 0) sessions_->setColumnWidth(column,
            name == QStringLiteral("STATE") ? 76 : 100);
    }
    if (sqlColumn >= 0) {
        sessions_->setColumnWidth(sqlColumn, 280);
        sessions_->horizontalHeader()->setSectionResizeMode(sqlColumn, QHeaderView::Stretch);
    }
    const int clientColumn = presented.columns.indexOf(QStringLiteral("CLIENT"));
    if (clientColumn >= 0) sessions_->setColumnWidth(clientColumn, 190);
    if (!sessionDefaultSortApplied_) {
        const int riskOrderColumn = presented.columns.indexOf(QStringLiteral("RISK_ORDER"));
        if (riskOrderColumn >= 0) {
            sessions_->sortItems(riskOrderColumn, Qt::AscendingOrder);
            sessionDefaultSortApplied_ = true;
        }
    }
    pendingSessionColumns_.clear();
    pendingSessionRows_.clear();
    finalizeSessionSnapshot();
    restoreSessionContext();
    applyPendingSessionActionOutcome(false);
}

void DbaDocumentWidget::finalizeSessionSnapshot() {
    sessionEndedCount_ = 0;
    if (compareSessionSnapshot_ && sessionSnapshotIdentityAvailable_) {
        for (auto it = previousSessionSnapshot_.cbegin();
                it != previousSessionSnapshot_.cend(); ++it) {
            if (!pendingSessionKeys_.contains(it.key())) ++sessionEndedCount_;
        }
    }
    if (sessionSnapshotIdentityAvailable_) {
        previousSessionSnapshot_ = pendingSessionSnapshot_;
        hasSessionSnapshot_ = true;
    } else {
        compareSessionSnapshot_ = false;
        sessionNewCount_ = 0;
        sessionChangedCount_ = 0;
    }
    sessionLastRefresh_ = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    applySessionFilters();
    if (!sessionMissingFields_.isEmpty()) {
        sessionsState_->setText(QStringLiteral(
            "字段不兼容，缺少：%1；变化比较和相关操作可能不可用。")
            .arg(sessionMissingFields_.join(QStringLiteral("、"))));
    }
}

void DbaDocumentWidget::applyPendingSessionActionOutcome(bool lockPage) {
    if (pendingSessionActionOutcome_.isEmpty()
            || pendingSessionOutcomeForLocks_ != lockPage) return;
    (lockPage ? lockState_ : sessionsState_)->setText(pendingSessionActionOutcome_);
    pendingSessionActionOutcome_.clear();
    pendingSessionOutcomeForLocks_ = false;
}

void DbaDocumentWidget::restoreSessionContext() {
    if (sessionSortColumn_ >= 0 && sessionSortColumn_ < sessions_->columnCount()) {
        sessions_->sortItems(sessionSortColumn_, sessionSortOrder_);
    }
    if (!sessionSelectionKey_.isEmpty()) {
        const int idColumn = sessionColumn(QStringLiteral("SESS_ID"));
        const int sequenceColumn = sessionColumn(QStringLiteral("SESS_SEQ"));
        for (int row = 0; row < sessions_->rowCount(); ++row) {
            if (sessions_->isRowHidden(row)) continue;
            if (idColumn < 0 || sequenceColumn < 0
                    || sessions_->item(row, idColumn) == nullptr
                    || sessions_->item(row, sequenceColumn) == nullptr) continue;
            const QString key = QStringLiteral("%1:%2")
                .arg(sessions_->item(row, idColumn)->text(),
                    sessions_->item(row, sequenceColumn)->text());
            if (key == sessionSelectionKey_) {
                sessions_->setCurrentCell(row, idColumn);
                break;
            }
        }
    }
    sessions_->verticalScrollBar()->setValue(sessionScrollValue_);
}

QString DbaDocumentWidget::selectedRole() const {
    const int row = users_->currentRow();
    if (row < 0) return {};
    for (int column = 0; column < users_->columnCount(); ++column) {
        const QTableWidgetItem* header = users_->horizontalHeaderItem(column);
        if (header != nullptr && header->text() == QStringLiteral("ROLE")) {
            const QTableWidgetItem* item = users_->item(row, column);
            return item == nullptr ? QString() : item->text();
        }
    }
    return {};
}

QString DbaDocumentWidget::selectedUser() const {
    const int row = users_ == nullptr ? -1 : users_->currentRow();
    if (row < 0) return {};
    for (int column = 0; column < users_->columnCount(); ++column) {
        const QTableWidgetItem* header = users_->horizontalHeaderItem(column);
        if (header != nullptr && header->text() == QStringLiteral("USERNAME")) {
            const QTableWidgetItem* item = users_->item(row, column);
            return item == nullptr ? QString() : item->text();
        }
    }
    return {};
}

void DbaDocumentWidget::updateActions() {
    const ConnectionProfile shownProfile =
        pendingProfile_.id.isEmpty() ? profile_ : pendingProfile_;
    connectionSelector_->setText(shownProfile.name.isEmpty()
        ? QStringLiteral("未连接")
        : QStringLiteral("%1 [%2]")
            .arg(shownProfile.name, connectionEnvironmentLabel(shownProfile.environment)));
    connectionSelector_->setToolTip(shownProfile.name.isEmpty()
        ? QStringLiteral("当前管理窗口未选择连接")
        : QStringLiteral("连接：%1\n用户：%2\n数据库：%3\n环境：%4\n状态：%5")
            .arg(shownProfile.name, shownProfile.user,
                shownProfile.database.isEmpty() ? QStringLiteral("—") : shownProfile.database,
                connectionEnvironmentLabel(shownProfile.environment),
                state_ == SessionState::Connected ? QStringLiteral("已连接")
                                                   : QStringLiteral("未连接")));
    const bool idle = state_ == SessionState::Connected && request_ == Request::None
        && verifyingSessionId_ <= 0;
    refreshSessions_->setEnabled(idle);
    refreshLocks_->setEnabled(idle);
    fitLockGraph_->setEnabled(!lockRecords_.isEmpty());
    refreshPerformance_->setEnabled(idle);
    refreshSlowSql_->setEnabled(idle);
    refreshTablespaces_->setEnabled(idle);
    refreshDataFiles_->setEnabled(idle);
    refreshUsers_->setEnabled(idle);
    createUser_->setEnabled(idle && !shownProfile.readOnly);
    const bool roleSelected =
        kind_ != DbaDocumentKind::Roles || !selectedRole().isEmpty();
    refreshPrivileges_->setEnabled(idle && roleSelected);
    if (kind_ == DbaDocumentKind::Roles || kind_ == DbaDocumentKind::Users) {
        users_->setEnabled(idle);
        const int page = kind_ == DbaDocumentKind::Roles ? rolePage_ : userPage_;
        const bool hasNext = kind_ == DbaDocumentKind::Roles
            ? roleHasNext_ : userHasNext_;
        rolesPrevious_->setEnabled(idle && page > 0);
        rolesNext_->setEnabled(idle && hasNext);
        rolesPageSize_->setEnabled(idle);
    }
    if (kind_ == DbaDocumentKind::Roles) {
        rolePrivilegesPrevious_->setEnabled(
            idle && roleSelected && rolePrivilegePage_ > 0);
        rolePrivilegesNext_->setEnabled(
            idle && roleSelected && rolePrivilegeHasNext_);
        roleMembersPrevious_->setEnabled(
            idle && roleSelected && roleMemberPage_ > 0);
        roleMembersNext_->setEnabled(
            idle && roleSelected && roleMemberHasNext_);
        rolePrivilegesPageSize_->setEnabled(idle && roleSelected);
        roleMembersPageSize_->setEnabled(idle && roleSelected);
    }
    refreshJobs_->setEnabled(idle);
    refreshJobSchedules_->setEnabled(idle);
    refreshJobHistory_->setEnabled(idle);
    refreshParameters_->setEnabled(idle && !comparisonBusy_);
    cancelRequest_->setEnabled(comparisonBusy_ || (request_ != Request::None
        && state_ != SessionState::Cancelling && state_ != SessionState::Disconnected));
    if (kind_ == DbaDocumentKind::Parameters) {
        const bool loading = request_ != Request::None || comparisonBusy_;
        const bool stopping = state_ == SessionState::Cancelling;
        cancelRequest_->setText(stopping ? QStringLiteral("正在停止…")
            : loading ? QStringLiteral("停止") : QStringLiteral("刷新"));
        cancelRequest_->setEnabled(!stopping && (loading || idle));
        cancelRequest_->setToolTip(parametersState_->text());
        context_->setToolTip(QStringLiteral("独立 Session；%1").arg(parametersState_->text()));
        const QString notice = parametersState_->text();
        const bool important = notice.contains(QStringLiteral("失败"))
            || notice.contains(QStringLiteral("不完整")) || notice.contains(QStringLiteral("权限"));
        context_->setText(stopping ? QStringLiteral("正在停止…")
            : loading ? QStringLiteral("正在刷新…")
            : important ? notice : QString());
    }
    bool validTarget = false;
    bool cancellableTarget = false;
    const int row = sessions_->currentRow();
    const int idColumn = sessionColumn(QStringLiteral("SESS_ID"));
    const int selfColumn = sessionColumn(QStringLiteral("IS_SELF"));
    if (row >= 0 && idColumn >= 0 && selfColumn >= 0) {
        const QTableWidgetItem* idItem = sessions_->item(row, idColumn);
        const QTableWidgetItem* selfItem = sessions_->item(row, selfColumn);
        bool parsed = false;
        const qint64 id = idItem == nullptr ? 0 : idItem->text().toLongLong(&parsed);
        validTarget = parsed && id > 0 && selfItem != nullptr
            && selfItem->text() != QStringLiteral("1");
        const QString state = selectedSessionValue(QStringLiteral("STATE"));
        cancellableTarget = state == QStringLiteral("ACTIVE")
            || state == QStringLiteral("PENDING");
    }
    const bool writableTarget = idle && validTarget && !shownProfile.readOnly;
    const bool readableTarget = idle && row >= 0 && idColumn >= 0;
    loadSessionSql_->setEnabled(idle && row >= 0 && idColumn >= 0);
    openSessionLocks_->setEnabled(idle);
    cancelSessionOperation_->setEnabled(writableTarget && cancellableTarget);
    closeSession_->setEnabled(writableTarget);
    copySessionSummary_->setEnabled(readableTarget);
    sessionActionMenu_->setEnabled(readableTarget);
    sessionActionMenu_->setToolTip(!idle
        ? QStringLiteral("等待当前请求完成后操作会话。")
        : !readableTarget ? QStringLiteral("请选择一个会话。")
        : shownProfile.readOnly ? QStringLiteral("可复制诊断摘要；只读连接不能取消或终止会话。")
        : !validTarget ? QStringLiteral("可复制诊断摘要；当前管理会话不能取消或终止。")
        : QStringLiteral("复制诊断摘要，或取消／终止选中会话。"));
    const bool hasSessionSql = !sessionSql_->toPlainText().trimmed().isEmpty();
    copySessionSql_->setEnabled(hasSessionSql);
    openSessionSql_->setEnabled(hasSessionSql);
    const LockWaitRecord* lockRecord = selectedLockRecord();
    const bool hasLockRecord = lockRecord != nullptr;
    copyLockDetails_->setEnabled(hasLockRecord);
    openBlockerSql_->setEnabled(hasLockRecord
        && !lockRecord->blockerSql.trimmed().isEmpty());
    openWaiterSql_->setEnabled(hasLockRecord
        && !lockRecord->waiterSql.trimmed().isEmpty());
    const bool validBlocker = hasLockRecord
        && lockRecord->blockerSessionId > 0
        && lockRecord->blockerSessionSeq > 0
        && lockRecord->blockerTransactionId > 0
        && lockRecord->waiterTransactionId > 0
        && !lockRecord->blockerIsSelf;
    const bool validWaiter = hasLockRecord
        && lockRecord->waiterSessionId > 0
        && lockRecord->waiterSessionSeq > 0
        && lockRecord->blockerTransactionId > 0
        && lockRecord->waiterTransactionId > 0
        && !lockRecord->waiterIsSelf;
    const bool writableLockTarget = idle && !shownProfile.readOnly;
    const bool blockerActive = hasLockRecord
        && (lockRecord->blockerState == QStringLiteral("ACTIVE")
            || lockRecord->blockerState == QStringLiteral("PENDING"));
    cancelBlockerOperation_->setEnabled(writableLockTarget
        && validBlocker && blockerActive);
    killBlocker_->setEnabled(writableLockTarget && validBlocker);
    killWaiter_->setEnabled(writableLockTarget && validWaiter);
}
