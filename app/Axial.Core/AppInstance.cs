namespace Axial;

// One settings app per user. The lock file is held open without sharing, which
// also covers explicit launches of different installed copies. A second launch
// only asks the first to show its window; it never starts the input service.
public sealed class AppInstance : IDisposable {
    FileStream? lockFile;
    public bool OwnsLock => lockFile != null;

    public static string DefaultDirectory {
        get {
            var custom = Environment.GetEnvironmentVariable("AXIAL_DATA");
            return string.IsNullOrEmpty(custom) ? System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Axial") : custom;
        }
    }
    static bool IsLink(string path) {
        FileSystemInfo info = Directory.Exists(path) ? new DirectoryInfo(path) : new FileInfo(path);
        return info.LinkTarget != null || (info.Exists && (info.Attributes & FileAttributes.ReparsePoint) != 0);
    }
    /// Returns false when another instance holds the lock.
    public bool Claim(string? directory = null) {
        if (OwnsLock) return true;
        directory ??= DefaultDirectory;
        Directory.CreateDirectory(directory);
        if (IsLink(directory)) throw new UnauthorizedAccessException("The Axial data directory is a link.");
        var path = System.IO.Path.Combine(directory, "app.lock");
        // A planted link could redirect the lock to a file owned by someone else.
        if (IsLink(path)) throw new UnauthorizedAccessException("The app lock is a link.");
        try {
            lockFile = new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None, 1, FileOptions.None);
        } catch (UnauthorizedAccessException) { throw; }
        catch (DirectoryNotFoundException) { throw; }
        catch (IOException) { return false; }
        if (IsLink(path)) { Release(); throw new UnauthorizedAccessException("The app lock is a link."); }
        return true;
    }
    public void Release() { lockFile?.Dispose(); lockFile = null; }
    public void Dispose() => Release();
}
