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

    internal static bool SetWindowAttribute(IntPtr window, int attribute, int value) {
        try { return DwmSetWindowAttribute(window, attribute, ref value, sizeof(int)) == 0; }
        catch (DllNotFoundException) { return false; }
        catch (EntryPointNotFoundException) { return false; }
    }
}
