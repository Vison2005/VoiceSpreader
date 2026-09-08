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

    public MainWindow? MainWindow => _window;

    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        await LaunchAsync(
            IsBackgroundLaunch(args.Arguments)
            || IsStartupTaskLaunch());
    }

    private async Task LaunchAsync(bool background)
    {
        try
        {
            Directory.SetCurrentDirectory(AppContext.BaseDirectory);
            await Host.InitializeAsync();
            _window = new MainWindow();
            _window.ApplyTheme(Host.Settings.ThemeMode);
            if (!background)
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

    private static bool IsBackgroundLaunch(string? arguments) =>
        arguments?.Split(' ', StringSplitOptions.RemoveEmptyEntries)
            .Contains("--background", StringComparer.OrdinalIgnoreCase) == true
        || Environment.GetCommandLineArgs().Contains("--background", StringComparer.OrdinalIgnoreCase);

    private static bool IsStartupTaskLaunch()
    {
        try
        {
            return Windows.ApplicationModel.AppInstance.GetActivatedEventArgs()?.Kind
                   == Windows.ApplicationModel.Activation.ActivationKind.StartupTask;
        }
        catch (Exception)
        {
            // 未打包运行时可能没有可读取的包激活信息。
            return false;
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
                "SoundSpreader");
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
