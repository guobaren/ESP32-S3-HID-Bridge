using HidBridge.Host.FirmwareUpdate;

namespace HidBridge.Host.Ui;

/// <summary>固件刷写进度窗口：小日志显示窗，实时展示刷写过程。</summary>
internal sealed class FirmwareFlashDialog : Form
{
    private readonly FirmwareFlashService _service;
    private readonly TextBox _logTextBox;
    private readonly Label _statusLabel;
    private readonly Button _closeButton;
    private readonly System.Windows.Forms.Timer _statusTimer;
    private bool _finished;

    internal FirmwareFlashDialog(FirmwareFlashService service)
    {
        _service = service;
        Text = "固件刷写进度";
        StartPosition = FormStartPosition.CenterParent;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        ShowInTaskbar = false;
        ClientSize = new Size(640, 400);
        Font = new Font("Microsoft YaHei UI", 9, FontStyle.Regular);

        _statusLabel = new Label
        {
            Dock = DockStyle.Top,
            Height = 34,
            Text = "正在准备刷写…",
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = Color.FromArgb(74, 88, 108),
        };
        _closeButton = new Button
        {
            Dock = DockStyle.Bottom,
            Height = 40,
            Text = "关闭",
            Enabled = false,
        };
        _closeButton.Click += (_, _) => Close();
        _logTextBox = new TextBox
        {
            Dock = DockStyle.Fill,
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Both,
            WordWrap = false,
            Font = new Font("Consolas", 9, FontStyle.Regular),
            BackColor = Color.White,
        };

        Controls.Add(_logTextBox);
        Controls.Add(_statusLabel);
        Controls.Add(_closeButton);

        _service.Log += OnLog;
        _statusTimer = new System.Windows.Forms.Timer { Interval = 500 };
        _statusTimer.Tick += (_, _) => RefreshStatus();
        _statusTimer.Start();
        AppendLog("刷写进度日志将显示在这里。");
        RefreshStatus();
    }

    private void OnLog(string line)
    {
        if (IsDisposed)
        {
            return;
        }
        if (InvokeRequired)
        {
            try
            {
                BeginInvoke(new Action<string>(OnLog), line);
            }
            catch (InvalidOperationException)
            {
                // 窗口正在关闭。
            }
            return;
        }
        AppendLog(line);
    }

    private void AppendLog(string line)
    {
        _logTextBox.AppendText(line + Environment.NewLine);
        _logTextBox.SelectionStart = _logTextBox.TextLength;
        _logTextBox.ScrollToCaret();
    }

    private void RefreshStatus()
    {
        FirmwareFlashSnapshot snapshot = _service.GetSnapshot();
        if (snapshot.State is "succeeded" or "failed")
        {
            _statusTimer.Stop();
            if (!_finished)
            {
                _finished = true;
                _closeButton.Enabled = true;
                AppendLog(snapshot.State == "succeeded"
                    ? "刷写完成。"
                    : $"刷写失败：{snapshot.Message}");
                _statusLabel.ForeColor = snapshot.State == "succeeded"
                    ? Color.FromArgb(34, 125, 70)
                    : Color.FromArgb(170, 42, 42);
                _statusLabel.Text = snapshot.State == "succeeded" ? "刷写完成" : "刷写失败";
            }
            return;
        }
        _statusLabel.ForeColor = SystemColors.ControlText;
        _statusLabel.Text = snapshot.State == "running"
            ? $"刷写中：{snapshot.Message}"
            : "等待开始…";
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        bool running = !_finished && _service.GetSnapshot().State == "running";
        _service.Log -= OnLog;
        _statusTimer.Stop();
        if (running)
        {
            DialogResult result = MessageBox.Show(
                this,
                "刷写仍在进行，关闭窗口不会中断刷写。确定关闭日志窗口吗？",
                "固件刷写进度",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Warning);
            if (result != DialogResult.Yes)
            {
                e.Cancel = true;
                _service.Log += OnLog;
                _statusTimer.Start();
                base.OnFormClosing(e);
                return;
            }
        }
        base.OnFormClosing(e);
    }
}
