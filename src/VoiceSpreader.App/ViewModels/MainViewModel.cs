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
    private bool _restartAfterStop;
    private string _statusBadge = "已停止";
    private string _statusTitle = "等待配置";
    private string _statusMessage = "选择一个系统声音来源和至少一个输出设备。";
    private string _programLevelText = "节目电平 -- dBFS · 探针暂停";
    private string? _noticeMessage;
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
        _savedOutputs = new Dictionary<string, OutputSettings>(
            host.Settings.Outputs,
            StringComparer.OrdinalIgnoreCase);
        _startWithWindows = AutoStartService.IsEnabled;

        _engine.StatusChanged += Engine_StatusChanged;
        _engine.ErrorOccurred += Engine_ErrorOccurred;
        _engine.RunningChanged += Engine_RunningChanged;
        _engine.ProgramLevelChanged += Engine_ProgramLevelChanged;
        _engine.CalibrationCompleted += Engine_CalibrationCompleted;
        _bluetooth.StatusChanged += Bluetooth_StatusChanged;
        _bluetooth.ErrorOccurred += Bluetooth_ErrorOccurred;
        _bluetooth.BusyChanged += Bluetooth_BusyChanged;
        _bluetooth.ConnectionChanged += Bluetooth_ConnectionChanged;
        _phone.StatusChanged += Phone_StatusChanged;
        _phone.ConnectionChanged += Phone_ConnectionChanged;
        _phone.MicrophoneStreamingChanged += Phone_MicrophoneStreamingChanged;
        _phone.MicrophoneLevelChanged += Phone_MicrophoneLevelChanged;

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
            }
        }
    }

    public bool ControlsEnabled => !IsRunning && !_isStarting && !IsCalibrating;

    public string StartButtonText => IsRunning || _isStarting ? "停止同步" : "开始同步";

    public string CalibrationButtonText => IsCalibrating ? "停止校准" : "自动校准";

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
                OnPropertyChanged(nameof(PhoneMicrophoneActionText));
            }
        }
    }

    public string PhoneConnectionText => IsPhoneConnected
        ? $"已连接：{_phoneName}"
        : "等待手机连接";

    public string PhoneMicrophoneActionText => IsPhoneMicrophoneStreaming ? "停用麦克风" : "启用麦克风";

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

    public async Task<string?> TogglePhoneMicrophoneAsync() =>
        await _phone.SetMicrophoneEnabledAsync(!IsPhoneMicrophoneStreaming)
            ? null
            : "手机尚未连接或控制命令发送失败。";

    public void DisconnectPhone() => _phone.DisconnectPhone();

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

            SelectedCapture = FindById(CaptureSources, _host.Settings.CaptureDeviceId)
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
        var usePhoneMicrophone = ContinuousAcousticTracking
                                 && selected.Length >= 2
                                 && IsPhoneConnected
                                 && IsPhoneMicrophoneStreaming;
        if (ContinuousAcousticTracking && selected.Length >= 2
            && !usePhoneMicrophone && SelectedMicrophone is null)
        {
            return "启用自同步时，请选择用于连续测量的麦克风。";
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
            usePhoneMicrophone);

        _isStarting = true;
        OnPropertyChanged(nameof(StartButtonText));
        OnPropertyChanged(nameof(ControlsEnabled));
        StatusBadge = "正在初始化";
        StatusTitle = "正在建立同步时钟";
        StatusMessage = $"正在连接 {selected.Length} 个输出端点，软件同步余量 {BufferMilliseconds} ms。";
        AddActivity(StatusMessage);

        if (!_engine.Start(configuration))
        {
            _isStarting = false;
            OnPropertyChanged(nameof(StartButtonText));
            OnPropertyChanged(nameof(ControlsEnabled));
            return "音频引擎已经在启动或运行。";
        }
        return null;
    }

    public string? StartOrStopCalibration()
    {
        if (IsCalibrating || _engine.IsCalibrating)
        {
            _engine.StopCalibration();
            StatusBadge = "正在停止";
            StatusTitle = "正在停止校准";
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

    private async Task SaveSettingsAsync()
    {
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
            Outputs = new Dictionary<string, OutputSettings>(_savedOutputs, StringComparer.OrdinalIgnoreCase),
        };
        try
        {
            await _host.SaveSettingsAsync(settings);
        }
        catch (IOException exception)
        {
            Dispatch(() => NoticeMessage = $"保存设置失败：{exception.Message}");
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
        StatusBadge = running ? "同步中" : "已停止";
        StatusTitle = running ? "所有输出共享同一时间线" : "同步已停止";
        if (!running)
        {
            ProgramLevelText = "节目电平 -- dBFS · 探针暂停";
            if (_restartAfterStop)
            {
                _restartAfterStop = false;
                var error = StartOrStop();
                if (error is not null)
                {
                    NoticeMessage = error;
                }
            }
        }
    });

    private void Engine_ProgramLevelChanged(object? sender, ProgramLevelEventArgs args) => Dispatch(() =>
    {
        var level = Math.Clamp(args.LevelDbfs, -160.0, 0.0);
        ProgramLevelText = $"节目电平 {level:0.0} dBFS · 探针{(args.ProbeAllowed ? "可用" : "暂停")}";
    });

    private void Engine_CalibrationCompleted(object? sender, CalibrationCompletedEventArgs args) => Dispatch(() =>
    {
        IsCalibrating = false;
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
        _phoneName = args.Connected ? args.PhoneName : string.Empty;
        IsPhoneConnected = args.Connected;
        OnPropertyChanged(nameof(PhoneConnectionText));
    });

    private void Phone_MicrophoneStreamingChanged(object? sender, bool enabled) => Dispatch(() =>
    {
        var changed = IsPhoneMicrophoneStreaming != enabled;
        IsPhoneMicrophoneStreaming = enabled;
        if (changed && _engine.IsActive && ContinuousAcousticTracking)
        {
            _restartAfterStop = true;
            AddActivity(enabled
                ? "正在切换为手机麦克风，音频同步将自动重启"
                : "手机麦克风已停用，音频同步将切回本机麦克风");
            _engine.Stop();
        }
    });

    private void Phone_MicrophoneLevelChanged(object? sender, double levelDbfs) => Dispatch(() =>
    {
        PhoneLevelPercent = Math.Clamp((levelDbfs + 60) / 60 * 100, 0, 100);
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
        _engine.StatusChanged -= Engine_StatusChanged;
        _engine.ErrorOccurred -= Engine_ErrorOccurred;
        _engine.RunningChanged -= Engine_RunningChanged;
        _engine.ProgramLevelChanged -= Engine_ProgramLevelChanged;
        _engine.CalibrationCompleted -= Engine_CalibrationCompleted;
        _bluetooth.StatusChanged -= Bluetooth_StatusChanged;
        _bluetooth.ErrorOccurred -= Bluetooth_ErrorOccurred;
        _bluetooth.BusyChanged -= Bluetooth_BusyChanged;
        _bluetooth.ConnectionChanged -= Bluetooth_ConnectionChanged;
        _phone.StatusChanged -= Phone_StatusChanged;
        _phone.ConnectionChanged -= Phone_ConnectionChanged;
        _phone.MicrophoneStreamingChanged -= Phone_MicrophoneStreamingChanged;
        _phone.MicrophoneLevelChanged -= Phone_MicrophoneLevelChanged;
        foreach (var output in Outputs)
        {
            output.SettingsChanged -= Output_SettingsChanged;
        }
    }
}
