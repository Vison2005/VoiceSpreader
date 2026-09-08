#pragma once

#include "audio_device.h"
#include "audio_engine.h"
#include "bluetooth_audio_receiver.h"
#include "latency_calibrator.h"
#include "phone_pairing_server.h"

#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QVector>

class LevelMeterWidget;
class QBoxLayout;
class QCloseEvent;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTextEdit;
class QToolButton;
class QSystemTrayIcon;
class QVBoxLayout;
class QResizeEvent;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void refreshDevices();
    void rebuildOutputCards();
    void startAudio();
    void stopAudio();
    void appendStatus(const QString& message);
    void showEngineError(const QString& message);
    void updateRunningState(bool running);
    void updateSelectionSummary();
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
    void showBluetoothAudioReceiver();
    void updatePhoneConnection(bool connected, const QString& phoneName);
    void updatePhoneMicrophoneStreaming(bool enabled);
    void updatePhoneMicrophoneLevel(double levelDbfs);
    void updateBluetoothConnection(bool connected, const QString& deviceName);
    void minimizeToTray();
    void restoreFromTray();
    void setAutoStartEnabled(bool enabled);

private:
    AudioDevice currentCaptureDevice() const;
    AudioDevice currentMicrophoneDevice() const;
    QVector<OutputDeviceSettings> selectedOutputDevices() const;
    void setControlsEnabled(bool enabled);
    void applyTheme();
    void updatePhoneButtonAppearance();
    void updateBluetoothButtonAppearance();
    void initializeSystemTray();
    void updateAutoStartButtonAppearance();
    bool isAutoStartEnabled() const;
    void updateResponsiveLayout();

    AudioEngine engine_;
    LatencyCalibrator calibrator_;
    PhonePairingServer phonePairingServer_;
    BluetoothAudioReceiver bluetoothAudioReceiver_;
    QVector<AudioDevice> devices_;
    QVector<AudioDevice> microphoneDevices_;

    QComboBox* captureCombo_ = nullptr;
    QComboBox* microphoneCombo_ = nullptr;
    QVBoxLayout* outputCardsLayout_ = nullptr;
    QSlider* bufferSlider_ = nullptr;
    QSpinBox* bufferSpin_ = nullptr;
    QPushButton* automaticLatencyCheck_ = nullptr;
    QPushButton* continuousAcousticCheck_ = nullptr;
    QPushButton* exclusiveModeCheck_ = nullptr;
    QLabel* selectionLabel_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* calibrateButton_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* minimizeToTrayButton_ = nullptr;
    QPushButton* autoStartButton_ = nullptr;
    QToolButton* themeButton_ = nullptr;
    QToolButton* phoneMicrophoneButton_ = nullptr;
    QToolButton* bluetoothButton_ = nullptr;
    LevelMeterWidget* phoneLevelMeter_ = nullptr;
    QTextEdit* logView_ = nullptr;
    QLabel* calibrationHintLabel_ = nullptr;
    QLabel* programLevelLabel_ = nullptr;
    QBoxLayout* contentLayout_ = nullptr;
    QBoxLayout* sourceControlsLayout_ = nullptr;
    QBoxLayout* calibrationControlsLayout_ = nullptr;
    QWidget* rightColumn_ = nullptr;
    QSystemTrayIcon* trayIcon_ = nullptr;

    QHash<QString, QPushButton*> outputChecks_;
    QHash<QString, QSlider*> volumeSliders_;
    QHash<QString, QLabel*> volumeLabels_;
    QHash<QString, QSpinBox*> delaySpinBoxes_;
    QHash<QString, QLabel*> acousticLabels_;
    QSet<QString> lockedOutputIds_;
    QString connectedPhoneName_;
    double phoneMicrophoneLevelDbfs_ = -160.0;
    int phoneConnectionState_ = 0;
    bool phoneMicrophoneActive_ = false;
    QString connectedBluetoothDeviceName_;
    int bluetoothConnectionState_ = 0;
    bool bluetoothBusy_ = false;
    bool darkTheme_ = false;
    bool compactLayout_ = false;
    bool quitting_ = false;
    bool trayMessageShown_ = false;
};
