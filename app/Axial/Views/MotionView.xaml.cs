using System;
using System.ComponentModel;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;

namespace Axial.App;

public partial class MotionView : UserControl {
    static readonly string[] AxisNames = ["Left / right", "Near / far", "Up / down", "Tilt", "Roll", "Spin"];
    static readonly Choice[] Navigation = [new("orbit", "Orbit around target"), new("camera", "Move camera")];
    SettingsModel? model;
    readonly AxisCard[] cards = new AxisCard[6];
    bool updating;

    public MotionView() {
        InitializeComponent();
        NavigationBox.ItemsSource = Navigation;
    }
    internal void Attach(SettingsModel model) {
        this.model = model;
        for (int i = 0; i < 6; i++) {
            int axis = i;
            cards[i] = new AxisCard(AxisNames[i],
                gain => Edit(p => p.Gain[axis] = gain), deadzone => Edit(p => p.Deadzone[axis] = deadzone), invert => Edit(p => p.Invert[axis] = invert));
            AxisGrid.Children.Add(cards[i]);
        }
        model.PropertyChanged += OnModelChanged;
        RefreshProfile(); RefreshDevice();
        IsEnabled = model.ConfigurationReady;
    }
    void Edit(Action<Profile> change) { if (!updating) model?.Edit(change); }

    void OnModelChanged(object? sender, PropertyChangedEventArgs e) {
        if (model == null) return;
        switch (e.PropertyName) {
            case nameof(SettingsModel.Profile): RefreshProfile(); break;
            case nameof(SettingsModel.Device): RefreshDevice(); break;
            case nameof(SettingsModel.ConfigurationReady): IsEnabled = model.ConfigurationReady; break;
        }
    }
    void RefreshProfile() {
        if (model == null) return;
        var profile = model.Profile;
        updating = true;
        try {
            TranslationToggle.IsChecked = profile.Translation;
            RotationToggle.IsChecked = profile.Rotation;
            DominantToggle.IsChecked = profile.Dominant;
            LedToggle.IsChecked = profile.Led;
            NavigationBox.SelectedItem = Navigation[profile.Orbit ? 0 : 1];
            for (int i = 0; i < 6; i++) cards[i].Show(profile.Gain[i], profile.Deadzone[i], profile.Invert[i]);
        } finally { updating = false; }
    }
    void RefreshDevice() {
        if (model == null) return;
        var device = model.Device;
        LedToggle.IsEnabled = device?.LedSupported != false;
        if (device?.LedError is uint error && error != 0) {
            LedError.Text = $"The controller rejected the LED change ({error.ToString("x", CultureInfo.InvariantCulture)}).";
            LedError.Visibility = Visibility.Visible;
        } else LedError.Visibility = Visibility.Collapsed;
        for (int i = 0; i < 6; i++) cards[i].Live(device != null && i < device.Axes.Length ? device.Axes[i] : 0);
        CalibrateButton.IsEnabled = device != null;
        ClearCalibrationButton.IsEnabled = device?.Calibrated == true;
        CalibrationState.Text = device?.Calibrated == true ? "Calibrated" : "";
    }
    async void OnCalibrate(object sender, RoutedEventArgs e) { if (model != null) await model.CalibrateAsync(); }
    async void OnClearCalibration(object sender, RoutedEventArgs e) { if (model != null) await model.CalibrateAsync(clear: true); }

    void OnToggle(object sender, RoutedEventArgs e) {
        if (sender is not CheckBox toggle) return;
        bool on = toggle.IsChecked == true;
        if (toggle == TranslationToggle) Edit(p => p.Translation = on);
        else if (toggle == RotationToggle) Edit(p => p.Rotation = on);
        else if (toggle == DominantToggle) Edit(p => p.Dominant = on);
        else if (toggle == LedToggle) Edit(p => p.Led = on);
    }
    void OnNavigation(object sender, SelectionChangedEventArgs e) {
        if (NavigationBox.SelectedItem is Choice choice) Edit(p => p.Orbit = choice.Id == "orbit");
    }
    void OnReset(object sender, RoutedEventArgs e) => Edit(p => {
        var d = new Profile();
        p.Gain = d.Gain; p.Deadzone = d.Deadzone; p.Invert = d.Invert; p.Dominant = d.Dominant;
        p.Translation = d.Translation; p.Rotation = d.Rotation; p.Orbit = d.Orbit; p.Led = d.Led; p.Buttons = d.Buttons;
    });

    // One axis: live meter, speed, deadzone and invert.
    sealed class AxisCard : Border {
        readonly TextBlock value = new() { FontFamily = new System.Windows.Media.FontFamily("Cascadia Mono, Consolas"), HorizontalAlignment = HorizontalAlignment.Right };
        readonly AxisMeter meter = new() { Margin = new Thickness(0, 6, 0, 4) };
        readonly Slider speed = new() { Minimum = 0, Maximum = 5, SmallChange = 0.05, LargeChange = 0.25 };
        readonly Slider deadzone = new() { Minimum = 0, Maximum = 100, SmallChange = 1, LargeChange = 5 };
        readonly TextBlock speedText = new() { FontFamily = new System.Windows.Media.FontFamily("Cascadia Mono, Consolas"), VerticalAlignment = VerticalAlignment.Center, TextAlignment = TextAlignment.Right };
        readonly CheckBox invert = new() { Content = "Invert", Margin = new Thickness(12, 0, 0, 0) };
        bool showing;

        public AxisCard(string name, Action<double> gain, Action<double> dead, Action<bool> inverted) {
            SetResourceReference(StyleProperty, "Card");
            Margin = new Thickness(5);
            value.SetResourceReference(TextBlock.ForegroundProperty, "SecondaryTextBrush");
            System.Windows.Automation.AutomationProperties.SetName(meter, name);
            System.Windows.Automation.AutomationProperties.SetName(speed, $"{name} speed");
            System.Windows.Automation.AutomationProperties.SetName(deadzone, $"{name} deadzone");
            System.Windows.Automation.AutomationProperties.SetName(invert, $"Invert {name}");
            var header = new Grid();
            header.Children.Add(new TextBlock { Text = name, FontWeight = FontWeights.SemiBold });
            header.Children.Add(value);
            var stack = new StackPanel();
            stack.Children.Add(header);
            stack.Children.Add(meter);
            stack.Children.Add(Row("Speed", speed, speedText));
            stack.Children.Add(Row("Deadzone", deadzone, invert));
            Child = stack;
            speed.ValueChanged += (_, e) => { speedText.Text = e.NewValue.ToString("0.00", CultureInfo.CurrentCulture); if (!showing) gain(Math.Round(e.NewValue, 2)); };
            deadzone.ValueChanged += (_, e) => { if (!showing) dead(Math.Round(e.NewValue)); };
            invert.Click += (_, _) => inverted(invert.IsChecked == true);
        }
        static Grid Row(string label, Slider slider, FrameworkElement trailing) {
            var row = new Grid { Margin = new Thickness(0, 2, 0, 0) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(66) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Auto), MinWidth = 40 });
            var text = new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(slider, 1); Grid.SetColumn(trailing, 2);
            row.Children.Add(text); row.Children.Add(slider); row.Children.Add(trailing);
            return row;
        }
        public void Show(double gain, double dead, bool inverted) {
            showing = true;
            try {
                speed.Value = gain; speedText.Text = gain.ToString("0.00", CultureInfo.CurrentCulture);
                deadzone.Value = dead; invert.IsChecked = inverted;
            } finally { showing = false; }
        }
        public void Live(int axis) {
            if (meter.Value == axis && value.Text.Length > 0) return;
            meter.Value = axis;
            value.Text = axis.ToString(CultureInfo.InvariantCulture);
        }
    }
}
