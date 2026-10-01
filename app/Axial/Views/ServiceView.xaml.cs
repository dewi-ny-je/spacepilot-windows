using System;
using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace Axial.App;

public partial class ServiceView : UserControl {
    SettingsModel? model;
    Action? restoreWindow;
    readonly DiagnosticChart[] charts = new DiagnosticChart[4];

    public ServiceView() {
        InitializeComponent();
        IsVisibleChanged += (_, _) => { Refresh(); if (IsVisible) RefreshCharts(); };
    }
    internal void Attach(SettingsModel model, Action restoreWindow) {
        this.model = model; this.restoreWindow = restoreWindow;
        Brush Color(string key) => (Brush)FindResource(key);
        charts[0] = new DiagnosticChart("Input reports", "/s", Color("BlueBrush"), s => s.ReportsPerSecond);
        charts[1] = new DiagnosticChart("Connected clients", "", Color("TealBrush"), s => s.ClientCount);
        charts[2] = new DiagnosticChart("Queue overflows", "/s", Color("OrangeBrush"), s => s.Overflows);
        charts[3] = new DiagnosticChart("Ignored reports", "/s", Color("RedBrush"), s => s.Ignored);
        foreach (var chart in charts) Charts.Children.Add(chart);
        model.PropertyChanged += OnModelChanged;
        Refresh();
    }
    void OnModelChanged(object? sender, PropertyChangedEventArgs e) {
        switch (e.PropertyName) {
            case nameof(SettingsModel.Diagnostics): if (IsVisible) RefreshCharts(); break;
            case nameof(SettingsModel.Status) or nameof(SettingsModel.Config) or nameof(SettingsModel.LaunchAtLogin) or nameof(SettingsModel.ConfigurationReady): Refresh(); break;
        }
    }
    void RefreshCharts() {
        if (model == null) return;
        foreach (var chart in charts) chart.Update(model.Diagnostics.Samples);
    }
    // Mirrors axial's Service & diagnostics pane; Windows has no Accessibility
    // permission, but SendInput cannot reach elevated windows from a normal process.
    internal void Refresh() {
        if (model == null) return;
        bool spin = model.WebSetup.Busy && IsVisible;
        if (spin != (Spinner.Visibility == Visibility.Visible)) {
            Spinner.Visibility = spin ? Visibility.Visible : Visibility.Collapsed;
            SpinnerRotation.BeginAnimation(RotateTransform.AngleProperty, spin ? new DoubleAnimation(0, 360, TimeSpan.FromSeconds(1)) { RepeatBehavior = RepeatBehavior.Forever } : null);
        }
        if (!IsVisible) return;
        var status = model.Status;
        var setup = model.WebSetup;
        LoginToggle.IsChecked = model.LaunchAtLogin;
        ShortcutText.Text = status == null ? "Keyboard shortcuts need the Axial service"
            : status.Accessibility ? "Keyboard shortcuts enabled" : "Keyboard shortcuts unavailable";
        ElevationText.Text = status?.Elevated == true
            ? "The Axial service runs as administrator, so shortcuts also reach applications running as administrator."
            : "Windows does not deliver shortcuts to applications running as administrator. To use shortcuts there, run that application normally or start Axial as administrator.";
        bool enabled = model.Config.Web?.Enabled ?? true;
        WebGroup.IsEnabled = model.ConfigurationReady;
        WebToggle.IsChecked = enabled;
        WebToggle.IsEnabled = !setup.Busy;
        WebStatus.Text = !enabled ? "Web navigation is disabled."
            : setup.Ready ? (status?.Web?.Listening == true ? "Ready" : status?.Web is { Error: { Length: > 0 } error } ? error : setup.Message)
            : setup.Message;
        SetupButton.Content = setup.Ready ? "Check / Repair…" : "Set Up…";
        SetupButton.IsEnabled = !setup.Busy && enabled && status?.Mock == false;
        WebHelp.Visibility = !setup.Ready && enabled ? Visibility.Visible : Visibility.Collapsed;
        MockLabel.Visibility = status?.Mock == true ? Visibility.Visible : Visibility.Collapsed;
    }

    void OnLogin(object sender, RoutedEventArgs e) {
        model?.Login(LoginToggle.IsChecked == true);
        Refresh();
    }
    void OnWebToggle(object sender, RoutedEventArgs e) {
        bool on = WebToggle.IsChecked == true;
        model?.EditWeb(web => web.Enabled = on);
    }
    async void OnSetUp(object sender, RoutedEventArgs e) {
        if (model != null) await model.SetUpWebAsync(restoreWindow);
    }
    void OnExport(object sender, RoutedEventArgs e) {
        if (model == null) return;
        var dialog = new Microsoft.Win32.SaveFileDialog { FileName = "Axial-diagnostics.json", Filter = "JSON files (*.json)|*.json", DefaultExt = ".json" };
        if (dialog.ShowDialog(Window.GetWindow(this)) == true) model.ExportDiagnostics(dialog.FileName);
    }
}
