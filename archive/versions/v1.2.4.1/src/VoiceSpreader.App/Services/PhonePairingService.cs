using System.Buffers.Binary;
using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;
using VoiceSpreader.App.Models;

namespace VoiceSpreader.App.Services;

public sealed record PhoneDeviceSnapshot(
    string Id,
    string Name,
    string RemoteAddress,
    int Protocol,
    bool MicrophoneStreaming,
    bool PlaybackRequested,
    bool PlaybackStreaming,
    int MicrophoneGainPercent,
    double MicrophoneLevelDbfs,
    double ClockDriftPpm,
    int MicrophoneBufferedMilliseconds,
    DateTimeOffset ConnectedAt,
    IReadOnlySet<string>? Capabilities = null)
{
    public bool SupportsPlayback => Protocol >= 2;
}

public sealed class PhoneDevicesChangedEventArgs(IReadOnlyList<PhoneDeviceSnapshot> devices) : EventArgs
{
    public IReadOnlyList<PhoneDeviceSnapshot> Devices { get; } = devices;
}

public sealed class PhoneConnectionChangedEventArgs(
    bool connected,
    string phoneName,
    string deviceId = "") : EventArgs
{
    public bool Connected { get; } = connected;

    public string PhoneName { get; } = phoneName;

    public string DeviceId { get; } = deviceId;
}

public sealed class PhoneMicrophoneRequestEventArgs(
    string deviceId,
    string deviceName,
    bool enabled,
    long requestId = 0,
    bool stateApplied = false) : EventArgs
{
    public string DeviceId { get; } = deviceId;

    public string DeviceName { get; } = deviceName;

    public bool Enabled { get; } = enabled;

    public long RequestId { get; } = requestId;

    public bool StateApplied { get; } = stateApplied;
}

public sealed class PhoneMicrophoneErrorEventArgs(
    string deviceId,
    string deviceName,
    string message) : EventArgs
{
    public string DeviceId { get; } = deviceId;

    public string DeviceName { get; } = deviceName;

    public string Message { get; } = message;
}

public sealed class PhonePairingService : IDisposable
{
    private const int DiscoveryPort = 39741;
    private const int ReconnectPort = 39742;
    private const int ReconnectAnnouncementCount = 20;
    private const int ReconnectAnnouncementIntervalMilliseconds = 2_000;
    private const int MaximumFrameBytes = 256 * 1024;
    private const int MaximumFileFrameBytes = 1024 * 1024;
    private const int MaximumMicrophoneFrameBytes = (int)RemoteSampleRate * sizeof(short) / 5;
    public const int MicrophoneOutputBufferMilliseconds = 60;
    private const uint RemoteSampleRate = 48_000;
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);
    private static readonly string[] VirtualAdapterMarkers =
        ["vmware", "virtual", "radmin", "zerotier", "vpn", "hyper-v", "vethernet", "wsl"];

    private readonly IRemoteMicrophoneSink _audioEngine;
    private readonly ISystemAudioCaptureSource? _systemAudioSource;
    private readonly object _sessionsLock = new();
    private readonly Dictionary<string, DeviceSession> _sessions =
        new(StringComparer.OrdinalIgnoreCase);
    // protocol 3 功能控制可以与旧版音频连接并行存在，避免新功能连接替换正在传输的音频会话。
    private readonly Dictionary<string, DeviceSession> _featureSessions =
        new(StringComparer.OrdinalIgnoreCase);
    private readonly CancellationTokenSource _lifetime = new();
    private readonly SemaphoreSlim _microphoneSelectionGate = new(1, 1);
    private TcpListener? _listener;
    private UdpClient? _discovery;
    private string _sessionId = string.Empty;
    private string _sessionSecret = string.Empty;
    private string _pairingCode = string.Empty;
    private string? _activeMicrophoneDeviceId;
    private long _microphoneSelectionRevision;
    private long _microphoneControlRevision;
    private long _microphoneCommandId;
    private readonly Dictionary<string, FileTransferState> _fileTransfers =
        new(StringComparer.OrdinalIgnoreCase);
    private bool _disposed;

    public PhonePairingService(
        IRemoteMicrophoneSink audioEngine,
        ISystemAudioCaptureSource? systemAudioSource = null)
    {
        _audioEngine = audioEngine;
        _systemAudioSource = systemAudioSource;
        if (_systemAudioSource is not null)
        {
            _systemAudioSource.SystemAudioFrameReady += SystemAudioSource_FrameReady;
        }
        LocalAddresses = GetLocalIpv4Addresses();
        LocalAddress = LocalAddresses.Count > 0 ? LocalAddresses[0] : IPAddress.Loopback.ToString();
        GenerateCredentials();
    }

    public event EventHandler<string>? StatusChanged;

    public event EventHandler<PhoneConnectionChangedEventArgs>? ConnectionChanged;

    public event EventHandler<PhoneDevicesChangedEventArgs>? DevicesChanged;

    public event EventHandler<PhoneMicrophoneRequestEventArgs>? MicrophoneRouteRequested;

    public event EventHandler<PhoneMicrophoneErrorEventArgs>? MicrophoneControlFailed;

    public event EventHandler<bool>? MicrophoneStreamingChanged;

    public event EventHandler<double>? MicrophoneLevelChanged;

    public event EventHandler<bool>? PlaybackStreamingChanged;

    public event EventHandler<PhoneInputPointerEventArgs>? InputPointerReceived;

    public event EventHandler<PhoneInputButtonEventArgs>? InputButtonReceived;

    public event EventHandler<PhoneInputScrollEventArgs>? InputScrollReceived;

    public event EventHandler<PhoneShortcutEventArgs>? ShortcutReceived;

    public event EventHandler<PhoneFileOfferEventArgs>? FileOfferReceived;

    public event EventHandler<PhoneFileCompleteEventArgs>? FileCompleteReceived;

    public event EventHandler<PhoneScanResultEventArgs>? ScanResultReceived;

    public event EventHandler<PhoneCaptureResultEventArgs>? CaptureResultReceived;

    public Func<IReadOnlyCollection<ComputerShortcut>>? ComputerShortcutsProvider { get; set; }

    public IReadOnlyList<string> LocalAddresses { get; private set; }

    public string LocalAddress { get; private set; }

    public int ServerPort { get; private set; }

    public string PairingCode => _pairingCode;

    public string PairingSessionId => _sessionId;

    public string PairingSecret => _sessionSecret;

    public string PairingPayload => $"VSP1:{LocalAddress}:{ServerPort}:{_sessionId}:{_sessionSecret}";

    public bool RestoreCredentials(string? sessionId, string? secret)
    {
        if (!IsCredential(sessionId) || !IsCredential(secret))
        {
            GenerateCredentials();
            return false;
        }

        _sessionId = sessionId!.ToUpperInvariant();
        _sessionSecret = secret!.ToUpperInvariant();
        return true;
    }

    public IReadOnlyList<PhoneDeviceSnapshot> ConnectedDevices
    {
        get
        {
            lock (_sessionsLock)
            {
                return CreateSnapshotsLocked();
            }
        }
    }

    public bool IsPhoneConnected
    {
        get
        {
            lock (_sessionsLock)
            {
                return _sessions.Count > 0;
            }
        }
    }

    public bool IsMicrophoneStreaming
    {
        get
        {
            lock (_sessionsLock)
            {
                return _sessions.Values.Any(session => session.MicrophoneStreaming);
            }
        }
    }

    public bool IsPlaybackStreaming
    {
        get
        {
            lock (_sessionsLock)
            {
                return _sessions.Values.Any(session => session.PlaybackStreaming);
            }
        }
    }

    public bool SupportsPlayback
    {
        get
        {
            lock (_sessionsLock)
            {
                return _sessions.Values.Any(session => session.Protocol >= 2);
            }
        }
    }

    public string ConnectedPhoneName
    {
        get
        {
            lock (_sessionsLock)
            {
                return _sessions.Count switch
                {
                    0 => string.Empty,
                    1 => _sessions.Values.First().Name,
                    _ => $"{_sessions.Count} 台设备",
                };
            }
        }
    }

    public void Start()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_listener is not null)
        {
            return;
        }

        LocalAddresses = GetLocalIpv4Addresses();
        if (!LocalAddresses.Contains(LocalAddress, StringComparer.OrdinalIgnoreCase))
        {
            LocalAddress = LocalAddresses.Count > 0 ? LocalAddresses[0] : IPAddress.Loopback.ToString();
        }

        _listener = new TcpListener(IPAddress.Any, 0);
        _listener.Start();
        ServerPort = ((IPEndPoint)_listener.LocalEndpoint).Port;
        _ = AcceptLoopAsync(_lifetime.Token);

        try
        {
            _discovery = new UdpClient(AddressFamily.InterNetwork);
            _discovery.Client.ExclusiveAddressUse = false;
            _discovery.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            _discovery.Client.Bind(new IPEndPoint(IPAddress.Any, DiscoveryPort));
            _ = DiscoveryLoopAsync(_lifetime.Token);
        }
        catch (SocketException exception)
        {
            _discovery?.Dispose();
            _discovery = null;
            StatusChanged?.Invoke(this, $"手机配对码发现不可用：{exception.Message}；二维码仍可使用。");
        }

        StatusChanged?.Invoke(this, $"移动设备互联服务已启动：{LocalAddress}:{ServerPort}");
        _ = AnnounceReconnectAsync(_lifetime.Token);
    }

    /// <summary>
    /// 向已保存的手机地址发送主动重连唤醒包。手机端只会用本地保存的配对密钥完成
    /// TCP 握手，因此不需要再次扫码或输入配对码。UDP 包重复发送几次以抵抗局域网丢包。
    /// </summary>
    public async Task<bool> RequestReconnectAsync(
        string? remoteAddress,
        CancellationToken cancellationToken = default)
    {
        if (ServerPort is <= 0 or > 65535)
        {
            return false;
        }

        var payload = Encoding.ASCII.GetBytes($"VSP_RECONNECT {_sessionId} {ServerPort}");
        try
        {
            using var sender = new UdpClient(AddressFamily.InterNetwork)
            {
                EnableBroadcast = true,
            };
            var destinations = new HashSet<IPAddress>();
            if (!string.IsNullOrWhiteSpace(remoteAddress)
                && IPAddress.TryParse(remoteAddress, out var destination)
                && destination.AddressFamily == AddressFamily.InterNetwork)
            {
                destinations.Add(destination);
            }
            // 保存地址可能已经因 DHCP 变化；同时广播一次，让手机端用 session 找到新的电脑端口。
            foreach (var broadcast in GetBroadcastAddresses())
            {
                destinations.Add(broadcast);
            }
            for (var attempt = 0; attempt < 4; attempt++)
            {
                foreach (var target in destinations)
                {
                    await sender.SendAsync(
                            payload,
                            new IPEndPoint(target, ReconnectPort),
                            cancellationToken)
                        .ConfigureAwait(false);
                }
                if (attempt + 1 < 4)
                {
                    await Task.Delay(350, cancellationToken).ConfigureAwait(false);
                }
            }
            return true;
        }
        catch (OperationCanceledException)
        {
            return false;
        }
        catch (SocketException exception)
        {
            StatusChanged?.Invoke(this, $"向手机发送重连请求失败：{exception.Message}");
            return false;
        }
    }

    public bool SetLocalAddress(string address)
    {
        if (!LocalAddresses.Contains(address, StringComparer.OrdinalIgnoreCase))
        {
            return false;
        }

        LocalAddress = address;
        StatusChanged?.Invoke(this, $"移动设备配对地址已切换到 {address}");
        return true;
    }

    public async Task<bool> SetMicrophoneEnabledAsync(bool enabled)
    {
        var controlRevision = Interlocked.Increment(ref _microphoneControlRevision);
        await _microphoneSelectionGate.WaitAsync();
        try
        {
            if (controlRevision != Volatile.Read(ref _microphoneControlRevision))
            {
                return true;
            }
            var session = !enabled
                ? GetActiveMicrophoneSession() ?? GetFirstSession()
                : GetFirstSession();
            if (session is null)
            {
                StatusChanged?.Invoke(this, "尚未连接移动设备，无法切换麦克风。");
                return false;
            }
            return await SetMicrophoneEnabledCoreAsync(session, enabled);
        }
        finally
        {
            _microphoneSelectionGate.Release();
        }
    }

    public async Task<bool> SetMicrophoneEnabledAsync(string deviceId, bool enabled)
    {
        var controlRevision = Interlocked.Increment(ref _microphoneControlRevision);
        await _microphoneSelectionGate.WaitAsync();
        try
        {
            if (controlRevision != Volatile.Read(ref _microphoneControlRevision))
            {
                return true;
            }
            var session = GetSession(deviceId);
            if (session is null)
            {
                StatusChanged?.Invoke(this, "目标设备已经断开，无法切换麦克风。");
                return false;
            }
            return await SetMicrophoneEnabledCoreAsync(session, enabled);
        }
        finally
        {
            _microphoneSelectionGate.Release();
        }
    }

    private async Task<bool> SetMicrophoneEnabledCoreAsync(DeviceSession session, bool enabled)
    {
        if (!enabled)
        {
            var changed = false;
            lock (_sessionsLock)
            {
                if (string.Equals(
                        _activeMicrophoneDeviceId,
                        session.Id,
                        StringComparison.OrdinalIgnoreCase))
                {
                    _activeMicrophoneDeviceId = null;
                    Interlocked.Increment(ref _microphoneSelectionRevision);
                }
                session.MicrophoneReportedActive = false;
                changed = DeactivateMicrophoneSessionLocked(session);
            }
            if (changed)
            {
                RaiseMicrophoneStateChanged();
            }
            return await SendMicrophoneCommandAsync(session, false, force: true);
        }

        DeviceSession[] otherSessions;
        var selectionChanged = false;
        lock (_sessionsLock)
        {
            _activeMicrophoneDeviceId = session.Id;
            Interlocked.Increment(ref _microphoneSelectionRevision);
            session.ClockEstimator.Reset();
            session.ClockDriftPpm = 0;
            otherSessions = _sessions.Values
                .Where(item => !ReferenceEquals(item, session))
                .ToArray();
            foreach (var otherSession in otherSessions)
            {
                otherSession.MicrophoneReportedActive = false;
                selectionChanged |= DeactivateMicrophoneSessionLocked(otherSession);
            }
            if (session.MicrophoneReportedActive && !session.MicrophoneStreaming)
            {
                session.MicrophoneStreaming = true;
                selectionChanged = true;
            }
        }
        if (selectionChanged)
        {
            RaiseMicrophoneStateChanged();
        }

        // Android 端只有收到停用命令后才会释放 AudioRecord，切换来源时必须先停旧设备。
        foreach (var otherSession in otherSessions)
        {
            await SendMicrophoneCommandAsync(otherSession, false);
        }

        var enabledSelectedSession = await SendMicrophoneCommandAsync(session, true);
        if (enabledSelectedSession)
        {
            return true;
        }

        lock (_sessionsLock)
        {
            if (string.Equals(
                    _activeMicrophoneDeviceId,
                    session.Id,
                    StringComparison.OrdinalIgnoreCase))
            {
                _activeMicrophoneDeviceId = null;
                Interlocked.Increment(ref _microphoneSelectionRevision);
            }
            DeactivateMicrophoneSessionLocked(session);
            session.MicrophoneReportedActive = false;
        }
        RaiseMicrophoneStateChanged();
        return false;
    }

    private async Task<bool> SendMicrophoneCommandAsync(
        DeviceSession session,
        bool enabled,
        bool force = false)
    {
        var revision = Interlocked.Increment(ref session.MicrophoneCommandRevision);
        try
        {
            await session.MicrophoneCommandGate.WaitAsync(session.Cancellation.Token);
            try
            {
                if (!force && revision != Volatile.Read(ref session.MicrophoneCommandRevision))
                {
                    return true;
                }
                // 在发送控制帧前记录期望状态。手机回传类型 2 时，只有匹配该状态
                // 才是控制确认；不匹配或没有待确认命令则视为手机主动切换。
                var commandId = Interlocked.Increment(ref _microphoneCommandId);
                lock (_sessionsLock)
                {
                    if (IsCurrentSessionLocked(session))
                    {
                        session.MicrophoneCommandPending = true;
                        session.MicrophoneCommandExpectedEnabled = enabled;
                        session.MicrophoneCommandExpectedId = commandId;
                    }
                }
                if (session.Protocol >= 2)
                {
                    if (session.SupportsMicrophoneSyncRevision)
                    {
                        var body = new byte[10];
                        body[0] = 10;
                        body[1] = enabled ? (byte)1 : (byte)0;
                        BinaryPrimitives.WriteUInt64BigEndian(
                            body.AsSpan(2, sizeof(long)),
                            unchecked((ulong)commandId));
                        await SendBinaryFrameAsync(body, session, session.Cancellation.Token);
                    }
                    else
                    {
                        await SendBinaryFrameAsync(
                            new byte[] { 10, enabled ? (byte)1 : (byte)0 },
                            session,
                            session.Cancellation.Token);
                    }
                }
                else
                {
                    await SendJsonLineAsync(
                        new { type = "setMicrophone", enabled },
                        session,
                        session.Cancellation.Token);
                }
            }
            finally
            {
                session.MicrophoneCommandGate.Release();
            }
            StatusChanged?.Invoke(
                this,
                enabled
                    ? $"已请求 {session.Name} 启用麦克风。"
                    : $"已请求 {session.Name} 停止并释放麦克风。");
            return true;
        }
        catch (Exception exception) when (exception is IOException
                                          or SocketException
                                          or ObjectDisposedException
                                          or OperationCanceledException)
        {
            lock (_sessionsLock)
            {
                if (IsCurrentSessionLocked(session))
                {
                    session.MicrophoneCommandPending = false;
                    session.MicrophoneCommandExpectedId = 0;
                }
            }
            StatusChanged?.Invoke(this, $"向 {session.Name} 发送麦克风命令失败：{exception.Message}");
            return false;
        }
    }

    public async Task<bool> SetPlaybackEnabledAsync(bool enabled)
    {
        var session = GetFirstSession(requirePlayback: true);
        if (session is null)
        {
            StatusChanged?.Invoke(this, "尚未连接支持播放的移动设备。");
            return false;
        }
        return await SetPlaybackEnabledAsync(session.Id, enabled);
    }

    public async Task<bool> SetPlaybackEnabledAsync(string deviceId, bool enabled)
    {
        var session = GetSession(deviceId);
        if (session is null || session.Protocol < 2)
        {
            StatusChanged?.Invoke(
                this,
                session is null
                    ? "目标设备已经断开，无法切换播放。"
                    : $"{session.Name} 的客户端版本不支持接收 Windows 声音。");
            return false;
        }

        var revision = Interlocked.Increment(ref session.PlaybackCommandRevision);
        try
        {
            await session.PlaybackCommandGate.WaitAsync(session.Cancellation.Token);
            try
            {
                if (revision != Volatile.Read(ref session.PlaybackCommandRevision))
                {
                    return true;
                }
                await SendBinaryFrameAsync(
                    new byte[] { 11, enabled ? (byte)1 : (byte)0 },
                    session,
                    session.Cancellation.Token);
            }
            finally
            {
                session.PlaybackCommandGate.Release();
            }
            lock (_sessionsLock)
            {
                if (IsCurrentSessionLocked(session))
                {
                    session.PlaybackRequested = enabled;
                    if (!enabled)
                    {
                        session.PlaybackStreaming = false;
                    }
                }
            }
            RaiseDevicesChanged();
            RaiseAggregatePlaybackState();
            StatusChanged?.Invoke(
                this,
                enabled
                    ? $"已请求 {session.Name} 准备播放 Windows 声音。"
                    : $"已请求 {session.Name} 停止播放 Windows 声音。");
            return true;
        }
        catch (Exception exception) when (exception is IOException
                                          or SocketException
                                          or ObjectDisposedException
                                          or OperationCanceledException)
        {
            StatusChanged?.Invoke(this, $"向 {session.Name} 发送播放命令失败：{exception.Message}");
            return false;
        }
    }

    /// <summary>
    /// 让电脑端请求手机打开内置扫码器。请求只发送到已经完成配对的控制通道，
    /// 具体的摄像头/相册权限仍由 Android 在前台界面中向用户申请。
    /// </summary>
    public Task<bool> RequestScanAsync(
        string deviceId,
        string mode = "camera",
        CancellationToken cancellationToken = default)
    {
        var normalizedMode = string.Equals(mode, "gallery", StringComparison.OrdinalIgnoreCase)
            ? "gallery"
            : "camera";
        var session = GetSession(deviceId, includeFeatureSession: true);
        if (session is null || session.Protocol < 3)
        {
            StatusChanged?.Invoke(this, "目标手机不支持 v1.2.4 扫码功能");
            return Task.FromResult(false);
        }

        var capability = normalizedMode == "gallery" ? "scan.gallery" : "scan.camera";
        if (!SupportsCapability(session, capability))
        {
            StatusChanged?.Invoke(this, $"{session.Name} 尚未声明 {capability} 能力");
            return Task.FromResult(false);
        }

        return SendFeatureRequestAsync(
            session,
            50,
            new
            {
                requestId = Guid.NewGuid().ToString("N"),
                mode = normalizedMode,
                autoOpen = true,
            },
            cancellationToken);
    }

    /// <summary>
    /// 请求手机拍摄一张照片并通过文件通道传回电脑。
    /// </summary>
    public Task<bool> RequestCaptureAsync(
        string deviceId,
        CancellationToken cancellationToken = default)
    {
        var session = GetSession(deviceId, includeFeatureSession: true);
        if (session is null || session.Protocol < 3)
        {
            StatusChanged?.Invoke(this, "目标手机不支持 v1.2.4 拍照传输功能");
            return Task.FromResult(false);
        }
        if (!SupportsCapability(session, "camera.capture"))
        {
            StatusChanged?.Invoke(this, $"{session.Name} 尚未声明 camera.capture 能力");
            return Task.FromResult(false);
        }

        return SendFeatureRequestAsync(
            session,
            60,
            new
            {
                requestId = Guid.NewGuid().ToString("N"),
                mime = "image/jpeg",
                quality = 92,
            },
            cancellationToken);
    }

    /// <summary>把 Windows 端已配置的应用白名单同步到手机触摸板。</summary>
    public void BroadcastComputerShortcuts(IReadOnlyCollection<ComputerShortcut> shortcuts)
    {
        DeviceSession[] sessions;
        lock (_sessionsLock)
        {
            sessions = _featureSessions.Values.ToArray();
        }

        foreach (var session in sessions)
        {
            if (!SupportsCapability(session, "input.app"))
            {
                continue;
            }

            _ = SendComputerShortcutCatalogAsync(session, shortcuts);
        }
    }

    private static async Task SendComputerShortcutCatalogAsync(
        DeviceSession session,
        IReadOnlyCollection<ComputerShortcut> shortcuts)
    {
        try
        {
            await SendJsonFrameAsync(
                session,
                34,
                new
                {
                    apps = shortcuts
                        .Where(item => !string.IsNullOrWhiteSpace(item.Id)
                                       && !string.IsNullOrWhiteSpace(item.Name))
                        .Take(24)
                        .Select(item => new { id = item.Id, name = item.Name }),
                },
                session.Cancellation.Token).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is IOException
                                          or SocketException
                                          or ObjectDisposedException
                                          or OperationCanceledException)
        {
            // 控制通道断开时由会话清理和下次连接重新发送目录。
        }
    }

    private static async Task<bool> SendFeatureRequestAsync(
        DeviceSession session,
        byte type,
        object payload,
        CancellationToken cancellationToken)
    {
        try
        {
            await SendJsonFrameAsync(session, type, payload, cancellationToken).ConfigureAwait(false);
            return true;
        }
        catch (Exception exception) when (exception is IOException
                                          or SocketException
                                          or ObjectDisposedException
                                          or OperationCanceledException)
        {
            return false;
        }
    }

    public void SetMicrophoneGain(string deviceId, int gainPercent)
    {
        lock (_sessionsLock)
        {
            if (_sessions.TryGetValue(deviceId, out var session))
            {
                session.MicrophoneGainPercent = Math.Clamp(gainPercent, 0, 200);
            }
        }
        RaiseDevicesChanged();
    }

    public void DisconnectDevice(string deviceId)
    {
        DeviceSession? audioSession;
        DeviceSession? featureSession;
        lock (_sessionsLock)
        {
            _sessions.TryGetValue(deviceId, out audioSession);
            _featureSessions.TryGetValue(deviceId, out featureSession);
        }
        foreach (var session in new[] { audioSession, featureSession }.OfType<DeviceSession>())
        {
            NotifyRemoteDisconnect(session);
            session.DisableAutoReconnect();
            session.Stop();
        }
    }

    public void DisconnectPhone(bool notifyRemote = true)
    {
        DeviceSession[] sessions;
        lock (_sessionsLock)
        {
            sessions = [.. _sessions.Values.Concat(_featureSessions.Values)];
        }
        foreach (var session in sessions)
        {
            if (notifyRemote)
            {
                NotifyRemoteDisconnect(session);
            }
            session.DisableAutoReconnect();
            session.Stop();
        }
    }

    public void ResetPairing()
    {
        DisconnectPhone();
        GenerateCredentials();
        StatusChanged?.Invoke(this, "已生成新的移动设备配对凭据。");
    }

    private async Task AcceptLoopAsync(CancellationToken cancellationToken)
    {
        try
        {
            while (!cancellationToken.IsCancellationRequested && _listener is not null)
            {
                var incoming = await _listener.AcceptTcpClientAsync(cancellationToken);
                incoming.NoDelay = true;
                _ = HandleClientAsync(incoming, cancellationToken);
            }
        }
        catch (OperationCanceledException)
        {
            // 应用退出时取消监听属于正常生命周期。
        }
        catch (SocketException exception)
        {
            StatusChanged?.Invoke(this, $"移动设备配对监听已停止：{exception.Message}");
        }
    }

    private async Task HandleClientAsync(TcpClient client, CancellationToken cancellationToken)
    {
        DeviceSession? deviceSession = null;
        var stream = client.GetStream();
        try
        {
            var handshake = await ReadLineAsync(stream, 4096, cancellationToken);
            if (!TryAuthenticate(
                    handshake,
                    (client.Client.RemoteEndPoint as IPEndPoint)?.Address.ToString() ?? string.Empty,
                    out var deviceId,
                    out var deviceName,
                    out var protocol,
                    out var microphoneRequests,
                    out var microphoneSyncRevision,
                    out var channel,
                    out var transferId,
                    out var capabilities))
            {
                await SendUncoordinatedJsonLineAsync(
                    new { type = "error", message = "移动设备配对凭据不匹配" },
                    stream,
                    cancellationToken);
                return;
            }

            if (string.Equals(channel, "file", StringComparison.OrdinalIgnoreCase))
            {
                await HandleFileChannelAsync(
                    client,
                    deviceId,
                    deviceName,
                    transferId,
                    capabilities,
                    cancellationToken);
                return;
            }

            var featureOnly = protocol >= 3
                               && capabilities.Any(IsFeatureCapability)
                               && !capabilities.Contains("audio");

            deviceSession = new DeviceSession(
                deviceId,
                deviceName,
                (client.Client.RemoteEndPoint as IPEndPoint)?.Address.ToString() ?? string.Empty,
                protocol,
                microphoneRequests,
                microphoneSyncRevision,
                capabilities,
                featureOnly,
                client,
                CancellationTokenSource.CreateLinkedTokenSource(_lifetime.Token));
            await SendJsonLineAsync(
                new
                {
                    type = "accepted",
                    protocol,
                    sampleRate = RemoteSampleRate,
                    microphoneEnabled = false,
                    playbackEnabled = false,
                    multiDevice = true,
                    microphoneRequests = protocol >= 2,
                    microphoneSyncRevision = protocol >= 2 && microphoneSyncRevision,
                    capabilities = capabilities.OrderBy(value => value, StringComparer.Ordinal).ToArray(),
                },
                deviceSession,
                cancellationToken);

            DeviceSession? replacedSession = null;
            lock (_sessionsLock)
            {
                var targetSessions = featureOnly ? _featureSessions : _sessions;
                if (targetSessions.TryGetValue(deviceId, out var existing))
                {
                    replacedSession = existing;
                }
                targetSessions[deviceId] = deviceSession;
            }
            replacedSession?.Stop();
            if (!featureOnly)
            {
                deviceSession.PlaybackSendTask = PlaybackSendLoopAsync(deviceSession);
                ConnectionChanged?.Invoke(
                    this,
                    new PhoneConnectionChangedEventArgs(true, deviceName, deviceId));
            }
            else
            {
                // 功能通道建立后立即下发电脑端应用目录，手机无需再次扫描即可显示快捷入口。
                await SendComputerShortcutCatalogAsync(
                    deviceSession,
                    ComputerShortcutsProvider?.Invoke() ?? Array.Empty<ComputerShortcut>());
            }
            StatusChanged?.Invoke(this, $"移动设备已连接：{deviceName}；音频链路保持关闭。");
            if (!featureOnly)
            {
                RaiseDevicesChanged();
            }

            var lengthBytes = new byte[4];
            while (!deviceSession.Cancellation.IsCancellationRequested
                   && !deviceSession.RemoteDisconnectRequested)
            {
                if (!await TryReadExactlyAsync(
                        stream,
                        lengthBytes,
                        deviceSession.Cancellation.Token))
                {
                    break;
                }
                var bodyLength = BinaryPrimitives.ReadUInt32BigEndian(lengthBytes);
                if (bodyLength is < 1 or > MaximumFrameBytes)
                {
                    throw new InvalidDataException("移动设备数据帧长度无效。");
                }

                var body = new byte[bodyLength];
                if (!await TryReadExactlyAsync(stream, body, deviceSession.Cancellation.Token))
                {
                    break;
                }
                ProcessFrame(deviceSession, body);
            }
        }
        catch (OperationCanceledException)
        {
            // 应用退出、设备重连或主动断开时不显示协议错误。
        }
        catch (Exception exception) when (exception is IOException
                                          or SocketException
                                          or InvalidDataException
                                          or ObjectDisposedException)
        {
            StatusChanged?.Invoke(this, $"移动设备连接异常：{exception.Message}");
        }
        finally
        {
            client.Dispose();
            if (deviceSession is not null)
            {
                var removed = false;
                lock (_sessionsLock)
                {
                    var targetSessions = deviceSession.FeatureOnly ? _featureSessions : _sessions;
                    if (targetSessions.TryGetValue(deviceSession.Id, out var current)
                        && ReferenceEquals(current, deviceSession))
                    {
                        targetSessions.Remove(deviceSession.Id);
                        if (!deviceSession.FeatureOnly && string.Equals(
                                _activeMicrophoneDeviceId,
                                deviceSession.Id,
                                StringComparison.OrdinalIgnoreCase))
                        {
                            _activeMicrophoneDeviceId = null;
                            Interlocked.Increment(ref _microphoneSelectionRevision);
                        }
                        removed = true;
                    }
                }
                deviceSession.Stop();
                if (removed)
                {
                    if (!deviceSession.FeatureOnly)
                    {
                        ConnectionChanged?.Invoke(
                        this,
                        new PhoneConnectionChangedEventArgs(
                            false,
                            deviceSession.Name,
                            deviceSession.Id));
                    }
                    StatusChanged?.Invoke(this, $"移动设备已断开：{deviceSession.Name}");
                    if (!deviceSession.FeatureOnly)
                    {
                        RaiseDevicesChanged();
                        RaiseAggregateMicrophoneState();
                        RaiseAggregatePlaybackState();
                        if (deviceSession.AutoReconnect)
                        {
                            _ = AnnounceReconnectAsync(_lifetime.Token);
                        }
                    }
                }
            }
        }
    }

    private bool TryAuthenticate(
        string handshake,
        string remoteAddress,
        out string deviceId,
        out string deviceName,
        out int negotiatedProtocol,
        out bool microphoneRequests,
        out bool microphoneSyncRevision,
        out string channel,
        out string? transferId,
        out HashSet<string> capabilities)
    {
        deviceId = string.Empty;
        deviceName = string.Empty;
        negotiatedProtocol = 1;
        microphoneRequests = false;
        microphoneSyncRevision = false;
        channel = "control";
        transferId = null;
        capabilities = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            using var document = JsonDocument.Parse(handshake);
            var root = document.RootElement;
            var protocolValue = root.TryGetProperty("protocol", out var protocol)
                ? protocol.GetInt32()
                : 0;
            var valid = root.TryGetProperty("type", out var type)
                        && type.GetString() == "hello"
                        && protocolValue is >= 1 and <= 3
                        && root.TryGetProperty("session", out var session)
                        && string.Equals(session.GetString(), _sessionId, StringComparison.OrdinalIgnoreCase)
                        && root.TryGetProperty("secret", out var secret)
                        && string.Equals(secret.GetString(), _sessionSecret, StringComparison.OrdinalIgnoreCase);
            if (!valid)
            {
                return false;
            }

            negotiatedProtocol = protocolValue;
            channel = root.TryGetProperty("channel", out var channelElement)
                ? channelElement.GetString() ?? "control"
                : "control";
            if (!string.Equals(channel, "control", StringComparison.OrdinalIgnoreCase)
                && !string.Equals(channel, "file", StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }
            transferId = root.TryGetProperty("transferId", out var transferElement)
                ? transferElement.GetString()
                : null;
            if (string.Equals(channel, "file", StringComparison.OrdinalIgnoreCase)
                && (protocolValue < 3 || string.IsNullOrWhiteSpace(transferId)))
            {
                return false;
            }
            if (root.TryGetProperty("capabilities", out var capabilityElement)
                && capabilityElement.ValueKind == JsonValueKind.Array)
            {
                foreach (var value in capabilityElement.EnumerateArray())
                {
                    var capability = value.GetString()?.Trim();
                    if (!string.IsNullOrWhiteSpace(capability)
                        && capability.Length <= 64
                        && capability.All(character => !char.IsControl(character)))
                    {
                        capabilities.Add(capability);
                    }
                }
            }
            microphoneRequests = protocolValue >= 2
                                 && root.TryGetProperty("microphoneRequests", out var requestCapability)
                                 && requestCapability.ValueKind == JsonValueKind.True;
            microphoneSyncRevision = protocolValue >= 2
                                     && root.TryGetProperty("microphoneSyncRevision", out var syncCapability)
                                     && syncCapability.ValueKind == JsonValueKind.True;
            deviceName = root.TryGetProperty("deviceName", out var name)
                ? name.GetString() ?? string.Empty
                : string.Empty;
            if (string.IsNullOrWhiteSpace(deviceName))
            {
                deviceName = "Android 设备";
            }
            deviceName = new string(deviceName
                .Trim()
                .Take(80)
                .Where(character => !char.IsControl(character))
                .ToArray());

            deviceId = root.TryGetProperty("deviceId", out var identifier)
                ? identifier.GetString() ?? string.Empty
                : string.Empty;
            deviceId = NormalizeDeviceId(deviceId);
            if (string.IsNullOrWhiteSpace(deviceId))
            {
                var legacyIdentity = Encoding.UTF8.GetBytes($"{deviceName}|{remoteAddress}");
                deviceId = $"legacy-{Convert.ToHexString(SHA256.HashData(legacyIdentity))[..16]}";
            }
            return true;
        }
        catch (Exception exception) when (exception is JsonException or InvalidOperationException)
        {
            return false;
        }
    }

    private void ProcessFrame(DeviceSession session, byte[] body)
    {
        session.LastSeen = DateTimeOffset.UtcNow;
        if (body.Length > 0 && body[0] >= 30)
        {
            ProcessFeatureFrame(session, body);
            return;
        }
        switch (body[0])
        {
            case 1 when body.Length >= 13:
                ProcessPcmFrame(session, body);
                break;
            case 2 when body.Length >= 2:
                // 类型 2 是手机实际采集状态；新客户端额外回传命令序号，用于丢弃迟到回包。
                var microphoneStateId = session.SupportsMicrophoneSyncRevision && body.Length >= 10
                    ? unchecked((long)BinaryPrimitives.ReadUInt64BigEndian(body.AsSpan(2, sizeof(long))))
                    : 0;
                UpdateMicrophoneStreaming(session, body[1] != 0, microphoneStateId);
                break;
            case 3:
                var microphoneError = Encoding.UTF8.GetString(body, 1, body.Length - 1);
                StatusChanged?.Invoke(
                    this,
                    $"{session.Name} 麦克风：{microphoneError}");
                long microphoneErrorStateId;
                lock (_sessionsLock)
                {
                    microphoneErrorStateId = session.MicrophoneCommandExpectedId;
                }
                UpdateMicrophoneStreaming(session, false, microphoneErrorStateId);
                MicrophoneControlFailed?.Invoke(
                    this,
                    new PhoneMicrophoneErrorEventArgs(session.Id, session.Name, microphoneError));
                break;
            case 4 when body.Length >= 17:
                ProcessClockFrame(session, body);
                break;
            case 5 when body.Length >= 2:
                UpdatePlaybackStreaming(session, body[1] != 0);
                break;
            case 6:
                StatusChanged?.Invoke(
                    this,
                    $"{session.Name} 播放：{Encoding.UTF8.GetString(body, 1, body.Length - 1)}");
                lock (_sessionsLock)
                {
                    session.PlaybackRequested = false;
                }
                UpdatePlaybackStreaming(session, false);
                break;
            case 7 when session.Protocol >= 2 && body.Length >= 2:
                var microphoneRouteEnabled = body[1] != 0;
                var microphoneRequestId = session.SupportsMicrophoneSyncRevision && body.Length >= 10
                    ? unchecked((long)BinaryPrimitives.ReadUInt64BigEndian(body.AsSpan(2, sizeof(long))))
                    : 0;
                if (microphoneRequestId > 0)
                {
                    lock (_sessionsLock)
                    {
                        if (!IsCurrentSessionLocked(session))
                        {
                            break;
                        }
                        if (microphoneRequestId <= session.LastMicrophoneRequestId)
                        {
                            break;
                        }
                        session.LastMicrophoneRequestId = microphoneRequestId;
                    }
                }
                // 新客户端的停止是明确的 desired=false。先在服务层收敛 Windows 状态并
                // 回发带命令编号的停止确认，不能把关键动作完全交给 UI 队列。
                var stateApplied = !microphoneRouteEnabled && session.Protocol >= 2;
                if (stateApplied)
                {
                    _ = ApplyPhoneMicrophoneStopAsync(session);
                }
                if (!session.SupportsMicrophoneSyncRevision && !microphoneRouteEnabled)
                {
                    // 旧版客户端没有命令序号，保留停止请求的即时收尾；新客户端由
                    // Windows 协调器完成一次完整的 desired -> reported 流程。
                    var stateChanged = false;
                    lock (_sessionsLock)
                    {
                        if (!IsCurrentSessionLocked(session))
                        {
                            break;
                        }
                        if (IsActiveMicrophoneSessionLocked(session))
                        {
                            _activeMicrophoneDeviceId = null;
                            Interlocked.Increment(ref _microphoneSelectionRevision);
                            stateChanged = true;
                        }
                        session.MicrophoneReportedActive = false;
                        stateChanged |= DeactivateMicrophoneSessionLocked(session);
                    }
                    if (stateChanged)
                    {
                        RaiseMicrophoneStateChanged();
                    }
                }
                StatusChanged?.Invoke(
                    this,
                    $"{session.Name} 请求{(microphoneRouteEnabled ? "启用" : "停止")}手机麦克风路由。");
                MicrophoneRouteRequested?.Invoke(
                    this,
                    new PhoneMicrophoneRequestEventArgs(
                        session.Id,
                        session.Name,
                        microphoneRouteEnabled,
                        microphoneRequestId,
                        stateApplied));
                break;
            case 13 when body.Length == 1:
                // 手机主动断开时明确关闭自动重连，避免正常操作触发重连广播。
                session.DisableAutoReconnect();
                session.RemoteDisconnectRequested = true;
                break;
        }
    }

    private async Task HandleFileChannelAsync(
        TcpClient client,
        string deviceId,
        string deviceName,
        string? transferId,
        HashSet<string> capabilities,
        CancellationToken cancellationToken)
    {
        if (!capabilities.Contains("file.send") || string.IsNullOrWhiteSpace(transferId))
        {
            return;
        }

        FileTransferState? transfer;
        lock (_sessionsLock)
        {
            _fileTransfers.TryGetValue(transferId, out transfer);
        }
        if (transfer is null
            || !string.Equals(transfer.DeviceId, deviceId, StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        var stream = client.GetStream();
        await WriteJsonLineAsync(
            stream,
            new { type = "accepted", protocol = 3, channel = "file", transferId },
            cancellationToken).ConfigureAwait(false);
        var lengthBytes = new byte[4];
        while (!cancellationToken.IsCancellationRequested)
        {
            if (!await TryReadExactlyAsync(stream, lengthBytes, cancellationToken).ConfigureAwait(false))
            {
                break;
            }
            var bodyLength = BinaryPrimitives.ReadUInt32BigEndian(lengthBytes);
            if (bodyLength is < 25 or > MaximumFileFrameBytes)
            {
                throw new InvalidDataException("文件数据帧长度无效");
            }
            var body = new byte[bodyLength];
            if (!await TryReadExactlyAsync(stream, body, cancellationToken).ConfigureAwait(false))
            {
                break;
            }
            if (body[0] != 42)
            {
                continue;
            }
            await WriteFileChunkAsync(transfer, body, cancellationToken).ConfigureAwait(false);
        }
    }

    private async Task WriteFileChunkAsync(
        FileTransferState transfer,
        byte[] body,
        CancellationToken cancellationToken)
    {
        var itemId = new Guid(body.AsSpan(1, 16), bigEndian: true).ToString("N");
        var offset = BinaryPrimitives.ReadUInt64BigEndian(body.AsSpan(17, 8));
        FileItemTransfer? item;
        lock (_sessionsLock)
        {
            transfer.Items.TryGetValue(itemId, out item);
        }
        if (item is null || offset != (ulong)item.ReceivedBytes)
        {
            throw new InvalidDataException("文件块序号无效");
        }

        var dataLength = body.Length - 25;
        if (dataLength <= 0 || offset + (ulong)dataLength > (ulong)item.Size)
        {
            throw new InvalidDataException("文件块大小无效");
        }
        item.Stream ??= new FileStream(
            item.PartPath,
            new FileStreamOptions
            {
                Access = FileAccess.Write,
                Mode = FileMode.OpenOrCreate,
                Share = FileShare.Read,
                BufferSize = 1024 * 1024,
                Options = FileOptions.Asynchronous | FileOptions.SequentialScan,
            });
        item.Stream.Position = checked((long)offset);
        await item.Stream.WriteAsync(body.AsMemory(25, dataLength), cancellationToken)
            .ConfigureAwait(false);
        item.ReceivedBytes = checked((long)offset + dataLength);
    }

    private bool TryRegisterFileOffer(
        DeviceSession session,
        string payload,
        out FileTransferState? state)
    {
        state = null;
        try
        {
            using var document = JsonDocument.Parse(payload);
            var root = document.RootElement;
            var transferId = root.GetProperty("transferId").GetString();
            if (string.IsNullOrWhiteSpace(transferId) || transferId.Length > 64)
            {
                return false;
            }
            var itemsElement = root.GetProperty("items");
            if (itemsElement.ValueKind != JsonValueKind.Array || itemsElement.GetArrayLength() is < 1 or > 64)
            {
                return false;
            }

            var directory = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                "Downloads",
                "VoiceSpreader");
            Directory.CreateDirectory(directory);
            var transferState = new FileTransferState(transferId, session.Id, session.Name);
            foreach (var element in itemsElement.EnumerateArray())
            {
                var itemId = element.GetProperty("itemId").GetString();
                var name = element.GetProperty("name").GetString();
                var size = element.GetProperty("size").GetInt64();
                if (!TryNormalizeGuid(itemId, out var normalizedItemId)
                    || string.IsNullOrWhiteSpace(name)
                    || size is < 0 or > 16L * 1024 * 1024 * 1024)
                {
                    return false;
                }
                var safeName = SanitizeFileName(name);
                var finalPath = GetUniquePath(directory, safeName);
                transferState.Items[normalizedItemId] = new FileItemTransfer(
                    normalizedItemId,
                    safeName,
                    size,
                    finalPath,
                    finalPath + ".vsp-part");
            }
            lock (_sessionsLock)
            {
                _fileTransfers[transferId] = transferState;
            }
            state = transferState;
            _ = SendFileAcceptAsync(session, transferState);
            return true;
        }
        catch (Exception exception) when (exception is JsonException or KeyNotFoundException or InvalidOperationException or IOException)
        {
            StatusChanged?.Invoke(this, $"文件传输请求无效：{exception.Message}");
            return false;
        }
    }

    private static async Task SendFileAcceptAsync(DeviceSession session, FileTransferState transfer)
    {
        var items = transfer.Items.Values.Select(item => new
        {
            itemId = item.ItemId,
            acceptedOffset = File.Exists(item.PartPath) ? new FileInfo(item.PartPath).Length : 0,
        });
        await SendJsonFrameAsync(
            session,
            41,
            new { transferId = transfer.TransferId, items },
            session.Cancellation.Token).ConfigureAwait(false);
    }

    private void CancelFileTransfer(string deviceId, string transferId)
    {
        lock (_sessionsLock)
        {
            if (!_fileTransfers.TryGetValue(transferId, out var transfer)
                || !string.Equals(transfer.DeviceId, deviceId, StringComparison.OrdinalIgnoreCase))
            {
                return;
            }
            foreach (var item in transfer.Items.Values)
            {
                item.Stream?.Dispose();
                item.Stream = null;
            }
            _fileTransfers.Remove(transferId);
        }
    }

    private async Task FinalizeFileTransferAsync(DeviceSession session, string payload)
    {
        try
        {
            using var document = JsonDocument.Parse(payload);
            var root = document.RootElement;
            var transferId = root.GetProperty("transferId").GetString();
            var itemId = root.GetProperty("itemId").GetString();
            var expectedHash = root.GetProperty("sha256").GetString();
            if (string.IsNullOrWhiteSpace(transferId)
                || !TryNormalizeGuid(itemId, out var normalizedItemId)
                || string.IsNullOrWhiteSpace(expectedHash)
                || expectedHash.Length != 64)
            {
                return;
            }
            FileTransferState? transfer = null;
            FileItemTransfer? item = null;
            lock (_sessionsLock)
            {
                _fileTransfers.TryGetValue(transferId, out transfer);
                transfer?.Items.TryGetValue(normalizedItemId, out item);
            }
            if (transfer is null || item is null || item.ReceivedBytes != item.Size)
            {
                return;
            }
            item.Stream?.Dispose();
            item.Stream = null;
            var actualHash = Convert.ToHexString(await SHA256.HashDataAsync(
                File.OpenRead(item.PartPath),
                session.Cancellation.Token).ConfigureAwait(false));
            var valid = string.Equals(actualHash, expectedHash, StringComparison.OrdinalIgnoreCase);
            if (valid)
            {
                File.Move(item.PartPath, item.FinalPath, overwrite: false);
            }
            FileCompleteReceived?.Invoke(
                this,
                new PhoneFileCompleteEventArgs(session.Id, session.Name, payload));
            await SendJsonFrameAsync(
                session,
                43,
                new { transferId, itemId = normalizedItemId, ok = valid, sha256 = actualHash },
                session.Cancellation.Token).ConfigureAwait(false);
            if (valid)
            {
                lock (_sessionsLock)
                {
                    transfer.Items.Remove(normalizedItemId);
                    if (transfer.Items.Count == 0)
                    {
                        _fileTransfers.Remove(transferId);
                    }
                }
            }
        }
        catch (Exception exception) when (exception is JsonException or KeyNotFoundException or InvalidOperationException or IOException or UnauthorizedAccessException)
        {
            StatusChanged?.Invoke(this, $"文件校验失败：{exception.Message}");
        }
    }

    private static async Task SendJsonFrameAsync(
        DeviceSession session,
        byte type,
        object payload,
        CancellationToken cancellationToken)
    {
        var json = JsonSerializer.SerializeToUtf8Bytes(payload, JsonOptions);
        var body = new byte[1 + json.Length];
        body[0] = type;
        json.CopyTo(body, 1);
        await SendBinaryFrameAsync(body, session, cancellationToken).ConfigureAwait(false);
    }

    private static async Task WriteJsonLineAsync(
        NetworkStream stream,
        object payload,
        CancellationToken cancellationToken)
    {
        var bytes = JsonSerializer.SerializeToUtf8Bytes(payload, JsonOptions);
        await stream.WriteAsync(bytes, cancellationToken).ConfigureAwait(false);
        await stream.WriteAsync("\n"u8.ToArray(), cancellationToken).ConfigureAwait(false);
    }

    private static bool TryNormalizeGuid(string? value, out string normalized)
    {
        normalized = string.Empty;
        return Guid.TryParse(value, out var guid)
               && AssignNormalizedGuid(guid, out normalized);
    }

    private static bool AssignNormalizedGuid(Guid value, out string normalized)
    {
        normalized = value.ToString("N");
        return true;
    }

    private static string SanitizeFileName(string value)
    {
        var name = Path.GetFileName(value.Trim());
        var invalid = Path.GetInvalidFileNameChars();
        var builder = new StringBuilder(name.Length);
        foreach (var character in name)
        {
            builder.Append(invalid.Contains(character) || char.IsControl(character) ? '_' : character);
        }
        var result = builder.ToString().Trim().TrimEnd('.');
        return string.IsNullOrWhiteSpace(result) ? "VoiceSpreader-file" : result[..Math.Min(result.Length, 180)];
    }

    private static string GetUniquePath(string directory, string fileName)
    {
        var path = Path.Combine(directory, fileName);
        if (!File.Exists(path) && !File.Exists(path + ".vsp-part"))
        {
            return path;
        }
        var stem = Path.GetFileNameWithoutExtension(fileName);
        var extension = Path.GetExtension(fileName);
        for (var index = 1; index < 10_000; index++)
        {
            path = Path.Combine(directory, $"{stem} ({index}){extension}");
            if (!File.Exists(path) && !File.Exists(path + ".vsp-part"))
            {
                return path;
            }
        }
        throw new IOException("无法生成唯一文件名");
    }

    private void ProcessFeatureFrame(DeviceSession session, byte[] body)
    {
        switch (body[0])
        {
            case 30:
                ProcessInputPointerFrame(session, body);
                break;
            case 31:
                ProcessInputButtonFrame(session, body);
                break;
            case 32:
                ProcessInputScrollFrame(session, body);
                break;
            case 33:
                ProcessShortcutFrame(session, body);
                break;
            case 40:
                ProcessFileOfferFrame(session, body);
                break;
            case 43:
                ProcessFileCompleteFrame(session, body);
                break;
            case 44:
                ProcessFileCancelFrame(session, body);
                break;
            case 51:
                ProcessScanResultFrame(session, body);
                break;
            case 61:
                ProcessCaptureResultFrame(session, body);
                break;
        }
    }

    private void ProcessInputPointerFrame(DeviceSession session, byte[] body)
    {
        if (!SupportsCapability(session, "input.touchpad") || body.Length != 26)
        {
            return;
        }
        var sequence = BinaryPrimitives.ReadUInt32BigEndian(body.AsSpan(1, 4));
        if (!AcceptInputSequence(session, sequence))
        {
            return;
        }
        try
        {
            InputPointerReceived?.Invoke(
                this,
                new PhoneInputPointerEventArgs(
                    session.Id,
                    session.Name,
                    sequence,
                    BinaryPrimitives.ReadUInt64BigEndian(body.AsSpan(5, 8)),
                    body[13],
                    BinaryPrimitives.ReadUInt32BigEndian(body.AsSpan(14, 4)),
                    BinaryPrimitives.ReadInt32BigEndian(body.AsSpan(18, 4)),
                    BinaryPrimitives.ReadInt32BigEndian(body.AsSpan(22, 4))));
        }
        catch (Exception exception)
        {
            // 输入桥接失败不能关闭手机的功能通道；下一帧仍可继续处理。
            ReportFeatureInputFailure("处理手机触摸输入失败", exception);
        }
    }

    private void ProcessInputButtonFrame(DeviceSession session, byte[] body)
    {
        if (!SupportsCapability(session, "input.touchpad") || body.Length != 15)
        {
            return;
        }
        var sequence = BinaryPrimitives.ReadUInt32BigEndian(body.AsSpan(1, 4));
        if (!AcceptInputSequence(session, sequence))
        {
            return;
        }
        try
        {
            InputButtonReceived?.Invoke(
                this,
                new PhoneInputButtonEventArgs(
                    session.Id,
                    session.Name,
                    sequence,
                    BinaryPrimitives.ReadUInt64BigEndian(body.AsSpan(5, 8)),
                    body[13],
                    body[14]));
        }
        catch (Exception exception)
        {
            ReportFeatureInputFailure("处理手机鼠标按键失败", exception);
        }
    }

    private void ProcessInputScrollFrame(DeviceSession session, byte[] body)
    {
        if (!SupportsCapability(session, "input.touchpad") || body.Length != 21)
        {
            return;
        }
        var sequence = BinaryPrimitives.ReadUInt32BigEndian(body.AsSpan(1, 4));
        if (!AcceptInputSequence(session, sequence))
        {
            return;
        }
        try
        {
            InputScrollReceived?.Invoke(
                this,
                new PhoneInputScrollEventArgs(
                    session.Id,
                    session.Name,
                    sequence,
                    BinaryPrimitives.ReadUInt64BigEndian(body.AsSpan(5, 8)),
                    BinaryPrimitives.ReadInt32BigEndian(body.AsSpan(13, 4)),
                    BinaryPrimitives.ReadInt32BigEndian(body.AsSpan(17, 4))));
        }
        catch (Exception exception)
        {
            ReportFeatureInputFailure("处理手机滚动输入失败", exception);
        }
    }

    private void ReportFeatureInputFailure(string message, Exception exception)
    {
        try
        {
            StatusChanged?.Invoke(this, $"{message}：{exception.Message}");
        }
        catch
        {
            // 状态通知是辅助信息，不能反过来中断控制通道。
        }
    }

    private void ProcessShortcutFrame(DeviceSession session, byte[] body)
    {
        if (!SupportsCapability(session, "input.shortcut") || body.Length <= 1 || body.Length > 8193)
        {
            return;
        }
        ShortcutReceived?.Invoke(
            this,
            new PhoneShortcutEventArgs(
                session.Id,
                session.Name,
                Encoding.UTF8.GetString(body, 1, body.Length - 1)));
    }

    private void ProcessFileOfferFrame(DeviceSession session, byte[] body)
    {
        if (!SupportsCapability(session, "file.send") || body.Length <= 1)
        {
            return;
        }
        var payload = Encoding.UTF8.GetString(body, 1, body.Length - 1);
        if (!TryRegisterFileOffer(session, payload, out _))
        {
            return;
        }
        FileOfferReceived?.Invoke(this, new PhoneFileOfferEventArgs(session.Id, session.Name, payload));
    }

    private void ProcessFileCompleteFrame(DeviceSession session, byte[] body)
    {
        if (body.Length <= 1)
        {
            return;
        }
        _ = FinalizeFileTransferAsync(
            session,
            Encoding.UTF8.GetString(body, 1, body.Length - 1));
    }

    private void ProcessFileCancelFrame(DeviceSession session, byte[] body)
    {
        if (body.Length <= 1)
        {
            return;
        }
        CancelFileTransfer(session.Id, Encoding.UTF8.GetString(body, 1, body.Length - 1));
    }

    private void ProcessScanResultFrame(DeviceSession session, byte[] body)
    {
        if (!SupportsCapability(session, "scan.camera") && !SupportsCapability(session, "scan.gallery"))
        {
            return;
        }
        if (body.Length <= 1)
        {
            return;
        }
        ScanResultReceived?.Invoke(
            this,
            new PhoneScanResultEventArgs(
                session.Id,
                session.Name,
                Encoding.UTF8.GetString(body, 1, body.Length - 1)));
    }

    private void ProcessCaptureResultFrame(DeviceSession session, byte[] body)
    {
        if (!SupportsCapability(session, "camera.capture") || body.Length <= 1)
        {
            return;
        }
        CaptureResultReceived?.Invoke(
            this,
            new PhoneCaptureResultEventArgs(
                session.Id,
                session.Name,
                Encoding.UTF8.GetString(body, 1, body.Length - 1)));
    }

    private static bool SupportsCapability(DeviceSession session, string capability) =>
        session.Protocol >= 3 && session.Capabilities.Contains(capability);

    private static bool IsFeatureCapability(string capability) =>
        capability.StartsWith("input.", StringComparison.OrdinalIgnoreCase)
        || capability.StartsWith("file.", StringComparison.OrdinalIgnoreCase)
        || capability.StartsWith("scan.", StringComparison.OrdinalIgnoreCase)
        || capability.StartsWith("camera.", StringComparison.OrdinalIgnoreCase);

    private static bool AcceptInputSequence(DeviceSession session, uint sequence)
    {
        lock (session.InputGate)
        {
            if (session.HasInputSequence && sequence <= session.LastInputSequence)
            {
                return false;
            }
            session.LastInputSequence = sequence;
            session.HasInputSequence = true;
            return true;
        }
    }

    private Task ApplyPhoneMicrophoneStopAsync(DeviceSession session)
    {
        try
        {
            // 先同步收敛 Windows 本地路由，再等待选择信号量发送确认帧；
            // 这样 UI 或另一条音频任务阻塞时，手机停止也不会继续显示传输中。
            var changed = false;
            lock (_sessionsLock)
            {
                if (IsCurrentSessionLocked(session))
                {
                    if (IsActiveMicrophoneSessionLocked(session))
                    {
                        _activeMicrophoneDeviceId = null;
                        Interlocked.Increment(ref _microphoneSelectionRevision);
                    }
                    session.MicrophoneReportedActive = false;
                    changed = DeactivateMicrophoneSessionLocked(session);
                }
            }
            if (changed)
            {
                RaiseMicrophoneStateChanged();
            }
            // 手机已经在本地停止采集并回报了类型 2 状态；这里不要再回发一条
            // 重复的停止命令，否则它会滞留在 TCP 队列，覆盖手机随后发起的启动请求。
        }
        catch (Exception exception) when (exception is IOException
                                          or SocketException
                                          or ObjectDisposedException
                                          or OperationCanceledException)
        {
            StatusChanged?.Invoke(this, $"{session.Name} 手机停止请求收尾失败：{exception.Message}");
        }

        return Task.CompletedTask;
    }

    private void ProcessPcmFrame(DeviceSession session, byte[] body)
    {
        int gainPercent;
        long selectionRevision;
        lock (_sessionsLock)
        {
            if (!IsActiveMicrophoneSessionLocked(session) || !session.MicrophoneStreaming)
            {
                return;
            }
            gainPercent = session.MicrophoneGainPercent;
            selectionRevision = Interlocked.Read(ref _microphoneSelectionRevision);
        }

        var sampleRate = BinaryPrimitives.ReadUInt32BigEndian(body.AsSpan(9, 4));
        var pcmBytes = body.Length - 13;
        if (sampleRate != RemoteSampleRate
            || pcmBytes <= 0
            || pcmBytes > MaximumMicrophoneFrameBytes
            || pcmBytes % 2 != 0)
        {
            return;
        }

        var samples = new short[pcmBytes / 2];
        var gain = gainPercent / 100.0;
        double energy = 0;
        for (var index = 0; index < samples.Length; index++)
        {
            var sample = BinaryPrimitives.ReadInt16LittleEndian(body.AsSpan(13 + index * 2, 2));
            samples[index] = (short)Math.Clamp(
                Math.Round(sample * gain),
                short.MinValue,
                short.MaxValue);
            var value = sample / 32768.0;
            energy += value * value;
        }
        lock (_sessionsLock)
        {
            if (selectionRevision != Interlocked.Read(ref _microphoneSelectionRevision)
                || !IsActiveMicrophoneSessionLocked(session)
                || !session.MicrophoneStreaming)
            {
                return;
            }
            session.MicrophoneLevelDbfs = energy > 0
                ? 20 * Math.Log10(Math.Sqrt(energy / samples.Length))
                : -120;
            // 单路音频直接进入原生自适应环形缓冲，避免托管定时混音器再次拆包造成欠载。
            _audioEngine.PushRemoteMicrophoneOutputPcm16(RemoteSampleRate, samples);
        }

        var now = Stopwatch.GetTimestamp();
        var minimumInterval = Stopwatch.Frequency / 10;
        if (now - Interlocked.Read(ref session.LastLevelNotificationTick) >= minimumInterval)
        {
            Interlocked.Exchange(ref session.LastLevelNotificationTick, now);
            RaiseDevicesChanged();
            RaiseAggregateMicrophoneLevel();
        }
    }

    private void ProcessClockFrame(DeviceSession session, byte[] body)
    {
        var frameIndex = BinaryPrimitives.ReadUInt64BigEndian(body.AsSpan(1, 8));
        var monotonicNanoseconds = BinaryPrimitives.ReadUInt64BigEndian(body.AsSpan(9, 8));
        lock (_sessionsLock)
        {
            if (!IsActiveMicrophoneSessionLocked(session) || !session.MicrophoneStreaming)
            {
                return;
            }
            session.ClockEstimator.Add(frameIndex, monotonicNanoseconds);
            session.ClockDriftPpm = session.ClockEstimator.DriftPpm;
        }
    }

    private void UpdateMicrophoneStreaming(DeviceSession session, bool enabled, long stateId)
    {
        bool changed;
        bool acceptedEnabled;
        bool routeRequest;
        lock (_sessionsLock)
        {
            if (!IsCurrentSessionLocked(session))
            {
                return;
            }
            var wasActiveSession = IsActiveMicrophoneSessionLocked(session);
            var wasReportedActive = session.MicrophoneReportedActive;
            var commandPending = session.MicrophoneCommandPending;
            if (session.SupportsMicrophoneSyncRevision)
            {
                // 新协议的状态回报必须带命令序号。迟到的旧回包不能覆盖新状态，
                // 也不能清掉正在等待确认的命令。
                if (stateId == 0
                    ? commandPending
                    : stateId < session.LastMicrophoneStateId
                      || (commandPending && stateId < session.MicrophoneCommandExpectedId))
                {
                    return;
                }
                if (stateId > 0)
                {
                    session.LastMicrophoneStateId = stateId;
                }
            }
            var commandAcknowledged = commandPending
                                      && session.MicrophoneCommandExpectedEnabled == enabled
                                      && (!session.SupportsMicrophoneSyncRevision
                                          || stateId == session.MicrophoneCommandExpectedId);
            // 类型 2 是实际采集状态。为了兼容旧版 Android（没有类型 7 请求帧），
            // 状态发生变化且并非 Windows 命令确认时，补发一个路由请求事件。
            routeRequest = !session.SupportsMicrophoneRequests
                           && !commandAcknowledged
                           && enabled != wasReportedActive
                           && (enabled || wasActiveSession);
            if (commandAcknowledged
                || !session.SupportsMicrophoneSyncRevision
                || (stateId > 0 && stateId >= session.MicrophoneCommandExpectedId))
            {
                session.MicrophoneCommandPending = false;
                session.MicrophoneCommandExpectedId = 0;
            }
            session.MicrophoneReportedActive = enabled;
            acceptedEnabled = enabled && IsActiveMicrophoneSessionLocked(session);
            changed = session.MicrophoneStreaming != acceptedEnabled;
            session.MicrophoneStreaming = acceptedEnabled;
            if (!acceptedEnabled)
            {
                session.MicrophoneLevelDbfs = -120;
            }
            if (!enabled && wasActiveSession)
            {
                _activeMicrophoneDeviceId = null;
                Interlocked.Increment(ref _microphoneSelectionRevision);
            }
        }
        if (routeRequest)
        {
            var handler = MicrophoneRouteRequested;
            if (handler is not null)
            {
                StatusChanged?.Invoke(
                    this,
                    $"{session.Name} 通过状态回报请求{(enabled ? "启用" : "停止")}手机麦克风路由。");
                handler.Invoke(
                    this,
                    new PhoneMicrophoneRequestEventArgs(session.Id, session.Name, enabled));
            }
            else if (enabled)
            {
                lock (_sessionsLock)
                {
                    if (IsCurrentSessionLocked(session)
                        && !IsActiveMicrophoneSessionLocked(session))
                    {
                        session.MicrophoneReportedActive = false;
                    }
                }
                _ = SendMicrophoneCommandAsync(session, false);
            }
        }
        if (!changed)
        {
            return;
        }

        RaiseDevicesChanged();
        RaiseAggregateMicrophoneState();
        RaiseAggregateMicrophoneLevel();
        StatusChanged?.Invoke(
            this,
            acceptedEnabled
                ? $"{session.Name} 麦克风已启用并开始回传。"
                : $"{session.Name} 麦克风已停止并释放。");
    }

    private void UpdatePlaybackStreaming(DeviceSession session, bool enabled)
    {
        bool changed;
        lock (_sessionsLock)
        {
            if (!IsCurrentSessionLocked(session))
            {
                return;
            }
            changed = session.PlaybackStreaming != enabled;
            session.PlaybackStreaming = enabled;
        }
        if (!changed)
        {
            return;
        }

        RaiseDevicesChanged();
        RaiseAggregatePlaybackState();
        StatusChanged?.Invoke(
            this,
            enabled
                ? $"{session.Name} 已准备播放 Windows 声音。"
                : $"{session.Name} 已停止播放 Windows 声音。");
    }

    private void SystemAudioSource_FrameReady(object? sender, SystemAudioFrameEventArgs args)
    {
        var body = CreatePlaybackFrame(args);
        if (body is null)
        {
            StatusChanged?.Invoke(this, "Windows 音频包过大，已丢弃该包。");
            return;
        }

        DeviceSession[] targets;
        lock (_sessionsLock)
        {
            targets = _sessions.Values
                .Where(session => session.PlaybackRequested && session.Protocol >= 2)
                .ToArray();
        }
        foreach (var session in targets)
        {
            session.PlaybackFrames.Writer.TryWrite(body);
        }
    }

    private async Task PlaybackSendLoopAsync(DeviceSession session)
    {
        try
        {
            await foreach (var body in session.PlaybackFrames.Reader.ReadAllAsync(
                               session.Cancellation.Token))
            {
                if (!session.PlaybackRequested)
                {
                    continue;
                }
                await SendBinaryFrameAsync(body, session, session.Cancellation.Token);
            }
        }
        catch (OperationCanceledException)
        {
            // 设备断开时停止独立发送循环属于正常生命周期。
        }
        catch (Exception exception) when (exception is IOException or SocketException or ObjectDisposedException)
        {
            lock (_sessionsLock)
            {
                session.PlaybackRequested = false;
                session.PlaybackStreaming = false;
            }
            StatusChanged?.Invoke(this, $"向 {session.Name} 发送 Windows 声音失败：{exception.Message}");
            RaiseDevicesChanged();
            RaiseAggregatePlaybackState();
        }
    }

    private static byte[]? CreatePlaybackFrame(SystemAudioFrameEventArgs frame)
    {
        if (frame.Samples.Length > (MaximumFrameBytes - 14) / sizeof(short))
        {
            return null;
        }

        var body = new byte[14 + frame.Samples.Length * sizeof(short)];
        body[0] = 12;
        BinaryPrimitives.WriteUInt64BigEndian(body.AsSpan(1, 8), frame.FirstFrameIndex);
        BinaryPrimitives.WriteUInt32BigEndian(body.AsSpan(9, 4), frame.SampleRate);
        body[13] = checked((byte)frame.Channels);
        for (var index = 0; index < frame.Samples.Length; index++)
        {
            BinaryPrimitives.WriteInt16LittleEndian(
                body.AsSpan(14 + index * sizeof(short), sizeof(short)),
                frame.Samples[index]);
        }
        return body;
    }

    private async Task DiscoveryLoopAsync(CancellationToken cancellationToken)
    {
        if (_discovery is null)
        {
            return;
        }

        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                var result = await _discovery.ReceiveAsync(cancellationToken);
                var request = Encoding.ASCII.GetString(result.Buffer).Trim();
                var codeDiscovery = request == $"VSP_DISCOVER {_pairingCode}";
                var sessionLocation = request == $"VSP_LOCATE {_sessionId}";
                if (!codeDiscovery && !sessionLocation)
                {
                    continue;
                }

                var response = codeDiscovery
                    ? new
                    {
                        protocol = 1,
                        host = LocalAddress,
                        port = ServerPort,
                        session = _sessionId,
                        secret = _sessionSecret,
                    }
                    : (object)new
                    {
                        protocol = 1,
                        host = LocalAddress,
                        port = ServerPort,
                        session = _sessionId,
                    };
                var bytes = JsonSerializer.SerializeToUtf8Bytes(response, JsonOptions);
                await _discovery.SendAsync(bytes, result.RemoteEndPoint, cancellationToken);
            }
        }
        catch (OperationCanceledException)
        {
            // 应用退出时取消发现服务属于正常生命周期。
        }
        catch (SocketException exception)
        {
            StatusChanged?.Invoke(this, $"移动设备发现服务已停止：{exception.Message}");
        }
    }

    /// <summary>
    /// 电脑重启或 TCP 断线后，通过局域网广播唤醒已保存的 Android 设备。
    /// 广播只携带稳定会话 ID 和当前 TCP 端口，不携带配对密钥；手机仍会用已保存密钥完成 TCP 握手。
    /// </summary>
    private async Task AnnounceReconnectAsync(CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(_sessionId) || ServerPort is <= 0 or > 65535)
        {
            return;
        }

        var payload = Encoding.ASCII.GetBytes($"VSP_RECONNECT {_sessionId} {ServerPort}");
        try
        {
            for (var attempt = 0; attempt < ReconnectAnnouncementCount; attempt++)
            {
                using var sender = new UdpClient(AddressFamily.InterNetwork)
                {
                    EnableBroadcast = true,
                };
                foreach (var destination in GetBroadcastAddresses())
                {
                        await sender.SendAsync(
                            payload,
                            new IPEndPoint(destination, ReconnectPort),
                            cancellationToken)
                        .ConfigureAwait(false);
                }

                if (attempt + 1 < ReconnectAnnouncementCount)
                {
                    await Task.Delay(
                            ReconnectAnnouncementIntervalMilliseconds,
                            cancellationToken)
                        .ConfigureAwait(false);
                }
            }
        }
        catch (OperationCanceledException)
        {
            // 应用退出时取消广播属于正常生命周期。
        }
        catch (SocketException exception)
        {
            StatusChanged?.Invoke(this, $"自动重连广播不可用：{exception.Message}");
        }
    }

    private static IPAddress[] GetBroadcastAddresses()
    {
        var destinations = new HashSet<IPAddress> { IPAddress.Broadcast };
        foreach (var network in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (network.OperationalStatus != OperationalStatus.Up
                || network.NetworkInterfaceType == NetworkInterfaceType.Loopback)
            {
                continue;
            }

            foreach (var address in network.GetIPProperties().UnicastAddresses)
            {
                if (address.Address.AddressFamily == AddressFamily.InterNetwork
                    && address.IPv4Mask is not null)
                {
                    var ip = address.Address.GetAddressBytes();
                    var mask = address.IPv4Mask.GetAddressBytes();
                    var broadcast = new byte[4];
                    for (var index = 0; index < broadcast.Length; index++)
                    {
                        broadcast[index] = (byte)(ip[index] | ~mask[index]);
                    }
                    destinations.Add(new IPAddress(broadcast));
                }
            }
        }

        return destinations.ToArray();
    }

    private DeviceSession? GetFirstSession(bool requirePlayback = false)
    {
        lock (_sessionsLock)
        {
            return _sessions.Values
                .Where(session => !requirePlayback || session.Protocol >= 2)
                .OrderBy(session => session.ConnectedAt)
                .FirstOrDefault();
        }
    }

    private DeviceSession? GetSession(string deviceId, bool includeFeatureSession = false)
    {
        lock (_sessionsLock)
        {
            if (includeFeatureSession
                && _featureSessions.TryGetValue(deviceId, out var featureSession))
            {
                return featureSession;
            }
            if (_sessions.TryGetValue(deviceId, out var session))
            {
                return session;
            }
            return null;
        }
    }

    private bool IsCurrentSessionLocked(DeviceSession session) =>
        _sessions.TryGetValue(session.Id, out var current) && ReferenceEquals(current, session);

    private void RaiseDevicesChanged()
    {
        IReadOnlyList<PhoneDeviceSnapshot> snapshots;
        lock (_sessionsLock)
        {
            snapshots = CreateSnapshotsLocked();
        }
        DevicesChanged?.Invoke(this, new PhoneDevicesChangedEventArgs(snapshots));
    }

    private PhoneDeviceSnapshot[] CreateSnapshotsLocked() =>
        _sessions.Values
            .OrderBy(session => session.ConnectedAt)
            .Select(session => new PhoneDeviceSnapshot(
                session.Id,
                session.Name,
                session.RemoteAddress,
                session.Protocol,
                session.MicrophoneStreaming,
                session.PlaybackRequested,
                session.PlaybackStreaming,
                session.MicrophoneGainPercent,
                session.MicrophoneLevelDbfs,
                session.ClockDriftPpm,
                session.MicrophoneStreaming ? MicrophoneOutputBufferMilliseconds : 0,
                session.ConnectedAt,
                session.Capabilities))
            .ToArray();

    private void RaiseAggregateMicrophoneState() =>
        MicrophoneStreamingChanged?.Invoke(this, IsMicrophoneStreaming);

    private void RaiseAggregatePlaybackState() =>
        PlaybackStreamingChanged?.Invoke(this, IsPlaybackStreaming);

    private void RaiseAggregateMicrophoneLevel()
    {
        double level;
        lock (_sessionsLock)
        {
            level = _sessions.Values
                .Where(session => session.MicrophoneStreaming)
                .Select(session => session.MicrophoneLevelDbfs)
                .DefaultIfEmpty(-120)
                .Max();
        }
        MicrophoneLevelChanged?.Invoke(this, level);
    }

    private DeviceSession? GetActiveMicrophoneSession()
    {
        lock (_sessionsLock)
        {
            return _activeMicrophoneDeviceId is not null
                   && _sessions.TryGetValue(_activeMicrophoneDeviceId, out var session)
                ? session
                : null;
        }
    }

    private bool IsActiveMicrophoneSessionLocked(DeviceSession session) =>
        IsCurrentSessionLocked(session)
        && string.Equals(
            _activeMicrophoneDeviceId,
            session.Id,
            StringComparison.OrdinalIgnoreCase);

    private static bool DeactivateMicrophoneSessionLocked(DeviceSession session)
    {
        var changed = session.MicrophoneStreaming;
        session.MicrophoneStreaming = false;
        session.MicrophoneLevelDbfs = -120;
        return changed;
    }

    private void RaiseMicrophoneStateChanged()
    {
        RaiseDevicesChanged();
        RaiseAggregateMicrophoneState();
        RaiseAggregateMicrophoneLevel();
    }

    private static string NormalizeDeviceId(string value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return string.Empty;
        }
        return new string(value
            .Trim()
            .Take(128)
            .Where(character => char.IsLetterOrDigit(character) || character is '-' or '_' or '.')
            .ToArray());
    }

    private static async Task SendJsonLineAsync(
        object value,
        DeviceSession session,
        CancellationToken cancellationToken)
    {
        var payload = JsonSerializer.SerializeToUtf8Bytes(value, JsonOptions);
        await session.SendGate.WaitAsync(cancellationToken);
        try
        {
            await session.Stream.WriteAsync(payload, cancellationToken);
            await session.Stream.WriteAsync("\n"u8.ToArray(), cancellationToken);
        }
        finally
        {
            session.SendGate.Release();
        }
    }

    private static async Task SendUncoordinatedJsonLineAsync(
        object value,
        NetworkStream stream,
        CancellationToken cancellationToken)
    {
        var payload = JsonSerializer.SerializeToUtf8Bytes(value, JsonOptions);
        await stream.WriteAsync(payload, cancellationToken);
        await stream.WriteAsync("\n"u8.ToArray(), cancellationToken);
    }

    private static async Task SendBinaryFrameAsync(
        ReadOnlyMemory<byte> body,
        DeviceSession session,
        CancellationToken cancellationToken)
    {
        if (body.Length is < 1 or > MaximumFrameBytes)
        {
            throw new InvalidDataException("发送给移动设备的数据帧长度无效。");
        }

        var length = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(length, checked((uint)body.Length));
        await session.SendGate.WaitAsync(cancellationToken);
        try
        {
            await session.Stream.WriteAsync(length, cancellationToken);
            await session.Stream.WriteAsync(body, cancellationToken);
        }
        finally
        {
            session.SendGate.Release();
        }
    }

    private static void NotifyRemoteDisconnect(DeviceSession session)
    {
        try
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromMilliseconds(300));
            SendBinaryFrameAsync(new byte[] { 13 }, session, timeout.Token)
                .GetAwaiter()
                .GetResult();
        }
        catch (Exception exception) when (exception is IOException
                                          or SocketException
                                          or ObjectDisposedException
                                          or OperationCanceledException)
        {
            // 连接已经异常时无需重复报告，Stop() 会完成本地清理。
        }
    }

    private static async Task<string> ReadLineAsync(
        NetworkStream stream,
        int maximumBytes,
        CancellationToken cancellationToken)
    {
        using var buffer = new MemoryStream();
        var singleByte = new byte[1];
        while (buffer.Length < maximumBytes)
        {
            var read = await stream.ReadAsync(singleByte, cancellationToken);
            if (read == 0)
            {
                throw new IOException("移动设备在握手完成前断开连接。");
            }
            if (singleByte[0] == (byte)'\n')
            {
                return Encoding.UTF8.GetString(buffer.GetBuffer(), 0, (int)buffer.Length);
            }
            buffer.WriteByte(singleByte[0]);
        }
        throw new InvalidDataException("移动设备握手数据过长。");
    }

    private static async Task<bool> TryReadExactlyAsync(
        NetworkStream stream,
        Memory<byte> buffer,
        CancellationToken cancellationToken)
    {
        var offset = 0;
        while (offset < buffer.Length)
        {
            var read = await stream.ReadAsync(buffer[offset..], cancellationToken);
            if (read == 0)
            {
                return false;
            }
            offset += read;
        }
        return true;
    }

    private void GenerateCredentials()
    {
        _sessionId = Convert.ToHexString(RandomNumberGenerator.GetBytes(16));
        _sessionSecret = Convert.ToHexString(RandomNumberGenerator.GetBytes(16));
        _pairingCode = RandomNumberGenerator.GetInt32(1_000_000)
            .ToString("D6", CultureInfo.InvariantCulture);
    }

    private static bool IsCredential(string? value) =>
        value is { Length: 32 } && value.All(Uri.IsHexDigit);

    private static string[] GetLocalIpv4Addresses()
    {
        var candidates = new List<(string Address, int Score)>();
        foreach (var network in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (network.OperationalStatus != OperationalStatus.Up
                || network.NetworkInterfaceType == NetworkInterfaceType.Loopback)
            {
                continue;
            }

            var score = network.NetworkInterfaceType switch
            {
                NetworkInterfaceType.Ethernet => 60,
                NetworkInterfaceType.Wireless80211 => 50,
                _ => 1,
            };
            var adapterText = $"{network.Name} {network.Description}".ToLowerInvariant();
            if (VirtualAdapterMarkers.Any(adapterText.Contains))
            {
                score -= 200;
            }

            foreach (var address in network.GetIPProperties().UnicastAddresses
                         .Where(entry => entry.Address.AddressFamily == AddressFamily.InterNetwork)
                         .Select(entry => entry.Address))
            {
                var text = address.ToString();
                if (text.StartsWith("169.254.", StringComparison.Ordinal))
                {
                    continue;
                }
                var addressScore = score + (text.StartsWith("192.168.", StringComparison.Ordinal) ? 30
                    : text.StartsWith("10.", StringComparison.Ordinal) ? 20
                    : IsPrivate172(address) ? 10 : 0);
                candidates.Add((text, addressScore));
            }
        }

        return candidates
            .OrderByDescending(candidate => candidate.Score)
            .ThenBy(candidate => candidate.Address, StringComparer.Ordinal)
            .Select(candidate => candidate.Address)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    private static bool IsPrivate172(IPAddress address)
    {
        var bytes = address.GetAddressBytes();
        return bytes[0] == 172 && bytes[1] is >= 16 and <= 31;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _lifetime.Cancel();
        if (_systemAudioSource is not null)
        {
            _systemAudioSource.SystemAudioFrameReady -= SystemAudioSource_FrameReady;
        }
        DisconnectPhone(notifyRemote: false);
        _listener?.Stop();
        _listener = null;
        _discovery?.Dispose();
        _discovery = null;
        _microphoneSelectionGate.Dispose();
        _lifetime.Dispose();
    }

    private sealed class DeviceSession(
        string id,
        string name,
        string remoteAddress,
        int protocol,
        bool supportsMicrophoneRequests,
        bool supportsMicrophoneSyncRevision,
        HashSet<string> capabilities,
        bool featureOnly,
        TcpClient client,
        CancellationTokenSource cancellation)
    {
        private int _stopped;
        private int _autoReconnect = 1;

        public string Id { get; } = id;

        public string Name { get; } = name;

        public string RemoteAddress { get; } = remoteAddress;

        public int Protocol { get; } = protocol;

        public bool SupportsMicrophoneRequests { get; } = supportsMicrophoneRequests;

        public bool SupportsMicrophoneSyncRevision { get; } = supportsMicrophoneSyncRevision;

        public HashSet<string> Capabilities { get; } = capabilities;

        public bool FeatureOnly { get; } = featureOnly;

        public object InputGate { get; } = new();

        public bool HasInputSequence { get; set; }

        public uint LastInputSequence { get; set; }

        public TcpClient Client { get; } = client;

        public NetworkStream Stream { get; } = client.GetStream();

        public CancellationTokenSource Cancellation { get; } = cancellation;

        public SemaphoreSlim SendGate { get; } = new(1, 1);

        public SemaphoreSlim MicrophoneCommandGate { get; } = new(1, 1);

        public SemaphoreSlim PlaybackCommandGate { get; } = new(1, 1);

        public long MicrophoneCommandRevision;

        public bool MicrophoneCommandPending { get; set; }

        public bool MicrophoneCommandExpectedEnabled { get; set; }

        public long MicrophoneCommandExpectedId { get; set; }

        public long LastMicrophoneStateId { get; set; }

        public long LastMicrophoneRequestId { get; set; }

        public long PlaybackCommandRevision;

        public Channel<byte[]> PlaybackFrames { get; } =
            Channel.CreateBounded<byte[]>(new BoundedChannelOptions(12)
            {
                FullMode = BoundedChannelFullMode.DropOldest,
                SingleReader = true,
                SingleWriter = false,
            });

        public RemoteClockEstimator ClockEstimator { get; } = new();

        public DateTimeOffset ConnectedAt { get; } = DateTimeOffset.UtcNow;

        public DateTimeOffset LastSeen { get; set; } = DateTimeOffset.UtcNow;

        public bool AutoReconnect => Volatile.Read(ref _autoReconnect) != 0;

        public bool RemoteDisconnectRequested { get; set; }

        public bool MicrophoneStreaming { get; set; }

        public bool MicrophoneReportedActive { get; set; }

        public volatile bool PlaybackRequested;

        public bool PlaybackStreaming { get; set; }

        public int MicrophoneGainPercent { get; set; } = 100;

        public double MicrophoneLevelDbfs { get; set; } = -120;

        public double ClockDriftPpm { get; set; }

        public long LastLevelNotificationTick;

        public Task? PlaybackSendTask { get; set; }

        public void DisableAutoReconnect() => Interlocked.Exchange(ref _autoReconnect, 0);

        public void Stop()
        {
            if (Interlocked.Exchange(ref _stopped, 1) != 0)
            {
                return;
            }
            PlaybackFrames.Writer.TryComplete();
            Cancellation.Cancel();
            Client.Dispose();
        }
    }

    private sealed class FileTransferState(string transferId, string deviceId, string deviceName)
    {
        public string TransferId { get; } = transferId;

        public string DeviceId { get; } = deviceId;

        public string DeviceName { get; } = deviceName;

        public Dictionary<string, FileItemTransfer> Items { get; } =
            new(StringComparer.OrdinalIgnoreCase);
    }

    private sealed class FileItemTransfer(
        string itemId,
        string name,
        long size,
        string finalPath,
        string partPath)
    {
        public string ItemId { get; } = itemId;

        public string Name { get; } = name;

        public long Size { get; } = size;

        public string FinalPath { get; } = finalPath;

        public string PartPath { get; } = partPath;

        public long ReceivedBytes { get; set; } = File.Exists(partPath) ? new FileInfo(partPath).Length : 0;

        public FileStream? Stream { get; set; }
    }

    private sealed class RemoteClockEstimator
    {
        private ulong _previousFrame;
        private ulong _previousNanoseconds;
        private double _driftPpm;

        public double DriftPpm => _driftPpm;

        public void Reset()
        {
            _previousFrame = 0;
            _previousNanoseconds = 0;
            _driftPpm = 0;
        }

        public void Add(ulong frame, ulong nanoseconds)
        {
            if (_previousNanoseconds != 0
                && frame > _previousFrame
                && nanoseconds > _previousNanoseconds)
            {
                var seconds = (nanoseconds - _previousNanoseconds) / 1_000_000_000.0;
                var measuredRate = (frame - _previousFrame) / seconds;
                var measuredDrift = (measuredRate / RemoteSampleRate - 1.0) * 1_000_000.0;
                if (double.IsFinite(measuredDrift) && Math.Abs(measuredDrift) <= 10_000)
                {
                    _driftPpm = _driftPpm == 0
                        ? measuredDrift
                        : _driftPpm * 0.8 + measuredDrift * 0.2;
                }
            }
            _previousFrame = frame;
            _previousNanoseconds = nanoseconds;
        }
    }
}
