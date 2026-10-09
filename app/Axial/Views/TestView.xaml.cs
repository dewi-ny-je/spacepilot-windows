using System;
using System.Collections.Specialized;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Media3D;
using Numerics = System.Numerics;

namespace Axial.App;

// The Test tab: a toy car driven by the live preview through the selected
// profile, as axial's SceneTest, plus the button event log.
public partial class TestView : UserControl {
    SettingsModel? model;
    IPreview? preview;
    TestNavigation navigation = new();
    Numerics.Quaternion orientation = Numerics.Quaternion.Identity;
    readonly double[] axes = new double[6];
    bool loading, resetRequested, rendering, watching;
    TimeSpan lastTime;
    Vector3D right, up;
    Window? observedWindow;

    public TestView() {
        InitializeComponent();
        var look = Camera.LookDirection; look.Normalize();
        right = Vector3D.CrossProduct(look, Camera.UpDirection); right.Normalize();
        up = Vector3D.CrossProduct(right, look); up.Normalize();
        IsVisibleChanged += (_, _) => { if (IsVisible) Load(); UpdateRendering(); };
        Loaded += (_, _) => {
            if (observedWindow == null && Window.GetWindow(this) is { } window) { observedWindow = window; window.StateChanged += (_, _) => UpdateRendering(); }
        };
        PreviewActivity.Changed += () => { if (!rendering) UpdateRendering(); };
        ApplyTransform();
    }
    internal void Attach(SettingsModel model, IPreview preview) {
        this.model = model; this.preview = preview;
        Log.ItemsSource = model.ButtonLog;
        model.ButtonLog.CollectionChanged += OnLogChanged;
        model.PropertyChanged += OnModelChanged;
        RefreshLog();
    }
    void OnModelChanged(object? sender, PropertyChangedEventArgs e) {
        switch (e.PropertyName) {
            case nameof(SettingsModel.Profile): case nameof(SettingsModel.SelectedDevice): UpdateRendering(); break;
            case nameof(SettingsModel.LostButtonLogs): case nameof(SettingsModel.ButtonLogError): RefreshLog(); break;
        }
    }
    void OnLogChanged(object? sender, NotifyCollectionChangedEventArgs e) => RefreshLog();
    void RefreshLog() {
        if (model == null) return;
        int count = model.ButtonLog.Count;
        Count.Text = count.ToString(CultureInfo.CurrentCulture);
        EmptyText.Visibility = count == 0 ? Visibility.Visible : Visibility.Collapsed;
        LostText.Text = $"{model.LostButtonLogs} events exceeded the log buffer.";
        LostText.Visibility = model.LostButtonLogs > 0 ? Visibility.Visible : Visibility.Collapsed;
        ErrorText.Text = model.ButtonLogError ?? "";
        ErrorText.Visibility = model.ButtonLogError != null ? Visibility.Visible : Visibility.Collapsed;
    }

    async void Load() {
        if (loading) return;
        loading = true;
        var path = Path.Combine(AppContext.BaseDirectory, "Assets", "toycar", "ToyCar.gltf");
        try {
            // The upstream fabric display stand is omitted, as in axial.
            Car.Content = await Task.Run(() => GltfLoader.Load(path, name => name == "Fabric"));
        } catch (Exception error) {
            ModelError.Text = $"Could not load the toy car: {error.Message}";
            ModelError.Visibility = Visibility.Visible;
        }
        Loading.Visibility = Visibility.Collapsed;
    }

    bool Shown => IsVisible && Window.GetWindow(this) is { IsVisible: true, WindowState: not WindowState.Minimized };
    // Filtered preview input for the selected device, as the service applies it.
    bool ReadAxes() {
        Array.Clear(axes);
        if (model == null || preview == null) return false;
        preview.Read((uint)(model.Device?.Id ?? 0), axes, out _);
        MotionFilter.Apply(axes, model.Profile);
        return axes.Any(v => v != 0);
    }
    // Render at display rate only while the scene is visible and moving. An
    // idle visible scene asks the bridge for an activity callback instead.
    void UpdateRendering() {
        bool shown = Shown;
        bool enabled = shown && (resetRequested || ReadAxes());
        SetRendering(enabled);
        Watch(shown && !enabled && preview != null);
    }
    void SetRendering(bool enabled) {
        if (rendering == enabled) return;
        rendering = enabled; lastTime = TimeSpan.Zero;
        if (enabled) CompositionTarget.Rendering += OnFrame;
        else CompositionTarget.Rendering -= OnFrame;
    }
    void Watch(bool enabled) {
        if (watching == enabled) return;
        watching = enabled;
        NativePreview.Watch(enabled);
    }
    void OnFrame(object? sender, EventArgs e) {
        var time = e is RenderingEventArgs frame ? frame.RenderingTime : TimeSpan.Zero;
        if (time == lastTime && time != TimeSpan.Zero) return;
        bool reset = resetRequested; resetRequested = false;
        bool moving = ReadAxes();
        if (!reset && !moving) { SetRendering(false); Watch(Shown); return; }
        if (reset) { navigation.Reset(); orientation = Numerics.Quaternion.Identity; }
        double seconds = lastTime == TimeSpan.Zero ? 0 : Math.Clamp((time - lastTime).TotalSeconds, 0, 0.05);
        lastTime = time;
        navigation.Advance(axes[0] / 350, -axes[2] / 350, -axes[1] / 350, seconds);
        orientation = Numerics.Quaternion.Normalize(MotionFilter.Rotation(axes, seconds) * orientation);
        ApplyTransform();
    }
    void ApplyTransform() {
        double aspect = Scene.ActualHeight > 0 ? Scene.ActualWidth / Scene.ActualHeight : 1.5;
        Camera.Width = 2 * navigation.Scale * aspect;
        var pan = right * navigation.Pan.X + up * navigation.Pan.Y;
        CarPosition.OffsetX = pan.X; CarPosition.OffsetY = pan.Y; CarPosition.OffsetZ = pan.Z;
        CarRotation.Quaternion = new Quaternion(orientation.X, orientation.Y, orientation.Z, orientation.W);
    }
    void OnSceneResized(object sender, SizeChangedEventArgs e) => ApplyTransform();
    void OnReset(object sender, RoutedEventArgs e) { resetRequested = true; UpdateRendering(); }
    void OnClear(object sender, RoutedEventArgs e) => model?.ClearButtonLog();
    async void OnExport(object sender, RoutedEventArgs e) {
        if (model == null) return;
        model.CollectButtonLog();
        var dialog = new Microsoft.Win32.SaveFileDialog { FileName = "Axial-buttons.csv", Filter = "CSV files (*.csv)|*.csv", DefaultExt = ".csv" };
        if (dialog.ShowDialog(Window.GetWindow(this)) == true) await model.ExportButtonLogAsync(dialog.FileName);
    }
}
