using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;

namespace Axial.App;

// The settings app owns the input service: login starts the app, and quitting
// the app stops the service (axial's ServiceLifecycle). The service also exits
// by itself when this process ends, so a crash never leaves a headless driver.
sealed class ServiceLauncher(string path) : IServiceLaunching {
    Process? process;
    public bool IsRunning {
        get {
            try { return process is { HasExited: false }; }
            catch (InvalidOperationException) { return false; }
            catch (Win32Exception) { return false; }
        }
    }
    public void Start() {
        if (IsRunning) return;
        if (!File.Exists(path)) throw new FileNotFoundException("axial-service.exe is missing from the Axial folder.", path);
        var start = new ProcessStartInfo(path) { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = Path.GetDirectoryName(path) ?? "" };
        start.ArgumentList.Add("--app-owned");
        process?.Dispose();
        process = Process.Start(start) ?? throw new InvalidOperationException("Windows did not start axial-service.exe.");
    }
    // The model has already asked the service to stop; it releases HID, held
    // keys and LEDs on its own. Kill only a helper that does not exit in time.
    public void Terminate() {
        if (process is not { } child || !IsRunning) return;
        _ = Task.Run(() => {
            try { if (!child.WaitForExit(2500)) child.Kill(); }
            catch (InvalidOperationException) { }
            catch (Win32Exception) { }
        });
    }
}
