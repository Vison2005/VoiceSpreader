using System.Runtime.InteropServices;
using System.Security;
using Microsoft.Win32;
using Windows.ApplicationModel;

namespace VoiceSpreader.App.Services;

public static class AutoStartService
{
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "VoiceSpreader";
    private const string StartupTaskId = "VoiceSpreaderStartup";

    public static bool IsEnabled => IsLegacyRegistryEnabled();

    public static async Task<bool> GetEnabledAsync()
    {
        var startupTask = await TryGetStartupTaskAsync();
        if (startupTask is null)
        {
            // 未打包运行时没有 StartupTask，修复旧注册表项指向当前可执行文件。
            if (IsLegacyRegistryEnabled())
            {
                TrySetRegistryEnabled(true);
            }

            return IsLegacyRegistryEnabled();
        }

        if (IsLegacyRegistryEnabled())
        {
            // 1.2.4.71 及更早版本使用了 Run 注册表项。用户已明确开启时，迁移到包启动任务。
            try
            {
                var state = startupTask.State;
                if (state == StartupTaskState.Disabled)
                {
                    state = await startupTask.RequestEnableAsync();
                }

                if (state is StartupTaskState.Enabled
                    or StartupTaskState.EnabledByPolicy
                    or StartupTaskState.Disabled
                    or StartupTaskState.DisabledByUser
                    or StartupTaskState.DisabledByPolicy)
                {
                    TryRemoveRegistryEntry();
                }
            }
            catch (Exception exception) when (IsExpectedStartupTaskFailure(exception))
            {
                // 包启动任务不可用时保留旧项，避免升级过程中丢失用户设置。
            }
        }

        return IsStartupTaskEnabled(startupTask.State);
    }

    public static async Task SetEnabledAsync(bool enabled)
    {
        var startupTask = await TryGetStartupTaskAsync();
        if (startupTask is not null)
        {
            if (!enabled)
            {
                startupTask.Disable();
                TryRemoveRegistryEntry();
                return;
            }

            var state = startupTask.State;
            if (state == StartupTaskState.DisabledByUser)
            {
                throw new InvalidOperationException("Windows 已在任务管理器中禁止此启动项，请手动重新启用后再试。");
            }

            state = await startupTask.RequestEnableAsync();
            if (!IsStartupTaskEnabled(state))
            {
                throw new InvalidOperationException("Windows 未接受开机自启动请求，请检查任务管理器中的启动项设置。");
            }

            TryRemoveRegistryEntry();
            return;
        }

        SetRegistryEnabled(enabled);
    }

    private static async Task<StartupTask?> TryGetStartupTaskAsync()
    {
        try
        {
            return await StartupTask.GetAsync(StartupTaskId);
        }
        catch (Exception exception) when (IsExpectedStartupTaskFailure(exception))
        {
            // 直接运行 bin 输出或旧包时没有 StartupTask，使用注册表回退。
            return null;
        }
    }

    private static bool IsExpectedStartupTaskFailure(Exception exception) =>
        exception is COMException
            or InvalidOperationException
            or FileNotFoundException
            or UnauthorizedAccessException;

    private static bool IsStartupTaskEnabled(StartupTaskState state) =>
        state is StartupTaskState.Enabled or StartupTaskState.EnabledByPolicy;

    private static bool IsLegacyRegistryEnabled()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath, writable: false);
            return key?.GetValue(ValueName) is string value
                   && !string.IsNullOrWhiteSpace(value);
        }
        catch (Exception exception) when (IsExpectedRegistryFailure(exception))
        {
            return false;
        }
    }

    private static void SetRegistryEnabled(bool enabled)
    {
        using var key = Registry.CurrentUser.CreateSubKey(RunKeyPath, writable: true)
                        ?? throw new InvalidOperationException("无法打开当前用户的开机启动设置。");
        if (!enabled)
        {
            key.DeleteValue(ValueName, throwOnMissingValue: false);
            return;
        }

        var executable = Environment.ProcessPath;
        if (string.IsNullOrWhiteSpace(executable))
        {
            throw new InvalidOperationException("无法确定当前程序路径。");
        }

        key.SetValue(
            ValueName,
            $"\"{Path.GetFullPath(executable)}\" --background",
            RegistryValueKind.String);
    }

    private static void TrySetRegistryEnabled(bool enabled)
    {
        try
        {
            SetRegistryEnabled(enabled);
        }
        catch (Exception exception) when (IsExpectedRegistryFailure(exception))
        {
            // 状态读取不能因为注册表修复失败而阻断应用启动。
        }
    }

    private static void TryRemoveRegistryEntry()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath, writable: true);
            key?.DeleteValue(ValueName, throwOnMissingValue: false);
        }
        catch (Exception exception) when (IsExpectedRegistryFailure(exception))
        {
            // 包启动任务已生效，旧注册表项清理失败不影响启动。
        }
    }

    private static bool IsExpectedRegistryFailure(Exception exception) =>
        exception is IOException
            or UnauthorizedAccessException
            or SecurityException;
}
