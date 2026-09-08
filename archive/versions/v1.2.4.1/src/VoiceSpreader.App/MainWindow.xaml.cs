using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using System.Runtime.InteropServices;
using VoiceSpreader.App.Models;
using VoiceSpreader.App.Services;
using Windows.Graphics;
using Windows.UI;
using Microsoft.UI.Xaml.Media;

namespace VoiceSpreader.App;

[System.Diagnostics.CodeAnalysis.SuppressMessage(
    "Design",
    "CA1001:Types that own disposable fields should be disposable",
    Justification = "WinUI Window 生命周期由 AppWindow 管理，Closing 事件会释放托盘资源。")]
public sealed partial class MainWindow : Window
{
    private const int InitialLogicalWidth = 1160;
    private const int InitialLogicalHeight = 840;
    private const int MinimumLogicalWidth = 760;
    private const int MinimumLogicalHeight = 820;
    private bool _initialSizeApplied;
    private readonly TrayIconService _trayIcon;
    private bool _exitRequested;
    private bool _trayMessageShown;
    private bool _disposed;

    public MainWindow()
    {
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        AppWindow.SetIcon("Assets/AppIcon.ico");
        AppWindow.Closing += AppWindow_Closing;
        _trayIcon = new TrayIconService();
        _trayIcon.ShowRequested += TrayIcon_ShowRequested;
        _trayIcon.HideRequested += TrayIcon_HideRequested;
        _trayIcon.ExitRequested += TrayIcon_ExitRequested;
        RootFrame.Navigate(typeof(MainPage));
        RootFrame.Loaded += RootFrame_Loaded;
    }

    public void MinimizeToTray()
    {
        AppWindow.Hide();
        if (!_trayMessageShown)
        {
            _trayIcon.ShowFirstMinimizeMessage();
            _trayMessageShown = true;
        }
    }

    private void TrayIcon_ShowRequested(object? sender, EventArgs args)
    {
        AppWindow.Show();
        Activate();
    }

    private void TrayIcon_HideRequested(object? sender, EventArgs args) => MinimizeToTray();

    private void TrayIcon_ExitRequested(object? sender, EventArgs args)
    {
        _exitRequested = true;
        Close();
    }

    public void ApplyTheme(AppThemeMode mode)
    {
        WindowRoot.RequestedTheme = mode switch
        {
            AppThemeMode.Light => ElementTheme.Light,
            AppThemeMode.Dark => ElementTheme.Dark,
            AppThemeMode.ExtremeDark => ElementTheme.Dark,
            _ => ElementTheme.Default,
        };

        var extreme = mode == AppThemeMode.ExtremeDark;
        SetExtremeBrush("VoicePageBrush", extreme ? Color.FromArgb(0xFF, 0x00, 0x00, 0x00) : null);
        SetExtremeBrush("VoiceSurfaceBrush", extreme ? Color.FromArgb(0xFF, 0x06, 0x06, 0x06) : null);
        SetExtremeBrush("VoiceElevatedBrush", extreme ? Color.FromArgb(0xFF, 0x10, 0x10, 0x10) : null);
        SetExtremeBrush("VoiceStrokeBrush", extreme ? Color.FromArgb(0x35, 0xFF, 0xFF, 0xFF) : null);
        SetExtremeBrush("VoiceTrackBrush", extreme ? Color.FromArgb(0x32, 0xFF, 0xFF, 0xFF) : null);
    }

    private void SetExtremeBrush(string key, Color? color)
    {
        if (color is Color value)
        {
            WindowRoot.Resources[key] = new SolidColorBrush(value);
        }
        else if (WindowRoot.Resources.ContainsKey(key))
        {
            WindowRoot.Resources.Remove(key);
        }
    }

    private void RootFrame_Loaded(object sender, RoutedEventArgs args)
    {
        RootFrame.Loaded -= RootFrame_Loaded;
        RootFrame.XamlRoot.Changed += XamlRoot_Changed;
        UpdateWindowMetrics(resizeWindow: true);
    }

    private void XamlRoot_Changed(XamlRoot sender, XamlRootChangedEventArgs args) =>
        UpdateWindowMetrics(resizeWindow: false);

    private void UpdateWindowMetrics(bool resizeWindow)
    {
        var windowHandle = WinRT.Interop.WindowNative.GetWindowHandle(this);
        var dpi = GetDpiForWindow(windowHandle);
        var scale = dpi > 0 ? dpi / 96.0 : RootFrame.XamlRoot?.RasterizationScale ?? 1.0;
        var displayArea = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Nearest);
        var workArea = displayArea.WorkArea;
        var minimumWidth = Math.Min(workArea.Width, (int)Math.Ceiling(MinimumLogicalWidth * scale));
        var minimumHeight = Math.Min(workArea.Height, (int)Math.Ceiling(MinimumLogicalHeight * scale));

        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.PreferredMinimumWidth = minimumWidth;
            presenter.PreferredMinimumHeight = minimumHeight;
        }
        if (!resizeWindow || _initialSizeApplied)
        {
            return;
        }

        const int workAreaMargin = 48;
        var width = Math.Min(
            Math.Max(1, workArea.Width - workAreaMargin),
            Math.Max(minimumWidth, (int)Math.Ceiling(InitialLogicalWidth * scale)));
        var height = Math.Min(
            Math.Max(1, workArea.Height - workAreaMargin),
            Math.Max(minimumHeight, (int)Math.Ceiling(InitialLogicalHeight * scale)));
        AppWindow.Resize(new SizeInt32(width, height));
        _initialSizeApplied = true;
    }

    [LibraryImport("user32.dll")]
    private static partial uint GetDpiForWindow(nint windowHandle);

    private void AppWindow_Closing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (!_exitRequested)
        {
            args.Cancel = true;
            MinimizeToTray();
            return;
        }

        DisposeTrayIcon();
        ((App)Application.Current).Host.Dispose();
    }

    private void DisposeTrayIcon()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _trayIcon.ShowRequested -= TrayIcon_ShowRequested;
        _trayIcon.HideRequested -= TrayIcon_HideRequested;
        _trayIcon.ExitRequested -= TrayIcon_ExitRequested;
        _trayIcon.Dispose();
    }
}
