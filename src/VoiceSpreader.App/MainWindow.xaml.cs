using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using System.Runtime.InteropServices;
using Windows.Graphics;

namespace VoiceSpreader.App;

public sealed partial class MainWindow : Window
{
    private const int InitialLogicalWidth = 1160;
    private const int InitialLogicalHeight = 800;
    private const int MinimumLogicalWidth = 760;
    private const int MinimumLogicalHeight = 680;
    private bool _initialSizeApplied;

    public MainWindow()
    {
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        AppWindow.SetIcon("Assets/AppIcon.ico");
        AppWindow.Closing += AppWindow_Closing;
        RootFrame.Navigate(typeof(MainPage));
        RootFrame.Loaded += RootFrame_Loaded;
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
        ((App)Application.Current).Host.Dispose();
    }
}
