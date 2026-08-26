"""使用项目内 Esp32MouseSender 控制远端鼠标画正方形的实际演示。

目标 IP、端口和正方形参数直接写在本文件顶部；运行脚本不读取命令行或
其他外部输入。执行后会按顺时针分段发送相对移动命令。
"""

from __future__ import annotations

import sys
import time

try:
    from tools.esp32_move import Esp32MouseSender
except ModuleNotFoundError:
    # 直接运行 `python tools/send-remote-mouse-sample.py` 时，脚本目录在 sys.path 中。
    from esp32_move import Esp32MouseSender


# 按实际运行 HidBridge.Host.exe 的电脑修改这两个常量。
TARGET_HOST = "127.0.0.1"
TARGET_PORT = 24814

# 正方形边长、每次发送的步长和相邻命令间隔，全部使用本文件内的固定值。
SQUARE_SIDE_PIXELS = 100
MOVE_STEP_PIXELS = 100
MOVE_INTERVAL_SECONDS = 0.001


def validate_demo_settings() -> None:
    if SQUARE_SIDE_PIXELS <= 0:
        raise ValueError("SQUARE_SIDE_PIXELS 必须大于 0")
    if MOVE_STEP_PIXELS <= 0:
        raise ValueError("MOVE_STEP_PIXELS 必须大于 0")
    if SQUARE_SIDE_PIXELS % MOVE_STEP_PIXELS != 0:
        raise ValueError("SQUARE_SIDE_PIXELS 必须能被 MOVE_STEP_PIXELS 整除")
    if MOVE_INTERVAL_SECONDS < 0:
        raise ValueError("MOVE_INTERVAL_SECONDS 不能小于 0")


def draw_square(sender: Esp32MouseSender) -> int:
    """顺时针发送一个正方形，返回发送的相对移动命令数量。"""

    steps_per_side = SQUARE_SIDE_PIXELS // MOVE_STEP_PIXELS
    directions = ((1, 0), (0, 1), (-1, 0), (0, -1))
    command_count = 0

    for side_index, (direction_x, direction_y) in enumerate(directions, start=1):
        print(f"发送第 {side_index}/4 条边")
        time.sleep(0.01)
        for _ in range(steps_per_side):
            sender.move(
                direction_x * MOVE_STEP_PIXELS,
                direction_y * MOVE_STEP_PIXELS,
            )
            command_count += 1
            if MOVE_INTERVAL_SECONDS:
                time.sleep(MOVE_INTERVAL_SECONDS)

    return command_count


def main() -> int:
    sender = None
    try:
        validate_demo_settings()
        print(f"目标：{TARGET_HOST}:{TARGET_PORT}")
        print(
            "正方形："
            f"边长 {SQUARE_SIDE_PIXELS}，"
            f"步长 {MOVE_STEP_PIXELS}，"
            f"间隔 {MOVE_INTERVAL_SECONDS:.3f} 秒"
        )
        sender = Esp32MouseSender(TARGET_HOST, TARGET_PORT)
        command_count = draw_square(sender)
    except (OSError, ValueError) as exception:
        print(f"演示失败：{exception}", file=sys.stderr)
        return 1
    finally:
        if sender is not None:
            sender.close()

    print(f"正方形演示完成，共发送 {command_count} 条相对移动命令。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
