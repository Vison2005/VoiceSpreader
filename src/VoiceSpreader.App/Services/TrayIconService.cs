using System.Collections.Concurrent;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace VoiceSpreader.App.Services;

public sealed partial class TrayIconService : IDisposable
{
    private const int WmApp = 0x8000;
    private const int TrayCallbackMessage = WmApp + 1;
    private const int WmLButtonUp = 0x0202;
    private const int WmRButtonUp = 0x0205;
    private const uint NimAdd = 0x00000000;
    private const uint NimModify = 0x00000001;
    private const uint NimDelete = 0x00000002;
    private const uint NifMessage = 0x00000001;
    private const uint NifIcon = 0x00000002;
    private const uint NifTip = 0x00000004;
    private const uint NifInfo = 0x00000010;
    private const uint ImageIcon = 1;
    private const uint LrLoadFromFile = 0x00000010;
    private const uint LrDefaultSize = 0x00000040;
    private const uint MfString = 0x00000000;
    private const uint MfSeparator = 0x00000800;
    private const uint TpmRightButton = 0x0002;
    private const uint TpmReturnCommand = 0x0100;
    private const uint CommandShow = 1;
    private const uint CommandHide = 2;
    private const uint CommandExit = 3;
    private const string WindowClassName = "SoundSpreader.TrayMessageWindow";

    private static readonly ConcurrentDictionary<nint, TrayIconService> Instances = new();
    private static readonly WndProc WindowProcedure = WindowProc;
    private static readonly object ClassRegistrationLock = new();
    private static bool _classRegistered;

    private readonly nint _windowHandle;
    private readonly nint _iconHandle;
    private readonly NotifyIconData _iconData;
    private bool _disposed;

    public TrayIconService()
    {
        EnsureWindowClass();
        _windowHandle = CreateWindowEx(
            0,
            WindowClassName,
            "SoundSpreader Tray",
            0,
            0,
            0,
            0,
            0,
            nint.Zero,
            nint.Zero,
            GetModuleHandle(null),
            nint.Zero);
        if (_windowHandle == nint.Zero)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "无法创建托盘消息窗口。");
        }
        Instances[_windowHandle] = this;

        var iconPath = Path.Combine(AppContext.BaseDirectory, "Assets", "AppIcon.ico");
        _iconHandle = LoadImage(
            nint.Zero,
            iconPath,
            ImageIcon,
            0,
            0,
            LrLoadFromFile | LrDefaultSize);
        if (_iconHandle == nint.Zero)
        {
            Dispose();
            throw new Win32Exception(Marshal.GetLastWin32Error(), "无法加载托盘图标。");
        }

        _iconData = CreateIconData(NifMessage | NifIcon | NifTip);
        if (!ShellNotifyIcon(NimAdd, ref _iconData))
        {
            Dispose();
            throw new Win32Exception(Marshal.GetLastWin32Error(), "无法添加系统托盘图标。");
        }
    }

    public event EventHandler? ShowRequested;

    public event EventHandler? HideRequested;

    public event EventHandler? ExitRequested;

    public void ShowFirstMinimizeMessage()
    {
        var notification = CreateIconData(NifInfo);
        notification.InfoTitle = "SoundSpreader 仍在运行";
        notification.Info = "多设备同步会继续在后台运行；可从通知区域恢复窗口或退出。";
        notification.TimeoutOrVersion = 3500;
        ShellNotifyIcon(NimModify, ref notification);
    }

    private NotifyIconData CreateIconData(uint flags) => new()
    {
        Size = (uint)Marshal.SizeOf<NotifyIconData>(),
        WindowHandle = _windowHandle,
        Id = 1,
        Flags = flags,
        CallbackMessage = TrayCallbackMessage,
        IconHandle = _iconHandle,
            Tip = "SoundSpreader",
        Info = string.Empty,
        InfoTitle = string.Empty,
    };

    private static void EnsureWindowClass()
    {
        lock (ClassRegistrationLock)
        {
            if (_classRegistered)
            {
                return;
            }

            var windowClass = new WindowClass
            {
                Size = (uint)Marshal.SizeOf<WindowClass>(),
                WindowProcedure = Marshal.GetFunctionPointerForDelegate(WindowProcedure),
                Instance = GetModuleHandle(null),
                ClassName = WindowClassName,
            };
            if (RegisterClassEx(ref windowClass) == 0)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "无法注册托盘消息窗口类。");
            }
            _classRegistered = true;
        }
    }

    private static nint WindowProc(nint windowHandle, uint message, nuint wParam, nint lParam)
    {
        if (message == TrayCallbackMessage && Instances.TryGetValue(windowHandle, out var instance))
        {
            switch ((int)lParam)
            {
                case WmLButtonUp:
                    instance.ShowRequested?.Invoke(instance, EventArgs.Empty);
                    return nint.Zero;
                case WmRButtonUp:
                    instance.ShowContextMenu();
                    return nint.Zero;
            }
        }
        return DefWindowProc(windowHandle, message, wParam, lParam);
    }

    private void ShowContextMenu()
    {
        var menu = CreatePopupMenu();
        if (menu == nint.Zero)
        {
            return;
        }

        try
        {
        AppendMenu(menu, MfString, CommandShow, "显示 SoundSpreader");
            AppendMenu(menu, MfString, CommandHide, "最小化到托盘");
            AppendMenu(menu, MfSeparator, 0, null);
            AppendMenu(menu, MfString, CommandExit, "退出");
            GetCursorPos(out var cursor);
            SetForegroundWindow(_windowHandle);
            var command = TrackPopupMenu(
                menu,
                TpmRightButton | TpmReturnCommand,
                cursor.X,
                cursor.Y,
                0,
                _windowHandle,
                nint.Zero);
            switch (command)
            {
                case CommandShow:
                    ShowRequested?.Invoke(this, EventArgs.Empty);
                    break;
                case CommandHide:
                    HideRequested?.Invoke(this, EventArgs.Empty);
                    break;
                case CommandExit:
                    ExitRequested?.Invoke(this, EventArgs.Empty);
                    break;
            }
        }
        finally
        {
            DestroyMenu(menu);
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        if (_windowHandle != nint.Zero)
        {
            var iconData = CreateIconData(0);
            ShellNotifyIcon(NimDelete, ref iconData);
            Instances.TryRemove(_windowHandle, out _);
            DestroyWindow(_windowHandle);
        }
        if (_iconHandle != nint.Zero)
        {
            DestroyIcon(_iconHandle);
        }
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WindowClass
    {
        public uint Size;
        public uint Style;
        public nint WindowProcedure;
        public int ClassExtra;
        public int WindowExtra;
        public nint Instance;
        public nint Icon;
        public nint Cursor;
        public nint Background;
        public string? MenuName;
        public string ClassName;
        public nint SmallIcon;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NotifyIconData
    {
        public uint Size;
        public nint WindowHandle;
        public uint Id;
        public uint Flags;
        public uint CallbackMessage;
        public nint IconHandle;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string Tip;
        public uint State;
        public uint StateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string Info;
        public uint TimeoutOrVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string InfoTitle;
        public uint InfoFlags;
        public Guid ItemGuid;
        public nint BalloonIconHandle;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Point
    {
        public int X;
        public int Y;
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate nint WndProc(nint windowHandle, uint message, nuint wParam, nint lParam);

    [DllImport("shell32.dll", EntryPoint = "Shell_NotifyIconW", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShellNotifyIcon(uint message, ref NotifyIconData data);

    [DllImport("user32.dll", EntryPoint = "RegisterClassExW", SetLastError = true)]
    private static extern ushort RegisterClassEx(ref WindowClass windowClass);

    [LibraryImport("user32.dll", EntryPoint = "CreateWindowExW", StringMarshalling = StringMarshalling.Utf16, SetLastError = true)]
    private static partial nint CreateWindowEx(
        uint extendedStyle,
        string className,
        string windowName,
        uint style,
        int x,
        int y,
        int width,
        int height,
        nint parent,
        nint menu,
        nint instance,
        nint parameter);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool DestroyWindow(nint windowHandle);

    [LibraryImport("user32.dll", EntryPoint = "DefWindowProcW")]
    private static partial nint DefWindowProc(nint windowHandle, uint message, nuint wParam, nint lParam);

    [LibraryImport("user32.dll", EntryPoint = "LoadImageW", StringMarshalling = StringMarshalling.Utf16, SetLastError = true)]
    private static partial nint LoadImage(
        nint instance,
        string name,
        uint type,
        int desiredWidth,
        int desiredHeight,
        uint loadFlags);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool DestroyIcon(nint iconHandle);

    [LibraryImport("user32.dll")]
    private static partial nint CreatePopupMenu();

    [LibraryImport("user32.dll", EntryPoint = "AppendMenuW", StringMarshalling = StringMarshalling.Utf16)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool AppendMenu(nint menu, uint flags, nuint item, string? text);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool DestroyMenu(nint menu);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetCursorPos(out Point point);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool SetForegroundWindow(nint windowHandle);

    [LibraryImport("user32.dll", EntryPoint = "TrackPopupMenu")]
    private static partial uint TrackPopupMenu(
        nint menu,
        uint flags,
        int x,
        int y,
        int reserved,
        nint windowHandle,
        nint rectangle);

    [LibraryImport("kernel32.dll", EntryPoint = "GetModuleHandleW", StringMarshalling = StringMarshalling.Utf16)]
    private static partial nint GetModuleHandle(string? moduleName);
}
