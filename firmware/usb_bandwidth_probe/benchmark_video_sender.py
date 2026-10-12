"""用真实录像帧模拟固定120fps的持久H.264编码器；不包含截图和网络。"""
import argparse
import json
import mmap
import subprocess
import tempfile
import threading
import time
from pathlib import Path


def distribution(values):
    ordered = sorted(values)
    if not ordered:
        raise ValueError('没有测量样本')
    return {'mean': sum(ordered) / len(ordered),
            'p50': ordered[round((len(ordered) - 1) * .50)],
            'p95': ordered[round((len(ordered) - 1) * .95)],
            'p99': ordered[round((len(ordered) - 1) * .99)],
            'max': ordered[-1]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffmpeg', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--start', type=float, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    result = {'passed': False, 'fps': 120, 'input': '640x640 BGRA',
              'encoder': 'libx264 ultrafast zerolatency threads=4 4Mbps VBV',
              'scope': '预解码原视频帧；包含BGRA转YUV420p和管道；不含截图、USB、Wi-Fi或解码'}
    frame_bytes = 640 * 640 * 4
    proc = None
    with tempfile.NamedTemporaryFile(dir=args.output_dir, suffix='.bgra', delete=False) as temp:
        raw = Path(temp.name)
    try:
        decode_cmd = [str(args.ffmpeg), '-hide_banner', '-loglevel', 'error', '-y',
                      '-ss', str(args.start), '-i', str(args.source), '-t', '30',
                      '-map', '0:v:0', '-an', '-vf', 'crop=640:640:(iw-640)/2:(ih-640)/2,format=bgra,setpts=N/(120*TB)',
                      '-fps_mode', 'passthrough', '-f', 'rawvideo', str(raw)]
        decode = subprocess.run(decode_cmd, capture_output=True)
        (args.output_dir / 'raw-decode.stderr.txt').write_bytes(decode.stderr)
        if decode.returncode or decode.stderr:
            raise RuntimeError('原视频预解码有错误，查看raw-decode.stderr.txt')
        if raw.stat().st_size % frame_bytes:
            raise ValueError('原始帧长度不完整')
        count = raw.stat().st_size // frame_bytes
        if count < 3500:
            raise ValueError('实际帧数不足')
        cmd = [str(args.ffmpeg), '-hide_banner', '-loglevel', 'error',
               '-probesize', '32', '-analyzeduration', '0',
               '-f', 'rawvideo', '-pixel_format', 'bgra', '-video_size', '640x640',
               '-framerate', '120', '-i', 'pipe:0', '-an', '-vf', 'format=yuv420p',
               '-c:v', 'libx264', '-preset', 'ultrafast', '-tune', 'zerolatency',
               '-threads', '4', '-b:v', '4M', '-minrate', '4M', '-maxrate', '4M',
               '-bufsize', '4M', '-g', '120', '-keyint_min', '120', '-sc_threshold', '0',
               '-bf', '0', '-x264-params', 'rc-lookahead=0:sync-lookahead=0',
               '-flush_packets', '1', '-f', 'framecrc', 'pipe:1']
        result['commands'] = {'raw_decode': decode_cmd, 'encoder': cmd}
        submitted = {}
        rows = {}
        errors = []
        with raw.open('rb') as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as frames, \
                (args.output_dir / 'paced-encode.stderr.txt').open('wb') as stderr:
            proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=stderr, bufsize=0)

            def collect():
                try:
                    for line in proc.stdout:
                        now = time.perf_counter()
                        line = line.decode('ascii').strip()
                        if not line or line.startswith('#'):
                            continue
                        parts = [s.strip() for s in line.split(',')]
                        index = int(parts[2])
                        if index not in submitted or index in rows:
                            raise ValueError(f'输出帧编号异常：{index}')
                        rows[index] = {'frame': index, 'size': int(parts[4]),
                                       'latency_ms': (now - submitted[index]) * 1000}
                except Exception as exc:
                    errors.append(str(exc))

            reader = threading.Thread(target=collect, daemon=True)
            reader.start()
            start = time.perf_counter()
            writes, late, reads = [], [], []
            for index in range(count):
                read_start = time.perf_counter()
                payload = frames[index * frame_bytes:(index + 1) * frame_bytes]
                reads.append((time.perf_counter() - read_start) * 1000)
                target = start + index / 120
                delay = target - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)
                before = time.perf_counter()
                late.append(max(0, before - target) * 1000)
                submitted[index] = before
                # 无缓冲管道可能短写，必须提交完整帧后再开始下一帧。
                view = memoryview(payload)
                while view:
                    n = proc.stdin.write(view)
                    if not n:
                        raise IOError('编码器管道短写')
                    view = view[n:]
                writes.append((time.perf_counter() - before) * 1000)
            proc.stdin.close()
            code = proc.wait(timeout=30)
            reader.join(timeout=5)
            elapsed = time.perf_counter() - start
            if code or reader.is_alive() or errors or len(rows) != count:
                raise RuntimeError(f'实际输出不完整：exit={code}, 输入={count}, 输出={len(rows)}, errors={errors}')
        if (args.output_dir / 'paced-encode.stderr.txt').stat().st_size:
            raise RuntimeError('持续编码有错误，查看stderr')
        ordered = [rows[i] for i in range(count)]
        sizes = [r['size'] for r in ordered]
        for i, row in enumerate(ordered):
            row.update(write_ms=writes[i], schedule_late_ms=late[i], raw_read_ms=reads[i])
        (args.output_dir / 'paced-frames.json').write_text(json.dumps(ordered), encoding='utf-8')
        result.update(passed=True, input_frames=count, output_frames=len(rows),
                      elapsed_seconds=elapsed, simulation_duration_seconds=count / 120,
                      encoder_bytes=sum(sizes), average_Mbps=sum(sizes) * 8 / (count / 120) / 1e6,
                      latency_ms=distribution([r['latency_ms'] for r in ordered]),
                      steady_latency_ms=distribution([r['latency_ms'] for r in ordered[120:]]),
                      pipe_write_ms=distribution(writes), schedule_late_ms=distribution(late),
                      raw_read_ms=distribution(reads), frame_size_bytes=distribution(sizes),
                      frames_latency_over_8_333ms=sum(r['latency_ms'] > 1000 / 120 for r in ordered),
                      frames_submission_late_over_8_333ms=sum(v > 1000 / 120 for v in late),
                      first_frame_latency_ms=ordered[0]['latency_ms'])
    finally:
        if proc is not None:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=10)
            for pipe in (proc.stdin, proc.stdout):
                if pipe is not None and not pipe.closed:
                    pipe.close()
        # 仅删除本次明确创建的临时原始帧文件。
        raw.unlink(missing_ok=True)
        (args.output_dir / 'paced-summary.json').write_text(
            json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'commands'}, ensure_ascii=False))


if __name__ == '__main__':
    main()
