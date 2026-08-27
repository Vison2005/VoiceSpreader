namespace VoiceSpreader.App.Models;

public enum AppThemeMode
{
    System,
    Light,
    Dark,
}

public sealed record OutputSettings(bool Selected = false, int VolumePercent = 100, int DelayMilliseconds = 0);

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

    public string? PhoneMicrophoneOutputDeviceId { get; init; }

    public int PhoneMicrophoneOutputVolume { get; init; } = 100;

    public string? PhonePlaybackSourceDeviceId { get; init; }

    public int PhonePlaybackVolume { get; init; } = 100;

    public Dictionary<string, OutputSettings> Outputs { get; init; } = new(StringComparer.OrdinalIgnoreCase);
}
