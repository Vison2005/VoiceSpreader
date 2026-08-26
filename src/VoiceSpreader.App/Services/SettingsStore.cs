using System.Text.Json;
using VoiceSpreader.App.Models;

namespace VoiceSpreader.App.Services;

public sealed class SettingsStore : IDisposable
{
    private static readonly JsonSerializerOptions SerializerOptions = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
    };

    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly string _directory = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "VoiceSpreader");

    private string SettingsPath => Path.Combine(_directory, "settings.json");

    public async Task<AppSettings> LoadAsync(CancellationToken cancellationToken = default)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (!File.Exists(SettingsPath))
            {
                return new AppSettings();
            }

            await using var stream = File.OpenRead(SettingsPath);
            return await JsonSerializer.DeserializeAsync<AppSettings>(
                       stream,
                       SerializerOptions,
                       cancellationToken)
                       .ConfigureAwait(false)
                   ?? new AppSettings();
        }
        catch (JsonException)
        {
            BackupCorruptedSettings();
            return new AppSettings();
        }
        catch (IOException)
        {
            return new AppSettings();
        }
        finally
        {
            _gate.Release();
        }
    }

    public async Task SaveAsync(AppSettings settings, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(settings);
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            Directory.CreateDirectory(_directory);
            var temporaryPath = SettingsPath + ".tmp";
            await using (var stream = File.Create(temporaryPath))
            {
                await JsonSerializer.SerializeAsync(
                        stream,
                        settings,
                        SerializerOptions,
                        cancellationToken)
                    .ConfigureAwait(false);
            }

            File.Move(temporaryPath, SettingsPath, overwrite: true);
        }
        finally
        {
            _gate.Release();
        }
    }

    private void BackupCorruptedSettings()
    {
        try
        {
            var backupPath = Path.Combine(
                _directory,
                $"settings.corrupt-{DateTimeOffset.Now:yyyyMMdd-HHmmss}.json");
            File.Move(SettingsPath, backupPath, overwrite: true);
        }
        catch (IOException)
        {
            // 隔离失败时保留原文件，下次启动仍会安全回退到默认设置。
        }
    }

    public void Dispose() => _gate.Dispose();
}
