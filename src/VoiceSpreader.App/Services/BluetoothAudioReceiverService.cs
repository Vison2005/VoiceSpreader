using VoiceSpreader.App.Models;
using Windows.Devices.Enumeration;
using Windows.Media.Audio;

namespace VoiceSpreader.App.Services;

public sealed class BluetoothAudioReceiverService : IDisposable
{
    private const string ParentAepIdProperty = "System.Devices.AepService.AepId";
    private const string ParentAepContainerIdProperty = "System.Devices.AepService.ContainerId";
    private const string DeviceContainerIdProperty = "System.Devices.ContainerId";

    private AudioPlaybackConnection? _connection;
    private string? _connectedDeviceId;
    private string? _connectedDeviceName;
    private bool _disposed;

    public event EventHandler<string>? StatusChanged;

    public event EventHandler<string>? ErrorOccurred;

    public event EventHandler<bool>? BusyChanged;

    public event EventHandler<bool>? ConnectionChanged;

    public bool IsConnected => _connection?.State == AudioPlaybackConnectionState.Opened;

    public string? ConnectedDeviceName => _connectedDeviceName;

    public async Task<IReadOnlyList<BluetoothAudioDevice>> GetDevicesAsync()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        BusyChanged?.Invoke(this, true);
        try
        {
            var properties = new[]
            {
                ParentAepIdProperty,
                ParentAepContainerIdProperty,
                DeviceContainerIdProperty,
            };
            var devices = await DeviceInformation.FindAllAsync(
                AudioPlaybackConnection.GetDeviceSelector(),
                properties);
            var result = new List<BluetoothAudioDevice>(devices.Count);
            foreach (var device in devices)
            {
                var name = await ResolveDeviceNameAsync(device);
                result.Add(new BluetoothAudioDevice(
                    device.Id,
                    string.IsNullOrWhiteSpace(name) ? "未命名蓝牙设备" : name));
            }

            StatusChanged?.Invoke(
                this,
                result.Count == 0
                    ? "未找到可接收音频的已配对手机。"
                    : $"发现 {result.Count} 台可用蓝牙音频设备。");
            return result;
        }
        catch (Exception exception)
        {
            var message = $"A2DP 接收功能不可用：{exception.Message}";
            ErrorOccurred?.Invoke(this, message);
            return [];
        }
        finally
        {
            BusyChanged?.Invoke(this, false);
        }
    }

    public async Task<bool> ConnectAsync(BluetoothAudioDevice device)
    {
        ArgumentNullException.ThrowIfNull(device);
        ObjectDisposedException.ThrowIf(_disposed, this);
        BusyChanged?.Invoke(this, true);
        try
        {
            Disconnect(notify: false);
            var connection = AudioPlaybackConnection.TryCreateFromId(device.Id);
            if (connection is null)
            {
                ErrorOccurred?.Invoke(this, "设备不再提供 A2DP 音频连接，请重新配对后再试。");
                return false;
            }

            _connectedDeviceId = device.Id;
            _connectedDeviceName = device.Name;
            _connection = connection;
            connection.StateChanged += Connection_StateChanged;
            await connection.StartAsync();
            var openResult = await connection.OpenAsync();
            if (openResult.Status != AudioPlaybackConnectionOpenResultStatus.Success)
            {
                var message = GetOpenFailureMessage(openResult.Status);
                Disconnect(notify: false);
                ErrorOccurred?.Invoke(this, message);
                return false;
            }

            ConnectionChanged?.Invoke(this, true);
            StatusChanged?.Invoke(
                this,
                $"已将 {device.Name} 的声音接收到 Windows；音频将从当前系统输出设备播放。");
            return true;
        }
        catch (Exception exception)
        {
            Disconnect(notify: false);
            ErrorOccurred?.Invoke(this, $"连接蓝牙音频失败：{exception.Message}");
            return false;
        }
        finally
        {
            BusyChanged?.Invoke(this, false);
        }
    }

    public void Disconnect() => Disconnect(notify: true);

    private void Disconnect(bool notify)
    {
        var previousName = _connectedDeviceName;
        if (_connection is not null)
        {
            _connection.StateChanged -= Connection_StateChanged;
            _connection.Dispose();
            _connection = null;
        }
        _connectedDeviceId = null;
        _connectedDeviceName = null;

        if (notify)
        {
            ConnectionChanged?.Invoke(this, false);
            StatusChanged?.Invoke(
                this,
                string.IsNullOrWhiteSpace(previousName)
                    ? "蓝牙音频接收已关闭。"
                    : $"已断开 {previousName} 的蓝牙音频。");
        }
    }

    private void Connection_StateChanged(AudioPlaybackConnection sender, object args)
    {
        if (sender.State == AudioPlaybackConnectionState.Opened || _connectedDeviceId is null)
        {
            return;
        }

        var previousName = _connectedDeviceName ?? "蓝牙设备";
        Disconnect(notify: false);
        ConnectionChanged?.Invoke(this, false);
        StatusChanged?.Invoke(this, $"{previousName} 已断开蓝牙音频连接。");
    }

    private static async Task<string> ResolveDeviceNameAsync(DeviceInformation service)
    {
        foreach (var (propertyName, kind) in new[]
                 {
                     (DeviceContainerIdProperty, DeviceInformationKind.DeviceContainer),
                     (ParentAepContainerIdProperty, DeviceInformationKind.AssociationEndpointContainer),
                 })
        {
            if (service.Properties.TryGetValue(propertyName, out var value) && value is Guid containerId)
            {
                try
                {
                    var container = await DeviceInformation.CreateFromIdAsync(
                        containerId.ToString("B"),
                        [],
                        kind);
                    if (!string.IsNullOrWhiteSpace(container?.Name))
                    {
                        return container.Name;
                    }
                }
                catch (Exception)
                {
                    // 部分驱动不允许读取父设备容器，继续尝试其他名称来源。
                }
            }
        }

        if (service.Properties.TryGetValue(ParentAepIdProperty, out var parentValue)
            && parentValue is string parentId
            && !string.IsNullOrWhiteSpace(parentId))
        {
            try
            {
                var parent = await DeviceInformation.CreateFromIdAsync(
                    parentId,
                    [],
                    DeviceInformationKind.AssociationEndpoint);
                if (!string.IsNullOrWhiteSpace(parent?.Name))
                {
                    return parent.Name;
                }
            }
            catch (Exception)
            {
                // 服务名称仍可作为稳定的回退显示名。
            }
        }

        return service.Name;
    }

    private static string GetOpenFailureMessage(AudioPlaybackConnectionOpenResultStatus status) => status switch
    {
        AudioPlaybackConnectionOpenResultStatus.RequestTimedOut =>
            "连接超时。请在手机的蓝牙音频输出列表中选择这台电脑后重试。",
        AudioPlaybackConnectionOpenResultStatus.DeniedBySystem =>
            "Windows 拒绝了蓝牙音频连接，请检查蓝牙权限和设备配对状态。",
        _ => "Windows 无法打开该手机的蓝牙音频连接。",
    };

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        Disconnect(notify: false);
        _disposed = true;
    }
}
