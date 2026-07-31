using System.ComponentModel;
using System.Runtime.InteropServices;

namespace HidBridge.Host.Input;

/// <summary>
/// 通过 Raw Input 接收鼠标设备上报的原生相对位移。
/// </summary>
internal sealed class RawMouseInputWindow : NativeWindow, IDisposable
{
    private readonly Action<NativeMethods.RawMouse> _inputHandler;
    private bool _registered;

    internal RawMouseInputWindow(Action<NativeMethods.RawMouse> inputHandler)
    {
        _inputHandler = inputHandler;

        CreateHandle(new CreateParams
        {
            Caption = "HidBridge.RawMouseInput",
            Parent = NativeMethods.HwndMessage,
        });

        NativeMethods.RawInputDevice[] devices =
        [
            new()
            {
                UsagePage = NativeMethods.GenericDesktopUsagePage,
                Usage = NativeMethods.MouseUsage,
                Flags = NativeMethods.RidevInputSink,
                Target = Handle,
            },
        ];

        if (!NativeMethods.RegisterRawInputDevices(
                devices,
                (uint)devices.Length,
                (uint)Marshal.SizeOf<NativeMethods.RawInputDevice>()))
        {
            int error = Marshal.GetLastWin32Error();
            DestroyHandle();
            throw new Win32Exception(error, "无法注册 Raw Input 鼠标输入。");
        }

        _registered = true;
    }

    protected override void WndProc(ref Message message)
    {
        if (message.Msg == NativeMethods.WmInput)
        {
            ProcessRawInput(message.LParam);
        }

        base.WndProc(ref message);
    }

    private void ProcessRawInput(IntPtr rawInputHandle)
    {
        uint size = 0;
        uint headerSize = (uint)Marshal.SizeOf<NativeMethods.RawInputHeader>();
        uint result = NativeMethods.GetRawInputData(
            rawInputHandle,
            NativeMethods.RidInput,
            IntPtr.Zero,
            ref size,
            headerSize);
        if (result != 0 || size < Marshal.SizeOf<NativeMethods.RawInput>())
        {
            return;
        }

        IntPtr buffer = Marshal.AllocHGlobal((int)size);
        try
        {
            result = NativeMethods.GetRawInputData(
                rawInputHandle,
                NativeMethods.RidInput,
                buffer,
                ref size,
                headerSize);
            if (result == uint.MaxValue)
            {
                return;
            }

            NativeMethods.RawInput input =
                Marshal.PtrToStructure<NativeMethods.RawInput>(buffer);
            if (input.Header.Type != NativeMethods.RimTypeMouse)
            {
                return;
            }

            if ((input.Mouse.Flags & NativeMethods.MouseMoveAbsolute) != 0)
            {
                // 绝对坐标设备仍可提供按钮和滚轮，但不作为移动来源。
                input.Mouse.LastX = 0;
                input.Mouse.LastY = 0;
            }

            _inputHandler(input.Mouse);
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    public void Dispose()
    {
        if (_registered)
        {
            NativeMethods.RawInputDevice[] devices =
            [
                new()
                {
                    UsagePage = NativeMethods.GenericDesktopUsagePage,
                    Usage = NativeMethods.MouseUsage,
                    Flags = NativeMethods.RidevRemove,
                    Target = IntPtr.Zero,
                },
            ];
            NativeMethods.RegisterRawInputDevices(
                devices,
                (uint)devices.Length,
                (uint)Marshal.SizeOf<NativeMethods.RawInputDevice>());
            _registered = false;
        }

        DestroyHandle();
    }
}
