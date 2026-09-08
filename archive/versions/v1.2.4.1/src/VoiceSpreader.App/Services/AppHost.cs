using VoiceSpreader.App.Models;

namespace VoiceSpreader.App.Services;

public sealed class AppHost : IDisposable
{
    private bool _disposed;

    public AppHost()
    {
        SettingsStore = new SettingsStore();
        AudioEngine = new NativeAudioEngineBridge();
        BluetoothAudioReceiver = new BluetoothAudioReceiverService();
        InputBridge = new InputBridgeService();
        Shortcuts = new ShortcutService(InputBridge);
        PhonePairing = new PhonePairingService(AudioEngine, AudioEngine);
    }

    public SettingsStore SettingsStore { get; }

    public NativeAudioEngineBridge AudioEngine { get; }

    public BluetoothAudioReceiverService BluetoothAudioReceiver { get; }

    public InputBridgeService InputBridge { get; }

    public ShortcutService Shortcuts { get; }

    public PhonePairingService PhonePairing { get; }

    public AppSettings Settings { get; private set; } = new();

    public async Task InitializeAsync(CancellationToken cancellationToken = default)
    {
        Settings = await SettingsStore.LoadAsync(cancellationToken).ConfigureAwait(false);
        if (PhonePairing.RestoreCredentials(
                Settings.PhonePairingSessionId,
                Settings.PhonePairingSecret))
        {
            return;
        }

        // 首次运行或旧版本没有持久化凭据时，立即保存稳定身份，供手机端下次直接重连。
        Settings = Settings with
        {
            PhonePairingSessionId = PhonePairing.PairingSessionId,
            PhonePairingSecret = PhonePairing.PairingSecret,
        };
        try
        {
            await SettingsStore.SaveAsync(Settings, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            // 设置目录不可写时仍允许继续运行，本次凭据会在后续保存设置时重试。
        }
    }

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
        PhonePairing.Dispose();
        BluetoothAudioReceiver.Dispose();
        InputBridge.Dispose();
        AudioEngine.Dispose();
        SettingsStore.Dispose();
    }
}
