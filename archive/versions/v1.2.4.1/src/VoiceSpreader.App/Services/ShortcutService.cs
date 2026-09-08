using System.Globalization;
using System.Diagnostics;
using System.Text.Json;
using VoiceSpreader.App.Models;

namespace VoiceSpreader.App.Services;

/// <summary>
/// 校验并执行手机端发来的有限快捷键组合。
/// </summary>
public sealed class ShortcutService
{
    private static readonly Dictionary<string, ushort> KeyMap =
        BuildKeyMap();
    private static readonly Dictionary<string, ushort> ModifierMap =
        new Dictionary<string, ushort>(StringComparer.OrdinalIgnoreCase)
        {
            ["CTRL"] = 0x11,
            ["CONTROL"] = 0x11,
            ["SHIFT"] = 0x10,
            ["ALT"] = 0x12,
            ["WIN"] = 0x5B,
            ["META"] = 0x5B,
        };

    private readonly InputBridgeService _input;

    public ShortcutService(InputBridgeService input)
    {
        _input = input;
    }

    public bool TryExecuteJson(string json, out string error)
        => TryExecuteJson(json, null, out error);

    public bool TryExecuteJson(
        string json,
        Func<string, ComputerShortcut?>? appResolver,
        out string error)
    {
        error = string.Empty;
        try
        {
            using var document = JsonDocument.Parse(json);
            var root = document.RootElement;
            var action = root.TryGetProperty("action", out var actionElement)
                ? actionElement.GetString()?.Trim()
                : null;
            if (string.Equals(action, "launchApp", StringComparison.OrdinalIgnoreCase))
            {
                return TryLaunchApp(root, appResolver, out error);
            }
            if (!string.IsNullOrWhiteSpace(action)
                && !string.Equals(action, "shortcut", StringComparison.OrdinalIgnoreCase))
            {
                error = "不支持的快捷操作";
                return false;
            }
            var keyName = root.TryGetProperty("key", out var key)
                ? key.GetString()?.Trim()
                : null;
            if (string.IsNullOrWhiteSpace(keyName)
                || !KeyMap.TryGetValue(keyName, out var virtualKey))
            {
                error = "快捷键不在允许列表中";
                return false;
            }

            var repeat = root.TryGetProperty("repeat", out var repeatElement)
                && repeatElement.TryGetInt32(out var parsedRepeat)
                ? parsedRepeat
                : 1;
            if (repeat is < 1 or > 3)
            {
                error = "快捷键重复次数无效";
                return false;
            }

            var modifiers = new List<ushort>();
            if (root.TryGetProperty("modifiers", out var modifierElement)
                && modifierElement.ValueKind == JsonValueKind.Array)
            {
                foreach (var value in modifierElement.EnumerateArray())
                {
                    var name = value.GetString()?.Trim();
                    if (string.IsNullOrWhiteSpace(name)
                        || !ModifierMap.TryGetValue(name, out var modifier)
                        || modifiers.Contains(modifier))
                    {
                        error = "快捷键修饰键无效";
                        return false;
                    }
                    modifiers.Add(modifier);
                    if (modifiers.Count > 4)
                    {
                        error = "快捷键修饰键过多";
                        return false;
                    }
                }
            }

            for (var index = 0; index < repeat; index++)
            {
                foreach (var modifier in modifiers)
                {
                    _input.KeyDown(modifier);
                }
                _input.KeyDown(virtualKey);
                _input.KeyUp(virtualKey);
                for (var modifierIndex = modifiers.Count - 1; modifierIndex >= 0; modifierIndex--)
                {
                    _input.KeyUp(modifiers[modifierIndex]);
                }
            }
            return true;
        }
        catch (JsonException)
        {
            error = "快捷键数据格式无效";
            return false;
        }
    }

    private static bool TryLaunchApp(
        JsonElement root,
        Func<string, ComputerShortcut?>? appResolver,
        out string error)
    {
        error = string.Empty;
        var appId = root.TryGetProperty("appId", out var appIdElement)
            ? appIdElement.GetString()?.Trim()
            : null;
        if (string.IsNullOrWhiteSpace(appId) || appResolver is null)
        {
            error = "电脑端尚未配置这个应用";
            return false;
        }

        var app = appResolver(appId);
        if (app is null)
        {
            error = "找不到已配置的应用";
            return false;
        }
        if (!Path.IsPathFullyQualified(app.ExecutablePath)
            || !File.Exists(app.ExecutablePath))
        {
            error = $"应用路径不存在：{app.ExecutablePath}";
            return false;
        }

        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = app.ExecutablePath,
                Arguments = app.Arguments ?? string.Empty,
                WorkingDirectory = Path.GetDirectoryName(app.ExecutablePath) ?? string.Empty,
                UseShellExecute = true,
            });
            return true;
        }
        catch (Exception exception) when (exception is InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            error = $"启动应用失败：{exception.Message}";
            return false;
        }
    }

    private static Dictionary<string, ushort> BuildKeyMap()
    {
        var map = new Dictionary<string, ushort>(StringComparer.OrdinalIgnoreCase)
        {
            ["TAB"] = 0x09,
            ["ENTER"] = 0x0D,
            ["ESC"] = 0x1B,
            ["ESCAPE"] = 0x1B,
            ["SPACE"] = 0x20,
            ["BACKSPACE"] = 0x08,
            ["DELETE"] = 0x2E,
            ["INSERT"] = 0x2D,
            ["HOME"] = 0x24,
            ["END"] = 0x23,
            ["PAGEUP"] = 0x21,
            ["PAGEDOWN"] = 0x22,
            ["LEFT"] = 0x25,
            ["UP"] = 0x26,
            ["RIGHT"] = 0x27,
            ["DOWN"] = 0x28,
            ["PRINTSCREEN"] = 0x2C,
            ["MEDIA_PLAY_PAUSE"] = 0xB3,
            ["MEDIA_NEXT"] = 0xB0,
            ["MEDIA_PREVIOUS"] = 0xB1,
            ["VOLUME_MUTE"] = 0xAD,
            ["VOLUME_DOWN"] = 0xAE,
            ["VOLUME_UP"] = 0xAF,
        };
        for (var index = 0; index < 26; index++)
        {
            map[((char)('A' + index)).ToString()] = (ushort)('A' + index);
        }
        for (var index = 0; index < 10; index++)
        {
            map[index.ToString(CultureInfo.InvariantCulture)] = (ushort)(0x30 + index);
        }
        for (var index = 1; index <= 24; index++)
        {
            map[$"F{index}"] = (ushort)(0x70 + index - 1);
        }
        return map;
    }
}
