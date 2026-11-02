"""KMNET / Makcu 鼠标链路测试器，不依赖厂商 pyd。"""

import ipaddress
import queue
import re
import secrets
import socket
import struct
import threading
import time
import tkinter as tk
from tkinter import filedialog, ttk

import serial
from serial.tools import list_ports


CONNECT = 0xAF3C2828
MOVE = 0xAEDE7345


class KmnetClient:
    """一个会话保留一个 socket；逐包核对来源及完整 ACK，不自动重发移动。"""

    def __init__(self, ip, port, uuid, timeout=1.0):
        address = ipaddress.IPv4Address(ip.strip())
        port = int(port)
        if not 1 <= port <= 65535:
            raise ValueError("端口必须为 1–65535")
        if not re.fullmatch(r"[0-9a-fA-F]{8}", uuid.strip()):
            raise ValueError("UUID 必须是 8 位十六进制字符，例如 AF425414")
        self.endpoint = (str(address), port)
        self.mac = int(uuid.strip(), 16)
        self.timeout = timeout
        self.index = 0
        self.ready = False
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", 0))

    def exchange(self, command, body=b""):
        self.index = (self.index + 1) & 0xFFFFFFFF
        header = struct.pack("<IIII", self.mac, secrets.randbits(32), self.index, command)
        started = time.perf_counter()
        self.sock.sendto(header + body, self.endpoint)
        deadline = started + self.timeout
        ignored = 0
        while True:
            remaining = deadline - time.perf_counter()
            if remaining <= 0:
                raise TimeoutError(f"等待匹配 ACK 超时（已忽略 {ignored} 个无关回包）；移动可能已执行，未自动重发")
            self.sock.settimeout(remaining)
            try:
                reply, source = self.sock.recvfrom(4096)
            except socket.timeout as exc:
                raise TimeoutError(f"等待匹配 ACK 超时（已忽略 {ignored} 个无关回包）；检查地址、服务和两端 UDP 防火墙规则") from exc
            if source == self.endpoint and reply == header:
                return (time.perf_counter() - started) * 1000
            ignored += 1

    def init(self):
        elapsed = self.exchange(CONNECT)
        self.ready = True
        return elapsed

    def move(self, x, y):
        if not self.ready:
            raise RuntimeError("请先连接并收到 init ACK")
        # 完整鼠标状态为 14 个 int32；始终不按下任何按钮。
        return self.exchange(MOVE, struct.pack("<14i", 0, int(x), int(y), *([0] * 11)))

    def close(self):
        """本工具不发送按键/按下命令；退出前尽力发送鼠标全松开状态。"""
        try:
            if self.ready:
                self.move(0, 0)
        finally:
            self.ready = False
            self.sock.close()

    def release(self):
        return self.move(0, 0)

    confirmation = "ACK 匹配，RTT"
    release_confirmation = "鼠标零按钮状态 ACK 匹配"


class MakcuClient:
    """V4 / 老版共有 ASCII 鼠标子集；查询回应不能代替移动消费证明。"""

    confirmation = "命令已写入，版本查询回应，耗时"
    release_confirmation = "已发送五键松开并收到版本查询回应（非逐命令 ACK）"

    def __init__(self, port, timeout=1.0):
        if not re.fullmatch(r"COM[1-9][0-9]*", port.strip(), re.IGNORECASE):
            raise ValueError("请选择或输入 COM 端口，例如 COM4")
        self.ready = False
        self.timeout = timeout
        self.identity = ""
        self.ser = serial.Serial()  # 不带 port 构造，先设置控制线再打开。
        self.ser.port = port.strip().upper()
        self.ser.baudrate = 115200
        self.ser.timeout = 0.05
        self.ser.write_timeout = timeout
        self.ser.dtr = False
        self.ser.rts = False
        try:
            self.ser.open()
            self.ser.dtr = False
            self.ser.rts = False
        except Exception:
            self.ser.close()
            raise

    def exchange(self, commands=()):
        started = time.perf_counter()
        self.ser.reset_input_buffer()
        payload = b"".join(command.encode("ascii") + b"\r\n" for command in commands)
        payload += b"km.version()\r\n"
        if self.ser.write(payload) != len(payload):
            raise OSError("Makcu 串口写入不完整；不重发移动")
        deadline = started + self.timeout
        buffer = bytearray()
        while time.perf_counter() < deadline:
            self.ser.timeout = min(0.05, max(0.001, deadline - time.perf_counter()))
            buffer.extend(self.ser.read(1))
            if len(buffer) > 8192:
                raise RuntimeError("Makcu 回应超长或存在其他数据流，请关闭按钮订阅后再测试")
            if buffer.endswith(b">>> "):
                text = buffer.decode("ascii", errors="replace")
                if "ERR" in text or "km.error(" in text:
                    raise RuntimeError(f"Makcu 返回错误：{text.strip()}")
                identities = ("km.MAKCU", "km.ESP32-S3-HID-Bridge-MakcuASCII-1")
                if any(line.strip() in identities for line in text.splitlines()):
                    self.identity = next(value for value in identities if value in text)
                    return (time.perf_counter() - started) * 1000
                buffer.clear()  # 老版 setter 有独立提示符，继续等待版本查询结果。
        raise TimeoutError("Makcu 版本查询无有效回应；检查 M 端口、115200 模式及 Host 是否已真正退出。移动可能已执行，不自动重发")

    def init(self):
        elapsed = self.exchange()
        self.ready = True
        return elapsed

    def move(self, x, y):
        if not self.ready:
            raise RuntimeError("请先连接 Makcu 并验证版本回应")
        return self.exchange((f"km.move({int(x)},{int(y)})",))

    def release(self):
        return self.exchange(tuple(f"km.{name}(0)" for name in ("left", "right", "middle", "side1", "side2")))

    def close(self):
        try:
            if self.ready:
                self.release()
        finally:
            self.ready = False
            self.ser.close()


class App:
    def __init__(self, root):
        self.root = root
        root.title("KMNET / Makcu 鼠标链路测试")
        root.geometry("850x800")
        root.minsize(780, 760)
        self.client = None
        self.busy = False
        self.closing = False
        self.cancel = threading.Event()
        self.events = queue.Queue()
        self.lines = []
        self.interface = tk.StringVar(value="KMNET（UDP）")
        self.com_port = tk.StringVar(value="")
        self.ip = tk.StringVar(value="")
        self.port = tk.StringVar(value="24814")
        self.uuid = tk.StringVar(value="AF425414")
        self.distance = tk.StringVar(value="20")
        self.dx = tk.StringVar(value="20")
        self.dy = tk.StringVar(value="0")
        self.status = tk.StringVar(value="未连接")
        frame = ttk.Frame(root, padding=16)
        frame.pack(fill="both", expand=True)
        ttk.Label(frame, text="KMNET / Makcu 鼠标链路测试", font=("Microsoft YaHei UI", 17, "bold")).pack(anchor="w")
        selector = ttk.Frame(frame)
        selector.pack(fill="x", pady=(8, 6))
        ttk.Label(selector, text="接口").pack(side="left", padx=(0, 8))
        self.interface_box = ttk.Combobox(selector, textvariable=self.interface, values=("KMNET（UDP）", "Makcu（串口）"), state="readonly", width=20)
        self.interface_box.pack(side="left")
        self.interface_box.bind("<<ComboboxSelected>>", lambda _: self.select_interface())
        self.hint = tk.StringVar()
        ttk.Label(frame, textvariable=self.hint, wraplength=780).pack(anchor="w", pady=(0, 8))
        fields = ttk.Frame(frame)
        self.network_fields = fields
        fields.pack(fill="x")
        self.entries = []
        for label, var, width in (("目标 IPv4", self.ip, 20), ("端口", self.port, 8), ("UUID（8 位十六进制）", self.uuid, 14)):
            ttk.Label(fields, text=label).pack(side="left", padx=(0, 5))
            entry = ttk.Entry(fields, textvariable=var, width=width)
            entry.pack(side="left", padx=(0, 12))
            self.entries.append(entry)
        self.serial_fields = ttk.Frame(frame)
        ttk.Label(self.serial_fields, text="M 板 COM 口").pack(side="left", padx=(0, 8))
        self.com_box = ttk.Combobox(self.serial_fields, textvariable=self.com_port, width=14)
        self.com_box.pack(side="left", padx=(0, 8))
        self.refresh_button = ttk.Button(self.serial_fields, text="刷新串口", command=self.refresh_ports)
        self.refresh_button.pack(side="left")
        ttk.Label(self.serial_fields, text="115200 / 8N1；V4 和老版共用 ASCII 移动接口").pack(side="left", padx=12)
        connection = ttk.Frame(frame)
        self.connection_row = connection
        connection.pack(fill="x", pady=12)
        self.connect_button = ttk.Button(connection, text="连接 / 测试 init", command=self.connect)
        self.connect_button.pack(side="left")
        self.disconnect_button = ttk.Button(connection, text="断开", command=self.disconnect)
        self.disconnect_button.pack(side="left", padx=8)
        ttk.Label(connection, textvariable=self.status).pack(side="left", padx=8)
        ttk.Label(frame, text="协议回应只证明服务或串口查询可达。是否实际移动，请观察 P 板连接的目标电脑。", wraplength=780, foreground="#945500").pack(anchor="w")
        movement = ttk.LabelFrame(frame, text="鼠标相对移动（不点击、不发送键盘）", padding=10)
        movement.pack(fill="x", pady=12)
        row = ttk.Frame(movement)
        row.pack(fill="x")
        ttk.Label(row, text="方向步长（1–500）").pack(side="left")
        ttk.Entry(row, textvariable=self.distance, width=7).pack(side="left", padx=8)
        self.move_buttons = []
        for label, x, y in (("← 左", -1, 0), ("→ 右", 1, 0), ("↑ 上", 0, -1), ("↓ 下", 0, 1)):
            button = ttk.Button(row, text=label, command=lambda x=x, y=y: self.direction(x, y))
            button.pack(side="left", padx=3)
            self.move_buttons.append(button)
        row = ttk.Frame(movement)
        row.pack(fill="x", pady=(10, 0))
        for label, var in (("X", self.dx), ("Y", self.dy)):
            ttk.Label(row, text=label).pack(side="left")
            ttk.Entry(row, textvariable=var, width=8).pack(side="left", padx=8)
        for label, callback in (("发送一次（±500）", self.custom_move), ("画方框", self.square)):
            button = ttk.Button(row, text=label, command=callback)
            button.pack(side="left", padx=4)
            self.move_buttons.append(button)
        self.stop_button = ttk.Button(row, text="停止测试", command=self.cancel.set)
        self.stop_button.pack(side="left", padx=4)
        ttk.Label(movement, text="方框按步长依次右、下、左、上，每边间隔 300 ms；无自动重试，停止不会自动回到起点。", wraplength=750).pack(anchor="w", pady=(10, 0))
        actions = ttk.Frame(frame)
        actions.pack(fill="x")
        ttk.Label(actions, text="诊断日志（网络往返 / 串口查询耗时均非鼠标输入延迟）").pack(side="left")
        ttk.Button(actions, text="保存日志", command=self.save_log).pack(side="right")
        self.logbox = tk.Text(frame, height=15, wrap="word", state="disabled", font=("Microsoft YaHei UI", 10))
        self.logbox.pack(fill="both", expand=True, pady=8)
        self.log("就绪。KMNET 使用 IP / 端口 / 8 位 UUID；Makcu 使用本机 COM 串口。测试动作相同。")
        self.log("KMNET init 会接管当前会话；Makcu 需先在 Host 开启模式，再从托盘真正退出 Host。")
        self.refresh_ports()
        self.select_interface()
        self.refresh()
        root.protocol("WM_DELETE_WINDOW", self.close_window)
        root.after(60, self.poll)

    def log(self, message):
        line = time.strftime("[%H:%M:%S] ") + message
        self.lines.append(line)
        self.logbox.configure(state="normal")
        self.logbox.insert("end", line + "\n")
        self.logbox.see("end")
        self.logbox.configure(state="disabled")

    def refresh(self):
        connected = self.client is not None and self.client.ready
        self.connect_button.configure(state="disabled" if self.busy or connected else "normal")
        self.disconnect_button.configure(state="normal" if connected and not self.busy else "disabled")
        for button in self.move_buttons:
            button.configure(state="normal" if connected and not self.busy else "disabled")
        for entry in self.entries:
            entry.configure(state="disabled" if connected or self.busy else "normal")
        self.interface_box.configure(state="disabled" if connected or self.busy else "readonly")
        self.com_box.configure(state="disabled" if connected or self.busy else "normal")
        self.refresh_button.configure(state="disabled" if connected or self.busy else "normal")
        self.stop_button.configure(state="normal" if self.busy else "disabled")

    def refresh_ports(self):
        self.com_box.configure(values=[port.device for port in list_ports.comports()])

    def select_interface(self):
        makcu = self.interface.get().startswith("Makcu")
        self.network_fields.pack_forget()
        self.serial_fields.pack_forget()
        (self.serial_fields if makcu else self.network_fields).pack(fill="x", before=self.connection_row)
        self.hint.set("串口直连本机 M 板；先在 Host 开启 Makcu，再真正退出 Host。工具不自动切模式或复位。" if makcu else "在副机运行；IP / 端口填写运行 HidBridge.Host 的电脑显示的 UDP 地址。")
        self.connect_button.configure(text="连接 / 查询版本" if makcu else "连接 / 测试 init")

    def launch(self, work):
        if self.busy:
            return
        self.busy = True
        self.cancel.clear()
        self.refresh()

        def run():
            try:
                work()
            except Exception as exc:
                self.events.put(("log", f"Failed：{exc}"))
                self.events.put(("status", "测试失败；请查看日志"))
                self.drop_client()
            finally:
                self.events.put(("done", None))
        threading.Thread(target=run, daemon=True).start()

    def drop_client(self):
        client, self.client = self.client, None
        if client:
            try:
                was_ready = client.ready
                client.close()
                message = client.release_confirmation if was_ready else "握手未完成，未发送输入或松开命令"
                self.events.put(("log", f"{message}；连接已关闭。"))
            except Exception as exc:
                self.events.put(("log", f"松开状态未确认：{exc}；连接已关闭。本工具从未发送按下命令。"))

    def connect(self):
        ip, port, uuid = self.ip.get(), self.port.get(), self.uuid.get()
        makcu, com = self.interface.get().startswith("Makcu"), self.com_port.get()

        def work():
            if makcu:
                self.events.put(("log", f"打开 Makcu {com} / 115200；DTR/RTS 打开前后均设为低，实机无复位尚未验证。"))
                self.client = MakcuClient(com)
                elapsed = self.client.init()
                self.events.put(("log", f"Pass：版本回应 {self.client.identity}，查询耗时={elapsed:.2f} ms；目标移动尚未验证。"))
                self.events.put(("status", "已连接 / Makcu 版本回应"))
                return
            self.client = KmnetClient(ip, port, uuid)
            self.events.put(("log", f"发送 init → {self.client.endpoint[0]}:{self.client.endpoint[1]}；本地 UDP 端口 {self.client.sock.getsockname()[1]}"))
            elapsed = self.client.init()
            self.events.put(("log", f"Pass：收到完整匹配 init ACK，RTT={elapsed:.2f} ms；目标鼠标移动尚未验证。"))
            self.events.put(("status", "已连接 / init ACK 匹配"))
        self.launch(work)

    def bounded(self, value, low, high):
        try:
            result = int(value)
            if not low <= result <= high:
                raise ValueError()
            return result
        except ValueError:
            raise ValueError(f"请输入 {low} 到 {high} 之间的整数")

    def direction(self, x, y):
        try:
            step = self.bounded(self.distance.get(), 1, 500)
            self.moves([(x * step, y * step)])
        except ValueError as exc:
            self.log(str(exc))

    def custom_move(self):
        try:
            self.moves([(self.bounded(self.dx.get(), -500, 500), self.bounded(self.dy.get(), -500, 500))])
        except ValueError as exc:
            self.log(str(exc))

    def square(self):
        try:
            step = self.bounded(self.distance.get(), 1, 500)
            self.moves([(step, 0), (0, step), (-step, 0), (0, -step)])
        except ValueError as exc:
            self.log(str(exc))

    def moves(self, points):
        def work():
            for index, (x, y) in enumerate(points):
                if self.cancel.is_set():
                    self.events.put(("log", "测试已停止；未发送剩余位移。"))
                    break
                elapsed = self.client.move(x, y)
                self.events.put(("log", f"move({x}, {y})：{self.client.confirmation}={elapsed:.2f} ms；请观察目标光标。"))
                if index + 1 < len(points) and self.cancel.wait(0.3):
                    break
            # 停止路径也发出零按钮完整状态，并保留 socket 接收确认。
            self.client.release()
            self.events.put(("log", f"本轮结束：{self.client.release_confirmation}。目标实际位移需人工确认。"))
        self.launch(work)

    def disconnect(self):
        def work():
            self.drop_client()
            self.events.put(("status", "已断开"))
        self.launch(work)

    def poll(self):
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "log":
                    self.log(value)
                elif kind == "status":
                    self.status.set(value)
                elif kind == "done":
                    self.busy = False
                    self.refresh()
        except queue.Empty:
            pass
        if self.closing and not self.busy:
            if self.client:
                self.disconnect()
            else:
                self.root.destroy()
                return
        self.root.after(60, self.poll)

    def close_window(self):
        self.closing = True
        self.cancel.set()
        self.status.set("正在停止并关闭连接…")

    def save_log(self):
        path = filedialog.asksaveasfilename(defaultextension=".txt", initialfile="kmnet-test-log.txt", filetypes=[("文本日志", "*.txt")])
        if path:
            try:
                with open(path, "w", encoding="utf-8-sig") as stream:
                    stream.write("\n".join(self.lines) + "\n")
                self.log("日志已保存。")
            except OSError as exc:
                self.log(f"保存失败：{exc}")


if __name__ == "__main__":
    App(tk.Tk()).root.mainloop()
