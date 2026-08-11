# 本机固件刷写接口

控制软件可以在不退出进程的情况下释放串口、刷写当前项目已构建的固件，并自动恢复连接。

## 启用

在主界面“设置”页勾选“启用本机固件刷写接口”。该选项默认关闭，保存后立即生效。

接口固定监听 `127.0.0.1`，默认端口为 `24815`，不接受局域网地址直接连接。需要远程刷写时，应通过既有远程控制在运行控制软件的电脑上执行本机请求。

## 调用

启动刷写：

```powershell
$headers = @{ 'X-HidBridge-Action' = 'flash-firmware' }
Invoke-RestMethod `
    -Method Post `
    -Uri 'http://127.0.0.1:24815/api/v1/firmware/flash' `
    -Headers $headers
```

查询状态：

```powershell
Invoke-RestMethod -Uri 'http://127.0.0.1:24815/api/v1/firmware/status'
```

状态 `state` 可能为：

- `idle`：尚未执行任务。
- `running`：正在释放串口、刷写或恢复连接。
- `succeeded`：三段设备端哈希校验、RTS 复位和串口恢复均通过。
- `failed`：任务失败；原因见 `message`，退出码见 `exitCode`。

重复提交时，正在运行的任务不会并发执行，接口返回 HTTP `409 Conflict` 和当前任务状态。

## 固定安全边界

- 只绑定 IPv4 Loopback `127.0.0.1`。
- POST 必须携带 `X-HidBridge-Action: flash-firmware`，且不接受请求体。
- 不接受远程上传、镜像路径、串口名或命令行参数。
- 固件固定来自项目的 `firmware/build/flasher_args.json`。
- 清单必须且只能包含 `0x0`、`0x8000`、`0x10000` 三段，并且所有文件必须位于 build 目录内。
- 控制软件刷写前关闭同步并发送 `ReleaseAll`，刷写期间独占串口，结束后恢复串口和原同步状态。
- 程序退出时若仍在刷写，会等待 esptool 结束，避免主动中断写入。
- 成功后日志会额外输出一条 `固件刷写最终摘要`，在同一行列出 `0x0`、`0x8000`、`0x10000` 三段 SHA-256、设备校验计数和 RTS 复位结果。

## 配置

`bridge.local.json` 可覆盖以下字段：

```json
{
  "firmwareUpdateApiPort": 24815,
  "firmwareProjectRoot": "D:\\ESP32-S3-HID-Bridge",
  "firmwareFlashBaudRate": 460800,
  "firmwareFlashTimeoutSeconds": 180
}
```

`firmwareProjectRoot` 留空时，程序会从 EXE 目录和当前目录向上查找 `firmware/build/flasher_args.json`。项目内必须存在 `.esp-idf/environment/.../esptool.exe`。
