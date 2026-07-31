# 连接与安全配置

## 输出组合

固件可以同时向以下后端发送同一份键鼠状态：

- USB HID：始终启用。
- BLE HID：默认编译启用，设备名可配置。
- Wi-Fi Target Agent：默认关闭，需要目标 Windows 设备运行配套程序。

电脑到开发板只能有一个活动输入租约。串口或 Wi-Fi 会话断开、超过租约时间或被另一会话替换时，开发板会向全部输出后端发送 `ReleaseAll`。

## 配置开发板

进入 ESP-IDF 环境后打开项目配置：

```powershell
Set-Location E:\ESP32-S3-HID-Bridge\firmware
. ..\scripts\Enter-EspIdf.ps1
idf.py menuconfig
```

在 `HID Bridge` 菜单配置：

- `Wi-Fi SSID` 和 `Wi-Fi 密码`：仅作为可选的首次启动回退值，通常留空并使用网页配网。
- `启用 SoftAP 网页配网`：默认启用。
- 配网热点密码：默认 `hidbridge`，正式部署前建议修改。
- 自动进入配网的连接失败时间：默认 30 秒。
- 配网按钮 GPIO 和长按时间：ESP32-S3-DevKitC-1 默认使用 GPIO0/BOOT，长按 5 秒。
- `Wi-Fi 输入预共享密钥`：电脑通过 Wi-Fi 连接开发板时使用，至少 16 字节。
- `通过 Wi-Fi 输出到目标 Agent`：需要网络目标端时启用。
- `目标 Agent IPv4 地址`、端口和独立的预共享密钥。
- `BLE HID 设备名称`。
- 输入租约超时时间，默认 1500 ms。

编译选项保存在被 Git 忽略的 `firmware/sdkconfig`，网页提交的 SSID 和密码由 Wi-Fi 驱动保存到开发板 NVS。项目使用适配 2 MiB Flash 的单应用分区表；启用 USB、Wi-Fi、BLE 和网页配网后仍保留约一半应用空间。

## SoftAP 网页配网

不需要在每次更换 Wi-Fi 时重新构建或烧录。以下任一条件会进入配网模式：

- NVS 中没有保存过 Wi-Fi。
- 已保存的网络连续 30 秒无法连接。
- 固件运行期间长按开发板 `BOOT` 键 5 秒。

进入配网前固件会结束当前输入租约并发送 `ReleaseAll`。操作步骤：

1. 在手机或电脑上连接 `HID-Bridge-Setup-XXXX` 热点。
2. 输入配网热点密码，默认是 `hidbridge`。
3. 等待系统自动弹出配网页面；没有弹出时访问 `http://192.168.4.1`。
4. 从扫描结果选择一个 2.4 GHz 网络，填写密码并保存。
5. 页面会显示开发板在新网络中获得的 IP；临时热点在连接成功约 10 秒后关闭。
6. 把显示的 IP 填入主机 `bridge.local.json` 的 `wiFiHost`。

新凭据保存到 NVS，后续重启会自动连接。更换网络只需再次长按 `BOOT` 并重复上述步骤。网页只修改路由器 SSID 和密码，不会改变 Wi-Fi 输入通道的 `networkPresharedKey`。

默认配网热点密码用于开发和首次测试。部署到不受信任环境前，应在 `menuconfig` 中修改它；也可以留空建立开放配网热点，但不建议这样做。

## 电脑通过串口连接开发板

复制主机配置：

```powershell
Copy-Item .\host\HidBridge.Host\bridge.json .\host\HidBridge.Host\bridge.local.json
```

保持 `transport` 为 `serial`，设置实际 `portName`。主机会在连接后发送 `SessionStart`，并每 500 ms 发送一次心跳。

## 电脑通过 Wi-Fi 连接开发板

在 `bridge.local.json` 中设置：

```json
{
  "transport": "wifi",
  "wiFiHost": "192.168.1.50",
  "wiFiPort": 24813,
  "networkPresharedKey": "替换为与固件 Wi-Fi 输入配置一致的随机密钥",
  "suppressLocalInput": false,
  "reconnectDelayMilliseconds": 1000,
  "heartbeatIntervalMilliseconds": 500
}
```

网络通道使用双向挑战认证、AES-256-GCM、独立会话随机数和严格递增计数器。认证失败、数据被篡改或检测到重放时会立即断开。

## 开发板通过 Wi-Fi 连接 Windows 目标设备

在目标 Windows 设备上复制配置：

```powershell
Copy-Item .\target\HidBridge.TargetAgent\agent.json .\target\HidBridge.TargetAgent\agent.local.json
dotnet run --project .\target\HidBridge.TargetAgent\HidBridge.TargetAgent.csproj -c Release
```

`agent.local.json` 中的 `presharedKey` 必须和固件的目标 Agent 密钥一致。目标 Agent 只接受一个活动连接，把键盘报告和相对鼠标报告转换为 Windows `SendInput`。开发板断开或心跳超时后，Agent 会释放全部按键和鼠标按钮。

目标 Agent 是 Windows 用户会话程序，不是系统服务。它不能控制安全桌面、UAC 安全提示或登录前界面。

## BLE HID

BLE 使用 NimBLE、配对和绑定机制，对外提供同一报告映射中的键盘 Report ID 1 与相对鼠标 Report ID 2。烧录后在目标设备的蓝牙设置中搜索配置的设备名并完成配对。

清除开发板 NVS 会同时删除网页保存的 Wi-Fi、BLE 绑定信息和 Wi-Fi 驱动状态；下次启动会自动回到配网模式。
