using System.Runtime.InteropServices;

namespace Axial;

public sealed record ControllerButton(int Id, string Name); // Id is the persistent profile slot.

public sealed class ControllerLayout(string name, IReadOnlyList<ControllerButton> buttons, IReadOnlyList<string?> names) {
    public string Name { get; } = name;
    public IReadOnlyList<ControllerButton> Buttons { get; } = buttons;
    public IReadOnlyList<string?> Names { get; } = names;
    public string ButtonName(int slot) => (slot >= 0 && slot < Names.Count ? Names[slot] : null) ?? $"Unknown button ({slot + 1})";
}

// The native hardware catalog, converted once. Names are resolved when the UI
// consumes log entries, never in the input path.
public static class DeviceCatalog {
    static readonly Lazy<IReadOnlyDictionary<uint, ControllerLayout>> layouts = new(Load);
    public static IReadOnlyDictionary<uint, ControllerLayout> Layouts => layouts.Value;
    static unsafe IReadOnlyDictionary<uint, ControllerLayout> Load() {
        var result = new Dictionary<uint, ControllerLayout>();
        byte* slots = stackalloc byte[32];
        for (uint index = 0; index < Native.AxialDeviceCount(); index++) {
            uint identity = Native.AxialDeviceIdentity(index);
            var name = Marshal.PtrToStringUTF8(Native.AxialDeviceName(identity));
            if (name == null) continue;
            int count = (int)Native.AxialDeviceButtonSlots(identity, slots, 32);
            var names = Enumerable.Range(0, 32).Select(slot => Marshal.PtrToStringUTF8(Native.AxialDeviceButtonName(identity, (uint)slot))).ToArray();
            var buttons = new List<ControllerButton>();
            for (int i = 0; i < count; i++) buttons.Add(new ControllerButton(slots[i], names[slots[i]]!));
            result[identity] = new ControllerLayout(name, buttons, names);
        }
        return result;
    }
    public static uint? Identity(int vendor, int product) =>
        vendor is >= 0 and <= 65535 && product is >= 0 and <= 65535 ? (uint)vendor << 16 | (uint)product : null;
}

public sealed record ButtonEntry {
    public ulong Id { get; }
    public DateTimeOffset Time { get; }
    public uint Device { get; }
    public int Button { get; }
    public bool Pressed { get; }
    public bool Reset { get; }
    public string DeviceName { get; }
    public string ButtonName { get; }
    public ButtonEntry(ulong id, DateTimeOffset time, uint device, int button, bool pressed, bool reset, uint identity) {
        Id = id; Time = time; Device = device; Button = button; Pressed = pressed; Reset = reset;
        var layout = DeviceCatalog.Layouts.GetValueOrDefault(identity);
        DeviceName = layout?.Name ?? (identity == 0 ? "Unknown controller" : $"USB controller {identity >> 16:x4}:{identity & 0xffff:x4}");
        ButtonName = layout?.ButtonName(button - 1) ?? $"Unknown button ({button})";
    }
    public string State => Pressed ? "Pressed" : Reset ? "Released · reset" : "Released";
    public string CsvRow {
        get {
            static string Quoted(string text) => "\"" + text.Replace("\"", "\"\"") + "\"";
            var time = Time.UtcDateTime.ToString("yyyy-MM-dd'T'HH:mm:ss.fff'Z'", System.Globalization.CultureInfo.InvariantCulture);
            return $"{time},{Device},{Button},{(Pressed ? "pressed" : "released")},{(Reset ? "reset" : "device")},{Quoted(DeviceName)},{Quoted(ButtonName)}\n";
        }
    }
}
