using System;
using Microsoft.Win32;

namespace Axial.App;

// Start at login through the per-user Run key. Windows' Startup apps page can
// disable the entry separately; enabling here clears that override.
sealed class LoginItem : ILoginItem {
    const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    const string ApprovedKey = @"Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run";
    const string Name = "Axial";
    static string Command => $"\"{Environment.ProcessPath}\" --background";

    public bool Enabled {
        get {
            using var run = Registry.CurrentUser.OpenSubKey(RunKey);
            if (run?.GetValue(Name) is not string) return false;
            using var approved = Registry.CurrentUser.OpenSubKey(ApprovedKey);
            return !(approved?.GetValue(Name) is byte[] state && state.Length > 0 && (state[0] & 1) != 0);
        }
    }
    public void Set(bool enabled) {
        using (var run = Registry.CurrentUser.CreateSubKey(RunKey, true)) {
            if (enabled) run.SetValue(Name, Command, RegistryValueKind.String);
            else run.DeleteValue(Name, false);
        }
        using var approved = Registry.CurrentUser.OpenSubKey(ApprovedKey, true);
        approved?.DeleteValue(Name, false);
    }
    // Keep an enabled entry pointing at this copy after the app moves.
    public void Refresh() {
        using var run = Registry.CurrentUser.OpenSubKey(RunKey, true);
        if (run?.GetValue(Name) is string value && !string.Equals(value, Command, StringComparison.OrdinalIgnoreCase)) run.SetValue(Name, Command, RegistryValueKind.String);
    }
}
