using System.Drawing.Drawing2D;

namespace HidBridge.Host.Ui;

internal sealed class MouseCaptureSurface : Panel
{
    private bool _forwarding;
    private bool _legacySingleBoardMode;

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

    internal bool LegacySingleBoardMode
    {
        get => _legacySingleBoardMode;
        set
        {
            if (_legacySingleBoardMode == value)
            {
                return;
            }

            _legacySingleBoardMode = value;
            Invalidate();
        }
    }

    public MouseCaptureSurface()
    {
        DoubleBuffered = true;
        Dock = DockStyle.Fill;
        BackColor = Color.FromArgb(249, 251, 254);
        MinimumSize = new Size(0, 150);
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

        using (Pen border = new(_forwarding ? Color.FromArgb(37, 164, 116) : Color.FromArgb(191, 205, 224), 1))
        {
            e.Graphics.DrawRectangle(border, 1, 1, Math.Max(0, Width - 3), Math.Max(0, Height - 3));
        }

        using Font detailFont = new(Font.FontFamily, 10, FontStyle.Regular);
        using Brush detailBrush = new SolidBrush(Color.FromArgb(82, 106, 139));
        string detail = _forwarding
            ? "旧版单板同步已开启，实体鼠标由 EXE 转发"
            : _legacySingleBoardMode
                ? "旧版单板：按 HOME 开启实体鼠标转发；关闭时输入留在本机"
                : "双板实体鼠标由 M→P 硬件直通；EXE 不重复转发，本机光标不锁定";
        SizeF detailSize = e.Graphics.MeasureString(detail, detailFont);
        float centerX = Width / 2f;
        float centerY = Height / 2f;
        float detailY = Math.Max(12, Height - detailSize.Height - 16);
        e.Graphics.DrawString(detail, detailFont, detailBrush, centerX - detailSize.Width / 2, detailY);

        using Pen crosshair = new(_forwarding ? Color.FromArgb(37, 164, 116) : Color.FromArgb(137, 157, 187), 1);
        e.Graphics.DrawEllipse(crosshair, centerX - 18, centerY - 18, 36, 36);
        e.Graphics.DrawLine(crosshair, centerX - 42, centerY, centerX + 42, centerY);
        e.Graphics.DrawLine(crosshair, centerX, centerY - 42, centerX, centerY + 42);
    }
}
