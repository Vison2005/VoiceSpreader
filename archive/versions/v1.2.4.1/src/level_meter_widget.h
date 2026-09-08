#pragma once

#include <QWidget>

class QPaintEvent;
class QTimer;

class LevelMeterWidget final : public QWidget
{
public:
    explicit LevelMeterWidget(QWidget* parent = nullptr);

    void setLevelDbfs(double levelDbfs);
    void setConnected(bool connected);
    void setDarkTheme(bool darkTheme);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void advanceAnimation();
    QString displayText() const;

    QTimer* animationTimer_ = nullptr;
    double targetLevelDbfs_ = -60.0;
    double displayedLevelDbfs_ = -60.0;
    bool hasSample_ = false;
    bool connected_ = false;
    bool darkTheme_ = false;
};
