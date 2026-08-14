using System.Runtime.InteropServices;
using System.Text;

namespace HidBridge.Host.Input;

internal static class NativeMethods
{
    internal const int WhKeyboardLl = 13;
    internal const int WhMouseLl = 14;
    internal const int HcAction = 0;

    internal const int WmKeyDown = 0x0100;
    internal const int WmKeyUp = 0x0101;
    internal const int WmSysKeyDown = 0x0104;
    internal const int WmSysKeyUp = 0x0105;
    internal const int WmInput = 0x00FF;
    internal const int WmQuit = 0x0012;

    internal const uint LlkhfExtended = 0x01;
    internal const uint LlkhfInjected = 0x10;
    internal const uint RidInput = 0x10000003;
    internal const uint RidiDeviceName = 0x20000007;
    internal const uint RimTypeMouse = 0;
    internal const uint RidevRemove = 0x00000001;
    internal const uint RidevInputSink = 0x00000100;
    internal const ushort GenericDesktopUsagePage = 0x01;
    internal const ushort MouseUsage = 0x02;
    internal const ushort MouseMoveAbsolute = 0x01;
    internal const ushort RawMouseLeftButtonDown = 0x0001;
    internal const ushort RawMouseLeftButtonUp = 0x0002;
    internal const ushort RawMouseRightButtonDown = 0x0004;
    internal const ushort RawMouseRightButtonUp = 0x0008;
    internal const ushort RawMouseMiddleButtonDown = 0x0010;
    internal const ushort RawMouseMiddleButtonUp = 0x0020;
    internal const ushort RawMouseButton4Down = 0x0040;
    internal const ushort RawMouseButton4Up = 0x0080;
    internal const ushort RawMouseButton5Down = 0x0100;
    internal const ushort RawMouseButton5Up = 0x0200;
    internal const ushort RawMouseWheel = 0x0400;
    internal const ushort RawMouseHorizontalWheel = 0x0800;

    internal static readonly IntPtr HwndMessage = new(-3);

    internal delegate IntPtr HookProc(int code, IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll")]
    internal static extern uint GetCurrentThreadId();

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool PostThreadMessage(
        uint threadId,
        int message,
        IntPtr wParam,
        IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    internal struct KeyboardHookData
    {
        internal uint VirtualKey;
        internal uint ScanCode;
        internal uint Flags;
        internal uint Time;
        internal UIntPtr ExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RawInputDevice
    {
        internal ushort UsagePage;
        internal ushort Usage;
        internal uint Flags;
        internal IntPtr Target;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RawInputHeader
    {
        internal uint Type;
        internal uint Size;
        internal IntPtr Device;
        internal IntPtr WParam;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RawMouse
    {
        internal ushort Flags;
        internal ushort Reserved;
        internal uint Buttons;
        internal uint RawButtons;
        internal int LastX;
        internal int LastY;
        internal uint ExtraInformation;

        internal readonly ushort ButtonFlags => unchecked((ushort)Buttons);
        internal readonly short ButtonData => unchecked((short)(Buttons >> 16));
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RawInput
    {
        internal RawInputHeader Header;
        internal RawMouse Mouse;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ClipRect
    {
        internal int Left;
        internal int Top;
        internal int Right;
        internal int Bottom;
    }

    [DllImport("user32.dll", SetLastError = true)]
    internal static extern IntPtr SetWindowsHookEx(
        int idHook,
        HookProc callback,
        IntPtr module,
        uint threadId);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool UnhookWindowsHookEx(IntPtr hook);

    [DllImport("user32.dll")]
    internal static extern IntPtr CallNextHookEx(
        IntPtr hook,
        int code,
        IntPtr wParam,
        IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool ClipCursor(ref ClipRect rect);

    [DllImport("user32.dll", EntryPoint = "ClipCursor", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool ReleaseCursorClip(IntPtr rect);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool RegisterRawInputDevices(
        [In] RawInputDevice[] devices,
        uint deviceCount,
        uint deviceSize);

    [DllImport("user32.dll", SetLastError = true)]
    internal static extern uint GetRawInputData(
        IntPtr rawInput,
        uint command,
        IntPtr data,
        ref uint size,
        uint headerSize);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    internal static extern uint GetRawInputDeviceInfo(
        IntPtr device,
        uint command,
        StringBuilder? data,
        ref uint size);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    internal static extern IntPtr GetModuleHandle(string? moduleName);
}
