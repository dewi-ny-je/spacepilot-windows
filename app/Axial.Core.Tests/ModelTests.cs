using System.Runtime.CompilerServices;
using System.Text.Json;
using Xunit;

namespace Axial.Tests;

public class ModelTests {
    static string StatusJson(int vendor, int product) => Json.Encode(new Status {
        Devices = [new Device { Id = 1, Vendor = vendor, Product = product, Name = "stale service name", Axes = [0, 0, 0, 0, 0, 0] }],
        Clients = 1, Mock = true });

    [Fact]
    public void ConnectedClientChartIncludesNativeAndWebSocketConnections() {
        foreach (var (native, web, expected) in new (int, int?, int)[] { (2, null, 2), (2, 0, 2), (2, 3, 5), (0, 3, 3) }) {
            var model = new SettingsModel(new MockControl());
            var status = new Status { Web = web is int n ? new WebStatus(true, true, n, "") : null, Devices = [], Clients = native, Mock = true };
            model.AcceptStatus(Json.Encode(status));
            Assert.Equal(expected, model.Diagnostics.Samples[^1].Clients);
            model.Shutdown();
        }
    }

    [Fact]
    public void CatalogNamesSparseSlotsCsvAndRecordingCancellation() {
        var explorer = DeviceCatalog.Layouts[0x046dc627];
        Assert.Equal("SpaceExplorer", explorer.Name); Assert.Equal(15, explorer.Buttons.Count);
        Assert.Equal("Fit", explorer.ButtonName(10)); Assert.Equal("2D", explorer.ButtonName(14));
        Assert.Equal("SpaceNavigator", DeviceCatalog.Layouts[0x046dc626].Name);
        Assert.Equal("SpaceNavigator for Notebooks", DeviceCatalog.Layouts[0x046dc628].Name);
        Assert.Equal(["Left", "Right"], DeviceCatalog.Layouts[0x256fc635].Buttons.Select(b => b.Name));
        Assert.Equal(["Left", "Right"], DeviceCatalog.Layouts[0x256fc652].Buttons.Select(b => b.Name));
        Assert.Equal(31, DeviceCatalog.Layouts[0x256fc633].Buttons.Count);
        Assert.Equal(21, DeviceCatalog.Layouts[0x046dc629].Buttons.Count);
        Assert.Equal(15, DeviceCatalog.Layouts.Count); Assert.False(DeviceCatalog.Layouts.ContainsKey(0xffffffff));
        Assert.Null(DeviceCatalog.Identity(-1, 1));
        var pro = DeviceCatalog.Layouts[0x046dc62b];
        Assert.Equal(15, pro.Buttons.Count); Assert.Equal("1", pro.Buttons[0].Name); Assert.Equal(12, pro.Buttons[0].Id);
        var bindings = new Profile(); bindings.Buttons[12] = new ButtonAction { Command = "existing-binding" };
        Assert.Equal("existing-binding", bindings.Buttons[pro.Buttons[0].Id].Command);
        var namedPress = new ButtonEntry(1, DateTimeOffset.FromUnixTimeSeconds(0), 1, 11, true, false, 0x046dc627);
        var namedRelease = new ButtonEntry(2, DateTimeOffset.FromUnixTimeSeconds(1), 1, 11, false, true, 0x046dc627);
        var reusedID = new ButtonEntry(3, DateTimeOffset.Now, 1, 2, true, false, 0x256fc635);
        Assert.Equal(("SpaceExplorer", "Fit"), (namedPress.DeviceName, namedPress.ButtonName));
        Assert.Equal(("SpaceExplorer", "Fit"), (namedRelease.DeviceName, namedRelease.ButtonName));
        Assert.Equal(("SpaceMouse Compact", "Right"), (reusedID.DeviceName, reusedID.ButtonName));
        Assert.Equal("1970-01-01T00:00:00.000Z,1,11,pressed,device,\"SpaceExplorer\",\"Fit\"\n", namedPress.CsvRow);
        var selection = new SettingsModel(new MockControl());
        selection.AcceptStatus(StatusJson(0x046d, 0xc627)); selection.Recording = 10;
        selection.AcceptStatus(StatusJson(0x046d, 0xc627)); Assert.Equal(10, selection.Recording);
        selection.AcceptStatus(StatusJson(0x256f, 0xc635));
        Assert.Null(selection.Recording); Assert.Equal("SpaceMouse Compact", selection.Device?.DisplayName);
        selection.Recording = 0; selection.AcceptStatus(""); Assert.Null(selection.Recording);
        selection.Shutdown();
    }

    [Fact]
    public void ProfilesDecodeWithDefaultsAndRejectBadDimensions() {
        var config = Json.TryDecode<Configuration>("{\"version\":1,\"profiles\":{\"*\":{\"gain\":[2,1,1,1,1,1],\"buttons\":[{\"keyCode\":65,\"modifiers\":2,\"label\":\"Ctrl+A\"}]}}}")!;
        var profile = config.Profiles["*"];
        Assert.Equal(2, profile.Gain[0]); Assert.True(profile.Translation); Assert.Equal(32, profile.Buttons.Length);
        Assert.Equal(65, profile.Buttons[0].KeyCode); Assert.True(profile.Buttons[1].IsEmpty);
        Assert.Null(Json.TryDecode<Configuration>("{\"version\":1,\"profiles\":{\"*\":{\"gain\":[1,1]}}}"));
        // Unset optional fields are omitted, so the service sees the same document as from macOS.
        var encoded = Json.Encode(new Configuration());
        Assert.DoesNotContain("keyCode", encoded); Assert.DoesNotContain("\"web\"", encoded);
        using var document = JsonDocument.Parse(encoded);
        Assert.Equal(32, document.RootElement.GetProperty("profiles").GetProperty("*").GetProperty("buttons").GetArrayLength());
    }

    [Fact]
    public void StaleReadsOrderedSavesFailedRetries() => SingleThreadContext.Run(async () => {
        // An old config read must not overwrite an edit made while it was pending.
        var control = new MockControl();
        var model = new SettingsModel(control);
        var loading = model.LoadAsync();
        await control.WaitForCount(1);
        model.Edit(p => p.Gain[0] = 2);
        control.Respond(0, "{\"version\":1,\"profiles\":{\"*\":{}}}");
        await loading;
        Assert.Equal(2, model.Profile.Gain[0]);
        var saving = model.SaveAsync();
        await control.WaitForCount(2);
        model.Edit(p => p.Gain[0] = 3);
        var waiter = model.SaveAsync();
        control.Respond(1, "{\"ok\":true}");
        await control.WaitForCount(3);
        using (var json = JsonDocument.Parse(control.Requests()[2]))
            Assert.Equal(3, json.RootElement.GetProperty("config").GetProperty("profiles").GetProperty("*").GetProperty("gain")[0].GetDouble());
        control.Respond(2, "{\"ok\":true}");
        await saving; await waiter;
        Assert.False(model.HasUnsavedChanges); Assert.Equal("", model.Message);
        model.Shutdown();

        // A failed write remains dirty and a subsequent successful retry clears it.
        var retryControl = new MockControl(); var retryModel = new SettingsModel(retryControl);
        retryModel.Edit(p => p.Led = false);
        var failed = retryModel.SaveAsync(); await retryControl.WaitForCount(1);
        retryControl.Respond(0, "{\"error\":\"disk full\"}"); await failed;
        Assert.True(retryModel.HasUnsavedChanges); Assert.Equal("disk full", retryModel.Message);
        var retried = retryModel.SaveAsync(); await retryControl.WaitForCount(2);
        retryControl.Respond(1, "{\"ok\":true}"); await retried;
        Assert.False(retryModel.HasUnsavedChanges); retryModel.Shutdown();
    });

    [MethodImpl(MethodImplOptions.NoInlining)]
    static WeakReference<SettingsModel> StartPollingModel(IServiceRequesting control) {
        var model = new SettingsModel(control); model.StartPolling();
        return new WeakReference<SettingsModel>(model);
    }

    [Fact]
    public void PollingDoesNotRetainTheModelAndRunsAtOneHertz() => SingleThreadContext.Run(async () => {
        var pollControl = new MockControl();
        var weak = StartPollingModel(pollControl);
        await pollControl.WaitForCount(1);
        for (int i = 0; i < 5 && Lifetime.Alive(weak); i++) { GC.Collect(); GC.WaitForPendingFinalizers(); await Task.Yield(); }
        Assert.False(Lifetime.Alive(weak), "Polling retained the model");
        pollControl.Respond(0, "{}"); await Task.Delay(20);
        Assert.Single(pollControl.Requests());
        var responsive = new ResponsiveControl(); var rateModel = new SettingsModel(responsive);
        rateModel.StartPolling(); await Task.Delay(2200); rateModel.Shutdown();
        Assert.InRange(responsive.StatusRequests, 2, 3);
    });

    [Fact]
    public void AutomaticServiceStartTakeoverAndQuit() => SingleThreadContext.Run(async () => {
        var automaticControl = new MockControl(); var launcher = new MockLauncher();
        var automatic = new SettingsModel(automaticControl, launcher);
        automatic.StartPolling(); await automaticControl.WaitForCount(1);
        automaticControl.Respond(0, "{}"); await automaticControl.WaitForCount(2);
        Assert.Equal(1, launcher.Starts);
        automaticControl.Respond(1, "{}"); await Task.Yield(); await Task.Delay(10);
        Assert.Equal(1, launcher.Starts); // an in-flight start must not launch another process
        var quitting = automatic.PrepareToQuitAsync();
        await automaticControl.WaitForCount(3);
        Assert.Contains("stop", automaticControl.Requests()[2]);
        automaticControl.Respond(2, "{\"ok\":true}"); await quitting;
        Assert.Equal(1, launcher.Terminations); Assert.False(launcher.IsRunning);
        await Task.Delay(1100);
        Assert.Equal(3, automaticControl.Requests().Count); Assert.Equal(1, launcher.Starts);

        var existingControl = new MockControl(); var replacement = new MockLauncher();
        var existing = new SettingsModel(existingControl, replacement);
        existing.StartPolling(); await existingControl.WaitForCount(1);
        existingControl.Respond(0, "{\"devices\":[],\"clients\":1,\"reports\":0,\"overflows\":0,\"rejected\":0,\"foregroundApp\":\"\",\"accessibility\":true,\"mock\":true}");
        await existingControl.WaitForCount(2);
        Assert.Contains("stop", existingControl.Requests()[1]); Assert.Equal(0, replacement.Starts);
        existingControl.Respond(1, "{\"ok\":true}");
        await existingControl.WaitForCount(3); existingControl.Respond(2, "{}");
        await existingControl.WaitForCount(4);
        Assert.Equal(1, replacement.Starts); // the app replaces a detached helper with its own child
        existing.Shutdown();
        existingControl.Respond(3, "{}");

        var quitControl = new MockControl(); var quitModel = new SettingsModel(quitControl);
        quitModel.Edit(p => p.Led = false);
        var orderlyQuit = quitModel.PrepareToQuitAsync();
        await quitControl.WaitForCount(1);
        Assert.Contains("setConfig", quitControl.Requests()[0]);
        quitControl.Respond(0, "{\"ok\":true}"); await quitControl.WaitForCount(2);
        Assert.Contains("stop", quitControl.Requests()[1]);
        quitControl.Respond(1, "{\"ok\":true}"); await orderlyQuit;
        Assert.False(quitModel.HasUnsavedChanges); // Quit commits pending settings before stopping the service
    });

    [Fact]
    public void ProfileKeysUseExecutableNamesAndDeviceSuffixes() {
        var model = new SettingsModel(new MockControl(), applicationName: id => id == "acme.exe" ? "Acme CAD" : null);
        model.AcceptStatus(StatusJson(0x046d, 0xc627));
        model.AddProfile("FreeCAD.exe");
        Assert.Equal("freecad.exe", model.Selected); Assert.Equal("FreeCAD", model.ProfileName("freecad.exe"));
        Assert.Equal("Acme CAD", model.ProfileName("acme.exe")); Assert.Equal("other", model.ProfileName("other.exe"));
        model.CustomizeForDevice();
        Assert.Equal("freecad.exe@046d:c627", model.Selected);
        Assert.Equal("FreeCAD — SpaceExplorer", model.ProfileName(model.Selected));
        model.RemoveSelectedProfile(); Assert.Equal("*", model.Selected);
        Assert.Equal("All applications", model.ProfileName("*"));
        model.Shutdown();
    }
}
