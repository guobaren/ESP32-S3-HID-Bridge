using System.ComponentModel;
using System.Diagnostics;
using HidBridge.Host.Drivers;

namespace HidBridge.Host.Ui;

internal static class DriverInstallPrompt
{
    internal static string BuildMessage(Ch341DriverPackage package, Ch341DriverPromptReason reason) =>
        (reason == Ch341DriverPromptReason.DriverStoreMissing
            ? "系统 Driver Store 中未检测到 CH341SER.INF；当前也未发现已插入的 CH340/CH341 设备。\n\n"
            : "检测到已插入的 CH340/CH341 设备，Windows Problem Code 为 28（缺少匹配驱动）。\n\n") +
        $"将使用随程序附带、签名有效的 WCH 官方安装器：\n{package.SetupPath}\n\n" +
        "点击“是”后会请求管理员权限并执行官方 SETUP.EXE /S。安装后可使用同一 SETUP.EXE 的 UNINSTALL，" +
        "或命令行 /U 正常卸载；点击“否”则稍后手动处理。";

    internal static Ch341DriverInstallResult? ShowAndInstall(
        Ch341DriverPackage package,
        Ch341DriverPromptReason reason)
    {
        DialogResult choice = MessageBox.Show(
            BuildMessage(package, reason),
            "安装 CH340/CH341 驱动",
            MessageBoxButtons.YesNo,
            MessageBoxIcon.Warning,
            MessageBoxDefaultButton.Button2);
        if (choice != DialogResult.Yes)
        {
            return null;
        }

        ProcessStartInfo startInfo = new()
        {
            FileName = package.SetupPath,
            Arguments = Ch341DriverInstallerPolicy.VendorInstallArguments,
            UseShellExecute = true,
            Verb = "runas",
            WorkingDirectory = package.RootPath,
            WindowStyle = ProcessWindowStyle.Hidden,
        };

        try
        {
            using Process process = Process.Start(startInfo)
                ?? throw new InvalidOperationException("无法启动 WCH SETUP.EXE。");
            process.WaitForExit();
            Ch341DriverInstallState state = Ch341DriverInstallerPolicy.ClassifyExitCode(process.ExitCode);
            return new Ch341DriverInstallResult(
                state,
                process.ExitCode,
                state == Ch341DriverInstallState.Failed
                    ? $"WCH SETUP.EXE /S 退出码为 {process.ExitCode}。"
                    : null);
        }
        catch (Win32Exception exception) when (exception.NativeErrorCode == 1223)
        {
            return new Ch341DriverInstallResult(
                Ch341DriverInstallState.Cancelled,
                null,
                "用户取消了管理员权限请求。");
        }
        catch (Exception exception) when (exception is Win32Exception or InvalidOperationException)
        {
            return new Ch341DriverInstallResult(
                Ch341DriverInstallState.Failed,
                null,
                exception.Message);
        }
    }
}
