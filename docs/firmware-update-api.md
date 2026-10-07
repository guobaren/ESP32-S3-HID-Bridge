# 局域网固件刷写接口

控制软件可以在不退出进程的情况下释放串口、刷写固件，并自动恢复连接。刷写工具使用**内置的独立版 esptool.exe**（无需安装 Python / ESP-IDF 环境），但 EXE 不内嵌任何固件镜像。远程 API 指定运行 EXE 电脑上的 JSON 清单，并自动使用设置页已经保存的刷写串口。

## 启用

在主界面“设置”页选择并保存刷写串口，再勾选“启用局域网固件刷写接口”。该选项默认关闭，保存后立即生效。

接口监听所有 IPv4 网卡，默认端口为 `24815`，可从局域网向 `<HOST_IP>:24815` 请求；将占位符替换成 Host 所在电脑的地址。启用前必须先在设置页选择并保存正确串口。

## 调用

`manifestPath` 在运行 Host 的电脑上由 Host 进程按当前工作目录解析，不是由发送请求的电脑解析。若使用下面的项目根相对路径，请在 Host 所在电脑从项目根目录启动 `HidBridge.Host.exe`（例如另开终端执行 `& .\HidBridge.Host.exe`），并确保构建清单和三段镜像已生成。

启动刷写：

```powershell
$headers = @{ 'X-HidBridge-Action' = 'flash-firmware' }
$body = @{
    # 双板主线固件；该相对路径由运行中的 Host 进程从项目根目录解析
    manifestPath = 'firmware/build/flasher_args.json'
} | ConvertTo-Json
Invoke-RestMethod `
    -Method Post `
    -Uri 'http://<HOST_IP>:24815/api/v1/firmware/flash' `
    -Headers $headers `
    -ContentType 'application/json' `
    -Body $body
```

查询状态：

```powershell
Invoke-RestMethod -Uri 'http://<HOST_IP>:24815/api/v1/firmware/status'
```

注意：当前没有单独的“只释放 COM、不刷写”HTTP 路由。COM 释放是
`POST /api/v1/firmware/flash` 的刷写前置步骤；如果不执行刷写，应使用控制软件自身的正常退出流程。

状态 `state` 可能为：

- `idle`：尚未执行任务。
- `running`：正在释放串口、刷写或恢复连接。
- `succeeded`：三段设备端哈希校验、RTS 复位和串口恢复均通过。
- `failed`：任务失败；原因见 `message`，退出码见 `exitCode`。

重复提交时，正在运行的任务不会并发执行，接口返回 HTTP `409 Conflict` 和当前任务状态。

## 固定安全边界

- 监听所有 IPv4 网卡且没有账号、令牌或 TLS 强认证，只能在受信任局域网使用，禁止暴露到公网。
- POST 必须携带 `X-HidBridge-Action: flash-firmware`，正文必须提供 `manifestPath`。
- 不接受远程上传固件或命令行参数；`manifestPath` 指向运行控制软件电脑上的本地文件；串口自动使用设置页已保存且当前存在的 `COM`，例如 `COM3`。
- 清单中的相对镜像路径按 JSON 所在目录解析。
- 双板工程的两块板**必须刷同一版本**：`PROFILE_ACK` 长度（17 bytes）即版本判据，混刷会判失败。当前分区表为单应用 + nvs/phy_init（表尾 `0x110000`，约 1.06 MB，构建目标 4 MB Flash），若分区表有改动必须**整片重刷**三段。
- 分区表已删除历史上供板载滚动日志使用的 `storage`（SPIFFS 4 MB）分区：刷写不再涉及该分区，固件也不写入板载日志；恢复计数只保存在 RAM，板卡重启后重新累计。
- 清单必须且只能包含 `0x0`、`0x8000`、`0x10000` 三段，并且所有镜像必须位于 JSON 所在目录内。
- 控制软件刷写前关闭同步并发送 `ReleaseAll`，暂停当前控制连接并把设置页已选串口交给 esptool；不要求该串口先通过 HID Bridge 应用层握手。结束后恢复自动连接和原同步状态。
- 程序退出时若仍在刷写，会等待 esptool 结束，避免主动中断写入。
- 成功后日志会额外输出一条 `固件刷写最终摘要`，在同一行列出 `0x0`、`0x8000`、`0x10000` 三段 SHA-256、设备校验计数和 RTS 复位结果。

## 配置

`bridge.local.json` 可覆盖以下字段：

```json
{
  "firmwareUpdateApiPort": 24815,
  "firmwareFlashBaudRate": 460800,
  "firmwareFlashTimeoutSeconds": 180
}
```

本地设置页的 JSON 路径和串口选择保存在自动化设置中；远程 API 每次请求单独提供 `manifestPath`，串口始终复用设置页已保存的选择。设置页检测到多个串口时会在本地确认窗口列出全部候选和当前选择；远程 API 不显示 UI 确认，但仍要求专用确认请求头。
