using System.Globalization;
using System.Windows.Data;
using System.Windows.Media;

namespace PolymarketUI.Converters;

/// <summary>
/// Maps order lifecycle states to operator-facing row backgrounds.
/// REJECTED/FAILED uses a red tone to distinguish hard failures.
/// </summary>
public class OrderLifecycleToBrushConverter : IValueConverter
{
    private static readonly Brush WorkingBrush = new SolidColorBrush(Color.FromRgb(0xFF, 0xF1, 0x8A));          // yellow
    private static readonly Brush FilledBrush = new SolidColorBrush(Color.FromRgb(0xC8, 0xE6, 0xC9));           // green
    private static readonly Brush CanceledNoFillBrush = new SolidColorBrush(Color.FromRgb(0xE0, 0xE0, 0xE0));   // gray
    private static readonly Brush RejectedBrush = new SolidColorBrush(Color.FromRgb(0xFF, 0xCD, 0xD2));         // red

    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        var status = (value as string)?.Trim().ToUpperInvariant() ?? string.Empty;
        return status switch
        {
            "WORKING" => WorkingBrush,
            "LIVE" => WorkingBrush,
            "PENDING" => WorkingBrush,
            "MATCHED" => FilledBrush,
            "MINED" => FilledBrush,
            "CONFIRMED" => FilledBrush,
            "RETRYING" => WorkingBrush,
            "FILLED" => FilledBrush,
            "CANCELED_WITH_FILL" => FilledBrush,
            "CANCELED_NO_FILL" => CanceledNoFillBrush,
            "REJECTED" => RejectedBrush,
            "FAILED" => RejectedBrush,
            _ => Brushes.Transparent
        };
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
    {
        throw new NotSupportedException();
    }
}
