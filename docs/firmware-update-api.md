# 本机固件刷写接口

控制软件可以在不退出进程的情况下释放串口、刷写固件，并自动恢复连接。刷写工具使用**内置的独立版 esptool.exe**（无需安装 Python / ESP-IDF 环境），但 EXE 不内嵌任何固件镜像。设置页和远程 API 分别指定本机 JSON 清单，并共用同一套计划校验与刷写执行工具。

## 启用

在主界面“设置”页勾选“启用本机固件刷写接口”。该选项默认关闭，保存后立即生效。

接口固定监听 `127.0.0.1`，默认端口为 `24815`，不接受局域网地址直接连接。需要远程刷写时，应通过既有远程控制在运行控制软件的电脑上执行本机请求。

## 调用

启动刷写：

```powershell
$headers = @{ 'X-HidBridge-Action' = 'flash-firmware' }
$body = @{ manifestPath = 'D:\ESP32-S3-HID-Bridge\firmware\build\flasher_args.json' } | ConvertTo-Json
Invoke-RestMethod `
    -Method Post `
    -Uri 'http://127.0.0.1:24815/api/v1/firmware/flash' `
    -Headers $headers `
    -ContentType 'application/json' `
    -Body $body
```

查询状态：

```powershell
Invoke-RestMethod -Uri 'http://127.0.0.1:24815/api/v1/firmware/status'
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

- 只绑定 IPv4 Loopback `127.0.0.1`。
- POST 必须携带 `X-HidBridge-Action: flash-firmware`，正文必须是 `{"manifestPath":"本机 JSON 绝对路径"}`。
- 不接受远程上传固件、串口名或命令行参数；`manifestPath` 指向运行控制软件电脑上的本地文件。
- 清单中的相对镜像路径按 JSON 所在目录解析。
- 清单必须且只能包含 `0x0`、`0x8000`、`0x10000` 三段，并且所有镜像必须位于 JSON 所在目录内。
- 控制软件刷写前关闭同步并发送 `ReleaseAll`，刷写期间独占串口，结束后恢复串口和原同步状态。
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

本地设置页选择的 JSON 路径保存在自动化设置中；远程 API 每次请求单独提供 `manifestPath`，不会复用设置页选择值。
