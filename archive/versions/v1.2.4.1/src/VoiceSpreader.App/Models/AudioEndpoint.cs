using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace VoiceSpreader.App.Models;

public sealed record AudioEndpoint(string Id, string Name, bool IsDefault)
{
    public string DisplayName => IsDefault ? $"{Name}（默认）" : Name;
}

public sealed class OutputEndpointItem : INotifyPropertyChanged
{
    private bool _isSelected;
    private int _volumePercent = 100;
    private int _delayMilliseconds;
    private int _automaticDelayMilliseconds;
    private string _correctionText = "等待测量";
    private bool _selectionEnabled = true;
    private bool _adjustmentsEnabled = true;

    public OutputEndpointItem(AudioEndpoint endpoint)
    {
        Endpoint = endpoint;
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public event EventHandler? SettingsChanged;

    public AudioEndpoint Endpoint { get; }

    public string Id => Endpoint.Id;

    public string Name => Endpoint.Name;

    public bool IsDefault => Endpoint.IsDefault;

    public bool IsSelected
    {
        get => _isSelected;
        set => SetField(ref _isSelected, value);
    }

    public int VolumePercent
    {
        get => _volumePercent;
        set => SetField(ref _volumePercent, Math.Clamp(value, 0, 100));
    }

    public int DelayMilliseconds
    {
        get => _delayMilliseconds;
        set
        {
            if (SetField(ref _delayMilliseconds, Math.Clamp(value, -500, 500)))
            {
                OnPropertyChanged(nameof(DelayDisplay));
                OnPropertyChanged(nameof(EffectiveDelayDisplay));
            }
        }
    }

    public string DelayDisplay => $"{DelayMilliseconds:+0;-0;0} ms";

    public int AutomaticDelayMilliseconds
    {
        get => _automaticDelayMilliseconds;
        set
        {
            if (_automaticDelayMilliseconds == value)
            {
                return;
            }

            _automaticDelayMilliseconds = Math.Clamp(value, 0, 500);
            OnPropertyChanged();
            OnPropertyChanged(nameof(AutomaticDelayDisplay));
            OnPropertyChanged(nameof(EffectiveDelayDisplay));
        }
    }

    public string AutomaticDelayDisplay => $"自动 {AutomaticDelayMilliseconds:+0;-0;0} ms";

    public string EffectiveDelayDisplay =>
        $"{DelayMilliseconds + AutomaticDelayMilliseconds:+0;-0;0} ms";

    public bool SelectionEnabled
    {
        get => _selectionEnabled;
        set => SetControlState(ref _selectionEnabled, value);
    }

    public bool AdjustmentsEnabled
    {
        get => _adjustmentsEnabled;
        set => SetControlState(ref _adjustmentsEnabled, value);
    }

    public string CorrectionText
    {
        get => _correctionText;
        set
        {
            if (_correctionText == value)
            {
                return;
            }

            _correctionText = value;
            OnPropertyChanged();
        }
    }

    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return false;
        }

        field = value;
        OnPropertyChanged(propertyName);
        SettingsChanged?.Invoke(this, EventArgs.Empty);
        return true;
    }

    private void SetControlState(ref bool field, bool value, [CallerMemberName] string? propertyName = null)
    {
        if (field == value)
        {
            return;
        }

        field = value;
        OnPropertyChanged(propertyName);
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
