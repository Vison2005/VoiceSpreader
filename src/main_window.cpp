#include "main_window.h"

#include "level_meter_widget.h"
#include "pairing_qr_code.h"
#include "wasapi_device_manager.h"

#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QTextEdit>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace
{
QLabel* createSectionTitle(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("SectionTitle"));
    return label;
}

QToolButton* createHelpButton(const QString& toolTip, QWidget* parent)
{
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("HelpButton"));
    button->setText(QStringLiteral("?"));
    button->setToolTip(toolTip);
    button->setAccessibleName(QStringLiteral("查看说明"));
    button->setCursor(Qt::WhatsThisCursor);
    button->setFixedSize(20, 20);
    return button;
}

QIcon createPhoneStatusIcon(int connectionState, bool darkTheme, bool selected)
{
    QPixmap pixmap(40, 40);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor outline(selected || darkTheme ? QStringLiteral("#EAF4F8")
                                               : QStringLiteral("#112E81"));
    painter.setPen(QPen(outline, 1.4));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(4.0, 1.5, 10.5, 16.5), 2.0, 2.0);
    painter.drawLine(QPointF(7.0, 4.0), QPointF(11.5, 4.0));
    painter.drawEllipse(QPointF(9.25, 15.5), 0.7, 0.7);

    QColor statusColor(QStringLiteral("#98A6B5"));
    if (connectionState == 1) {
        statusColor = QColor(QStringLiteral("#38B26D"));
    } else if (connectionState == 2) {
        statusColor = QColor(QStringLiteral("#E05252"));
    }
    const QColor dotBorder(selected ? QStringLiteral("#112E81")
                                    : darkTheme ? QStringLiteral("#10284E")
                                                : QStringLiteral("#FFFFFF"));
    painter.setPen(QPen(dotBorder, 1.2));
    painter.setBrush(statusColor);
    painter.drawEllipse(QPointF(14.5, 14.0), 3.0, 3.0);
    return QIcon(pixmap);
}

QIcon createThemeIcon(bool darkTheme)
{
    QPixmap pixmap(40, 40);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor color(darkTheme ? QStringLiteral("#F6D365")
                                 : QStringLiteral("#112E81"));
    painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
    if (darkTheme) {
        painter.setBrush(color);
        painter.drawEllipse(QPointF(10.0, 10.0), 3.5, 3.5);
        for (int index = 0; index < 8; ++index) {
            const double angle = index * 3.14159265358979323846 / 4.0;
            const QPointF inner(10.0 + std::cos(angle) * 6.0,
                                10.0 + std::sin(angle) * 6.0);
            const QPointF outer(10.0 + std::cos(angle) * 8.0,
                                10.0 + std::sin(angle) * 8.0);
            painter.drawLine(inner, outer);
        }
    } else {
        QPainterPath moon;
        moon.addEllipse(QRectF(3.0, 2.0, 14.0, 16.0));
        QPainterPath cutout;
        cutout.addEllipse(QRectF(8.0, 0.0, 12.0, 14.0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPath(moon.subtracted(cutout));
    }
    return QIcon(pixmap);
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
    setWindowTitle(QStringLiteral("VoiceSpreader"));
    setWindowIcon(QIcon(QStringLiteral(":/assets/app.png")));
    setMinimumSize(600, 360);
    QSize initialSize(1120, 760);
    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        const QSize available = screen->availableGeometry().size();
        initialSize.setWidth(std::max(minimumWidth(),
                                      std::min(initialSize.width(), available.width() - 24)));
        initialSize.setHeight(std::max(minimumHeight(),
                                       std::min(initialSize.height(), available.height() - 24)));
    }
    resize(initialSize);

    auto* root = new QWidget(this);
    root->setObjectName(QStringLiteral("Root"));
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(24, 16, 24, 18);
    rootLayout->setSpacing(14);

    auto* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(10);

    auto* brandIcon = new QLabel(root);
    brandIcon->setObjectName(QStringLiteral("BrandIcon"));
    brandIcon->setFixedSize(50, 50);
    brandIcon->setAlignment(Qt::AlignCenter);
    brandIcon->setPixmap(
        QPixmap(QStringLiteral(":/assets/app.png")).scaled(
            44, 44, Qt::KeepAspectRatio, Qt::SmoothTransformation));

    titleLabel_ = new QLabel(QStringLiteral("VoiceSpreader"), root);
    titleLabel_->setObjectName(QStringLiteral("AppTitle"));

    phoneLevelMeter_ = new LevelMeterWidget(root);

    phoneMicrophoneButton_ = new QToolButton(root);
    phoneMicrophoneButton_->setObjectName(QStringLiteral("PhoneButton"));
    phoneMicrophoneButton_->setText(QStringLiteral("连接手机"));
    phoneMicrophoneButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    phoneMicrophoneButton_->setIconSize(QSize(20, 20));
    phoneMicrophoneButton_->setCheckable(true);
    phoneMicrophoneButton_->setCursor(Qt::PointingHandCursor);
    phoneMicrophoneButton_->setMinimumSize(108, 38);
    phoneMicrophoneButton_->setContextMenuPolicy(Qt::CustomContextMenu);

    themeButton_ = new QToolButton(root);
    themeButton_->setObjectName(QStringLiteral("ThemeButton"));
    themeButton_->setIconSize(QSize(20, 20));
    themeButton_->setCursor(Qt::PointingHandCursor);
    themeButton_->setFixedSize(42, 38);

    headerLayout->addWidget(brandIcon, 0, Qt::AlignVCenter);
    headerLayout->addWidget(titleLabel_, 0, Qt::AlignVCenter);
    headerLayout->addStretch();
    headerLayout->addWidget(phoneLevelMeter_, 0, Qt::AlignVCenter);
    headerLayout->addWidget(phoneMicrophoneButton_, 0, Qt::AlignVCenter);
    headerLayout->addWidget(themeButton_, 0, Qt::AlignVCenter);
    rootLayout->addLayout(headerLayout);

    auto* contentScroll = new QScrollArea(root);
    contentScroll->setObjectName(QStringLiteral("ContentScroll"));
    contentScroll->setWidgetResizable(true);
    contentScroll->setFrameShape(QFrame::NoFrame);
    contentScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* contentContainer = new QWidget(contentScroll);
    contentContainer->setObjectName(QStringLiteral("Transparent"));
    contentLayout_ = new QBoxLayout(QBoxLayout::LeftToRight, contentContainer);
    contentLayout_->setContentsMargins(0, 0, 0, 0);
    contentLayout_->setSpacing(16);

    auto* leftColumn = new QWidget(contentContainer);
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
    sourceHeader->addWidget(createHelpButton(
        QStringLiteral("物理源模式：该设备由 Windows 直接发声，副输出无法比它更早；明显不同步时建议改用虚拟音频源。"),
        sourceCard));
    sourceHeader->addStretch();
    sourceLayout->addLayout(sourceHeader);

    sourceControlsLayout_ = new QBoxLayout(QBoxLayout::LeftToRight);
    sourceControlsLayout_->setSpacing(8);
    captureCombo_ = new QComboBox(sourceCard);
    captureCombo_->setMinimumHeight(38);
    refreshButton_ = new QPushButton(QStringLiteral("刷新设备"), sourceCard);
    refreshButton_->setObjectName(QStringLiteral("SecondaryButton"));
    refreshButton_->setCursor(Qt::PointingHandCursor);
    refreshButton_->setMinimumHeight(38);
    sourceControlsLayout_->addWidget(captureCombo_, 1);
    sourceControlsLayout_->addWidget(refreshButton_);
    sourceLayout->addLayout(sourceControlsLayout_);

    auto* calibrationHeader = new QHBoxLayout();
    auto* calibrationTitle = new QLabel(QStringLiteral("校准麦克风"), sourceCard);
    calibrationTitle->setObjectName(QStringLiteral("ControlLabel"));
    calibrationHeader->addWidget(calibrationTitle);
    calibrationHeader->addWidget(createHelpButton(
        QStringLiteral("把麦克风放在听音位置；校准会依次播放短扫频并自动回填设备补偿。"),
        sourceCard));
    calibrationHeader->addStretch();
    sourceLayout->addLayout(calibrationHeader);

    calibrationControlsLayout_ = new QBoxLayout(QBoxLayout::LeftToRight);
    calibrationControlsLayout_->setSpacing(8);
    microphoneCombo_ = new QComboBox(sourceCard);
    microphoneCombo_->setMinimumHeight(38);
    calibrateButton_ = new QPushButton(QStringLiteral("自动校准"), sourceCard);
    calibrateButton_->setObjectName(QStringLiteral("SecondaryButton"));
    calibrateButton_->setCursor(Qt::PointingHandCursor);
    calibrateButton_->setMinimumHeight(38);
    calibrationControlsLayout_->addWidget(microphoneCombo_, 1);
    calibrationControlsLayout_->addWidget(calibrateButton_);
    sourceLayout->addLayout(calibrationControlsLayout_);

    calibrationHintLabel_ = new QLabel(sourceCard);
    calibrationHintLabel_->setObjectName(QStringLiteral("CalibrationStatus"));
    calibrationHintLabel_->setWordWrap(true);
    calibrationHintLabel_->setVisible(false);
    sourceLayout->addWidget(calibrationHintLabel_);
    leftLayout->addWidget(sourceCard);

    auto* outputCard = new QFrame(leftColumn);
    outputCard->setObjectName(QStringLiteral("Card"));
    outputCard->setMinimumHeight(280);
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

    rightColumn_ = new QWidget(contentContainer);
    rightColumn_->setObjectName(QStringLiteral("Transparent"));
    rightColumn_->setMinimumWidth(330);
    rightColumn_->setMaximumWidth(390);
    auto* rightLayout = new QVBoxLayout(rightColumn_);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(14);

    auto* syncCard = new QFrame(rightColumn_);
    syncCard->setObjectName(QStringLiteral("Card"));
    auto* syncLayout = new QVBoxLayout(syncCard);
    syncLayout->setContentsMargins(18, 16, 18, 16);
    syncLayout->setSpacing(12);
    syncLayout->addWidget(createSectionTitle(QStringLiteral("同步设置"), syncCard));

    auto* bufferLabelLayout = new QHBoxLayout();
    auto* bufferTitle = new QLabel(QStringLiteral("全局延迟"), syncCard);
    bufferTitle->setObjectName(QStringLiteral("ControlLabel"));
    bufferLabelLayout->addWidget(bufferTitle);
    bufferLabelLayout->addWidget(createHelpButton(
        QStringLiteral("播放中也可调整。建议从 5 ms 开始；若有爆音，每次增加 2–5 ms。"),
        syncCard));
    bufferSpin_ = new QSpinBox(syncCard);
    bufferSpin_->setRange(2, 100);
    bufferSpin_->setSingleStep(1);
    bufferSpin_->setValue(5);
    bufferSpin_->setSuffix(QStringLiteral(" ms"));
    bufferSpin_->setFixedWidth(116);
    bufferSpin_->setMinimumHeight(36);
    bufferLabelLayout->addStretch();
    bufferLabelLayout->addWidget(bufferSpin_);
    syncLayout->addLayout(bufferLabelLayout);

    bufferSlider_ = new QSlider(Qt::Horizontal, syncCard);
    bufferSlider_->setRange(2, 100);
    bufferSlider_->setValue(5);
    bufferSlider_->setSingleStep(1);
    syncLayout->addWidget(bufferSlider_);

    auto* optionsLayout = new QHBoxLayout();
    optionsLayout->setSpacing(6);

    automaticLatencyCheck_ = new QCheckBox(QStringLiteral("自动补偿"), syncCard);
    automaticLatencyCheck_->setChecked(true);
    auto* automaticHelp = createHelpButton(
        QStringLiteral("根据 Windows 音频端点报告的流延迟，自动延后较快设备；手动补偿仍会叠加。"),
        syncCard);
    auto* automaticLayout = new QHBoxLayout();
    automaticLayout->setSpacing(2);
    automaticLayout->addWidget(automaticLatencyCheck_);
    automaticLayout->addWidget(automaticHelp);
    optionsLayout->addLayout(automaticLayout);

    continuousAcousticCheck_ = new QCheckBox(QStringLiteral("自同步"), syncCard);
    continuousAcousticCheck_->setChecked(true);
    auto* acousticHelp = createHelpButton(
        QStringLiteral("播放中使用自适应声学探针复核设备间漂移；仅在节目声足以掩蔽探针时发送。"),
        syncCard);
    auto* acousticLayout = new QHBoxLayout();
    acousticLayout->setSpacing(2);
    acousticLayout->addWidget(continuousAcousticCheck_);
    acousticLayout->addWidget(acousticHelp);
    optionsLayout->addLayout(acousticLayout);

    exclusiveModeCheck_ = new QCheckBox(QStringLiteral("独占输出"), syncCard);
    exclusiveModeCheck_->setChecked(false);
    auto* exclusiveHelp = createHelpButton(
        QStringLiteral("绕过共享混音以降低输出延迟；同一驱动的多个端点可能互斥，仅建议独立物理声卡使用。"),
        syncCard);
    auto* exclusiveLayout = new QHBoxLayout();
    exclusiveLayout->setSpacing(2);
    exclusiveLayout->addWidget(exclusiveModeCheck_);
    exclusiveLayout->addWidget(exclusiveHelp);
    optionsLayout->addLayout(exclusiveLayout);
    optionsLayout->addStretch();
    syncLayout->addLayout(optionsLayout);

    programLevelLabel_ = new QLabel(
        QStringLiteral("实时节目电平：未监测 · 探针暂停"), syncCard);
    programLevelLabel_->setObjectName(QStringLiteral("ProgramLevel"));
    programLevelLabel_->setProperty("active", false);
    syncLayout->addWidget(programLevelLabel_);
    rightLayout->addWidget(syncCard);

    auto* logCard = new QFrame(rightColumn_);
    logCard->setObjectName(QStringLiteral("Card"));
    logCard->setMinimumHeight(190);
    auto* logLayout = new QVBoxLayout(logCard);
    logLayout->setContentsMargins(18, 16, 18, 16);
    logLayout->setSpacing(10);
    logLayout->addWidget(createSectionTitle(QStringLiteral("运行日志"), logCard));
    logView_ = new QTextEdit(logCard);
    logView_->setObjectName(QStringLiteral("LogView"));
    logView_->setReadOnly(true);
    logLayout->addWidget(logView_, 1);
    rightLayout->addWidget(logCard, 1);

    contentLayout_->addWidget(leftColumn, 1);
    contentLayout_->addWidget(rightColumn_);
    contentScroll->setWidget(contentContainer);
    rootLayout->addWidget(contentScroll, 1);

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
    startButton_->setMinimumSize(140, 42);
    startButton_->setProperty("running", false);
    actionLayout->addWidget(stateLabel_);
    actionLayout->addWidget(stateHint);
    actionLayout->addStretch();
    actionLayout->addWidget(startButton_);
    rootLayout->addWidget(actionBar);

    setCentralWidget(root);

    connect(themeButton_, &QToolButton::clicked, this, &MainWindow::toggleTheme);
    connect(refreshButton_, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(captureCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::rebuildOutputCards);
    connect(bufferSlider_, &QSlider::valueChanged, bufferSpin_, &QSpinBox::setValue);
    connect(bufferSpin_, qOverload<int>(&QSpinBox::valueChanged), bufferSlider_, &QSlider::setValue);
    connect(bufferSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int value) {
                engine_.setSynchronizationMargin(value);
            });
    connect(startButton_, &QPushButton::clicked, this, [this] {
        if (engine_.isActive()) {
            stopAudio();
        } else {
            startAudio();
        }
    });
    connect(calibrateButton_, &QPushButton::clicked,
            this, &MainWindow::startOrStopCalibration);
    connect(&engine_, &AudioEngine::statusChanged, this, &MainWindow::appendStatus);
    connect(&engine_, &AudioEngine::errorOccurred, this, &MainWindow::showEngineError);
    connect(&engine_, &AudioEngine::runningChanged, this, &MainWindow::updateRunningState);
    connect(&engine_, &AudioEngine::acousticCorrectionChanged,
            this, &MainWindow::updateAcousticCorrection);
    connect(&engine_, &AudioEngine::programLevelChanged,
            this, &MainWindow::updateProgramLevel);
    connect(phoneMicrophoneButton_, &QToolButton::clicked, this,
            [this](bool checked) {
                if (engine_.isActive() || calibrator_.isActive()) {
                    const QSignalBlocker blocker(phoneMicrophoneButton_);
                    phoneMicrophoneButton_->setChecked(!checked);
                    QToolTip::showText(
                        phoneMicrophoneButton_->mapToGlobal(
                            QPoint(phoneMicrophoneButton_->width() / 2,
                                   phoneMicrophoneButton_->height())),
                        QStringLiteral("请先停止同步或校准，再切换手机麦克风。"),
                        phoneMicrophoneButton_);
                    updatePhoneButtonAppearance();
                    return;
                }
                if (phoneConnectionState_ == 1) {
                    updatePhoneButtonAppearance();
                    return;
                }
                const QSignalBlocker blocker(phoneMicrophoneButton_);
                phoneMicrophoneButton_->setChecked(false);
                showPhonePairing();
            });
    connect(phoneMicrophoneButton_, &QToolButton::customContextMenuRequested,
            this, [this](const QPoint& position) {
                QMenu menu(this);
                QAction* disconnectAction = menu.addAction(
                    createPhoneStatusIcon(2, darkTheme_, false),
                    QStringLiteral("断开手机连接"));
                disconnectAction->setEnabled(phoneConnectionState_ == 1);
                if (menu.exec(phoneMicrophoneButton_->mapToGlobal(position))
                    == disconnectAction) {
                    phonePairingServer_.disconnectPhone();
                }
            });
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
        phoneConnectionState_ = 0;
    } else {
        phoneConnectionState_ = 2;
        appendStatus(QStringLiteral("手机配对服务启动失败：%1").arg(pairingError));
    }
    updatePhoneButtonAppearance();
    updateResponsiveLayout();
}

MainWindow::~MainWindow()
{
    calibrator_.stop();
    engine_.stop();
    phonePairingServer_.stop();
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    updateResponsiveLayout();
}

void MainWindow::updateResponsiveLayout()
{
    if (contentLayout_ == nullptr || rightColumn_ == nullptr
        || sourceControlsLayout_ == nullptr
        || calibrationControlsLayout_ == nullptr) {
        return;
    }

    const bool compact = width() < 940;
    const bool narrow = width() < 720;
    titleLabel_->setVisible(!narrow);
    sourceControlsLayout_->setDirection(narrow ? QBoxLayout::TopToBottom
                                               : QBoxLayout::LeftToRight);
    calibrationControlsLayout_->setDirection(narrow ? QBoxLayout::TopToBottom
                                                    : QBoxLayout::LeftToRight);
    refreshButton_->setSizePolicy(
        narrow ? QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed)
               : QSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed));
    calibrateButton_->setSizePolicy(
        narrow ? QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed)
               : QSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed));
    if (compactLayout_ == compact
        && contentLayout_->direction()
               == (compact ? QBoxLayout::TopToBottom
                           : QBoxLayout::LeftToRight)) {
        return;
    }

    compactLayout_ = compact;
    contentLayout_->setDirection(compact ? QBoxLayout::TopToBottom
                                         : QBoxLayout::LeftToRight);
    contentLayout_->setStretch(0, compact ? 0 : 1);
    contentLayout_->setStretch(1, 0);
    rightColumn_->setMinimumWidth(compact ? 0 : 330);
    rightColumn_->setMaximumWidth(compact ? QWIDGETSIZE_MAX : 390);
    rightColumn_->setSizePolicy(
        compact ? QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred)
                : QSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding));
    contentLayout_->invalidate();
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
        delaySpin->setFixedWidth(116);
        delaySpin->setMinimumHeight(34);
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
                                    && phoneMicrophoneButton_->isChecked()
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
    startButton_->setEnabled(true);
    startButton_->setText(QStringLiteral("停止同步"));
    startButton_->setProperty("running", true);
    refreshDynamicStyle(startButton_);
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
    startButton_->setEnabled(false);
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
    stateLabel_->setText(QStringLiteral("声学校准中"));
    stateLabel_->setProperty("running", true);
    refreshDynamicStyle(stateLabel_);
    calibrationHintLabel_->setText(
        QStringLiteral("正在测量：请保持环境安静，不要移动麦克风。"));
    calibrationHintLabel_->setVisible(true);
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
    calibrationHintLabel_->setVisible(true);
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
}

void MainWindow::showCalibrationError(const QString& message)
{
    appendStatus(QStringLiteral("校准错误：%1").arg(message));
    calibrationHintLabel_->setText(
        QStringLiteral("校准失败：请检查麦克风、扬声器音量和环境噪声后重试。"));
    calibrationHintLabel_->setVisible(true);
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
            phoneConnectionState_ = 2;
            updatePhoneButtonAppearance();
            QMessageBox::critical(this,
                                  QStringLiteral("手机配对失败"),
                                  QStringLiteral("无法启动配对服务：%1").arg(error));
            return;
        }
        phoneConnectionState_ = 0;
        updatePhoneButtonAppearance();
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("VoiceSpreader 手机配对"));
    dialog.setMinimumWidth(430);
    auto* layout = new QVBoxLayout(&dialog);
    auto* title = new QLabel(QStringLiteral("用 VoiceSpreader Android 扫描二维码"), &dialog);
    title->setObjectName(QStringLiteral("SectionTitle"));
    layout->addWidget(title, 0, Qt::AlignHCenter);

    auto* addressControls = new QHBoxLayout();
    auto* addressTitle = new QLabel(QStringLiteral("二维码电脑地址"), &dialog);
    auto* addressCombo = new QComboBox(&dialog);
    addressCombo->addItems(phonePairingServer_.localIpv4Addresses());
    addressCombo->setCurrentText(phonePairingServer_.localAddress());
    addressCombo->setMinimumHeight(34);
    addressControls->addWidget(addressTitle);
    addressControls->addWidget(addressCombo, 1);
    layout->addLayout(addressControls);

    auto* qrLabel = new QLabel(&dialog);
    qrLabel->setObjectName(QStringLiteral("QrCodeSurface"));
    qrLabel->setAlignment(Qt::AlignCenter);
    qrLabel->setContentsMargins(14, 14, 14, 14);
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
    codeLabel->setObjectName(QStringLiteral("PairingCode"));
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
    connect(addressCombo, &QComboBox::currentTextChanged, &dialog,
            [&](const QString& address) {
                if (!phonePairingServer_.setLocalAddress(address)) {
                    return;
                }
                refreshQr();
                addressLabel->setText(
                    QStringLiteral("电脑地址 %1:%2 · 手机和电脑必须连接同一局域网")
                        .arg(phonePairingServer_.localAddress())
                        .arg(phonePairingServer_.serverPort()));
            });

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
        updatePhoneButtonAppearance();
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
        phoneConnectionState_ = 1;
        connectedPhoneName_ = phoneName;
        phoneMicrophoneLevelDbfs_ = -160.0;
        phoneMicrophoneButton_->setChecked(false);
    } else {
        phoneConnectionState_ = 0;
        connectedPhoneName_.clear();
        phoneMicrophoneLevelDbfs_ = -160.0;
        phoneMicrophoneButton_->setChecked(false);
    }
    phoneLevelMeter_->setConnected(connected);
    phoneLevelMeter_->setLevelDbfs(phoneMicrophoneLevelDbfs_);
    updatePhoneButtonAppearance();
}

void MainWindow::updatePhoneMicrophoneLevel(double levelDbfs)
{
    phoneMicrophoneLevelDbfs_ = levelDbfs;
    phoneLevelMeter_->setLevelDbfs(levelDbfs);
    updatePhoneButtonAppearance();
}

void MainWindow::updatePhoneButtonAppearance()
{
    phoneMicrophoneButton_->setText(
        phoneConnectionState_ == 1
            ? QStringLiteral("已连接")
            : phoneConnectionState_ == 2
                ? QStringLiteral("异常")
                : QStringLiteral("连接手机"));
    phoneMicrophoneButton_->setIcon(
        createPhoneStatusIcon(phoneConnectionState_,
                              darkTheme_,
                              phoneMicrophoneButton_->isChecked()));
    phoneMicrophoneButton_->setProperty("connectionState", phoneConnectionState_);

    if (phoneConnectionState_ == 1) {
        const QString levelText = phoneMicrophoneLevelDbfs_ <= -150.0
                                      ? QStringLiteral("等待采样")
                                      : QStringLiteral("%1 dBFS")
                                            .arg(phoneMicrophoneLevelDbfs_, 0, 'f', 1);
        phoneMicrophoneButton_->setToolTip(
            QStringLiteral("已连接：%1\n手机麦克风：%2\n%3")
                .arg(connectedPhoneName_,
                     levelText,
                     phoneMicrophoneButton_->isChecked()
                         ? QStringLiteral("当前用于自同步；点击停用")
                         : QStringLiteral("当前未使用；点击启用")));
    } else if (phoneConnectionState_ == 2) {
        phoneMicrophoneButton_->setToolTip(
            QStringLiteral("手机配对服务异常；点击重试。"));
    } else {
        phoneMicrophoneButton_->setToolTip(
            QStringLiteral("手机未连接；点击打开二维码和配对码。"));
    }

    // 保持按钮可用，确保同步过程中仍能通过右键主动断开手机。
    phoneMicrophoneButton_->setEnabled(true);
    refreshDynamicStyle(phoneMicrophoneButton_);
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
    const bool active = running || engine_.isActive();
    stateLabel_->setText(running ? QStringLiteral("正在同步") : QStringLiteral("未启动"));
    stateLabel_->setProperty("running", running);
    refreshDynamicStyle(stateLabel_);
    setControlsEnabled(!active);
    startButton_->setText(active ? QStringLiteral("停止同步") : QStringLiteral("开始同步"));
    startButton_->setProperty("running", active);
    startButton_->setEnabled(active || !calibrator_.isActive());
    refreshDynamicStyle(startButton_);

    if (!active) {
        lockedOutputIds_.clear();
        setControlsEnabled(true);
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
    // 左键切换会在运行中被拦截，但右键断开必须始终可用。
    phoneMicrophoneButton_->setEnabled(true);
    exclusiveModeCheck_->setEnabled(enabled);
    refreshButton_->setEnabled(enabled);
    startButton_->setEnabled(enabled || engine_.isActive());
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
    themeButton_->setText(QString());
    themeButton_->setIcon(createThemeIcon(darkTheme_));
    themeButton_->setToolTip(darkTheme_ ? QStringLiteral("切换到浅色模式")
                                        : QStringLiteral("切换到深色模式"));
    phoneLevelMeter_->setDarkTheme(darkTheme_);

    QPalette palette;
    if (darkTheme_) {
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#07142F")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#EAF4F8")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#091A38")));
        palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#0D2247")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#EAF4F8")));
        palette.setColor(QPalette::Button, QColor(QStringLiteral("#10284E")));
        palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#EAF4F8")));
        palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#4382DF")));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::Link, QColor(QStringLiteral("#77A9EE")));
        palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#10284E")));
        palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#EAF4F8")));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#6E86A4")));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#6E86A4")));
    } else {
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#F3F7FB")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#13213F")));
        palette.setColor(QPalette::Base, Qt::white);
        palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#EDF4FA")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#13213F")));
        palette.setColor(QPalette::Button, Qt::white);
        palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#112E81")));
        palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#4382DF")));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::Link, QColor(QStringLiteral("#4382DF")));
        palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#112E81")));
        palette.setColor(QPalette::ToolTipText, Qt::white);
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#8B9DB0")));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#8B9DB0")));
    }
    qApp->setPalette(palette);

    const char* lightStyle = R"QSS(
QWidget { color: #13213F; font-family: "HarmonyOS Sans SC"; font-size: 13px; }
QMainWindow, QDialog, QWidget#Root { background: #F3F7FB; }
QWidget#Transparent { background: transparent; }
QLabel#BrandIcon { background: #E9F2FC; border: 1px solid #AACCD6; border-radius: 13px; }
QLabel#AppTitle { color: #112E81; font-size: 27px; font-weight: 700; }
QLabel#SectionTitle { color: #112E81; font-size: 15px; font-weight: 700; }
QLabel#MutedText { color: #627694; font-size: 12px; }
QLabel#CalibrationStatus { color: #112E81; background: #EEF5FC; border-radius: 7px; padding: 6px 8px; font-size: 11px; }
QLabel#ControlLabel { color: #314668; font-weight: 500; }
QLabel#ValueLabel { color: #112E81; font-weight: 500; }
QLabel#AcousticState { color: #284F7D; background: #E9F2FC; border: 1px solid #D6E7F0; border-radius: 6px; padding: 4px 7px; font-size: 11px; }
QLabel#ProgramLevel { color: #49647E; background: #EDF3F7; border-radius: 7px; padding: 5px 8px; font-size: 11px; }
QLabel#ProgramLevel[active="true"] { color: #112E81; background: #E4EEFC; }
QLabel#CountBadge { color: #4647AE; background: #EEF0FF; border: 1px solid #D7DCF5; border-radius: 9px; padding: 2px 8px; font-size: 10px; font-weight: 700; }
QLabel#StatusLabel { color: #355270; background: #EAF1F6; border-radius: 11px; padding: 5px 11px; font-weight: 700; }
QLabel#StatusLabel[running="true"] { color: #112E81; background: #DDEBFB; }
QFrame#Card, QFrame#ActionBar { background: #FFFFFF; border: 1px solid #D8E5ED; border-radius: 14px; }
QFrame#DeviceCard { background: #F8FBFD; border: 1px solid #DFE9EF; border-radius: 10px; }
QFrame#DeviceCard[selected="true"] { background: #EAF2FE; border: 1px solid #4382DF; }
QCheckBox#DeviceCheck { color: #162C54; font-size: 13px; font-weight: 500; spacing: 9px; }
QCheckBox { color: #314668; spacing: 8px; }
QCheckBox::indicator { width: 17px; height: 17px; }
QComboBox, QSpinBox { color: #162C54; background: #FFFFFF; border: 1px solid #C8D9E4; border-radius: 8px; padding: 6px 34px 6px 12px; selection-background-color: #4382DF; }
QComboBox:hover, QSpinBox:hover { border-color: #4382DF; }
QComboBox:focus, QSpinBox:focus { border: 1px solid #4647AE; }
QComboBox::drop-down { subcontrol-origin: border; subcontrol-position: top right; width: 34px; background: #EEF5FC; border: none; border-left: 1px solid #D8E5ED; border-top-right-radius: 8px; border-bottom-right-radius: 8px; }
QComboBox::drop-down:hover { background: #DDEBFB; }
QComboBox::down-arrow { image: url(:/controls/chevron_down_light.svg); width: 12px; height: 8px; }
QComboBox QAbstractItemView { color: #162C54; background: #FFFFFF; border: 1px solid #AACCD6; selection-background-color: #4382DF; selection-color: #FFFFFF; outline: none; }
QSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 30px; height: 17px; background: #EEF5FC; border: none; border-left: 1px solid #D8E5ED; border-bottom: 1px solid #D8E5ED; border-top-right-radius: 8px; }
QSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 30px; height: 17px; background: #EEF5FC; border: none; border-left: 1px solid #D8E5ED; border-bottom-right-radius: 8px; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: #DDEBFB; }
QSpinBox::up-arrow { image: url(:/controls/chevron_up_light.svg); width: 12px; height: 8px; }
QSpinBox::down-arrow { image: url(:/controls/chevron_down_light.svg); width: 12px; height: 8px; }
QPushButton, QToolButton { border-radius: 8px; padding: 0 15px; font-weight: 500; }
QPushButton#PrimaryButton { color: #FFFFFF; background: #112E81; border: 1px solid #112E81; }
QPushButton#PrimaryButton:hover { background: #4647AE; border-color: #4647AE; }
QPushButton#PrimaryButton:pressed { background: #0D246A; border-color: #0D246A; }
QPushButton#PrimaryButton[running="true"] { background: #4647AE; border-color: #4647AE; }
QPushButton#SecondaryButton, QToolButton#ThemeButton { color: #112E81; background: #FFFFFF; border: 1px solid #AACCD6; }
QPushButton#SecondaryButton:hover, QToolButton#ThemeButton:hover { background: #EAF2FA; border-color: #4382DF; }
QToolButton#ThemeButton { border-radius: 10px; padding: 0; font-size: 18px; }
QToolButton#PhoneButton { color: #112E81; background: #FFFFFF; border: 1px solid #AACCD6; border-radius: 10px; padding: 0 10px; font-weight: 500; }
QToolButton#PhoneButton:hover { background: #EAF2FA; border-color: #4382DF; }
QToolButton#PhoneButton:checked { color: #FFFFFF; background: #112E81; border-color: #112E81; }
QToolButton#PhoneButton:checked:disabled { color: #FFFFFF; background: #274A91; border-color: #274A91; }
QToolButton#PhoneButton[connectionState="2"] { border-color: #E05252; }
QToolButton#HelpButton { color: #4382DF; background: #F5F9FD; border: 1px solid #AACCD6; border-radius: 10px; padding: 0; font-size: 11px; font-weight: 700; }
QToolButton#HelpButton:hover { color: #FFFFFF; background: #4382DF; border-color: #4382DF; }
QPushButton:disabled, QToolButton:disabled { color: #8B9DB0; background: #EDF2F5; border-color: #D9E2E8; }
QSlider::groove:horizontal { height: 7px; background: #DCE8EF; border-radius: 4px; }
QSlider::sub-page:horizontal { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #4647AE, stop:1 #4382DF); border-radius: 4px; }
QSlider::handle:horizontal { image: url(:/controls/slider_handle_light.svg); width: 20px; height: 20px; margin: -7px 0; background: transparent; border: none; }
QTextEdit#LogView { color: #314668; background: #F7FAFC; border: 1px solid #D8E5ED; border-radius: 9px; padding: 7px; font-size: 11px; }
QLabel#QrCodeSurface { background: #FFFFFF; border: 1px solid #AACCD6; border-radius: 14px; }
QLabel#PairingCode { color: #112E81; background: #EAF2FA; border-radius: 9px; padding: 8px; }
QScrollArea#OutputScroll, QScrollArea#ContentScroll { background: transparent; border: none; }
QScrollArea#OutputScroll > QWidget > QWidget, QScrollArea#ContentScroll > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { width: 8px; background: transparent; margin: 2px; }
QScrollBar::handle:vertical { min-height: 28px; background: #AACCD6; border-radius: 4px; }
QScrollBar::handle:vertical:hover { background: #7FA8C4; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QToolTip { color: #FFFFFF; background: #112E81; border: 1px solid #4382DF; padding: 5px; }
)QSS";

    const char* darkStyle = R"QSS(
QWidget { color: #EAF4F8; font-family: "HarmonyOS Sans SC"; font-size: 13px; }
QMainWindow, QDialog, QWidget#Root { background: #07142F; }
QWidget#Transparent { background: transparent; }
QLabel#BrandIcon { background: #102A57; border: 1px solid #315D8F; border-radius: 13px; }
QLabel#AppTitle { color: #F2F7FA; font-size: 27px; font-weight: 700; }
QLabel#SectionTitle { color: #DCEAF2; font-size: 15px; font-weight: 700; }
QLabel#MutedText { color: #AACCD6; font-size: 12px; }
QLabel#CalibrationStatus { color: #C7DDF7; background: #102A50; border-radius: 7px; padding: 6px 8px; font-size: 11px; }
QLabel#ControlLabel { color: #C5D9E2; font-weight: 500; }
QLabel#ValueLabel { color: #AFCDF7; font-weight: 500; }
QLabel#AcousticState { color: #BBD5E1; background: #10284E; border: 1px solid #1E4773; border-radius: 6px; padding: 4px 7px; font-size: 11px; }
QLabel#ProgramLevel { color: #AACCD6; background: #102542; border-radius: 7px; padding: 5px 8px; font-size: 11px; }
QLabel#ProgramLevel[active="true"] { color: #D9E9FF; background: #19376F; }
QLabel#CountBadge { color: #C6C7FF; background: #24265E; border: 1px solid #4647AE; border-radius: 9px; padding: 2px 8px; font-size: 10px; font-weight: 700; }
QLabel#StatusLabel { color: #AACCD6; background: #102542; border-radius: 11px; padding: 5px 11px; font-weight: 700; }
QLabel#StatusLabel[running="true"] { color: #EAF4F8; background: #112E81; }
QFrame#Card, QFrame#ActionBar { background: #0D1F3E; border: 1px solid #1E3D64; border-radius: 14px; }
QFrame#DeviceCard { background: #102442; border: 1px solid #1D3B5D; border-radius: 10px; }
QFrame#DeviceCard[selected="true"] { background: #112E81; border: 1px solid #6B9FE9; }
QCheckBox#DeviceCheck { color: #EDF5F8; font-size: 13px; font-weight: 500; spacing: 9px; }
QCheckBox { color: #C5D9E2; spacing: 8px; }
QCheckBox::indicator { width: 17px; height: 17px; }
QComboBox, QSpinBox { color: #EAF4F8; background: #091A38; border: 1px solid #315377; border-radius: 8px; padding: 6px 34px 6px 12px; selection-background-color: #4382DF; }
QComboBox:hover, QSpinBox:hover { border-color: #5D95E6; }
QComboBox:focus, QSpinBox:focus { border: 1px solid #4382DF; }
QComboBox::drop-down { subcontrol-origin: border; subcontrol-position: top right; width: 34px; background: #10284E; border: none; border-left: 1px solid #234A73; border-top-right-radius: 8px; border-bottom-right-radius: 8px; }
QComboBox::drop-down:hover { background: #173662; }
QComboBox::down-arrow { image: url(:/controls/chevron_down_dark.svg); width: 12px; height: 8px; }
QComboBox QAbstractItemView { color: #EAF4F8; background: #0D2247; border: 1px solid #315377; selection-background-color: #4382DF; selection-color: #FFFFFF; outline: none; }
QSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 30px; height: 17px; background: #10284E; border: none; border-left: 1px solid #234A73; border-bottom: 1px solid #234A73; border-top-right-radius: 8px; }
QSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 30px; height: 17px; background: #10284E; border: none; border-left: 1px solid #234A73; border-bottom-right-radius: 8px; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: #173662; }
QSpinBox::up-arrow { image: url(:/controls/chevron_up_dark.svg); width: 12px; height: 8px; }
QSpinBox::down-arrow { image: url(:/controls/chevron_down_dark.svg); width: 12px; height: 8px; }
QPushButton, QToolButton { border-radius: 8px; padding: 0 15px; font-weight: 500; }
QPushButton#PrimaryButton { color: #FFFFFF; background: #4382DF; border: 1px solid #4382DF; }
QPushButton#PrimaryButton:hover { background: #5B93E5; border-color: #5B93E5; }
QPushButton#PrimaryButton:pressed { background: #4647AE; border-color: #4647AE; }
QPushButton#PrimaryButton[running="true"] { background: #4647AE; border-color: #6C6DD0; }
QPushButton#SecondaryButton, QToolButton#ThemeButton { color: #DCEAF2; background: #10284E; border: 1px solid #315377; }
QPushButton#SecondaryButton:hover, QToolButton#ThemeButton:hover { background: #173662; border-color: #4382DF; }
QToolButton#ThemeButton { border-radius: 10px; padding: 0; font-size: 18px; }
QToolButton#PhoneButton { color: #DCEAF2; background: #10284E; border: 1px solid #315377; border-radius: 10px; padding: 0 10px; font-weight: 500; }
QToolButton#PhoneButton:hover { background: #173662; border-color: #4382DF; }
QToolButton#PhoneButton:checked { color: #FFFFFF; background: #4647AE; border-color: #7778DE; }
QToolButton#PhoneButton:checked:disabled { color: #FFFFFF; background: #35368B; border-color: #5556B2; }
QToolButton#PhoneButton[connectionState="2"] { border-color: #E05252; }
QToolButton#HelpButton { color: #AACCD6; background: #10284E; border: 1px solid #315377; border-radius: 10px; padding: 0; font-size: 11px; font-weight: 700; }
QToolButton#HelpButton:hover { color: #FFFFFF; background: #4382DF; border-color: #4382DF; }
QPushButton:disabled, QToolButton:disabled { color: #6E86A4; background: #0E203B; border-color: #1E3651; }
QSlider::groove:horizontal { height: 7px; background: #27415F; border-radius: 4px; }
QSlider::sub-page:horizontal { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #4647AE, stop:1 #4382DF); border-radius: 4px; }
QSlider::handle:horizontal { image: url(:/controls/slider_handle_dark.svg); width: 20px; height: 20px; margin: -7px 0; background: transparent; border: none; }
QTextEdit#LogView { color: #C7DAE4; background: #091A38; border: 1px solid #1E3D64; border-radius: 9px; padding: 7px; font-size: 11px; }
QLabel#QrCodeSurface { background: #FFFFFF; border: 1px solid #315377; border-radius: 14px; }
QLabel#PairingCode { color: #DCEAF2; background: #102A50; border-radius: 9px; padding: 8px; }
QScrollArea#OutputScroll, QScrollArea#ContentScroll { background: transparent; border: none; }
QScrollArea#OutputScroll > QWidget > QWidget, QScrollArea#ContentScroll > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { width: 8px; background: transparent; margin: 2px; }
QScrollBar::handle:vertical { min-height: 28px; background: #315377; border-radius: 4px; }
QScrollBar::handle:vertical:hover { background: #4382DF; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QToolTip { color: #EAF4F8; background: #10284E; border: 1px solid #4382DF; padding: 5px; }
)QSS";

    setStyleSheet(QString::fromUtf8(darkTheme_ ? darkStyle : lightStyle));
    updatePhoneButtonAppearance();
}
