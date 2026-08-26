"""HidBridge 的 Python UDP 相对鼠标调用库。

保持 Esp32MouseSender 的简单调用接口：初始化时绑定目标地址，调用
move() 发送相对移动或滚轮命令，使用 close() 释放 socket。
"""

import json
import socket


class Esp32MouseSender:
    """复用一个 UDP socket，向初始化时指定的 HidBridge 主机发送命令。"""

    def __init__(self, host, port):
        self._udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._address = (str(host), int(port))

    def move(self, dx, dy, wheel=0, pan=0):
        command = {
            "dx": int(dx),
            "dy": int(dy),
            "wheel": int(wheel),
            "pan": int(pan),
        }
        if not any(command.values()):
            return 0

        payload = json.dumps(command, separators=(",", ":")).encode("utf-8")
        return self._udp.sendto(payload, self._address)

    def close(self):
        if self._udp is not None:
            self._udp.close()
            self._udp = None
