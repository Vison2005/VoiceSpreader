using Microsoft.UI.Xaml;
using VoiceSpreader.App.Models;
using VoiceSpreader.App.Services;

namespace VoiceSpreader.App;

public partial class App : Application
{
    private MainWindow? _window;

    public App()
    {
        InitializeComponent();
        UnhandledException += App_UnhandledException;
    }

    public AppHost Host { get; } = new();

    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            await Host.InitializeAsync();
            _window = new MainWindow();
            _window.ApplyTheme(Host.Settings.ThemeMode);
            if (!Environment.GetCommandLineArgs().Contains("--background", StringComparer.OrdinalIgnoreCase))
            {
                _window.Activate();
            }
        }
        catch (Exception exception)
        {
            WriteStartupFailure(exception);
            Exit();
        }
    }

    public void ApplyTheme(AppThemeMode mode) => _window?.ApplyTheme(mode);

    public void MinimizeToTray() => _window?.MinimizeToTray();

    private void App_UnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs args)
    {
        WriteStartupFailure(args.Exception);
    }

    private static void WriteStartupFailure(Exception exception)
    {
        try
        {
            var directory = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "VoiceSpreader");
            Directory.CreateDirectory(directory);
            File.WriteAllText(
                Path.Combine(directory, "startup-error.log"),
                $"{DateTimeOffset.Now:O}{Environment.NewLine}{exception}");
        }
        catch (IOException)
        {
            // 启动异常优先于诊断文件写入失败。
        }
    }
}
