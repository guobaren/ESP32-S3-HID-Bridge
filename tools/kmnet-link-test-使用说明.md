# KMNET / Makcu 鼠标链路测试

将 ZIP 解压到副机，运行 `KmnetLinkTest.exe`。无需安装 Python 或厂商 pyd。适用 Windows 10/11 x64；接收方系统兼容性需实际运行确认。

顶部选择“KMNET（UDP）”或“Makcu（串口）”；下方方向、自定义位移、方框、停止与日志测试功能相同。连接期间不能切接口，先断开再切换。

## KMNET 接口

1. 在连接开发板的电脑启动 `HidBridge.Host.exe`，确认 M 板连接与软件鼠标通路可用。记录 Host 显示的局域网 UDP 地址和端口。
2. 停止其他 KMNET 发送程序。本工具的 init 会接管 Host 当前 KMNET 会话；其他程序重新 init 也会抢占本工具的会话。
3. 在副机填写该 IPv4、端口（默认 24814）及 8 位十六进制 UUID（例如 AF425414）。UUID 不是 Windows 的长 GUID，Host 兼容服务接受 init 中的 UUID 作为本会话标识。
4. 点击“连接 / 测试 init”。日志应显示完整匹配的 init ACK 和往返耗时。
5. 点击方向键或输入 X/Y 发送一次，观察 **P 板 USB 连接的目标电脑** 的光标。方框测试依次右、下、左、上移动，每边间隔 300 ms。相对位移单位是输入计数，系统加速等设置可能使它不等于屏幕像素；不能保证回到原点。
6. 可随时“停止测试”，结束后点击“断开”。保存日志可交给排查人员。

## Makcu 接口

1. 将 M 板 UART0 / CH340 串口 USB 接到运行工具的电脑；P 板仍连接目标电脑。Makcu 是串口直连，不通过局域网，IP / 端口 / UUID 对此接口不适用。
2. 先在 Host 里开启 Makcu（V4 或老版），确认切换成功，然后从托盘菜单真正退出 Host 以释放串口；关闭窗口退托盘仍会占用串口。本工具不自动切换模式或波特率。
3. 选择“Makcu（串口）”，刷新并选择 M 板 COM 口，也可手动输入。固定使用项目当前的 115200 / 8N1；串口列表不证明板卡角色，不要按 CH340 名称猜测。
4. 点击“连接 / 查询版本”。支持当前桥接 V4 的 `km.MAKCU` 与老版 `km.ESP32-S3-HID-Bridge-MakcuASCII-1` 标识。仅验证项目支持的 ASCII 子集，不承诺任意原厂设备适用。
5. 测试操作与 KMNET 相同。移动写入后附加 `km.version()` 查询，显示查询回应及耗时；**这不是移动 ACK，也不是目标实际执行证明**。V4 setter 默认无回应，老版 setter 的提示符也不会被误当作版本回应；显式错误会报告 Failed。
6. 每轮结束、停止、断开和正常退出发送五个鼠标按钮的松开命令，并检查后续版本回应；中途失败也尽力松开，不发送按下或键盘命令。

工具使用不带 port 的 serial 构造器，在 open 前设置 DTR/RTS 为低，打开后再次设置低；没有复位脉冲或自动波特率探测。实机是否完全无复位尚未验证：Makcu 期间无法读取本项目 uptime 统计，本轮没有打开真实串口，不能把控制线设置夹具测试视为无复位验收。

## 结果如何理解

- init ACK 匹配：副机到 Host 的 KMNET 协议往返可达；不代表串口、开发板、USB 或目标输入通过。
- move ACK 匹配但目标不动：Host 当前实现会对合法鼠标包应答，即使后续移动未输出。检查 M 板通路；旧单板还要检查 HOME / 始终开启 UDP 输出设置。双板网络输入不需要打开旧版“UDP 模拟输入测试”。
- 超时 / Failed：没有收到完整匹配回包。检查 IP、端口、Host 是否运行、两端 UDP 防火墙规则、其他程序是否抢占 KMNET 会话。不能仅据超时确定是防火墙。
- 10054 / 远程主机强迫关闭：UDP 回包端口不可达等情况也可能触发，不能直接归因为防火墙。
- RTT 仅为该请求的网络及服务处理往返耗时，不是鼠标端到端延迟。
- Makcu 无版本回应或串口占用：检查当前实际模式、M 板端口和 Host 是否真正退出；无回应不能直接判定移动未执行。

## 行为边界

工具直接实现项目文档中的明文 KMNET `init` / `move` 兼容数据包，不调用厂商二进制库，不使用 JSON UDP 接口。会话始终复用一个 UDP socket，逐包核对回包来源及 mac/rand/index/cmd，忽略无关包；超时不会自动重发移动。所有鼠标包的按钮位均为零，不发送键盘、点击、屏蔽、监视、重启或配置命令。停止、断开和正常退出尽力发送鼠标松开状态并接收回包；网络故障时会明确报告无法确认。工具从不按下键鼠，不负责释放其他客户端的键盘状态。

未实施实际副机、防火墙、真实串口或开发板端到端验收；本地 UDP 对端和串口字节流夹具测试不能替代实际目标光标观察。

## 源码运行与构建

```powershell
# 源码运行需 pyserial；分发 EXE 已内置。
python -m pip install pyserial==3.5
python tools/kmnet_link_test.py
python -m unittest discover -s tools -p test_kmnet_link_test.py -v
# 构建机需提前安装 PyInstaller；不要求接收方安装。
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-kmnet-link-test.ps1
```

输出：`dist/kmnet-link-test/KmnetLinkTest.exe` 与 `dist/KmnetLinkTest-win-x64.zip`。
