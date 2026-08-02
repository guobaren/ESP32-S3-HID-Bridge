using System.ComponentModel;
using System.Drawing;
using System.Runtime.InteropServices;

namespace HidBridge.Host.Input;

internal sealed class MouseCursorLock : IDisposable
{
    private bool _locked;

    internal bool IsLocked => _locked;

    internal void LockAt(Point screenPoint)
    {
        NativeMethods.ClipRect rect = CalculateClipRect(screenPoint);
        if (!NativeMethods.ClipCursor(ref rect))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "无法锁定鼠标位置。");
        }

        if (!NativeMethods.SetCursorPos(screenPoint.X, screenPoint.Y))
        {
            NativeMethods.ReleaseCursorClip(IntPtr.Zero);
            _locked = false;
            throw new Win32Exception(Marshal.GetLastWin32Error(), "无法将鼠标停放到同步位置。");
        }

        _locked = true;
    }

    internal void Release()
    {
        if (!_locked)
        {
            return;
        }

        NativeMethods.ReleaseCursorClip(IntPtr.Zero);
        _locked = false;
    }

    internal static NativeMethods.ClipRect CalculateClipRect(Point screenPoint) => new()
    {
        Left = screenPoint.X,
        Top = screenPoint.Y,
        Right = screenPoint.X + 1,
        Bottom = screenPoint.Y + 1,
    };

    public void Dispose() => Release();
}
