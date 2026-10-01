using System.Collections.Concurrent;
using System.Runtime.CompilerServices;

namespace Axial.Tests;

// Runs async test bodies on one thread, as the app runs its model on the UI thread.
sealed class SingleThreadContext : SynchronizationContext {
    readonly BlockingCollection<(SendOrPostCallback Callback, object? State)> queue = new();
    public override void Post(SendOrPostCallback d, object? state) {
        try { queue.Add((d, state)); } catch (InvalidOperationException) { }
    }
    public override void Send(SendOrPostCallback d, object? state) => throw new NotSupportedException();
    public static void Run(Func<Task> body) {
        var previous = Current; var context = new SingleThreadContext();
        SetSynchronizationContext(context);
        try {
            var task = body();
            task.ContinueWith(_ => context.queue.CompleteAdding(), TaskScheduler.Default);
            foreach (var (callback, state) in context.queue.GetConsumingEnumerable()) callback(state);
            task.GetAwaiter().GetResult();
        } finally { SetSynchronizationContext(previous); }
    }
}

sealed class MockControl : IServiceRequesting {
    readonly object gate = new();
    readonly List<string> texts = [];
    readonly Dictionary<int, TaskCompletionSource<string>> pending = [];
    public Task<string> RequestAsync(string text) {
        var completion = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (gate) { pending[texts.Count] = completion; texts.Add(text); }
        return completion.Task;
    }
    public List<string> Requests() { lock (gate) return [.. texts]; }
    public void Respond(int id, string json) {
        TaskCompletionSource<string>? completion;
        lock (gate) { pending.Remove(id, out completion); }
        completion?.SetResult(json);
    }
    public async Task WaitForCount(int count) {
        for (int i = 0; i < 3000; i++) {
            lock (gate) if (texts.Count >= count) return;
            await Task.Delay(1);
        }
        throw new TimeoutException($"Timed out waiting for request {count}, got {string.Join(" | ", Requests())}");
    }
}
sealed class ResponsiveControl : IServiceRequesting {
    int statusRequests;
    public int StatusRequests => Volatile.Read(ref statusRequests);
    public Task<string> RequestAsync(string text) {
        if (text.Contains("status")) {
            Interlocked.Increment(ref statusRequests);
            return Task.FromResult("{\"devices\":[],\"clients\":1,\"reports\":0,\"overflows\":0,\"rejected\":0,\"foregroundApp\":\"\",\"accessibility\":true,\"mock\":true}");
        }
        if (text.Contains("getConfig")) return Task.FromResult("{\"version\":1,\"profiles\":{\"*\":{}}}");
        return Task.FromResult("{}");
    }
}
sealed class MockLauncher : IServiceLaunching {
    public bool IsRunning { get; set; }
    public int Starts, Terminations;
    public void Start() { Starts++; IsRunning = true; }
    public void Terminate() { Terminations++; IsRunning = false; }
}
sealed class FakeWebSetup : IWebSetupOperations {
    public WebSetupSnapshot Snapshot = new(false, false);
    public bool Cancelled, InstallFails;
    public int Inspections, Generations, Approvals;
    public Task<WebSetupSnapshot> InspectAsync() { Inspections++; return Task.FromResult(Snapshot); }
    public Task InstallAsync() {
        if (Cancelled && !Snapshot.Trusted) throw WebSetupException.Cancel();
        if (InstallFails) throw new WebSetupException("Test repair failure");
        if (!Snapshot.CredentialsReady) { Generations++; Snapshot = Snapshot with { CredentialsReady = true, Trusted = false }; }
        if (!Snapshot.Trusted) { Approvals++; Snapshot = Snapshot with { Trusted = true }; }
        return Task.CompletedTask;
    }
}
