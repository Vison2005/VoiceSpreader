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
using var service = new PhonePairingService(sink);
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

await stream.WriteAsync(CreateMicrophoneStateFrame(enabled: true));
await WaitForAsync(() => sink.Connected, "手机麦克风启用状态未写入原生缓冲");

var pcm = new short[] { 0, 16384, -16384, 32767, -32768 };
await stream.WriteAsync(CreatePcmFrame(123456, 48000, pcm));
await stream.WriteAsync(CreateClockFrame(123456, 1_000_000_000));
await WaitForAsync(() => sink.Samples.SequenceEqual(pcm), "PCM 数据未抵达远程缓冲接口");
await WaitForAsync(() => sink.ClockFrameIndex == 123456, "手机时钟样本未抵达远程缓冲接口");

Require(await service.SetMicrophoneEnabledAsync(false), "桌面端无法发送麦克风停用命令");
var command = await ReadLineAsync(stream);
Require(command.Contains("\"enabled\":false", StringComparison.Ordinal), "麦克风停用命令格式无效");
await stream.WriteAsync(CreateMicrophoneStateFrame(enabled: false));
await WaitForAsync(() => !sink.Connected, "手机麦克风停用状态未写入原生缓冲");
service.DisconnectPhone();
await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(2));

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
    private bool _connected;
    private short[] _samples = [];
    private ulong _clockFrameIndex;

    public bool Connected
    {
        get { lock (_gate) { return _connected; } }
    }

    public short[] Samples
    {
        get { lock (_gate) { return [.. _samples]; } }
    }

    public ulong ClockFrameIndex
    {
        get { lock (_gate) { return _clockFrameIndex; } }
    }

    public void SetRemoteMicrophoneConnected(bool connected, uint sampleRate = 48_000)
    {
        lock (_gate)
        {
            _connected = connected;
        }
    }

    public void AddRemoteMicrophoneClockSample(ulong frameIndex, ulong monotonicNanoseconds)
    {
        lock (_gate)
        {
            _clockFrameIndex = frameIndex;
        }
    }

    public void AppendRemoteMicrophonePcm16(ulong firstFrameIndex, uint sampleRate, short[] samples)
    {
        lock (_gate)
        {
            _samples = [.. samples];
        }
    }
}
