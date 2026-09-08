using System.Runtime.InteropServices;

namespace VoiceSpreader.App.Services;

/// <summary>
/// 将手机端规范化的输入事件转换为 Windows 鼠标/键盘事件。
/// </summary>
public sealed class InputBridgeService : IDisposable
{
    private const uint InputMouse = 0;
    private const uint InputKeyboard = 1;
    private const uint MouseMove = 0x0001;
    private const uint MouseLeftDown = 0x0002;
    private const uint MouseLeftUp = 0x0004;
    private const uint MouseRightDown = 0x0008;
    private const uint MouseRightUp = 0x0010;
    private const uint MouseMiddleDown = 0x0020;
    private const uint MouseMiddleUp = 0x0040;
    private const uint MouseXDown = 0x0080;
    private const uint MouseXUp = 0x0100;
    private const uint MouseWheel = 0x0800;
    private const uint MouseHWheel = 0x01000;
    private const uint KeyboardKeyUp = 0x0002;
    private const uint KeyScancode = 0x0008;
    private readonly object _gate = new();
    private readonly HashSet<ushort> _pressedKeys = [];
    private readonly HashSet<byte> _pressedButtons = [];
    private bool _disposed;

    public bool Move(double deltaX, double deltaY)
    {
        if (_disposed)
        {
            return false;
        }

        var input = CreateMouseInput(
            (int)Math.Clamp(Math.Round(deltaX), int.MinValue, int.MaxValue),
            (int)Math.Clamp(Math.Round(deltaY), int.MinValue, int.MaxValue),
            0,
            MouseMove);
        return TrySend([input]) == 1;
    }

    public bool Scroll(double deltaX, double deltaY)
    {
        if (_disposed)
        {
            return false;
        }

        var inputs = new List<INPUT>(2);
        if (Math.Abs(deltaY) >= 0.001)
        {
            inputs.Add(CreateMouseInput(0, 0, ToWheelData(deltaY), MouseWheel));
        }
        if (Math.Abs(deltaX) >= 0.001)
        {
            inputs.Add(CreateMouseInput(0, 0, ToWheelData(deltaX), MouseHWheel));
        }
        return inputs.Count == 0 || TrySend(inputs.ToArray()) == (uint)inputs.Count;
    }

    public bool SetMouseButton(byte button, bool pressed)
    {
        if (_disposed || !TryGetMouseButtonFlags(button, pressed, out var flags, out var data))
        {
            return false;
        }

        lock (_gate)
        {
            if (pressed)
            {
                _pressedButtons.Add(button);
            }
            else
            {
                _pressedButtons.Remove(button);
            }
        }
        return TrySend([CreateMouseInput(0, 0, data, flags)]) == 1;
    }

    public bool KeyDown(ushort virtualKey)
    {
        if (_disposed || virtualKey == 0)
        {
            return false;
        }

        lock (_gate)
        {
            _pressedKeys.Add(virtualKey);
        }
        return TrySend([CreateKeyboardInput(virtualKey, 0)]) == 1;
    }

    public bool KeyUp(ushort virtualKey)
    {
        if (_disposed || virtualKey == 0)
        {
            return false;
        }

        lock (_gate)
        {
            _pressedKeys.Remove(virtualKey);
        }
        return TrySend([CreateKeyboardInput(virtualKey, KeyboardKeyUp)]) == 1;
    }

    public void PanicRelease()
    {
        ushort[] keys;
        byte[] buttons;
        lock (_gate)
        {
            keys = _pressedKeys.ToArray();
            buttons = _pressedButtons.ToArray();
            _pressedKeys.Clear();
            _pressedButtons.Clear();
        }

        var inputs = new List<INPUT>(keys.Length + buttons.Length);
        foreach (var key in keys)
        {
            inputs.Add(CreateKeyboardInput(key, KeyboardKeyUp));
        }
        foreach (var button in buttons)
        {
            if (TryGetMouseButtonFlags(button, false, out var flags, out var data))
            {
                inputs.Add(CreateMouseInput(0, 0, data, flags));
            }
        }
        if (inputs.Count > 0)
        {
            _ = TrySend(inputs.ToArray());
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;
        PanicRelease();
    }

    private static uint ToWheelData(double value)
    {
        // 手机端以 1/1000 wheel unit 上报；Windows 一格使用 120。
        var wheel = value * 120.0 / 1000.0;
        return unchecked((uint)Math.Clamp((int)Math.Round(wheel), short.MinValue, short.MaxValue));
    }

    private static INPUT CreateMouseInput(int dx, int dy, uint data, uint flags) =>
        new()
        {
            Type = InputMouse,
            Data = new InputUnion
            {
                Mouse = new MOUSEINPUT
                {
                    Dx = dx,
                    Dy = dy,
                    MouseData = data,
                    Flags = flags,
                },
            },
        };

    private static INPUT CreateKeyboardInput(ushort virtualKey, uint flags) =>
        new()
        {
            Type = InputKeyboard,
            Data = new InputUnion
            {
                Keyboard = new KEYBDINPUT
                {
                    VirtualKey = virtualKey,
                    ScanCode = 0,
                    Flags = flags,
                },
            },
        };

    private static bool TryGetMouseButtonFlags(
        byte button,
        bool pressed,
        out uint flags,
        out uint data)
    {
        data = button is 4 or 5 ? button == 4 ? 1u : 2u : 0u;
        flags = button switch
        {
            1 => pressed ? MouseLeftDown : MouseLeftUp,
            2 => pressed ? MouseRightDown : MouseRightUp,
            3 => pressed ? MouseMiddleDown : MouseMiddleUp,
            4 or 5 => pressed ? MouseXDown : MouseXUp,
            _ => 0,
        };
        return flags != 0;
    }

    private static uint TrySend(INPUT[] inputs)
    {
        try
        {
            return SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<INPUT>());
        }
        catch (DllNotFoundException)
        {
            // 非 Windows 测试环境或精简运行环境没有 user32 时，输入事件不能影响控制连接。
            return 0;
        }
        catch (EntryPointNotFoundException)
        {
            return 0;
        }
        catch (BadImageFormatException)
        {
            return 0;
        }
        catch (Exception)
        {
            // SendInput 是可选的本机能力；任何平台/权限异常都按“本帧未发送”处理。
            return 0;
        }
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint numberOfInputs, INPUT[] inputs, int sizeOfInput);

    [StructLayout(LayoutKind.Sequential)]
    private struct INPUT
    {
        public uint Type;
        public InputUnion Data;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct InputUnion
    {
        [FieldOffset(0)]
        public MOUSEINPUT Mouse;

        [FieldOffset(0)]
        public KEYBDINPUT Keyboard;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MOUSEINPUT
    {
        public int Dx;
        public int Dy;
        public uint MouseData;
        public uint Flags;
        public uint Time;
        public nint ExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KEYBDINPUT
    {
        public ushort VirtualKey;
        public ushort ScanCode;
        public uint Flags;
        public uint Time;
        public nint ExtraInfo;
    }
}
