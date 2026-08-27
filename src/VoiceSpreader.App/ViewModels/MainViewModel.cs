using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Net.Sockets;
using System.Runtime.CompilerServices;
using Microsoft.UI.Dispatching;
using VoiceSpreader.App.Models;
using VoiceSpreader.App.Services;

namespace VoiceSpreader.App.ViewModels;

public sealed class MainViewModel : INotifyPropertyChanged, IDisposable
{
    private readonly AppHost _host;
    private readonly NativeAudioEngineBridge _engine;
    private readonly BluetoothAudioReceiverService _bluetooth;
    private readonly PhonePairingService _phone;
    private readonly DispatcherQueue _dispatcher;
    private readonly Dictionary<string, OutputSettings> _savedOutputs;
    private readonly Dictionary<string, PhoneDeviceSettings> _savedPhoneDevices;
    private readonly SemaphoreSlim _settingsSaveGate = new(1, 1);
    private AudioEndpoint? _selectedCapture;
    private AudioEndpoint? _selectedMicrophone;
    private int _bufferMilliseconds;
    private bool _automaticLatencyCompensation;
    private bool _continuousAcousticTracking;
    private bool _exclusiveMode;
    private AppThemeMode _themeMode;
    private bool _isBusy;
    private bool _isStarting;
    private bool _isRunning;
    private bool _isCalibrating;
    private bool _isStoppingCalibration;
    private bool _isBluetoothBusy;
    private bool _isBluetoothConnected;
    private bool _startWithWindows;
    private BluetoothAudioDevice? _selectedBluetoothDevice;
    private string _bluetoothStatus = "尚未扫描蓝牙音频设备。";
    private bool _isPhoneConnected;
    private bool _isPhoneMicrophoneStreaming;
    private string _phoneName = string.Empty;
    private string _phoneStatus = "等待手机输入配对码。";
    private double _phoneLevelPercent;
    private string? _selectedPhoneAddress;
    private AudioEndpoint? _selectedPhoneMicrophoneOutput;
    private int _phoneMicrophoneOutputVolume;
    private bool _isPhoneMicrophoneRouting;
    private bool _isPhoneMicrophoneRouteBusy;
    private AudioEndpoint? _selectedPhonePlaybackSource;
    private int _phonePlaybackVolume;
    private bool _isPhonePlaybackRouting;
    private bool _isPhonePlaybackStreaming;
    private bool _isPhonePlaybackRouteBusy;
    private string _statusBadge = "已停止";
    private string _statusTitle = "等待配置";
    private string _statusMessage = "选择一个系统声音来源和至少一个输出设备。";
    private string _programLevelText = "节目电平 -- dBFS · 探针暂停";
    private string? _noticeMessage;
    private long _settingsRevision;
    private bool _disposed;

    public MainViewModel(AppHost host, DispatcherQueue dispatcher)
    {
        _host = host;
        _engine = host.AudioEngine;
        _bluetooth = host.BluetoothAudioReceiver;
        _phone = host.PhonePairing;
        _dispatcher = dispatcher;
        _bufferMilliseconds = host.Settings.BufferMilliseconds;
        _automaticLatencyCompensation = host.Settings.AutomaticLatencyCompensation;
        _continuousAcousticTracking = host.Settings.ContinuousAcousticTracking;
        _exclusiveMode = host.Settings.ExclusiveMode;
        _themeMode = host.Settings.ThemeMode;
        _phoneMicrophoneOutputVolume = Math.Clamp(host.Settings.PhoneMicrophoneOutputVolume, 0, 100);
        _phonePlaybackVolume = Math.Clamp(host.Settings.PhonePlaybackVolume, 0, 100);
        _savedOutputs = new Dictionary<string, OutputSettings>(
            host.Settings.Outputs,
            StringComparer.OrdinalIgnoreCase);
        _savedPhoneDevices = new Dictionary<string, PhoneDeviceSettings>(
            host.Settings.PhoneDevices,
            StringComparer.OrdinalIgnoreCase);
        _startWithWindows = AutoStartService.IsEnabled;

        _engine.StatusChanged += Engine_StatusChanged;
        _engine.ErrorOccurred += Engine_ErrorOccurred;
        _engine.RunningChanged += Engine_RunningChanged;
        _engine.ProgramLevelChanged += Engine_ProgramLevelChanged;
        _engine.CalibrationCompleted += Engine_CalibrationCompleted;
        _engine.AcousticCorrectionChanged += Engine_AcousticCorrectionChanged;
        _bluetooth.StatusChanged += Bluetooth_StatusChanged;
        _bluetooth.ErrorOccurred += Bluetooth_ErrorOccurred;
        _bluetooth.BusyChanged += Bluetooth_BusyChanged;
        _bluetooth.ConnectionChanged += Bluetooth_ConnectionChanged;
        _phone.StatusChanged += Phone_StatusChanged;
        _phone.ConnectionChanged += Phone_ConnectionChanged;
        _phone.DevicesChanged += Phone_DevicesChanged;
        _phone.MicrophoneStreamingChanged += Phone_MicrophoneStreamingChanged;
        _phone.MicrophoneLevelChanged += Phone_MicrophoneLevelChanged;
        _phone.PlaybackStreamingChanged += Phone_PlaybackStreamingChanged;

        try
        {
            _phone.Start();
            ReplaceCollection(PhoneAddresses, _phone.LocalAddresses);
            var savedAddress = host.Settings.PhonePairingAddress;
            _selectedPhoneAddress = !string.IsNullOrWhiteSpace(savedAddress)
                                    && _phone.SetLocalAddress(savedAddress)
                ? savedAddress
                : _phone.LocalAddress;
        }
        catch (Exception exception) when (exception is SocketException or InvalidOperationException)
        {
            NoticeMessage = $"手机配对服务启动失败：{exception.Message}";
        }

        if (!_engine.IsAvailable)
        {
            NoticeMessage = _engine.AvailabilityError;
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public ObservableCollection<AudioEndpoint> CaptureSources { get; } = [];

    public ObservableCollection<AudioEndpoint> Microphones { get; } = [];

    public ObservableCollection<OutputEndpointItem> Outputs { get; } = [];

    public ObservableCollection<OutputEndpointItem> SelectedOutputs { get; } = [];

    public ObservableCollection<string> Activity { get; } = [];

    public ObservableCollection<BluetoothAudioDevice> BluetoothDevices { get; } = [];

    public ObservableCollection<string> PhoneAddresses { get; } = [];

    public ObservableCollection<AudioEndpoint> PhoneMicrophoneOutputs { get; } = [];

    public ObservableCollection<PhoneDeviceItem> PhoneDevices { get; } = [];

    public bool IsEngineAvailable => _engine.IsAvailable;

    public AudioEndpoint? SelectedCapture
    {
        get => _selectedCapture;
        set
        {
            if (SetField(ref _selectedCapture, value))
            {
                RebuildOutputs();
                _ = SaveSettingsAsync();
            }
        }
    }

    public AudioEndpoint? SelectedMicrophone
    {
        get => _selectedMicrophone;
        set
        {
            if (SetField(ref _selectedMicrophone, value))
            {
                _ = SaveSettingsAsync();
            }
        }
    }

    public int BufferMilliseconds
    {
        get => _bufferMilliseconds;
        set
        {
            if (SetField(ref _bufferMilliseconds, Math.Clamp(value, 2, 100)))
            {
                if (_engine.IsActive)
                {
                    _engine.SetSynchronizationMargin(_bufferMilliseconds);
                }
                _ = SaveSettingsAsync();
            }
        }
    }

    public bool AutomaticLatencyCompensation
    {
        get => _automaticLatencyCompensation;
        set
        {
            if (SetField(ref _automaticLatencyCompensation, value))
            {
                _ = SaveSettingsAsync();
            }
        }
    }

    public bool ContinuousAcousticTracking
    {
        get => _continuousAcousticTracking;
        set
        {
            if (SetField(ref _continuousAcousticTracking, value))
            {
                _ = SaveSettingsAsync();
            }
        }
    }

    public bool ExclusiveMode
    {
        get => _exclusiveMode;
        set
        {
            if (SetField(ref _exclusiveMode, value))
            {
                _ = SaveSettingsAsync();
            }
        }
    }

    public AppThemeMode ThemeMode
    {
        get => _themeMode;
        set
        {
            if (SetField(ref _themeMode, value))
            {
                _ = SaveSettingsAsync();
            }
        }
    }

    public bool IsBusy
    {
        get => _isBusy;
        private set => SetField(ref _isBusy, value);
    }

    public bool IsRunning
    {
        get => _isRunning;
        private set
        {
            if (SetField(ref _isRunning, value))
            {
                OnPropertyChanged(nameof(StartButtonText));
                OnPropertyChanged(nameof(ControlsEnabled));
                OnPropertyChanged(nameof(CanCalibrate));
                UpdateOutputControlStates();
            }
        }
    }

    public bool IsCalibrating
    {
        get => _isCalibrating;
        private set
        {
            if (SetField(ref _isCalibrating, value))
            {
                OnPropertyChanged(nameof(CalibrationButtonText));
                OnPropertyChanged(nameof(ControlsEnabled));
                OnPropertyChanged(nameof(AdjustmentsEnabled));
                OnPropertyChanged(nameof(CanControlSynchronization));
                OnPropertyChanged(nameof(CanCalibrate));
                UpdateOutputControlStates();
            }
        }
    }

    public bool IsStoppingCalibration
    {
        get => _isStoppingCalibration;
        private set
        {
            if (SetField(ref _isStoppingCalibration, value))
            {
                OnPropertyChanged(nameof(CalibrationButtonText));
                OnPropertyChanged(nameof(CanCalibrate));
                OnPropertyChanged(nameof(CanControlSynchronization));
            }
        }
    }

    public bool ControlsEnabled => !IsRunning && !_isStarting && !IsCalibrating;

    public bool AdjustmentsEnabled => !IsCalibrating;

    public string StartButtonText => IsRunning || _isStarting ? "停止同步" : "开始同步";

    public string CalibrationButtonText => IsStoppingCalibration
        ? "正在停止..."
        : IsCalibrating ? "停止校准" : "自动校准";

    public bool CanCalibrate => IsEngineAvailable
                                && !IsStoppingCalibration
                                && (IsCalibrating || (!IsRunning && !_isStarting));

    public bool CanControlSynchronization => IsEngineAvailable
                                             && !IsCalibrating
                                             && !IsStoppingCalibration;

    public string StatusBadge
    {
        get => _statusBadge;
        private set => SetField(ref _statusBadge, value);
    }

    public string StatusTitle
    {
        get => _statusTitle;
        private set => SetField(ref _statusTitle, value);
    }

    public string StatusMessage
    {
        get => _statusMessage;
        private set => SetField(ref _statusMessage, value);
    }

    public string ProgramLevelText
    {
        get => _programLevelText;
        private set => SetField(ref _programLevelText, value);
    }

    public string? NoticeMessage
    {
        get => _noticeMessage;
        private set
        {
            if (SetField(ref _noticeMessage, value))
            {
                OnPropertyChanged(nameof(HasNotice));
            }
        }
    }

    public bool HasNotice => !string.IsNullOrWhiteSpace(NoticeMessage);

    public BluetoothAudioDevice? SelectedBluetoothDevice
    {
        get => _selectedBluetoothDevice;
        set => SetField(ref _selectedBluetoothDevice, value);
    }

    public bool IsBluetoothBusy
    {
        get => _isBluetoothBusy;
        private set
        {
            if (SetField(ref _isBluetoothBusy, value))
            {
                OnPropertyChanged(nameof(IsBluetoothIdle));
            }
        }
    }

    public bool IsBluetoothIdle => !IsBluetoothBusy;

    public bool IsBluetoothConnected
    {
        get => _isBluetoothConnected;
        private set
        {
            if (SetField(ref _isBluetoothConnected, value))
            {
                OnPropertyChanged(nameof(BluetoothActionText));
            }
        }
    }

    public string BluetoothActionText => IsBluetoothConnected ? "断开连接" : "连接并接收声音";

    public string BluetoothStatus
    {
        get => _bluetoothStatus;
        private set => SetField(ref _bluetoothStatus, value);
    }

    public bool IsPhoneConnected
    {
        get => _isPhoneConnected;
        private set
        {
            if (SetField(ref _isPhoneConnected, value))
            {
                OnPropertyChanged(nameof(PhoneConnectionText));
                NotifyPhoneRouteStateChanged();
                NotifyPhonePlaybackStateChanged();
            }
        }
    }

    public bool IsPhoneMicrophoneStreaming
    {
        get => _isPhoneMicrophoneStreaming;
        private set
        {
            if (SetField(ref _isPhoneMicrophoneStreaming, value))
            {
                NotifyPhoneRouteStateChanged();
            }
        }
    }

    public int ConnectedPhoneDeviceCount => PhoneDevices.Count;

    public int SelectedPhoneMicrophoneCount =>
        PhoneDevices.Count(device => device.UseAsMicrophone);

    public int SelectedPhoneSpeakerCount =>
        PhoneDevices.Count(device => device.UseAsSpeaker && device.SupportsPlayback);

    public string ConnectedPhoneDeviceCountText => ConnectedPhoneDeviceCount switch
    {
        0 => "暂无设备",
        1 => "1 台设备在线",
        _ => $"{ConnectedPhoneDeviceCount} 台设备在线",
    };

    public string PhoneRouteSelectionText =>
        $"麦克风 {SelectedPhoneMicrophoneCount} · 扬声器 {SelectedPhoneSpeakerCount}";

    public string PhoneConnectionText => IsPhoneConnected
        ? ConnectedPhoneDeviceCount == 1 ? $"已连接：{_phoneName}" : $"已连接：{ConnectedPhoneDeviceCount} 台设备"
        : "等待设备连接";

    public string PhonePairingCode => _phone.PairingCode;

    public string PhonePairingPayload => _phone.PairingPayload;

    public string PhoneStatus
    {
        get => _phoneStatus;
        private set => SetField(ref _phoneStatus, value);
    }

    public double PhoneLevelPercent
    {
        get => _phoneLevelPercent;
        private set => SetField(ref _phoneLevelPercent, value);
    }

    public AudioEndpoint? SelectedPhoneMicrophoneOutput
    {
        get => _selectedPhoneMicrophoneOutput;
        set
        {
            if (SetField(ref _selectedPhoneMicrophoneOutput, value))
            {
                NotifyPhoneRouteStateChanged();
                _ = SaveSettingsAsync();
            }
        }
    }

    public int PhoneMicrophoneOutputVolume
    {
        get => _phoneMicrophoneOutputVolume;
        set
        {
            if (SetField(ref _phoneMicrophoneOutputVolume, Math.Clamp(value, 0, 100)))
            {
                if (IsPhoneMicrophoneRouting)
                {
                    _engine.SetRemoteMicrophoneOutputVolume(_phoneMicrophoneOutputVolume);
                }
                _ = SaveSettingsAsync();
            }
        }
    }

    public bool IsPhoneMicrophoneRouting
    {
        get => _isPhoneMicrophoneRouting;
        private set
        {
            if (SetField(ref _isPhoneMicrophoneRouting, value))
            {
                NotifyPhoneRouteStateChanged();
            }
        }
    }

    public bool IsPhoneMicrophoneRouteBusy
    {
        get => _isPhoneMicrophoneRouteBusy;
        private set
        {
            if (SetField(ref _isPhoneMicrophoneRouteBusy, value))
            {
                NotifyPhoneRouteStateChanged();
            }
        }
    }

    public bool HasVirtualCable => PhoneMicrophoneOutputs.Count > 0;

    public bool CanTogglePhoneMicrophoneRoute => !IsPhoneMicrophoneRouteBusy
                                                  && (IsPhoneMicrophoneRouting
                                                  || (IsEngineAvailable
                                                      && SelectedPhoneMicrophoneCount > 0
                                                      && SelectedPhoneMicrophoneOutput is not null));

    public string PhoneMicrophoneRouteActionText =>
        IsPhoneMicrophoneRouteBusy
            ? "正在切换…"
            : IsPhoneMicrophoneRouting ? "停止输出" : "输出到 Windows 应用";

    public string PhoneMicrophoneRouteBadge => IsPhoneMicrophoneRouting
        ? IsPhoneMicrophoneStreaming ? "传输中" : "等待音频"
        : HasVirtualCable ? "已就绪" : "需要安装";

    public string PhoneMicrophoneRouteStatus
    {
        get
        {
            if (!HasVirtualCable)
            {
                return "未检测到 VB-CABLE。安装后刷新设备即可使用。";
            }
            if (SelectedPhoneMicrophoneCount == 0)
            {
                return "请选择至少一台在线设备加入麦克风混音。";
            }
            if (IsPhoneMicrophoneRouting && IsPhoneMicrophoneStreaming)
            {
                return SelectedPhoneMicrophoneCount == 1
                    ? $"正在输出到 {SelectedPhoneMicrophoneOutput?.Name}；校准时在同步控制选择 CABLE Output。"
                    : $"正在混音 {SelectedPhoneMicrophoneCount} 台设备；校准前请只保留一个麦克风。";
            }
            if (IsPhoneMicrophoneRouting)
            {
                return "输出端点已经打开，正在等待手机开始回传麦克风。";
            }
            return "设备已就绪；校准麦克风统一在同步控制中选择 CABLE Output。";
        }
    }

    public AudioEndpoint? SelectedPhonePlaybackSource
    {
        get => _selectedPhonePlaybackSource;
        set
        {
            if (SetField(ref _selectedPhonePlaybackSource, value))
            {
                NotifyPhonePlaybackStateChanged();
                _ = SaveSettingsAsync();
            }
        }
    }

    public int PhonePlaybackVolume
    {
        get => _phonePlaybackVolume;
        set
        {
            if (SetField(ref _phonePlaybackVolume, Math.Clamp(value, 0, 100)))
            {
                if (IsPhonePlaybackRouting)
                {
                    _engine.SetSystemAudioCaptureVolume(_phonePlaybackVolume);
                }
                _ = SaveSettingsAsync();
            }
        }
    }

    public bool IsPhonePlaybackRouting
    {
        get => _isPhonePlaybackRouting;
        private set
        {
            if (SetField(ref _isPhonePlaybackRouting, value))
            {
                NotifyPhonePlaybackStateChanged();
            }
        }
    }

    public bool IsPhonePlaybackStreaming
    {
        get => _isPhonePlaybackStreaming;
        private set
        {
            if (SetField(ref _isPhonePlaybackStreaming, value))
            {
                NotifyPhonePlaybackStateChanged();
            }
        }
    }

    public bool IsPhonePlaybackRouteBusy
    {
        get => _isPhonePlaybackRouteBusy;
        private set
        {
            if (SetField(ref _isPhonePlaybackRouteBusy, value))
            {
                NotifyPhonePlaybackStateChanged();
            }
        }
    }

    public bool CanTogglePhonePlayback => !IsPhonePlaybackRouteBusy
                                           && (IsPhonePlaybackRouting
                                           || (IsEngineAvailable
                                               && SelectedPhoneSpeakerCount > 0
                                               && SelectedPhonePlaybackSource is not null));

    public bool PhonePlaybackSourceEnabled => !IsPhonePlaybackRouting && !IsPhonePlaybackRouteBusy;

    public string PhonePlaybackActionText =>
        IsPhonePlaybackRouteBusy
            ? "正在切换…"
            : IsPhonePlaybackRouting ? "停止手机播放" : "在手机上播放";

    public string PhonePlaybackBadge => IsPhonePlaybackRouting
        ? IsPhonePlaybackStreaming ? "播放中" : "等待声音"
        : !IsPhoneConnected ? "等待连接"
        : SelectedPhoneSpeakerCount == 0 ? "未选择设备" : "已就绪";

    public string PhonePlaybackStatus
    {
        get
        {
            if (!IsPhoneConnected)
            {
                return "连接 Android 手机后即可建立独立播放链路。";
            }
            if (SelectedPhoneSpeakerCount == 0)
            {
                return "请选择至少一台支持播放的在线设备。";
            }
            if (IsPhonePlaybackRouting && IsPhonePlaybackStreaming)
            {
                return $"正在从 {SelectedPhonePlaybackSource?.Name} 向 {SelectedPhoneSpeakerCount} 台设备分发 48 kHz 音频。";
            }
            if (IsPhonePlaybackRouting)
            {
                return "链路已经建立，正在等待 Windows 产生可播放声音。";
            }
            return "选择 Windows 播放端点，并勾选要作为电脑扬声器的设备。";
        }
    }

    public string? SelectedPhoneAddress
    {
        get => _selectedPhoneAddress;
        set
        {
            if (string.IsNullOrWhiteSpace(value) || !SetField(ref _selectedPhoneAddress, value))
            {
                return;
            }
            if (_phone.SetLocalAddress(value))
            {
                OnPropertyChanged(nameof(PhonePairingPayload));
                _ = SaveSettingsAsync();
            }
        }
    }

    public async Task<string?> TogglePhoneMicrophoneRouteAsync()
    {
        if (IsPhoneMicrophoneRouteBusy)
        {
            return null;
        }

        IsPhoneMicrophoneRouteBusy = true;
        try
        {
            if (IsPhoneMicrophoneRouting || _engine.IsRemoteMicrophoneOutputActive)
            {
                await Task.Run(_engine.StopRemoteMicrophoneOutput);
                var devicesToStop = PhoneDevices
                    .Where(device => device.UseAsMicrophone || device.MicrophoneStreaming)
                    .Select(device => device.Id)
                    .ToArray();
                await Task.WhenAll(devicesToStop.Select(
                    deviceId => _phone.SetMicrophoneEnabledAsync(deviceId, false)));
                IsPhoneMicrophoneRouting = false;
                AddActivity("已停止移动设备麦克风混音输出");
                return null;
            }

            var selectedDevices = PhoneDevices
                .Where(device => device.UseAsMicrophone)
                .ToArray();
            if (selectedDevices.Length == 0)
            {
                return "请选择至少一台设备作为电脑麦克风。";
            }
            var output = SelectedPhoneMicrophoneOutput;
            if (output is null)
            {
                return "未检测到 VB-CABLE 的 CABLE Input，请安装后刷新音频设备。";
            }
            var started = await Task.Run(() => _engine.StartRemoteMicrophoneOutput(
                output,
                bufferMilliseconds: 20,
                volumePercent: PhoneMicrophoneOutputVolume));
            if (!started)
            {
                return "无法打开 VB-CABLE 输出端点，请确认它未被其他应用独占。";
            }

            IsPhoneMicrophoneRouting = true;
            var microphoneResults = await Task.WhenAll(selectedDevices.Select(
                device => _phone.SetMicrophoneEnabledAsync(device.Id, true)));
            if (!microphoneResults.Any(result => result))
            {
                await Task.Run(_engine.StopRemoteMicrophoneOutput);
                IsPhoneMicrophoneRouting = false;
                return "输出端点已经打开，但未能让选中的设备开始回传麦克风。";
            }
            AddActivity($"{microphoneResults.Count(result => result)} 台设备的麦克风将混音到 {output.Name}");
            return null;
        }
        catch (Exception exception)
        {
            if (_engine.IsRemoteMicrophoneOutputActive)
            {
                await Task.Run(_engine.StopRemoteMicrophoneOutput);
            }
            IsPhoneMicrophoneRouting = false;
            return $"切换手机麦克风路由失败：{exception.Message}";
        }
        finally
        {
            IsPhoneMicrophoneRouteBusy = false;
        }
    }

    public async Task<string?> TogglePhonePlaybackAsync()
    {
        if (IsPhonePlaybackRouteBusy)
        {
            return null;
        }

        IsPhonePlaybackRouteBusy = true;
        try
        {
            if (IsPhonePlaybackRouting || _engine.IsSystemAudioCaptureActive)
            {
                await Task.Run(_engine.StopSystemAudioCapture);
                var devicesToStop = PhoneDevices
                    .Where(device => device.UseAsSpeaker || device.PlaybackRequested)
                    .Select(device => device.Id)
                    .ToArray();
                await Task.WhenAll(devicesToStop.Select(
                    deviceId => _phone.SetPlaybackEnabledAsync(deviceId, false)));
                IsPhonePlaybackRouting = false;
                IsPhonePlaybackStreaming = false;
                AddActivity("已停止 Windows 声音到移动设备的播放链路");
                return null;
            }

            var selectedDevices = PhoneDevices
                .Where(device => device.UseAsSpeaker && device.SupportsPlayback)
                .ToArray();
            if (selectedDevices.Length == 0)
            {
                return "请选择至少一台设备作为电脑扬声器。";
            }
            var source = SelectedPhonePlaybackSource;
            if (source is null)
            {
                return "请选择要发送到手机的 Windows 播放端点。";
            }
            var playbackResults = await Task.WhenAll(selectedDevices.Select(
                device => _phone.SetPlaybackEnabledAsync(device.Id, true)));
            var startedDeviceIds = selectedDevices
                .Where((_, index) => playbackResults[index])
                .Select(device => device.Id)
                .ToArray();
            if (startedDeviceIds.Length == 0)
            {
                return "未能让选中的设备准备音频播放。";
            }

            var started = await Task.Run(() => _engine.StartSystemAudioCapture(
                source,
                PhonePlaybackVolume));
            if (!started)
            {
                await Task.WhenAll(startedDeviceIds.Select(
                    deviceId => _phone.SetPlaybackEnabledAsync(deviceId, false)));
                return "无法打开 Windows 系统声音捕获端点。";
            }

            IsPhonePlaybackRouting = true;
            AddActivity($"Windows 声音将从 {source.Name} 发送到 {startedDeviceIds.Length} 台设备");
            return null;
        }
        catch (Exception exception)
        {
            if (_engine.IsSystemAudioCaptureActive)
            {
                await Task.Run(_engine.StopSystemAudioCapture);
            }
            IsPhonePlaybackRouting = false;
            IsPhonePlaybackStreaming = false;
            return $"切换手机播放路由失败：{exception.Message}";
        }
        finally
        {
            IsPhonePlaybackRouteBusy = false;
        }
    }

    public void DisconnectPhone() => _phone.DisconnectPhone();

    public void DisconnectPhoneDevice(string deviceId) => _phone.DisconnectDevice(deviceId);

    public void ResetPhonePairing()
    {
        _phone.ResetPairing();
        OnPropertyChanged(nameof(PhonePairingCode));
        OnPropertyChanged(nameof(PhonePairingPayload));
    }

    public bool StartWithWindows
    {
        get => _startWithWindows;
        set
        {
            if (_startWithWindows == value)
            {
                return;
            }

            try
            {
                AutoStartService.SetEnabled(value);
                SetField(ref _startWithWindows, value);
                AddActivity(value ? "已启用开机自启动" : "已关闭开机自启动");
            }
            catch (Exception exception)
            {
                NoticeMessage = $"更新开机自启动失败：{exception.Message}";
                OnPropertyChanged();
            }
        }
    }

    public async Task RefreshBluetoothDevicesAsync()
    {
        var devices = await _bluetooth.GetDevicesAsync();
        Dispatch(() =>
        {
            ReplaceCollection(BluetoothDevices, devices);
            SelectedBluetoothDevice = devices.Count > 0 ? devices[0] : null;
        });
    }

    public async Task<string?> ConnectOrDisconnectBluetoothAsync()
    {
        if (IsBluetoothConnected)
        {
            _bluetooth.Disconnect();
            return null;
        }
        if (SelectedBluetoothDevice is null)
        {
            return "请选择一个已配对的蓝牙音频设备。";
        }

        return await _bluetooth.ConnectAsync(SelectedBluetoothDevice)
            ? null
            : "未能建立蓝牙音频连接，请查看状态信息。";
    }

    public async Task RefreshDevicesAsync()
    {
        if (!IsEngineAvailable || IsRunning || IsCalibrating)
        {
            return;
        }

        IsBusy = true;
        NoticeMessage = null;
        try
        {
            var renderTask = Task.Run(NativeAudioEngineBridge.GetRenderDevices);
            var captureTask = Task.Run(NativeAudioEngineBridge.GetCaptureDevices);
            await Task.WhenAll(renderTask, captureTask);
            var render = await renderTask;
            var capture = await captureTask;

            ReplaceCollection(CaptureSources, render.Devices);
            ReplaceCollection(Microphones, capture.Devices);
            var virtualCableOutputs = render.Devices
                .Where(IsVirtualCableInput)
                .OrderBy(device => device.Name, StringComparer.CurrentCultureIgnoreCase)
                .ToArray();
            ReplaceCollection(PhoneMicrophoneOutputs, virtualCableOutputs);
            SelectedPhoneMicrophoneOutput = FindById(
                                               PhoneMicrophoneOutputs,
                                               _host.Settings.PhoneMicrophoneOutputDeviceId)
                                           ?? PhoneMicrophoneOutputs.FirstOrDefault();
            OnPropertyChanged(nameof(HasVirtualCable));
            NotifyPhoneRouteStateChanged();

            SelectedCapture = FindById(CaptureSources, _host.Settings.CaptureDeviceId)
                              ?? CaptureSources.FirstOrDefault(device => device.IsDefault)
                              ?? CaptureSources.FirstOrDefault();
            SelectedPhonePlaybackSource = FindById(
                                              CaptureSources,
                                              _host.Settings.PhonePlaybackSourceDeviceId)
                                          ?? CaptureSources.FirstOrDefault(device => device.IsDefault)
                                          ?? CaptureSources.FirstOrDefault();
            SelectedMicrophone = FindById(Microphones, _host.Settings.MicrophoneDeviceId)
                                 ?? Microphones.FirstOrDefault(device => device.IsDefault)
                                 ?? Microphones.FirstOrDefault();

            var errors = new[] { render.Error, capture.Error }
                .Where(message => !string.IsNullOrWhiteSpace(message));
            NoticeMessage = string.Join(Environment.NewLine, errors);
            AddActivity($"已刷新设备：{render.Devices.Count} 个播放端点，{capture.Devices.Count} 个麦克风");
        }
        catch (Exception exception)
        {
            NoticeMessage = $"刷新设备失败：{exception.Message}";
        }
        finally
        {
            IsBusy = false;
        }
    }

    public string? StartOrStop()
    {
        if (IsRunning || _isStarting || _engine.IsActive)
        {
            StatusBadge = "正在停止";
            StatusTitle = "正在停止同步";
            _engine.Stop();
            return null;
        }

        if (IsCalibrating || IsStoppingCalibration || _engine.IsCalibrating)
        {
            return "请先停止声学校准，再开始音频同步。";
        }

        if (!IsEngineAvailable)
        {
            return NoticeMessage ?? "原生音频引擎不可用。";
        }
        if (SelectedCapture is null)
        {
            return "请选择系统声音来源。";
        }

        var selected = Outputs.Where(output => output.IsSelected).ToArray();
        if (selected.Length == 0)
        {
            return "请至少选择一个输出设备。";
        }
        if (ContinuousAcousticTracking && selected.Length >= 2
            && SelectedMicrophone is null)
        {
            return "启用自同步时，请选择用于连续测量的麦克风。";
        }

        foreach (var output in selected)
        {
            output.AutomaticDelayMilliseconds = 0;
        }

        var configuration = new EngineConfiguration(
            SelectedCapture,
            selected.Select(output => new OutputEngineSettings(
                    output.Endpoint,
                    output.VolumePercent,
                    output.DelayMilliseconds))
                .ToArray(),
            BufferMilliseconds,
            ExclusiveMode,
            AutomaticLatencyCompensation,
            SelectedMicrophone,
            ContinuousAcousticTracking && selected.Length >= 2,
            UseRemoteMicrophone: false);

        _isStarting = true;
        OnPropertyChanged(nameof(StartButtonText));
        OnPropertyChanged(nameof(ControlsEnabled));
        OnPropertyChanged(nameof(CanCalibrate));
        UpdateOutputControlStates();
        StatusBadge = "正在初始化";
        StatusTitle = "正在建立同步时钟";
        StatusMessage = $"正在连接 {selected.Length} 个输出端点，软件同步余量 {BufferMilliseconds} ms。";
        AddActivity(StatusMessage);

        if (!_engine.Start(configuration))
        {
            _isStarting = false;
            OnPropertyChanged(nameof(StartButtonText));
            OnPropertyChanged(nameof(ControlsEnabled));
            OnPropertyChanged(nameof(CanCalibrate));
            UpdateOutputControlStates();
            return "音频引擎已经在启动或运行。";
        }
        return null;
    }

    public string? StartOrStopCalibration()
    {
        if (IsStoppingCalibration)
        {
            return null;
        }
        if (IsCalibrating || _engine.IsCalibrating)
        {
            IsStoppingCalibration = true;
            StatusBadge = "正在停止";
            StatusTitle = "正在停止校准";
            StatusMessage = "正在释放校准使用的音频设备，请稍候。";
            _engine.StopCalibration();
            return null;
        }
        if (IsRunning || _isStarting)
        {
            return "请先停止音频同步，再开始声学校准。";
        }
        if (SelectedMicrophone is null)
        {
            return "请选择校准麦克风。";
        }

        var outputs = Outputs.Where(output => output.IsSelected).Select(output => output.Endpoint).ToArray();
        if (outputs.Length < 2)
        {
            return "自动校准至少需要两个输出设备。";
        }

        IsCalibrating = _engine.StartCalibration(new CalibrationConfiguration(SelectedMicrophone, outputs));
        if (!IsCalibrating)
        {
            return "校准器已经在运行。";
        }

        StatusBadge = "校准中";
        StatusTitle = "正在测量声学路径";
        StatusMessage = "请保持麦克风位置不变，并避免在测量期间产生额外声音。";
        AddActivity("声学校准已开始");
        return null;
    }

    public void NotifyOutputSettingsChanged(OutputEndpointItem output)
    {
        RebuildSelectedOutputs();
        if (_engine.IsActive)
        {
            _engine.SetOutputVolume(output.Id, output.VolumePercent);
            _engine.SetOutputDelay(output.Id, output.DelayMilliseconds);
        }
        _ = SaveSettingsAsync();
    }

    private void RebuildOutputs()
    {
        foreach (var output in Outputs)
        {
            output.SettingsChanged -= Output_SettingsChanged;
            _savedOutputs[output.Id] = new OutputSettings(
                output.IsSelected,
                output.VolumePercent,
                output.DelayMilliseconds);
        }
        Outputs.Clear();

        foreach (var endpoint in CaptureSources.Where(device =>
                     !string.Equals(device.Id, SelectedCapture?.Id, StringComparison.OrdinalIgnoreCase)))
        {
            var item = new OutputEndpointItem(endpoint);
            item.SelectionEnabled = ControlsEnabled;
            item.AdjustmentsEnabled = AdjustmentsEnabled;
            if (_savedOutputs.TryGetValue(endpoint.Id, out var settings))
            {
                item.IsSelected = settings.Selected;
                item.VolumePercent = settings.VolumePercent;
                item.DelayMilliseconds = settings.DelayMilliseconds;
            }
            item.SettingsChanged += Output_SettingsChanged;
            Outputs.Add(item);
        }
        RebuildSelectedOutputs();
    }

    private void Output_SettingsChanged(object? sender, EventArgs args)
    {
        if (sender is OutputEndpointItem output)
        {
            NotifyOutputSettingsChanged(output);
        }
    }

    private void RebuildSelectedOutputs()
    {
        SelectedOutputs.Clear();
        foreach (var output in Outputs.Where(output => output.IsSelected))
        {
            SelectedOutputs.Add(output);
        }
        OnPropertyChanged(nameof(SelectedOutputs));
    }

    private void UpdateOutputControlStates()
    {
        foreach (var output in Outputs)
        {
            output.SelectionEnabled = ControlsEnabled;
            output.AdjustmentsEnabled = AdjustmentsEnabled;
        }
    }

    private async Task SaveSettingsAsync()
    {
        if (_disposed)
        {
            return;
        }

        foreach (var output in Outputs)
        {
            _savedOutputs[output.Id] = new OutputSettings(
                output.IsSelected,
                output.VolumePercent,
                output.DelayMilliseconds);
        }

        var settings = new AppSettings
        {
            CaptureDeviceId = SelectedCapture?.Id,
            MicrophoneDeviceId = SelectedMicrophone?.Id,
            BufferMilliseconds = BufferMilliseconds,
            AutomaticLatencyCompensation = AutomaticLatencyCompensation,
            ContinuousAcousticTracking = ContinuousAcousticTracking,
            ExclusiveMode = ExclusiveMode,
            ThemeMode = ThemeMode,
            PhonePairingAddress = SelectedPhoneAddress,
            PhoneMicrophoneOutputDeviceId = SelectedPhoneMicrophoneOutput?.Id,
            PhoneMicrophoneOutputVolume = PhoneMicrophoneOutputVolume,
            PhonePlaybackSourceDeviceId = SelectedPhonePlaybackSource?.Id,
            PhonePlaybackVolume = PhonePlaybackVolume,
            PhoneDevices = new Dictionary<string, PhoneDeviceSettings>(
                _savedPhoneDevices,
                StringComparer.OrdinalIgnoreCase),
            Outputs = new Dictionary<string, OutputSettings>(_savedOutputs, StringComparer.OrdinalIgnoreCase),
        };
        var revision = Interlocked.Increment(ref _settingsRevision);
        await _settingsSaveGate.WaitAsync();
        try
        {
            if (_disposed || revision != Volatile.Read(ref _settingsRevision))
            {
                return;
            }
            await _host.SaveSettingsAsync(settings);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            Dispatch(() => NoticeMessage = $"保存设置失败：{exception.Message}");
        }
        finally
        {
            _settingsSaveGate.Release();
        }
    }

    private void Engine_StatusChanged(object? sender, string message) => Dispatch(() =>
    {
        StatusMessage = message;
        AddActivity(message);
    });

    private void Engine_ErrorOccurred(object? sender, string message) => Dispatch(() =>
    {
        NoticeMessage = message;
        StatusBadge = "需要处理";
        StatusTitle = "同步遇到问题";
        StatusMessage = message;
        AddActivity(message);
    });

    private void Engine_RunningChanged(object? sender, bool running) => Dispatch(() =>
    {
        _isStarting = false;
        IsRunning = running;
        OnPropertyChanged(nameof(StartButtonText));
        OnPropertyChanged(nameof(ControlsEnabled));
        UpdateOutputControlStates();
        StatusBadge = running ? "同步中" : "已停止";
        StatusTitle = running ? "所有输出共享同一时间线" : "同步已停止";
        if (!running)
        {
            ProgramLevelText = "节目电平 -- dBFS · 探针暂停";
        }
    });

    private void Engine_ProgramLevelChanged(object? sender, ProgramLevelEventArgs args) => Dispatch(() =>
    {
        var level = Math.Clamp(args.LevelDbfs, -160.0, 0.0);
        ProgramLevelText = $"节目电平 {level:0.0} dBFS · 探针{(args.ProbeAllowed ? "可用" : "暂停")}";
    });

    private void Engine_AcousticCorrectionChanged(object? sender, AcousticCorrectionEventArgs args) =>
        Dispatch(() =>
        {
            var output = Outputs.FirstOrDefault(item =>
                string.Equals(item.Id, args.DeviceId, StringComparison.OrdinalIgnoreCase));
            if (output is null)
            {
                return;
            }

            output.AutomaticDelayMilliseconds = args.DelayMilliseconds;
            output.CorrectionText =
                $"{args.ProbeMode} · 自动 {args.DelayMilliseconds:+0;-0;0} ms · "
                + $"漂移 {args.DriftPpm:+0.0;-0.0;0.0} ppm · 置信度 {args.Confidence:P0}";
        });

    private void Engine_CalibrationCompleted(object? sender, CalibrationCompletedEventArgs args) => Dispatch(() =>
    {
        IsCalibrating = false;
        IsStoppingCalibration = false;
        if (args.Outcome == CalibrationOutcome.Cancelled)
        {
            StatusBadge = "已取消";
            StatusTitle = "校准已停止";
            StatusMessage = "音频设备已释放，可以重新校准或开始同步。";
            AddActivity("声学校准已取消");
            return;
        }
        if (args.Outcome == CalibrationOutcome.Failed)
        {
            return;
        }

        var applied = 0;
        foreach (var result in args.Results)
        {
            var output = Outputs.FirstOrDefault(item =>
                string.Equals(item.Id, result.Device.Id, StringComparison.OrdinalIgnoreCase));
            if (output is null)
            {
                continue;
            }

            output.CorrectionText = result.Detected
                ? $"{result.MeasuredLatencyMilliseconds:0.0} ms · 置信度 {result.Confidence:P0}"
                : "未检测到有效到达峰";
            if (result.Detected)
            {
                output.AutomaticDelayMilliseconds = 0;
                output.DelayMilliseconds = result.RecommendedDelayMilliseconds;
                applied++;
            }
        }

        if (applied > 0)
        {
            AutomaticLatencyCompensation = false;
        }
        StatusBadge = "校准完成";
        StatusTitle = $"已对齐 {applied} 个输出";
        StatusMessage = applied > 0
            ? "声学路径补偿已写入设备，并关闭端点自动补偿以避免重复计算。"
            : "未获得可应用的测量结果，请检查麦克风位置和播放音量。";
        AddActivity(StatusMessage);
    });

    private void Bluetooth_StatusChanged(object? sender, string message) => Dispatch(() =>
    {
        BluetoothStatus = message;
        AddActivity(message);
    });

    private void Bluetooth_ErrorOccurred(object? sender, string message) => Dispatch(() =>
    {
        BluetoothStatus = message;
        NoticeMessage = message;
        AddActivity(message);
    });

    private void Bluetooth_BusyChanged(object? sender, bool busy) =>
        Dispatch(() => IsBluetoothBusy = busy);

    private void Bluetooth_ConnectionChanged(object? sender, bool connected) => Dispatch(() =>
    {
        IsBluetoothConnected = connected;
        OnPropertyChanged(nameof(BluetoothActionText));
    });

    private void Phone_StatusChanged(object? sender, string message) => Dispatch(() =>
    {
        PhoneStatus = message;
        AddActivity(message);
    });

    private void Phone_ConnectionChanged(object? sender, PhoneConnectionChangedEventArgs args) => Dispatch(() =>
    {
        AddActivity(args.Connected
            ? $"移动设备已连接：{args.PhoneName}"
            : $"移动设备已断开：{args.PhoneName}");
    });

    private void Phone_DevicesChanged(object? sender, PhoneDevicesChangedEventArgs args) =>
        Dispatch(() => SyncPhoneDevices(args.Devices));

    private void SyncPhoneDevices(IReadOnlyList<PhoneDeviceSnapshot> snapshots)
    {
        var snapshotIds = snapshots
            .Select(snapshot => snapshot.Id)
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var removed in PhoneDevices.Where(device => !snapshotIds.Contains(device.Id)).ToArray())
        {
            _savedPhoneDevices[removed.Id] = removed.ToSettings();
            removed.SettingsChanged -= PhoneDevice_SettingsChanged;
            PhoneDevices.Remove(removed);
        }

        foreach (var snapshot in snapshots)
        {
            var item = PhoneDevices.FirstOrDefault(device =>
                string.Equals(device.Id, snapshot.Id, StringComparison.OrdinalIgnoreCase));
            if (item is null)
            {
                var hasSavedSettings = _savedPhoneDevices.TryGetValue(snapshot.Id, out var settings);
                settings ??= new PhoneDeviceSettings(
                    UseAsMicrophone: true,
                    UseAsSpeaker: snapshot.SupportsPlayback,
                    MicrophoneGainPercent: 100);
                item = new PhoneDeviceItem(snapshot, settings);
                item.SettingsChanged += PhoneDevice_SettingsChanged;
                PhoneDevices.Add(item);
                _savedPhoneDevices[item.Id] = item.ToSettings();
                _phone.SetMicrophoneGain(item.Id, item.MicrophoneGainPercent);

                if (!hasSavedSettings)
                {
                    _ = SaveSettingsAsync();
                }
                if (IsPhoneMicrophoneRouting && item.UseAsMicrophone)
                {
                    _ = _phone.SetMicrophoneEnabledAsync(item.Id, true);
                }
                if (IsPhonePlaybackRouting && item.UseAsSpeaker && item.SupportsPlayback)
                {
                    _ = _phone.SetPlaybackEnabledAsync(item.Id, true);
                }
            }
            else
            {
                item.Update(snapshot);
            }
        }

        _phoneName = _phone.ConnectedPhoneName;
        IsPhoneConnected = PhoneDevices.Count > 0;
        IsPhoneMicrophoneStreaming = PhoneDevices.Any(device => device.MicrophoneStreaming);
        IsPhonePlaybackStreaming = PhoneDevices.Any(device => device.PlaybackStreaming);

        if (PhoneDevices.Count == 0 || SelectedPhoneMicrophoneCount == 0)
        {
            if (IsPhoneMicrophoneRouting || _engine.IsRemoteMicrophoneOutputActive)
            {
                _ = Task.Run(_engine.StopRemoteMicrophoneOutput);
                IsPhoneMicrophoneRouting = false;
            }
        }
        if (PhoneDevices.Count == 0 || SelectedPhoneSpeakerCount == 0)
        {
            if (IsPhonePlaybackRouting || _engine.IsSystemAudioCaptureActive)
            {
                _ = Task.Run(_engine.StopSystemAudioCapture);
                IsPhonePlaybackRouting = false;
                IsPhonePlaybackStreaming = false;
            }
        }
        NotifyPhoneDeviceSelectionChanged();
    }

    private async void PhoneDevice_SettingsChanged(object? sender, string propertyName)
    {
        if (sender is not PhoneDeviceItem device)
        {
            return;
        }

        _savedPhoneDevices[device.Id] = device.ToSettings();
        if (propertyName == nameof(PhoneDeviceItem.MicrophoneGainPercent))
        {
            _phone.SetMicrophoneGain(device.Id, device.MicrophoneGainPercent);
        }
        else if (propertyName == nameof(PhoneDeviceItem.UseAsMicrophone)
                 && IsPhoneMicrophoneRouting)
        {
            await _phone.SetMicrophoneEnabledAsync(device.Id, device.UseAsMicrophone);
            if (!PhoneDevices.Any(item => item.UseAsMicrophone))
            {
                await Task.Run(_engine.StopRemoteMicrophoneOutput);
                IsPhoneMicrophoneRouting = false;
            }
        }
        else if (propertyName == nameof(PhoneDeviceItem.UseAsSpeaker)
                 && IsPhonePlaybackRouting
                 && device.SupportsPlayback)
        {
            await _phone.SetPlaybackEnabledAsync(device.Id, device.UseAsSpeaker);
            if (!PhoneDevices.Any(item => item.UseAsSpeaker && item.SupportsPlayback))
            {
                await Task.Run(_engine.StopSystemAudioCapture);
                IsPhonePlaybackRouting = false;
                IsPhonePlaybackStreaming = false;
            }
        }

        NotifyPhoneDeviceSelectionChanged();
        await SaveSettingsAsync();
    }

    private void Phone_MicrophoneStreamingChanged(object? sender, bool enabled) => Dispatch(() =>
    {
        IsPhoneMicrophoneStreaming = enabled;
    });

    private void Phone_MicrophoneLevelChanged(object? sender, double levelDbfs) => Dispatch(() =>
    {
        PhoneLevelPercent = Math.Clamp((levelDbfs + 60) / 60 * 100, 0, 100);
    });

    private void Phone_PlaybackStreamingChanged(object? sender, bool enabled) => Dispatch(() =>
    {
        IsPhonePlaybackStreaming = enabled;
    });

    private void AddActivity(string message)
    {
        Activity.Insert(0, $"{DateTimeOffset.Now:HH:mm:ss}  {message}");
        while (Activity.Count > 80)
        {
            Activity.RemoveAt(Activity.Count - 1);
        }
    }

    private void Dispatch(Action action)
    {
        if (_dispatcher.HasThreadAccess)
        {
            action();
        }
        else
        {
            _dispatcher.TryEnqueue(() => action());
        }
    }

    private static AudioEndpoint? FindById(IEnumerable<AudioEndpoint> devices, string? id) =>
        string.IsNullOrWhiteSpace(id)
            ? null
            : devices.FirstOrDefault(device => string.Equals(device.Id, id, StringComparison.OrdinalIgnoreCase));

    private static bool IsVirtualCableInput(AudioEndpoint device)
    {
        var name = device.Name;
        return name.Contains("CABLE Input", StringComparison.OrdinalIgnoreCase)
               || name.Contains("CABLE-A Input", StringComparison.OrdinalIgnoreCase)
               || name.Contains("CABLE-B Input", StringComparison.OrdinalIgnoreCase)
               || name.Contains("CABLE-C Input", StringComparison.OrdinalIgnoreCase)
               || name.Contains("CABLE-D Input", StringComparison.OrdinalIgnoreCase)
               || name.Contains("VB-Audio Virtual Cable", StringComparison.OrdinalIgnoreCase);
    }

    private void NotifyPhoneRouteStateChanged()
    {
        OnPropertyChanged(nameof(CanTogglePhoneMicrophoneRoute));
        OnPropertyChanged(nameof(PhoneMicrophoneRouteActionText));
        OnPropertyChanged(nameof(PhoneMicrophoneRouteBadge));
        OnPropertyChanged(nameof(PhoneMicrophoneRouteStatus));
    }

    private void NotifyPhonePlaybackStateChanged()
    {
        OnPropertyChanged(nameof(CanTogglePhonePlayback));
        OnPropertyChanged(nameof(PhonePlaybackActionText));
        OnPropertyChanged(nameof(PhonePlaybackBadge));
        OnPropertyChanged(nameof(PhonePlaybackStatus));
        OnPropertyChanged(nameof(PhonePlaybackSourceEnabled));
    }

    private void NotifyPhoneDeviceSelectionChanged()
    {
        OnPropertyChanged(nameof(ConnectedPhoneDeviceCount));
        OnPropertyChanged(nameof(ConnectedPhoneDeviceCountText));
        OnPropertyChanged(nameof(SelectedPhoneMicrophoneCount));
        OnPropertyChanged(nameof(SelectedPhoneSpeakerCount));
        OnPropertyChanged(nameof(PhoneRouteSelectionText));
        OnPropertyChanged(nameof(PhoneConnectionText));
        NotifyPhoneRouteStateChanged();
        NotifyPhonePlaybackStateChanged();
    }

    private static void ReplaceCollection<T>(ObservableCollection<T> collection, IEnumerable<T> values)
    {
        collection.Clear();
        foreach (var value in values)
        {
            collection.Add(value);
        }
    }

    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return false;
        }

        field = value;
        OnPropertyChanged(propertyName);
        return true;
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _engine.StopRemoteMicrophoneOutput();
        _engine.StopSystemAudioCapture();
        _engine.StatusChanged -= Engine_StatusChanged;
        _engine.ErrorOccurred -= Engine_ErrorOccurred;
        _engine.RunningChanged -= Engine_RunningChanged;
        _engine.ProgramLevelChanged -= Engine_ProgramLevelChanged;
        _engine.CalibrationCompleted -= Engine_CalibrationCompleted;
        _engine.AcousticCorrectionChanged -= Engine_AcousticCorrectionChanged;
        _bluetooth.StatusChanged -= Bluetooth_StatusChanged;
        _bluetooth.ErrorOccurred -= Bluetooth_ErrorOccurred;
        _bluetooth.BusyChanged -= Bluetooth_BusyChanged;
        _bluetooth.ConnectionChanged -= Bluetooth_ConnectionChanged;
        _phone.StatusChanged -= Phone_StatusChanged;
        _phone.ConnectionChanged -= Phone_ConnectionChanged;
        _phone.DevicesChanged -= Phone_DevicesChanged;
        _phone.MicrophoneStreamingChanged -= Phone_MicrophoneStreamingChanged;
        _phone.MicrophoneLevelChanged -= Phone_MicrophoneLevelChanged;
        _phone.PlaybackStreamingChanged -= Phone_PlaybackStreamingChanged;
        foreach (var output in Outputs)
        {
            output.SettingsChanged -= Output_SettingsChanged;
        }
        foreach (var device in PhoneDevices)
        {
            device.SettingsChanged -= PhoneDevice_SettingsChanged;
        }
    }
}
