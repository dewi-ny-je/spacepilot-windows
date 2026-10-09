using System;
using System.IO;
using Forms = System.Windows.Forms;

namespace Axial.App;

// The notification-area icon, axial's menu-bar status item.
sealed class TrayIcon : IDisposable {
    readonly Forms.NotifyIcon icon;
    readonly System.Drawing.Icon image;
    public TrayIcon(Action open, Action quit) {
        var menu = new Forms.ContextMenuStrip();
        menu.Items.Add("Open Settings…", null, (_, _) => open());
        menu.Items.Add(new Forms.ToolStripSeparator());
        menu.Items.Add("Quit Axial", null, (_, _) => quit());
        image = LoadIcon();
        icon = new Forms.NotifyIcon { Icon = image, Text = "Axial", ContextMenuStrip = menu, Visible = true };
        icon.MouseClick += (_, e) => { if (e.Button == Forms.MouseButtons.Left) open(); };
    }
    static System.Drawing.Icon LoadIcon() {
        var path = Path.Combine(AppContext.BaseDirectory, "Assets", "Axial.ico");
        try { if (File.Exists(path)) return new System.Drawing.Icon(path, Forms.SystemInformation.SmallIconSize); }
        catch (ArgumentException) { }
        catch (IOException) { }
        try { if (Environment.ProcessPath is { } exe && System.Drawing.Icon.ExtractAssociatedIcon(exe) is { } associated) return associated; }
        catch (ArgumentException) { }
        return (System.Drawing.Icon)System.Drawing.SystemIcons.Application.Clone();
    }
    public void Dispose() {
        icon.Visible = false;
        icon.ContextMenuStrip?.Dispose();
        icon.Dispose(); image.Dispose();
    }
}
