using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using VoiceSpreader.App.Models;
using VoiceSpreader.App.ViewModels;

namespace VoiceSpreader.App;

public sealed partial class MainPage : Page
{
    private bool _loaded;

    public MainPage()
    {
        ViewModel = new MainViewModel(((App)Application.Current).Host, DispatcherQueue);
        InitializeComponent();
    }

    public MainViewModel ViewModel { get; }

    private async void Page_Loaded(object sender, RoutedEventArgs args)
    {
        if (_loaded)
        {
            return;
        }

        _loaded = true;
        await ViewModel.RefreshDevicesAsync();
    }

    private void Page_Unloaded(object sender, RoutedEventArgs args) => ViewModel.Dispose();

    private void PageRoot_SizeChanged(object sender, SizeChangedEventArgs args)
    {
        var wide = args.NewSize.Width >= 1000;
        PrimaryColumn.Width = new GridLength(wide ? 1.6 : 1, GridUnitType.Star);
        SecondaryColumn.Width = wide ? new GridLength(1, GridUnitType.Star) : new GridLength(0);
        Grid.SetColumn(StatusColumn, wide ? 1 : 0);
        Grid.SetRow(StatusColumn, wide ? 0 : 1);
        WorkspaceGrid.ColumnSpacing = wide ? 16 : 0;
    }

    private async void RefreshButton_Click(object sender, RoutedEventArgs args) =>
        await ViewModel.RefreshDevicesAsync();

    private void OutputCheckBox_Click(object sender, RoutedEventArgs args)
    {
        if (sender is CheckBox { DataContext: OutputEndpointItem output } checkBox)
        {
            output.IsSelected = checkBox.IsChecked == true;
        }
    }

    private void OutputVolumeSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs args)
    {
        if (sender is Slider { DataContext: OutputEndpointItem output })
        {
            output.VolumePercent = (int)Math.Round(args.NewValue);
        }
    }

    [System.Diagnostics.CodeAnalysis.SuppressMessage(
        "Performance",
        "CA1822:Mark members as static",
        Justification = "WinUI XAML 事件处理器必须是页面实例方法。")]
    private void OutputDelayNumberBox_ValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs args)
    {
        if (sender.DataContext is OutputEndpointItem output && !double.IsNaN(args.NewValue))
        {
            output.DelayMilliseconds = (int)Math.Round(args.NewValue);
        }
    }

    private void BufferNumberBox_ValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs args)
    {
        if (!double.IsNaN(args.NewValue))
        {
            ViewModel.BufferMilliseconds = (int)Math.Round(args.NewValue);
        }
    }

    private async void StartButton_Click(object sender, RoutedEventArgs args)
    {
        var error = ViewModel.StartOrStop();
        if (error is not null)
        {
            await ShowValidationAsync("无法开始同步", error);
        }
    }

    private async void CalibrateButton_Click(object sender, RoutedEventArgs args)
    {
        var error = ViewModel.StartOrStopCalibration();
        if (error is not null)
        {
            await ShowValidationAsync("无法开始校准", error);
        }
    }

    private async Task ShowValidationAsync(string title, string message)
    {
        var dialog = new ContentDialog
        {
            Title = title,
            Content = message,
            CloseButtonText = "知道了",
            XamlRoot = XamlRoot,
        };
        await dialog.ShowAsync();
    }
}
