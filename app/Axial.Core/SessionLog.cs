namespace Axial;

// Disk work is serialized, with a bounded backlog and explicit failure state.
// The full-session export copies the file rather than loading the CSV into memory.
public sealed class SessionLog : IDisposable {
    public const string Header = "time,device,button,state,reason,device_name,button_name\n";
    public string Path { get; }
    readonly object gate = new();
    readonly Action<byte[]> write;
    readonly Action synchronize;
    readonly int maxPendingBytes;
    readonly FileStream? file;
    int pendingBytes;
    string? failure;
    Task tail = Task.CompletedTask;

    public string? Error { get { lock (gate) return failure; } }

    public SessionLog(string directory, int maxPendingBytes = 4 * 1024 * 1024) {
        Directory.CreateDirectory(directory);
        Path = System.IO.Path.Combine(directory, $"buttons-{Guid.NewGuid()}.csv");
        var stream = new FileStream(Path, FileMode.CreateNew, FileAccess.Write, FileShare.Read);
        var header = System.Text.Encoding.UTF8.GetBytes(Header);
        stream.Write(header);stream.Flush();
        file = stream;
        write = data => stream.Write(data);
        synchronize = () => stream.Flush(true);
        this.maxPendingBytes = maxPendingBytes;
    }
    // Injectable sink for blocked-disk and failure regression tests.
    public SessionLog(string path, int maxPendingBytes, Action<byte[]> write, Action? synchronize = null) {
        Path = path; this.maxPendingBytes = maxPendingBytes; this.write = write; this.synchronize = synchronize ?? (() => { });
    }
    public bool Append(byte[] data) {
        lock (gate) {
            if (failure != null) return false;
            if (data.Length > maxPendingBytes - pendingBytes) {
                failure = "Button log disk backlog is full; recording stopped. Visible events remain available.";
                return false;
            }
            pendingBytes += data.Length;
            tail = tail.ContinueWith(_ => {
                try { write(data); } catch (Exception error) { Record(error); }
                lock (gate) pendingBytes -= data.Length;
            }, CancellationToken.None, TaskContinuationOptions.None, TaskScheduler.Default);
        }
        return true;
    }
    void Record(Exception error) {
        lock (gate) failure ??= $"Could not write button log: {error.Message}";
    }
    Task Enqueue(Action work) {
        lock (gate) {
            tail = tail.ContinueWith(_ => work(), CancellationToken.None, TaskContinuationOptions.None, TaskScheduler.Default);
            return tail;
        }
    }
    public void Flush() {
        try { Enqueue(() => { try { synchronize(); } catch (Exception error) { Record(error); } }).Wait(); }
        catch (AggregateException) { }
    }
    public Task ExportAsync(string destination) => Enqueue(() => {
        var temporary = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(destination))!, $".axial-export-{Guid.NewGuid()}");
        try {
            synchronize();
            File.Copy(Path, temporary);
            File.Move(temporary, destination, overwrite: true);
        } finally { try { File.Delete(temporary); } catch (IOException) { } }
    });
    public void Dispose() { Flush(); file?.Dispose(); }
}
