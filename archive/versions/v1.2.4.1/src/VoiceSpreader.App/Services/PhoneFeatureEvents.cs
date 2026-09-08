namespace VoiceSpreader.App.Services;

public sealed class PhoneInputPointerEventArgs(
    string deviceId,
    string deviceName,
    uint sequence,
    ulong timestamp,
    byte action,
    uint pointerId,
    int deltaX,
    int deltaY) : EventArgs
{
    public string DeviceId { get; } = deviceId;
    public string DeviceName { get; } = deviceName;
    public uint Sequence { get; } = sequence;
    public ulong Timestamp { get; } = timestamp;
    public byte Action { get; } = action;
    public uint PointerId { get; } = pointerId;
    public int DeltaX { get; } = deltaX;
    public int DeltaY { get; } = deltaY;
}

public sealed class PhoneInputButtonEventArgs(
    string deviceId,
    string deviceName,
    uint sequence,
    ulong timestamp,
    byte button,
    byte state) : EventArgs
{
    public string DeviceId { get; } = deviceId;
    public string DeviceName { get; } = deviceName;
    public uint Sequence { get; } = sequence;
    public ulong Timestamp { get; } = timestamp;
    public byte Button { get; } = button;
    public byte State { get; } = state;
}

public sealed class PhoneInputScrollEventArgs(
    string deviceId,
    string deviceName,
    uint sequence,
    ulong timestamp,
    int deltaX,
    int deltaY) : EventArgs
{
    public string DeviceId { get; } = deviceId;
    public string DeviceName { get; } = deviceName;
    public uint Sequence { get; } = sequence;
    public ulong Timestamp { get; } = timestamp;
    public int DeltaX { get; } = deltaX;
    public int DeltaY { get; } = deltaY;
}

public sealed class PhoneShortcutEventArgs(
    string deviceId,
    string deviceName,
    string payload) : EventArgs
{
    public string DeviceId { get; } = deviceId;
    public string DeviceName { get; } = deviceName;
    public string Payload { get; } = payload;
}

public sealed class PhoneFileOfferEventArgs(
    string deviceId,
    string deviceName,
    string payload) : EventArgs
{
    public string DeviceId { get; } = deviceId;
    public string DeviceName { get; } = deviceName;
    public string Payload { get; } = payload;
}

public sealed class PhoneFileCompleteEventArgs(
    string deviceId,
    string deviceName,
    string payload) : EventArgs
{
    public string DeviceId { get; } = deviceId;
    public string DeviceName { get; } = deviceName;
    public string Payload { get; } = payload;
}

public sealed class PhoneScanResultEventArgs(
    string deviceId,
    string deviceName,
    string payload) : EventArgs
{
    public string DeviceId { get; } = deviceId;
    public string DeviceName { get; } = deviceName;
    public string Payload { get; } = payload;
}

public sealed class PhoneCaptureResultEventArgs(
    string deviceId,
    string deviceName,
    string payload) : EventArgs
{
    public string DeviceId { get; } = deviceId;
    public string DeviceName { get; } = deviceName;
    public string Payload { get; } = payload;
}
