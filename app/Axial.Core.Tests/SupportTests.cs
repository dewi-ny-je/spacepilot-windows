using Xunit;

namespace Axial.Tests;

public class SupportTests {
    [Fact]
    public async Task SessionLogExportBackpressureErrorsAndLifetime() {
        var directory = Path.Combine(Path.GetTempPath(), "axial-log-" + Guid.NewGuid());
        try {
            var log = new SessionLog(directory);
            Assert.True(log.Append("first\n"u8.ToArray())); Assert.True(log.Append("second\n"u8.ToArray()));
            var destination = Path.Combine(directory, "export.csv");
            await log.ExportAsync(destination);
            var expected = SessionLog.Header + "first\nsecond\n";
            Assert.Equal(expected, File.ReadAllText(destination));
            Assert.True(log.Append("third\n"u8.ToArray()));
            await log.ExportAsync(destination);
            Assert.Equal(expected + "third\n", File.ReadAllText(destination));
            log.Dispose();
            using var entered = new SemaphoreSlim(0); using var unblock = new SemaphoreSlim(0);
            var blocked = new SessionLog(destination, 8, _ => { entered.Release(); unblock.Wait(); });
            Assert.True(blocked.Append(new byte[8]));
            Assert.True(entered.Wait(2000));
            Assert.False(blocked.Append([2])); Assert.NotNull(blocked.Error);
            unblock.Release(); blocked.Flush();
            Assert.False(blocked.Append([3])); // overflow is explicit and stops recording
            var failed = new SessionLog(destination, 8, _ => throw new IOException("disk full"));
            Assert.True(failed.Append([1])); failed.Flush();
            Assert.NotNull(failed.Error); Assert.False(failed.Append([2]));
            var released = Released(directory);
            for (int i = 0; i < 5 && released.TryGetTarget(out _); i++) { GC.Collect(); GC.WaitForPendingFinalizers(); }
            Assert.False(released.TryGetTarget(out _), "Log writer retained after draining");
        } finally { Directory.Delete(directory, true); }
    }
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    static WeakReference<SessionLog> Released(string directory) {
        var temporary = new SessionLog(directory); temporary.Append([1]); temporary.Flush(); temporary.Dispose();
        return new WeakReference<SessionLog>(temporary);
    }

    [Fact]
    public void DiagnosticRatesRestartsAndBoundedHistory() {
        var history = new DiagnosticHistory();
        void Sample(double time, ulong reports, int clients = 2, ulong overflows = 0, ulong ignored = 0) =>
            history.Record(time, DateTimeOffset.FromUnixTimeMilliseconds((long)(time * 1000)), reports, clients, overflows, ignored);
        Sample(0, 100); Sample(0.5, 150);
        Assert.Single(history.Samples);
        Sample(2, 300, 3, 4, 6);
        var first = history.Samples[^1];
        Assert.Equal((100.0, 3, 2.0, 3.0), (first.ReportsPerSecond, first.Clients, first.Overflows, first.Ignored));
        Sample(3, 0, 1);
        Assert.Single(history.Samples); Assert.Equal(0, history.Samples[0].ReportsPerSecond); // restart without a spike
        for (int time = 4; time <= 250; time++) Sample(time, (ulong)(time * 100));
        Assert.Equal(120, history.Samples.Count); Assert.Equal(100, history.Samples[^1].ReportsPerSecond);
        history.Disconnect(); Assert.Empty(history.Samples);
        Sample(251, 999999);
        Assert.Equal(0, history.Samples[^1].ReportsPerSecond); // reconnection does not count historical reports
    }

    [Fact]
    public void TestNavigationIsUnboundedNeutralAndResettable() {
        var state = new TestNavigation();
        for (int i = 0; i < 1200; i++) state.Advance(1, -1, 0, 1.0 / 120);
        Assert.True(state.Pan.X > 30 && state.Pan.Y < -30, "Panning stopped at the old scene boundary");
        state.Reset();
        for (int i = 0; i < 2400; i++) state.Advance(0, 0, -1, 1.0 / 120);
        Assert.True(state.Scale < 1e-12, "Zoom hit a fixed minimum");
        for (int i = 0; i < 4800; i++) state.Advance(0, 0, 1, 1.0 / 120);
        Assert.True(state.Scale > 1e12, "Zoom hit a fixed maximum");
        var before = state.Scale;
        state.Advance(0, 0, 0, 1);
        Assert.Equal(before, state.Scale); Assert.Equal(Vector2D.Zero, state.Pan);
        state.Reset(); Assert.Equal(TestNavigation.InitialScale, state.Scale); Assert.Equal(Vector2D.Zero, state.Pan);
    }

    [Fact]
    public void MotionFilterMatchesProfileSettings() {
        var profile = new Profile(); profile.Deadzone[0] = 10; profile.Gain[1] = 2; profile.Invert[2] = true;
        double[] axes = [15, -30, 40, 100, 20, -5];
        MotionFilter.Apply(axes, profile);
        Assert.Equal([5, -60, -40, 100, 20, -5], axes);
        profile.Dominant = true; axes = [15, -30, 40, 100, 20, -5]; MotionFilter.Apply(axes, profile);
        Assert.Equal([0, 0, 0, 100, 0, 0], axes);
        profile.Dominant = false; profile.Rotation = false; axes = [15, -30, 40, 100, 20, -5]; MotionFilter.Apply(axes, profile);
        Assert.Equal([5, -60, -40, 0, 0, 0], axes);
    }

    [Fact]
    public void AppInstanceIsExclusiveReleasableAndRejectsLinks() {
        var directory = Path.Combine(Path.GetTempPath(), "axial-instance-" + Guid.NewGuid());
        try {
            using var first = new AppInstance(); using var second = new AppInstance();
            Assert.True(first.Claim(directory)); Assert.True(first.OwnsLock);
            Assert.False(second.Claim(directory)); Assert.False(second.OwnsLock);
            first.Release();
            Assert.True(second.Claim(directory)); second.Release();
            var lockPath = Path.Combine(directory, "app.lock");
            File.Delete(lockPath);
            try { File.CreateSymbolicLink(lockPath, Path.Combine(directory, "foreign")); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return; } // links need Developer Mode on Windows
            Assert.Throws<UnauthorizedAccessException>(() => first.Claim(directory));
            Assert.False(File.Exists(Path.Combine(directory, "foreign")));
        } finally { Directory.Delete(directory, true); }
    }
}
