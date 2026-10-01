// axial-bridge.dll: the settings app's native helper. Control requests, the
// live preview buffer read by the Test tab, and the device catalog.
#include "axial/transport.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#define AXIAL_EXPORT extern "C" __declspec(dllexport)
AXIAL_EXPORT char* AxialRequest(const char* request) {
    if(!request)return nullptr;auto result=sn::request(request);
    return _strdup(result.c_str());
}
AXIAL_EXPORT void AxialFreeString(char* text){free(text);}

namespace {
struct PreviewSlot {
    std::atomic<uint64_t> version{0},timestamp{0};
    std::atomic<uint32_t> device{0},buttons{0},identity{0};
    std::array<std::atomic<int16_t>,6> axes{};
};
struct ButtonLog {uint64_t timestamp=0;uint32_t device=0,changed=0,buttons=0,reason=0,identity=0;};
using ActivityCallback=void(__stdcall*)();
struct Preview {
    std::atomic<ActivityCallback> activityCallback{nullptr};
    std::atomic<unsigned> activityObservers{0};
    // Producer notifications coalesce: the callback runs on the notifier
    // thread at most once until the event is consumed.
    HANDLE activity=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    std::thread notifier;
    std::array<PreviewSlot,16> slots;
    std::array<ButtonLog,4096> log;
    std::atomic<size_t> read{0},write{0};
    std::atomic<uint64_t> lost{0};
    std::atomic<bool> running{false},alive{true};
    std::thread worker;
    std::mutex lifecycle,startStop;
    sn::Socket fd=INVALID_SOCKET;
    void changed(){if(activityObservers.load())SetEvent(activity);}
    void append(uint64_t time,uint32_t device,uint32_t changed,uint32_t buttons,uint32_t reason,uint32_t identity){
        if(!changed)return;size_t w=write.load(std::memory_order_relaxed);
        if(w-read.load(std::memory_order_acquire)==log.size()){++lost;return;}
        log[w%log.size()]={time,device,changed,buttons,reason,identity};write.store(w+1,std::memory_order_release);
    }
    void clear(uint32_t device=0,uint32_t reason=uint32_t(sn::Kind::reset)){
        for(auto& slot:slots)if(slot.device.load()&&(!device||slot.device.load()==device)){
            if(reason==uint32_t(sn::Kind::removed))append(sn::now(),slot.device.load(),slot.buttons.load(),0,reason,slot.identity.load());
            slot.version.fetch_add(1,std::memory_order_acq_rel);for(auto& axis:slot.axes)axis=0;slot.timestamp=0;
            if(reason==uint32_t(sn::Kind::removed)){slot.buttons=0;slot.device=0;slot.identity=0;}
            slot.version.fetch_add(1,std::memory_order_release);
        }
        changed();
    }
    void receive(const sn::Event& event){
        if(event.kind==sn::Kind::reset||event.kind==sn::Kind::removed){clear(event.device,uint32_t(event.kind));return;}
        if(!event.device)return;
        PreviewSlot* selected=nullptr;
        for(auto& slot:slots)if(slot.device.load()==event.device){selected=&slot;break;}
        if(!selected)for(auto& slot:slots)if(!slot.device.load()){selected=&slot;break;}
        if(!selected)return;auto& slot=*selected;
        uint32_t identity=uint32_t(event.vendor)<<16|event.product;
        if(!identity)identity=slot.identity.load();
        if(event.kind==sn::Kind::buttons)append(event.received,event.device,slot.buttons.load()^event.buttons,event.buttons,uint32_t(event.kind),identity);
        bool different=false;for(int i=0;i<6;++i)different|=slot.axes[i].load()!=event.axes[i];
        bool wake=event.kind==sn::Kind::motion&&(different||!slot.timestamp.load()||event.received-slot.timestamp.load()>250000000);
        slot.version.fetch_add(1,std::memory_order_acq_rel);slot.device=event.device;slot.buttons=event.buttons;slot.identity=identity;
        if(event.kind==sn::Kind::motion){for(int i=0;i<6;++i)slot.axes[i]=event.axes[i];slot.timestamp=event.received;}
        slot.version.fetch_add(1,std::memory_order_release);
        if(wake||event.kind!=sn::Kind::motion)changed();
    }
    void start(){
        std::lock_guard call(startStop);
        if(!notifier.joinable())notifier=std::thread([this]{
            while(WaitForSingleObject(activity,INFINITE)==WAIT_OBJECT_0&&alive.load())
                if(activityObservers.load())if(auto callback=activityCallback.load())callback();
        });
        if(running.exchange(true))return;
        worker=std::thread([this]{SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
            while(running.load()){
                sn::Socket connected=sn::openEvents(sn::Flags::monitor);
                if(connected==INVALID_SOCKET){for(int i=0;i<10&&running;++i)std::this_thread::sleep_for(std::chrono::milliseconds(100));continue;}
                {std::lock_guard lock(lifecycle);fd=connected;if(!running)shutdown(fd,SD_BOTH);}
                sn::Event event;
                while(running&&sn::readAll(connected,&event,sizeof(event))&&sn::valid(event))receive(event);
                {std::lock_guard lock(lifecycle);closesocket(connected);fd=INVALID_SOCKET;}
                clear(0,uint32_t(sn::Kind::removed));
            }
        });
    }
    void stop(){std::lock_guard call(startStop);running=false;{std::lock_guard lock(lifecycle);if(fd!=INVALID_SOCKET)shutdown(fd,SD_BOTH);}if(worker.joinable())worker.join();}
    // Threads are not joined at process exit: the loader lock is held there.
    ~Preview(){alive=false;running=false;if(notifier.joinable())notifier.detach();if(worker.joinable())worker.detach();}
};
Preview preview;
}
AXIAL_EXPORT void AxialPreviewStart(){preview.start();}
AXIAL_EXPORT void AxialPreviewStop(){preview.stop();}
// The callback runs on a bridge thread; the app marshals to its UI thread.
// It is process-wide and must not capture a view or renderer lifetime.
AXIAL_EXPORT void AxialPreviewActivity(ActivityCallback callback){preview.activityCallback=callback;}
// Balanced once per visible, idle view. Active renderers already read the
// buffer every frame; they do not need UI work on every HID report.
AXIAL_EXPORT void AxialPreviewWatch(bool enabled){
    if(enabled){++preview.activityObservers;preview.changed();}
    else --preview.activityObservers;
}
AXIAL_EXPORT uint64_t AxialPreviewNow(){return sn::now();}
AXIAL_EXPORT uint64_t AxialPreviewLostLogs(){return preview.lost.load();}
AXIAL_EXPORT bool AxialPreviewRead(uint32_t device,double* axes,uint32_t* buttons){
    if(!axes||!buttons)return false;
    for(auto& slot:preview.slots)if(slot.device.load()&&(!device||slot.device.load()==device)){
        for(int attempt=0;attempt<3;++attempt){uint64_t version=slot.version.load(std::memory_order_acquire);if(version&1)continue;
            uint32_t found=slot.device.load();uint64_t timestamp=slot.timestamp.load();for(int i=0;i<6;++i)axes[i]=slot.axes[i].load();*buttons=slot.buttons.load();
            if(slot.version.load(std::memory_order_acquire)==version&&found&&(!device||found==device)){if(!timestamp||sn::now()-timestamp>250000000)for(int i=0;i<6;++i)axes[i]=0;return true;}
        }
    }
    for(int i=0;i<6;++i)axes[i]=0;*buttons=0;return false;
}
AXIAL_EXPORT bool AxialPreviewPopLog(uint64_t* timestamp,uint32_t* device,uint32_t* changed,uint32_t* buttons,uint32_t* reason,uint32_t* identity){
    if(!timestamp||!device||!changed||!buttons||!reason||!identity)return false;
    size_t r=preview.read.load(std::memory_order_relaxed);if(r==preview.write.load(std::memory_order_acquire))return false;
    const auto& event=preview.log[r%preview.log.size()];*timestamp=event.timestamp;*device=event.device;*changed=event.changed;*buttons=event.buttons;*reason=event.reason;*identity=event.identity;
    preview.read.store(r+1,std::memory_order_release);return true;
}
#ifdef AXIAL_PREVIEW_TEST
// Test-only injection into the real preview buffer; never shipped.
AXIAL_EXPORT void AxialTestInput(double value) {
    sn::Event event;event.kind=sn::Kind::motion;event.device=1;
    event.vendor=0x046d;event.product=0xc627;event.received=sn::now();
    event.axes[0]=int16_t(value);event.axes[3]=int16_t(value);
    preview.receive(event);
}
#endif
