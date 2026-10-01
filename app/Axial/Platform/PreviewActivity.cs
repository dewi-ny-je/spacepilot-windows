using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Windows.Threading;

namespace Axial.App;

// The bridge's process-wide activity callback, marshalled to the UI thread.
// It wakes an idle Test scene; active frames read the preview themselves.
static unsafe class PreviewActivity {
    static Dispatcher? dispatcher;
    static int pending;
    public static event Action? Changed;
    public static void Install(Dispatcher target) {
        dispatcher = target;
        delegate* unmanaged[Stdcall]<void> callback = &OnActivity;
        NativePreview.Activity((IntPtr)(void*)callback);
    }
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    static void OnActivity() {
        try {
            if (System.Threading.Interlocked.Exchange(ref pending, 1) == 1) return;
            dispatcher?.BeginInvoke(DispatcherPriority.Input, new Action(() => {
                System.Threading.Interlocked.Exchange(ref pending, 0);
                Changed?.Invoke();
            }));
        } catch (Exception) { }
    }
}
