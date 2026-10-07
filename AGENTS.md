# ESP32-S3 HID Bridge 项目补充规则

除下面列出的项目专属约束外，通用启动、协作、测试和双文档协议遵循全局 `AGENTS.md`。

## 项目特有规则

### 通用要求

- 面向人的文档、注释和提交说明优先使用中文；协议字段、API 名称和技术标识符保持原样。
- 不在日志、配置或示例中加入真实设备序列号等不必要的本机标识。

### 修改主机端（C#）后

至少运行：

```powershell
dotnet build .\host\HidBridge.Host\HidBridge.Host.csproj -c Release
dotnet format .\host\HidBridge.Host\HidBridge.Host.csproj --verify-no-changes --no-restore
```

### 修改固件（ESP32-S3）后

本分支（`main`）的 `firmware/` **就是双板主固件工程**（工程名 `dual_s3_hid_proxy`），直接在这里构建，不要再寻找 `firmware/dual_proxy`。单板固件只在 `single-board` 分支维护，本分支不新增单板构建入口。在已进入 ESP-IDF 环境的终端运行：

```powershell
Set-Location .\firmware
. ..\scripts\Enter-EspIdf.ps1
idf.py build
```

`idf.py set-target esp32s3` **只在 `firmware/sdkconfig` 尚不存在时**执行一次：它会重置已有配置（分区表、TinyUSB 缓冲等），在已配置的工作区重复执行会丢掉既有设置。

### 文档分层

- README 只写面向使用者的内容：能做什么、怎么连、注意事项；协议细节、命令表、帧格式、API 与实现说明写到 `docs/` 下（协议进 `docs/protocol.md`，固件工程说明进 `docs/固件.md`，验证入口与边界进 `docs/验证.md`）。
- Makcu 与 kmnet（kmboxNet）目标功能相同、只是通路不同：面向用户按「本项目 UDP 接口 / kmnet 接口 / Makcu 盒子接口」三条通路并列介绍，开发者细节不进 README。
- 单板固件属于 `single-board` 分支：本分支（main）只构建双板固件，不新增单板构建入口。

### 串口协议变更

串口协议发生变化时，同步更新 `docs/protocol.md`、C# 编码器和 C 解析器。

### 串口查询默认不触发复位

所有**只读查询**（统计快照、角色探测、Makcu 版本查询、日志监视等）都必须在不触发板卡复位的前提下打开串口。这是默认做法，不是可选项：

```python
ser = serial.Serial()      # 先不指定 port：构造时不会打开端口
ser.port = port_name
ser.baudrate = baud
ser.dtr = False            # 必须在 open() 之前置低
ser.rts = False
ser.open()
ser.dtr = False            # 打开后再确认一次，防止驱动恢复默认电平
ser.rts = False
```

- 原因：ESP32/ESP32-S3 开发板把 DTR/RTS 接到 EN 与 GPIO0，pyserial 默认在 `open()` 时拉高这两根线，经自动复位电路复位板卡。`serial.Serial(port, baud)` 这种"带 port 直接打开"的写法必然经过默认电平，**禁止用于只读查询**。
- 复位会污染结论：内存计数是 RAM-only，一次复位就把累计值清零并打断 USB 链路与克隆；复位后的快照不能再当作累计计数使用。
- 需要复位的动作（刷写、显式复位脉冲）只能由专门入口执行，并在注释与 `--help` 里写明它会复位。
- 每次查询都要能证明没有复位：把设备上报的 `uptimeMilliseconds` 与该次查询的墙钟时间对比，uptime 必须随时间连续增长；若 uptime 归零或明显偏小，说明发生了复位，该次数据只能按"复位后窗口"使用并在记录中标注。
- 参考实现：`tools/read_device_stats.py` 打开端口的部分。

### 输入安全

任何停止转发、断线或退出路径都必须释放键盘按键和鼠标按钮，避免目标设备卡键。
