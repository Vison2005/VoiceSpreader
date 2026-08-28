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
    DateTimeOffset ConnectedAt)
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
    bool enabled) : EventArgs
{
    public string DeviceId { get; } = deviceId;

    public string DeviceName { get; } = deviceName;

    public bool Enabled { get; } = enabled;
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
    private const int MaximumFrameBytes = 256 * 1024;
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
    private readonly CancellationTokenSource _lifetime = new();
    private readonly SemaphoreSlim _microphoneSelectionGate = new(1, 1);
    private TcpListener? _listener;
    private UdpClient? _discovery;
    private string _sessionId = string.Empty;
    private string _sessionSecret = string.Empty;
    private string _pairingCode = string.Empty;
    private string? _activeMicrophoneDeviceId;
    private long _microphoneSelectionRevision;
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

    public IReadOnlyList<string> LocalAddresses { get; private set; }

    public string LocalAddress { get; private set; }

    public int ServerPort { get; private set; }

    public string PairingCode => _pairingCode;

    public string PairingPayload => $"VSP1:{LocalAddress}:{ServerPort}:{_sessionId}:{_sessionSecret}";

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
        await _microphoneSelectionGate.WaitAsync();
        try
        {
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
        await _microphoneSelectionGate.WaitAsync();
        try
        {
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
            return await SendMicrophoneCommandAsync(session, false);
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

    private async Task<bool> SendMicrophoneCommandAsync(DeviceSession session, bool enabled)
    {
        var revision = Interlocked.Increment(ref session.MicrophoneCommandRevision);
        try
        {
            await session.MicrophoneCommandGate.WaitAsync(session.Cancellation.Token);
            try
            {
                if (revision != Volatile.Read(ref session.MicrophoneCommandRevision))
                {
                    return true;
                }
                if (session.Protocol >= 2)
                {
                    await SendBinaryFrameAsync(
                        new byte[] { 10, enabled ? (byte)1 : (byte)0 },
                        session,
                        session.Cancellation.Token);
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
        GetSession(deviceId)?.Stop();
    }

    public void DisconnectPhone()
    {
        DeviceSession[] sessions;
        lock (_sessionsLock)
        {
            sessions = [.. _sessions.Values];
        }
        foreach (var session in sessions)
        {
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
                    out var protocol))
            {
                await SendUncoordinatedJsonLineAsync(
                    new { type = "error", message = "移动设备配对凭据不匹配" },
                    stream,
                    cancellationToken);
                return;
            }

            deviceSession = new DeviceSession(
                deviceId,
                deviceName,
                (client.Client.RemoteEndPoint as IPEndPoint)?.Address.ToString() ?? string.Empty,
                protocol,
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
                },
                deviceSession,
                cancellationToken);

            DeviceSession? replacedSession = null;
            lock (_sessionsLock)
            {
                if (_sessions.TryGetValue(deviceId, out var existing))
                {
                    replacedSession = existing;
                }
                _sessions[deviceId] = deviceSession;
            }
            replacedSession?.Stop();
            deviceSession.PlaybackSendTask = PlaybackSendLoopAsync(deviceSession);
            ConnectionChanged?.Invoke(
                this,
                new PhoneConnectionChangedEventArgs(true, deviceName, deviceId));
            StatusChanged?.Invoke(this, $"移动设备已连接：{deviceName}；音频链路保持关闭。");
            RaiseDevicesChanged();

            var lengthBytes = new byte[4];
            while (!deviceSession.Cancellation.IsCancellationRequested)
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
                    if (_sessions.TryGetValue(deviceSession.Id, out var current)
                        && ReferenceEquals(current, deviceSession))
                    {
                        _sessions.Remove(deviceSession.Id);
                        if (string.Equals(
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
                    ConnectionChanged?.Invoke(
                        this,
                        new PhoneConnectionChangedEventArgs(
                            false,
                            deviceSession.Name,
                            deviceSession.Id));
                    StatusChanged?.Invoke(this, $"移动设备已断开：{deviceSession.Name}");
                    RaiseDevicesChanged();
                    RaiseAggregateMicrophoneState();
                    RaiseAggregatePlaybackState();
                }
            }
        }
    }

    private bool TryAuthenticate(
        string handshake,
        string remoteAddress,
        out string deviceId,
        out string deviceName,
        out int negotiatedProtocol)
    {
        deviceId = string.Empty;
        deviceName = string.Empty;
        negotiatedProtocol = 1;
        try
        {
            using var document = JsonDocument.Parse(handshake);
            var root = document.RootElement;
            var protocolValue = root.TryGetProperty("protocol", out var protocol)
                ? protocol.GetInt32()
                : 0;
            var valid = root.TryGetProperty("type", out var type)
                        && type.GetString() == "hello"
                        && protocolValue is 1 or 2
                        && root.TryGetProperty("session", out var session)
                        && string.Equals(session.GetString(), _sessionId, StringComparison.OrdinalIgnoreCase)
                        && root.TryGetProperty("secret", out var secret)
                        && string.Equals(secret.GetString(), _sessionSecret, StringComparison.OrdinalIgnoreCase);
            if (!valid)
            {
                return false;
            }

            negotiatedProtocol = protocolValue;
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
        switch (body[0])
        {
            case 1 when body.Length >= 13:
                ProcessPcmFrame(session, body);
                break;
            case 2 when body.Length >= 2:
                UpdateMicrophoneStreaming(session, body[1] != 0);
                break;
            case 3:
                var microphoneError = Encoding.UTF8.GetString(body, 1, body.Length - 1);
                StatusChanged?.Invoke(
                    this,
                    $"{session.Name} 麦克风：{microphoneError}");
                UpdateMicrophoneStreaming(session, false);
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
                if (!microphoneRouteEnabled)
                {
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
                        microphoneRouteEnabled));
                break;
        }
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

    private void UpdateMicrophoneStreaming(DeviceSession session, bool enabled)
    {
        bool changed;
        bool acceptedEnabled;
        bool requestRouteChange;
        lock (_sessionsLock)
        {
            if (!IsCurrentSessionLocked(session))
            {
                return;
            }
            var wasActiveSession = IsActiveMicrophoneSessionLocked(session);
            session.MicrophoneReportedActive = enabled;
            acceptedEnabled = enabled && IsActiveMicrophoneSessionLocked(session);
            requestRouteChange = enabled && !wasActiveSession
                                 || !enabled && wasActiveSession;
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
        if (requestRouteChange)
        {
            var handler = MicrophoneRouteRequested;
            if (handler is not null)
            {
                StatusChanged?.Invoke(
                    this,
                    $"{session.Name} 请求{(enabled ? "启用" : "停止")}手机麦克风路由。");
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

    private DeviceSession? GetSession(string deviceId)
    {
        lock (_sessionsLock)
        {
            return _sessions.TryGetValue(deviceId, out var session) ? session : null;
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
                session.ConnectedAt))
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
        DisconnectPhone();
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
        TcpClient client,
        CancellationTokenSource cancellation)
    {
        private int _stopped;

        public string Id { get; } = id;

        public string Name { get; } = name;

        public string RemoteAddress { get; } = remoteAddress;

        public int Protocol { get; } = protocol;

        public TcpClient Client { get; } = client;

        public NetworkStream Stream { get; } = client.GetStream();

        public CancellationTokenSource Cancellation { get; } = cancellation;

        public SemaphoreSlim SendGate { get; } = new(1, 1);

        public SemaphoreSlim MicrophoneCommandGate { get; } = new(1, 1);

        public SemaphoreSlim PlaybackCommandGate { get; } = new(1, 1);

        public long MicrophoneCommandRevision;

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

        public bool MicrophoneStreaming { get; set; }

        public bool MicrophoneReportedActive { get; set; }

        public volatile bool PlaybackRequested;

        public bool PlaybackStreaming { get; set; }

        public int MicrophoneGainPercent { get; set; } = 100;

        public double MicrophoneLevelDbfs { get; set; } = -120;

        public double ClockDriftPpm { get; set; }

        public long LastLevelNotificationTick;

        public Task? PlaybackSendTask { get; set; }

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
