using System.Text.Json;
using System.Text.Json.Serialization;

namespace Axial;

// The service's settings document. Field names and defaults match the service
// parser; absent values keep their defaults, as in the macOS app.
public sealed record ButtonAction {
    public int? KeyCode { get; init; }
    public ulong? Modifiers { get; init; }
    public string? Command { get; init; }
    public string? Action { get; init; }
    public string? Label { get; init; }
    [JsonIgnore] public bool IsEmpty => KeyCode == null && Command == null && Action == null;
}

public sealed class Profile : IJsonOnDeserialized, IEquatable<Profile> {
    public const int ButtonSlots = 32;
    public double[] Gain { get; set; } = [1, 1, 1, 1, 1, 1];
    public double[] Deadzone { get; set; } = [0, 0, 0, 0, 0, 0];
    public bool[] Invert { get; set; } = new bool[6];
    public bool Dominant { get; set; }
    public bool Translation { get; set; } = true;
    public bool Rotation { get; set; } = true;
    public bool Orbit { get; set; } = true;
    public bool Led { get; set; } = true;
    public ButtonAction[] Buttons { get; set; } = Enumerable.Repeat(new ButtonAction(), ButtonSlots).ToArray();

    public void OnDeserialized() {
        if (Buttons.Length < ButtonSlots) Buttons = Buttons.Concat(Enumerable.Repeat(new ButtonAction(), ButtonSlots - Buttons.Length)).ToArray();
        if (Gain.Length != 6 || Deadzone.Length != 6 || Invert.Length != 6 || Buttons.Length != ButtonSlots)
            throw new JsonException("Invalid profile dimensions");
    }
    public Profile Clone() => new() {
        Gain = (double[])Gain.Clone(), Deadzone = (double[])Deadzone.Clone(), Invert = (bool[])Invert.Clone(),
        Dominant = Dominant, Translation = Translation, Rotation = Rotation, Orbit = Orbit, Led = Led,
        Buttons = (ButtonAction[])Buttons.Clone()
    };
    public bool Equals(Profile? other) => other is not null && Gain.SequenceEqual(other.Gain) && Deadzone.SequenceEqual(other.Deadzone)
        && Invert.SequenceEqual(other.Invert) && Dominant == other.Dominant && Translation == other.Translation
        && Rotation == other.Rotation && Orbit == other.Orbit && Led == other.Led && Buttons.SequenceEqual(other.Buttons);
    public override bool Equals(object? obj) => Equals(obj as Profile);
    public override int GetHashCode() => HashCode.Combine(Dominant, Translation, Rotation, Orbit, Led, Gain[0]);
}

public sealed class WebConfiguration {
    public bool Enabled { get; set; } = true;
}
public sealed record WebStatus(bool Enabled, bool Listening, int Connections, string Error);

public sealed class Configuration {
    public int Version { get; set; } = 1;
    public Dictionary<string, Profile> Profiles { get; set; } = new() { ["*"] = new Profile() };
    public WebConfiguration? Web { get; set; }
}

public sealed record Device {
    public int Id { get; init; }
    public int Vendor { get; init; }
    public int Product { get; init; }
    public string Name { get; init; } = "";
    public int[] Axes { get; init; } = new int[6];
    public uint Buttons { get; init; }
    public bool? LedSupported { get; init; }
    public int? LedState { get; init; }
    public uint? LedError { get; init; }
    [JsonIgnore] public ControllerLayout? Layout => DeviceCatalog.Identity(Vendor, Product) is uint identity ? DeviceCatalog.Layouts.GetValueOrDefault(identity) : null;
    [JsonIgnore] public string DisplayName => Layout?.Name ?? Name;
}

public sealed record Status {
    public WebStatus? Web { get; init; }
    public required Device[] Devices { get; init; }
    public int Clients { get; init; }
    public ulong Reports { get; init; }
    public ulong Overflows { get; init; }
    public ulong Rejected { get; init; }
    public string ForegroundApp { get; init; } = "";
    public bool Accessibility { get; init; }
    public bool Mock { get; init; }
    public bool? Elevated { get; init; }
    [JsonIgnore] public int ConnectedClients => Clients + (Web?.Connections ?? 0);
}

public sealed record AppCommand(string Id, string Label);

public static class Json {
    public static readonly JsonSerializerOptions Options = new() {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        PropertyNameCaseInsensitive = false,
        NumberHandling = JsonNumberHandling.Strict,
    };
    public static T? TryDecode<T>(string text) where T : class {
        if (string.IsNullOrEmpty(text)) return null;
        try { return JsonSerializer.Deserialize<T>(text, Options); }
        catch (JsonException) { return null; }
        catch (NotSupportedException) { return null; }
        catch (InvalidOperationException) { return null; }
    }
    public static string Encode<T>(T value) => JsonSerializer.Serialize(value, Options);
}
