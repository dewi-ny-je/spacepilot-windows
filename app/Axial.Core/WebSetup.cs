using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Text.Json;

namespace Axial;

public sealed record WebSetupSnapshot(bool CredentialsReady, bool Trusted) {
    public bool Ready => CredentialsReady && Trusted;
}

public sealed class WebSetupException(string message, bool cancelled = false) : Exception(message) {
    public bool Cancelled { get; } = cancelled;
    public static WebSetupException Cancel() => new("Setup was cancelled. You can approve it when you are ready.", true);
}

public interface IWebSetupOperations {
    // Reads state only; never prompts.
    Task<WebSetupSnapshot> InspectAsync();
    // Creates or renews credentials and asks Windows to trust the local CA.
    Task InstallAsync();
}

// The bundled setup tool. Credentials live in the user's profile and the CA is
// trusted only in the user's root store, so no administrator prompt is needed;
// Windows itself asks the user to confirm the new root certificate.
public sealed class NativeWebSetup(string helper) : IWebSetupOperations {
    async Task<string> Run(string argument) {
        var start = new ProcessStartInfo(helper, argument) { CreateNoWindow = true, UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
        using var process = Process.Start(start) ?? throw new WebSetupException("Could not start the web setup tool.");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        if (process.ExitCode == 3) throw WebSetupException.Cancel();
        if (process.ExitCode != 0) {
            Trace.TraceError("Axial web setup failed: {0}", await error);
            throw new WebSetupException("Windows could not complete web setup. Please retry and approve the certificate prompt.");
        }
        return await output;
    }
    public async Task<WebSetupSnapshot> InspectAsync() {
        var text = await Run("--check");
        using var document = JsonDocument.Parse(text);
        return new WebSetupSnapshot(document.RootElement.GetProperty("credentialsReady").GetBoolean(), document.RootElement.GetProperty("trusted").GetBoolean());
    }
    public Task InstallAsync() => Run("--install");
}

public sealed class WebSetupCoordinator(IWebSetupOperations operations) : INotifyPropertyChanged {
    bool busy, ready;
    string message = "Checking web navigation setup…";
    DateTimeOffset lastCheck = DateTimeOffset.MinValue;
    public event PropertyChangedEventHandler? PropertyChanged;
    public bool Busy { get => busy; private set => Set(ref busy, value); }
    public bool Ready { get => ready; private set => Set(ref ready, value); }
    public string Message { get => message; private set => Set(ref message, value); }
    void Set<T>(ref T field, T value, [CallerMemberName] string? name = null) {
        if (EqualityComparer<T>.Default.Equals(field, value)) return;
        field = value; PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }

    public async Task RefreshAsync(bool enabled, bool listening, string listenerError, Func<Task> retry, bool force = false) {
        if (Busy) return;
        if (!enabled) { Ready = false; lastCheck = DateTimeOffset.MinValue; Message = "Web navigation is disabled."; return; }
        if (!force && DateTimeOffset.UtcNow - lastCheck < TimeSpan.FromSeconds(60)) return;
        Busy = true; lastCheck = DateTimeOffset.UtcNow;
        try {
            var snapshot = await operations.InspectAsync();
            if (!snapshot.CredentialsReady) { Ready = false; Message = "Web navigation needs setup or certificate renewal."; return; }
            if (!snapshot.Trusted) { Ready = false; Message = "Local web certificate needs your approval."; return; }
            bool wasReady = Ready; Ready = true;
            if (!wasReady || !listening) await retry();
            Message = listening ? "Web navigation is ready." : string.IsNullOrEmpty(listenerError) ? "Starting web navigation…" : listenerError;
        } catch (Exception) {
            Ready = false; Message = "Could not check web setup. Retry from the installed Axial app.";
        } finally { Busy = false; }
    }

    public async Task SetUpAsync(Func<Task> retry, Action? restoreWindow = null) {
        if (Busy) return;
        Busy = true; Ready = false; Message = "Waiting for Windows approval…";
        try {
            var snapshot = await operations.InspectAsync();
            if (!snapshot.Ready) { await operations.InstallAsync(); restoreWindow?.Invoke(); }
            snapshot = await operations.InspectAsync();
            if (!snapshot.CredentialsReady) throw new WebSetupException("Local web setup did not finish. Retry to repair it.");
            if (!snapshot.Trusted) throw new WebSetupException("The certificate still needs approval.");
            await retry(); Ready = true; Message = "Web navigation setup is complete.";
        } catch (WebSetupException error) {
            Message = error.Message;
        } catch (Exception error) {
            Message = error.Message;
        } finally { Busy = false; lastCheck = DateTimeOffset.UtcNow; restoreWindow?.Invoke(); }
    }
}
