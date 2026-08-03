#pragma once

#include "audio_device.h"
#include "audio_engine.h"
#include "latency_calibrator.h"
#include "phone_pairing_server.h"

#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTextEdit;
class QToolButton;
class QVBoxLayout;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void refreshDevices();
    void rebuildOutputCards();
    void startAudio();
    void stopAudio();
    void appendStatus(const QString& message);
    void showEngineError(const QString& message);
    void updateRunningState(bool running);
    void updateSelectionSummary();
    void updateSourceHint();
    void toggleTheme();
    void startOrStopCalibration();
    void applyCalibrationResults();
    void updateCalibrationRunningState(bool running);
    void showCalibrationError(const QString& message);
    void updateAcousticCorrection(const QString& deviceId,
                                  int delayMilliseconds,
                                  double driftPpm,
                                  const QString& probeMode,
                                  double confidence);
    void updateProgramLevel(double levelDbfs, bool probeAllowed);
    void showPhonePairing();
    void updatePhoneConnection(bool connected, const QString& phoneName);
    void updatePhoneMicrophoneLevel(double levelDbfs);

private:
    AudioDevice currentCaptureDevice() const;
    AudioDevice currentMicrophoneDevice() const;
    QVector<OutputDeviceSettings> selectedOutputDevices() const;
    void setControlsEnabled(bool enabled);
    void applyTheme();

    AudioEngine engine_;
    LatencyCalibrator calibrator_;
    PhonePairingServer phonePairingServer_;
    QVector<AudioDevice> devices_;
    QVector<AudioDevice> microphoneDevices_;

    QComboBox* captureCombo_ = nullptr;
    QComboBox* microphoneCombo_ = nullptr;
    QVBoxLayout* outputCardsLayout_ = nullptr;
    QSlider* bufferSlider_ = nullptr;
    QSpinBox* bufferSpin_ = nullptr;
    QCheckBox* automaticLatencyCheck_ = nullptr;
    QCheckBox* continuousAcousticCheck_ = nullptr;
    QCheckBox* exclusiveModeCheck_ = nullptr;
    QLabel* selectionLabel_ = nullptr;
    QLabel* sourceHintLabel_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* calibrateButton_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QToolButton* themeButton_ = nullptr;
    QTextEdit* logView_ = nullptr;
    QLabel* calibrationHintLabel_ = nullptr;
    QLabel* programLevelLabel_ = nullptr;
    QLabel* phoneStatusLabel_ = nullptr;
    QLabel* phoneLevelLabel_ = nullptr;
    QPushButton* phonePairButton_ = nullptr;
    QCheckBox* usePhoneMicrophoneCheck_ = nullptr;

    QHash<QString, QCheckBox*> outputChecks_;
    QHash<QString, QSlider*> volumeSliders_;
    QHash<QString, QLabel*> volumeLabels_;
    QHash<QString, QSpinBox*> delaySpinBoxes_;
    QHash<QString, QLabel*> acousticLabels_;
    QSet<QString> lockedOutputIds_;
    bool darkTheme_ = false;
};
