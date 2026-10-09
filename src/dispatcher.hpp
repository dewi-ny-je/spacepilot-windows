#pragma once
#include "axial/platform.hpp"
#include <dwmapi.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace sn {
// Runs work on the thread that owns a client session, the Windows counterpart
// of a serial dispatch queue. A message-only window receives posted work, so
// the client's own message loop executes Navlib callbacks on its UI thread.
// Multithreaded sessions get a private thread with its own message loop.
class Dispatcher {
    enum : UINT {workMessage=WM_APP+0x4158,syncMessage,frameMessage};
    HWND window=nullptr;
    DWORD thread=0;
    std::mutex mutex;
    std::deque<std::function<void()>> queue;
    bool signalled=false,destroyed=false,ownsThread=false;
    std::atomic<bool> framePending{false};
    std::weak_ptr<void> owner;
    std::function<void()> onFrame,onTimer;
    static HINSTANCE module() {
        HMODULE m=nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&Dispatcher::module),&m);
        return m;
    }
    static const wchar_t* windowClass() {
        static const wchar_t* name=[] {
            WNDCLASSEXW c{};c.cbSize=sizeof(c);c.lpfnWndProc=procedure;c.hInstance=module();c.lpszClassName=L"AxialNavlibDispatcher";
            RegisterClassExW(&c);return L"AxialNavlibDispatcher";
        }();
        return name;
    }
    static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam) {
        auto* d=reinterpret_cast<Dispatcher*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(!d||(message!=workMessage&&message!=syncMessage&&message!=frameMessage&&message!=WM_TIMER))return DefWindowProcW(hwnd,message,wParam,lParam);
        // Hold the session while its callbacks run; a callback may close it.
        auto keep=d->owner.lock();if(!keep)return 0;
        if(message==workMessage){
            std::deque<std::function<void()>> work;
            {std::lock_guard lock(d->mutex);work.swap(d->queue);d->signalled=false;}
            for(auto& item:work)item();
        } else if(message==syncMessage)(*reinterpret_cast<std::function<void()>*>(lParam))();
        else if(message==frameMessage){d->framePending=false;if(d->onFrame)d->onFrame();}
        else if(d->onTimer)d->onTimer();
        return 0;
    }
    bool createWindow() {
        window=CreateWindowExW(0,windowClass(),L"",0,0,0,0,0,HWND_MESSAGE,nullptr,module(),nullptr);
        if(!window)return false;
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
        thread=GetCurrentThreadId();return true;
    }
public:
    // ownThread selects a private thread; otherwise the calling thread owns it.
    bool create(std::weak_ptr<void> keepAlive,bool ownThread,std::function<void()> frame,std::function<void()> timer) {
        owner=std::move(keepAlive);onFrame=std::move(frame);onTimer=std::move(timer);
        if(!ownThread)return createWindow();
        ownsThread=true;
        std::mutex ready;std::condition_variable created;bool done=false,ok=false;
        std::thread([&] {
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
            bool result=createWindow();
            {std::lock_guard lock(ready);ok=result;done=true;}created.notify_all();
            if(!result)return;
            MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
        }).detach();
        std::unique_lock lock(ready);created.wait(lock,[&]{return done;});
        return ok;
    }
    bool current() const {return GetCurrentThreadId()==thread;}
    void post(std::function<void()> work) {
        bool notify=false;
        {std::lock_guard lock(mutex);if(destroyed||!window)return;queue.push_back(std::move(work));notify=!signalled;signalled=true;}
        if(notify)PostMessageW(window,workMessage,0,0);
    }
    // Returns false when the session's thread is gone and the work did not run.
    bool sync(const std::function<void()>& work) {
        if(current()){work();return true;}
        HWND target;{std::lock_guard lock(mutex);if(destroyed||!window)return false;target=window;}
        bool ran=false;std::function<void()> wrapped=[&]{ran=true;work();};
        SendMessageW(target,syncMessage,0,reinterpret_cast<LPARAM>(&wrapped));
        return ran;
    }
    // Frames coalesce: a busy client thread receives at most one pending tick.
    void requestFrame() {
        if(framePending.exchange(true))return;
        HWND target;{std::lock_guard lock(mutex);target=destroyed?nullptr:window;}
        if(!target||!PostMessageW(target,frameMessage,0,0))framePending=false;
    }
    void setTimer(UINT milliseconds){if(window)SetTimer(window,1,milliseconds,nullptr);}
    // Called on the owning thread.
    void destroy() {
        std::deque<std::function<void()>> dropped;
        HWND old;
        {std::lock_guard lock(mutex);if(destroyed)return;destroyed=true;old=window;dropped.swap(queue);}
        if(old){KillTimer(old,1);SetWindowLongPtrW(old,GWLP_USERDATA,0);DestroyWindow(old);}
        // A private thread ends its message loop once the current message returns.
        if(ownsThread)PostQuitMessage(0);
    }
};

// Display-paced ticks while navigation is in motion. DwmFlush returns once per
// composition (the monitor's refresh); a high-resolution 120 Hz timer is the
// fallback when composition is unavailable.
class FrameClock {
    std::thread worker;
    HANDLE runEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic<bool> quit{false};
    std::function<void()> tick;
    void run() {
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_TIME_CRITICAL);
        HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002/*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/,TIMER_ALL_ACCESS);
        if(!timer)timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
        LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);
        bool composition=true;int unpaced=0;
        while(!quit) {
            WaitForSingleObject(runEvent,INFINITE);if(quit)break;
            bool paced=false;
            if(composition){
                LARGE_INTEGER start,end;QueryPerformanceCounter(&start);
                if(SUCCEEDED(DwmFlush())){
                    QueryPerformanceCounter(&end);
                    // A flush that never blocks is not tied to the display.
                    paced=(end.QuadPart-start.QuadPart)*1000>=frequency.QuadPart;
                    unpaced=paced?0:unpaced+1;if(unpaced>=8)composition=false;
                } else composition=false;
            }
            if(!paced&&timer){LARGE_INTEGER due;due.QuadPart=-83333;SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE);WaitForSingleObject(timer,20);}
            else if(!paced)Sleep(8);
            if(!quit&&WaitForSingleObject(runEvent,0)==WAIT_OBJECT_0)tick();
        }
        if(timer)CloseHandle(timer);
    }
public:
    explicit FrameClock(std::function<void()> t):tick(std::move(t)){worker=std::thread([this]{run();});}
    void running(bool enabled){if(enabled)SetEvent(runEvent);else ResetEvent(runEvent);}
    bool isRunning() const {return WaitForSingleObject(runEvent,0)==WAIT_OBJECT_0;}
    void stop(){quit=true;SetEvent(runEvent);if(worker.joinable()){if(worker.get_id()==std::this_thread::get_id())worker.detach();else worker.join();}}
    ~FrameClock(){stop();CloseHandle(runEvent);}
};

// Work that must not block a client thread (service requests).
inline void background(std::function<void()> work) {
    auto* item=new std::function<void()>(std::move(work));
    if(!TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE,void* context){
        std::unique_ptr<std::function<void()>> w(static_cast<std::function<void()>*>(context));(*w)();},item,nullptr)){
        std::thread([item]{std::unique_ptr<std::function<void()>> w(item);(*w)();}).detach();
    }
}
}
