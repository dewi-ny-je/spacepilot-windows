using System;
using System.Text;

namespace Axial.App;

// Shortcut encoding shared with the service: Windows virtual-key codes and
// modifier bits Shift=1, Ctrl=2, Alt=4, Win=8.
static class KeyNames {
    public const ulong Shift = 1, Control = 2, Alt = 4, Windows = 8;
    static bool Extended(int key) => key is 0x2D or 0x2E or 0x24 or 0x23 or 0x21 or 0x22 or 0x25 or 0x26 or 0x27 or 0x28
        or 0x90 or 0x6F or 0xA3 or 0xA5 or 0x5B or 0x5C or 0x5D or 0x2C;
    public static unsafe string Name(int key) {
        uint scan = NativeMethods.MapVirtualKeyW((uint)key, 0);
        if (scan != 0) {
            char* buffer = stackalloc char[64];
            int length = NativeMethods.GetKeyNameTextW((int)(scan << 16) | (Extended(key) ? 1 << 24 : 0), buffer, 64);
            if (length > 0) {
                var name = new string(buffer, 0, length);
                return name.Length == 1 ? name.ToUpperInvariant() : name;
            }
        }
        return $"Key {key}";
    }
    public static string Label(int key, ulong modifiers) {
        var text = new StringBuilder();
        if ((modifiers & Windows) != 0) text.Append("Win+");
        if ((modifiers & Control) != 0) text.Append("Ctrl+");
        if ((modifiers & Alt) != 0) text.Append("Alt+");
        if ((modifiers & Shift) != 0) text.Append("Shift+");
        return text.Append(Name(key)).ToString();
    }
}
