namespace VoiceSpreader.App.Models;

public enum AppThemeMode
{
    System,
    Light,
    Dark,
    ExtremeDark,
}

public sealed record OutputSettings(bool Selected = false, int VolumePercent = 100, int DelayMilliseconds = 0);

public sealed record PhoneDeviceSettings(
    bool UseAsMicrophone = false,
    bool UseAsSpeaker = false,
    int MicrophoneGainPercent = 100,
    // 保留设备信息，断线或重启后 Windows 可以向手机发起重连请求。
    string? Name = null,
    string? RemoteAddress = null,
    DateTimeOffset? LastConnectedAt = null);

/// <summary>可从手机触摸板一键启动的 Windows 应用白名单。</summary>
public sealed record ComputerShortcut(
    string Id,
    string Name,
    string ExecutablePath,
    string Arguments = "");

public sealed record AppSettings
{
    public string? CaptureDeviceId { get; init; }

    public string? MicrophoneDeviceId { get; init; }

    public int BufferMilliseconds { get; init; } = 5;

    public bool AutomaticLatencyCompensation { get; init; } = true;

    public bool ContinuousAcousticTracking { get; init; } = true;

    public bool ExclusiveMode { get; init; }

    public AppThemeMode ThemeMode { get; init; } = AppThemeMode.System;

    public string? PhonePairingAddress { get; init; }

    // 配对会话身份需要跨应用重启保持不变，已保存的手机才能自动定位新的监听端口。
    public string? PhonePairingSessionId { get; init; }

    public string? PhonePairingSecret { get; init; }

    public string? PhoneMicrophoneOutputDeviceId { get; init; }

    public int PhoneMicrophoneOutputVolume { get; init; } = 100;

    public string? PhonePlaybackSourceDeviceId { get; init; }

    public int PhonePlaybackVolume { get; init; } = 100;

    public Dictionary<string, PhoneDeviceSettings> PhoneDevices { get; init; } =
        new(StringComparer.OrdinalIgnoreCase);

    public List<ComputerShortcut> ComputerShortcuts { get; init; } = [];

    public Dictionary<string, OutputSettings> Outputs { get; init; } = new(StringComparer.OrdinalIgnoreCase);
}
