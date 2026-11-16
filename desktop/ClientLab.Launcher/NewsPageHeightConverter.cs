using System.Globalization;
using Avalonia.Data.Converters;

namespace NosCore.ClientLab.Launcher;

public sealed class NewsPageHeightConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        value is double width && double.IsFinite(width) ? Math.Max(0, width / 2 - 10) * 250 / 444 * 2 + 150 : 150d;
    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) => throw new NotSupportedException();
}
