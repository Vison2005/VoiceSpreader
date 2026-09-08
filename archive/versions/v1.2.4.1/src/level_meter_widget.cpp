#include "level_meter_widget.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace
{
constexpr double minimumVisibleDbfs = -60.0;
constexpr double silentThresholdDbfs = -119.0;
}

LevelMeterWidget::LevelMeterWidget(QWidget* parent)
    : QWidget(parent)
    , animationTimer_(new QTimer(this))
{
    setObjectName(QStringLiteral("PhoneLevelMeter"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMinimumSize(minimumSizeHint());
    setAccessibleName(QStringLiteral("手机麦克风实时电平"));
    animationTimer_->setInterval(16);
    connect(animationTimer_, &QTimer::timeout, this,
            [this] { advanceAnimation(); });
}

void LevelMeterWidget::setLevelDbfs(double levelDbfs)
{
    hasSample_ = levelDbfs > -150.0;
    targetLevelDbfs_ = levelDbfs <= silentThresholdDbfs
                           ? minimumVisibleDbfs
                           : std::clamp(levelDbfs, minimumVisibleDbfs, 0.0);
    if (!animationTimer_->isActive()) {
        animationTimer_->start();
    }
    update();
}

void LevelMeterWidget::setConnected(bool connected)
{
    if (connected_ == connected) {
        return;
    }
    connected_ = connected;
    if (!connected_) {
        hasSample_ = false;
        targetLevelDbfs_ = minimumVisibleDbfs;
        displayedLevelDbfs_ = minimumVisibleDbfs;
        animationTimer_->stop();
    }
    update();
}

void LevelMeterWidget::setDarkTheme(bool darkTheme)
{
    if (darkTheme_ == darkTheme) {
        return;
    }
    darkTheme_ = darkTheme;
    update();
}

QSize LevelMeterWidget::sizeHint() const
{
    return QSize(158, 38);
}

QSize LevelMeterWidget::minimumSizeHint() const
{
    return QSize(142, 38);
}

void LevelMeterWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF track = QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75);
    const qreal radius = std::min<qreal>(10.0, track.height() / 2.0);
    QPainterPath trackPath;
    trackPath.addRoundedRect(track, radius, radius);

    const QColor trackColor(darkTheme_ ? QStringLiteral("#10284E")
                                       : QStringLiteral("#EEF5FC"));
    const QColor outlineColor(darkTheme_ ? QStringLiteral("#315377")
                                         : QStringLiteral("#AACCD6"));
    const QColor fillColor(darkTheme_ ? QStringLiteral("#4382DF")
                                      : QStringLiteral("#4647AE"));
    const QColor textColor(darkTheme_ ? QStringLiteral("#DCEAF2")
                                      : QStringLiteral("#314668"));

    painter.fillPath(trackPath, trackColor);
    painter.setPen(QPen(outlineColor, 1.0));
    painter.drawPath(trackPath);

    const double normalized = connected_ && hasSample_
                                  ? std::clamp(
                                        (displayedLevelDbfs_ - minimumVisibleDbfs)
                                            / -minimumVisibleDbfs,
                                        0.0,
                                        1.0)
                                  : 0.0;
    QPainterPath fillClip;
    fillClip.addRect(QRectF(track.left(), track.top(),
                            track.width() * normalized, track.height()));
    const QPainterPath fillPath = trackPath.intersected(fillClip);
    painter.fillPath(fillPath, fillColor);

    QFont meterFont = font();
    meterFont.setPointSizeF(8.5);
    meterFont.setWeight(QFont::DemiBold);
    meterFont.setHintingPreference(QFont::PreferFullHinting);
    painter.setFont(meterFont);

    const QString text = displayText();
    painter.setPen(textColor);
    painter.drawText(track, Qt::AlignCenter, text);
    if (!fillPath.isEmpty()) {
        painter.save();
        painter.setClipPath(fillPath);
        painter.setPen(Qt::white);
        painter.drawText(track, Qt::AlignCenter, text);
        painter.restore();
    }
}

void LevelMeterWidget::advanceAnimation()
{
    const double difference = targetLevelDbfs_ - displayedLevelDbfs_;
    if (std::abs(difference) < 0.05) {
        displayedLevelDbfs_ = targetLevelDbfs_;
        animationTimer_->stop();
        update();
        return;
    }

    // 电平上升时快速响应，下降时缓慢释放，避免画面跳动。
    const double smoothing = difference > 0.0 ? 0.46 : 0.10;
    displayedLevelDbfs_ += difference * smoothing;
    update();
}

QString LevelMeterWidget::displayText() const
{
    if (!connected_) {
        return QStringLiteral("手机电平  未连接");
    }
    if (!hasSample_) {
        return QStringLiteral("手机电平  等待采样");
    }
    if (targetLevelDbfs_ <= minimumVisibleDbfs
        && displayedLevelDbfs_ <= minimumVisibleDbfs + 0.5) {
        return QStringLiteral("手机电平  −∞ dBFS");
    }
    return QStringLiteral("手机电平  %1 dBFS")
        .arg(displayedLevelDbfs_, 0, 'f', 1);
}
