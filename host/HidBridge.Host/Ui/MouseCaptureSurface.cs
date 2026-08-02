using System.Drawing.Drawing2D;

namespace HidBridge.Host.Ui;

internal sealed class MouseCaptureSurface : Panel
{
    private bool _forwarding;

    internal bool Forwarding
    {
        get => _forwarding;
        set
        {
            if (_forwarding == value)
            {
                return;
            }

            _forwarding = value;
            Invalidate();
        }
    }

    public MouseCaptureSurface()
    {
        DoubleBuffered = true;
        Dock = DockStyle.Fill;
        BackColor = Color.FromArgb(24, 31, 42);
        MinimumSize = new Size(0, 180);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        Rectangle bounds = ClientRectangle;
        using (Brush background = new SolidBrush(BackColor))
        {
            e.Graphics.FillRectangle(background, bounds);
        }

        using (Pen border = new(_forwarding ? Color.FromArgb(58, 198, 139) : Color.FromArgb(92, 105, 124), 2))
        {
            e.Graphics.DrawRectangle(border, 1, 1, Math.Max(0, Width - 3), Math.Max(0, Height - 3));
        }

        using Font titleFont = new(Font.FontFamily, 12, FontStyle.Bold);
        using Font detailFont = new(Font.FontFamily, 10, FontStyle.Regular);
        using Brush titleBrush = new SolidBrush(Color.White);
        using Brush detailBrush = new SolidBrush(Color.FromArgb(190, 202, 218));
        string status = _forwarding ? "同步已开启" : "同步已关闭";
        string detail = _forwarding
            ? "鼠标已锁定在此处，移动和按钮只发送到对端"
            : "按 HOME 开启同步后，鼠标将停放在中心";
        SizeF statusSize = e.Graphics.MeasureString(status, titleFont);
        SizeF detailSize = e.Graphics.MeasureString(detail, detailFont);
        float centerX = Width / 2f;
        float centerY = Height / 2f;
        e.Graphics.DrawString(status, titleFont, titleBrush, centerX - statusSize.Width / 2, centerY - 72);
        e.Graphics.DrawString(detail, detailFont, detailBrush, centerX - detailSize.Width / 2, centerY + 48);

        using Pen crosshair = new(_forwarding ? Color.FromArgb(85, 224, 164) : Color.FromArgb(136, 151, 173), 2);
        e.Graphics.DrawEllipse(crosshair, centerX - 18, centerY - 18, 36, 36);
        e.Graphics.DrawLine(crosshair, centerX - 42, centerY, centerX + 42, centerY);
        e.Graphics.DrawLine(crosshair, centerX, centerY - 42, centerX, centerY + 42);
    }
}
