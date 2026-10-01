#pragma once
#include "axial/platform.hpp"
#include <functional>
#include <future>
#include <mutex>
#include <vector>

namespace sn {
// A serial event loop, the Windows counterpart of the service's dispatch queue.
// One thread waits on kernel objects (HID reads, socket events, the owning
// process) and runs posted work in order. Callbacks never run concurrently.
class Loop {
    struct Source {HANDLE handle;std::function<void()> ready;};
    std::vector<Source> sources;
    std::vector<std::function<void()>> posted;
    std::mutex mutex;
    HANDLE wakeEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    DWORD thread=0;
    bool running=true;
public:
    ~Loop(){CloseHandle(wakeEvent);}
    bool current() const {return GetCurrentThreadId()==thread;}
    // Only loop-thread code adds or removes sources.
    void add(HANDLE handle,std::function<void()> ready){sources.push_back({handle,std::move(ready)});}
    void remove(HANDLE handle){std::erase_if(sources,[&](const Source& s){return s.handle==handle;});}
    size_t capacity() const {return MAXIMUM_WAIT_OBJECTS-1-sources.size();}
    void post(std::function<void()> work){{std::lock_guard lock(mutex);posted.push_back(std::move(work));}SetEvent(wakeEvent);}
    template<class F> auto sync(F work)->decltype(work()) {
        if(current()||!thread)return work();
        std::packaged_task<decltype(work())()> task(std::move(work));auto result=task.get_future();
        post([&task]{task();});return result.get();
    }
    void stop(){running=false;}
    void run() {
        thread=GetCurrentThreadId();
        std::vector<HANDLE> handles;
        while(running) {
            handles.clear();handles.push_back(wakeEvent);
            for(const auto& s:sources)handles.push_back(s.handle);
            DWORD result=WaitForMultipleObjects(DWORD(handles.size()),handles.data(),FALSE,INFINITE);
            if(result==WAIT_OBJECT_0){
                std::vector<std::function<void()>> work;
                {std::lock_guard lock(mutex);work.swap(posted);}
                for(auto& item:work){item();if(!running)return;}
                continue;
            }
            if(result>WAIT_OBJECT_0&&result<WAIT_OBJECT_0+handles.size()){
                // WaitForMultipleObjects favours low indices. Service every
                // signalled source once per pass so a busy one cannot starve others.
                for(size_t i=result-WAIT_OBJECT_0;i<handles.size()&&running;++i){
                    if(i!=result-WAIT_OBJECT_0&&WaitForSingleObject(handles[i],0)!=WAIT_OBJECT_0)continue;
                    // A handler may remove its own or other sources; look it up again.
                    for(auto& s:sources)if(s.handle==handles[i]){auto callback=s.ready;callback();break;}
                }
                continue;
            }
            if(result==WAIT_FAILED){Sleep(1);continue;}
        }
    }
};
}
