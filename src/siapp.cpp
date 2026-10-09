// siappdll.dll: the legacy 3DxWare input library, the Windows counterpart of
// the macOS 3DconnexionClient framework. Applications open a handle bound to a
// window, receive a registered "SpaceWareMessage00" window message, and fetch
// the event with SiGetEvent from their own message loop.
#include "axial/stream.hpp"
#include "axial/siapp.h"
#include "axial/application_buttons.hpp"
#include <boost/json/src.hpp>
#include <algorithm>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

extern "C" {__declspec(dllexport) SpwRetVal SpwErrorVal=SPW_NO_ERROR;}
namespace {
namespace json=boost::json;
struct Queued {SiSpwEvent event;size_t size;};
struct Handle {
    HWND window=nullptr;
    SiDevID device=SI_ANY_DEVICE;
    std::string application;
    std::deque<Queued> queue;
    uint32_t buttons=0;
    bool moving=false;
    uint64_t lastMotion=0;
};
std::recursive_mutex mutex;
bool initialized=false;
std::unique_ptr<sn::Stream> stream;
std::array<sn::Event,16> devices{};
std::set<Handle*> handles;
UINT spaceWareMessage(){static const UINT message=RegisterWindowMessageW(L"SpaceWareMessage00");return message;}
SpwRetVal fail(SpwRetVal error){SpwErrorVal=error;return error;}
SpwRetVal ok(){SpwErrorVal=SPW_NO_ERROR;return SPW_NO_ERROR;}
Handle* lookup(SiHdl handle){auto h=reinterpret_cast<Handle*>(handle);return handles.count(h)?h:nullptr;}
// Axial events use raw HID axes: X right, Y toward the user, Z down. The SDK
// reports a right-handed frame with Y up and Z toward the user.
void convert(const std::array<int16_t,6>& a,SPWint32 m[6]){
    m[SI_TX]=a[0];m[SI_TY]=-a[2];m[SI_TZ]=a[1];
    m[SI_RX]=a[3];m[SI_RY]=-a[5];m[SI_RZ]=a[4];
}
// Device slot N is button N+1 in the SDK, which reports button N as bit N.
uint32_t sdkButtons(uint32_t slots){return slots<<1;}
bool matches(const Handle& h,uint32_t device){return h.device==SI_ANY_DEVICE||h.device==0||uint32_t(h.device)==device;}
void deliver(Handle& h,const SiSpwEvent& event,size_t size){
    bool motion=event.type==SI_MOTION_EVENT;
    // Unread motion is replaced in place; each queued event has one message.
    if(motion&&!h.queue.empty()&&h.queue.back().event.type==SI_MOTION_EVENT){h.queue.back()={event,size};return;}
    if(h.queue.size()>=64){
        auto old=std::find_if(h.queue.begin(),h.queue.end(),[](const Queued& q){return q.event.type==SI_MOTION_EVENT;});
        h.queue.erase(old!=h.queue.end()?old:h.queue.begin());
    }
    h.queue.push_back({event,size});
    if(h.window)PostMessageW(h.window,spaceWareMessage(),0,reinterpret_cast<LPARAM>(&h));
}
constexpr size_t header=offsetof(SiSpwEvent,u);
void buttons(Handle& h,uint32_t current){
    if(current==h.buttons)return;
    SiSpwEvent event{};event.type=SI_BUTTON_EVENT;auto& b=event.u.spwData.bData;
    b.last=h.buttons;b.current=current;b.pressed=current&~h.buttons;b.released=h.buttons&~current;h.buttons=current;
    deliver(h,event,header+sizeof(SiSpwData));
}
void zero(Handle& h){
    if(!h.moving)return;h.moving=false;
    SiSpwEvent event{};event.type=SI_ZERO_EVENT;event.u.spwData.bData.last=event.u.spwData.bData.current=h.buttons;
    deliver(h,event,header+sizeof(SiSpwData));
}
void deviceChange(Handle& h,int type,uint32_t device){
    SiSpwEvent event{};event.type=SI_DEVICE_CHANGE_EVENT;event.u.deviceChangeEventData={type,SiDevID(device)};
    deliver(h,event,header+sizeof(SiDeviceChangeHeader));
}
void receive(void*,const sn::Event& e){
    std::lock_guard lock(mutex);
    if(e.kind==sn::Kind::reset){
        for(auto* h:handles)if(!e.device||matches(*h,e.device)){zero(*h);buttons(*h,0);}
        if(e.flags&sn::Flags::disconnected){
            for(const auto& d:devices)if(d.device)for(auto* h:handles)if(matches(*h,d.device))deviceChange(*h,SI_DEVICE_CHANGE_DISCONNECT,d.device);
            devices={};
        }
        return;
    }
    if(e.kind==sn::Kind::added){
        auto slot=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.device==e.device;});
        if(slot==devices.end())slot=std::find_if(devices.begin(),devices.end(),[](const auto& d){return !d.device;});
        if(slot!=devices.end())*slot=e;
        for(auto* h:handles)if(matches(*h,e.device))deviceChange(*h,SI_DEVICE_CHANGE_CONNECT,e.device);
        return;
    }
    if(e.kind==sn::Kind::removed){
        for(auto* h:handles)if(matches(*h,e.device)){zero(*h);buttons(*h,0);deviceChange(*h,SI_DEVICE_CHANGE_DISCONNECT,e.device);}
        for(auto& d:devices)if(d.device==e.device)d={};
        return;
    }
    for(auto* h:handles){
        if(!matches(*h,e.device))continue;
        if(e.kind==sn::Kind::buttons)buttons(*h,sdkButtons(e.buttons));
        else if(e.kind==sn::Kind::motion){
            if(std::all_of(e.axes.begin(),e.axes.end(),[](int x){return x==0;})){zero(*h);continue;}
            SiSpwEvent event{};event.type=SI_MOTION_EVENT;auto& data=event.u.spwData;
            data.bData.last=data.bData.current=h->buttons;convert(e.axes,data.mData);
            uint64_t time=sn::now();data.period=h->lastMotion?SPWint32(std::clamp<uint64_t>((time-h->lastMotion)/1000000,1,1000)):16;
            h->lastMotion=time;h->moving=true;deliver(*h,event,header+sizeof(SiSpwData));
        }
    }
}
// Device list from the service, used before the event stream has synchronised.
std::vector<sn::Event> serviceDevices(){
    std::vector<sn::Event> result;boost::system::error_code ec;
    auto status=json::parse(sn::request("{\"op\":\"status\"}"),ec);
    if(ec||!status.is_object())return result;auto list=status.as_object().if_contains("devices");
    if(!list||!list->is_array())return result;
    for(const auto& item:list->as_array())if(auto d=item.if_object()){
        sn::Event e;auto number=[&](const char* key){auto v=d->if_contains(key);return v&&v->is_number()?v->to_number<int64_t>():0;};
        e.device=uint32_t(number("id"));e.vendor=uint16_t(number("vendor"));e.product=uint16_t(number("product"));
        if(e.device)result.push_back(e);
    }
    return result;
}
std::vector<sn::Event> knownDevices(){
    std::vector<sn::Event> result;
    {std::lock_guard lock(mutex);for(const auto& d:devices)if(d.device)result.push_back(d);}
    return result.empty()?serviceDevices():result;
}
bool deviceIdentity(SiDevID id,sn::Event& found){
    for(const auto& d:knownDevices())if(id==SI_ANY_DEVICE||id==0||SiDevID(d.device)==id){found=d;return true;}
    return false;
}
int deviceType(uint16_t vendor,uint16_t product){
    if(vendor==0x046d)switch(product){
        case 0xc621:return SI_SPACEBALL_5000;case 0xc623:return SI_TRAVELER;case 0xc625:return SI_SPACEPILOT;
        case 0xc626:return SI_SPACENAVIGATOR;case 0xc627:return SI_SPACEEXPLORER;case 0xc628:return SI_SPACENAVIGATOR_FOR_NOTEBOOKS;
        default:break;
    }
    return product?int(product):int(SI_UNKNOWN_DEVICE);
}
void copy(char* target,size_t size,const std::string& text){if(size){size_t n=std::min(size-1,text.size());memcpy(target,text.data(),n);target[n]=0;}}
SpwRetVal fetch(SiHdl handle,int,const SiGetEventData* data,SiSpwEvent* event,bool remove){
    if(!event)return fail(SI_BAD_VALUE);
    std::lock_guard lock(mutex);
    Handle* h=lookup(handle);if(!h)return fail(SI_BAD_HANDLE);
    if(data&&h->window&&data->msg!=spaceWareMessage())return fail(SI_NOT_EVENT);
    if(h->queue.empty())return fail(SI_SKIP_EVENT);
    const auto& next=h->queue.front();
    memcpy(event,&next.event,next.size);
    if(remove)h->queue.pop_front();
    return ok(),SI_IS_EVENT;
}
}

extern "C" {
SpwRetVal SPWAPI SiInitialize(void){
    static const bool mapped=[] {sn::keepCallbackCodeMapped(reinterpret_cast<const void*>(&SiInitialize));return true;}();(void)mapped;
    if(!sn::winsock())return fail(SI_NO_DRIVER);
    std::lock_guard lock(mutex);
    spaceWareMessage();initialized=true;
    return ok();
}
SPWbool SPWAPI SiIsInitialized(void){std::lock_guard lock(mutex);return initialized;}
void SPWAPI SiTerminate(void){
    std::unique_ptr<sn::Stream> retired;
    {std::lock_guard lock(mutex);for(auto* h:handles)delete h;handles.clear();retired=std::move(stream);devices={};initialized=false;}
    if(retired)retired->stop();
}
int SPWAPI SiGetNumDevices(void){return int(knownDevices().size());}
SiDevID SPWAPI SiDeviceIndex(int index){
    auto list=knownDevices();
    return index>=0&&size_t(index)<list.size()?SiDevID(list[size_t(index)].device):SI_NO_DEVICE;
}
void SPWAPI SiOpenWinInit(SiOpenData* data,HWND window){if(data)data->hWnd=window;}
SiHdl SPWAPI SiOpen(const char* application,SiDevID device,const SiTypeMask*,int mode,const SiOpenData* data){
    std::lock_guard lock(mutex);
    if(!initialized){fail(SI_UNINITIALIZED);return nullptr;}
    if(mode&SI_EVENT&&(!data||!data->hWnd)){fail(SI_BAD_VALUE);return nullptr;}
    auto* h=new Handle;h->window=(mode&SI_EVENT)?data->hWnd:nullptr;h->device=device;h->application=application?application:"";
    handles.insert(h);
    // A handle is valid before a controller is attached; hotplug events follow.
    if(!stream){stream=std::make_unique<sn::Stream>(receive,nullptr);stream->start();}
    else for(const auto& d:devices)if(d.device&&matches(*h,d.device))deviceChange(*h,SI_DEVICE_CHANGE_CONNECT,d.device);
    ok();return reinterpret_cast<SiHdl>(h);
}
SiHdl SPWAPI SiOpenPort(const char* application,const SiDevPort* port,int mode,const SiOpenData* data){
    return SiOpen(application,port?port->devID:SI_ANY_DEVICE,nullptr,mode,data);
}
SpwRetVal SPWAPI SiClose(SiHdl handle){
    std::unique_ptr<sn::Stream> retired;
    {std::lock_guard lock(mutex);Handle* h=lookup(handle);if(!h)return fail(SI_BAD_HANDLE);
        handles.erase(h);delete h;if(handles.empty()){retired=std::move(stream);devices={};}}
    if(retired)retired->stop();
    return ok();
}
void SPWAPI SiGetEventWinInit(SiGetEventData* data,UINT message,WPARAM wParam,LPARAM lParam){if(data){data->msg=message;data->wParam=wParam;data->lParam=lParam;}}
SpwRetVal SPWAPI SiGetEvent(SiHdl handle,int flags,const SiGetEventData* data,SiSpwEvent* event){return fetch(handle,flags,data,event,true);}
SpwRetVal SPWAPI SiPeekEvent(SiHdl handle,int flags,const SiGetEventData* data,SiSpwEvent* event){return fetch(handle,flags,data,event,false);}
SPWbool SPWAPI SiIsSpaceWareEvent(const SiGetEventData* data,SiHdl){return data&&data->msg==spaceWareMessage();}
int SPWAPI SiDispatch(SiHdl handle,SiGetEventData* data,SiSpwEvent* event,SiSpwHandlers* handlers){
    if(!event||!handlers||!lookup(handle))return 0;
    SiOpenData open{};{std::lock_guard lock(mutex);if(Handle* h=lookup(handle))open.hWnd=h->window;}
    SiEventHandler* handler=nullptr;
    switch(event->type){
        case SI_BUTTON_EVENT:handler=&handlers->button;break;
        case SI_MOTION_EVENT:handler=&handlers->motion;break;
        case SI_COMBO_EVENT:handler=&handlers->combo;break;
        case SI_ZERO_EVENT:handler=&handlers->zero;break;
        default:handler=&handlers->exception;break;
    }
    return handler&&handler->func?handler->func(&open,data,event,handler->data):0;
}
int SPWAPI SiButtonPressed(SiSpwEvent* event){
    if(!event||event->type!=SI_BUTTON_EVENT||!event->u.spwData.bData.pressed)return SI_NO_BUTTON;
    unsigned long index;_BitScanForward(&index,event->u.spwData.bData.pressed);return int(index);
}
int SPWAPI SiButtonReleased(SiSpwEvent* event){
    if(!event||event->type!=SI_BUTTON_EVENT||!event->u.spwData.bData.released)return SI_NO_BUTTON;
    unsigned long index;_BitScanForward(&index,event->u.spwData.bData.released);return int(index);
}
SpwRetVal SPWAPI SiGetButtonName(SiHdl handle,SPWuint32 button,SiButtonName* name){
    if(!name)return fail(SI_BAD_VALUE);
    SiDevID device;{std::lock_guard lock(mutex);Handle* h=lookup(handle);if(!h)return fail(SI_BAD_HANDLE);device=h->device;}
    sn::Event e;if(!button||!deviceIdentity(device,e))return fail(SI_BAD_VALUE);
    auto spec=sn::deviceSpec(e.vendor,e.product);auto b=spec?spec->button(button-1):nullptr;
    if(!b)return fail(SI_BAD_VALUE);
    // Names are UTF-8 in the device table; convert the minus sign to ASCII.
    std::string text=b->name;if(text=="\xe2\x88\x92")text="-";
    copy(name->name,SI_STRSIZE,text);return ok();
}
SpwRetVal SPWAPI SiGetDeviceName(SiHdl handle,SiDeviceName* name){
    if(!name)return fail(SI_BAD_VALUE);
    SiDevID device;{std::lock_guard lock(mutex);Handle* h=lookup(handle);if(!h)return fail(SI_BAD_HANDLE);device=h->device;}
    sn::Event e;if(!deviceIdentity(device,e))return fail(SI_BAD_ID);
    auto spec=sn::deviceSpec(e.vendor,e.product);copy(name->name,SI_STRSIZE,spec?spec->name:"3D controller");return ok();
}
SpwRetVal SPWAPI SiGetDeviceInfo(SiHdl handle,SiDevInfo* info){
    if(!info)return fail(SI_BAD_VALUE);
    SiDevID device;{std::lock_guard lock(mutex);Handle* h=lookup(handle);if(!h)return fail(SI_BAD_HANDLE);device=h->device;}
    sn::Event e;if(!deviceIdentity(device,e))return fail(SI_BAD_ID);
    auto spec=sn::deviceSpec(e.vendor,e.product);
    info->devType=deviceType(e.vendor,e.product);info->numButtons=spec?int(spec->buttons.size()):0;
    info->numDegrees=6;info->canBeep=FALSE;copy(info->firmware,SI_STRSIZE,"Axial");return ok();
}
SiDevID SPWAPI SiGetDeviceID(SiHdl handle){
    SiDevID device;{std::lock_guard lock(mutex);Handle* h=lookup(handle);if(!h){fail(SI_BAD_HANDLE);return SI_NO_DEVICE;}device=h->device;}
    sn::Event e;return deviceIdentity(device,e)?SiDevID(e.device):SI_NO_DEVICE;
}
SpwRetVal SPWAPI SiGetDevicePort(SiDevID device,SiDevPort* port){
    if(!port)return fail(SI_BAD_VALUE);
    sn::Event e;if(!deviceIdentity(device,e))return fail(SI_BAD_ID);
    auto spec=sn::deviceSpec(e.vendor,e.product);
    port->devID=SiDevID(e.device);port->devType=deviceType(e.vendor,e.product);port->devClass=0;
    copy(port->devName,SI_STRSIZE,spec?spec->name:"3D controller");
    char text[32];snprintf(text,sizeof(text),"USB %04x:%04x",e.vendor,e.product);copy(port->portName,SI_MAXPORTNAME,text);
    return ok();
}
void SPWAPI SiGetLibraryInfo(SiVerInfo* info){if(!info)return;*info={};info->major=4;info->minor=0;info->build=0;copy(info->version,SI_STRSIZE,"Axial siappdll 4.0");copy(info->date,SI_STRSIZE,__DATE__);}
SpwRetVal SPWAPI SiGetDriverInfo(SiVerInfo* info){
    if(!info)return fail(SI_BAD_VALUE);*info={};info->major=4;copy(info->version,SI_STRSIZE,"Axial");copy(info->date,SI_STRSIZE,__DATE__);
    return sn::request("{\"op\":\"status\"}").starts_with("{\"error\"")?fail(SI_NO_DRIVER):ok();
}
SpwRetVal SPWAPI SiBeep(SiHdl handle,char*){std::lock_guard lock(mutex);return lookup(handle)?ok():fail(SI_BAD_HANDLE);}
// Zero calibration: the current deflection of the cap becomes its rest position.
SpwRetVal SPWAPI SiRezero(SiHdl handle){
    {std::lock_guard lock(mutex);if(!lookup(handle))return fail(SI_BAD_HANDLE);}
    return sn::request("{\"op\":\"calibrate\"}").starts_with("{\"error\"")?fail(SI_NO_DRIVER):ok();
}
SpwRetVal SPWAPI SiGrabDevice(SiHdl handle,SPWbool){std::lock_guard lock(mutex);return lookup(handle)?ok():fail(SI_BAD_HANDLE);}
SpwRetVal SPWAPI SiReleaseDevice(SiHdl handle){std::lock_guard lock(mutex);return lookup(handle)?ok():fail(SI_BAD_HANDLE);}
SpwRetVal SPWAPI SiSetUiMode(SiHdl handle,SPWuint32){std::lock_guard lock(mutex);return lookup(handle)?ok():fail(SI_BAD_HANDLE);}
// LED state follows the active Axial profile; applications cannot override it.
SpwRetVal SPWAPI SiSetLEDs(SiHdl handle,SPWuint32){std::lock_guard lock(mutex);return lookup(handle)?ok():fail(SI_BAD_HANDLE);}
SpwRetVal SPWAPI SiGetDeviceImageFileName(SiHdl,char*,SPWuint32*){return fail(SI_UNSUPPORTED);}
SpwRetVal SPWAPI SiGetCompanyLogoFileName(char*,SPWuint32*){return fail(SI_UNSUPPORTED);}
HICON SPWAPI SiGetCompanyIcon(void){return nullptr;}
const char* SPWAPI SpwErrorString(SpwRetVal error){
    static const char* names[]={"No error","Function failed","Invalid handle","Invalid device ID","Invalid argument",
        "Event is a SpaceWare event","Skip this SpaceWare event","Not a SpaceWare event","Axial is not running",
        "Axial is not responding","Unsupported function","Library is not initialized","Incorrect driver",
        "Internal error","Bad protocol","Out of memory","DLL load error","Device not open","Item not found","Unsupported device"};
    return unsigned(error)<std::size(names)?names[error]:"Unknown error";
}
SpwRetVal __cdecl SiSetTypeMask(SiTypeMask* mask,int,...){if(mask)memset(mask,0,sizeof(*mask));return ok();}
// The synchronous configuration API of 3DxWare 10 is not provided by Axial;
// exported with exact argument sizes so 32-bit stdcall callers stay balanced.
#define AXIAL_SYNC(name,...) SpwRetVal SPWAPI name(__VA_ARGS__){return fail(SI_UNSUPPORTED);}
AXIAL_SYNC(SiSyncSendQuery,SiHdl)
AXIAL_SYNC(SiSyncGetVersion,SiHdl,SPWuint32*,SPWuint32*)
AXIAL_SYNC(SiSyncGetNumberOfFunctions,SiHdl,SPWuint32*)
AXIAL_SYNC(SiSyncGetFunction,SiHdl,SPWuint32,SPWint32*,wchar_t*,SPWuint32*)
AXIAL_SYNC(SiSyncGetButtonAssignment,SiHdl,SPWuint32,SPWint32*)
AXIAL_SYNC(SiSyncSetButtonAssignment,SiHdl,SPWuint32,SPWint32)
AXIAL_SYNC(SiSyncSetButtonAssignmentAbsolute,SiHdl,SPWuint32,SPWint32)
AXIAL_SYNC(SiSyncSetButtonName,SiHdl,SPWuint32,const wchar_t*)
AXIAL_SYNC(SiSyncGetAxisLabel,SiHdl,SPWuint32,wchar_t*,SPWuint32*)
AXIAL_SYNC(SiSyncSetAxisLabel,SiHdl,SPWuint32,const wchar_t*)
AXIAL_SYNC(SiSyncGetOrientation,SiHdl,SPWint32*)
AXIAL_SYNC(SiSyncSetOrientation,SiHdl,const SPWint32*)
AXIAL_SYNC(SiSyncGetFilter,SiHdl,int,int*)
AXIAL_SYNC(SiSyncSetFilter,SiHdl,int,int)
AXIAL_SYNC(SiSyncGetAxesState,SiHdl,int*)
AXIAL_SYNC(SiSyncSetAxesState,SiHdl,int)
AXIAL_SYNC(SiSyncSetInfoLine,SiHdl,SPWint32,const wchar_t*)
AXIAL_SYNC(SiSyncGetScaleOverall,SiHdl,SPWfloat32*)
AXIAL_SYNC(SiSyncSetScaleOverall,SiHdl,SPWfloat32)
AXIAL_SYNC(SiSyncGetScaleTx,SiHdl,SPWfloat32*)
AXIAL_SYNC(SiSyncSetScaleTx,SiHdl,SPWfloat32)
AXIAL_SYNC(SiSyncGetScaleTy,SiHdl,SPWfloat32*)
AXIAL_SYNC(SiSyncSetScaleTy,SiHdl,SPWfloat32)
AXIAL_SYNC(SiSyncGetScaleTz,SiHdl,SPWfloat32*)
AXIAL_SYNC(SiSyncSetScaleTz,SiHdl,SPWfloat32)
AXIAL_SYNC(SiSyncGetScaleRx,SiHdl,SPWfloat32*)
AXIAL_SYNC(SiSyncSetScaleRx,SiHdl,SPWfloat32)
AXIAL_SYNC(SiSyncGetScaleRy,SiHdl,SPWfloat32*)
AXIAL_SYNC(SiSyncSetScaleRy,SiHdl,SPWfloat32)
AXIAL_SYNC(SiSyncGetScaleRz,SiHdl,SPWfloat32*)
AXIAL_SYNC(SiSyncSetScaleRz,SiHdl,SPWfloat32)
AXIAL_SYNC(SiSyncInvokeAbsoluteFunction,SiHdl,int)
AXIAL_SYNC(SiSyncSetButtonState,SiHdl,SPWuint32,int)
}
