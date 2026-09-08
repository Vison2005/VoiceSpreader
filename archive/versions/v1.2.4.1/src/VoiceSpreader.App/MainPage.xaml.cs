using System.Globalization;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using VoiceSpreader.App.Models;
using VoiceSpreader.App.Services;
using VoiceSpreader.App.ViewModels;
using Windows.System;
using Windows.Storage.Pickers;

namespace VoiceSpreader.App;

public sealed partial class MainPage : Page
{
    private const double OutputRowHeight = 68;
    private const double DefaultOutputListMaximumHeight = OutputRowHeight * 5;
    private const double ModerateColumnOverflow = 96;
    private bool _loaded;
    private bool _wideLayout;
    private bool _adaptiveLayoutQueued;

    public MainPage()
    {
        ViewModel = new MainViewModel(((App)Application.Current).Host, DispatcherQueue);
        InitializeComponent();
        ThemeSelector.SelectedItem = ThemeSelector.Items[(int)ViewModel.ThemeMode];
        WorkspaceSelector.SelectedItem = WorkspaceSelector.Items[0];
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
        await UpdatePhoneQrCodeAsync();
    }

    private void Page_Unloaded(object sender, RoutedEventArgs args) => ViewModel.Dispose();

    private void PageRoot_SizeChanged(object sender, SizeChangedEventArgs args)
    {
        var wide = args.NewSize.Width >= 960;
        _wideLayout = wide;
        PrimaryColumn.Width = new GridLength(1, GridUnitType.Star);
        SecondaryColumn.Width = wide ? new GridLength(340) : new GridLength(0);
        DeviceLinkPrimaryColumn.Width = new GridLength(1, GridUnitType.Star);
        DeviceLinkSecondaryColumn.Width = wide ? new GridLength(340) : new GridLength(0);
        Grid.SetColumn(StatusColumn, wide ? 1 : 0);
        Grid.SetRow(StatusColumn, wide ? 0 : 1);
        Grid.SetColumn(DeviceLinkStatusColumn, wide ? 1 : 0);
        Grid.SetRow(DeviceLinkStatusColumn, wide ? 0 : 1);
        Grid.SetColumnSpan(DeviceLinkRouteCard, wide ? 2 : 1);
        Grid.SetRow(DeviceLinkRouteCard, wide ? 1 : 2);
        Grid.SetColumnSpan(DeviceLinkFeatureCard, wide ? 2 : 1);
        Grid.SetRow(DeviceLinkFeatureCard, wide ? 2 : 3);
        var routesWide = args.NewSize.Width >= 780;
        PhoneMicrophoneRouteColumn.Width = new GridLength(1, GridUnitType.Star);
        PhoneRouteSeparatorColumn.Width = routesWide ? new GridLength(1) : new GridLength(0);
        PhoneSpeakerRouteColumn.Width = routesWide ? new GridLength(1, GridUnitType.Star) : new GridLength(0);
        Grid.SetColumn(PhoneMicrophoneRoutePanel, 0);
        Grid.SetRow(PhoneMicrophoneRoutePanel, 0);
        Grid.SetColumnSpan(PhoneMicrophoneRoutePanel, routesWide ? 1 : 3);
        Grid.SetColumn(PhoneRouteSeparator, routesWide ? 1 : 0);
        Grid.SetRow(PhoneRouteSeparator, routesWide ? 0 : 1);
        Grid.SetColumnSpan(PhoneRouteSeparator, routesWide ? 1 : 3);
        PhoneRouteSeparator.Height = routesWide ? double.NaN : 1;
        Grid.SetColumn(PhoneSpeakerRoutePanel, routesWide ? 2 : 0);
        Grid.SetRow(PhoneSpeakerRoutePanel, routesWide ? 0 : 2);
        Grid.SetColumnSpan(PhoneSpeakerRoutePanel, routesWide ? 1 : 3);
        WorkspaceGrid.ColumnSpacing = wide ? 16 : 0;
        WorkspaceGrid.RowSpacing = wide ? 0 : 16;
        DeviceLinkWorkspace.ColumnSpacing = wide ? 16 : 0;
        DeviceLinkWorkspace.RowSpacing = 16;
        QueueAdaptiveWorkspaceLayout();
    }

    private void AdaptiveColumn_SizeChanged(object sender, SizeChangedEventArgs args) =>
        QueueAdaptiveWorkspaceLayout();

    private void QueueAdaptiveWorkspaceLayout()
    {
        if (_adaptiveLayoutQueued)
        {
            return;
        }

        _adaptiveLayoutQueued = true;
        DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () =>
        {
            _adaptiveLayoutQueued = false;
            UpdateAdaptiveWorkspaceLayout();
        });
    }

    private void UpdateAdaptiveWorkspaceLayout()
    {
        if (WorkspaceGrid.Visibility != Visibility.Visible)
        {
            return;
        }
        if (!_wideLayout)
        {
            SetOutputListMaximumHeight(DefaultOutputListMaximumHeight);
            return;
        }
        if (ViewModel.Outputs.Count == 0)
        {
            SetOutputListMaximumHeight(DefaultOutputListMaximumHeight);
            return;
        }
        if (AudioSourceCard.DesiredSize.Height <= 0
            || OutputDevicesCard.DesiredSize.Height <= 0
            || SyncStatusCard.DesiredSize.Height <= 0
            || SyncControlCard.DesiredSize.Height <= 0
            || OutputDeviceList.DesiredSize.Height <= 0)
        {
            return;
        }

        var outputCardChromeHeight = Math.Max(
            0,
            OutputDevicesCard.DesiredSize.Height - OutputDeviceList.DesiredSize.Height);
        var fixedPrimaryHeight = AudioSourceCard.DesiredSize.Height
                                 + 16
                                 + outputCardChromeHeight;
        var naturalStatusHeight = SyncStatusCard.DesiredSize.Height
                                  + 16
                                  + SyncControlCard.DesiredSize.Height;
        var naturalListHeight = ViewModel.Outputs.Count * OutputRowHeight;
        var naturalPrimaryHeight = fixedPrimaryHeight + naturalListHeight;

        if (naturalPrimaryHeight - naturalStatusHeight <= ModerateColumnOverflow)
        {
            SetOutputListMaximumHeight(naturalListHeight);
            return;
        }

        var availableListHeight = Math.Max(
            OutputRowHeight * 3,
            naturalStatusHeight - fixedPrimaryHeight);
        var completeVisibleRows = Math.Max(3, Math.Floor(availableListHeight / OutputRowHeight));
        SetOutputListMaximumHeight(Math.Min(
            naturalListHeight,
            completeVisibleRows * OutputRowHeight));
    }

    private void SetOutputListMaximumHeight(double height)
    {
        var normalizedHeight = Math.Max(0, height);
        if (Math.Abs(OutputDeviceList.MaxHeight - normalizedHeight) >= 0.5)
        {
            OutputDeviceList.MaxHeight = normalizedHeight;
        }
    }

    private void PageRoot_Tapped(object sender, TappedRoutedEventArgs args)
    {
        if (args.OriginalSource is DependencyObject source
            && FindAncestor<NumberBox>(source) is not null)
        {
            return;
        }

        var focusedElement = FocusManager.GetFocusedElement(XamlRoot) as DependencyObject;
        var focusedNumberBox = FindAncestor<NumberBox>(focusedElement);
        if (focusedNumberBox is null)
        {
            return;
        }

        CommitNumberBox(focusedNumberBox);
        EndNumberBoxEditing();
    }

    private async void RefreshButton_Click(object sender, RoutedEventArgs args) =>
        await ViewModel.RefreshDevicesAsync();

    private async void BluetoothFlyout_Opening(object sender, object args)
    {
        if (ViewModel.BluetoothDevices.Count == 0 && !ViewModel.IsBluetoothBusy)
        {
            await ViewModel.RefreshBluetoothDevicesAsync();
        }
    }

    private async void RefreshBluetoothButton_Click(object sender, RoutedEventArgs args) =>
        await ViewModel.RefreshBluetoothDevicesAsync();

    [System.Diagnostics.CodeAnalysis.SuppressMessage(
        "Performance",
        "CA1822:Mark members as static",
        Justification = "WinUI XAML 事件处理器必须是页面实例方法。")]
    private async void PairBluetoothButton_Click(object sender, RoutedEventArgs args) =>
        await Windows.System.Launcher.LaunchUriAsync(new Uri("ms-settings:bluetooth"));

    private async void BluetoothActionButton_Click(object sender, RoutedEventArgs args)
    {
        var error = await ViewModel.ConnectOrDisconnectBluetoothAsync();
        if (error is not null)
        {
            await ShowValidationAsync("蓝牙音频连接失败", error);
        }
    }

    private void OpenDeviceLinkButton_Click(object sender, RoutedEventArgs args) =>
        WorkspaceSelector.SelectedItem = WorkspaceSelector.Items[1];

    private async void PhoneFlyout_Opening(object sender, object args) =>
        await UpdatePhoneQrCodeAsync();

    private async void PhoneAddress_SelectionChanged(object sender, SelectionChangedEventArgs args) =>
        await UpdatePhoneQrCodeAsync();

    private async void ResetPhonePairingButton_Click(object sender, RoutedEventArgs args)
    {
        ViewModel.ResetPhonePairing();
        await UpdatePhoneQrCodeAsync();
    }

    private void DisconnectPhoneButton_Click(object sender, RoutedEventArgs args) =>
        ViewModel.DisconnectPhone();

    private async Task UpdatePhoneQrCodeAsync()
    {
        if (!string.IsNullOrWhiteSpace(ViewModel.PhonePairingPayload))
        {
            var source = await QrCodeService.CreateImageAsync(ViewModel.PhonePairingPayload);
            PhoneQrImage.Source = source;
            DeviceLinkQrImage.Source = source;
        }
    }

    private void DisconnectPhoneDeviceButton_Click(object sender, RoutedEventArgs args)
    {
        if (sender is Button { DataContext: PhoneDeviceItem device })
        {
            ViewModel.DisconnectPhoneDevice(device.Id);
        }
    }

    private async void ReconnectSavedPhoneDeviceButton_Click(object sender, RoutedEventArgs args)
    {
        if (sender is not Button { DataContext: SavedPhoneDevice device })
        {
            return;
        }

        var error = await ViewModel.ReconnectSavedPhoneDeviceAsync(device.Id);
        if (error is not null)
        {
            await ShowValidationAsync("无法发起手机连接", error);
        }
    }

    private void ForgetSavedPhoneDeviceButton_Click(object sender, RoutedEventArgs args)
    {
        if (sender is Button { DataContext: SavedPhoneDevice device })
        {
            ViewModel.ForgetSavedPhoneDevice(device.Id);
        }
    }

    private async void RequestPhoneScanButton_Click(object sender, RoutedEventArgs args) =>
        await ViewModel.RequestPhoneScanAsync("camera");

    private async void RequestPhoneGalleryScanButton_Click(object sender, RoutedEventArgs args) =>
        await ViewModel.RequestPhoneScanAsync("gallery");

    private async void RequestPhoneCaptureButton_Click(object sender, RoutedEventArgs args) =>
        await ViewModel.RequestPhoneCaptureAsync();

    private void OpenPhoneTouchpadButton_Click(object sender, RoutedEventArgs args)
    {
        ViewModel.ShowFeatureStatus("请在 Android 端打开“触摸板”模式；手机产生的指针和滚轮事件会实时注入电脑。" );
    }

    private async void AddComputerShortcutButton_Click(object sender, RoutedEventArgs args)
    {
        var picker = new FileOpenPicker();
        picker.FileTypeFilter.Add(".exe");
        var window = ((App)Application.Current).MainWindow;
        if (window is null)
        {
            return;
        }

        WinRT.Interop.InitializeWithWindow.Initialize(
            picker,
            WinRT.Interop.WindowNative.GetWindowHandle(window));
        var file = await picker.PickSingleFileAsync();
        if (file is null)
        {
            return;
        }

        var error = await ViewModel.AddComputerShortcutAsync(file.Path);
        if (error is not null)
        {
            await ShowValidationAsync("无法添加电脑应用", error);
        }
    }

    private async void RemoveComputerShortcutButton_Click(object sender, RoutedEventArgs args)
    {
        if (sender is not Button { DataContext: ComputerShortcut shortcut })
        {
            return;
        }

        await ViewModel.RemoveComputerShortcutAsync(shortcut);
    }

    private async void PhoneMicrophoneRouteButton_Click(object sender, RoutedEventArgs args)
    {
        var error = await ViewModel.TogglePhoneMicrophoneRouteAsync();
        if (error is not null)
        {
            await ShowValidationAsync("无法建立手机麦克风路由", error);
        }
    }

    private async void PhonePlaybackButton_Click(object sender, RoutedEventArgs args)
    {
        var error = await ViewModel.TogglePhonePlaybackAsync();
        if (error is not null)
        {
            await ShowValidationAsync("无法播放 Windows 声音", error);
        }
    }

    [System.Diagnostics.CodeAnalysis.SuppressMessage(
        "Performance",
        "CA1822:Mark members as static",
        Justification = "WinUI XAML 事件处理器必须是页面实例方法。")]
    private async void OpenVirtualCableWebsiteButton_Click(object sender, RoutedEventArgs args) =>
        await Launcher.LaunchUriAsync(new Uri("https://vb-audio.com/Cable/"));

    private async void WorkspaceSelector_SelectionChanged(
        SelectorBar sender,
        SelectorBarSelectionChangedEventArgs args)
    {
        var deviceLinkSelected = sender.Items.IndexOf(sender.SelectedItem) == 1;
        WorkspaceGrid.Visibility = deviceLinkSelected ? Visibility.Collapsed : Visibility.Visible;
        DeviceLinkWorkspace.Visibility = deviceLinkSelected ? Visibility.Visible : Visibility.Collapsed;
        SyncFooterStatus.Visibility = deviceLinkSelected ? Visibility.Collapsed : Visibility.Visible;
        DeviceLinkFooterStatus.Visibility = deviceLinkSelected ? Visibility.Visible : Visibility.Collapsed;
        CalibrationActionButton.Visibility = deviceLinkSelected ? Visibility.Collapsed : Visibility.Visible;
        StartActionButton.Visibility = deviceLinkSelected ? Visibility.Collapsed : Visibility.Visible;
        WorkspaceTitle.Text = deviceLinkSelected ? "设备互联" : "同步控制台";
        WorkspaceSubtitle.Text = deviceLinkSelected
            ? "在 Windows 与 Android 之间建立独立音频链路"
            : "将系统声音同步到多个 Windows 音频设备";
        if (deviceLinkSelected)
        {
            await UpdatePhoneQrCodeAsync();
        }
        else
        {
            QueueAdaptiveWorkspaceLayout();
        }
    }

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

    private void OutputDelayNumberBox_KeyDown(object sender, KeyRoutedEventArgs args)
    {
        if (args.Key != VirtualKey.Enter || sender is not NumberBox numberBox)
        {
            return;
        }

        CommitOutputDelay(numberBox);
        EndNumberBoxEditing();
        args.Handled = true;
    }

    private void OutputDelayNumberBox_LostFocus(object sender, RoutedEventArgs args)
    {
        if (sender is NumberBox numberBox)
        {
            CommitOutputDelay(numberBox);
        }
    }

    private void OutputDelayNumberBox_PointerWheelChanged(
        object sender,
        PointerRoutedEventArgs args)
    {
        if (sender is not NumberBox { IsEnabled: true } numberBox
            || numberBox.DataContext is not OutputEndpointItem output)
        {
            return;
        }

        var wheelDelta = args.GetCurrentPoint(numberBox).Properties.MouseWheelDelta;
        if (wheelDelta == 0)
        {
            return;
        }

        var currentValue = TryReadNumber(numberBox, out var enteredValue)
            ? (int)Math.Round(enteredValue)
            : output.DelayMilliseconds;
        var step = (args.KeyModifiers & VirtualKeyModifiers.Control) != 0 ? 10 : 1;
        var notchCount = Math.Max(1, Math.Abs(wheelDelta) / 120);
        var nextValue = Math.Clamp(
            currentValue + Math.Sign(wheelDelta) * step * notchCount,
            -500,
            500);

        numberBox.Value = nextValue;
        args.Handled = true;
    }

    private void BufferNumberBox_ValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs args)
    {
        if (!double.IsNaN(args.NewValue))
        {
            ViewModel.BufferMilliseconds = (int)Math.Round(args.NewValue);
        }
    }

    private void BufferNumberBox_KeyDown(object sender, KeyRoutedEventArgs args)
    {
        if (args.Key != VirtualKey.Enter || sender is not NumberBox numberBox)
        {
            return;
        }

        CommitBufferDelay(numberBox);
        EndNumberBoxEditing();
        args.Handled = true;
    }

    private void BufferNumberBox_LostFocus(object sender, RoutedEventArgs args)
    {
        if (sender is NumberBox numberBox)
        {
            CommitBufferDelay(numberBox);
        }
    }

    private void BufferNumberBox_PointerWheelChanged(
        object sender,
        PointerRoutedEventArgs args)
    {
        if (sender is not NumberBox { IsEnabled: true } numberBox)
        {
            return;
        }

        var wheelDelta = args.GetCurrentPoint(numberBox).Properties.MouseWheelDelta;
        if (wheelDelta == 0)
        {
            return;
        }

        var currentValue = TryReadNumber(numberBox, out var enteredValue)
            ? (int)Math.Round(enteredValue)
            : ViewModel.BufferMilliseconds;
        var step = (args.KeyModifiers & VirtualKeyModifiers.Control) != 0 ? 10 : 1;
        var notchCount = Math.Max(1, Math.Abs(wheelDelta) / 120);
        var nextValue = Math.Clamp(
            currentValue + Math.Sign(wheelDelta) * step * notchCount,
            2,
            100);

        numberBox.Value = nextValue;
        args.Handled = true;
    }

    private void CommitNumberBox(NumberBox numberBox)
    {
        if (numberBox.DataContext is OutputEndpointItem)
        {
            CommitOutputDelay(numberBox);
        }
        else if (ReferenceEquals(numberBox, BufferNumberBox))
        {
            CommitBufferDelay(numberBox);
        }
    }

    private static void CommitOutputDelay(NumberBox numberBox)
    {
        if (numberBox.DataContext is not OutputEndpointItem output
            || !TryReadNumber(numberBox, out var value))
        {
            return;
        }

        var delay = (int)Math.Round(Math.Clamp(value, -500, 500));
        numberBox.Value = delay;
        output.DelayMilliseconds = delay;
    }

    private void CommitBufferDelay(NumberBox numberBox)
    {
        if (!TryReadNumber(numberBox, out var value))
        {
            return;
        }

        var delay = (int)Math.Round(Math.Clamp(value, 2, 100));
        numberBox.Value = delay;
        ViewModel.BufferMilliseconds = delay;
    }

    private void EndNumberBoxEditing()
    {
        if (!PageScrollViewer.Focus(FocusState.Programmatic))
        {
            StartActionButton.Focus(FocusState.Programmatic);
        }
    }

    private static bool TryReadNumber(NumberBox numberBox, out double value) =>
        double.TryParse(
            numberBox.Text,
            NumberStyles.Float | NumberStyles.AllowThousands,
            CultureInfo.CurrentCulture,
            out value)
        || double.TryParse(
            numberBox.Text,
            NumberStyles.Float | NumberStyles.AllowThousands,
            CultureInfo.InvariantCulture,
            out value)
        || (!double.IsNaN(numberBox.Value) && AssignValue(numberBox.Value, out value));

    private static bool AssignValue(double source, out double value)
    {
        value = source;
        return true;
    }

    private static T? FindAncestor<T>(DependencyObject? current) where T : DependencyObject
    {
        while (current is not null)
        {
            if (current is T match)
            {
                return match;
            }
            current = VisualTreeHelper.GetParent(current);
        }
        return null;
    }

    private void ThemeSelector_SelectionChanged(SelectorBar sender, SelectorBarSelectionChangedEventArgs args)
    {
        var selectedIndex = sender.Items.IndexOf(sender.SelectedItem);
        if (selectedIndex < 0)
        {
            return;
        }

        ViewModel.ThemeMode = (AppThemeMode)selectedIndex;
        ((App)Application.Current).ApplyTheme(ViewModel.ThemeMode);
    }

    private void MinimizeToTrayButton_Click(object sender, RoutedEventArgs args) =>
        ((App)Application.Current).MinimizeToTray();

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
