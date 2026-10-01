using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;

namespace Axial.App;

// Friendly names for executable profile ids, from a chosen file or a running process.
sealed class ApplicationNames {
    readonly Dictionary<string, string> known = new(StringComparer.OrdinalIgnoreCase);
    static string? Describe(string path) {
        try {
            var info = FileVersionInfo.GetVersionInfo(path);
            var name = string.IsNullOrWhiteSpace(info.FileDescription) ? info.ProductName : info.FileDescription;
            return string.IsNullOrWhiteSpace(name) ? null : name.Trim();
        } catch (FileNotFoundException) { return null; }
    }
    public void Remember(string path) {
        if (Describe(path) is { } name) known[Path.GetFileName(path).ToLowerInvariant()] = name;
    }
    public string? Find(string id) {
        if (known.TryGetValue(id, out var name)) return name;
        if (!id.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) return null;
        foreach (var process in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(id))) {
            using (process) {
                try {
                    if (process.MainModule?.FileName is { } path && Describe(path) is { } found) return known[id] = found;
                } catch (Win32Exception) { } catch (InvalidOperationException) { }
            }
        }
        return null;
    }
}
