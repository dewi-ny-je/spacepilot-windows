#include "support.hpp"
#include "axial/siapp.h"
#include <cerrno>
#include <navlib/navlib.h>
#include <array>
#include <atomic>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
extern "C" void AxialPreviewStart();
extern "C" void AxialPreviewStop();
extern "C" bool AxialPreviewRead(uint32_t,double*,uint32_t*);
extern "C" bool AxialPreviewPopLog(uint64_t*,uint32_t*,uint32_t*,uint32_t*,uint32_t*,uint32_t*);
extern "C" uint64_t AxialPreviewLostLogs();
#define CHECK(x) do{if(!(x)){std::cerr<<__FILE__<<":"<<__LINE__<<" " #x "\n";throw std::runtime_error(#x);}}while(0)
namespace {
// Client message loop: Navlib and the legacy library both deliver through it.
void pump(int milliseconds){
    auto deadline=GetTickCount64()+uint64_t(milliseconds);
    do{MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        MsgWaitForMultipleObjects(0,nullptr,FALSE,1,QS_ALLINPUT);
    }while(GetTickCount64()<deadline);
}
bool waitFor(std::function<bool()> condition,int attempts=300){for(int i=0;i<attempts;++i){if(condition())return true;pump(10);}return condition();}
bool clients(int count){return sn::request("{\"op\":\"status\"}").find("\"clients\":"+std::to_string(count)+",")!=std::string::npos;}

// The legacy 3DxWare API, loaded the way applications load siappdll.dll.
struct Legacy {
    HMODULE module=nullptr;
    decltype(&SiInitialize) initialize;decltype(&SiTerminate) terminate;decltype(&SiOpenWinInit) openWinInit;
    decltype(&SiOpen) open;decltype(&SiClose) close;decltype(&SiGetEventWinInit) eventWinInit;decltype(&SiGetEvent) getEvent;
    decltype(&SiPeekEvent) peekEvent;decltype(&SiIsSpaceWareEvent) isEvent;decltype(&SiButtonPressed) pressed;decltype(&SiButtonReleased) released;
    decltype(&SiGetDeviceInfo) deviceInfo;decltype(&SiGetDeviceName) deviceName;decltype(&SiGetDeviceID) deviceID;decltype(&SiGetButtonName) buttonName;
    decltype(&SiGetNumDevices) numDevices;decltype(&SiDeviceIndex) deviceIndex;decltype(&SiDispatch) dispatch;decltype(&SpwErrorString) errorString;
    SpwRetVal* errorValue=nullptr;
    template<class F> void load(F& f,const char* name){f=reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(module,name)));CHECK(f);}
    explicit Legacy(const std::string& path){
        module=LoadLibraryW(sn::wide(path).c_str());CHECK(module);
        load(initialize,"SiInitialize");load(terminate,"SiTerminate");load(openWinInit,"SiOpenWinInit");load(open,"SiOpen");load(close,"SiClose");
        load(eventWinInit,"SiGetEventWinInit");load(getEvent,"SiGetEvent");load(peekEvent,"SiPeekEvent");load(isEvent,"SiIsSpaceWareEvent");
        load(pressed,"SiButtonPressed");load(released,"SiButtonReleased");load(deviceInfo,"SiGetDeviceInfo");load(deviceName,"SiGetDeviceName");
        load(deviceID,"SiGetDeviceID");load(buttonName,"SiGetButtonName");load(numDevices,"SiGetNumDevices");load(deviceIndex,"SiDeviceIndex");
        load(dispatch,"SiDispatch");load(errorString,"SpwErrorString");
        errorValue=reinterpret_cast<SpwRetVal*>(GetProcAddress(module,"SpwErrorVal"));CHECK(errorValue);
    }
};
struct Received {SiHdl handle;SiSpwEvent event;};
Legacy* legacy=nullptr;
std::vector<SiHdl> handles;
std::vector<Received> received;
std::function<void(SiHdl,const SiSpwEvent&)> onEvent;
LRESULT CALLBACK clientWindow(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam){
    if(legacy){
        SiGetEventData data;legacy->eventWinInit(&data,message,wParam,lParam);
        if(legacy->isEvent(&data,nullptr)){
            // Iterate over a copy: a handler may close handles.
            auto open=handles;
            for(auto h:open){SiSpwEvent event;
                while(std::find(handles.begin(),handles.end(),h)!=handles.end()&&legacy->getEvent(h,0,&data,&event)==SI_IS_EVENT){received.push_back({h,event});if(onEvent)onEvent(h,event);}
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd,message,wParam,lParam);
}
size_t count(SiHdl h,int type){size_t n=0;for(const auto& r:received)if(r.handle==h&&r.event.type==type)++n;return n;}
const SiSpwEvent* last(SiHdl h,int type){for(auto it=received.rbegin();it!=received.rend();++it)if(it->handle==h&&it->event.type==type)return &it->event;return nullptr;}

struct CameraState {
    navlib::matrix_t matrix{};
    std::atomic<int> frames=0,transactions=0,motion=0;
    bool perspective=true,rotatable=true;
    decltype(&navlib::NlClose) closeDuringGet=nullptr;
    navlib::nlHandle_t handle=0;
    bool wasClosed=false;
    std::vector<std::string> commands;
    DWORD thread=0;
    navlib::box_t extents{{-10,-10,-10},{10,10,10}};
    CameraState(){matrix.m00=matrix.m11=matrix.m22=matrix.m33=1;matrix.m23=10;}
};
long getCamera(navlib::param_t param,navlib::property_t name,navlib::value_t* out){
    auto& c=*reinterpret_cast<CameraState*>(param);
    CHECK(!c.wasClosed);c.thread=GetCurrentThreadId();
    if(!strcmp(name,"view.affine"))*out=c.matrix;
    else if(!strcmp(name,"view.perspective"))*out=c.perspective;
    else if(!strcmp(name,"view.rotatable"))*out=c.rotatable;
    else if(!strcmp(name,"view.target"))*out=navlib::point_t{0,0,0};
    else if(!strcmp(name,"view.extents"))*out=c.extents;
    else if(!strcmp(name,"model.extents"))*out=navlib::box_t{{-1,-1,-1},{1,1,1}};
    else return navlib::make_result_code(0x201);
    if(c.closeDuringGet&&!strcmp(name,"view.perspective")){auto close=c.closeDuringGet;c.closeDuringGet=nullptr;CHECK(close(c.handle)==0);c.wasClosed=true;}
    return 0;
}
long setCamera(navlib::param_t param,navlib::property_t name,const navlib::value_t* value){
    auto& c=*reinterpret_cast<CameraState*>(param);c.thread=GetCurrentThreadId();
    if(!strcmp(name,"view.affine")){c.matrix=value->matrix;++c.frames;}
    else if(!strcmp(name,"transaction")){CHECK(value->l==0||value->l==1);c.transactions+=value->l?1:-1;CHECK(c.transactions>=0&&c.transactions<=1);}
    else if(!strcmp(name,"motion"))c.motion=value->b;
    else if(!strcmp(name,"view.extents"))c.extents=value->box;
    else if(!strcmp(name,"commands.activeCommand"))c.commands.emplace_back(value->string.p);
    return 0;
}
}
int main(int argc,char** argv){try {
    if(argc!=4){std::cerr<<"usage: integration-tests service siappdll navlib\n";return 2;}
    MockService service(argv[1],true);
    CHECK(waitFor([]{auto status=sn::request("{\"op\":\"status\"}");return status.find("\"accessibility\":true")!=std::string::npos;}));

    // Legacy siappdll: window-message delivery through the client's own loop.
    Legacy api(argv[2]);legacy=&api;
    WNDCLASSW windowClass{};windowClass.lpfnWndProc=clientWindow;windowClass.hInstance=GetModuleHandleW(nullptr);windowClass.lpszClassName=L"AxialIntegrationClient";
    CHECK(RegisterClassW(&windowClass));
    HWND window=CreateWindowExW(0,L"AxialIntegrationClient",L"Axial test",WS_OVERLAPPEDWINDOW,0,0,100,100,nullptr,nullptr,windowClass.hInstance,nullptr);CHECK(window);
    SiOpenData open{};api.openWinInit(&open,window);CHECK(open.hWnd==window);
    CHECK(api.open("PrusaSlicer",SI_ANY_DEVICE,nullptr,SI_EVENT,&open)==nullptr);CHECK(*api.errorValue==SI_UNINITIALIZED);
    CHECK(api.initialize()==SPW_NO_ERROR);
    CHECK(api.open("PrusaSlicer",SI_ANY_DEVICE,nullptr,SI_EVENT,nullptr)==nullptr);CHECK(*api.errorValue==SI_BAD_VALUE);
    SiHdl client=api.open("PrusaSlicer",SI_ANY_DEVICE,nullptr,SI_EVENT,&open);CHECK(client);handles.push_back(client);
    sn::Socket inject=sn::openEvents(sn::Flags::replay);CHECK(inject!=INVALID_SOCKET);
    CHECK(waitFor([]{return clients(2);}));
    sn::Event e;e.device=1;e.vendor=0x046d;e.product=0xc627;e.kind=sn::Kind::added;e.received=sn::now();
    CHECK(sn::writeAll(inject,&e,sizeof(e)));
    CHECK(waitFor([&]{auto c=last(client,SI_DEVICE_CHANGE_EVENT);return c&&c->u.deviceChangeEventData.type==SI_DEVICE_CHANGE_CONNECT&&c->u.deviceChangeEventData.devID==1;}));
    CHECK(api.deviceID(client)==1);CHECK(api.numDevices()==1);CHECK(api.deviceIndex(0)==1);CHECK(api.deviceIndex(1)==SI_NO_DEVICE);
    SiDevInfo info{};CHECK(api.deviceInfo(client,&info)==SPW_NO_ERROR);CHECK(info.devType==SI_SPACEEXPLORER);CHECK(info.numButtons==15);CHECK(info.numDegrees==6);
    SiDeviceName name{};CHECK(api.deviceName(client,&name)==SPW_NO_ERROR);CHECK(std::string(name.name)=="SpaceExplorer");
    SiButtonName button{};CHECK(api.buttonName(client,1,&button)==SPW_NO_ERROR);CHECK(button.name[0]);
    CHECK(api.buttonName(client,0,&button)==SI_BAD_VALUE);
    // Device IDs grow across hotplug cycles; IDs 1 and 17 must not alias.
    auto another=e;another.device=17;another.vendor=0x256f;another.product=0xc635;
    CHECK(sn::writeAll(inject,&another,sizeof(another)));CHECK(waitFor([&]{return count(client,SI_DEVICE_CHANGE_EVENT)==2;}));
    another.kind=sn::Kind::removed;CHECK(sn::writeAll(inject,&another,sizeof(another)));CHECK(waitFor([&]{return count(client,SI_DEVICE_CHANGE_EVENT)==3;}));
    CHECK(last(client,SI_DEVICE_CHANGE_EVENT)->u.deviceChangeEventData.type==SI_DEVICE_CHANGE_DISCONNECT);
    CHECK(last(client,SI_DEVICE_CHANGE_EVENT)->u.deviceChangeEventData.devID==17);
    CHECK(api.deviceID(client)==1);
    CHECK(sn::request("{\"op\":\"status\"}").find("\"product\":50727")!=std::string::npos);
    // Axial's HID frame (Y toward the user, Z down) maps to the SDK frame.
    e.kind=sn::Kind::motion;e.axes={350,0,0,0,0,0};e.received=sn::now();CHECK(sn::writeAll(inject,&e,sizeof(e)));
    CHECK(waitFor([&]{auto m=last(client,SI_MOTION_EVENT);return m&&m->u.spwData.mData[SI_TX]==350;}));
    e.axes={0,40,-30,0,0,0};CHECK(sn::writeAll(inject,&e,sizeof(e)));
    CHECK(waitFor([&]{auto m=last(client,SI_MOTION_EVENT);return m&&m->u.spwData.mData[SI_TY]==30&&m->u.spwData.mData[SI_TZ]==40;}));
    e.axes={};CHECK(sn::writeAll(inject,&e,sizeof(e)));CHECK(waitFor([&]{return count(client,SI_ZERO_EVENT)==1;}));
    // Device slots 0 and 2 are SDK buttons 1 and 3.
    e.kind=sn::Kind::buttons;e.buttons=5;CHECK(sn::writeAll(inject,&e,sizeof(e)));
    CHECK(waitFor([&]{return count(client,SI_BUTTON_EVENT)==1;}));
    {auto b=*last(client,SI_BUTTON_EVENT);CHECK(b.u.spwData.bData.current==0xa&&b.u.spwData.bData.pressed==0xa&&b.u.spwData.bData.released==0);CHECK(api.pressed(&b)==1);CHECK(api.released(&b)==SI_NO_BUTTON);}
    e.buttons=4;CHECK(sn::writeAll(inject,&e,sizeof(e)));CHECK(waitFor([&]{return count(client,SI_BUTTON_EVENT)==2;}));
    {auto b=*last(client,SI_BUTTON_EVENT);CHECK(b.u.spwData.bData.last==0xa&&b.u.spwData.bData.released==2);CHECK(api.released(&b)==1);}
    e.buttons=0;CHECK(sn::writeAll(inject,&e,sizeof(e)));CHECK(waitFor([&]{return count(client,SI_BUTTON_EVENT)==3;}));
    std::string config=R"({"op":"setConfig","config":{"version":1,"profiles":{"*":{"gain":[2,1,1,1,1,1]}}}})";
    CHECK(sn::request(config).find("\"ok\":true")!=std::string::npos);
    CHECK(sn::request(R"({"op":"setConfig","config":{"version":1,"profiles":{"*":{"led":false}}}})").find("\"ok\":true")!=std::string::npos);
    CHECK(sn::request("{\"op\":\"getConfig\"}").find("\"led\":false")!=std::string::npos);
    CHECK(sn::request(R"({"op":"setConfig","config":{"version":1,"profiles":{},"web":{"enabled":"true"}}})").find("error")!=std::string::npos);
    CHECK(sn::request(R"({"op":"setConfig","config":{"version":1,"profiles":{},"web":{"enabled":false}}})").find("\"ok\":true")!=std::string::npos);
    CHECK(sn::request(R"({"op":"setConfig","config":{"version":1,"profiles":{"*":{"led":1}}}})").find("error")!=std::string::npos);
    CHECK(sn::request(config).find("\"ok\":true")!=std::string::npos);
    e.kind=sn::Kind::motion;e.axes={350,0,0,0,0,0};e.received=sn::now();CHECK(sn::writeAll(inject,&e,sizeof(e)));
    CHECK(waitFor([&]{auto m=last(client,SI_MOTION_EVENT);return m&&m->u.spwData.mData[SI_TX]==700;}));
    CHECK(sn::request(R"({"op":"setConfig","config":{"version":1,"profiles":{"*":{"gain":[-1,1,1,1,1,1]}}}})").find("error")!=std::string::npos);
    for(const char* invalid:{R"({"buttons":[{"keyCode":{}}]})",R"({"buttons":[{"keyCode":1.5}]})",R"({"buttons":[{"keyCode":255}]})",R"({"buttons":[{"modifiers":-1}]})",R"({"buttons":[{"command":42}]})",R"({"dominant":1})"}){
        CHECK(sn::request(std::string("{\"op\":\"setConfig\",\"config\":{\"version\":1,\"profiles\":{\"*\":")+invalid+"}}}").find("error")!=std::string::npos);
    }
    CHECK(sn::request(R"({"op":"setConfig","config":{"version":1,"profiles":{"*":{"buttons":[{"action":"dominant"}]}}}})").find("\"ok\":true")!=std::string::npos);
    e.kind=sn::Kind::buttons;e.buttons=0;CHECK(sn::writeAll(inject,&e,sizeof(e)));e.buttons=1;CHECK(sn::writeAll(inject,&e,sizeof(e)));
    e.kind=sn::Kind::motion;e.axes={100,20,0,0,0,0};CHECK(sn::writeAll(inject,&e,sizeof(e)));
    CHECK(waitFor([&]{auto m=last(client,SI_MOTION_EVENT);return m&&m->u.spwData.mData[SI_TX]==100;}));CHECK(last(client,SI_MOTION_EVENT)->u.spwData.mData[SI_TZ]==0);
    CHECK(sn::request(config).find("\"ok\":true")!=std::string::npos);
    e.kind=sn::Kind::removed;e.axes={};e.buttons=0;CHECK(sn::writeAll(inject,&e,sizeof(e)));
    CHECK(waitFor([&]{auto c=last(client,SI_DEVICE_CHANGE_EVENT);return c&&c->u.deviceChangeEventData.devID==1&&c->u.deviceChangeEventData.type==SI_DEVICE_CHANGE_DISCONNECT;}));
    CHECK(api.close(client)==SPW_NO_ERROR);handles.clear();CHECK(waitFor([]{return clients(1);}));
    // A closed handle is rejected rather than dereferenced.
    SiSpwEvent stale;SiGetEventData data{};CHECK(api.close(client)==SI_BAD_HANDLE);CHECK(api.getEvent(client,0,&data,&stale)==SI_BAD_HANDLE);
    CHECK(std::string(api.errorString(SI_BAD_HANDLE))=="Invalid handle");
    // Polling handles have no window; SiGetEvent reads the queue directly.
    SiHdl polled=api.open("poller",SI_ANY_DEVICE,nullptr,SI_POLL,nullptr);CHECK(polled);
    CHECK(waitFor([]{return clients(2);}));
    e.kind=sn::Kind::added;CHECK(sn::writeAll(inject,&e,sizeof(e)));
    e.kind=sn::Kind::motion;e.axes={0,0,0,0,0,123};CHECK(sn::writeAll(inject,&e,sizeof(e)));
    bool polledMotion=false;
    CHECK(waitFor([&]{SiSpwEvent event;while(api.getEvent(polled,0,nullptr,&event)==SI_IS_EVENT)if(event.type==SI_MOTION_EVENT&&event.u.spwData.mData[SI_RY]==-123)polledMotion=true;return polledMotion;}));
    {SiSpwEvent event;CHECK(api.getEvent(polled,0,nullptr,&event)==SI_SKIP_EVENT);}
    e.axes={};CHECK(sn::writeAll(inject,&e,sizeof(e)));
    CHECK(api.close(polled)==SPW_NO_ERROR);CHECK(waitFor([]{return clients(1);}));
    // A handler may close its own handle while processing a reset; the other
    // handle keeps receiving.
    auto retiring=api.open("retiring",SI_ANY_DEVICE,nullptr,SI_EVENT,&open);auto survivor=api.open("survivor",SI_ANY_DEVICE,nullptr,SI_EVENT,&open);
    CHECK(retiring&&survivor);handles={retiring,survivor};received.clear();int retiredCallbacks=0;
    onEvent=[&](SiHdl h,const SiSpwEvent& event){
        if(h==retiring&&event.type==SI_BUTTON_EVENT){++retiredCallbacks;CHECK(api.close(retiring)==SPW_NO_ERROR);std::erase(handles,retiring);}
    };
    CHECK(waitFor([]{return clients(2);}));
    e.kind=sn::Kind::buttons;e.buttons=1;CHECK(sn::writeAll(inject,&e,sizeof(e)));
    auto reset=e;reset.kind=sn::Kind::reset;CHECK(sn::writeAll(inject,&reset,sizeof(reset)));
    CHECK(waitFor([&]{return retiredCallbacks==1&&count(survivor,SI_BUTTON_EVENT)==2;}));
    reset.kind=sn::Kind::motion;reset.axes={5,0,0,0,0,0};CHECK(sn::writeAll(inject,&reset,sizeof(reset)));CHECK(waitFor([&]{return count(survivor,SI_MOTION_EVENT)>0;}));
    CHECK(retiredCallbacks==1);onEvent=nullptr;
    reset.axes={};CHECK(sn::writeAll(inject,&reset,sizeof(reset)));
    // SiDispatch routes to the handler for each event type.
    int dispatched=0;SiSpwHandlers handlers{};
    handlers.motion={[](SiOpenData*,SiGetEventData*,SiSpwEvent* event,void* context){CHECK(event->type==SI_MOTION_EVENT);++*static_cast<int*>(context);return 1;},&dispatched};
    SiSpwEvent motion{};motion.type=SI_MOTION_EVENT;CHECK(api.dispatch(survivor,&data,&motion,&handlers)==1);CHECK(dispatched==1);
    SiSpwEvent other{};other.type=SI_ZERO_EVENT;CHECK(api.dispatch(survivor,&data,&other,&handlers)==0);
    CHECK(api.close(survivor)==SPW_NO_ERROR);handles.clear();
    CHECK(waitFor([]{return clients(1);}));
    e.kind=sn::Kind::removed;e.buttons=0;CHECK(sn::writeAll(inject,&e,sizeof(e)));
    api.terminate();legacy=nullptr;CHECK(FreeLibrary(api.module));pump(50);
    DestroyWindow(window);

    HMODULE library=LoadLibraryW(sn::wide(argv[3]).c_str());CHECK(library);
    auto symbol=[&](const char* name){auto p=GetProcAddress(library,name);CHECK(p);return reinterpret_cast<void*>(p);};
    auto create=reinterpret_cast<decltype(&navlib::NlCreate)>(symbol("NlCreate"));
    auto write=reinterpret_cast<decltype(&navlib::NlWriteValue)>(symbol("NlWriteValue"));
    auto read=reinterpret_cast<decltype(&navlib::NlReadValue)>(symbol("NlReadValue"));
    auto type=reinterpret_cast<decltype(&navlib::NlGetType)>(symbol("NlGetType"));
    auto close=reinterpret_cast<decltype(&navlib::NlClose)>(symbol("NlClose"));
    CHECK(type("view.affine")==navlib::matrix_type);CHECK(type("unknown")==navlib::unknown_type);
    CameraState camera;navlib::accessor_t accessors[]={
        {"view.affine",getCamera,setCamera,reinterpret_cast<uint64_t>(&camera)},
        {"view.perspective",getCamera,nullptr,reinterpret_cast<uint64_t>(&camera)},
        {"view.rotatable",getCamera,nullptr,reinterpret_cast<uint64_t>(&camera)},
        {"view.target",getCamera,nullptr,reinterpret_cast<uint64_t>(&camera)},
        {"view.extents",getCamera,setCamera,reinterpret_cast<uint64_t>(&camera)},
        {"model.extents",getCamera,nullptr,reinterpret_cast<uint64_t>(&camera)},
        {"motion",nullptr,setCamera,reinterpret_cast<uint64_t>(&camera)},
        {"commands.activeCommand",nullptr,setCamera,reinterpret_cast<uint64_t>(&camera)},
        {"transaction",nullptr,setCamera,reinterpret_cast<uint64_t>(&camera)}};
    navlib::nlCreateOptions_t options{sizeof(options),false,navlib::row_major_order};navlib::nlHandle_t handle;
    CHECK(create(&handle,"mock-fusion",accessors,std::size(accessors),&options)==0);
    // Presence must follow hardware events instead of always reporting true.
    navlib::value_t present;CHECK(read(handle,"device.present",&present)==0);CHECK(!present.b);
    e.kind=sn::Kind::added;CHECK(sn::writeAll(inject,&e,sizeof(e)));
    CHECK(waitFor([&]{return read(handle,"device.present",&present)==0&&present.b;}));
    e.kind=sn::Kind::removed;CHECK(sn::writeAll(inject,&e,sizeof(e)));
    CHECK(waitFor([&]{return read(handle,"device.present",&present)==0&&!present.b;}));
    e.kind=sn::Kind::added;CHECK(sn::writeAll(inject,&e,sizeof(e)));
    CHECK(waitFor([&]{return read(handle,"device.present",&present)==0&&present.b;}));
    navlib::value_t timing(long(1));CHECK(write(handle,"frame.timingSource",&timing)==0);
    CHECK(waitFor([]{return clients(2);}));
    e.kind=sn::Kind::motion;e.axes={350,0,0,0,0,0};e.received=sn::now();CHECK(sn::writeAll(inject,&e,sizeof(e)));CHECK(waitFor([&]{return camera.motion==1;}));
    // Callbacks run on the thread that created the session.
    CHECK(camera.thread==GetCurrentThreadId());
    navlib::value_t time(100.0);CHECK(write(handle,"frame.time",&time)==0);CHECK(camera.frames>0);CHECK(camera.matrix.m03>0);CHECK(camera.transactions==0);
    camera.perspective=false;e.axes={0,350,0,0,0,0};e.received=sn::now();CHECK(sn::writeAll(inject,&e,sizeof(e)));pump(20);
    CHECK(waitFor([&]{time.d+=16;CHECK(write(handle,"frame.time",&time)==0);return camera.extents.max.x>10;}));
    auto validExtents=camera.extents;int validFrames=camera.frames;
    camera.extents.max.x=std::numeric_limits<double>::infinity();time.d+=16;
    CHECK(write(handle,"frame.time",&time)==0);CHECK(camera.frames==validFrames);CHECK(camera.transactions==0);camera.extents=validExtents;
    e.axes={};CHECK(sn::writeAll(inject,&e,sizeof(e)));CHECK(waitFor([&]{return camera.motion==0;}));
    int frames=camera.frames;time=132.0;CHECK(write(handle,"frame.time",&time)==0);CHECK(camera.frames==frames);
    navlib::value_t invalid(1.0);CHECK(write(handle,"active",&invalid)!=0);CHECK(read(handle,"unknown",&invalid)!=0);
    e.kind=sn::Kind::command;e.flags=0x10000;e.received=sn::now();double beforeFit=camera.matrix.m23;
    CHECK(sn::writeAll(inject,&e,sizeof(e)));CHECK(waitFor([&]{return camera.matrix.m23!=beforeFit;}));CHECK(camera.transactions==0);
    CHECK(close(handle)==0);CHECK(close(handle)!=0);
    CHECK(waitFor([]{return clients(1);}));
    // Per-device bindings and edge state; neutral motion must not clear a held button.
    CHECK(sn::request(R"({"op":"setConfig","config":{"version":1,"profiles":{"*@046d:c627":{"buttons":[{"command":"first-device"}]},"*@256f:c635":{"buttons":[{"command":"second-device"}]}}}})").find("\"ok\":true")!=std::string::npos);
    CHECK(create(&handle,"multi-device",accessors,std::size(accessors),&options)==0);
    CHECK(write(handle,"frame.timingSource",&timing)==0);
    e.kind=sn::Kind::added;e.flags=0;e.buttons=0;e.axes={};CHECK(sn::writeAll(inject,&e,sizeof(e)));
    another=e;another.device=17;another.vendor=0x256f;another.product=0xc635;CHECK(sn::writeAll(inject,&another,sizeof(another)));
    auto press=[&](sn::Event event,uint32_t buttons){event.kind=sn::Kind::buttons;event.buttons=buttons;CHECK(sn::writeAll(inject,&event,sizeof(event)));};
    CHECK(waitFor([&]{
        press(e,0);press(another,0);press(e,1);press(another,1);
        return std::find(camera.commands.begin(),camera.commands.end(),"first-device")!=camera.commands.end()&&std::find(camera.commands.begin(),camera.commands.end(),"second-device")!=camera.commands.end();
    }));
    // Drain the readiness probes, then start with both devices released.
    press(e,0);press(another,0);pump(50);camera.commands.clear();
    press(e,1);press(another,1);CHECK(waitFor([&]{return camera.commands.size()==2;}));
    CHECK(camera.commands[0]=="first-device"&&camera.commands[1]=="second-device");
    e.kind=sn::Kind::motion;e.axes={};CHECK(sn::writeAll(inject,&e,sizeof(e)));press(e,1);
    pump(30);CHECK(camera.commands.size()==2);
    press(e,0);press(another,0);CHECK(waitFor([&]{return camera.commands.size()==4;}));
    CHECK(camera.commands[2].empty()&&camera.commands[3].empty());
    another.kind=sn::Kind::removed;CHECK(sn::writeAll(inject,&another,sizeof(another)));
    CHECK(close(handle)==0);
    CHECK(waitFor([]{return clients(1);}));
    CHECK(sn::request(config).find("\"ok\":true")!=std::string::npos);
    camera.matrix={};camera.matrix.m00=camera.matrix.m11=camera.matrix.m22=camera.matrix.m33=1;camera.matrix.m32=10;
    options.options=navlib::none;
    CHECK(create(&handle,"column-major",accessors,std::size(accessors),&options)==0);
    CHECK(write(handle,"frame.timingSource",&timing)==0);
    CHECK(waitFor([]{return clients(2);}));
    e.kind=sn::Kind::motion;e.flags=sn::Flags::orbit;e.axes={350,0,0,0,0,0};e.received=sn::now();CHECK(sn::writeAll(inject,&e,sizeof(e)));CHECK(waitFor([&]{return camera.motion==1;}));
    time=100.0;CHECK(write(handle,"frame.time",&time)==0);CHECK(camera.matrix.m30>0);CHECK(camera.matrix.m03==0);CHECK(close(handle)==0);
    CHECK(waitFor([]{return clients(1);}));
    CHECK(create(&handle,"automatic-clock",accessors,std::size(accessors),&options)==0);
    CHECK(waitFor([]{return clients(2);}));
    int automaticFrames=camera.frames;e.received=sn::now();CHECK(sn::writeAll(inject,&e,sizeof(e)));
    CHECK(waitFor([&]{return camera.frames>automaticFrames+1;}));
    CHECK(waitFor([&]{return camera.motion==0;})); // stale cap input stops the clock
    automaticFrames=camera.frames;pump(50);CHECK(camera.frames==automaticFrames);
    CHECK(close(handle)==0);
    CHECK(waitFor([]{return clients(1);}));
    // Multithreaded sessions run callbacks on a private thread, without the
    // client pumping messages.
    options.bMultiThreaded=true;camera.thread=0;
    CHECK(create(&handle,"multithreaded",accessors,std::size(accessors),&options)==0);
    for(int i=0;i<300&&!clients(2);++i)Sleep(10);
    CHECK(clients(2));
    automaticFrames=camera.frames;e.received=sn::now();CHECK(sn::writeAll(inject,&e,sizeof(e)));
    for(int i=0;i<300&&camera.frames<=automaticFrames+1;++i)Sleep(10);
    CHECK(camera.frames>automaticFrames+1);CHECK(camera.thread!=0&&camera.thread!=GetCurrentThreadId());
    for(int i=0;i<300&&camera.motion!=0;++i)Sleep(10);
    CHECK(camera.motion==0);
    navlib::value_t moving;CHECK(read(handle,"motion",&moving)==0);CHECK(!moving.b);
    CHECK(close(handle)==0);options.bMultiThreaded=false;
    CHECK(waitFor([]{return clients(1);}));
    CHECK(create(&handle,"reentrant-close",accessors,std::size(accessors),&options)==0);
    CHECK(write(handle,"frame.timingSource",&timing)==0);
    CHECK(waitFor([]{return clients(2);}));
    camera.handle=handle;camera.closeDuringGet=close;e.received=sn::now();CHECK(sn::writeAll(inject,&e,sizeof(e)));CHECK(waitFor([&]{return camera.motion==1;}));
    time=100.0;CHECK(write(handle,"frame.time",&time)==0);CHECK(camera.wasClosed);CHECK(close(handle)!=0);
    CHECK(FreeLibrary(library));pump(50);
    // Slow clients cannot grow memory without bound or silently lose button edges.
    // Fill a monitor's receive buffers without reading. The service must disconnect it.
    sn::Socket slow=sn::openEvents(sn::Flags::monitor);CHECK(slow!=INVALID_SOCKET);
    int smallBuffer=1024;setsockopt(slow,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&smallBuffer),sizeof(smallBuffer));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    e.kind=sn::Kind::buttons;bool overflowed=false;
    for(int round=0;round<16&&!overflowed;++round){
        for(int i=0;i<4096;++i){e.buttons=i%2;e.received=sn::now();CHECK(sn::writeAll(inject,&e,sizeof(e)));}
        overflowed=waitFor([]{return sn::request("{\"op\":\"status\"}").find("\"overflows\":0")==std::string::npos;},20);
    }
    CHECK(overflowed);
    sn::closeSocket(slow);
    sn::Socket fragmented=sn::connectSocket();CHECK(fragmented!=INVALID_SOCKET);sn::Event hello;hello.kind=sn::Kind::hello;hello.flags=sn::Flags::monitor;
    CHECK(sn::writeAll(fragmented,&hello,7));CHECK(sn::writeAll(fragmented,reinterpret_cast<char*>(&hello)+7,sizeof(hello)-7));
    sn::Event event;CHECK(sn::readAll(fragmented,&event,sizeof(event)));CHECK(event.kind==sn::Kind::added);
    e.kind=sn::Kind::motion;e.axes={-123,0,0,0,0,0};CHECK(sn::writeAll(inject,&e,sizeof(e)));
    do CHECK(sn::readAll(fragmented,&event,sizeof(event)));while(event.kind!=sn::Kind::motion);
    CHECK(event.axes[0]==-123);
    sn::closeSocket(fragmented);
    // Zero calibration: the current deflection becomes the device's rest
    // position, is applied to later reports (clamped), and can be cleared.
    sn::Socket zero=sn::openEvents(sn::Flags::monitor);CHECK(zero!=INVALID_SOCKET);
    auto nextMotion=[&]{do CHECK(sn::readAll(zero,&event,sizeof(event)));while(event.kind!=sn::Kind::motion||event.device!=e.device);return event.axes;};
    using Axes=std::array<int16_t,6>;
    e.axes={12,-7,0,0,0,30};CHECK(sn::writeAll(inject,&e,sizeof(e)));CHECK((nextMotion()==Axes{12,-7,0,0,0,30}));
    CHECK(sn::request(R"({"op":"calibrate","clear":1})").find("error")!=std::string::npos);
    CHECK(sn::request(R"({"op":"calibrate","device":-1})").find("error")!=std::string::npos);
    CHECK(sn::request(R"({"op":"calibrate"})").find("\"ok\":true")!=std::string::npos);
    CHECK((nextMotion()==Axes{}));
    CHECK(sn::request("{\"op\":\"status\"}").find("\"calibrated\":true")!=std::string::npos);
    e.axes={20,-7,0,0,0,-32768};CHECK(sn::writeAll(inject,&e,sizeof(e)));CHECK((nextMotion()==Axes{8,0,0,0,0,-32768}));
    CHECK(sn::request(R"({"op":"calibrate","clear":true})").find("\"ok\":true")!=std::string::npos);
    CHECK((nextMotion()==Axes{20,-7,0,0,0,-32768}));
    e.axes={};CHECK(sn::writeAll(inject,&e,sizeof(e)));CHECK((nextMotion()==Axes{}));
    CHECK(sn::request("{\"op\":\"status\"}").find("\"calibrated\":true")==std::string::npos);
    sn::closeSocket(zero);
    // Exercise socket reuse with short-lived clients, as when an app restarts.
    for(int i=0;i<200;++i){
        sn::Socket peer=sn::openEvents(sn::Flags::monitor);CHECK(peer!=INVALID_SOCKET);
        CHECK(sn::readAll(peer,&event,sizeof(event)));CHECK(event.kind==sn::Kind::added);sn::closeSocket(peer);
    }
    CHECK(sn::request("{\"op\":\"status\"}").find("\"mock\":true")!=std::string::npos);
    // The native test tab receives full event edges, independently of its UI
    // refresh. Short press/release pairs must both reach the session log.
    AxialPreviewStart();
    CHECK(waitFor([]{return clients(2);}));
    e.kind=sn::Kind::buttons;e.buttons=0;e.received=sn::now();CHECK(sn::writeAll(inject,&e,sizeof(e)));
    e.buttons=5;CHECK(sn::writeAll(inject,&e,sizeof(e)));e.buttons=0;CHECK(sn::writeAll(inject,&e,sizeof(e)));
    e.kind=sn::Kind::motion;e.axes={321,-12,0,0,0,0};
    // Preview motion expires after 250 ms. Refresh its event timestamp while
    // waiting so a slow CI runner cannot make the sample stale before reading it.
    bool previewMotion=false;
    for(int attempt=0;attempt<10&&!previewMotion;++attempt){
        e.received=sn::now();CHECK(sn::writeAll(inject,&e,sizeof(e)));
        previewMotion=waitFor([]{double axes[6]{};uint32_t buttons=0;return AxialPreviewRead(1,axes,&buttons)&&axes[0]==321&&axes[1]==-12&&buttons==0;},30);
    }
    CHECK(previewMotion);
    uint64_t timestamp=0;uint32_t deviceID=0,changed=0,buttons=0,reason=0,identity=0,presses=0,releases=0;
    while(AxialPreviewPopLog(&timestamp,&deviceID,&changed,&buttons,&reason,&identity))if(deviceID==1&&reason==uint32_t(sn::Kind::buttons)){CHECK(identity==0x046dc627);presses|=changed&buttons;releases|=changed&~buttons;}
    CHECK((presses&5)==5&&(releases&5)==5);CHECK(AxialPreviewLostLogs()==0);
    // Queued edges keep their hardware identity after unplug and numeric ID reuse.
    press(e,1u<<13);
    auto removal=e;removal.kind=sn::Kind::removed;removal.vendor=0;removal.product=0;
    CHECK(sn::writeAll(inject,&removal,sizeof(removal)));
    auto replacement=e;replacement.kind=sn::Kind::added;replacement.vendor=0x256f;replacement.product=0xc635;replacement.buttons=0;
    CHECK(sn::writeAll(inject,&replacement,sizeof(replacement)));press(replacement,2);
    CHECK(sn::writeAll(inject,&removal,sizeof(removal)));
    auto barrier=replacement;barrier.device=17;barrier.kind=sn::Kind::motion;barrier.axes={233,0,0,0,0,0};barrier.received=sn::now();
    CHECK(sn::writeAll(inject,&barrier,sizeof(barrier)));
    CHECK(waitFor([]{double axes[6]{};uint32_t buttons=0;return AxialPreviewRead(17,axes,&buttons)&&axes[0]==233;}));
    struct Edge {uint32_t identity,changed,buttons,reason;};
    const Edge expectedEdges[]={
        {0x046dc627,1u<<13,1u<<13,uint32_t(sn::Kind::buttons)},
        {0x046dc627,1u<<13,0,uint32_t(sn::Kind::removed)},
        {0x256fc635,2,2,uint32_t(sn::Kind::buttons)},
        {0x256fc635,2,0,uint32_t(sn::Kind::removed)}
    };
    size_t edge=0;
    while(AxialPreviewPopLog(&timestamp,&deviceID,&changed,&buttons,&reason,&identity))if(deviceID==1){
        CHECK(edge<std::size(expectedEdges));const auto& expected=expectedEdges[edge++];
        CHECK(identity==expected.identity&&changed==expected.changed&&buttons==expected.buttons&&reason==expected.reason);
    }
    CHECK(edge==std::size(expectedEdges)&&AxialPreviewLostLogs()==0);
    AxialPreviewStop();
    std::thread starter([]{for(int i=0;i<30;++i){AxialPreviewStart();AxialPreviewStop();}});
    for(int i=0;i<30;++i){AxialPreviewStart();AxialPreviewStop();}starter.join();AxialPreviewStop();
    // Invalid command catalogs must not break the app's response parsing.
    for(const char* bad:{R"({"op":"commands","app":"mock","commands":[1]})",R"({"op":"commands","app":"mock","commands":[{"id":4,"label":"bad"}]})"})CHECK(sn::request(bad).find("error")!=std::string::npos);
    // An incomplete control frame must not mutate settings, even if its JSON is valid.
    sn::Socket incomplete=sn::connectSocket(true);CHECK(incomplete!=INVALID_SOCKET);
    std::string command=R"({"op":"commands","app":"incomplete","commands":[]})";
    CHECK(sn::writeAll(incomplete,command.data(),command.size()));shutdown(incomplete,SD_SEND);
    char response[256];recv(incomplete,response,sizeof(response),0);sn::closeSocket(incomplete);
    CHECK(sn::request("{\"op\":\"getCommands\"}").find("incomplete")==std::string::npos);
    // Accepted command data must always fit in a readable getCommands response.
    std::string entries;
    for(int i=0;i<128;++i){if(i)entries+=',';entries+="{\"id\":\""+std::to_string(i)+"\",\"label\":\""+std::string(1024,'x')+"\"}";}
    bool bounded=false;
    for(int i=0;i<12;++i){auto result=sn::request("{\"op\":\"commands\",\"app\":\"large-"+std::to_string(i)+"\",\"commands\":["+entries+"]}");if(result.find("response size limit")!=std::string::npos){bounded=true;break;}}
    CHECK(bounded);auto catalog=sn::request("{\"op\":\"getCommands\"}");CHECK(catalog.size()<1024*1024);CHECK(catalog.find("large-0")!=std::string::npos);
    sn::closeSocket(inject);
    std::cout<<"Integration: real IPC, siappdll window messages and polling, profiles, Navlib camera/zoom/transactions/neutral, multithreaded sessions and preview passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
