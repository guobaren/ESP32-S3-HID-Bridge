# WCH CH341SER Windows 驱动（离线包）

本目录保存 WCH 官方 CH340/CH341 USB 转串口驱动，适用于当前检测到的
`USB\VID_1A86&PID_7523`。历史诊断中该设备曾为 `CM_PROB_FAILED_INSTALL`、Problem Code
`28`，表示 Windows 尚未安装可用的匹配驱动；安装后应出现在“端口 (COM 和 LPT)”下。
这不是 Code 43。

## 来源与文件

- 官方页面：[CH341SER.ZIP](https://www.wch-ic.com/downloads/CH341SER_ZIP.html)
- 官方直链：<https://www.wch-ic.com/download/file?id=5>
- 官方同站 API 元数据确认：版本 `4.0`、上传日期 `2026-06-26`、页面标注大小 `696KB`、文件名 `CH341SER.ZIP`。
- 保存文件：`CH341SER_v4.0_2026-06-26.zip`
- 归档中的可安装 INF：`CH341SER\CH341SER.INF`
- 本地大小：`713,322` bytes（约 696 KiB）
- SHA-256：`59967D9CE371D0BF3DF02DEC0B66C8DFBF9CA576DA0572FF4404148A7C381807`

## 离线校验记录（2026-08-25）

- 直链请求返回 HTTP `200 OK`，响应 `Content-Disposition` 为 `CH341SER.ZIP`。
- ZIP 头为 `50 4B 03 04`，`Expand-Archive` 解压成功。
- `CH341SER\CH341SER.INF` 的 `DriverVer` 为 `02/11/2026, 4.0.2026.02`，并包含 `USB\VID_1A86&PID_7523` 安装项。
- `CH341SER\CH341SER.CAT` 和 `WIN 1X\CH341SER.CAT`：Authenticode `Valid`，签名者为 Microsoft Windows Hardware Compatibility Publisher。
- `CH341SER\SETUP.EXE`、`CH341SER\DRVSETUP64\DRVSETUP64.exe`：Authenticode `Valid`，签名者为 Nanjing Qinheng Microelectronics Co., Ltd.

归档内容包括：

```text
CH341SER/
  CH341SER.INF       CH341SER.CAT       SETUP.EXE
  DRVSETUP64/DRVSETUP64.exe
  CH341M64.sys       CH341S64.sys       CH341S98.SYS
  CH341SER.sys       CH341SER.VXD
  CH341PORTS.DLL     CH341PORTSA64.DLL  CH341PT.DLL  CH341PTA64.DLL
  WIN 1X/            （Windows 1X 兼容驱动及 INF/CAT）
  WIN 9X/            （Windows 9X 兼容驱动及 INF/VXD）
```

## 离线安装方式

1. 将 ZIP 解压到本机目录。
2. 手动以管理员身份运行解压目录中的 `CH341SER\SETUP.EXE`，或打开设备管理器，找到带黄色标记的 `USB\VID_1A86&PID_7523`，选择“更新驱动程序”→“浏览我的电脑查找驱动程序”，指向解压后的 `CH341SER` 目录或其中的 `CH341SER.INF`。
3. 安装完成后确认设备出现在“端口 (COM 和 LPT)”下，并记录分配的 `COMx`；也可运行：

   ```powershell
   Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'USB\VID_1A86&PID_7523*'
   [System.IO.Ports.SerialPort]::GetPortNames()
   ```

安装和设备驱动更新需要管理员权限。安装后的 COM 端口和 Host 连接需在目标电脑上单独验证。
