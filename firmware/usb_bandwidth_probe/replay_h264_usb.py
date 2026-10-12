"""按120fps重放Annex-B包到现有WinUSB探针，仅校验字节接收，不是视频解码。"""
import argparse
import json
import time
from pathlib import Path
from benchmark import BLOCK, packet, receive, statistics, write
from benchmark_video_sender import distribution
from winusb_transport import WinUSB


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--video', type=Path, required=True)
    parser.add_argument('--packets', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    stream = args.video.read_bytes()
    packets = json.loads(args.packets.read_text(encoding='utf-8'))['packets']
    if len(packets) != 3600 or sum(int(p['size']) for p in packets) != len(stream):
        raise ValueError('视频包数或字节数不一致')
    result = {'passed': False, 'scope': '120fps录像重放，单板USB接收CRC；不包含编码、Wi-Fi及解码'}
    try:
        with WinUSB(1) as port:
            write(port, packet(1))
            if receive(port) != (1, 0, b'USB_BANDWIDTH_PROBE_V1;FS;VENDOR64'):
                raise ValueError('探针握手失败')
            write(port, packet(10))
            if receive(port) != (10, 0, b''):
                raise ValueError('探针初始化失败')
            start = time.perf_counter()
            carry = bytearray()
            seq, lags, writes, completion_lags = 0, [], [], []
            for i, p in enumerate(packets):
                target = start + i / 120
                delay = target - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)
                before = time.perf_counter()
                lags.append(max(0, before - target) * 1000)
                pos, size = int(p['pos']), int(p['size'])
                carry.extend(stream[pos:pos + size])
                while len(carry) >= BLOCK:
                    write(port, packet(2, seq, bytes(carry[:BLOCK])))
                    del carry[:BLOCK]
                    seq += 1
                now = time.perf_counter()
                writes.append((now - before) * 1000)
                # 尚不足4KB的尾部未发送，故这不是该帧设备接收延迟。
                completion_lags.append(max(0, now - target) * 1000)
            padding = 0
            if carry:
                padding = BLOCK - len(carry)
                write(port, packet(2, seq, bytes(carry) + bytes(padding)))
                seq += 1
            write(port, packet(3))
            kind, _, payload = receive(port)
            elapsed = time.perf_counter() - start
            actual = statistics(payload)
            if kind != 5 or (actual['bytes'], actual['blocks'], actual['crc_errors'], actual['sequence_errors']) != (seq * BLOCK, seq, 0, 0):
                raise ValueError(f'设备实际接收校验失败：{actual}')
            result.update(passed=True, fps=120, frames=len(packets), video_bytes=len(stream),
                          padding_bytes=padding, device=actual, elapsed_seconds=elapsed,
                          average_video_Mbps=len(stream) * 8 / 30 / 1e6,
                          submission_late_ms=distribution(lags), frame_send_blocking_ms=distribution(writes),
                          host_write_completion_late_ms=distribution(completion_lags),
                          submissions_late_over_8_333ms=sum(v > 1000 / 120 for v in lags))
    finally:
        args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False))


if __name__ == '__main__':
    main()
