#pragma once
#include "transport.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace sn {
// Worker threads and posted callbacks can outlive a client's FreeLibrary call.
// Pin the compatibility DLL for the process lifetime; resources still stop at
// SiTerminate/NlClose. This runs once at registration, never on input.
inline void keepCallbackCodeMapped(const void* entry) {
    HMODULE module=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(entry),&module);
}
// A reconnecting subscriber. The callback runs on the stream's own thread;
// adapters forward events to the thread that owns their client state.
class Stream {
    uint32_t flags;
    void (*callback)(void*,const Event&);
    void* context;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    bool running=false;
    Socket fd=INVALID_SOCKET;
    void run() {
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST);
        for(;;) {
            {std::unique_lock lock(mutex);if(!running)return;}
            Socket connected=openEvents(flags);
            if(connected==INVALID_SOCKET){std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(100),[&]{return !running;});continue;}
            {std::lock_guard lock(mutex);if(!running){closesocket(connected);return;}fd=connected;}
            Event e;
            while(readAll(connected,&e,sizeof(e))&&valid(e))callback(context,e);
            bool stopping;
            {std::lock_guard lock(mutex);fd=INVALID_SOCKET;stopping=!running;}
            closesocket(connected);
            if(stopping)return;
            Event reset;reset.kind=Kind::reset;reset.flags=Flags::disconnected;reset.received=now();callback(context,reset);
        }
    }
public:
    Stream(void (*cb)(void*,const Event&),void* ctx,uint32_t f=0):flags(f),callback(cb),context(ctx){}
    void start() {
        std::lock_guard lock(mutex);if(running||worker.joinable())return;
        running=true;worker=std::thread([this]{run();});
    }
    void stop() {
        {std::lock_guard lock(mutex);running=false;if(fd!=INVALID_SOCKET)shutdown(fd,SD_BOTH);}
        wake.notify_all();
        if(!worker.joinable())return;
        if(worker.get_id()==std::this_thread::get_id())worker.detach();else worker.join();
    }
    ~Stream(){stop();}
};
}
