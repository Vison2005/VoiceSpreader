using VoiceSpreader.App.Models;

namespace VoiceSpreader.App.Services;

public sealed class AppHost : IDisposable
{
    private bool _disposed;

    public AppHost()
    {
        SettingsStore = new SettingsStore();
        AudioEngine = new NativeAudioEngineBridge();
    }

    public SettingsStore SettingsStore { get; }

    public NativeAudioEngineBridge AudioEngine { get; }

    public AppSettings Settings { get; private set; } = new();

    public async Task InitializeAsync(CancellationToken cancellationToken = default) =>
        Settings = await SettingsStore.LoadAsync(cancellationToken).ConfigureAwait(false);

    public async Task SaveSettingsAsync(AppSettings settings, CancellationToken cancellationToken = default)
    {
        Settings = settings;
        await SettingsStore.SaveAsync(settings, cancellationToken).ConfigureAwait(false);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        AudioEngine.Dispose();
        SettingsStore.Dispose();
    }
}
