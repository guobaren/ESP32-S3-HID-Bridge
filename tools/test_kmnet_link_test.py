"""独立 UDP 对端验证真实数据报，不连接板卡或注入本机鼠标。"""

import socket
import struct
import threading
import time
import tkinter as tk
import unittest
from unittest.mock import patch

from kmnet_link_test import App, KmnetClient, MakcuClient


class SerialPeer:
    """只模拟串口字节流，记录实际写入、控制线及生命周期。"""

    def __init__(self, legacy=False, error=None):
        self.legacy = legacy
        self.error = error
        self.pending = bytearray()
        self.writes = []
        self.is_open = False
        self.open_state = None

    def open(self):
        self.open_state = (self.port, self.baudrate, self.dtr, self.rts)
        self.is_open = True

    def reset_input_buffer(self):
        self.pending.clear()

    def write(self, data):
        self.writes.append(data)
        for command in data.splitlines():
            if command == b"km.version()":
                if self.error == "silent":
                    continue
                identity = b"km.ESP32-S3-HID-Bridge-MakcuASCII-1" if self.legacy else b"km.MAKCU"
                if self.error == "identity":
                    identity = b"unknown"
                self.pending.extend(identity + b"\r\n>>> ")
            elif self.error == "setter":
                self.pending.extend(b"km.error(argument)\r\n>>> " if self.legacy else b"ERR\r\n>>> ")
            elif self.legacy:
                self.pending.extend(b"\r\n>>> ")
        return len(data)

    def read(self, count):
        result = bytes(self.pending[:count])
        del self.pending[:count]
        if not result:
            time.sleep(0.001)
        return result

    def close(self):
        self.is_open = False


class MakcuTests(unittest.TestCase):
    def test_gui_failed_handshake_closes_without_false_success(self):
        root = tk.Tk()
        root.withdraw()
        app = App(root)
        peer = SerialPeer(error="silent")
        try:
            with patch("kmnet_link_test.serial.Serial", return_value=peer):
                app.interface.set("Makcu（串口）")
                app.com_port.set("COM4")
                app.connect()
                deadline = time.monotonic() + 3
                while app.busy and time.monotonic() < deadline:
                    root.update()
                    time.sleep(0.01)
                self.assertFalse(app.busy)
                self.assertIsNone(app.client)
                self.assertFalse(peer.is_open)
                self.assertTrue(any("Failed" in line for line in app.lines))
                self.assertFalse(any("Pass：" in line or "收到版本查询回应" in line for line in app.lines))
                self.assertEqual(peer.writes, [b"km.version()\r\n"])
        finally:
            root.destroy()

    def test_both_versions_move_release_and_low_control_lines(self):
        for legacy in (False, True):
            with self.subTest(legacy=legacy):
                peer = SerialPeer(legacy)
                with patch("kmnet_link_test.serial.Serial", return_value=peer) as factory:
                    client = MakcuClient("com4", timeout=0.1)
                    factory.assert_called_once_with()
                    self.assertEqual(peer.open_state, ("COM4", 115200, False, False))
                    self.assertFalse(peer.dtr)
                    self.assertFalse(peer.rts)
                    client.init()
                    client.move(20, -10)
                    client.close()
                self.assertFalse(peer.is_open)
                self.assertEqual(peer.writes[0], b"km.version()\r\n")
                self.assertEqual(peer.writes[1], b"km.move(20,-10)\r\nkm.version()\r\n")
                self.assertEqual(peer.writes[2], b"km.left(0)\r\nkm.right(0)\r\nkm.middle(0)\r\nkm.side1(0)\r\nkm.side2(0)\r\nkm.version()\r\n")

    def test_timeout_unknown_identity_and_setter_errors_fail(self):
        for legacy in (False, True):
            for error in ("silent", "identity", "setter"):
                with self.subTest(legacy=legacy, error=error):
                    peer = SerialPeer(legacy, error)
                    with patch("kmnet_link_test.serial.Serial", return_value=peer):
                        client = MakcuClient("COM4", timeout=0.025)
                        try:
                            if error == "setter":
                                client.init()
                                with self.assertRaises(RuntimeError):
                                    client.move(20, 0)
                                self.assertEqual(len(peer.writes), 2)
                            else:
                                with self.assertRaises(TimeoutError):
                                    client.init()
                                self.assertFalse(client.ready)
                        finally:
                            peer.close()

    def test_gui_same_test_actions_on_makcu_and_exit(self):
        root = tk.Tk()
        root.withdraw()
        app = App(root)
        peer = SerialPeer(legacy=True)

        def wait():
            deadline = time.monotonic() + 3
            while app.busy and time.monotonic() < deadline:
                root.update()
                time.sleep(0.005)
            self.assertFalse(app.busy)

        try:
            with patch("kmnet_link_test.serial.Serial", return_value=peer):
                app.interface.set("Makcu（串口）")
                app.select_interface()
                app.com_port.set("COM4")
                app.connect()
                wait()
                self.assertIn("Makcu", app.status.get())
                self.assertEqual(str(app.interface_box.cget("state")), "disabled")
                app.direction(-1, 0)
                wait()
                app.dx.set("12")
                app.dy.set("-7")
                app.custom_move()
                wait()
                app.square()
                wait()
                moves = [part for data in peer.writes for part in data.splitlines() if part.startswith(b"km.move(")]
                self.assertEqual(moves, [b"km.move(-20,0)", b"km.move(12,-7)", b"km.move(20,0)", b"km.move(0,20)", b"km.move(-20,0)", b"km.move(0,-20)"])
                app.disconnect()
                wait()
                self.assertFalse(peer.is_open)
                self.assertIsNone(app.client)
                self.assertTrue(any("非逐命令 ACK" in line for line in app.lines))
                self.assertFalse(any("Failed" in line for line in app.lines))
        finally:
            peer.close()
            root.destroy()


class Peer:
    def __init__(self, mode="echo"):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.05)
        self.port = self.sock.getsockname()[1]
        self.mode = mode
        self.packets = []
        self.sources = []
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run)

    def run(self):
        while not self.stop.is_set():
            try:
                data, source = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            self.packets.append(data)
            self.sources.append(source)
            if self.mode == "echo":
                # 先返回错误序号，客户端必须忽略它再接受匹配回包。
                wrong = bytearray(data[:16])
                wrong[8] ^= 0x80
                self.sock.sendto(wrong, source)
                self.sock.sendto(data[:16], source)
            elif self.mode == "wrong":
                self.sock.sendto(b"invalid", source)
            elif self.mode == "spoof":
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as other:
                    other.sendto(data[:16], source)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.stop.set()
        self.thread.join(2)
        self.sock.close()


class LinkTests(unittest.TestCase):
    def test_wire_move_release_and_persistent_endpoint(self):
        with Peer() as peer:
            client = KmnetClient("127.0.0.1", peer.port, "AF425414", timeout=0.2)
            self.assertGreater(client.init(), 0)
            self.assertGreater(client.move(20, -10), 0)
            client.close()
            self.assertEqual(len(peer.packets), 3)
            self.assertEqual(len(set(peer.sources)), 1)
            init, move, release = peer.packets
            mac, _, index, command = struct.unpack("<IIII", init)
            self.assertEqual((mac, index, command), (0xAF425414, 1, 0xAF3C2828))
            self.assertEqual(len(move), 72)
            self.assertEqual(struct.unpack_from("<I", move, 12)[0], 0xAEDE7345)
            self.assertEqual(struct.unpack("<14i", move[16:]), (0, 20, -10) + (0,) * 11)
            self.assertEqual(struct.unpack("<14i", release[16:]), (0,) * 14)

    def test_wrong_ack_fails_without_retry(self):
        with Peer("wrong") as peer:
            client = KmnetClient("127.0.0.1", peer.port, "AF425414", timeout=0.08)
            try:
                with self.assertRaises(TimeoutError):
                    client.init()
                self.assertFalse(client.ready)
                self.assertEqual(len(peer.packets), 1)
            finally:
                client.close()

    def test_timeout_is_failure(self):
        with Peer("silent") as peer:
            client = KmnetClient("127.0.0.1", peer.port, "AF425414", timeout=0.08)
            try:
                with self.assertRaises(TimeoutError):
                    client.init()
                self.assertFalse(client.ready)
            finally:
                client.close()

    def test_matching_ack_from_wrong_source_is_rejected(self):
        with Peer("spoof") as peer:
            client = KmnetClient("127.0.0.1", peer.port, "AF425414", timeout=0.08)
            try:
                with self.assertRaises(TimeoutError):
                    client.init()
                self.assertFalse(client.ready)
            finally:
                client.close()

    def test_gui_stop_then_exit_releases_and_closes(self):
        root = tk.Tk()
        root.withdraw()
        app = App(root)
        try:
            with Peer() as peer:
                app.client = KmnetClient("127.0.0.1", peer.port, "AF425414", timeout=0.2)
                app.client.init()
                app.square()
                deadline = time.monotonic() + 2
                while len(peer.packets) < 2 and time.monotonic() < deadline:
                    time.sleep(0.005)
                app.close_window()
                while time.monotonic() < deadline:
                    try:
                        root.update()
                    except tk.TclError:
                        break
                    if app.client is None and not app.busy:
                        break
                    time.sleep(0.01)
                self.assertIsNone(app.client)
                self.assertFalse(app.busy)
                moves = [struct.unpack("<14i", data[16:])[1:3] for data in peer.packets[1:]]
                self.assertEqual(moves, [(20, 0), (0, 0), (0, 0)])
        finally:
            try:
                root.destroy()
            except tk.TclError:
                pass

    def test_invalid_inputs(self):
        for ip, port, uuid in (("bad", 24814, "AF425414"), ("127.0.0.1", 0, "AF425414"), ("127.0.0.1", 65536, "AF425414"), ("127.0.0.1", 24814, "xyz")):
            with self.subTest(ip=ip, port=port, uuid=uuid):
                with self.assertRaises(ValueError):
                    KmnetClient(ip, port, uuid)

    def test_gui_connect_square_disconnect(self):
        root = tk.Tk()
        root.withdraw()
        app = App(root)

        def wait():
            deadline = time.monotonic() + 4
            while app.busy and time.monotonic() < deadline:
                root.update()
                time.sleep(0.01)
            self.assertFalse(app.busy, "UI 工作线程未结束")

        try:
            with Peer() as peer:
                app.ip.set("127.0.0.1")
                app.port.set(str(peer.port))
                app.connect()
                wait()
                self.assertIn("init ACK", app.status.get())
                app.square()
                wait()
                moves = [struct.unpack("<14i", data[16:])[1:3] for data in peer.packets[1:]]
                self.assertEqual(moves, [(20, 0), (0, 20), (-20, 0), (0, -20), (0, 0)])
                app.disconnect()
                wait()
                self.assertIsNone(app.client)
                self.assertEqual(app.status.get(), "已断开")
                self.assertEqual(struct.unpack("<14i", peer.packets[-1][16:]), (0,) * 14)
                self.assertFalse(any("Failed" in line for line in app.lines))
        finally:
            if app.client:
                app.client.sock.close()
            root.destroy()


if __name__ == "__main__":
    unittest.main(verbosity=2)
