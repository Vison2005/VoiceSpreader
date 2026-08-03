#include "main_window.h"

#include "pairing_qr_code.h"
#include "wasapi_device_manager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QPixmap>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
QLabel* createSectionTitle(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("SectionTitle"));
    return label;
}

void refreshDynamicStyle(QWidget* widget)
{
    if (widget == nullptr) {
        return;
    }
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , engine_(this)
    , calibrator_(this)
    , phonePairingServer_(this)
{
    setWindowTitle(QStringLiteral("VoiceSpreader - 多设备音频同步"));
    setMinimumSize(980, 680);
    resize(1120, 760);

    auto* root = new QWidget(this);
    root->setObjectName(QStringLiteral("Root"));
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(24, 20, 24, 20);
    rootLayout->setSpacing(16);

    auto* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(12);
    auto* brandLayout = new QVBoxLayout();
    brandLayout->setSpacing(2);

    auto* eyebrow = new QLabel(QStringLiteral("LOCAL AUDIO ROUTER"), root);
    eyebrow->setObjectName(QStringLiteral("Eyebrow"));
    auto* title = new QLabel(QStringLiteral("VoiceSpreader"), root);
    title->setObjectName(QStringLiteral("AppTitle"));
    auto* subtitle = new QLabel(
        QStringLiteral("低延迟 WASAPI 多设备分发 · 每台设备独立音量与同步补偿"), root);
    subtitle->setObjectName(QStringLiteral("MutedText"));
    brandLayout->addWidget(eyebrow);
    brandLayout->addWidget(title);
    brandLayout->addWidget(subtitle);

    themeButton_ = new QToolButton(root);
    themeButton_->setObjectName(QStringLiteral("ThemeButton"));
    themeButton_->setCursor(Qt::PointingHandCursor);
    themeButton_->setFixedHeight(36);

    headerLayout->addLayout(brandLayout, 1);
    headerLayout->addWidget(themeButton_, 0, Qt::AlignTop);
    rootLayout->addLayout(headerLayout);

    auto* contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(16);

    auto* leftColumn = new QWidget(root);
    leftColumn->setObjectName(QStringLiteral("Transparent"));
    auto* leftLayout = new QVBoxLayout(leftColumn);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(14);

    auto* sourceCard = new QFrame(leftColumn);
    sourceCard->setObjectName(QStringLiteral("Card"));
    auto* sourceLayout = new QVBoxLayout(sourceCard);
    sourceLayout->setContentsMargins(18, 16, 18, 16);
    sourceLayout->setSpacing(10);

    auto* sourceHeader = new QHBoxLayout();
    sourceHeader->addWidget(createSectionTitle(QStringLiteral("音频来源"), sourceCard));
    sourceHeader->addStretch();
    auto* sourceTag = new QLabel(QStringLiteral("WASAPI LOOPBACK"), sourceCard);
    sourceTag->setObjectName(QStringLiteral("Tag"));
    sourceHeader->addWidget(sourceTag);
    sourceLayout->addLayout(sourceHeader);

    auto* sourceControls = new QHBoxLayout();
    sourceControls->setSpacing(8);
    captureCombo_ = new QComboBox(sourceCard);
    captureCombo_->setMinimumHeight(38);
    refreshButton_ = new QPushButton(QStringLiteral("刷新设备"), sourceCard);
    refreshButton_->setObjectName(QStringLiteral("SecondaryButton"));
    refreshButton_->setCursor(Qt::PointingHandCursor);
    refreshButton_->setMinimumHeight(38);
    sourceControls->addWidget(captureCombo_, 1);
    sourceControls->addWidget(refreshButton_);
    sourceLayout->addLayout(sourceControls);

    sourceHintLabel_ = new QLabel(sourceCard);
    sourceHintLabel_->setObjectName(QStringLiteral("SourceHint"));
    sourceHintLabel_->setWordWrap(true);
    sourceLayout->addWidget(sourceHintLabel_);

    auto* calibrationHeader = new QHBoxLayout();
    auto* calibrationTitle = new QLabel(QStringLiteral("声学校准麦克风"), sourceCard);
    calibrationTitle->setObjectName(QStringLiteral("ControlLabel"));
    calibrationHeader->addWidget(calibrationTitle);
    calibrationHeader->addStretch();
    auto* calibrationTag = new QLabel(QStringLiteral("ACOUSTIC PROBE"), sourceCard);
    calibrationTag->setObjectName(QStringLiteral("Tag"));
    calibrationHeader->addWidget(calibrationTag);
    sourceLayout->addLayout(calibrationHeader);

    auto* calibrationControls = new QHBoxLayout();
    calibrationControls->setSpacing(8);
    microphoneCombo_ = new QComboBox(sourceCard);
    microphoneCombo_->setMinimumHeight(38);
    calibrateButton_ = new QPushButton(QStringLiteral("自动校准"), sourceCard);
    calibrateButton_->setObjectName(QStringLiteral("SecondaryButton"));
    calibrateButton_->setCursor(Qt::PointingHandCursor);
    calibrateButton_->setMinimumHeight(38);
    calibrationControls->addWidget(microphoneCombo_, 1);
    calibrationControls->addWidget(calibrateButton_);
    sourceLayout->addLayout(calibrationControls);

    calibrationHintLabel_ = new QLabel(
        QStringLiteral("把麦克风放在听音位置；校准会依次播放短扫频并自动回填设备补偿。"),
        sourceCard);
    calibrationHintLabel_->setObjectName(QStringLiteral("MutedText"));
    calibrationHintLabel_->setWordWrap(true);
    sourceLayout->addWidget(calibrationHintLabel_);

    auto* phoneHeader = new QHBoxLayout();
    auto* phoneTitle = new QLabel(QStringLiteral("手机麦克风"), sourceCard);
    phoneTitle->setObjectName(QStringLiteral("ControlLabel"));
    phoneHeader->addWidget(phoneTitle);
    phoneHeader->addStretch();
    auto* phoneTag = new QLabel(QStringLiteral("ANDROID · LAN"), sourceCard);
    phoneTag->setObjectName(QStringLiteral("Tag"));
    phoneHeader->addWidget(phoneTag);
    sourceLayout->addLayout(phoneHeader);

    auto* phoneControls = new QHBoxLayout();
    phoneStatusLabel_ = new QLabel(QStringLiteral("配对服务正在初始化"), sourceCard);
    phoneStatusLabel_->setObjectName(QStringLiteral("PhoneStatus"));
    phoneStatusLabel_->setWordWrap(true);
    phonePairButton_ = new QPushButton(QStringLiteral("配对手机"), sourceCard);
    phonePairButton_->setObjectName(QStringLiteral("SecondaryButton"));
    phonePairButton_->setMinimumHeight(36);
    phoneControls->addWidget(phoneStatusLabel_, 1);
    phoneControls->addWidget(phonePairButton_);
    sourceLayout->addLayout(phoneControls);

    usePhoneMicrophoneCheck_ = new QCheckBox(
        QStringLiteral("连续声学跟踪使用手机麦克风"), sourceCard);
    usePhoneMicrophoneCheck_->setEnabled(false);
    sourceLayout->addWidget(usePhoneMicrophoneCheck_);
    phoneLevelLabel_ = new QLabel(QStringLiteral("手机麦克风电平：未连接"), sourceCard);
    phoneLevelLabel_->setObjectName(QStringLiteral("MutedText"));
    sourceLayout->addWidget(phoneLevelLabel_);
    leftLayout->addWidget(sourceCard);

    auto* outputCard = new QFrame(leftColumn);
    outputCard->setObjectName(QStringLiteral("Card"));
    auto* outputLayout = new QVBoxLayout(outputCard);
    outputLayout->setContentsMargins(18, 16, 18, 16);
    outputLayout->setSpacing(12);

    auto* outputHeader = new QHBoxLayout();
    outputHeader->addWidget(createSectionTitle(QStringLiteral("输出设备"), outputCard));
    outputHeader->addStretch();
    selectionLabel_ = new QLabel(QStringLiteral("未选择设备"), outputCard);
    selectionLabel_->setObjectName(QStringLiteral("CountBadge"));
    outputHeader->addWidget(selectionLabel_);
    outputLayout->addLayout(outputHeader);

    auto* outputScroll = new QScrollArea(outputCard);
    outputScroll->setObjectName(QStringLiteral("OutputScroll"));
    outputScroll->setWidgetResizable(true);
    outputScroll->setFrameShape(QFrame::NoFrame);
    auto* outputContainer = new QWidget(outputScroll);
    outputContainer->setObjectName(QStringLiteral("Transparent"));
    outputCardsLayout_ = new QVBoxLayout(outputContainer);
    outputCardsLayout_->setContentsMargins(0, 0, 5, 0);
    outputCardsLayout_->setSpacing(9);
    outputCardsLayout_->setAlignment(Qt::AlignTop);
    outputScroll->setWidget(outputContainer);
    outputLayout->addWidget(outputScroll, 1);
    leftLayout->addWidget(outputCard, 1);

    auto* rightColumn = new QWidget(root);
    rightColumn->setObjectName(QStringLiteral("Transparent"));
    rightColumn->setMinimumWidth(330);
    rightColumn->setMaximumWidth(390);
    auto* rightLayout = new QVBoxLayout(rightColumn);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(14);

    auto* syncCard = new QFrame(rightColumn);
    syncCard->setObjectName(QStringLiteral("Card"));
    auto* syncLayout = new QVBoxLayout(syncCard);
    syncLayout->setContentsMargins(18, 16, 18, 16);
    syncLayout->setSpacing(13);
    syncLayout->addWidget(createSectionTitle(QStringLiteral("同步引擎"), syncCard));

    auto* modeBadge = new QLabel(QStringLiteral("IAudioClient3 低延迟优先"), syncCard);
    modeBadge->setObjectName(QStringLiteral("AccentBadge"));
    syncLayout->addWidget(modeBadge, 0, Qt::AlignLeft);

    auto* bufferLabelLayout = new QHBoxLayout();
    auto* bufferTitle = new QLabel(QStringLiteral("实时同步余量"), syncCard);
    bufferTitle->setObjectName(QStringLiteral("ControlLabel"));
    bufferSpin_ = new QSpinBox(syncCard);
    bufferSpin_->setRange(2, 100);
    bufferSpin_->setSingleStep(1);
    bufferSpin_->setValue(5);
    bufferSpin_->setSuffix(QStringLiteral(" ms"));
    bufferSpin_->setFixedWidth(92);
    bufferLabelLayout->addWidget(bufferTitle);
    bufferLabelLayout->addStretch();
    bufferLabelLayout->addWidget(bufferSpin_);
    syncLayout->addLayout(bufferLabelLayout);

    bufferSlider_ = new QSlider(Qt::Horizontal, syncCard);
    bufferSlider_->setRange(2, 100);
    bufferSlider_->setValue(5);
    bufferSlider_->setSingleStep(1);
    syncLayout->addWidget(bufferSlider_);

    auto* bufferHint = new QLabel(
        QStringLiteral("播放中也可调整。建议从 5 ms 开始；若有爆音，每次增加 2–5 ms。"), syncCard);
    bufferHint->setObjectName(QStringLiteral("MutedText"));
    bufferHint->setWordWrap(true);
    syncLayout->addWidget(bufferHint);

    automaticLatencyCheck_ = new QCheckBox(QStringLiteral("自动补偿设备报告的流延迟"), syncCard);
    automaticLatencyCheck_->setChecked(true);
    syncLayout->addWidget(automaticLatencyCheck_);

    continuousAcousticCheck_ = new QCheckBox(
        QStringLiteral("播放中自适应声学跟踪"), syncCard);
    continuousAcousticCheck_->setChecked(true);
    continuousAcousticCheck_->setToolTip(
        QStringLiteral("逐台发送近超声探针；设备不支持时自动切换到由节目声掩蔽的低电平扩频探针。"));
    syncLayout->addWidget(continuousAcousticCheck_);

    auto* acousticHint = new QLabel(
        QStringLiteral("优先近超声；检测失败自动改用低电平扩频。所有探针仅在实时节目电平足以掩蔽时发送。"),
        syncCard);
    acousticHint->setObjectName(QStringLiteral("MutedText"));
    acousticHint->setWordWrap(true);
    syncLayout->addWidget(acousticHint);

    programLevelLabel_ = new QLabel(
        QStringLiteral("实时节目电平：未监测 · 探针暂停"), syncCard);
    programLevelLabel_->setObjectName(QStringLiteral("ProgramLevel"));
    programLevelLabel_->setProperty("active", false);
    syncLayout->addWidget(programLevelLabel_);

    exclusiveModeCheck_ = new QCheckBox(QStringLiteral("实验性：优先独占输出"), syncCard);
    exclusiveModeCheck_->setChecked(false);
    syncLayout->addWidget(exclusiveModeCheck_);

    auto* exclusiveHint = new QLabel(
        QStringLiteral("同一驱动的多个端点可能互斥；仅建议互相独立的物理声卡尝试。"), syncCard);
    exclusiveHint->setObjectName(QStringLiteral("MutedText"));
    exclusiveHint->setWordWrap(true);
    syncLayout->addWidget(exclusiveHint);

    auto* autoHint = new QLabel(
        QStringLiteral("会把较快的副输出延后到最慢副输出；手动延迟仍会叠加。"), syncCard);
    autoHint->setObjectName(QStringLiteral("MutedText"));
    autoHint->setWordWrap(true);
    syncLayout->addWidget(autoHint);
    rightLayout->addWidget(syncCard);

    auto* logCard = new QFrame(rightColumn);
    logCard->setObjectName(QStringLiteral("Card"));
    auto* logLayout = new QVBoxLayout(logCard);
    logLayout->setContentsMargins(18, 16, 18, 16);
    logLayout->setSpacing(10);
    logLayout->addWidget(createSectionTitle(QStringLiteral("运行日志"), logCard));
    logView_ = new QTextEdit(logCard);
    logView_->setObjectName(QStringLiteral("LogView"));
    logView_->setReadOnly(true);
    logView_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    logLayout->addWidget(logView_, 1);
    rightLayout->addWidget(logCard, 1);

    contentLayout->addWidget(leftColumn, 1);
    contentLayout->addWidget(rightColumn);
    rootLayout->addLayout(contentLayout, 1);

    auto* actionBar = new QFrame(root);
    actionBar->setObjectName(QStringLiteral("ActionBar"));
    auto* actionLayout = new QHBoxLayout(actionBar);
    actionLayout->setContentsMargins(16, 10, 12, 10);
    actionLayout->setSpacing(10);

    stateLabel_ = new QLabel(QStringLiteral("未启动"), actionBar);
    stateLabel_->setObjectName(QStringLiteral("StatusLabel"));
    stateLabel_->setProperty("running", false);
    auto* stateHint = new QLabel(QStringLiteral("配置设备后开始同步"), actionBar);
    stateHint->setObjectName(QStringLiteral("MutedText"));
    startButton_ = new QPushButton(QStringLiteral("开始同步"), actionBar);
    startButton_->setObjectName(QStringLiteral("PrimaryButton"));
    startButton_->setCursor(Qt::PointingHandCursor);
    startButton_->setMinimumSize(132, 42);
    stopButton_ = new QPushButton(QStringLiteral("停止"), actionBar);
    stopButton_->setObjectName(QStringLiteral("SecondaryButton"));
    stopButton_->setCursor(Qt::PointingHandCursor);
    stopButton_->setMinimumSize(88, 42);
    stopButton_->setEnabled(false);
    actionLayout->addWidget(stateLabel_);
    actionLayout->addWidget(stateHint);
    actionLayout->addStretch();
    actionLayout->addWidget(stopButton_);
    actionLayout->addWidget(startButton_);
    rootLayout->addWidget(actionBar);

    setCentralWidget(root);

    connect(themeButton_, &QToolButton::clicked, this, &MainWindow::toggleTheme);
    connect(refreshButton_, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(captureCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::rebuildOutputCards);
    connect(captureCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::updateSourceHint);
    connect(bufferSlider_, &QSlider::valueChanged, bufferSpin_, &QSpinBox::setValue);
    connect(bufferSpin_, qOverload<int>(&QSpinBox::valueChanged), bufferSlider_, &QSlider::setValue);
    connect(bufferSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int value) {
                engine_.setSynchronizationMargin(value);
            });
    connect(startButton_, &QPushButton::clicked, this, &MainWindow::startAudio);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::stopAudio);
    connect(calibrateButton_, &QPushButton::clicked,
            this, &MainWindow::startOrStopCalibration);
    connect(&engine_, &AudioEngine::statusChanged, this, &MainWindow::appendStatus);
    connect(&engine_, &AudioEngine::errorOccurred, this, &MainWindow::showEngineError);
    connect(&engine_, &AudioEngine::runningChanged, this, &MainWindow::updateRunningState);
    connect(&engine_, &AudioEngine::acousticCorrectionChanged,
            this, &MainWindow::updateAcousticCorrection);
    connect(&engine_, &AudioEngine::programLevelChanged,
            this, &MainWindow::updateProgramLevel);
    connect(phonePairButton_, &QPushButton::clicked,
            this, &MainWindow::showPhonePairing);
    connect(&phonePairingServer_, &PhonePairingServer::statusChanged,
            this, &MainWindow::appendStatus);
    connect(&phonePairingServer_, &PhonePairingServer::connectionChanged,
            this, &MainWindow::updatePhoneConnection);
    connect(&phonePairingServer_, &PhonePairingServer::microphoneLevelChanged,
            this, &MainWindow::updatePhoneMicrophoneLevel);
    connect(&calibrator_, &LatencyCalibrator::statusChanged,
            this, &MainWindow::appendStatus);
    connect(&calibrator_, &LatencyCalibrator::errorOccurred,
            this, &MainWindow::showCalibrationError);
    connect(&calibrator_, &LatencyCalibrator::runningChanged,
            this, &MainWindow::updateCalibrationRunningState);
    connect(&calibrator_, &LatencyCalibrator::finished,
            this, &MainWindow::applyCalibrationResults);

    applyTheme();
    refreshDevices();
    QString pairingError;
    if (phonePairingServer_.start(&pairingError)) {
        phoneStatusLabel_->setText(
            QStringLiteral("未连接 · 配对码 %1").arg(phonePairingServer_.pairingCode()));
    } else {
        phoneStatusLabel_->setText(QStringLiteral("配对服务启动失败"));
        appendStatus(QStringLiteral("手机配对服务启动失败：%1").arg(pairingError));
    }
}

MainWindow::~MainWindow()
{
    calibrator_.stop();
    engine_.stop();
    phonePairingServer_.stop();
}

void MainWindow::refreshDevices()
{
    const QString previousCaptureId = captureCombo_->currentData().toString();
    const QString previousMicrophoneId = microphoneCombo_->currentData().toString();
    QString renderError;
    QString microphoneError;
    devices_ = WasapiDeviceManager::enumerateRenderDevices(&renderError);
    microphoneDevices_ = WasapiDeviceManager::enumerateCaptureDevices(&microphoneError);

    const QSignalBlocker blocker(captureCombo_);
    const QSignalBlocker microphoneBlocker(microphoneCombo_);
    captureCombo_->clear();
    microphoneCombo_->clear();

    int selectedIndex = -1;
    for (int index = 0; index < devices_.size(); ++index) {
        const AudioDevice& device = devices_.at(index);
        const QString label = device.isDefault
                                  ? QStringLiteral("%1  ·  Windows 默认").arg(device.name)
                                  : device.name;
        captureCombo_->addItem(label, device.id);
        if (device.id == previousCaptureId || (previousCaptureId.isEmpty() && device.isDefault)) {
            selectedIndex = index;
        }
    }

    if (selectedIndex >= 0) {
        captureCombo_->setCurrentIndex(selectedIndex);
    }

    int selectedMicrophoneIndex = -1;
    for (int index = 0; index < microphoneDevices_.size(); ++index) {
        const AudioDevice& device = microphoneDevices_.at(index);
        const QString label = device.isDefault
                                  ? QStringLiteral("%1  ·  Windows 默认").arg(device.name)
                                  : device.name;
        microphoneCombo_->addItem(label, device.id);
        if (device.id == previousMicrophoneId
            || (previousMicrophoneId.isEmpty() && device.isDefault)) {
            selectedMicrophoneIndex = index;
        }
    }
    if (selectedMicrophoneIndex >= 0) {
        microphoneCombo_->setCurrentIndex(selectedMicrophoneIndex);
    }
    rebuildOutputCards();
    updateSourceHint();

    if (!renderError.isEmpty()) {
        showEngineError(renderError);
    } else if (!microphoneError.isEmpty()) {
        showCalibrationError(microphoneError);
    } else {
        appendStatus(QStringLiteral("发现 %1 个活动播放设备、%2 个录音设备")
                         .arg(devices_.size())
                         .arg(microphoneDevices_.size()));
    }
}

void MainWindow::rebuildOutputCards()
{
    QSet<QString> selectedIds;
    QHash<QString, int> savedVolumes;
    QHash<QString, int> savedDelays;
    for (auto iterator = outputChecks_.constBegin(); iterator != outputChecks_.constEnd(); ++iterator) {
        if (iterator.value()->isChecked()) {
            selectedIds.insert(iterator.key());
        }
        savedVolumes.insert(iterator.key(), volumeSliders_.value(iterator.key())->value());
        savedDelays.insert(iterator.key(), delaySpinBoxes_.value(iterator.key())->value());
    }

    while (QLayoutItem* item = outputCardsLayout_->takeAt(0)) {
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    outputChecks_.clear();
    volumeSliders_.clear();
    volumeLabels_.clear();
    delaySpinBoxes_.clear();
    acousticLabels_.clear();

    const QString captureId = captureCombo_->currentData().toString();
    for (const AudioDevice& device : devices_) {
        if (device.id == captureId) {
            continue;
        }

        const int savedVolume = savedVolumes.value(device.id, 100);
        const int savedDelay = savedDelays.value(device.id, 0);

        auto* card = new QFrame();
        card->setObjectName(QStringLiteral("DeviceCard"));
        card->setProperty("selected", selectedIds.contains(device.id));
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(14, 11, 14, 11);
        cardLayout->setSpacing(9);

        auto* cardHeader = new QHBoxLayout();
        auto* enabledCheck = new QCheckBox(device.name, card);
        enabledCheck->setObjectName(QStringLiteral("DeviceCheck"));
        enabledCheck->setChecked(selectedIds.contains(device.id));
        cardHeader->addWidget(enabledCheck, 1);
        if (device.isDefault) {
            auto* defaultBadge = new QLabel(QStringLiteral("默认"), card);
            defaultBadge->setObjectName(QStringLiteral("Tag"));
            cardHeader->addWidget(defaultBadge);
        }
        cardLayout->addLayout(cardHeader);

        auto* controls = new QHBoxLayout();
        controls->setSpacing(8);
        auto* volumeTitle = new QLabel(QStringLiteral("音量"), card);
        volumeTitle->setObjectName(QStringLiteral("MutedText"));
        auto* volumeSlider = new QSlider(Qt::Horizontal, card);
        volumeSlider->setRange(0, 100);
        volumeSlider->setValue(savedVolume);
        volumeSlider->setMinimumWidth(120);
        auto* volumeLabel = new QLabel(QStringLiteral("%1%").arg(savedVolume), card);
        volumeLabel->setObjectName(QStringLiteral("ValueLabel"));
        volumeLabel->setMinimumWidth(38);
        volumeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* delayTitle = new QLabel(QStringLiteral("实时补偿"), card);
        delayTitle->setObjectName(QStringLiteral("MutedText"));
        auto* delaySpin = new QSpinBox(card);
        delaySpin->setRange(0, 2000);
        delaySpin->setSingleStep(1);
        delaySpin->setValue(savedDelay);
        delaySpin->setSuffix(QStringLiteral(" ms"));
        delaySpin->setFixedWidth(96);
        delaySpin->setToolTip(QStringLiteral("播放过程中可调整；较大的变化会平滑追踪，避免爆音。"));

        controls->addWidget(volumeTitle);
        controls->addWidget(volumeSlider, 1);
        controls->addWidget(volumeLabel);
        controls->addSpacing(8);
        controls->addWidget(delayTitle);
        controls->addWidget(delaySpin);
        cardLayout->addLayout(controls);

        auto* acousticLabel = new QLabel(
            QStringLiteral("声学跟踪：尚未开始"), card);
        acousticLabel->setObjectName(QStringLiteral("AcousticState"));
        acousticLabel->setWordWrap(true);
        cardLayout->addWidget(acousticLabel);
        outputCardsLayout_->addWidget(card);

        outputChecks_.insert(device.id, enabledCheck);
        volumeSliders_.insert(device.id, volumeSlider);
        volumeLabels_.insert(device.id, volumeLabel);
        delaySpinBoxes_.insert(device.id, delaySpin);
        acousticLabels_.insert(device.id, acousticLabel);

        connect(enabledCheck, &QCheckBox::toggled, this,
                [this, card](bool checked) {
                    card->setProperty("selected", checked);
                    refreshDynamicStyle(card);
                    updateSelectionSummary();
                });
        connect(volumeSlider, &QSlider::valueChanged, this,
                [this, volumeLabel, deviceId = device.id](int value) {
                    volumeLabel->setText(QStringLiteral("%1%").arg(value));
                    engine_.setOutputVolume(deviceId, value);
                });
        connect(delaySpin, qOverload<int>(&QSpinBox::valueChanged), this,
                [this, deviceId = device.id](int value) {
                    engine_.setOutputDelay(deviceId, value);
                });
    }

    outputCardsLayout_->addStretch();
    updateSelectionSummary();
}

void MainWindow::startAudio()
{
    if (calibrator_.isActive()) {
        QMessageBox::information(this,
                                 QStringLiteral("正在校准"),
                                 QStringLiteral("请先等待声学校准完成或停止校准。"));
        return;
    }

    const AudioDevice capture = currentCaptureDevice();
    const AudioDevice microphone = currentMicrophoneDevice();
    const QVector<OutputDeviceSettings> outputs = selectedOutputDevices();
    if (capture.id.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法启动"),
                             QStringLiteral("请选择系统声音捕获源。"));
        return;
    }
    if (outputs.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法启动"),
                             QStringLiteral("请至少选择一个输出设备。"));
        return;
    }
    const bool continuousTracking = continuousAcousticCheck_->isChecked()
                                    && outputs.size() >= 2;
    const bool usePhoneMicrophone = continuousTracking
                                    && usePhoneMicrophoneCheck_->isChecked()
                                    && phonePairingServer_.phoneConnected();
    if (continuousTracking && !usePhoneMicrophone && microphone.id.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法启动声学跟踪"),
                             QStringLiteral("请选择用于连续测量的麦克风。"));
        return;
    }

    if (!engine_.start(capture,
                       outputs,
                       bufferSpin_->value(),
                       exclusiveModeCheck_->isChecked(),
                       automaticLatencyCheck_->isChecked(),
                       microphone,
                       continuousTracking,
                       usePhoneMicrophone
                           ? phonePairingServer_.remoteBuffer()
                           : nullptr)) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("音频引擎已经在启动或运行。"));
        return;
    }

    lockedOutputIds_.clear();
    for (const OutputDeviceSettings& output : outputs) {
        lockedOutputIds_.insert(output.device.id);
        if (QLabel* label = acousticLabels_.value(output.device.id, nullptr)) {
            label->setText(continuousTracking
                               ? QStringLiteral("声学跟踪：等待自适应探针")
                               : QStringLiteral("声学跟踪：未启用"));
        }
    }

    setControlsEnabled(false);
    stopButton_->setEnabled(true);
    stateLabel_->setText(QStringLiteral("正在初始化"));
    stateLabel_->setProperty("running", true);
    refreshDynamicStyle(stateLabel_);
    appendStatus(QStringLiteral("正在初始化 %1 个输出设备，软件同步余量 %2 ms")
                     .arg(outputs.size())
                     .arg(bufferSpin_->value()));
    programLevelLabel_->setText(QStringLiteral("实时节目电平：等待音频 · 探针暂停"));
    programLevelLabel_->setProperty("active", false);
    refreshDynamicStyle(programLevelLabel_);
}

void MainWindow::stopAudio()
{
    stateLabel_->setText(QStringLiteral("正在停止"));
    stopButton_->setEnabled(false);
    engine_.stop();
}

void MainWindow::startOrStopCalibration()
{
    if (calibrator_.isActive()) {
        calibrateButton_->setEnabled(false);
        stateLabel_->setText(QStringLiteral("正在停止校准"));
        calibrator_.stop();
        return;
    }

    if (engine_.isActive()) {
        QMessageBox::information(this,
                                 QStringLiteral("正在同步"),
                                 QStringLiteral("请先停止音频同步，再进行声学校准。"));
        return;
    }

    const AudioDevice microphone = currentMicrophoneDevice();
    if (microphone.id.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("无法校准"),
                             QStringLiteral("请选择用于测量的麦克风。"));
        return;
    }

    const QVector<OutputDeviceSettings> settings = selectedOutputDevices();
    if (settings.size() < 2) {
        QMessageBox::warning(this,
                             QStringLiteral("无法校准"),
                             QStringLiteral("请至少选择两个输出设备。"));
        return;
    }

    QVector<AudioDevice> outputs;
    outputs.reserve(settings.size());
    for (const OutputDeviceSettings& output : settings) {
        outputs.push_back(output.device);
    }

    if (!calibrator_.start(microphone, outputs)) {
        QMessageBox::information(this,
                                 QStringLiteral("提示"),
                                 QStringLiteral("声学校准已经在运行。"));
        return;
    }

    lockedOutputIds_.clear();
    for (const AudioDevice& device : outputs) {
        lockedOutputIds_.insert(device.id);
    }
    setControlsEnabled(false);
    calibrateButton_->setEnabled(true);
    calibrateButton_->setText(QStringLiteral("停止校准"));
    stopButton_->setEnabled(false);
    stateLabel_->setText(QStringLiteral("声学校准中"));
    stateLabel_->setProperty("running", true);
    refreshDynamicStyle(stateLabel_);
    calibrationHintLabel_->setText(
        QStringLiteral("正在测量：请保持环境安静，不要移动麦克风。"));
    appendStatus(QStringLiteral("准备校准 %1 台输出设备；麦克风应放在实际听音位置")
                     .arg(outputs.size()));
}

void MainWindow::applyCalibrationResults()
{
    const QVector<LatencyCalibrationResult> measuredResults = calibrator_.results();
    int appliedCount = 0;
    for (const LatencyCalibrationResult& result : measuredResults) {
        if (!result.detected) {
            appendStatus(QStringLiteral("未能可靠识别：%1（置信度 %2%）")
                             .arg(result.device.name)
                             .arg(result.confidence * 100.0, 0, 'f', 0));
            continue;
        }
        QSpinBox* delaySpin = delaySpinBoxes_.value(result.device.id, nullptr);
        if (delaySpin == nullptr) {
            continue;
        }
        delaySpin->setValue(result.recommendedDelayMilliseconds);
        ++appliedCount;
    }

    calibrationHintLabel_->setText(
        QStringLiteral("校准完成：已向 %1 台设备回填相对延迟补偿，可直接开始同步。")
            .arg(appliedCount));
}

void MainWindow::updateCalibrationRunningState(bool running)
{
    if (running) {
        return;
    }

    lockedOutputIds_.clear();
    calibrateButton_->setText(QStringLiteral("自动校准"));
    calibrateButton_->setEnabled(true);
    stateLabel_->setText(QStringLiteral("未启动"));
    stateLabel_->setProperty("running", false);
    refreshDynamicStyle(stateLabel_);
    setControlsEnabled(true);
    stopButton_->setEnabled(false);
}

void MainWindow::showCalibrationError(const QString& message)
{
    appendStatus(QStringLiteral("校准错误：%1").arg(message));
    calibrationHintLabel_->setText(
        QStringLiteral("校准失败：请检查麦克风、扬声器音量和环境噪声后重试。"));
    QMessageBox::critical(this, QStringLiteral("声学校准错误"), message);
}

void MainWindow::updateAcousticCorrection(const QString& deviceId,
                                          int delayMilliseconds,
                                          double driftPpm,
                                          const QString& probeMode,
                                          double confidence)
{
    QLabel* label = acousticLabels_.value(deviceId, nullptr);
    if (label == nullptr) {
        return;
    }
    label->setText(
        QStringLiteral("声学跟踪：%1 · 自动 %2 ms · 漂移 %3 ppm · 置信度 %4%")
            .arg(probeMode)
            .arg(delayMilliseconds)
            .arg(driftPpm, 0, 'f', 1)
            .arg(confidence * 100.0, 0, 'f', 0));
}

void MainWindow::updateProgramLevel(double levelDbfs, bool probeAllowed)
{
    programLevelLabel_->setText(
        probeAllowed
            ? QStringLiteral("实时节目电平：%1 dBFS · 允许探针")
                  .arg(levelDbfs, 0, 'f', 1)
            : QStringLiteral("实时节目电平：−∞ dBFS · 探针暂停"));
    programLevelLabel_->setProperty("active", probeAllowed);
    refreshDynamicStyle(programLevelLabel_);
}

void MainWindow::showPhonePairing()
{
    if (phonePairingServer_.serverPort() == 0) {
        QString error;
        if (!phonePairingServer_.start(&error)) {
            QMessageBox::critical(this,
                                  QStringLiteral("手机配对失败"),
                                  QStringLiteral("无法启动配对服务：%1").arg(error));
            return;
        }
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("配对 Android 手机"));
    dialog.setMinimumWidth(430);
    auto* layout = new QVBoxLayout(&dialog);
    auto* title = new QLabel(QStringLiteral("用 VoiceSpreader Android 扫描二维码"), &dialog);
    title->setObjectName(QStringLiteral("SectionTitle"));
    layout->addWidget(title, 0, Qt::AlignHCenter);

    auto* qrLabel = new QLabel(&dialog);
    qrLabel->setAlignment(Qt::AlignCenter);
    auto refreshQr = [&] {
        const QImage qr = createPairingQrCode(phonePairingServer_.pairingPayload(), 7);
        qrLabel->setPixmap(QPixmap::fromImage(qr));
    };
    refreshQr();
    layout->addWidget(qrLabel, 0, Qt::AlignHCenter);

    auto* codeLabel = new QLabel(
        QStringLiteral("或在手机输入配对码：<b style='font-size:24px'>%1</b>")
            .arg(phonePairingServer_.pairingCode()),
        &dialog);
    codeLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(codeLabel);
    auto* addressLabel = new QLabel(
        QStringLiteral("电脑地址 %1:%2 · 手机和电脑必须连接同一局域网")
            .arg(phonePairingServer_.localAddress())
            .arg(phonePairingServer_.serverPort()),
        &dialog);
    addressLabel->setObjectName(QStringLiteral("MutedText"));
    addressLabel->setAlignment(Qt::AlignCenter);
    addressLabel->setWordWrap(true);
    layout->addWidget(addressLabel);

    auto* actions = new QHBoxLayout();
    auto* regenerateButton = new QPushButton(QStringLiteral("生成新配对码"), &dialog);
    regenerateButton->setObjectName(QStringLiteral("SecondaryButton"));
    auto* closeButton = new QPushButton(QStringLiteral("关闭"), &dialog);
    closeButton->setObjectName(QStringLiteral("PrimaryButton"));
    actions->addWidget(regenerateButton);
    actions->addStretch();
    actions->addWidget(closeButton);
    layout->addLayout(actions);
    connect(regenerateButton, &QPushButton::clicked, &dialog, [&] {
        phonePairingServer_.resetPairing();
        refreshQr();
        codeLabel->setText(
            QStringLiteral("或在手机输入配对码：<b style='font-size:24px'>%1</b>")
                .arg(phonePairingServer_.pairingCode()));
        phoneStatusLabel_->setText(
            QStringLiteral("未连接 · 配对码 %1").arg(phonePairingServer_.pairingCode()));
    });
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(&phonePairingServer_, &PhonePairingServer::connectionChanged,
            &dialog, [&dialog](bool connected, const QString&) {
                if (connected) {
                    dialog.accept();
                }
            });
    dialog.exec();
}

void MainWindow::updatePhoneConnection(bool connected, const QString& phoneName)
{
    if (connected) {
        phoneStatusLabel_->setText(QStringLiteral("已连接：%1").arg(phoneName));
        usePhoneMicrophoneCheck_->setEnabled(!engine_.isActive());
        usePhoneMicrophoneCheck_->setChecked(true);
        phoneLevelLabel_->setText(QStringLiteral("手机麦克风电平：等待采样"));
    } else {
        phoneStatusLabel_->setText(
            QStringLiteral("未连接 · 配对码 %1").arg(phonePairingServer_.pairingCode()));
        usePhoneMicrophoneCheck_->setChecked(false);
        usePhoneMicrophoneCheck_->setEnabled(false);
        phoneLevelLabel_->setText(QStringLiteral("手机麦克风电平：未连接"));
    }
}

void MainWindow::updatePhoneMicrophoneLevel(double levelDbfs)
{
    phoneLevelLabel_->setText(
        QStringLiteral("手机麦克风电平：%1 dBFS").arg(levelDbfs, 0, 'f', 1));
}

void MainWindow::appendStatus(const QString& message)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    logView_->append(QStringLiteral("<span style=\"color:#8b95a7\">%1</span>  %2")
                         .arg(timestamp, message.toHtmlEscaped()));
}

void MainWindow::showEngineError(const QString& message)
{
    appendStatus(QStringLiteral("错误：%1").arg(message));
    QMessageBox::critical(this, QStringLiteral("VoiceSpreader 错误"), message);
}

void MainWindow::updateRunningState(bool running)
{
    stateLabel_->setText(running ? QStringLiteral("正在同步") : QStringLiteral("未启动"));
    stateLabel_->setProperty("running", running);
    refreshDynamicStyle(stateLabel_);
    setControlsEnabled(!running && !engine_.isActive());
    stopButton_->setEnabled(running || engine_.isActive());

    if (!running && !engine_.isActive()) {
        lockedOutputIds_.clear();
        setControlsEnabled(true);
        stopButton_->setEnabled(false);
        programLevelLabel_->setText(QStringLiteral("实时节目电平：未监测 · 探针暂停"));
        programLevelLabel_->setProperty("active", false);
        refreshDynamicStyle(programLevelLabel_);
    }
}

void MainWindow::updateSelectionSummary()
{
    int count = 0;
    for (QCheckBox* check : outputChecks_) {
        if (check->isChecked()) {
            ++count;
        }
    }
    selectionLabel_->setText(count == 0
                                 ? QStringLiteral("未选择设备")
                                 : QStringLiteral("已选择 %1 台").arg(count));
}

void MainWindow::updateSourceHint()
{
    const AudioDevice source = currentCaptureDevice();
    const bool virtualSource = source.name.contains(QStringLiteral("CABLE"), Qt::CaseInsensitive)
                               || source.name.contains(QStringLiteral("Virtual"), Qt::CaseInsensitive)
                               || source.name.contains(QStringLiteral("虚拟"), Qt::CaseInsensitive);
    sourceHintLabel_->setProperty("virtual", virtualSource);
    if (virtualSource) {
        sourceHintLabel_->setText(
            QStringLiteral("虚拟源模式：所有实体扬声器都可由 VoiceSpreader 统一延迟，设备间更容易对齐。"));
    } else {
        sourceHintLabel_->setText(
            QStringLiteral("物理源模式：该设备由 Windows 直接发声，副输出无法比它更早；明显不同步时建议改用虚拟音频源。"));
    }
    refreshDynamicStyle(sourceHintLabel_);
}

void MainWindow::toggleTheme()
{
    darkTheme_ = !darkTheme_;
    applyTheme();
}

AudioDevice MainWindow::currentCaptureDevice() const
{
    const QString id = captureCombo_->currentData().toString();
    for (const AudioDevice& device : devices_) {
        if (device.id == id) {
            return device;
        }
    }
    return {};
}

AudioDevice MainWindow::currentMicrophoneDevice() const
{
    const QString id = microphoneCombo_->currentData().toString();
    for (const AudioDevice& device : microphoneDevices_) {
        if (device.id == id) {
            return device;
        }
    }
    return {};
}

QVector<OutputDeviceSettings> MainWindow::selectedOutputDevices() const
{
    QVector<OutputDeviceSettings> outputs;
    for (const AudioDevice& device : devices_) {
        const QCheckBox* enabledCheck = outputChecks_.value(device.id, nullptr);
        if (enabledCheck == nullptr || !enabledCheck->isChecked()) {
            continue;
        }

        OutputDeviceSettings settings;
        settings.device = device;
        settings.volumePercent = volumeSliders_.value(device.id)->value();
        settings.extraDelayMilliseconds = delaySpinBoxes_.value(device.id)->value();
        outputs.push_back(settings);
    }
    return outputs;
}

void MainWindow::setControlsEnabled(bool enabled)
{
    captureCombo_->setEnabled(enabled);
    microphoneCombo_->setEnabled(enabled);
    bufferSlider_->setEnabled(enabled || engine_.isActive());
    bufferSpin_->setEnabled(enabled || engine_.isActive());
    automaticLatencyCheck_->setEnabled(enabled);
    continuousAcousticCheck_->setEnabled(enabled);
    usePhoneMicrophoneCheck_->setEnabled(enabled
                                         && phonePairingServer_.phoneConnected());
    phonePairButton_->setEnabled(enabled);
    exclusiveModeCheck_->setEnabled(enabled);
    refreshButton_->setEnabled(enabled);
    startButton_->setEnabled(enabled);
    calibrateButton_->setEnabled(enabled || calibrator_.isActive());

    for (QCheckBox* check : outputChecks_) {
        check->setEnabled(enabled);
    }
    for (auto iterator = delaySpinBoxes_.cbegin(); iterator != delaySpinBoxes_.cend(); ++iterator) {
        iterator.value()->setEnabled(
            enabled || (engine_.isActive() && lockedOutputIds_.contains(iterator.key())));
    }
    for (QSlider* volumeSlider : volumeSliders_) {
        volumeSlider->setEnabled(enabled || engine_.isActive());
    }
}

void MainWindow::applyTheme()
{
    themeButton_->setText(darkTheme_ ? QStringLiteral("切换浅色") : QStringLiteral("切换深色"));

    const char* lightStyle = R"QSS(
QWidget#Root { background: #f4f6fa; color: #182033; font-family: "Segoe UI", "Microsoft YaHei UI"; font-size: 13px; }
QWidget#Transparent { background: transparent; }
QLabel#Eyebrow { color: #6874e8; font-size: 10px; font-weight: 700; letter-spacing: 2px; }
QLabel#AppTitle { color: #111827; font-size: 27px; font-weight: 700; }
QLabel#SectionTitle { color: #172033; font-size: 15px; font-weight: 650; }
QLabel#MutedText { color: #778196; font-size: 12px; }
QLabel#ControlLabel { color: #39445a; font-weight: 600; }
QLabel#ValueLabel { color: #4c5870; font-weight: 600; }
QLabel#AcousticState { color: #557061; background: #edf8f3; border-radius: 6px; padding: 4px 7px; font-size: 11px; }
QLabel#ProgramLevel { color: #8a5b27; background: #fff6e8; border-radius: 7px; padding: 5px 8px; font-size: 11px; }
QLabel#ProgramLevel[active="true"] { color: #17765b; background: #eaf9f3; }
QLabel#PhoneStatus { color: #496174; background: #edf4f8; border-radius: 7px; padding: 6px 8px; }
QLabel#Tag, QLabel#CountBadge { color: #6570dc; background: #eef0ff; border: 1px solid #dfe2ff; border-radius: 9px; padding: 2px 8px; font-size: 10px; font-weight: 650; }
QLabel#AccentBadge { color: #4e5bd7; background: #eef0ff; border-radius: 10px; padding: 5px 10px; font-weight: 600; }
QLabel#SourceHint { color: #9a5b16; background: #fff7e8; border: 1px solid #f7dfb5; border-radius: 8px; padding: 8px 10px; }
QLabel#SourceHint[virtual="true"] { color: #17765b; background: #eaf9f3; border-color: #c7ebdd; }
QLabel#StatusLabel { color: #5f6b80; background: #edf0f5; border-radius: 11px; padding: 5px 11px; font-weight: 650; }
QLabel#StatusLabel[running="true"] { color: #187458; background: #e1f7ee; }
QFrame#Card, QFrame#ActionBar { background: #ffffff; border: 1px solid #e2e6ee; border-radius: 14px; }
QFrame#DeviceCard { background: #fafbfc; border: 1px solid #e6e9f0; border-radius: 10px; }
QFrame#DeviceCard[selected="true"] { background: #f3f5ff; border: 1px solid #7a83ee; }
QCheckBox#DeviceCheck { color: #20293a; font-size: 13px; font-weight: 600; spacing: 9px; }
QCheckBox { color: #39445a; spacing: 8px; }
QCheckBox::indicator { width: 17px; height: 17px; }
QComboBox, QSpinBox { color: #20293a; background: #f9fafc; border: 1px solid #d9dee8; border-radius: 8px; padding: 6px 10px; selection-background-color: #6672e7; }
QComboBox:hover, QSpinBox:hover { border-color: #9099ef; }
QComboBox:focus, QSpinBox:focus { border: 1px solid #6672e7; }
QPushButton, QToolButton { border-radius: 8px; padding: 0 15px; font-weight: 600; }
QPushButton#PrimaryButton { color: white; background: #626de3; border: 1px solid #626de3; }
QPushButton#PrimaryButton:hover { background: #5360d8; }
QPushButton#SecondaryButton, QToolButton#ThemeButton { color: #48536a; background: #ffffff; border: 1px solid #d9dee8; }
QPushButton#SecondaryButton:hover, QToolButton#ThemeButton:hover { background: #f5f6fa; border-color: #bcc3d1; }
QPushButton:disabled, QToolButton:disabled { color: #aab1bf; background: #f0f2f5; border-color: #e1e4ea; }
QSlider::groove:horizontal { height: 5px; background: #dfe3ec; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #6975e8; border-radius: 2px; }
QSlider::handle:horizontal { width: 15px; height: 15px; margin: -5px 0; background: #ffffff; border: 2px solid #6975e8; border-radius: 8px; }
QTextEdit#LogView { color: #3d475b; background: #f8f9fb; border: 1px solid #e2e6ed; border-radius: 9px; padding: 6px; font-size: 11px; }
QScrollArea#OutputScroll { background: transparent; border: none; }
QScrollBar:vertical { width: 8px; background: transparent; margin: 2px; }
QScrollBar::handle:vertical { min-height: 28px; background: #c8ceda; border-radius: 4px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
)QSS";

    const char* darkStyle = R"QSS(
QWidget#Root { background: #0e1320; color: #e8ecf5; font-family: "Segoe UI", "Microsoft YaHei UI"; font-size: 13px; }
QWidget#Transparent { background: transparent; }
QLabel#Eyebrow { color: #8d95ff; font-size: 10px; font-weight: 700; letter-spacing: 2px; }
QLabel#AppTitle { color: #f4f6fb; font-size: 27px; font-weight: 700; }
QLabel#SectionTitle { color: #edf0f7; font-size: 15px; font-weight: 650; }
QLabel#MutedText { color: #8d97aa; font-size: 12px; }
QLabel#ControlLabel { color: #c5ccda; font-weight: 600; }
QLabel#ValueLabel { color: #aeb7c8; font-weight: 600; }
QLabel#AcousticState { color: #83c9ae; background: #1a2c29; border-radius: 6px; padding: 4px 7px; font-size: 11px; }
QLabel#ProgramLevel { color: #d0a36b; background: #2c2419; border-radius: 7px; padding: 5px 8px; font-size: 11px; }
QLabel#ProgramLevel[active="true"] { color: #75d8b7; background: #172a27; }
QLabel#PhoneStatus { color: #9cc5d8; background: #182831; border-radius: 7px; padding: 6px 8px; }
QLabel#Tag, QLabel#CountBadge { color: #aeb3ff; background: #272d4e; border: 1px solid #343b64; border-radius: 9px; padding: 2px 8px; font-size: 10px; font-weight: 650; }
QLabel#AccentBadge { color: #b7bbff; background: #272d4e; border-radius: 10px; padding: 5px 10px; font-weight: 600; }
QLabel#SourceHint { color: #e4b26a; background: #2c2419; border: 1px solid #493921; border-radius: 8px; padding: 8px 10px; }
QLabel#SourceHint[virtual="true"] { color: #75d8b7; background: #172a27; border-color: #23483f; }
QLabel#StatusLabel { color: #a8b0c0; background: #252c39; border-radius: 11px; padding: 5px 11px; font-weight: 650; }
QLabel#StatusLabel[running="true"] { color: #79dbba; background: #1b3932; }
QFrame#Card, QFrame#ActionBar { background: #171d2a; border: 1px solid #262f40; border-radius: 14px; }
QFrame#DeviceCard { background: #1b2230; border: 1px solid #293243; border-radius: 10px; }
QFrame#DeviceCard[selected="true"] { background: #242a47; border: 1px solid #7f88f3; }
QCheckBox#DeviceCheck { color: #e7eaf1; font-size: 13px; font-weight: 600; spacing: 9px; }
QCheckBox { color: #c4cad6; spacing: 8px; }
QCheckBox::indicator { width: 17px; height: 17px; }
QComboBox, QSpinBox { color: #e8ebf2; background: #111722; border: 1px solid #303a4d; border-radius: 8px; padding: 6px 10px; selection-background-color: #727ceb; }
QComboBox:hover, QSpinBox:hover { border-color: #6872d8; }
QComboBox:focus, QSpinBox:focus { border: 1px solid #8189ee; }
QComboBox QAbstractItemView { color: #e8ebf2; background: #171d2a; border: 1px solid #303a4d; selection-background-color: #303861; }
QPushButton, QToolButton { border-radius: 8px; padding: 0 15px; font-weight: 600; }
QPushButton#PrimaryButton { color: white; background: #6974e8; border: 1px solid #6974e8; }
QPushButton#PrimaryButton:hover { background: #7781ef; }
QPushButton#SecondaryButton, QToolButton#ThemeButton { color: #d3d8e3; background: #1d2432; border: 1px solid #343e50; }
QPushButton#SecondaryButton:hover, QToolButton#ThemeButton:hover { background: #252d3d; border-color: #4b566c; }
QPushButton:disabled, QToolButton:disabled { color: #687184; background: #181e29; border-color: #272e3a; }
QSlider::groove:horizontal { height: 5px; background: #303849; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #7a84ef; border-radius: 2px; }
QSlider::handle:horizontal { width: 15px; height: 15px; margin: -5px 0; background: #e9ebff; border: 2px solid #7a84ef; border-radius: 8px; }
QTextEdit#LogView { color: #b8c0cf; background: #111722; border: 1px solid #293243; border-radius: 9px; padding: 6px; font-size: 11px; }
QScrollArea#OutputScroll { background: transparent; border: none; }
QScrollBar:vertical { width: 8px; background: transparent; margin: 2px; }
QScrollBar::handle:vertical { min-height: 28px; background: #3d475a; border-radius: 4px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
)QSS";

    setStyleSheet(QString::fromUtf8(darkTheme_ ? darkStyle : lightStyle));
}
