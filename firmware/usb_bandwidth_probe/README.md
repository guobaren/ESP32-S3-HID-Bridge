# 原生 USB CDC / WinUSB 吞吐探针

独立 ESP32-S3 测试固件，仅测 USB 数据收发；不包含 HID、Wi-Fi、视频编解码或 Sender/run 接入。先保存整片 Flash 备份，再按需要保留测试固件或恢复备份。用户再次要求提高发送速度，本轮保留 WinUSB 测试固件，不自动恢复。不得将 CH340 维护口的 UART 速度作为原生 USB 吞吐。

## 构建与运行

在项目根目录加载 `scripts/Enter-EspIdf.ps1`，使用项目 ESP-IDF 6.0.2：

```powershell
. ./scripts/Enter-EspIdf.ps1
$probeRoot = (Resolve-Path firmware/usb_bandwidth_probe).Path
# 独立配置文件与构建目录，避免沿用另一模式的生成配置。
idf.py -C $probeRoot -B "$probeRoot/build-cdc" -D "IDF_TARGET=esp32s3" -D "SDKCONFIG=$probeRoot/sdkconfig.cdc" -D "SDKCONFIG_DEFAULTS=$probeRoot/sdkconfig.defaults" build
idf.py -C $probeRoot -B "$probeRoot/build-vendor" -D "IDF_TARGET=esp32s3" -D "SDKCONFIG=$probeRoot/sdkconfig.vendor" -D "SDKCONFIG_DEFAULTS=$probeRoot/sdkconfig.defaults;$probeRoot/sdkconfig.vendor.defaults" build
# 按实际维护口刷写；原生 USB 口需同时连接。
idf.py -C $probeRoot -B "$probeRoot/build-vendor" -p COM4 -b 460800 flash
python firmware/usb_bandwidth_probe/benchmark.py --transport winusb --seconds 30 --batch-blocks 16 --queue-depth 1 --output usb-results.json
# CDC 模式刷对应目录，测速使用 --transport cdc --port COM17。
```

COM 号只是本次示例。CDC 为 `303A:4009`；WinUSB 为 `303A:400A`、版本 0101、单接口 OUT01/IN81、Bulk64，自动绑定 Windows 内置 WinUSB，没有原生 COM 口，维护 COM4 保留。脚本检查专用 GUID、实际设备描述符、管道类型/包长和固件握手，拒绝其他设备。CDC 的 115200 line coding 不限制原生 USB bulk 传输。每个方向分别测量，不代表全双工速度。

Vendor 模式显式开启，工程默认 CDC。当前实测构建位于仓库根 `build/`，当前生成 `sdkconfig` 是 Vendor；以上新目录命令用于干净复现，未重跑这些新目录命令。复用生成配置时，仅追加 defaults 不会覆盖已保存的 sdkconfig。

WinUSB 模式 RX FIFO32768、TX FIFO16384、EP8192；对 TinyUSB 组件启用 `CFG_TUD_VENDOR_RX_NEED_ZLP=1`，主机 OUT 设置 SHORT_PACKET_TERMINATE。发送完成事件唤醒应用替代固定 10 ms 等待。主机 OVERLAPPED 请求保持缓冲/事件有效直到完成，所有请求排空后才发送统计结束帧，实际耗时包含排空。默认队列深度 1；深度 4 实测未明显增益。

单接口使用设备级 MS OS 2.0 描述符（162 字节），不使用复合设备的配置/功能 subset。描述符对象有显式链接引用，避免弱回调占位。参考 [TinyUSB 单接口自动绑定记录](https://github.com/hathach/tinyusb/discussions/3366)。

## 协议和校验

全速 USB，bulk 端点最大包 64 字节。小端协议头为五个 uint32：magic=`0x31504255`、type、sequence、length、CRC32；最大 payload 4096 字节。CRC 与 Python zlib 一致。

- type 1：握手；10：清零下行计数并应答。
- type 2：电脑发送确定性数据块；3：结束下行并请求统计。
- type 4：请求上行，payload 为 uint32 毫秒；6：固件上行数据。
- type 5：五个 uint64 统计：bytes、elapsed_us、crc_errors、sequence_errors、blocks。

电脑检查下行设备计数、CRC/序号错误，上行逐块检查 CRC、序号、实际字节内容并比较设备计数。JSON 中 `passed` 仅表示这次完整性验证通过，吞吐率使用主机实际耗时计算有效 payload。

## 2026-10-06 真实设备结果

证据目录：`artifacts/tests/usb-bandwidth-probe-20261006/`。原固件全片备份 SHA256：`FE1318C080EF002C9C1C7A5199CFC149E072A67AAA31178E241D7B951F12B92C`。备份含设备数据，只保存在本地。

| 版本，每方向约 10 秒 | 电脑→ESP32 | ESP32→电脑 |
| --- | ---: | ---: |
| 初始轮询，单块写入 | 4.020 Mbps | 8.132 Mbps |
| RX 回调通知，单块写入 | 4.026 Mbps | 8.139 Mbps |
| RX 回调通知，16 块批量写入 | 4.069 Mbps | 8.138 Mbps |

三次均无 CRC/序号错误，设备字节/块数与电脑一致。回调和批量写入均未显著改善当前吞吐。测试不等于芯片极限、其他 USB 驱动的性能或长时间稳定性。

同期 H.264 样片为最后 30 秒中心裁剪 640×640、目标 4 Mbps，实际视频流 4.060 Mbps，最大帧 142066 字节；原视频约 60 fps，输出 100 fps 包含重复帧。旧 CDC 吞吐几乎等于视频平均码率，缺乏关键帧突发和调度余量；不应据此承诺稳定实时传输。

## 发送提速对照

证据目录：`artifacts/tests/usb-send-speed-20261006/`。

| 同机每方向约10秒 | 电脑→ESP32 | ESP32→电脑 |
| --- | ---: | ---: |
| CDC EP8192 基线 | 4.070 Mbps | 8.143 Mbps |
| CDC EP512 | 4.070 Mbps | 8.122 Mbps |
| WinUSB 单请求 | 8.066 Mbps | 8.014 Mbps |
| WinUSB 四请求 | 8.083 Mbps | 8.023 Mbps |

全部设备/主机计数一致、CRC/序号错误0。Vendor 同时改变了驱动、FIFO配比和通知路径，不能仅凭对照断言增益全部来自驱动。旧 CDC 的 EP512 配置没有性能增益，32KB CDC RX尝试受Kconfig上限10000约束，未构建/刷写。

单请求每方向30秒确认：下行30343168 B/7408块、30.096666秒、1.008190 MB/s（8.065523 Mbps）；上行30121984 B/7354块、30.017579秒、1.003478 MB/s（8.027825 Mbps），CRC/序号0，`vendor-confirmation.json` 中 passed=true。下行相对同期 CDC 基线约1.98倍。最终 app206704 B，SHA256 `B1E939B7C19431956548DF6BC429D6E4141C74C9837793A3301B023C3F53A8D4`。原生口当前为WinUSB，原COM17消失是预期行为；COM4维护口仍在。

保留失败记录：Vendor 首次构建因无条件包含 CDC 头文件 Failed，条件编译修复；首次枚举 Code43（弱 BOS 回调），显式链接修复；继而 Code28（单接口误用复合设备 OS 描述符），设备级描述符及新版本修复。最终驱动 Service=WINUSB、状态OK/错误码0。

尚未验证 Wi-Fi、双板同时收发、编码/解码延迟、端到端帧延迟及长时间稳定性。恢复原固件使用完整备份、地址 0、flash mode/size/freq `keep`；恢复校验记录在证据目录，不以刷写校验推断实体 HID 验收。

本轮也未验证异常拔线/取消/超时后的恢复；主机取消超时保留在途对象，避免提前释放内核使用的缓冲。吞吐验证不能替代异常路径验收。
