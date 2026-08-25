using System.Globalization;

namespace HidBridge.Host.Input;

internal static class MouseOutputSensitivity
{
    internal const double Minimum = 0.3;
    internal const double Maximum = 3.0;
    internal const double Default = 1.0;
    internal const int TrackBarScale = 100;

    internal static double Clamp(double value)
    {
        if (!double.IsFinite(value))
        {
            return Default;
        }

        double rounded = Math.Round(value, 2, MidpointRounding.AwayFromZero);
        return Math.Clamp(rounded, Minimum, Maximum);
    }

    internal static int ToTrackBarValue(double value) =>
        Math.Clamp(
            (int)Math.Round(Clamp(value) * TrackBarScale, MidpointRounding.AwayFromZero),
            (int)(Minimum * TrackBarScale),
            (int)(Maximum * TrackBarScale));

    internal static double FromTrackBarValue(int value) =>
        Math.Clamp(value, (int)(Minimum * TrackBarScale), (int)(Maximum * TrackBarScale)) /
        (double)TrackBarScale;

    internal static bool TryParse(string text, out double value)
    {
        if (!double.TryParse(
                text.Trim(),
                NumberStyles.Float,
                CultureInfo.InvariantCulture,
                out value) &&
            !double.TryParse(
                text.Trim(),
                NumberStyles.Float,
                CultureInfo.CurrentCulture,
                out value))
        {
            value = Default;
            return false;
        }

        return double.IsFinite(value);
    }

    internal static string Format(double value) =>
        Clamp(value).ToString("0.##", CultureInfo.InvariantCulture);
}
