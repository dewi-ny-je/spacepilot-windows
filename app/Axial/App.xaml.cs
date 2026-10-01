using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Windows;
using System.Windows.Threading;

namespace Axial.App;

// Owns the app's lifetime, as axial's AxialAppDelegate: one instance per user,
// a notification-area icon, a settings window created on demand, and an
// orderly service stop on Quit.
public partial class App : Application {
    const string ShowSettingsEvent = @"Local\Axial.ShowSettings";
    readonly AppInstance instance = new();
    readonly NativePreview preview = new();
    readonly ApplicationNames names = new();
    EventWaitHandle? showRequests;
    RegisteredWaitHandle? showWait;
    SettingsModel? model;
    MainWindow? window;
    TrayIcon? tray;
    DispatcherTimer? timer;

    internal bool Quitting { get; private set; }

    protected override void OnStartup(StartupEventArgs e) {
        base.OnStartup(e);
        // Open the event before claiming the lock, so a second launch that
        // signals while this one is still starting is not lost.
        showRequests = new EventWaitHandle(false, EventResetMode.AutoReset, ShowSettingsEvent);
        bool owner;
        try { owner = instance.Claim(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) {
            MessageBox.Show($"Check permissions on the Axial data folder, then try again.\n\n{error.Message}", "Axial could not secure its app session.",
                MessageBoxButton.OK, MessageBoxImage.Error);
            Quitting = true; Shutdown(1); return;
        }
        if (!owner) {
            // Let the running instance bring its window to the front.
            NativeMethods.AllowSetForegroundWindow(NativeMethods.AllowAnyProcess);
            showRequests.Set();
            Quitting = true; Shutdown(); return;
        }
        showWait = ThreadPool.RegisterWaitForSingleObject(showRequests, (_, _) => Dispatcher.BeginInvoke(new Action(ShowSettings)), null, Timeout.Infinite, false);
        DispatcherUnhandledException += (_, args) => Trace.TraceError("Axial: {0}", args.Exception);
        SessionEnding += (_, _) => { Quitting = true; model?.Shutdown(); };

        var directory = AppContext.BaseDirectory;
        var login = new LoginItem();
        try { login.Refresh(); } catch (Exception error) when (error is UnauthorizedAccessException or IOException or System.Security.SecurityException) { }
        PreviewActivity.Install(Dispatcher);
        model = new SettingsModel(new ServiceClient(), new ServiceLauncher(Path.Combine(directory, "axial-service.exe")),
            new NativeWebSetup(Path.Combine(directory, "axial-web-setup.exe")), preview, login, names.Find);
        tray = new TrayIcon(ShowSettings, Quit);
        model.Start(Path.Combine(AppInstance.DefaultDirectory, "Logs"));
        // Live preview for the visible Motion and Buttons tabs, and the button log.
        timer = new DispatcherTimer(TimeSpan.FromMilliseconds(33), DispatcherPriority.Normal, OnTick, Dispatcher);
        timer.Start();
        if (!e.Args.Contains("--background", StringComparer.OrdinalIgnoreCase)) ShowSettings();
    }

    void OnTick(object? sender, EventArgs e) {
        if (model == null || Quitting) return;
        model.RefreshPreview(window?.IsShown == true);
        model.CollectButtonLog();
    }

    internal void ShowSettings() {
        if (model == null || Quitting) return;
        window ??= new MainWindow(model, preview, names, this);
        window.Show();
        if (window.WindowState == WindowState.Minimized) window.WindowState = WindowState.Normal;
        window.Activate();
    }

    async void Quit() {
        if (model == null || Quitting) return;
        Quitting = true;
        timer?.Stop();
        window?.Hide();
        try { await model.PrepareToQuitAsync(); }
        catch (Exception error) { Trace.TraceError("Axial could not stop cleanly: {0}", error); }
        Shutdown();
    }

    protected override void OnExit(ExitEventArgs e) {
        Quitting = true;
        timer?.Stop();
        model?.Shutdown();
        tray?.Dispose();
        showWait?.Unregister(null);
        showRequests?.Dispose();
        instance.Release();
        base.OnExit(e);
    }
}
