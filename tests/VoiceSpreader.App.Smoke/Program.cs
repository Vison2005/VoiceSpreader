using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using VoiceSpreader.App.Services;
using VoiceSpreader.App.Models;

var adjustableOutput = new OutputEndpointItem(new AudioEndpoint("test-output", "测试输出", false))
{
    DelayMilliseconds = -25,
};
Require(adjustableOutput.DelayMilliseconds == -25, "输出相对补偿不接受负值");
Require(adjustableOutput.DelayDisplay == "-25 ms", "输出相对补偿显示格式无效");

var sink = new RecordingRemoteMicrophoneSink();
var systemAudio = new RecordingSystemAudioSource();
using var service = new PhonePairingService(sink, systemAudio);
var disconnected = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
service.ConnectionChanged += (_, args) =>
{
    if (!args.Connected)
    {
        disconnected.TrySetResult();
    }
};
service.Start();

using (var discovery = new UdpClient(AddressFamily.InterNetwork))
{
    var request = Encoding.ASCII.GetBytes($"VSP_DISCOVER {service.PairingCode}");
    await discovery.SendAsync(request, new IPEndPoint(IPAddress.Loopback, 39741));
    using var discoveryTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(2));
    var reply = await discovery.ReceiveAsync(discoveryTimeout.Token);
    using var response = JsonDocument.Parse(reply.Buffer);
    Require(response.RootElement.GetProperty("secret").GetString()?.Length == 32,
        "UDP 发现没有返回会话密钥");
}

var payloadParts = service.PairingPayload.Split(':');
Require(payloadParts.Length == 5, "配对负载格式无效");

using var client = new TcpClient();
await client.ConnectAsync(IPAddress.Loopback, service.ServerPort);
var stream = client.GetStream();
var hello = JsonSerializer.SerializeToUtf8Bytes(new
{
    type = "hello",
    protocol = 1,
    session = payloadParts[3],
    secret = payloadParts[4],
    deviceName = "WinUI smoke phone",
});
await stream.WriteAsync(hello);
await stream.WriteAsync("\n"u8.ToArray());
var accepted = await ReadLineAsync(stream);
Require(accepted.Contains("\"accepted\"", StringComparison.Ordinal), "TCP 握手未被接受");

Require(await service.SetMicrophoneEnabledAsync(true), "桌面端无法发送旧协议麦克风启用命令");
var startCommand = await ReadLineAsync(stream);
Require(startCommand.Contains("\"enabled\":true", StringComparison.Ordinal), "旧协议麦克风启用命令格式无效");
await stream.WriteAsync(CreateMicrophoneStateFrame(enabled: true));
await WaitForAsync(() => service.IsMicrophoneStreaming, "手机麦克风启用状态未写入会话");

var pcmPrefix = new short[] { 0, 16384, -16384, 32767, -32768 };
var pcm = new short[1920];
pcmPrefix.CopyTo(pcm, 0);
await stream.WriteAsync(CreatePcmFrame(123456, 48000, pcm));
await stream.WriteAsync(CreateClockFrame(123456, 1_000_000_000));
await stream.WriteAsync(CreateClockFrame(123936, 1_010_000_000));
await WaitForAsync(
    () => sink.MixedSamples.Take(pcmPrefix.Length).SequenceEqual(pcmPrefix),
    "PCM 数据未抵达 VB-CABLE 单路输出接口");

Require(await service.SetMicrophoneEnabledAsync(false), "桌面端无法发送麦克风停用命令");
var command = await ReadLineAsync(stream);
Require(command.Contains("\"enabled\":false", StringComparison.Ordinal), "麦克风停用命令格式无效");
await stream.WriteAsync(CreateMicrophoneStateFrame(enabled: false));
await WaitForAsync(() => !service.IsMicrophoneStreaming, "手机麦克风停用状态未写入会话");
service.DisconnectPhone();
await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(2));

using var protocol2Client = new TcpClient();
await protocol2Client.ConnectAsync(IPAddress.Loopback, service.ServerPort);
var protocol2Stream = protocol2Client.GetStream();
var protocol2Hello = JsonSerializer.SerializeToUtf8Bytes(new
{
    type = "hello",
    protocol = 2,
    session = payloadParts[3],
    secret = payloadParts[4],
    deviceId = "phone-1",
    deviceName = "WinUI protocol 2 phone",
});
await protocol2Stream.WriteAsync(protocol2Hello);
await protocol2Stream.WriteAsync("\n"u8.ToArray());
var protocol2Accepted = await ReadLineAsync(protocol2Stream);
Require(protocol2Accepted.Contains("\"protocol\":2", StringComparison.Ordinal),
    "TCP v2 握手未协商二进制下行协议");

Require(await service.SetMicrophoneEnabledAsync("phone-1", true), "桌面端无法发送 v2 麦克风启用命令");
var protocol2MicrophoneCommand = await ReadBinaryFrameAsync(protocol2Stream);
Require(protocol2MicrophoneCommand.SequenceEqual(new byte[] { 10, 1 }),
    "v2 麦克风启用命令格式无效");
await protocol2Stream.WriteAsync(CreateMicrophoneStateFrame(enabled: true));
await WaitForAsync(() => service.IsMicrophoneStreaming, "v2 手机麦克风启用状态未写入会话");

Require(await service.SetPlaybackEnabledAsync("phone-1", true), "桌面端无法发送手机播放启用命令");
var playbackCommand = await ReadBinaryFrameAsync(protocol2Stream);
Require(playbackCommand.SequenceEqual(new byte[] { 11, 1 }), "手机播放启用命令格式无效");

var playbackPcm = new short[] { 1000, -1000, 2000, -2000 };
systemAudio.Emit(new SystemAudioFrameEventArgs(480, 48_000, 2, playbackPcm));
var playbackFrame = await ReadBinaryFrameAsync(protocol2Stream);
Require(playbackFrame[0] == 12, "Windows 音频帧类型无效");
Require(BinaryPrimitives.ReadUInt64BigEndian(playbackFrame.AsSpan(1, 8)) == 480,
    "Windows 音频帧序号无效");
Require(BinaryPrimitives.ReadUInt32BigEndian(playbackFrame.AsSpan(9, 4)) == 48_000,
    "Windows 音频采样率无效");
Require(playbackFrame[13] == 2, "Windows 音频声道数无效");
Require(BinaryPrimitives.ReadInt16LittleEndian(playbackFrame.AsSpan(14, 2)) == 1000,
    "Windows PCM 数据无效");

await protocol2Stream.WriteAsync(CreatePlaybackStateFrame(enabled: true));
await WaitForAsync(() => service.IsPlaybackStreaming, "手机播放就绪状态未写入桌面端");

using var tabletClient = new TcpClient();
await tabletClient.ConnectAsync(IPAddress.Loopback, service.ServerPort);
var tabletStream = tabletClient.GetStream();
var tabletHello = JsonSerializer.SerializeToUtf8Bytes(new
{
    type = "hello",
    protocol = 2,
    session = payloadParts[3],
    secret = payloadParts[4],
    deviceId = "tablet-1",
    deviceName = "WinUI protocol 2 tablet",
});
await tabletStream.WriteAsync(tabletHello);
await tabletStream.WriteAsync("\n"u8.ToArray());
var tabletAccepted = await ReadLineAsync(tabletStream);
Require(tabletAccepted.Contains("\"multiDevice\":true", StringComparison.Ordinal),
    "TCP v2 握手未声明多设备能力");
await WaitForAsync(() => service.ConnectedDevices.Count == 2, "服务端没有同时保留两台设备");

service.SetMicrophoneGain("phone-1", 100);
service.SetMicrophoneGain("tablet-1", 50);
Require(await service.SetMicrophoneEnabledAsync("tablet-1", true), "桌面端无法切换到第二台设备麦克风");
Require((await ReadBinaryFrameAsync(protocol2Stream)).SequenceEqual(new byte[] { 10, 0 }),
    "切换麦克风来源时没有停用第一台设备");
Require((await ReadBinaryFrameAsync(tabletStream)).SequenceEqual(new byte[] { 10, 1 }),
    "第二台设备麦克风命令格式无效");
await tabletStream.WriteAsync(CreateMicrophoneStateFrame(enabled: true));
await WaitForAsync(() => service.IsMicrophoneStreaming, "第二台设备麦克风启用状态未写入会话");
var framesBeforeRejectedSource = sink.MixedFrameCount;
await protocol2Stream.WriteAsync(CreateMicrophoneStateFrame(enabled: true));
await protocol2Stream.WriteAsync(CreatePcmFrame(124000, 48_000, Enumerable.Repeat((short)3000, 1920).ToArray()));
await Task.Delay(150);
Require(sink.MixedFrameCount == framesBeforeRejectedSource,
    "已经停用的设备仍能向 VB-CABLE 写入麦克风数据");
await tabletStream.WriteAsync(CreatePcmFrame(224000, 48_000, Enumerable.Repeat((short)2000, 1920).ToArray()));
await WaitForAsync(
    () => sink.MixedFrameCount > framesBeforeRejectedSource
          && sink.MixedSamples.FirstOrDefault() == 1000,
    "当前设备麦克风没有按独立增益进入单路输出");

Require(await service.SetPlaybackEnabledAsync("tablet-1", true), "桌面端无法启用第二台设备播放");
Require((await ReadBinaryFrameAsync(tabletStream)).SequenceEqual(new byte[] { 11, 1 }),
    "第二台设备播放命令格式无效");
systemAudio.Emit(new SystemAudioFrameEventArgs(960, 48_000, 2, playbackPcm));
var phoneBroadcastFrame = await ReadBinaryFrameAsync(protocol2Stream);
var tabletBroadcastFrame = await ReadBinaryFrameAsync(tabletStream);
Require(phoneBroadcastFrame[0] == 12 && tabletBroadcastFrame[0] == 12,
    "Windows 音频没有广播到所有选中的设备");

Require(await service.SetPlaybackEnabledAsync("tablet-1", false), "桌面端无法停止第二台设备播放");
Require((await ReadBinaryFrameAsync(tabletStream)).SequenceEqual(new byte[] { 11, 0 }),
    "第二台设备播放停用命令格式无效");
Require(await service.SetPlaybackEnabledAsync("phone-1", false), "桌面端无法发送手机播放停用命令");
var stopPlaybackCommand = await ReadBinaryFrameAsync(protocol2Stream);
Require(stopPlaybackCommand.SequenceEqual(new byte[] { 11, 0 }), "手机播放停用命令格式无效");
Require(await service.SetMicrophoneEnabledAsync("tablet-1", false), "桌面端无法停止第二台设备麦克风");
Require((await ReadBinaryFrameAsync(tabletStream)).SequenceEqual(new byte[] { 10, 0 }),
    "第二台设备麦克风停用命令格式无效");
Require(await service.SetMicrophoneEnabledAsync("phone-1", false), "桌面端无法发送 v2 麦克风停用命令");
var stopProtocol2MicrophoneCommand = await ReadBinaryFrameAsync(protocol2Stream);
Require(stopProtocol2MicrophoneCommand.SequenceEqual(new byte[] { 10, 0 }),
    "v2 麦克风停用命令格式无效");

Console.WriteLine("WinUI phone pairing protocol smoke test passed");

static byte[] CreateMicrophoneStateFrame(bool enabled)
{
    var frame = new byte[6];
    BinaryPrimitives.WriteUInt32BigEndian(frame, 2);
    frame[4] = 2;
    frame[5] = enabled ? (byte)1 : (byte)0;
    return frame;
}

static byte[] CreatePcmFrame(ulong firstFrame, uint sampleRate, short[] samples)
{
    var bodyLength = 13 + samples.Length * 2;
    var frame = new byte[4 + bodyLength];
    BinaryPrimitives.WriteUInt32BigEndian(frame, (uint)bodyLength);
    frame[4] = 1;
    BinaryPrimitives.WriteUInt64BigEndian(frame.AsSpan(5, 8), firstFrame);
    BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(13, 4), sampleRate);
    for (var index = 0; index < samples.Length; index++)
    {
        BinaryPrimitives.WriteInt16LittleEndian(frame.AsSpan(17 + index * 2, 2), samples[index]);
    }
    return frame;
}

static byte[] CreatePlaybackStateFrame(bool enabled)
{
    var frame = new byte[6];
    BinaryPrimitives.WriteUInt32BigEndian(frame, 2);
    frame[4] = 5;
    frame[5] = enabled ? (byte)1 : (byte)0;
    return frame;
}

static byte[] CreateClockFrame(ulong frameIndex, ulong monotonicNanoseconds)
{
    var frame = new byte[21];
    BinaryPrimitives.WriteUInt32BigEndian(frame, 17);
    frame[4] = 4;
    BinaryPrimitives.WriteUInt64BigEndian(frame.AsSpan(5, 8), frameIndex);
    BinaryPrimitives.WriteUInt64BigEndian(frame.AsSpan(13, 8), monotonicNanoseconds);
    return frame;
}

static async Task<string> ReadLineAsync(NetworkStream stream)
{
    using var buffer = new MemoryStream();
    var value = new byte[1];
    while (await stream.ReadAsync(value) == 1)
    {
        if (value[0] == (byte)'\n')
        {
            return Encoding.UTF8.GetString(buffer.GetBuffer(), 0, (int)buffer.Length);
        }
        buffer.WriteByte(value[0]);
    }
    throw new InvalidDataException("连接在读取 JSON 行时关闭");
}

static async Task<byte[]> ReadBinaryFrameAsync(NetworkStream stream)
{
    var lengthBytes = new byte[4];
    await stream.ReadExactlyAsync(lengthBytes);
    var length = checked((int)BinaryPrimitives.ReadUInt32BigEndian(lengthBytes));
    if (length is < 1 or > 256 * 1024)
    {
        throw new InvalidDataException("二进制下行帧长度无效");
    }
    var body = new byte[length];
    await stream.ReadExactlyAsync(body);
    return body;
}

static async Task WaitForAsync(Func<bool> predicate, string message)
{
    var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(2);
    while (DateTime.UtcNow < deadline)
    {
        if (predicate())
        {
            return;
        }
        await Task.Delay(10);
    }
    throw new InvalidOperationException(message);
}

static void Require(bool condition, string message)
{
    if (!condition)
    {
        throw new InvalidOperationException(message);
    }
}

file sealed class RecordingRemoteMicrophoneSink : IRemoteMicrophoneSink
{
    private readonly object _gate = new();
    private int _mixedFrameCount;
    private short[] _mixedSamples = [];

    public short[] MixedSamples
    {
        get { lock (_gate) { return [.. _mixedSamples]; } }
    }

    public int MixedFrameCount
    {
        get { lock (_gate) { return _mixedFrameCount; } }
    }

    public void PushRemoteMicrophoneOutputPcm16(uint sampleRate, short[] samples)
    {
        lock (_gate)
        {
            _mixedSamples = [.. samples];
            _mixedFrameCount++;
        }
    }
}

file sealed class RecordingSystemAudioSource : ISystemAudioCaptureSource
{
    public event EventHandler<SystemAudioFrameEventArgs>? SystemAudioFrameReady;

    public bool StartSystemAudioCapture(AudioEndpoint device, int volumePercent = 100) => true;

    public void StopSystemAudioCapture()
    {
    }

    public bool IsSystemAudioCaptureActive => false;

    public void SetSystemAudioCaptureVolume(int volumePercent)
    {
    }

    public void Emit(SystemAudioFrameEventArgs frame) =>
        SystemAudioFrameReady?.Invoke(this, frame);
}
