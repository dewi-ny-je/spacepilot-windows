using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace Axial.App;

// One rolling two-minute diagnostics chart: a stepped line with a soft area,
// as axial's DiagnosticChart.
sealed class DiagnosticChart : Border {
    readonly Func<DiagnosticSample, double> value;
    readonly TextBlock latest = new() { FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center };
    readonly Plot plot;

    public DiagnosticChart(string title, string unit, Brush color, Func<DiagnosticSample, double> value) {
        this.value = value;
        SetResourceReference(StyleProperty, "Card");
        Margin = new Thickness(0, 0, 12, 12);
        plot = new Plot(color, value) { Height = 112 };
        var header = new DockPanel { Margin = new Thickness(0, 0, 0, 6) };
        var right = new StackPanel { Orientation = Orientation.Horizontal };
        right.Children.Add(latest);
        if (unit.Length > 0) {
            var label = new TextBlock { Text = unit, Margin = new Thickness(4, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center, FontSize = 11.5 };
            label.SetResourceReference(TextBlock.ForegroundProperty, "SecondaryTextBrush");
            right.Children.Add(label);
        }
        DockPanel.SetDock(right, Dock.Right);
        header.Children.Add(right);
        header.Children.Add(new TextBlock { Text = title, FontWeight = FontWeights.SemiBold });
        var stack = new StackPanel();
        stack.Children.Add(header); stack.Children.Add(plot);
        Child = stack;
        Update([]);
    }
    public void Update(IReadOnlyList<DiagnosticSample> samples) {
        latest.Text = (samples.Count > 0 ? value(samples[^1]) : 0).ToString("0.#", CultureInfo.CurrentCulture);
        plot.Samples = samples.ToArray();
        plot.InvalidateVisual();
    }

    sealed class Plot(Brush color, Func<DiagnosticSample, double> value) : FrameworkElement {
        public DiagnosticSample[] Samples { get; set; } = [];
        static readonly Typeface Font = new(new FontFamily("Segoe UI"), FontStyles.Normal, FontWeights.Normal, FontStretches.Normal);
        protected override void OnRender(DrawingContext context) {
            const double left = 34, bottom = 16;
            double width = ActualWidth - left, height = ActualHeight - bottom;
            if (width <= 10 || height <= 10) return;
            var secondary = TryFindResource("SecondaryTextBrush") as Brush ?? Brushes.Gray;
            var grid = new Pen(TryFindResource("DividerBrush") as Brush ?? Brushes.DimGray, 1);
            double dip = VisualTreeHelper.GetDpi(this).PixelsPerDip;
            FormattedText Label(string text) => new(text, CultureInfo.CurrentCulture, FlowDirection.LeftToRight, Font, 10, secondary, dip);

            double maximum = Math.Max(1, (Samples.Length > 0 ? Samples.Max(value) : 0) * 1.1);
            var end = Samples.Length > 0 ? Samples[^1].Time : DateTimeOffset.Now;
            var start = end.AddSeconds(-119);
            double X(DateTimeOffset time) => left + Math.Clamp((time - start).TotalSeconds / 119, 0, 1) * width;
            double Y(double v) => height - Math.Clamp(v / maximum, 0, 1) * height;

            foreach (double tick in new[] { 0, maximum / 2, maximum }) {
                double y = Math.Round(Y(tick)) + 0.5;
                context.DrawLine(grid, new Point(left, y), new Point(left + width, y));
                var label = Label(tick.ToString(maximum >= 10 ? "0" : "0.#", CultureInfo.CurrentCulture));
                context.DrawText(label, new Point(left - 6 - label.Width, y - label.Height / 2));
            }
            foreach (var time in new[] { start, start.AddSeconds(59.5), end }) {
                double x = X(time);
                context.DrawLine(grid, new Point(x, 0), new Point(x, height));
                var label = Label(time.LocalDateTime.ToString("mm:ss", CultureInfo.CurrentCulture));
                context.DrawText(label, new Point(Math.Clamp(x - label.Width / 2, left, left + width - label.Width), height + 3));
            }
            if (Samples.Length == 0) return;
            var line = new StreamGeometry();
            var area = new StreamGeometry();
            using (var stroke = line.Open())
            using (var fill = area.Open()) {
                var first = new Point(X(Samples[0].Time), Y(value(Samples[0])));
                stroke.BeginFigure(first, false, false);
                fill.BeginFigure(new Point(first.X, height), true, true);
                fill.LineTo(first, false, false);
                double y = first.Y;
                for (int i = 1; i < Samples.Length; i++) {
                    double x = X(Samples[i].Time), next = Y(value(Samples[i]));
                    stroke.LineTo(new Point(x, y), true, false); fill.LineTo(new Point(x, y), false, false);
                    stroke.LineTo(new Point(x, next), true, false); fill.LineTo(new Point(x, next), false, false);
                    y = next;
                }
                fill.LineTo(new Point(X(Samples[^1].Time), height), false, false);
            }
            line.Freeze(); area.Freeze();
            var soft = color.Clone(); soft.Opacity = 0.14; soft.Freeze();
            context.PushClip(new RectangleGeometry(new Rect(left, 0, width, height)));
            context.DrawGeometry(soft, null, area);
            context.DrawGeometry(null, new Pen(color, 1.5) { LineJoin = PenLineJoin.Round }, line);
            context.Pop();
        }
    }
}
