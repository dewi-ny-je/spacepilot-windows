using System;
using System.Runtime.InteropServices;

namespace Axial.App;

// DllImport rather than LibraryImport: WPF's temporary markup-compilation
// project does not always run source generators.
internal static class NativeMethods {
    internal const int DwmUseImmersiveDarkMode = 20, DwmSystemBackdropType = 38, BackdropMica = 2;
    internal const uint AllowAnyProcess = uint.MaxValue;
    [DllImport("dwmapi.dll", ExactSpelling = true)] internal static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);
    [DllImport("user32.dll", ExactSpelling = true)] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool AllowSetForegroundWindow(uint processId);
    [DllImport("user32.dll", ExactSpelling = true)] internal static extern uint MapVirtualKeyW(uint code, uint mapType);
    [DllImport("user32.dll", ExactSpelling = true)] internal static extern unsafe int GetKeyNameTextW(int parameter, char* buffer, int size);
    [DllImport("user32.dll", ExactSpelling = true)] internal static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", ExactSpelling = true)] internal static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", ExactSpelling = true)] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool AttachThreadInput(uint thread, uint target, [MarshalAs(UnmanagedType.Bool)] bool attach);
    [DllImport("user32.dll", ExactSpelling = true)] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll", ExactSpelling = true)] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool BringWindowToTop(IntPtr window);
    [DllImport("kernel32.dll", ExactSpelling = true)] internal static extern uint GetCurrentThreadId();

    // A device button asks for the window while another application has the
    // foreground, so Windows' foreground lock would only flash the taskbar.
    // Sharing the foreground thread's input state lets the window take focus.
    internal static void BringToFront(IntPtr window) {
        var foreground = GetForegroundWindow();
        if (window == IntPtr.Zero || foreground == window) return;
        uint target = foreground == IntPtr.Zero ? 0 : GetWindowThreadProcessId(foreground, out _), current = GetCurrentThreadId();
        bool attached = target != 0 && target != current && AttachThreadInput(current, target, true);
        try { BringWindowToTop(window); SetForegroundWindow(window); }
        finally { if (attached) AttachThreadInput(current, target, false); }
    }

    internal static bool SetWindowAttribute(IntPtr window, int attribute, int value) {
        try { return DwmSetWindowAttribute(window, attribute, ref value, sizeof(int)) == 0; }
        catch (DllNotFoundException) { return false; }
        catch (EntryPointNotFoundException) { return false; }
    }
}
