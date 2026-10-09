using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.Json;

namespace Axial;

public interface ILoginItem {
    bool Enabled { get; }
    void Set(bool enabled);
}

// The settings app's state, ported from axial's Model. All members run on one
// synchronization context (the UI thread); service I/O is awaited.
public sealed class SettingsModel : INotifyPropertyChanged {
    public const string AllApplications = "*";
    readonly IServiceRequesting client;
    readonly IServiceLaunching? launcher;
    readonly IPreview? preview;
    readonly ILoginItem? loginItem;
    readonly Func<string, string?> applicationName;
    readonly Dictionary<string, string> appNames = [];
    SessionLog? log;
    CancellationTokenSource? pollCancel, saveCancel;
    double lastServiceStart = double.NegativeInfinity;
    bool quitting, loaded, ownsPreview, stopped, saveInFlight;
    ulong editRevision, savedRevision, nextLogID;
    readonly List<TaskCompletionSource> saveWaiters = [];

    Status? status;
    Configuration config = new();
    string selected = AllApplications;
    string message = "Connecting to Axial…";
    Dictionary<string, AppCommand[]> commands = [];
    bool configurationReady, launchAtLogin;
    int? selectedDevice, recording;
    int tab;
    ulong lostButtonLogs;
    string? buttonLogError;

    public event PropertyChangedEventHandler? PropertyChanged;
    public WebSetupCoordinator WebSetup { get; }
    public DiagnosticHistory Diagnostics { get; } = new();
    public ObservableCollection<ButtonEntry> ButtonLog { get; } = []; // newest first
    public bool HasUnsavedChanges => editRevision != savedRevision;

    public SettingsModel(IServiceRequesting client, IServiceLaunching? launcher = null, IWebSetupOperations? webSetup = null,
        IPreview? preview = null, ILoginItem? loginItem = null, Func<string, string?>? applicationName = null) {
        this.client = client; this.launcher = launcher; this.preview = preview; this.loginItem = loginItem;
        this.applicationName = applicationName ?? (_ => null);
        WebSetup = new WebSetupCoordinator(webSetup ?? new NativeWebSetup("axial-web-setup.exe"));
        if (preview == null) configurationReady = true;
    }
    // Live start: preview, session log and polling. Tests drive the model directly.
    public void Start(string logDirectory) {
        if (preview != null) { preview.Start(); ownsPreview = true; }
        try { log = new SessionLog(logDirectory); }
        catch (Exception error) { ButtonLogError = $"Could not create button log: {error.Message}"; }
        if (loginItem != null) LaunchAtLogin = loginItem.Enabled;
        StartPolling();
    }

    void Set<T>(ref T field, T value, [CallerMemberName] string? name = null) {
        if (EqualityComparer<T>.Default.Equals(field, value)) return;
        field = value; Changed(name);
    }
    void Changed(string? name) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));

    public Status? Status { get => status; private set { status = value; Changed(nameof(Status)); Changed(nameof(Device)); } }
    public Configuration Config { get => config; private set { config = value; ConfigChanged(); } }
    public string Selected { get => selected; set { if (selected == value) return; selected = value; Changed(nameof(Selected)); ConfigChanged(); } }
    public string Message { get => message; set => Set(ref message, value); }
    public Dictionary<string, AppCommand[]> Commands { get => commands; private set => Set(ref commands, value); }
    public bool ConfigurationReady { get => configurationReady; private set => Set(ref configurationReady, value); }
    public bool LaunchAtLogin { get => launchAtLogin; private set => Set(ref launchAtLogin, value); }
    public int? SelectedDevice { get => selectedDevice; set { if (selectedDevice == value) return; selectedDevice = value; Changed(nameof(SelectedDevice)); Changed(nameof(Device)); } }
    public int Tab { get => tab; set => Set(ref tab, value); }
    public int? Recording { get => recording; set => Set(ref recording, value); }
    public ulong LostButtonLogs { get => lostButtonLogs; private set => Set(ref lostButtonLogs, value); }
    public string? ButtonLogError { get => buttonLogError; private set => Set(ref buttonLogError, value); }
    void ConfigChanged() { Changed(nameof(Config)); Changed(nameof(Profile)); Changed(nameof(ProfileKeys)); }

    public Device? Device => Status?.Devices.FirstOrDefault(d => d.Id == SelectedDevice) ?? Status?.Devices.FirstOrDefault();
    public Profile Profile => Config.Profiles.GetValueOrDefault(Selected) ?? Config.Profiles.GetValueOrDefault(AllApplications) ?? new Profile();
    public IReadOnlyList<string> ProfileKeys => Config.Profiles.Keys.OrderBy(k => k, StringComparer.Ordinal).ToList();

    public void StartPolling() {
        if (pollCancel != null || stopped) return;
        pollCancel = new CancellationTokenSource();
        _ = Poll(new WeakReference<SettingsModel>(this), client, pollCancel.Token);
    }
    // The loop holds the model weakly, so a suspended request never retains it.
    static async Task Poll(WeakReference<SettingsModel> weak, IServiceRequesting client, CancellationToken token) {
        int count = 0;
        while (!token.IsCancellationRequested) {
            var data = await client.RequestAsync("{\"op\":\"status\"}");
            if (token.IsCancellationRequested) break;
            if (!await PollStep(weak, client, data, count, token)) break;
            count++;
            try { await Task.Delay(1000, token); } catch (TaskCanceledException) { break; }
        }
    }
    static async Task<bool> PollStep(WeakReference<SettingsModel> weak, IServiceRequesting client, string data, int count, CancellationToken token) {
        if (!weak.TryGetTarget(out var self)) return false;
        self.AcceptStatus(data);
        if (self.Status != null && self.launcher?.IsRunning == false) {
            // Replace a helper left by an older installation with our child.
            // Wait for a disconnected poll before starting, so HID is released.
            await client.RequestAsync("{\"op\":\"stop\"}");
            if (token.IsCancellationRequested) return false;
            self.AcceptStatus("");
        } else if (self.Status == null) self.EnsureServiceRunning();
        if (self.Status != null) {
            if (!self.loaded) await self.LoadAsync();
            if (self.HasUnsavedChanges) await self.SaveAsync();
            if (count % 3 == 0) await self.LoadCommandsAsync();
            if (count % 5 == 0) await self.RefreshWebSetupAsync();
        }
        return true;
    }

    public void AcceptStatus(string data) {
        if (stopped) return;
        if (Json.TryDecode<Status>(data) is { } next) {
            var previous = Device;
            Status = next;
            Diagnostics.Record(Clock.Seconds, DateTimeOffset.Now, next.Reports, next.ConnectedClients, next.Overflows, next.Rejected);
            Changed(nameof(Diagnostics));
            if (!next.Devices.Any(d => d.Id == SelectedDevice)) SelectedDevice = next.Devices.FirstOrDefault()?.Id;
            var current = Device;
            if (previous?.Id != current?.Id || previous?.Vendor != current?.Vendor || previous?.Product != current?.Product) Recording = null;
        } else {
            if (Status != null) { Diagnostics.Disconnect(); Changed(nameof(Diagnostics)); }
            Status = null; loaded = false; Recording = null;
            Message = "Connecting to Axial…";
        }
    }
    // Live axes and buttons for the Motion and Buttons tabs, while visible.
    public void RefreshPreview(bool visible) {
        if (!visible || (Tab != 0 && Tab != 1) || preview == null) return;
        if (Status is not { } next || SelectedDevice is not int id) return;
        int index = Array.FindIndex(next.Devices, d => d.Id == id);
        if (index < 0) return;
        var axes = new double[6];
        if (!preview.Read((uint)id, axes, out uint buttons)) return;
        var values = axes.Select(x => (int)x).ToArray();
        var device = next.Devices[index];
        if ((Tab == 0 && !device.Axes.SequenceEqual(values)) || device.Buttons != buttons) {
            var devices = (Device[])next.Devices.Clone();
            devices[index] = device with { Axes = values, Buttons = buttons };
            Status = next with { Devices = devices };
        }
    }
    public void Shutdown() {
        if (stopped) return; stopped = true;
        pollCancel?.Cancel(); saveCancel?.Cancel();
        if (ownsPreview) { preview?.Stop(); ownsPreview = false; }
        CollectButtonLog(4096); log?.Flush();
    }
    public void CollectButtonLog(int limit = 256) {
        if (preview == null) return;
        var csv = new StringBuilder();
        var wall = DateTimeOffset.Now; ulong monotonic = preview.Now();
        // Bound UI-thread work even when a producer continuously fills the ring.
        for (int n = 0; n < limit && preview.PopLog(out var timestamp, out var device, out var changed, out var buttons, out var reason, out var identity); n++) {
            var time = wall.AddTicks((long)(((double)timestamp - monotonic) / 100));
            for (int index = 0; index < 32; index++) {
                if ((changed & (1u << index)) == 0) continue;
                var entry = new ButtonEntry(++nextLogID, time, device, index + 1, (buttons & (1u << index)) != 0, reason != 3, identity);
                ButtonLog.Insert(0, entry); csv.Append(entry.CsvRow);
            }
        }
        while (ButtonLog.Count > 2000) ButtonLog.RemoveAt(ButtonLog.Count - 1);
        if (csv.Length > 0) log?.Append(Encoding.UTF8.GetBytes(csv.ToString()));
        if (log?.Error is { } error) ButtonLogError = error;
        LostButtonLogs = preview.LostLogs();
    }
    public void ClearButtonLog() => ButtonLog.Clear();
    public async Task ExportButtonLogAsync(string destination) {
        CollectButtonLog();
        if (log == null) return;
        try { await log.ExportAsync(destination); }
        catch (Exception error) { Message = $"Could not export button log: {error.Message}"; }
    }

    public void Edit(Action<Profile> change) {
        if (!ConfigurationReady) return;
        var p = Profile.Clone(); change(p); Config.Profiles[Selected] = p; editRevision++; ConfigChanged();
        ScheduleSave(250);
    }
    void ScheduleSave(int milliseconds) {
        saveCancel?.Cancel();
        var cancel = saveCancel = new CancellationTokenSource();
        _ = Debounce(new WeakReference<SettingsModel>(this), milliseconds, cancel.Token);
    }
    static async Task Debounce(WeakReference<SettingsModel> weak, int milliseconds, CancellationToken token) {
        try { await Task.Delay(milliseconds, token); } catch (TaskCanceledException) { return; }
        if (weak.TryGetTarget(out var self)) await self.SaveAsync();
    }
    public async Task LoadAsync() {
        var revision = editRevision;
        var data = await client.RequestAsync("{\"op\":\"getConfig\"}");
        if (stopped || Json.TryDecode<Configuration>(data) is not { } value) return;
        loaded = true; ConfigurationReady = true;
        if (editRevision == revision && !HasUnsavedChanges) {
            Config = value; Message = "";
            if (!Config.Profiles.ContainsKey(Selected)) Selected = AllApplications;
        }
    }
    public async Task LoadCommandsAsync() {
        var data = await client.RequestAsync("{\"op\":\"getCommands\"}");
        if (!stopped && Json.TryDecode<Dictionary<string, AppCommand[]>>(data) is { } value) Commands = value;
    }
    public void EditWeb(Action<WebConfiguration> change) {
        if (!ConfigurationReady) return;
        var web = Config.Web ?? new WebConfiguration(); change(web); Config.Web = web; editRevision++; ConfigChanged();
        ScheduleSave(300);
    }
    public Task RefreshWebSetupAsync() {
        if (Status?.Mock != false || !ConfigurationReady) return Task.CompletedTask;
        var client = this.client;
        return WebSetup.RefreshAsync(Config.Web?.Enabled ?? true, Status?.Web?.Listening ?? false, Status?.Web?.Error ?? "",
            async () => await client.RequestAsync("{\"op\":\"retryWeb\"}"));
    }
    public Task SetUpWebAsync(Action? restoreWindow = null) {
        if (Status?.Mock != false || stopped || Config.Web?.Enabled == false) return Task.CompletedTask;
        var client = this.client;
        return WebSetup.SetUpAsync(async () => { if (!quitting && !stopped) await client.RequestAsync("{\"op\":\"retryWeb\"}"); },
            () => { if (!quitting && !stopped) restoreWindow?.Invoke(); });
    }
    public async Task SaveAsync() {
        if (stopped || !ConfigurationReady) return;
        if (saveInFlight) {
            var waiter = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            saveWaiters.Add(waiter); await waiter.Task; return;
        }
        if (!HasUnsavedChanges) return;
        saveInFlight = true;
        try {
            do {
                var revision = editRevision;
                var json = Json.Encode(Config);
                var data = await client.RequestAsync("{\"op\":\"setConfig\",\"config\":" + json + "}");
                bool saved = false; string? error = null;
                try {
                    using var response = JsonDocument.Parse(data);
                    if (response.RootElement.ValueKind == JsonValueKind.Object) {
                        saved = response.RootElement.TryGetProperty("ok", out var ok) && ok.ValueKind == JsonValueKind.True;
                        if (response.RootElement.TryGetProperty("error", out var e) && e.ValueKind == JsonValueKind.String) error = e.GetString();
                    }
                } catch (JsonException) { }
                if (saved) { savedRevision = revision; Changed(nameof(HasUnsavedChanges)); }
                if (revision == editRevision) {
                    Message = saved ? "" : error ?? "Could not save settings: the service did not respond.";
                    return;
                }
                // Edits that arrive while a request is in flight are committed next.
            } while (!stopped);
        } finally {
            saveInFlight = false;
            var waiters = saveWaiters.ToArray(); saveWaiters.Clear();
            foreach (var waiter in waiters) waiter.TrySetResult();
        }
    }
    // Zero calibration: the selected device's current deflection becomes its
    // rest position until it is cleared or the device is reconnected.
    public async Task CalibrateAsync(bool clear = false) {
        if (stopped || Device is not { } device) return;
        var request = "{\"op\":\"calibrate\",\"device\":" + device.Id + (clear ? ",\"clear\":true}" : "}");
        var data = await client.RequestAsync(request);
        bool done = false;
        try {
            using var response = JsonDocument.Parse(data);
            done = response.RootElement.ValueKind == JsonValueKind.Object && response.RootElement.TryGetProperty("ok", out var ok) && ok.ValueKind == JsonValueKind.True;
        } catch (JsonException) { }
        Message = done ? "" : "Could not calibrate: the service did not respond.";
        AcceptStatus(await client.RequestAsync("{\"op\":\"status\"}"));
    }
    void EnsureServiceRunning() {
        if (quitting || stopped || launcher == null || launcher.IsRunning) return;
        double now = Clock.Seconds;
        if (now - lastServiceStart < 3) return;
        lastServiceStart = now;
        try { launcher.Start(); Message = "Starting service…"; }
        catch (Exception error) { Message = $"Could not start service: {error.Message}"; }
    }
    public async Task PrepareToQuitAsync() {
        if (quitting) return; quitting = true;
        pollCancel?.Cancel(); saveCancel?.Cancel();
        if (HasUnsavedChanges) await SaveAsync();
        await client.RequestAsync("{\"op\":\"stop\"}");
        launcher?.Terminate();
        // Wait for an owned helper to release HID and held keys before exiting.
        var deadline = Clock.Seconds + 3;
        while (launcher?.IsRunning == true && Clock.Seconds < deadline) await Task.Delay(20);
        Shutdown();
    }
    public void Login(bool enabled) {
        if (loginItem == null) return;
        try { loginItem.Set(enabled); LaunchAtLogin = loginItem.Enabled; }
        catch (Exception error) { Message = error.Message; }
    }
    public void AddProfile(string id) {
        if (!ConfigurationReady) return;
        id = id.ToLowerInvariant();
        Config.Profiles[id] = Config.Profiles.GetValueOrDefault(id) ?? Config.Profiles.GetValueOrDefault(AllApplications)?.Clone() ?? new Profile();
        Selected = id; Edit(_ => { });
    }
    public void CustomizeForDevice() {
        if (!ConfigurationReady || Device is not { } d || Selected.Contains('@')) return;
        var key = $"{Selected}@{d.Vendor:x4}:{d.Product:x4}";
        Config.Profiles[key] = Profile.Clone(); Selected = key; Edit(_ => { });
    }
    public void RemoveSelectedProfile() {
        if (!ConfigurationReady || Selected == AllApplications) return;
        Config.Profiles.Remove(Selected); Selected = AllApplications; editRevision++; ConfigChanged();
        _ = SaveAsync();
    }
    public string ProfileName(string key) {
        var parts = key.Split('@', 2);
        var id = parts[0];
        string name = id switch {
            AllApplications => "All applications",
            "fusion360.exe" => "Autodesk Fusion",
            "prusa-slicer.exe" => "PrusaSlicer",
            "freecad.exe" => "FreeCAD",
            "blender.exe" => "Blender",
            _ => appNames.GetValueOrDefault(id) ?? applicationName(id) ?? (id.EndsWith(".exe", StringComparison.Ordinal) ? id[..^4] : id),
        };
        appNames[id] = name;
        if (parts.Length == 2) {
            var device = Status?.Devices.FirstOrDefault(d => $"{d.Vendor:x4}:{d.Product:x4}" == parts[1]);
            name += " — " + (device?.DisplayName ?? "USB controller");
        }
        return name;
    }
    public IReadOnlyList<AppCommand> CommandsForSelection =>
        Commands.GetValueOrDefault(Selected.Split('@')[0]) ?? [];
    public void ExportDiagnostics(string path) {
        try {
            var export = new { status = Status, history = Diagnostics.Samples };
            File.WriteAllText(path, JsonSerializer.Serialize(export, new JsonSerializerOptions(Json.Options) { WriteIndented = true }));
        } catch (Exception error) { Message = error.Message; }
    }
}
