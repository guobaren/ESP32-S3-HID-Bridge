using System.ComponentModel;
using System.IO.Ports;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace HidBridge.Host.Transport;

/// <summary>同时关闭两根控制线，避免 .NET 首次初始化 DCB 时短暂保留旧 RTS。</summary>
internal static class NonResettingSerialPort
{
    private const uint ControlLineMask = (3u << 4) | (3u << 12);

    internal static void Open(SerialPort port)
    {
        if (port.IsOpen)
        {
            throw new InvalidOperationException("串口已经打开。");
        }
        if (port.Handshake != Handshake.None || port.DtrEnable || port.RtsEnable)
        {
            throw new InvalidOperationException("无复位串口要求关闭握手、DTR 和 RTS。");
        }

        // 与 pyserial 相同：GetCommState 后只用一次 SetCommState 同时关闭 DTR/RTS。
        // 不能先分别 CLR DTR/RTS；其中一根仍处于旧电平时可能拉低 EN。
        // 临时句柄不收发数据、不刷新队列、不修改系统持久化的默认串口配置。
        using (SafeFileHandle handle = CreateFileW(@"\\.\" + port.PortName,
            0xC0000000, 0, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw NativeFailure("打开串口控制线配置句柄失败");
            }
            Dcb state = new() { Length = (uint)Marshal.SizeOf<Dcb>() };
            if (!GetCommState(handle, ref state))
            {
                throw NativeFailure("读取串口控制线配置失败");
            }
            state.Flags &= ~ControlLineMask;
            if (!SetCommState(handle, ref state))
            {
                throw NativeFailure("同时关闭串口 DTR/RTS 失败");
            }
            if (!GetCommState(handle, ref state))
            {
                throw NativeFailure("校验串口控制线配置失败");
            }
            if ((state.Flags & ControlLineMask) != 0)
            {
                throw new IOException("驱动未保留关闭 DTR/RTS 的配置，停止打开串口。");
            }
        }

        port.Open();
        try
        {
            port.DtrEnable = false;
            port.RtsEnable = false;
        }
        catch
        {
            port.Dispose();
            throw;
        }
    }

    private static IOException NativeFailure(string message)
    {
        int error = Marshal.GetLastWin32Error();
        return new IOException($"{message}（Win32={error}）。", new Win32Exception(error));
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Dcb
    {
        public uint Length;
        public uint BaudRate;
        public uint Flags;
        public ushort Reserved;
        public ushort XonLimit;
        public ushort XoffLimit;
        public byte ByteSize;
        public byte Parity;
        public byte StopBits;
        public byte XonChar;
        public byte XoffChar;
        public byte ErrorChar;
        public byte EofChar;
        public byte EventChar;
        public ushort Reserved1;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
    private static extern SafeFileHandle CreateFileW(string fileName, uint desiredAccess,
        uint shareMode, IntPtr securityAttributes, uint creationDisposition, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetCommState(SafeFileHandle handle, ref Dcb state);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetCommState(SafeFileHandle handle, ref Dcb state);
}
