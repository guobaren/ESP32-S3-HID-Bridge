using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using HidBridge.Host.Input;

namespace HidBridge.Host.Ui;

internal sealed class MouseMovementAnalysisForm : Form
{
    private readonly PictureBox _chartPictureBox;

    internal MouseMovementAnalysisForm(MouseMovementRecording recording)
    {
        ArgumentNullException.ThrowIfNull(recording);

        Text = "鼠标移动分析图";
        StartPosition = FormStartPosition.CenterParent;
        MinimumSize = new Size(900, 560);
        ClientSize = new Size(1200, 700);
        Font = new Font("Microsoft YaHei UI", 10, FontStyle.Regular);

        string? savedImagePath = null;
        try
        {
            savedImagePath = MouseMovementAnalysisRenderer.SavePng(
                recording,
                Path.Combine(AppContext.BaseDirectory, "log"));
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"保存鼠标移动分析图失败：{exception.Message}");
        }

        Label summaryLabel = new()
        {
            Dock = DockStyle.Top,
            Height = 64,
            Padding = new Padding(12, 8, 12, 6),
            TextAlign = ContentAlignment.MiddleLeft,
            AutoEllipsis = true,
            Text = BuildSummary(recording, savedImagePath),
            ForeColor = Color.FromArgb(58, 72, 92),
        };

        _chartPictureBox = new PictureBox
        {
            Dock = DockStyle.Fill,
            BackColor = Color.White,
            SizeMode = PictureBoxSizeMode.Zoom,
            Image = MouseMovementAnalysisRenderer.Render(recording, new Size(1600, 800)),
        };

        Controls.Add(_chartPictureBox);
        Controls.Add(summaryLabel);
        SavedImagePath = savedImagePath;
    }

    internal string? SavedImagePath { get; }
    internal PictureBox ChartPictureBox => _chartPictureBox;

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _chartPictureBox.Image?.Dispose();
        }
        base.Dispose(disposing);
    }

    private static string BuildSummary(MouseMovementRecording recording, string? savedImagePath)
    {
        string xSummary = BuildAxisSummary("X", recording.XValues);
        string ySummary = BuildAxisSummary("Y", recording.YValues);
        string path = savedImagePath is null ? "图片保存失败，仅在窗口显示" : $"已保存：{savedImagePath}";
        return $"样本={recording.SampleCount}；{xSummary}；{ySummary}\r\n{path}";
    }

    private static string BuildAxisSummary(string axis, IReadOnlyList<int> values)
    {
        if (values.Count == 0)
        {
            return $"{axis} 范围=0..0，平均=0.00";
        }
        return $"{axis} 范围={values.Min()}..{values.Max()}，平均={values.Average():F2}";
    }
}

internal static class MouseMovementAnalysisRenderer
{
    private static readonly Color XColor = Color.FromArgb(45, 112, 213);
    private static readonly Color YColor = Color.FromArgb(220, 91, 72);

    internal static Bitmap Render(MouseMovementRecording recording, Size size)
    {
        ArgumentNullException.ThrowIfNull(recording);
        if (size.Width < 400 || size.Height < 240)
        {
            throw new ArgumentOutOfRangeException(nameof(size), "分析图尺寸至少为 400x240。");
        }

        Bitmap bitmap = new(size.Width, size.Height, PixelFormat.Format32bppArgb);
        using Graphics graphics = Graphics.FromImage(bitmap);
        graphics.SmoothingMode = SmoothingMode.AntiAlias;
        graphics.Clear(Color.White);

        using Font titleFont = new("Microsoft YaHei UI", 18, FontStyle.Bold);
        using Brush titleBrush = new SolidBrush(Color.FromArgb(38, 47, 61));
        const string title = "鼠标移动命令有符号分析";
        SizeF titleSize = graphics.MeasureString(title, titleFont);
        graphics.DrawString(title, titleFont, titleBrush, (size.Width - titleSize.Width) / 2, 16);

        const int outerMargin = 28;
        const int gap = 22;
        int top = 62;
        int panelWidth = (size.Width - outerMargin * 2 - gap) / 2;
        int panelHeight = size.Height - top - outerMargin;
        Rectangle xBounds = new(outerMargin, top, panelWidth, panelHeight);
        Rectangle yBounds = new(outerMargin + panelWidth + gap, top, panelWidth, panelHeight);
        DrawSeries(
            graphics,
            xBounds,
            "X 有符号值（时间顺序向上）",
            recording.XValues,
            XColor,
            timeAscendingUp: true);
        DrawSeries(
            graphics,
            yBounds,
            "Y 有符号值（发送顺序）",
            recording.YValues,
            YColor,
            timeAscendingUp: false);
        return bitmap;
    }

    internal static string SavePng(MouseMovementRecording recording, string directory)
    {
        Directory.CreateDirectory(directory);
        string timestamp = recording.CompletedUtc.ToLocalTime().ToString("yyyyMMdd-HHmmss-fff");
        string path = Path.Combine(directory, $"mouse-movement-{timestamp}.png");
        using Bitmap bitmap = Render(recording, new Size(1600, 800));
        bitmap.Save(path, ImageFormat.Png);
        return path;
    }

    private static void DrawSeries(
        Graphics graphics,
        Rectangle bounds,
        string title,
        IReadOnlyList<int> values,
        Color seriesColor,
        bool timeAscendingUp)
    {
        using Brush background = new SolidBrush(Color.FromArgb(248, 250, 253));
        using Pen border = new(Color.FromArgb(204, 213, 226));
        graphics.FillRectangle(background, bounds);
        graphics.DrawRectangle(border, bounds);

        using Font panelTitleFont = new("Microsoft YaHei UI", 12, FontStyle.Bold);
        using Font axisFont = new("Microsoft YaHei UI", 9, FontStyle.Regular);
        using Brush textBrush = new SolidBrush(Color.FromArgb(58, 72, 92));
        graphics.DrawString(title, panelTitleFont, textBrush, bounds.Left + 14, bounds.Top + 10);

        Rectangle plot = new(bounds.Left + 62, bounds.Top + 48, bounds.Width - 82, bounds.Height - 94);
        using Pen axisPen = new(Color.FromArgb(137, 150, 169), 1);
        graphics.DrawLine(axisPen, plot.Left, plot.Top, plot.Left, plot.Bottom);
        graphics.DrawLine(axisPen, plot.Left, plot.Bottom, plot.Right, plot.Bottom);

        int dataMinimum = values.Count == 0 ? 0 : values.Min();
        int dataMaximum = values.Count == 0 ? 0 : values.Max();
        int scaleMinimum = Math.Min(0, dataMinimum);
        int scaleMaximum = Math.Max(0, dataMaximum);
        if (scaleMinimum == scaleMaximum)
        {
            scaleMinimum = -1;
            scaleMaximum = 1;
        }

        using Pen zeroPen = new(Color.FromArgb(170, 178, 191), 1)
        {
            DashStyle = DashStyle.Dash,
        };
        if (timeAscendingUp)
        {
            graphics.DrawString(Math.Max(1, values.Count).ToString(), axisFont, textBrush, bounds.Left + 8, plot.Top - 7);
            graphics.DrawString("1", axisFont, textBrush, bounds.Left + 40, plot.Bottom - 8);
            graphics.DrawString(scaleMinimum.ToString(), axisFont, textBrush, plot.Left - 3, plot.Bottom + 10);
            DrawRightAlignedString(graphics, scaleMaximum.ToString(), axisFont, textBrush, plot.Right, plot.Bottom + 10);
            float zeroX = MapSignedValueX(0, scaleMinimum, scaleMaximum, plot);
            graphics.DrawLine(zeroPen, zeroX, plot.Top, zeroX, plot.Bottom);
        }
        else
        {
            graphics.DrawString(scaleMaximum.ToString(), axisFont, textBrush, bounds.Left + 8, plot.Top - 7);
            graphics.DrawString(scaleMinimum.ToString(), axisFont, textBrush, bounds.Left + 8, plot.Bottom - 8);
            graphics.DrawString("1", axisFont, textBrush, plot.Left - 3, plot.Bottom + 10);
            DrawRightAlignedString(
                graphics,
                Math.Max(1, values.Count).ToString(),
                axisFont,
                textBrush,
                plot.Right,
                plot.Bottom + 10);
            float zeroY = MapSignedValueY(0, scaleMinimum, scaleMaximum, plot);
            graphics.DrawLine(zeroPen, plot.Left, zeroY, plot.Right, zeroY);
        }

        if (values.Count == 0)
        {
            const string noData = "没有记录到移动报告";
            SizeF noDataSize = graphics.MeasureString(noData, axisFont);
            graphics.DrawString(
                noData,
                axisFont,
                textBrush,
                plot.Left + (plot.Width - noDataSize.Width) / 2,
                plot.Top + (plot.Height - noDataSize.Height) / 2);
            return;
        }

        using Pen seriesPen = new(seriesColor, 1.8f);
        if (timeAscendingUp)
        {
            DrawVerticalTimeSeries(
                graphics,
                seriesPen,
                plot,
                values,
                scaleMinimum,
                scaleMaximum,
                seriesColor);
            return;
        }

        if (values.Count == 1)
        {
            float y = MapSignedValueY(values[0], scaleMinimum, scaleMaximum, plot);
            using Brush pointBrush = new SolidBrush(seriesColor);
            graphics.FillEllipse(pointBrush, plot.Left - 3, y - 3, 6, 6);
            return;
        }

        PointF[] points = new PointF[values.Count];
        for (int index = 0; index < values.Count; index++)
        {
            float x = plot.Left + index * plot.Width / (float)(values.Count - 1);
            points[index] = new PointF(
                x,
                MapSignedValueY(values[index], scaleMinimum, scaleMaximum, plot));
        }
        graphics.DrawLines(seriesPen, points);
    }

    private static void DrawVerticalTimeSeries(
        Graphics graphics,
        Pen seriesPen,
        Rectangle plot,
        IReadOnlyList<int> values,
        int scaleMinimum,
        int scaleMaximum,
        Color seriesColor)
    {
        if (values.Count == 1)
        {
            PointF point = MapVerticalTimePoint(values[0], 0, 1, scaleMinimum, scaleMaximum, plot);
            using Brush pointBrush = new SolidBrush(seriesColor);
            graphics.FillEllipse(pointBrush, point.X - 3, point.Y - 3, 6, 6);
            return;
        }

        PointF[] points = new PointF[values.Count];
        for (int index = 0; index < values.Count; index++)
        {
            points[index] = MapVerticalTimePoint(
                values[index],
                index,
                values.Count,
                scaleMinimum,
                scaleMaximum,
                plot);
        }
        graphics.DrawLines(seriesPen, points);
    }

    internal static PointF MapVerticalTimePoint(
        float value,
        int index,
        int count,
        int minimum,
        int maximum,
        Rectangle plot)
    {
        if (count <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(count));
        }
        if (index < 0 || index >= count)
        {
            throw new ArgumentOutOfRangeException(nameof(index));
        }
        if (maximum <= minimum)
        {
            throw new ArgumentOutOfRangeException(nameof(maximum));
        }
        float y = count == 1
            ? plot.Bottom
            : plot.Bottom - index * plot.Height / (float)(count - 1);
        return new PointF(MapSignedValueX(value, minimum, maximum, plot), y);
    }

    internal static float MapSignedValueX(
        float value,
        int minimum,
        int maximum,
        Rectangle plot)
    {
        if (maximum <= minimum)
        {
            throw new ArgumentOutOfRangeException(nameof(maximum));
        }
        return plot.Left + (value - minimum) * plot.Width / (float)(maximum - minimum);
    }

    internal static float MapSignedValueY(
        float value,
        int minimum,
        int maximum,
        Rectangle plot)
    {
        if (maximum <= minimum)
        {
            throw new ArgumentOutOfRangeException(nameof(maximum));
        }
        return plot.Bottom - (value - minimum) * plot.Height / (float)(maximum - minimum);
    }

    private static void DrawRightAlignedString(
        Graphics graphics,
        string value,
        Font font,
        Brush brush,
        float right,
        float top)
    {
        SizeF size = graphics.MeasureString(value, font);
        graphics.DrawString(value, font, brush, right - size.Width, top);
    }
}
