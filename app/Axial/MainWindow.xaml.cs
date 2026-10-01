using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Shell;

namespace Axial.App;

// A combo box entry: a stable id and its visible title.
sealed record Choice(string Id, string Title) {
    public override string ToString() => Title;
}

// The settings window: sidebar and segmented tabs, as axial's SettingsView.
public partial class MainWindow : Window {
    const double CaptionHeight = 32;
    readonly SettingsModel model;
    readonly ApplicationNames names;
    readonly App app;
    bool backdrop = Environment.OSVersion.Version.Build >= 22621;
    bool updating;
    bool? serviceRunning;
    string deviceSignature = "";

    internal MainWindow(SettingsModel model, IPreview preview, ApplicationNames names, App app) {
        this.model = model; this.names = names; this.app = app;
        InitializeComponent();
        DataContext = model;
        if (backdrop) {
            // Mica behind a teal tint; the glass frame covers the whole window.
            WindowChrome.SetWindowChrome(this, new WindowChrome {
                CaptionHeight = CaptionHeight, GlassFrameThickness = new Thickness(-1), UseAeroCaptionButtons = true,
                ResizeBorderThickness = SystemParameters.WindowResizeBorderThickness, CornerRadius = new CornerRadius(0),
            });
            Background = Brushes.Transparent;
            Root.SetResourceReference(Panel.BackgroundProperty, "GlassTintBrush");
            Layout.Margin = new Thickness(8, CaptionHeight, 8, 8);
        }
        var area = SystemParameters.WorkArea;
        Width = Math.Min(Width, area.Width - 40); Height = Math.Min(Height, area.Height - 40);
        Motion.Attach(model);
        Buttons.Attach(model);
        Test.Attach(model, preview);
        Service.Attach(model, () => app.ShowSettings());
        model.PropertyChanged += OnModelChanged;
        model.WebSetup.PropertyChanged += (_, _) => Service.Refresh();
        PreviewKeyDown += OnPreviewKeyDown;
        StateChanged += (_, _) => Root.Margin = WindowState == WindowState.Maximized && backdrop ? new Thickness(7) : new Thickness(0);
        RefreshAll();
    }

    protected override void OnSourceInitialized(EventArgs e) {
        base.OnSourceInitialized(e);
        var handle = new WindowInteropHelper(this).Handle;
        NativeMethods.SetWindowAttribute(handle, NativeMethods.DwmUseImmersiveDarkMode, 1);
        if (!backdrop) return;
        if (HwndSource.FromHwnd(handle) is { CompositionTarget: { } target }) target.BackgroundColor = Colors.Transparent;
        if (!NativeMethods.SetWindowAttribute(handle, NativeMethods.DwmSystemBackdropType, NativeMethods.BackdropMica)) {
            // Older builds: keep the system title bar over the solid teal gradient.
            backdrop = false;
            WindowChrome.SetWindowChrome(this, null);
            SetResourceReference(BackgroundProperty, "WindowFallbackBrush");
            Root.Background = null;
            Layout.Margin = new Thickness(8);
        }
    }

    protected override void OnClosing(CancelEventArgs e) {
        base.OnClosing(e);
        if (app.Quitting) return;
        // Closing settings keeps Axial running in the notification area.
        e.Cancel = true;
        Hide();
    }

    public bool IsShown => IsVisible && WindowState != WindowState.Minimized;

    void OnModelChanged(object? sender, PropertyChangedEventArgs e) {
        switch (e.PropertyName) {
            case nameof(SettingsModel.Status): RefreshService(); RefreshDevices(); break;
            case nameof(SettingsModel.ProfileKeys): case nameof(SettingsModel.Selected): RefreshProfiles(); break;
            case nameof(SettingsModel.SelectedDevice): RefreshDevices(); RefreshProfiles(); break;
            case nameof(SettingsModel.ConfigurationReady): RefreshProfiles(); break;
            case nameof(SettingsModel.Tab): RefreshTab(); break;
            case nameof(SettingsModel.Message): MessagePanel.Visibility = string.IsNullOrEmpty(model.Message) ? Visibility.Collapsed : Visibility.Visible; break;
        }
    }
    void RefreshAll() {
        RefreshService(); RefreshDevices(); RefreshProfiles(); RefreshTab();
        MessagePanel.Visibility = string.IsNullOrEmpty(model.Message) ? Visibility.Collapsed : Visibility.Visible;
    }
    void RefreshService() {
        bool running = model.Status != null;
        if (serviceRunning == running) return;
        serviceRunning = running;
        ServiceText.Text = running ? "Service running" : "Service stopped";
        ServiceText.SetResourceReference(TextBlock.ForegroundProperty, running ? "GreenBrush" : "SecondaryTextBrush");
        ServiceDot.Fill = running ? (Brush)FindResource("GreenBrush") : Brushes.Transparent;
        ServiceDot.StrokeThickness = running ? 0 : 1.5;
        ServiceCheck.Visibility = running ? Visibility.Visible : Visibility.Collapsed;
    }
    void RefreshTab() {
        int tab = model.Tab;
        Motion.Visibility = tab == 0 ? Visibility.Visible : Visibility.Collapsed;
        Buttons.Visibility = tab == 1 ? Visibility.Visible : Visibility.Collapsed;
        Test.Visibility = tab == 2 ? Visibility.Visible : Visibility.Collapsed;
        Service.Visibility = tab == 3 ? Visibility.Visible : Visibility.Collapsed;
    }

    // Combo boxes are filled only when their choices change, so an open
    // drop-down is not closed by the once-per-second status poll.
    static void Fill(ComboBox box, IReadOnlyList<Choice> choices, string? selected) {
        if (box.ItemsSource is not IReadOnlyList<Choice> current || !current.SequenceEqual(choices)) box.ItemsSource = choices;
        var match = choices.FirstOrDefault(c => c.Id == selected);
        if (!Equals(box.SelectedItem, match)) box.SelectedItem = match;
    }
    void RefreshProfiles() {
        updating = true;
        try {
            Fill(ProfileBox, model.ProfileKeys.Select(k => new Choice(k, model.ProfileName(k))).ToList(), model.Selected);
        } finally { updating = false; }
        bool ready = model.ConfigurationReady;
        AddProfileButton.IsEnabled = ready;
        CustomizeButton.Visibility = model.Device != null && !model.Selected.Contains('@') ? Visibility.Visible : Visibility.Collapsed;
        CustomizeButton.IsEnabled = ready;
        RemoveButton.Visibility = model.Selected != SettingsModel.AllApplications ? Visibility.Visible : Visibility.Collapsed;
        RemoveButton.IsEnabled = ready;
    }
    void RefreshDevices() {
        var devices = model.Status?.Devices ?? [];
        var device = model.Device;
        var signature = string.Join("|", devices.Select(d => $"{d.Id}:{d.Vendor}:{d.Product}:{d.DisplayName}")) + "#" + device?.Id;
        if (signature == deviceSignature) return;
        bool changed = deviceSignature.Split('#')[0] != signature.Split('#')[0];
        deviceSignature = signature;
        DevicePanel.Visibility = device != null ? Visibility.Visible : Visibility.Collapsed;
        NoDeviceText.Visibility = device == null ? Visibility.Visible : Visibility.Collapsed;
        DeviceName.Text = device?.DisplayName ?? "";
        DeviceBox.Visibility = devices.Length > 1 ? Visibility.Visible : Visibility.Collapsed;
        updating = true;
        try {
            Fill(DeviceBox, devices.Select(d => new Choice(d.Id.ToString(CultureInfo.InvariantCulture), d.DisplayName)).ToList(), device?.Id.ToString(CultureInfo.InvariantCulture));
        } finally { updating = false; }
        // Device-specific profile names include the device name.
        if (changed) RefreshProfiles();
    }

    void OnProfileChosen(object sender, SelectionChangedEventArgs e) {
        if (!updating && ProfileBox.SelectedItem is Choice choice) model.Selected = choice.Id;
    }
    void OnDeviceChosen(object sender, SelectionChangedEventArgs e) {
        if (updating || DeviceBox.SelectedItem is not Choice choice || !int.TryParse(choice.Id, NumberStyles.Integer, CultureInfo.InvariantCulture, out int id)) return;
        model.Recording = null; model.SelectedDevice = id;
    }
    void OnAddProfile(object sender, RoutedEventArgs e) {
        AddProfileMenu.PlacementTarget = AddProfileButton;
        AddProfileMenu.Placement = PlacementMode.Bottom;
        AddProfileMenu.MinWidth = AddProfileButton.ActualWidth;
        AddProfileMenu.IsOpen = true;
    }
    void OnAddKnownProfile(object sender, RoutedEventArgs e) {
        if (sender is MenuItem { Tag: string id }) model.AddProfile(id);
    }
    void OnChooseApplication(object sender, RoutedEventArgs e) {
        var dialog = new Microsoft.Win32.OpenFileDialog {
            Title = "Choose application", Filter = "Applications (*.exe)|*.exe",
            InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
        };
        if (dialog.ShowDialog(this) != true) return;
        names.Remember(dialog.FileName);
        model.AddProfile(Path.GetFileName(dialog.FileName));
    }
    void OnCustomize(object sender, RoutedEventArgs e) => model.CustomizeForDevice();
    void OnRemoveProfile(object sender, RoutedEventArgs e) => model.RemoveSelectedProfile();

    // Shortcut recording for the Buttons tab: the next non-modifier key.
    void OnPreviewKeyDown(object sender, KeyEventArgs e) {
        if (model.Recording is not int slot || model.Tab != 1) return;
        var key = e.Key switch { Key.System => e.SystemKey, Key.ImeProcessed => e.ImeProcessedKey, Key.DeadCharProcessed => e.DeadCharProcessedKey, _ => e.Key };
        e.Handled = true;
        if (key is Key.None or Key.LeftShift or Key.RightShift or Key.LeftCtrl or Key.RightCtrl or Key.LeftAlt or Key.RightAlt or Key.LWin or Key.RWin) return;
        int code = KeyInterop.VirtualKeyFromKey(key);
        if (code < 1 || code > 254) return;
        var held = Keyboard.Modifiers;
        ulong modifiers = (held.HasFlag(ModifierKeys.Shift) ? KeyNames.Shift : 0) | (held.HasFlag(ModifierKeys.Control) ? KeyNames.Control : 0)
            | (held.HasFlag(ModifierKeys.Alt) ? KeyNames.Alt : 0) | (held.HasFlag(ModifierKeys.Windows) ? KeyNames.Windows : 0);
        var label = KeyNames.Label(code, modifiers);
        model.Edit(p => p.Buttons[slot] = new ButtonAction { KeyCode = code, Modifiers = modifiers, Label = label });
        model.Recording = null;
    }
}
