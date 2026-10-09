using System.Runtime.InteropServices;

namespace Axial;

// axial-bridge exports. The catalog is present on every host; control requests
// and the live preview exist only in the Windows bridge.
internal static partial class Native {
    const string Bridge = "axial-bridge";
    [LibraryImport(Bridge)] internal static partial uint AxialDeviceCount();
    [LibraryImport(Bridge)] internal static partial uint AxialDeviceIdentity(uint index);
    [LibraryImport(Bridge)] internal static partial IntPtr AxialDeviceName(uint identity);
    [LibraryImport(Bridge)] internal static partial IntPtr AxialDeviceButtonName(uint identity, uint slot);
    [LibraryImport(Bridge)] internal static unsafe partial uint AxialDeviceButtonSlots(uint identity, byte* slots, uint capacity);

    [LibraryImport(Bridge, StringMarshalling = StringMarshalling.Utf8)] internal static partial IntPtr AxialRequest(string request);
    [LibraryImport(Bridge)] internal static partial void AxialFreeString(IntPtr text);

    [LibraryImport(Bridge)] internal static partial void AxialPreviewStart();
    [LibraryImport(Bridge)] internal static partial void AxialPreviewStop();
    [LibraryImport(Bridge)] internal static partial void AxialPreviewActivity(IntPtr callback);
    [LibraryImport(Bridge)] internal static partial void AxialPreviewWatch([MarshalAs(UnmanagedType.U1)] bool enabled);
    [LibraryImport(Bridge)] internal static partial ulong AxialPreviewNow();
    [LibraryImport(Bridge)] internal static partial ulong AxialPreviewLostLogs();
    [LibraryImport(Bridge)] [return: MarshalAs(UnmanagedType.U1)]
    internal static unsafe partial bool AxialPreviewRead(uint device, double* axes, out uint buttons);
    [LibraryImport(Bridge)] [return: MarshalAs(UnmanagedType.U1)]
    internal static partial bool AxialPreviewPopLog(out ulong timestamp, out uint device, out uint changed, out uint buttons, out uint reason, out uint identity);
}
