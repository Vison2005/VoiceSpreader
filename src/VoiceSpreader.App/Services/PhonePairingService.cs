using System.Buffers.Binary;
using System.Globalization;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace VoiceSpreader.App.Services;

public sealed class PhoneConnectionChangedEventArgs(bool connected, string phoneName) : EventArgs
{
    public bool Connected { get; } = connected;

    public string PhoneName { get; } = phoneName;
}

public sealed class PhonePairingService : IDisposable
{
    private const int DiscoveryPort = 39741;
    private const int MaximumFrameBytes = 256 * 1024;
    private const uint RemoteSampleRate = 48_000;
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);
    private static readonly string[] VirtualAdapterMarkers =
        ["vmware", "virtual", "radmin", "zerotier", "vpn", "hyper-v", "vethernet", "wsl"];

    private readonly IRemoteMicrophoneSink _audioEngine;
    private readonly object _clientLock = new();
    private readonly SemaphoreSlim _sendGate = new(1, 1);
    private readonly CancellationTokenSource _lifetime = new();
    private TcpListener? _listener;
    private UdpClient? _discovery;
    private TcpClient? _client;
    private NetworkStream? _clientStream;
    private string _sessionId = string.Empty;
    private string _sessionSecret = string.Empty;
    private string _pairingCode = string.Empty;
    private string _phoneName = string.Empty;
    private bool _authenticated;
    private bool _microphoneStreaming;
    private bool _disposed;

    public PhonePairingService(IRemoteMicrophoneSink audioEngine)
    {
        _audioEngine = audioEngine;
        LocalAddresses = GetLocalIpv4Addresses();
        LocalAddress = LocalAddresses.Count > 0 ? LocalAddresses[0] : IPAddress.Loopback.ToString();
        GenerateCredentials();
    }

    public event EventHandler<string>? StatusChanged;

    public event EventHandler<PhoneConnectionChangedEventArgs>? ConnectionChanged;

    public event EventHandler<bool>? MicrophoneStreamingChanged;

    public event EventHandler<double>? MicrophoneLevelChanged;

    public IReadOnlyList<string> LocalAddresses { get; private set; }

    public string LocalAddress { get; private set; }

    public int ServerPort { get; private set; }

    public string PairingCode => _pairingCode;

    public string PairingPayload => $"VSP1:{LocalAddress}:{ServerPort}:{_sessionId}:{_sessionSecret}";

    public bool IsPhoneConnected => _authenticated && _client?.Connected == true;

    public bool IsMicrophoneStreaming => _microphoneStreaming;

    public string ConnectedPhoneName => _phoneName;

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

        StatusChanged?.Invoke(this, $"手机麦克风配对服务已启动：{LocalAddress}:{ServerPort}");
    }

    public bool SetLocalAddress(string address)
    {
        if (!LocalAddresses.Contains(address, StringComparer.OrdinalIgnoreCase))
        {
            return false;
        }

        LocalAddress = address;
        StatusChanged?.Invoke(this, $"手机配对地址已切换到 {address}");
        return true;
    }

    public async Task<bool> SetMicrophoneEnabledAsync(bool enabled)
    {
        if (!_authenticated || _clientStream is null)
        {
            StatusChanged?.Invoke(this, "手机尚未连接，无法切换麦克风。");
            return false;
        }

        try
        {
            await SendJsonLineAsync(
                new { type = "setMicrophone", enabled },
                _clientStream,
                _lifetime.Token);
            StatusChanged?.Invoke(
                this,
                enabled ? "已请求手机启用麦克风。" : "已请求手机停止并释放麦克风。");
            return true;
        }
        catch (Exception exception) when (exception is IOException or SocketException)
        {
            StatusChanged?.Invoke(this, $"向手机发送麦克风控制命令失败：{exception.Message}");
            return false;
        }
    }

    public void DisconnectPhone()
    {
        var previousName = _phoneName;
        var wasConnected = _authenticated || _client is not null;
        lock (_clientLock)
        {
            _client?.Dispose();
            _client = null;
            _clientStream = null;
        }
        ResetClientState();
        if (wasConnected)
        {
            ConnectionChanged?.Invoke(this, new PhoneConnectionChangedEventArgs(false, previousName));
        }
        if (!string.IsNullOrWhiteSpace(previousName))
        {
            StatusChanged?.Invoke(this, $"已断开手机：{previousName}");
        }
    }

    public void ResetPairing()
    {
        DisconnectPhone();
        GenerateCredentials();
        StatusChanged?.Invoke(this, "已生成新的手机配对凭据。");
    }

    private async Task AcceptLoopAsync(CancellationToken cancellationToken)
    {
        try
        {
            while (!cancellationToken.IsCancellationRequested && _listener is not null)
            {
                var incoming = await _listener.AcceptTcpClientAsync(cancellationToken);
                lock (_clientLock)
                {
                    if (_client is not null)
                    {
                        _ = RejectAdditionalClientAsync(incoming, cancellationToken);
                        continue;
                    }
                    _client = incoming;
                    _clientStream = incoming.GetStream();
                }
                _ = HandleClientAsync(incoming, cancellationToken);
            }
        }
        catch (OperationCanceledException)
        {
            // 应用退出时取消监听属于正常生命周期。
        }
        catch (SocketException exception)
        {
            StatusChanged?.Invoke(this, $"手机配对监听已停止：{exception.Message}");
        }
    }

    private static async Task RejectAdditionalClientAsync(
        TcpClient client,
        CancellationToken cancellationToken)
    {
        using (client)
        {
            var bytes = Encoding.UTF8.GetBytes("{\"type\":\"error\",\"message\":\"phone already connected\"}\n");
            await client.GetStream().WriteAsync(bytes, cancellationToken);
        }
    }

    private async Task HandleClientAsync(TcpClient client, CancellationToken cancellationToken)
    {
        var stream = client.GetStream();
        try
        {
            var handshake = await ReadLineAsync(stream, 4096, cancellationToken);
            if (!TryAuthenticate(handshake, out var phoneName))
            {
                await SendJsonLineAsync(
                    new { type = "error", message = "手机配对凭据不匹配" },
                    stream,
                    cancellationToken);
                return;
            }

            _phoneName = phoneName;
            _authenticated = true;
            UpdateMicrophoneStreaming(false);
            await SendJsonLineAsync(
                new
                {
                    type = "accepted",
                    protocol = 1,
                    sampleRate = RemoteSampleRate,
                    microphoneEnabled = false,
                },
                stream,
                cancellationToken);
            ConnectionChanged?.Invoke(this, new PhoneConnectionChangedEventArgs(true, phoneName));
            StatusChanged?.Invoke(this, $"手机已连接：{phoneName}；麦克风保持关闭。");

            var lengthBytes = new byte[4];
            while (!cancellationToken.IsCancellationRequested)
            {
                if (!await TryReadExactlyAsync(stream, lengthBytes, cancellationToken))
                {
                    break;
                }
                var bodyLength = BinaryPrimitives.ReadUInt32BigEndian(lengthBytes);
                if (bodyLength is < 1 or > MaximumFrameBytes)
                {
                    throw new InvalidDataException("手机数据帧长度无效。");
                }

                var body = new byte[bodyLength];
                if (!await TryReadExactlyAsync(stream, body, cancellationToken))
                {
                    break;
                }
                ProcessFrame(body);
            }
        }
        catch (OperationCanceledException)
        {
            // 应用退出或主动断开时不显示协议错误。
        }
        catch (Exception exception) when (exception is IOException or SocketException or InvalidDataException)
        {
            StatusChanged?.Invoke(this, $"手机麦克风连接异常：{exception.Message}");
        }
        finally
        {
            var wasCurrentClient = false;
            lock (_clientLock)
            {
                if (ReferenceEquals(_client, client))
                {
                    _client = null;
                    _clientStream = null;
                    wasCurrentClient = true;
                }
            }
            client.Dispose();
            if (wasCurrentClient)
            {
                var previousName = _phoneName;
                ResetClientState();
                ConnectionChanged?.Invoke(this, new PhoneConnectionChangedEventArgs(false, previousName));
                StatusChanged?.Invoke(this, "手机连接已断开。");
            }
        }
    }

    private bool TryAuthenticate(string handshake, out string phoneName)
    {
        phoneName = string.Empty;
        try
        {
            using var document = JsonDocument.Parse(handshake);
            var root = document.RootElement;
            var valid = root.TryGetProperty("type", out var type)
                        && type.GetString() == "hello"
                        && root.TryGetProperty("protocol", out var protocol)
                        && protocol.GetInt32() == 1
                        && root.TryGetProperty("session", out var session)
                        && string.Equals(session.GetString(), _sessionId, StringComparison.OrdinalIgnoreCase)
                        && root.TryGetProperty("secret", out var secret)
                        && string.Equals(secret.GetString(), _sessionSecret, StringComparison.OrdinalIgnoreCase);
            if (!valid)
            {
                return false;
            }

            phoneName = root.TryGetProperty("deviceName", out var name)
                ? name.GetString() ?? string.Empty
                : string.Empty;
            if (string.IsNullOrWhiteSpace(phoneName))
            {
                phoneName = "Android 手机";
            }
            return true;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private void ProcessFrame(byte[] body)
    {
        switch (body[0])
        {
            case 1 when body.Length >= 13:
                ProcessPcmFrame(body);
                break;
            case 2 when body.Length >= 2:
                UpdateMicrophoneStreaming(body[1] != 0);
                break;
            case 3:
                StatusChanged?.Invoke(this, $"手机麦克风：{Encoding.UTF8.GetString(body, 1, body.Length - 1)}");
                UpdateMicrophoneStreaming(false);
                break;
            case 4 when body.Length >= 17 && _microphoneStreaming:
                _audioEngine.AddRemoteMicrophoneClockSample(
                    BinaryPrimitives.ReadUInt64BigEndian(body.AsSpan(1, 8)),
                    BinaryPrimitives.ReadUInt64BigEndian(body.AsSpan(9, 8)));
                break;
        }
    }

    private void ProcessPcmFrame(byte[] body)
    {
        if (!_microphoneStreaming)
        {
            return;
        }

        var firstFrame = BinaryPrimitives.ReadUInt64BigEndian(body.AsSpan(1, 8));
        var sampleRate = BinaryPrimitives.ReadUInt32BigEndian(body.AsSpan(9, 4));
        var pcmBytes = body.Length - 13;
        if (sampleRate is < 8000 or > 192_000 || pcmBytes <= 0 || pcmBytes % 2 != 0)
        {
            return;
        }

        var samples = new short[pcmBytes / 2];
        double energy = 0;
        for (var index = 0; index < samples.Length; index++)
        {
            var sample = BinaryPrimitives.ReadInt16LittleEndian(body.AsSpan(13 + index * 2, 2));
            samples[index] = sample;
            var value = sample / 32768.0;
            energy += value * value;
        }
        _audioEngine.AppendRemoteMicrophonePcm16(firstFrame, sampleRate, samples);
        var rms = Math.Sqrt(energy / samples.Length);
        MicrophoneLevelChanged?.Invoke(this, rms > 0.000001 ? 20 * Math.Log10(rms) : -120);
    }

    private void UpdateMicrophoneStreaming(bool enabled)
    {
        if (_microphoneStreaming == enabled)
        {
            if (!enabled)
            {
                _audioEngine.SetRemoteMicrophoneConnected(false);
            }
            return;
        }

        _microphoneStreaming = enabled;
        _audioEngine.SetRemoteMicrophoneConnected(enabled, RemoteSampleRate);
        if (!enabled)
        {
            MicrophoneLevelChanged?.Invoke(this, -160);
        }
        MicrophoneStreamingChanged?.Invoke(this, enabled);
        StatusChanged?.Invoke(
            this,
            enabled ? "手机麦克风已启用并开始回传。" : "手机麦克风已停止并释放。");
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
            StatusChanged?.Invoke(this, $"手机发现服务已停止：{exception.Message}");
        }
    }

    private async Task SendJsonLineAsync(object value, NetworkStream stream, CancellationToken cancellationToken)
    {
        var payload = JsonSerializer.SerializeToUtf8Bytes(value, JsonOptions);
        await _sendGate.WaitAsync(cancellationToken);
        try
        {
            await stream.WriteAsync(payload, cancellationToken);
            await stream.WriteAsync("\n"u8.ToArray(), cancellationToken);
        }
        finally
        {
            _sendGate.Release();
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
                throw new IOException("手机在握手完成前断开连接。");
            }
            if (singleByte[0] == (byte)'\n')
            {
                return Encoding.UTF8.GetString(buffer.GetBuffer(), 0, (int)buffer.Length);
            }
            buffer.WriteByte(singleByte[0]);
        }
        throw new InvalidDataException("手机握手数据过长。");
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

    private void ResetClientState()
    {
        _authenticated = false;
        _phoneName = string.Empty;
        UpdateMicrophoneStreaming(false);
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
        DisconnectPhone();
        _listener?.Stop();
        _listener = null;
        _discovery?.Dispose();
        _discovery = null;
        _sendGate.Dispose();
        _lifetime.Dispose();
    }
}
