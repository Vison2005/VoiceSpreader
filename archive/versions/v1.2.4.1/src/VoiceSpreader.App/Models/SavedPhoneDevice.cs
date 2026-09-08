namespace VoiceSpreader.App.Models;

/// <summary>
/// Windows 端保留的手机连接信息。配对密钥仍由当前 Windows 配对会话统一管理，
/// 这里保存设备地址仅用于发起局域网重连唤醒。
/// </summary>
public sealed record SavedPhoneDevice(
    string Id,
    string Name,
    string RemoteAddress,
    DateTimeOffset LastConnectedAt)
{
    public string AddressText => $"{RemoteAddress} · 最近连接 {LastConnectedAt:MM-dd HH:mm}";
}
