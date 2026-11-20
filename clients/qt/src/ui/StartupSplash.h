#pragma once

#include "Theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QDialog>
#include <QFrame>
#include <QFontMetricsF>
#include <QGraphicsOpacityEffect>
#include <QHideEvent>
#include <QIcon>
#include <QHeaderView>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QScreen>
#include <QScrollBar>
#include <QPushButton>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QShortcut>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QTimer>
#include <QTreeWidget>
#include <QWindow>
#include <cmath>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

// Same convergence / ignition / burst rhythm as reid-cli's WelcomeAnimation,
// rendered with Qt's existing raster painter for Windows and bundled Wine.
class StartupSplash final : public QDialog {
public:
    static constexpr int introMilliseconds = 2500;
    static constexpr int frameMilliseconds = 33;
    static constexpr int readyPlaybackMilliseconds = 3000;

    explicit StartupSplash(const QString& version)
        : QDialog(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint), version_(version), motion_(!reduceMotion()),
          logo_(QIcon(QStringLiteral(":/icons/app.svg")).pixmap(QSize(96, 96), devicePixelRatioF())) {
        setObjectName(QStringLiteral("startupSplash"));
        resize(canvas().size());
        Theme::applyStartupAppearance(*this);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_QuitOnClose, false);
        setWindowIcon(QIcon(QStringLiteral(":/icons/app.svg")));
        setWindowTitle(QStringLiteral("小智达梦管理工具"));
        setAccessibleName(QStringLiteral("小智达梦管理工具启动画面"));
        setAccessibleDescription(QStringLiteral("正在初始化工作台"));
        timer_.setParent(this);
        readyHold_.setParent(this);
        timer_.setInterval(frameMilliseconds);
        timer_.setObjectName(QStringLiteral("startupAnimationTimer"));
        connect(&timer_, &QTimer::timeout, this, [this]() { update(); });
        readyHold_.setObjectName(QStringLiteral("startupReadyHoldTimer"));
        readyHold_.setSingleShot(true);
        readyHold_.setTimerType(Qt::PreciseTimer);
        readyHold_.setInterval(readyPlaybackMilliseconds);
        connect(&readyHold_, &QTimer::timeout, this, [this]() {
            if (!recoveryActive_) close();
        });
        dismissButton_ = new QPushButton(QStringLiteral("关闭"), this);
        dismissButton_->setObjectName(QStringLiteral("startupDismiss"));
        dismissButton_->setProperty("role", QStringLiteral("secondary"));
        dismissButton_->setAutoDefault(false);
        dismissButton_->setAccessibleName(QStringLiteral("关闭启动画面"));
        dismissButton_->setToolTip(QStringLiteral("关闭启动画面（Esc），继续使用工作台"));
        connect(dismissButton_, &QPushButton::clicked, this, &QWidget::close);
        dismissShortcut_ = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        dismissShortcut_->setObjectName(QStringLiteral("startupDismissShortcut"));
        dismissShortcut_->setContext(Qt::ApplicationShortcut);
        dismissShortcut_->setEnabled(false);
        connect(dismissShortcut_, &QShortcut::activated, this, &QWidget::close);
    }

    void workspaceReady(QWidget* workspace) {
        if (!isVisible() || ready_ || !workspace) return;
        ready_ = true;
        workspace_ = workspace;
        workspace->installEventFilter(this);
        connect(workspace, &QObject::destroyed, this, &QWidget::close);
        // Keep the initial stacking and animation timeline. Raising the splash
        // after MainWindow::show() can reveal it a second time on Windows/Wine.
        setAccessibleDescription(QStringLiteral("工作台就绪，动画继续播放 3 秒，可提前关闭"));
        update();
        // Start the three seconds once the workspace can actually be seen.
        // No finish(), sleep or nested event loop delays workspace interaction.
        if (workspace->windowHandle() && workspace->windowHandle()->isExposed()) readyHold_.start();
    }
    bool animationEnabled() const { return motion_; }
    bool animationRunning() const { return timer_.isActive(); }
    bool wasDismissed() const { return dismissed_; }

    // Reuse the recovery controls and result handling in this same native window.
    // Only a recovery decision is modal; ordinary startup remains non-modal.
    int execRecovery(QDialog& source) {
        if (isVisible() || dismissed_ || !source.layout()) return source.exec();
        readyHold_.stop();
        recoveryActive_ = true;
        // Modality must be established before the first native show. Calling
        // exec() on an already visible non-modal dialog does not register it
        // as the active modal widget on every Qt platform plugin.
        setWindowModality(Qt::ApplicationModal);
        setAccessibleDescription(QStringLiteral("选择要恢复的工作实例，或暂不恢复进入工作台"));
        dismissButton_->setAccessibleName(QStringLiteral("跳过启动恢复"));
        dismissButton_->setToolTip(QStringLiteral("暂不恢复（Esc），保留历史快照并进入工作台"));
        auto* outer = new QVBoxLayout(this);
        outer->setSizeConstraint(QLayout::SetNoConstraint);
        outer->setContentsMargins(20, 112, 20, 20);
        auto* panel = new QFrame(this);
        panel->setObjectName(QStringLiteral("startupRecoveryPanel"));
        panel->setFont(source.font());
        panel->setLayout(source.layout());
        auto* history = panel->findChild<QTreeWidget*>(QStringLiteral("workspaceHistoryList"));
        if (history) {
            history->setSelectionBehavior(QAbstractItemView::SelectRows);
            history->setAllColumnsShowFocus(true);
            history->setAlternatingRowColors(false);
            history->setUniformRowHeights(true);
            history->header()->setHighlightSections(false);
            history->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
            history->header()->setStretchLastSection(true);
            history->setFrameShape(QFrame::NoFrame);
            history->ensurePolished();
            const int rowHeight = qMax(20, history->fontMetrics().height()) + 16;
            const int headerHeight = history->header()->sizeHint().height();
            // Show one to five rows, then scroll. A single snapshot should not
            // leave the same empty area as a long recovery history.
            history->setMinimumHeight(headerHeight + rowHeight + 2);
            history->setMaximumHeight(headerHeight
                + qBound(1, history->topLevelItemCount(), 5) * rowHeight
                + history->horizontalScrollBar()->sizeHint().height() + 2);
            panel->setFocusProxy(history);
        }
        outer->addWidget(panel);
        auto* opacity = new QGraphicsOpacityEffect(panel);
        opacity->setOpacity(0);
        panel->setGraphicsEffect(opacity);
        panel->setEnabled(false);
        panel->show();

        const QRect room = (screen() ? screen()->availableGeometry() : QRect(0, 0, 1024, 768))
            .adjusted(16, 16, -16, -16);
        QRect initial(QPoint(), size());
        initial.moveCenter(room.center());
        setGeometry(initial);
        panel->ensurePolished();
        const QSize finalSize = QSize(760, qMax(360, panel->sizeHint().height() + 132))
            .boundedTo(room.size());
        QRect target(QPoint(), finalSize);
        target.moveCenter(initial.center());
        target.moveLeft(qBound(room.left(), target.left(), room.right() - target.width() + 1));
        target.moveTop(qBound(room.top(), target.top(), room.bottom() - target.height() + 1));
        QVariantAnimation transition(this);
        transition.setObjectName(QStringLiteral("startupRecoveryTransition"));
        // Converge the particles, move the brand and reveal recovery together.
        // Do not wait out the intro and then play a second expansion animation.
        transition.setDuration(motion_ ? introMilliseconds : 0);
        transition.setEasingCurve(QEasingCurve::InOutCubic);
        transition.setStartValue(0.0);
        transition.setEndValue(1.0);
        connect(&transition, &QVariantAnimation::valueChanged, this, [this, initial, target, opacity](const QVariant& value) {
            recoveryProgress_ = value.toReal();
            const auto mix = [this](int from, int to) { return qRound(from + (to - from) * recoveryProgress_); };
            setGeometry(mix(initial.x(), target.x()), mix(initial.y(), target.y()),
                mix(initial.width(), target.width()), mix(initial.height(), target.height()));
            opacity->setOpacity(qBound<qreal>(0, (recoveryProgress_ - 0.35) / 0.65, 1));
            update();
        });
        connect(&transition, &QVariantAnimation::finished, panel, [panel]() {
            panel->setEnabled(true);
            panel->setGraphicsEffect(nullptr);
            panel->setFocus(Qt::OtherFocusReason);
        });
        const auto finish = connect(&source, &QDialog::finished, this, &QDialog::done);
        transition.start();
        const int decision = exec();
        disconnect(finish);
        transition.stop();
        source.setLayout(panel->layout());
        source.setResult(decision);
        delete outer;
        delete panel;
        recoveryActive_ = false;
        recoveryProgress_ = 0;
        return decision;
    }

    // Deterministic offscreen frames for visual regression, with no time sleeps.
    static QImage frameImage(const QSize& size, qreal seconds, const QString& version,
            bool motion = true, bool ready = false, qreal recovery = 0) {
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        drawScene(painter, size, seconds, version, motion, ready,
            QIcon(QStringLiteral(":/icons/app.svg")).pixmap(96, 96), recovery);
        return image;
    }

protected:
    void showEvent(QShowEvent* event) override {
        QDialog::showEvent(event);
        ready_ = false;
        setAccessibleDescription(recoveryActive_
            ? QStringLiteral("选择要恢复的工作实例，或暂不恢复进入工作台")
            : QStringLiteral("正在初始化工作台"));
        clock_.start();
        dismissShortcut_->setEnabled(true);
        positionDismissButton();
        if (motion_) timer_.start();
    }
    void hideEvent(QHideEvent* event) override {
        dismissed_ = true;
        timer_.stop();
        readyHold_.stop();
        dismissShortcut_->setEnabled(false);
        if (workspace_) workspace_->removeEventFilter(this);
        workspace_.clear();
        QDialog::hideEvent(event);
    }
    void resizeEvent(QResizeEvent* event) override {
        QDialog::resizeEvent(event);
        positionDismissButton();
    }
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == workspace_ && event->type() == QEvent::Paint
                && ready_ && !recoveryActive_ && isVisible() && !readyHold_.isActive()) readyHold_.start();
        return QDialog::eventFilter(watched, event);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        drawScene(painter, size(), clock_.isValid() ? clock_.elapsed() / 1000.0 : 0,
            version_, motion_, ready_, logo_, recoveryProgress_);
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (!recoveryActive_) close();
        else QDialog::mousePressEvent(event);
    }

private:
    QString version_;
    bool motion_ = true, ready_ = false;
    bool recoveryActive_ = false;
    bool dismissed_ = false;
    qreal recoveryProgress_ = 0;
    QElapsedTimer clock_;
    QTimer timer_;
    QTimer readyHold_;
    QPointer<QWidget> workspace_;
    QPushButton* dismissButton_ = nullptr;
    QShortcut* dismissShortcut_ = nullptr;
    QPixmap logo_;

    void positionDismissButton() {
        if (!dismissButton_) return;
        dismissButton_->adjustSize();
        dismissButton_->move(qMax(0, width() - dismissButton_->width() - 16), 16);
    }

    static QPixmap canvas() {
        QSize size(680, 400);
        if (auto* screen = QGuiApplication::primaryScreen()) {
            const QSize room = screen->availableGeometry().size() - QSize(32, 48);
            size.scale(room, Qt::KeepAspectRatio);
            size = size.boundedTo(QSize(680, 400));
        }
        QPixmap pixmap(size);
        pixmap.fill(Qt::transparent);
        return pixmap;
    }
    static bool reduceMotion() {
        const auto geometry = QJsonDocument::fromJson(qgetenv("REID_MACOS_DISPLAY_GEOMETRY")).object();
        if (geometry.value(QStringLiteral("reducedMotion")).toBool()) return true;
#ifdef Q_OS_WIN
        BOOL enabled = TRUE;
        if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) && !enabled) return true;
#endif
        return false;
    }
    static qreal fade(qreal time, qreal start, qreal duration) {
        const qreal x = qBound<qreal>(0, (time - start) / duration, 1);
        return 1 - std::pow(1 - x, 3);
    }
    static void drawScene(QPainter& p, const QSizeF& size, qreal seconds, const QString& version,
            bool motion, bool ready, const QPixmap& logo, qreal recovery = 0) {
        constexpr qreal pi = 3.14159265358979323846;
        // Stretch the original 1.65-second scene to 2.5 seconds. Keep the
        // repaint cadence and ready hold independent of the animation speed.
        constexpr qreal sceneIntroSeconds = 1.65;
        constexpr qreal playbackRate = sceneIntroSeconds * 1000 / introMilliseconds;
        const qreal t = motion ? qMax<qreal>(0, seconds) * playbackRate : sceneIntroSeconds;
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(size.width() / 680, size.height() / 400);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(1, 1, 678, 398), 20, 20);
        p.setClipPath(clip);
        QLinearGradient night(0, 0, 680, 400);
        night.setColorAt(0, QColor("#09162C"));
        night.setColorAt(0.5, QColor("#11152D"));
        night.setColorAt(1, QColor("#071D29"));
        p.fillRect(QRectF(0, 0, 680, 400), night);
        const qreal drift = motion ? t : 0;
        for (int band = 0; band < 4; ++band) {
            QPainterPath ribbon;
            const auto y = [=](qreal x) {
                return 100 + band * 32 + 42 * std::sin(x / 150 + drift * 0.42 + band * 0.9)
                    + 18 * std::sin(x / 74 - drift * 0.23 + band);
            };
            ribbon.moveTo(-20, y(-20));
            for (int x = -10; x <= 700; x += 10) ribbon.lineTo(x, y(x));
            for (int x = 700; x >= -20; x -= 10) ribbon.lineTo(x, y(x) + 42 + band * 8);
            ribbon.closeSubpath();
            QLinearGradient aurora(0, 70, 680, 240);
            aurora.setColorAt(0, QColor(38, 232, 203, 0));
            aurora.setColorAt(0.25, QColor(42, 225, 212, 28 + band * 4));
            aurora.setColorAt(0.65, QColor(125, 94, 245, 38));
            aurora.setColorAt(1, QColor(91, 131, 255, 0));
            p.fillPath(ribbon, aurora);
        }
        p.setPen(QPen(QColor(117, 188, 228, 12), 0.6));
        for (int x = 28; x < 680; x += 32) p.drawLine(x, 20, x, 310);
        for (int y = 22; y < 310; y += 32) p.drawLine(20, y, 660, y);
        const QPointF center(340, 140);
        const qreal aspectCorrection = size.width() * 400 / (size.height() * 680);
        p.save();
        const QPointF markCenter = center + (QPointF(44, 44 * aspectCorrection) - center) * recovery;
        p.translate(markCenter);
        const qreal markScale = 1 - 0.67 * recovery;
        p.scale(markScale, markScale * (1 + (aspectCorrection - 1) * recovery));
        p.translate(-center);
        QRadialGradient glow(center, 148);
        glow.setColorAt(0, QColor(81, 185, 235, 72));
        glow.setColorAt(0.45, QColor(83, 99, 213, 36));
        glow.setColorAt(1, QColor(40, 99, 173, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        p.drawEllipse(center, 148, 148);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(105, 218, 242, 64), 1));
        p.drawArc(QRectF(254, 54, 172, 172), int((drift * 18 + 20) * 16), 235 * 16);
        p.setPen(QPen(QColor(167, 130, 255, 55), 1));
        p.drawArc(QRectF(248, 48, 184, 184), int((145 - drift * 13) * 16), 95 * 16);
        if (motion) for (int index = 0; index < 48; ++index) {
            const qreal delay = (index % 5) * 0.018;
            const qreal progress = (t - delay) / 1.35;
            if (progress <= 0) continue;
            const bool intro = progress < 1;
            const qreal angle = index * 2 * pi / 48 + (intro
                ? 0.12 * std::sin(progress * pi) : drift * (0.22 + (index % 3) * 0.03));
            const qreal radius = intro ? (progress < 0.7
                ? (180 + (index * 47) % 220) * (1 - std::pow(progress / 0.7, 2.4))
                : 90 * fade(progress, 0.7, 0.3))
                : 96 + (index % 4) * 12 + 8 * std::sin(drift * 0.8 + index * 0.6);
            QColor color = index % 3 == 0 ? QColor(159, 128, 255) : QColor(80, 235, 222);
            color.setAlphaF(intro
                ? qBound<qreal>(0, qMin(progress / 0.2, (1 - progress) / 0.28) * 0.85, 0.85)
                : fade(t, 1.35 + delay, 0.6) * (0.38 + 0.16 * std::sin(drift + index)));
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            const QPointF point = center + QPointF(std::cos(angle) * radius, std::sin(angle) * radius);
            const qreal dot = 1 + index % 3 * 0.5;
            p.drawEllipse(point, dot, dot);
        }
        p.setOpacity(motion ? fade(t, 0.32, 0.55) : 1);
        p.drawPixmap(QRect(292, 92, 96, 96), logo);
        p.restore();
        // Expanding the window changes its aspect ratio. Keep the brand mark
        // and header lettering proportional as the recovery panel grows below.
        p.save();
        p.scale(1, 1 + (aspectCorrection - 1) * recovery);
        p.setOpacity(motion ? fade(t, 0.58, 0.55) : 1);
        p.setPen(QColor("#F0F6FF"));
        QFont title = p.font();
        title.setPixelSize(qRound(30 - 10 * recovery));
        title.setWeight(QFont::DemiBold);
        p.setFont(title);
        const QString product = QStringLiteral("小智达梦管理工具");
        const qreal initialTitleX = (680 - QFontMetricsF(title).horizontalAdvance(product)) / 2;
        p.drawText(QRectF(initialTitleX + (84 - initialTitleX) * recovery,
            230 - 208 * recovery, 540, 42 - 12 * recovery), Qt::AlignLeft | Qt::AlignVCenter, product);
        QFont detail = title;
        detail.setPixelSize(14);
        detail.setWeight(QFont::Normal);
        p.setFont(detail);
        p.setPen(QColor("#9DB5CD"));
        const QString subtitle = QStringLiteral("达梦数据库工作台");
        const qreal initialDetailX = (680 - QFontMetricsF(detail).horizontalAdvance(subtitle)) / 2;
        const QRectF detailRect(initialDetailX + (84 - initialDetailX) * recovery,
            276 - 228 * recovery, 540, 26);
        p.setOpacity((motion ? fade(t, 0.58, 0.55) : 1) * (1 - recovery));
        p.drawText(detailRect, Qt::AlignLeft | Qt::AlignVCenter, subtitle);
        p.setOpacity(recovery);
        p.drawText(detailRect, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("恢复工作实例 · 版本 ") + version);
        p.restore();
        p.setOpacity(1 - recovery);
        detail.setPixelSize(12);
        p.setFont(detail);
        p.setPen(QColor("#9DB5CD"));
        p.drawText(QRectF(32, 341, 440, 22), Qt::AlignLeft | Qt::AlignVCenter,
            ready ? QStringLiteral("工作台就绪") : QStringLiteral("正在初始化工作台…"));
        p.drawText(QRectF(470, 341, 178, 22), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("版本 ") + version);
        p.setOpacity(1);
        p.setClipping(false);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(119, 173, 220, 95), 1));
        p.drawRoundedRect(QRectF(1, 1, 678, 398), 20, 20);
        p.restore();
    }
};
