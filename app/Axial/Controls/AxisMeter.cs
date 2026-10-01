using System;
using System.Globalization;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Media;

namespace Axial.App;

// A centred bar for one live axis value in -350…350, as axial's AxisMeter.
sealed class AxisMeter : FrameworkElement {
    public static readonly DependencyProperty ValueProperty = DependencyProperty.Register(nameof(Value), typeof(int), typeof(AxisMeter),
        new FrameworkPropertyMetadata(0, FrameworkPropertyMetadataOptions.AffectsRender, (d, e) => AutomationProperties.SetHelpText(d, ((int)e.NewValue).ToString("+0;-0;0", CultureInfo.InvariantCulture))));
    public int Value { get => (int)GetValue(ValueProperty); set => SetValue(ValueProperty, value); }

    static readonly Typeface Mono = new(new FontFamily("Cascadia Mono, Consolas"), FontStyles.Normal, FontWeights.Normal, FontStretches.Normal);
    Brush Resource(string key, Brush fallback) => TryFindResource(key) as Brush ?? fallback;

    protected override Size MeasureOverride(Size available) => new(double.IsInfinity(available.Width) ? 120 : available.Width, 28);

    protected override void OnRender(DrawingContext context) {
        double width = ActualWidth, half = width / 2;
        if (width <= 0) return;
        var secondary = Resource("SecondaryTextBrush", Brushes.Gray);
        var track = Resource("TrackBrush", Brushes.DimGray);
        context.DrawRoundedRectangle(track, null, new Rect(0, 3, width, 6), 3, 3);
        double fraction = Math.Min(Math.Abs(Value) / 350.0, 1), length = half * fraction;
        if (length > 0) {
            var fill = Value < 0 ? Resource("OrangeBrush", Brushes.Orange) : Resource("AccentBrush", Brushes.Teal);
            context.DrawRoundedRectangle(fill, null, new Rect(Value < 0 ? half - length : half, 3, length, 6), 3, 3);
        }
        context.DrawRectangle(Resource("TextBrush", Brushes.White), null, new Rect(Math.Round(half) - 0.5, 0, 1, 12));
        double dip = VisualTreeHelper.GetDpi(this).PixelsPerDip;
        FormattedText Label(string text) => new(text, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, Mono, 10, secondary, dip);
        var minus = Label("−"); var zero = Label("0"); var plus = Label("+");
        context.DrawText(minus, new Point(0, 14));
        context.DrawText(zero, new Point(half - zero.Width / 2, 14));
        context.DrawText(plus, new Point(width - plus.Width, 14));
    }
}
