using System.Diagnostics;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Threading.Channels;

namespace Axial;

public interface IServiceRequesting {
    Task<string> RequestAsync(string text);
}

// One ordered I/O lane. Blocking local IPC never occupies the UI thread, and
// an older save cannot overtake a newer one.
public sealed class ServiceClient : IServiceRequesting {
    readonly Channel<(string Text, TaskCompletionSource<string> Result)> queue = Channel.CreateUnbounded<(string, TaskCompletionSource<string>)>(new() { SingleReader = true });
    public ServiceClient() {
        var thread = new Thread(() => {
            while (queue.Reader.WaitToReadAsync().AsTask().GetAwaiter().GetResult())
                while (queue.Reader.TryRead(out var item)) item.Result.TrySetResult(Request(item.Text));
        }) { IsBackground = true, Name = "Axial control" };
        thread.Start();
    }
    static string Request(string text) {
        var result = Native.AxialRequest(text);
        if (result == IntPtr.Zero) return "";
        try { return Marshal.PtrToStringUTF8(result) ?? ""; }
        finally { Native.AxialFreeString(result); }
    }
    public Task<string> RequestAsync(string text) {
        var completion = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
        queue.Writer.TryWrite((text, completion));
        return completion.Task;
    }
}

public interface IServiceLaunching {
    bool IsRunning { get; }
    void Start();
    void Terminate();
}

// The app's live input preview and button-edge log, read from the bridge.
public interface IPreview {
    void Start();
    void Stop();
    ulong Now();
    ulong LostLogs();
    bool Read(uint device, double[] axes, out uint buttons);
    bool PopLog(out ulong timestamp, out uint device, out uint changed, out uint buttons, out uint reason, out uint identity);
}
public sealed class NativePreview : IPreview {
    public void Start() => Native.AxialPreviewStart();
    public void Stop() => Native.AxialPreviewStop();
    public ulong Now() => Native.AxialPreviewNow();
    public ulong LostLogs() => Native.AxialPreviewLostLogs();
    public unsafe bool Read(uint device, double[] axes, out uint buttons) {
        if (axes.Length < 6) throw new ArgumentException("Six axes are required", nameof(axes));
        fixed (double* p = axes) return Native.AxialPreviewRead(device, p, out buttons);
    }
    public bool PopLog(out ulong timestamp, out uint device, out uint changed, out uint buttons, out uint reason, out uint identity) =>
        Native.AxialPreviewPopLog(out timestamp, out device, out changed, out buttons, out reason, out identity);
    // Wakes an idle view; the callback runs on a bridge thread.
    public static void Activity(IntPtr callback) => Native.AxialPreviewActivity(callback);
    public static void Watch(bool enabled) => Native.AxialPreviewWatch(enabled);
}

public static class Clock {
    // Monotonic seconds, independent of wall-clock changes.
    public static double Seconds => Stopwatch.GetTimestamp() / (double)Stopwatch.Frequency;
}

public sealed record DiagnosticSample(DateTimeOffset Time, double ReportsPerSecond, int Clients, double Overflows, double Ignored) {
    public double ClientCount => Clients;
}
public sealed class DiagnosticHistory {
    readonly List<DiagnosticSample> samples = [];
    (double Time, ulong Reports, ulong Overflows, ulong Ignored)? previous;
    public IReadOnlyList<DiagnosticSample> Samples => samples;
    public void Disconnect() { previous = null; samples.Clear(); }
    public void Record(double time, DateTimeOffset wall, ulong reports, int clients, ulong overflows, ulong ignored) {
        if (previous is not { } old) {
            previous = (time, reports, overflows, ignored);
            samples.Add(new DiagnosticSample(wall, 0, clients, 0, 0));
            return;
        }
        double interval = time - old.Time;
        if (interval < 1) return;
        if (reports < old.Reports || overflows < old.Overflows || ignored < old.Ignored) {
            Disconnect(); Record(time, wall, reports, clients, overflows, ignored); return;
        }
        samples.Add(new DiagnosticSample(wall, (reports - old.Reports) / interval, clients, (overflows - old.Overflows) / interval, (ignored - old.Ignored) / interval));
        if (samples.Count > 120) samples.RemoveRange(0, samples.Count - 120);
        previous = (time, reports, overflows, ignored);
    }
}

// Double precision and multiplicative zoom avoid arbitrary travel/zoom limits.
// Reject only values beyond representable floating-point range.
public struct TestNavigation {
    public const double InitialScale = 2.1;
    public double Scale { get; private set; }
    public Vector2D Pan { get; private set; }
    public TestNavigation() { Scale = InitialScale; Pan = default; }
    public void Advance(double horizontal, double vertical, double zoom, double seconds) {
        double nextScale = Scale * Math.Exp(zoom * seconds * 1.8);
        if (double.IsFinite(nextScale) && nextScale > 0) Scale = nextScale;
        var next = new Vector2D(Pan.X + horizontal * Scale * seconds * 1.8, Pan.Y + vertical * Scale * seconds * 1.8);
        if (double.IsFinite(next.X) && double.IsFinite(next.Y)) Pan = next;
    }
    public void Reset() => this = new TestNavigation();
}
public readonly record struct Vector2D(double X, double Y) {
    public static readonly Vector2D Zero = default;
}

// Applies a profile's motion settings to preview input, as the service does.
public static class MotionFilter {
    public static void Apply(double[] values, Profile profile) {
        for (int i = 0; i < 6; i++) {
            double gain = (i < 3 && !profile.Translation) || (i >= 3 && !profile.Rotation) ? 0 : profile.Gain[i] * (profile.Invert[i] ? -1 : 1);
            double x = values[i];
            values[i] = Math.Sign(x) * Math.Max(0, Math.Abs(x) - profile.Deadzone[i]) * gain;
        }
        if (profile.Dominant) {
            int index = 0;
            for (int i = 1; i < 6; i++) if (Math.Abs(values[i]) > Math.Abs(values[index])) index = i;
            for (int i = 0; i < 6; i++) if (i != index) values[i] = 0;
        }
    }
    public static Quaternion Rotation(double[] axes, double seconds) {
        // Rotating the model needs the opposite sign from rotating a camera around it.
        var rotation = new Vector3((float)axes[3], (float)-axes[5], (float)axes[4]) / 350f;
        float length = rotation.Length();
        return length > 0 ? Quaternion.CreateFromAxisAngle(rotation / length, length * (float)seconds * 1.8f) : Quaternion.Identity;
    }
}
