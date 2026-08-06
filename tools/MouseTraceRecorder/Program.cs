using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

internal static class Program
{
    private const int WM_INPUT = 0x00FF;
    private const int RID_INPUT = 0x10000003;
    private const int RIM_TYPEMOUSE = 0;
    private const uint RIDEV_INPUTSINK = 0x00000100;
    private const ushort HID_USAGE_PAGE_GENERIC = 0x01;
    private const ushort HID_USAGE_GENERIC_MOUSE = 0x02;
    private const ushort RI_MOUSE_BUTTON_4_DOWN = 0x0040;
    private const ushort RI_MOUSE_BUTTON_4_UP = 0x0080;
    private const ushort RI_MOUSE_BUTTON_5_DOWN = 0x0100;
    private const ushort RI_MOUSE_BUTTON_5_UP = 0x0200;
    private const ushort RI_MOUSE_WHEEL = 0x0400;
    private const ushort RI_MOUSE_HWHEEL = 0x0800;

    [STAThread]
    private static void Main(string[] args)
    {
        var options = Options.Parse(args);
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(options.Output))!);

        ApplicationConfiguration.Initialize();
        using var recorder = new Recorder(options);
        using var form = new CaptureWindow(recorder);
        recorder.Attach(form);
        Application.Run(form);
    }

    private sealed class CaptureWindow : Form
    {
        private readonly Recorder _recorder;

        public CaptureWindow(Recorder recorder)
        {
            _recorder = recorder;
            ShowInTaskbar = false;
            FormBorderStyle = FormBorderStyle.FixedToolWindow;
            WindowState = FormWindowState.Minimized;
            Opacity = 0;
            Width = 1;
            Height = 1;
            Text = "HidBridge Mouse Trace Recorder";
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            _recorder.Start(Handle);
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == WM_INPUT)
            {
                _recorder.OnRawInput(m.LParam);
            }
            base.WndProc(ref m);
        }

        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            _recorder.Stop();
            base.OnFormClosed(e);
        }
    }

    private sealed class Recorder : IDisposable
    {
        private readonly Options _options;
        private readonly Stopwatch _clock = Stopwatch.StartNew();
        private readonly List<long> _intervalsUs = new();
        private readonly object _sync = new();
        private StreamWriter? _writer;
        private CaptureWindow? _window;
        private long _lastEventTicks;
        private long _eventCount;
        private long _totalDx;
        private long _totalDy;
        private long _totalWheel;
        private long _totalHWheel;
        private DateTimeOffset _startedAt;
        private bool _stopped;

        public Recorder(Options options) => _options = options;

        public void Attach(CaptureWindow window) => _window = window;

        public void Start(IntPtr hwnd)
        {
            _startedAt = DateTimeOffset.Now;
            _writer = new StreamWriter(_options.Output, false, new UTF8Encoding(false)) { AutoFlush = true };
            _writer.WriteLine("kind,label,local_time,monotonic_us,interval_us,device,dx,dy,wheel,hwheel,button_flags,cursor_x,cursor_y");

            var devices = new[]
            {
                new RawInputDevice
                {
                    UsagePage = HID_USAGE_PAGE_GENERIC,
                    Usage = HID_USAGE_GENERIC_MOUSE,
                    Flags = RIDEV_INPUTSINK,
                    Target = hwnd,
                },
            };
            if (!RegisterRawInputDevices(devices, (uint)devices.Length, (uint)Marshal.SizeOf<RawInputDevice>()))
            {
                throw new InvalidOperationException($"RegisterRawInputDevices 失败，Win32={Marshal.GetLastWin32Error()}");
            }

            Console.WriteLine($"开始记录 {_options.Seconds} 秒，标签={_options.Label}");
            Console.WriteLine($"Raw Input CSV: {Path.GetFullPath(_options.Output)}");
            Console.WriteLine("请在记录期间执行固定路线移动；结束后会生成同名 .summary.txt");
            _ = Task.Run(async () =>
            {
                await Task.Delay(TimeSpan.FromSeconds(_options.Seconds));
                if (_window is not null && !_window.IsDisposed)
                {
                    _window.BeginInvoke(_window.Close);
                }
            });
        }

        public void OnRawInput(IntPtr rawInput)
        {
            uint size = 0;
            if (GetRawInputData(rawInput, RID_INPUT, IntPtr.Zero, ref size, (uint)Marshal.SizeOf<RawInputHeader>()) == uint.MaxValue || size == 0)
            {
                return;
            }

            var buffer = Marshal.AllocHGlobal((int)size);
            try
            {
                if (GetRawInputData(rawInput, RID_INPUT, buffer, ref size, (uint)Marshal.SizeOf<RawInputHeader>()) == uint.MaxValue)
                {
                    return;
                }

                var input = Marshal.PtrToStructure<RawInput>(buffer);
                if (input.Header.Type != RIM_TYPEMOUSE)
                {
                    return;
                }

                var nowTicks = _clock.ElapsedTicks;
                var nowUs = ToMicroseconds(nowTicks);
                var intervalUs = _lastEventTicks == 0 ? 0 : ToMicroseconds(nowTicks - _lastEventTicks);
                _lastEventTicks = nowTicks;
                var device = GetDeviceName(input.Header.Device);
                GetCursorPos(out var cursor);
                var mouse = input.Mouse;
                var wheel = (mouse.ButtonFlags & RI_MOUSE_WHEEL) != 0 ? (short)mouse.ButtonData : 0;
                var hWheel = (mouse.ButtonFlags & RI_MOUSE_HWHEEL) != 0 ? (short)mouse.ButtonData : 0;

                lock (_sync)
                {
                    _eventCount++;
                    _totalDx += mouse.LastX;
                    _totalDy += mouse.LastY;
                    _totalWheel += wheel;
                    _totalHWheel += hWheel;
                    if (intervalUs > 0)
                    {
                        _intervalsUs.Add(intervalUs);
                    }
                    _writer?.WriteLine(string.Join(',',
                        "raw",
                        Csv(_options.Label),
                        Csv(DateTimeOffset.Now.ToString("O")),
                        nowUs.ToString(CultureInfo.InvariantCulture),
                        intervalUs.ToString(CultureInfo.InvariantCulture),
                        Csv(device),
                        mouse.LastX.ToString(CultureInfo.InvariantCulture),
                        mouse.LastY.ToString(CultureInfo.InvariantCulture),
                        wheel.ToString(CultureInfo.InvariantCulture),
                        hWheel.ToString(CultureInfo.InvariantCulture),
                        mouse.ButtonFlags.ToString(CultureInfo.InvariantCulture),
                        cursor.X.ToString(CultureInfo.InvariantCulture),
                        cursor.Y.ToString(CultureInfo.InvariantCulture)));
                }
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }

        public void Stop()
        {
            lock (_sync)
            {
                if (_stopped)
                {
                    return;
                }
                _stopped = true;
                var summaryPath = Path.ChangeExtension(_options.Output, ".summary.txt");
                using var summary = new StreamWriter(summaryPath, false, new UTF8Encoding(false));
                summary.WriteLine($"label={_options.Label}");
                summary.WriteLine($"started_local={_startedAt:O}");
                summary.WriteLine($"duration_seconds={_clock.Elapsed.TotalSeconds:F3}");
                summary.WriteLine($"raw_event_count={_eventCount}");
                summary.WriteLine($"total_dx={_totalDx}");
                summary.WriteLine($"total_dy={_totalDy}");
                summary.WriteLine($"total_wheel={_totalWheel}");
                summary.WriteLine($"total_hwheel={_totalHWheel}");
                if (_intervalsUs.Count > 0)
                {
                    _intervalsUs.Sort();
                    summary.WriteLine($"interval_min_us={_intervalsUs[0]}");
                    summary.WriteLine($"interval_median_us={Percentile(0.50)}");
                    summary.WriteLine($"interval_p95_us={Percentile(0.95)}");
                    summary.WriteLine($"interval_max_us={_intervalsUs[^1]}");
                    summary.WriteLine($"raw_rate_hz={_eventCount / Math.Max(0.001, _clock.Elapsed.TotalSeconds):F2}");
                }
                else
                {
                    summary.WriteLine("intervals=none");
                }
                _writer?.Dispose();
                _writer = null;
                Console.WriteLine($"记录完成：{Path.GetFullPath(_options.Output)}");
                Console.WriteLine($"摘要：{Path.GetFullPath(summaryPath)}");
            }
        }

        private long Percentile(double percentile)
        {
            var index = (int)Math.Clamp(Math.Round((_intervalsUs.Count - 1) * percentile), 0, _intervalsUs.Count - 1);
            return _intervalsUs[index];
        }

        public void Dispose() => Stop();

        private static long ToMicroseconds(long ticks) => ticks * 1_000_000 / Stopwatch.Frequency;

        private static string Csv(string value) => $"\"{value.Replace("\"", "\"\"")}\"";

        private static string GetDeviceName(IntPtr device)
        {
            if (device == IntPtr.Zero)
            {
                return "unknown";
            }
            uint size = 0;
            GetRawInputDeviceInfo(device, 0x20000007, null, ref size);
            if (size == 0)
            {
                return $"handle:{device}";
            }
            var name = new StringBuilder((int)size + 1);
            return GetRawInputDeviceInfo(device, 0x20000007, name, ref size) >= 0 ? name.ToString() : $"handle:{device}";
        }
    }

    private sealed record Options(string Label, int Seconds, string Output)
    {
        public static Options Parse(string[] args)
        {
            var label = "unknown";
            var seconds = 30;
            var output = $"mouse-trace-{DateTime.Now:yyyyMMdd-HHmmss}.csv";
            for (var i = 0; i < args.Length; i++)
            {
                switch (args[i].ToLowerInvariant())
                {
                    case "--label" when i + 1 < args.Length:
                        label = args[++i];
                        break;
                    case "--seconds" when i + 1 < args.Length && int.TryParse(args[++i], out var parsed):
                        seconds = Math.Clamp(parsed, 1, 3600);
                        break;
                    case "--output" when i + 1 < args.Length:
                        output = args[++i];
                        break;
                    case "--help":
                    case "-h":
                        Console.WriteLine("用法：MouseTraceRecorder.exe --label BLE|USB --seconds 30 --output trace.csv");
                        Environment.Exit(0);
                        break;
                }
            }
            return new Options(label, seconds, output);
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct RawInputDevice
    {
        public ushort UsagePage;
        public ushort Usage;
        public uint Flags;
        public IntPtr Target;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct RawInputHeader
    {
        public uint Type;
        public uint Size;
        public IntPtr Device;
        public IntPtr WParam;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct RawInput
    {
        public RawInputHeader Header;
        public RawMouse Mouse;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct RawMouse
    {
        [FieldOffset(0)] public ushort Flags;
        [FieldOffset(2)] public ushort ButtonFlags;
        [FieldOffset(4)] public ushort ButtonData;
        [FieldOffset(8)] public uint RawButtons;
        [FieldOffset(12)] public int LastX;
        [FieldOffset(16)] public int LastY;
        [FieldOffset(20)] public uint ExtraInformation;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Point
    {
        public int X;
        public int Y;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RegisterRawInputDevices(RawInputDevice[] devices, uint count, uint size);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetRawInputData(IntPtr rawInput, uint command, IntPtr data, ref uint size, uint headerSize);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern int GetRawInputDeviceInfo(IntPtr device, uint command, StringBuilder? data, ref uint size);

    [DllImport("user32.dll")]
    private static extern bool GetCursorPos(out Point point);
}
