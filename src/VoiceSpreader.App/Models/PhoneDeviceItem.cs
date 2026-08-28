using System.ComponentModel;
using System.Runtime.CompilerServices;
using VoiceSpreader.App.Services;

namespace VoiceSpreader.App.Models;

public sealed class PhoneDeviceItem : INotifyPropertyChanged
{
    private bool _useAsMicrophone;
    private bool _useAsSpeaker;
    private int _microphoneGainPercent;
    private bool _microphoneStreaming;
    private bool _playbackRequested;
    private bool _playbackStreaming;
    private double _microphoneLevelDbfs;
    private double _clockDriftPpm;
    private int _microphoneBufferedMilliseconds;

    public PhoneDeviceItem(PhoneDeviceSnapshot snapshot, PhoneDeviceSettings settings)
    {
        Id = snapshot.Id;
        Name = snapshot.Name;
        RemoteAddress = snapshot.RemoteAddress;
        Protocol = snapshot.Protocol;
        _useAsMicrophone = settings.UseAsMicrophone;
        _useAsSpeaker = settings.UseAsSpeaker;
        _microphoneGainPercent = Math.Clamp(settings.MicrophoneGainPercent, 0, 200);
        Update(snapshot);
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public event EventHandler<string>? SettingsChanged;

    public string Id { get; }

    public string Name { get; private set; }

    public string RemoteAddress { get; private set; }

    public int Protocol { get; private set; }

    public bool SupportsPlayback => Protocol >= 2;

    public bool UseAsMicrophone
    {
        get => _useAsMicrophone;
        set => SetSetting(ref _useAsMicrophone, value);
    }

    public bool UseAsSpeaker
    {
        get => _useAsSpeaker;
        set => SetSetting(ref _useAsSpeaker, value);
    }

    public int MicrophoneGainPercent
    {
        get => _microphoneGainPercent;
        set
        {
            if (SetSetting(ref _microphoneGainPercent, Math.Clamp(value, 0, 200)))
            {
                OnPropertyChanged(nameof(MicrophoneGainText));
            }
        }
    }

    public string MicrophoneGainText => $"{MicrophoneGainPercent}%";

    public bool MicrophoneStreaming
    {
        get => _microphoneStreaming;
        private set => SetField(ref _microphoneStreaming, value);
    }

    public bool PlaybackRequested
    {
        get => _playbackRequested;
        private set => SetField(ref _playbackRequested, value);
    }

    public bool PlaybackStreaming
    {
        get => _playbackStreaming;
        private set => SetField(ref _playbackStreaming, value);
    }

    public double MicrophoneLevelPercent =>
        Math.Clamp((MicrophoneLevelDbfs + 60) / 60 * 100, 0, 100);

    public string ConnectionDetail => string.IsNullOrWhiteSpace(RemoteAddress)
        ? "Wi-Fi 已连接"
        : $"{RemoteAddress} · Wi-Fi";

    public string RouteSummary
    {
        get
        {
            var routes = new List<string>();
            if (UseAsSpeaker)
            {
                routes.Add(PlaybackStreaming ? "扬声器播放中" : "电脑扬声器");
            }
            if (UseAsMicrophone)
            {
                routes.Add(MicrophoneStreaming ? "麦克风传输中" : "电脑麦克风");
            }
            return routes.Count == 0 ? "未加入音频路由" : string.Join(" · ", routes);
        }
    }

    public string SynchronizationDetail
    {
        get
        {
            if (!MicrophoneStreaming)
            {
                return "麦克风未传输";
            }
            return $"输出缓冲 {MicrophoneBufferedMilliseconds} ms · 时钟 {ClockDriftPpm:+0.0;-0.0;0.0} ppm";
        }
    }

    public string PlaybackSynchronizationDetail => !SupportsPlayback
        ? "客户端版本不支持下行音频"
        : PlaybackStreaming ? "共享帧序号 · 80 ms 预缓冲" : "等待开始播放";

    public double MicrophoneLevelDbfs
    {
        get => _microphoneLevelDbfs;
        private set
        {
            if (SetField(ref _microphoneLevelDbfs, value))
            {
                OnPropertyChanged(nameof(MicrophoneLevelPercent));
            }
        }
    }

    public double ClockDriftPpm
    {
        get => _clockDriftPpm;
        private set => SetField(ref _clockDriftPpm, value);
    }

    public int MicrophoneBufferedMilliseconds
    {
        get => _microphoneBufferedMilliseconds;
        private set => SetField(ref _microphoneBufferedMilliseconds, value);
    }

    public PhoneDeviceSettings ToSettings() => new(
        UseAsMicrophone,
        UseAsSpeaker,
        MicrophoneGainPercent);

    public void Update(PhoneDeviceSnapshot snapshot)
    {
        if (Name != snapshot.Name)
        {
            Name = snapshot.Name;
            OnPropertyChanged(nameof(Name));
        }
        if (RemoteAddress != snapshot.RemoteAddress)
        {
            RemoteAddress = snapshot.RemoteAddress;
            OnPropertyChanged(nameof(RemoteAddress));
            OnPropertyChanged(nameof(ConnectionDetail));
        }
        if (Protocol != snapshot.Protocol)
        {
            Protocol = snapshot.Protocol;
            OnPropertyChanged(nameof(Protocol));
            OnPropertyChanged(nameof(SupportsPlayback));
        }
        MicrophoneStreaming = snapshot.MicrophoneStreaming;
        PlaybackRequested = snapshot.PlaybackRequested;
        PlaybackStreaming = snapshot.PlaybackStreaming;
        MicrophoneLevelDbfs = snapshot.MicrophoneLevelDbfs;
        ClockDriftPpm = snapshot.ClockDriftPpm;
        MicrophoneBufferedMilliseconds = snapshot.MicrophoneBufferedMilliseconds;
        OnPropertyChanged(nameof(RouteSummary));
        OnPropertyChanged(nameof(SynchronizationDetail));
        OnPropertyChanged(nameof(PlaybackSynchronizationDetail));
    }

    private bool SetSetting<T>(ref T field, T value, [CallerMemberName] string propertyName = "")
    {
        if (!SetField(ref field, value, propertyName))
        {
            return false;
        }
        OnPropertyChanged(nameof(RouteSummary));
        OnPropertyChanged(nameof(SynchronizationDetail));
        SettingsChanged?.Invoke(this, propertyName);
        return true;
    }

    private bool SetField<T>(ref T field, T value, [CallerMemberName] string propertyName = "")
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return false;
        }
        field = value;
        OnPropertyChanged(propertyName);
        return true;
    }

    private void OnPropertyChanged([CallerMemberName] string propertyName = "") =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
