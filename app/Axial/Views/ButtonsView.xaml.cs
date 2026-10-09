using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Shapes;

namespace Axial.App;

public partial class ButtonsView : UserControl {
    static readonly Choice[] DriverActions = [
        new("", "No driver action"), new("dominant", "Toggle dominant axis"), new("translation", "Toggle translation"),
        new("rotation", "Toggle rotation"), new("faster", "Increase speed"), new("slower", "Decrease speed"), new("fit", "Fit view"),
        // Standard views for Navlib and 3DconnexionJS applications, such as Fusion.
        new("front", "Front view"), new("back", "Back view"), new("left", "Left view"), new("right", "Right view"),
        new("top", "Top view"), new("bottom", "Bottom view"), new("iso1", "Isometric view (front, right, top)"),
        new("iso2", "Isometric view (front, left, top)"),
        // Keys held while the button is held.
        new("escape", "Esc key"), new("alt", "Alt key"), new("shift", "Shift key"), new("control", "Ctrl key"),
        new("settings", "Open Axial settings"),
    ];
    SettingsModel? model;
    readonly List<ButtonRow> rows = [];
    ControllerLayout? layout;

    public ButtonsView() => InitializeComponent();
    internal void Attach(SettingsModel model) {
        this.model = model;
        model.PropertyChanged += OnModelChanged;
        IsEnabled = model.ConfigurationReady;
        RefreshLayout();
    }
    void OnModelChanged(object? sender, PropertyChangedEventArgs e) {
        if (model == null) return;
        switch (e.PropertyName) {
            case nameof(SettingsModel.Device): RefreshLayout(); RefreshPressed(); break;
            case nameof(SettingsModel.Profile): case nameof(SettingsModel.Commands): case nameof(SettingsModel.Recording): case nameof(SettingsModel.Selected): RefreshRows(); break;
            case nameof(SettingsModel.ConfigurationReady): IsEnabled = model.ConfigurationReady; break;
        }
    }
    void RefreshLayout() {
        if (model == null) return;
        var device = model.Device;
        var next = device?.Layout;
        Summary.Text = device != null && next != null ? $"{device.DisplayName} · {next.Buttons.Count} buttons" : "";
        EmptyText.Text = device == null ? "Connect a controller to configure its buttons." : "Button layout is unavailable for this controller.";
        EmptyText.Visibility = next == null ? Visibility.Visible : Visibility.Collapsed;
        if (ReferenceEquals(next, layout)) return;
        layout = next;
        rows.Clear(); Rows.Children.Clear();
        foreach (var button in layout?.Buttons ?? []) {
            var row = new ButtonRow(button, model, DriverActions);
            rows.Add(row); Rows.Children.Add(row);
        }
        RefreshRows(); RefreshPressed();
    }
    void RefreshRows() {
        if (model == null) return;
        var commands = new List<Choice> { new("", "No app command") };
        commands.AddRange(model.CommandsForSelection.Select(c => new Choice(c.Id, c.Label)));
        foreach (var row in rows) row.Refresh(model.Profile, commands, model.Recording);
    }
    void RefreshPressed() {
        uint buttons = model?.Device?.Buttons ?? 0;
        foreach (var row in rows) row.Pressed((buttons & (1u << row.Slot)) != 0);
    }

    sealed class ButtonRow : Border {
        readonly SettingsModel model;
        readonly IReadOnlyList<Choice> actions;
        readonly Ellipse dot = new() { Width = 10, Height = 10, Margin = new Thickness(0, 0, 10, 0), VerticalAlignment = VerticalAlignment.Center };
        readonly Button record = new() { Margin = new Thickness(8, 0, 0, 0) };
        readonly TextBlock prompt = new() { Text = "Press shortcut…", VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(8, 0, 0, 0) };
        readonly Button cancel = new() { Content = "Cancel", Margin = new Thickness(8, 0, 0, 0) };
        readonly ComboBox command = new() { Width = 320, HorizontalAlignment = HorizontalAlignment.Left };
        readonly ComboBox action = new() { Width = 320, HorizontalAlignment = HorizontalAlignment.Left };
        bool updating, pressed = true;
        public int Slot { get; }

        public ButtonRow(ControllerButton button, SettingsModel model, IReadOnlyList<Choice> actions) {
            this.model = model; this.actions = actions; Slot = button.Id;
            SetResourceReference(StyleProperty, "Card");
            Margin = new Thickness(0, 0, 0, 10);
            prompt.SetResourceReference(TextBlock.ForegroundProperty, "AccentBrush");
            var clear = new Button { Content = "✕", ToolTip = "Clear assignment", Margin = new Thickness(6, 0, 0, 0) };
            clear.SetResourceReference(StyleProperty, "IconButton");
            AutomationProperties.SetName(clear, $"Clear assignment for {button.Name}");
            AutomationProperties.SetName(command, $"Command for {button.Name}");
            AutomationProperties.SetName(action, $"Driver action for {button.Name}");
            var header = new DockPanel { LastChildFill = false };
            var trailing = new StackPanel { Orientation = Orientation.Horizontal };
            trailing.Children.Add(prompt); trailing.Children.Add(cancel); trailing.Children.Add(record); trailing.Children.Add(clear);
            DockPanel.SetDock(trailing, Dock.Right);
            header.Children.Add(trailing);
            header.Children.Add(dot);
            header.Children.Add(new TextBlock { Text = button.Name, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center });
            action.ItemsSource = actions;
            var stack = new StackPanel();
            stack.Children.Add(header);
            stack.Children.Add(Row("Command", command));
            stack.Children.Add(Row("Driver action", action));
            Child = stack;
            int slot = Slot;
            record.Click += (_, _) => model.Recording = slot;
            cancel.Click += (_, _) => model.Recording = null;
            clear.Click += (_, _) => model.Edit(p => p.Buttons[slot] = new ButtonAction());
            command.SelectionChanged += (_, _) => {
                if (updating || command.SelectedItem is not Choice choice) return;
                model.Edit(p => p.Buttons[slot] = new ButtonAction { Command = choice.Id.Length == 0 ? null : choice.Id });
            };
            action.SelectionChanged += (_, _) => {
                if (updating || action.SelectedItem is not Choice choice) return;
                model.Edit(p => p.Buttons[slot] = new ButtonAction { Action = choice.Id.Length == 0 ? null : choice.Id });
            };
            Pressed(false);
        }
        static DockPanel Row(string label, ComboBox box) {
            var row = new DockPanel { Margin = new Thickness(0, 10, 0, 0) };
            row.Children.Add(new TextBlock { Text = label, Width = 102, VerticalAlignment = VerticalAlignment.Center });
            row.Children.Add(box);
            return row;
        }
        public void Pressed(bool down) {
            if (pressed == down) return;
            pressed = down;
            dot.Fill = down ? (Brush)FindResource("GreenBrush") : new SolidColorBrush(Color.FromArgb(0x33, 0xFF, 0xFF, 0xFF));
        }
        public void Refresh(Profile profile, List<Choice> commands, int? recording) {
            var assigned = profile.Buttons[Slot];
            bool capturing = recording == Slot;
            prompt.Visibility = cancel.Visibility = capturing ? Visibility.Visible : Visibility.Collapsed;
            record.Visibility = capturing ? Visibility.Collapsed : Visibility.Visible;
            record.Content = assigned.KeyCode == null ? "Record shortcut" : assigned.Label ?? "Replace shortcut";
            var choices = commands;
            if (assigned.Command is { } id && !commands.Any(c => c.Id == id)) choices = [.. commands, new Choice(id, id)];
            updating = true;
            try {
                if (command.ItemsSource is not List<Choice> current || !current.SequenceEqual(choices)) command.ItemsSource = choices;
                command.SelectedItem = choices.First(c => c.Id == (assigned.Command ?? ""));
                action.SelectedItem = actions.FirstOrDefault(c => c.Id == (assigned.Action ?? "")) ?? actions[0];
            } finally { updating = false; }
        }
    }
}
