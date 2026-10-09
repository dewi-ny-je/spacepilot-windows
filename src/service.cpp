#include "axial/transport.hpp"
#include "axial/keys.hpp"
#include "axial/application.hpp"
#include "axial/web.hpp"
#include "loop.hpp"
#include <boost/json.hpp>
// Older mingw-w64 headers lack C linkage guards; the Windows SDK ones have them.
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <setupapi.h>
#include <dbt.h>
#include <psapi.h>
#include <aclapi.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

using namespace sn;
namespace json=boost::json;
namespace {
enum class Action : uint8_t {none,dominant,translation,rotation,faster,slower,fit};
// Modifier bits stored with a recorded shortcut (Windows virtual-key based).
enum Modifier : uint64_t {shiftKey=1,controlKey=2,altKey=4,windowsKey=8};
struct Profile {
    Settings motion;
    bool led=true;
    std::array<int,32> keys;
    std::array<uint64_t,32> modifiers{};
    std::array<Action,32> actions{};
    Profile(){keys.fill(-1);}
};
struct Configuration {std::map<std::string,Profile> profiles;WebConfiguration web;};
std::unique_ptr<WebServer> webServer;
struct Peer {
    Socket fd=INVALID_SOCKET;
    HANDLE event=nullptr;
    DWORD pid=0;
    sn::ProcessIdentity identity;
    bool foreground=false;
    bool registered=false,observe=false,injecting=false;
    Event incoming{};
    size_t readOffset=0,writeOffset=0;
    EventQueue<256> queue;
};
struct LEDOutput {
    HANDLE file;
    PHIDP_PREPARSED_DATA preparsed;
    UCHAR report;USHORT collection;USHORT length;
    std::atomic<bool> connected{true};
    std::atomic<int> state{-1};
    std::atomic<uint32_t> error{0};
    LEDOutput(HANDLE f,PHIDP_PREPARSED_DATA p,UCHAR r,USHORT c,USHORT l):report(r),collection(c),length(l){
        DuplicateHandle(GetCurrentProcess(),f,GetCurrentProcess(),&file,0,FALSE,DUPLICATE_SAME_ACCESS);preparsed=p;
    }
    ~LEDOutput(){CloseHandle(file);HidD_FreePreparsedData(preparsed);}
};
struct HID {
    HANDLE file=INVALID_HANDLE_VALUE;
    std::wstring path;
    OVERLAPPED overlapped{};
    std::vector<uint8_t> buffer;
    bool sixAxisReport=false;
    Decoder decoder;
    Profile profile;
    uint32_t previousButtons=0;
    uint32_t suppressedButtons=0;
    std::shared_ptr<LEDOutput> led;
    int requestedLED=-1;
    bool active() const {return file!=INVALID_HANDLE_VALUE;}
};
Loop loop;
std::array<Peer,32> peers;
std::array<HID,16> hidDevices;
std::array<Event,16> virtualDevices{};
std::array<uint32_t,16> virtualButtonState{};
std::unique_ptr<Configuration> configuration;
Profile activeProfile;
std::string foregroundApp;
DWORD foregroundPID=0;
sn::ProcessIdentity foregroundIdentity;
std::string serviceUser;
void resolveForeground(Peer& peer) {
    peer.foreground=sn::belongsToApplication(peer.identity,foregroundIdentity,sn::processIdentity);
}
bool mockMode=false;
uint64_t reports=0,overflows=0,rejected=0;
uint32_t nextDevice=1;
std::string settingsPath;
json::value document;
json::object commandCatalog;
std::mutex documentMutex;

// Serial worker used for key injection and LED output, mirroring the
// independent macOS dispatch queues: neither may delay HID decoding.
class Worker {
    std::deque<std::function<void()>> work;
    std::mutex mutex;std::condition_variable ready;
    bool running=true;std::thread thread;
public:
    Worker(){thread=std::thread([this]{
        for(;;){std::function<void()> item;
            {std::unique_lock lock(mutex);ready.wait(lock,[&]{return !work.empty()||!running;});if(work.empty())return;item=std::move(work.front());work.pop_front();}
            item();
        }});}
    void post(std::function<void()> item){{std::lock_guard lock(mutex);work.push_back(std::move(item));}ready.notify_one();}
    void drain(){std::promise<void> done;auto future=done.get_future();post([&]{done.set_value();});future.wait();}
    void detach(){thread.detach();}
};
Worker* keyWorker;
Worker* outputWorker;
struct KeyWork {uint32_t device=0,buttons=0;bool release=false;std::array<int,32> keys{};std::array<uint64_t,32> modifiers{};};
std::array<KeyWork,128> keyWork;
std::atomic<size_t> keyRead{0},keyWrite{0};
std::atomic<bool> keyOverflow{false};
KeyState heldKeys;

void setLED(HID& hid,bool enabled) {
    if(!hid.led||hid.requestedLED==int(enabled))return;
    hid.requestedLED=enabled;auto output=hid.led;
    outputWorker->post([output,enabled]{
        if(!output->connected.load())return;
        std::vector<char> report(output->length,0);report[0]=char(output->report);
        USAGE usage=0x4b;ULONG count=1;NTSTATUS status=HIDP_STATUS_SUCCESS;
        if(enabled)status=HidP_SetUsages(HidP_Output,0x08,output->collection,&usage,&count,output->preparsed,report.data(),ULONG(report.size()));
        if(status!=HIDP_STATUS_SUCCESS){output->error=uint32_t(status);output->state=-1;return;}
        bool ok=HidD_SetOutputReport(output->file,report.data(),ULONG(report.size()));
        if(!ok){
            // Some firmware accepts only interrupt OUT transfers.
            OVERLAPPED overlapped{};overlapped.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);DWORD written=0;
            ok=WriteFile(output->file,report.data(),DWORD(report.size()),nullptr,&overlapped)||GetLastError()==ERROR_IO_PENDING;
            if(ok&&WaitForSingleObject(overlapped.hEvent,500)!=WAIT_OBJECT_0){CancelIoEx(output->file,&overlapped);ok=false;}
            if(ok)ok=GetOverlappedResult(output->file,&overlapped,&written,TRUE);else GetOverlappedResult(output->file,&overlapped,&written,TRUE);
            CloseHandle(overlapped.hEvent);
        }
        output->error=ok?0:GetLastError();output->state=ok?int(enabled):-1;
    });
}

bool extendedKey(int key) {
    switch(key){
        case VK_INSERT:case VK_DELETE:case VK_HOME:case VK_END:case VK_PRIOR:case VK_NEXT:
        case VK_LEFT:case VK_RIGHT:case VK_UP:case VK_DOWN:case VK_NUMLOCK:case VK_DIVIDE:
        case VK_RCONTROL:case VK_RMENU:case VK_LWIN:case VK_RWIN:case VK_APPS:case VK_SNAPSHOT:return true;
        default:return false;
    }
}
INPUT keyInput(int key,bool down) {
    INPUT input{};input.type=INPUT_KEYBOARD;input.ki.wVk=WORD(key);
    input.ki.wScan=WORD(MapVirtualKeyW(UINT(key),MAPVK_VK_TO_VSC));
    input.ki.dwFlags=(down?0:KEYEVENTF_KEYUP)|(extendedKey(key)?KEYEVENTF_EXTENDEDKEY:0);
    return input;
}
void postKey(int key,bool down,uint64_t modifiers){
    const std::pair<uint64_t,int> modifierKeys[]={{controlKey,VK_CONTROL},{altKey,VK_MENU},{shiftKey,VK_SHIFT},{windowsKey,VK_LWIN}};
    std::vector<INPUT> inputs;
    if(down){for(auto [bit,vk]:modifierKeys)if(modifiers&bit)inputs.push_back(keyInput(vk,true));inputs.push_back(keyInput(key,true));}
    else{inputs.push_back(keyInput(key,false));for(auto it=std::rbegin(modifierKeys);it!=std::rend(modifierKeys);++it)if(modifiers&it->first)inputs.push_back(keyInput(it->second,false));}
    SendInput(UINT(inputs.size()),inputs.data(),sizeof(INPUT));
}
void clearHeld(uint32_t device=0){heldKeys.release(device,postKey);}
void drainKeys(){
    bool overflowed=keyOverflow.exchange(false);
    if(overflowed)clearHeld();
    size_t r=keyRead.load(std::memory_order_relaxed);
    while(r!=keyWrite.load(std::memory_order_acquire)){
        KeyWork work=keyWork[r%keyWork.size()];keyRead.store(++r,std::memory_order_release);
        if(work.release){clearHeld(work.device);continue;}
        heldKeys.update(work.device,work.buttons,work.keys,work.modifiers,postKey);
    }
    if(keyOverflow.exchange(false)||overflowed)clearHeld();
}
void submitKeyWork(const KeyWork& work){
    size_t w=keyWrite.load(std::memory_order_relaxed);
    if(w-keyRead.load(std::memory_order_acquire)==keyWork.size()){keyOverflow=true;++overflows;}
    else{keyWork[w%keyWork.size()]=work;keyWrite.store(w+1,std::memory_order_release);}
    keyWorker->post(drainKeys);
}
void releaseKeys(uint32_t device=0){KeyWork work;work.release=true;work.device=device;submitKeyWork(work);}

void closePeer(Peer& p) {
    if(p.fd==INVALID_SOCKET)return;
    loop.remove(p.event);WSACloseEvent(p.event);closesocket(p.fd);
    p.fd=INVALID_SOCKET;p.event=nullptr;p.queue.clear();p.writeOffset=0;p.readOffset=0;
}
void flush(Peer& p) {
    while(p.fd!=INVALID_SOCKET&&!p.queue.empty()) {
        const char* bytes=reinterpret_cast<const char*>(&p.queue.front());
        int n=send(p.fd,bytes+p.writeOffset,int(sizeof(Event)-p.writeOffset),0);
        if(n<0&&socketError()==WSAEWOULDBLOCK)break; // FD_WRITE resumes the queue.
        if(n<0&&socketError()==WSAEINTR)continue;
        if(n<=0){closePeer(p);return;}
        p.writeOffset+=size_t(n);
        if(p.writeOffset==sizeof(Event)){p.queue.pop();p.writeOffset=0;}
    }
}
void enqueue(Peer& p,const Event& e) {
    // Never replace a partly written front frame: a 64-byte short write is
    // exceptionally rare. Disconnect rather than corrupt an ABI frame.
    if(p.writeOffset&&p.queue.size()==1){++overflows;closePeer(p);return;}
    if(!p.queue.push(e)){++overflows;closePeer(p);return;}
    flush(p);
}
void route(const Event& raw) {
    ++reports;
    Profile* selected=&activeProfile;
    uint32_t suppressed=0;
    uint32_t ignoredPrevious=0;uint32_t* previous=&ignoredPrevious;
    if(mockMode)for(size_t i=0;i<virtualDevices.size();++i)if(virtualDevices[i].device==raw.device){previous=&virtualButtonState[i];break;}
    for(auto& h:hidDevices)if(h.active()&&h.decoder.state.device==raw.device){selected=&h.profile;previous=&h.previousButtons;if(raw.kind==Kind::buttons)h.suppressedButtons&=raw.buttons;suppressed=h.suppressedButtons;break;}
    if(raw.kind==Kind::buttons){
        uint32_t pressed=raw.buttons&~*previous;*previous=raw.buttons;
        for(int i=0;i<32;++i)if(pressed&(1u<<i))switch(selected->actions[i]){
            case Action::dominant:selected->motion.dominant=!selected->motion.dominant;break;
            case Action::translation:selected->motion.translation=!selected->motion.translation;break;
            case Action::rotation:selected->motion.rotation=!selected->motion.rotation;break;
            case Action::faster:for(auto& gain:selected->motion.gain)gain=std::min(20.f,gain*1.25f);break;
            case Action::slower:for(auto& gain:selected->motion.gain)gain=std::max(.01f,gain/1.25f);break;
            case Action::fit:{Event command=raw;command.kind=Kind::command;command.flags=0x10000;
                if(webServer)webServer->receive(command);
                for(auto& p:peers)if(p.fd!=INVALID_SOCKET&&p.registered&&!p.injecting&&!p.observe&&(mockMode||p.foreground))enqueue(p,command);break;}
            default:break;
        }
    }
    Event e=filter(raw,selected->motion);
    e.buttons&=~suppressed;
    if(webServer)webServer->receive(e);
    for(auto& p:peers)if(p.fd!=INVALID_SOCKET&&p.registered&&!p.injecting){
        if(p.observe)enqueue(p,raw);
        else if(mockMode||p.foreground||raw.kind==Kind::added||raw.kind==Kind::removed||raw.kind==Kind::reset)enqueue(p,e);
    }
    if(raw.kind==Kind::buttons&&!mockMode){
        KeyWork work;work.device=raw.device;work.buttons=e.buttons;work.keys=selected->keys;work.modifiers=selected->modifiers;submitKeyWork(work);
    }
    if(raw.kind==Kind::removed||raw.kind==Kind::reset)releaseKeys(raw.device);
}
void resetFocus(){for(auto& h:hidDevices)if(h.active())h.suppressedButtons=h.decoder.state.buttons;Event e;e.kind=Kind::reset;e.received=now();route(e);}
void selectProfile() {
    activeProfile=Profile{};
    if(!configuration)return;
    auto it=configuration->profiles.find(foregroundApp);
    if(it==configuration->profiles.end())it=configuration->profiles.find("*");
    if(it!=configuration->profiles.end())activeProfile=it->second;
    for(auto& h:hidDevices)if(h.active()){
        h.profile=activeProfile;
        char suffix[16];snprintf(suffix,sizeof(suffix),"@%04x:%04x",h.decoder.state.vendor,h.decoder.state.product);
        auto device=configuration->profiles.find(foregroundApp+suffix);
        if(device==configuration->profiles.end())device=configuration->profiles.find(std::string("*")+suffix);
        if(device!=configuration->profiles.end())h.profile=device->second;
        setLED(h,h.profile.led);
    }
}

// ---- HID input ---------------------------------------------------------
void readNext(HID& h);
void removeDevice(HID& h) {
    if(!h.active())return;
    CancelIoEx(h.file,&h.overlapped);DWORD ignored=0;GetOverlappedResult(h.file,&h.overlapped,&ignored,TRUE);
    loop.remove(h.overlapped.hEvent);
    Event e=h.decoder.state;e.axes={};e.buttons=0;e.kind=Kind::removed;e.received=now();route(e);
    if(h.led)h.led->connected=false;
    h.led.reset();CloseHandle(h.overlapped.hEvent);CloseHandle(h.file);h.file=INVALID_HANDLE_VALUE;h.path.clear();
}
// Windows pads every input report to the longest report of the collection.
// Restore each report's own length, as delivered by IOKit on macOS.
size_t reportLength(const HID& h,size_t length) {
    if(!length)return 0;
    switch(h.buffer[0]){
        case 1:return std::min(length,size_t(h.sixAxisReport?13:7));
        case 2:return std::min(length,size_t(7));
        case 3:return std::min(length,size_t(5));
        case 0x1c:case 0x1d:return std::min(length,size_t(13));
        default:return length;
    }
}
void completed(HID& h) {
    DWORD length=0;
    if(!GetOverlappedResult(h.file,&h.overlapped,&length,FALSE)){
        DWORD error=GetLastError();if(error==ERROR_IO_INCOMPLETE)return;
        removeDevice(h);return;
    }
    uint64_t begin=now();Event e;
    std::span<const uint8_t> report(h.buffer.data(),reportLength(h,length));
    if(h.decoder.decode(report,begin,e)){e.decoded=now();route(e);}else ++rejected;
    if(h.active())readNext(h);
}
void readNext(HID& h) {
    for(;;){
        ResetEvent(h.overlapped.hEvent);
        if(ReadFile(h.file,h.buffer.data(),DWORD(h.buffer.size()),nullptr,&h.overlapped)){
            // Completed synchronously: the event is set, the loop picks it up.
            return;
        }
        DWORD error=GetLastError();
        if(error==ERROR_IO_PENDING)return;
        removeDevice(h);return;
    }
}
std::vector<std::wstring> hidPaths() {
    std::vector<std::wstring> paths;GUID guid;HidD_GetHidGuid(&guid);
    HDEVINFO info=SetupDiGetClassDevsW(&guid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(info==INVALID_HANDLE_VALUE)return paths;
    SP_DEVICE_INTERFACE_DATA data{};data.cbSize=sizeof(data);
    for(DWORD i=0;SetupDiEnumDeviceInterfaces(info,nullptr,&guid,i,&data);++i){
        DWORD size=0;SetupDiGetDeviceInterfaceDetailW(info,&data,nullptr,0,&size,nullptr);
        if(!size)continue;
        std::vector<char> storage(size);auto detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize=sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if(SetupDiGetDeviceInterfaceDetailW(info,&data,detail,size,nullptr,nullptr))paths.emplace_back(detail->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(info);return paths;
}
void openDevice(const std::wstring& path) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if(file==INVALID_HANDLE_VALUE)file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if(file==INVALID_HANDLE_VALUE)return;
    HIDD_ATTRIBUTES attributes{};attributes.Size=sizeof(attributes);
    PHIDP_PREPARSED_DATA preparsed=nullptr;HIDP_CAPS caps{};
    bool ok=HidD_GetAttributes(file,&attributes)&&deviceSpec(attributes.VendorID,attributes.ProductID)&&
        HidD_GetPreparsedData(file,&preparsed)&&HidP_GetCaps(preparsed,&caps)==HIDP_STATUS_SUCCESS&&
        caps.UsagePage==1&&caps.Usage==8&&caps.InputReportByteLength>=7;
    if(!ok||loop.capacity()==0){if(preparsed)HidD_FreePreparsedData(preparsed);CloseHandle(file);return;}
    for(auto& h:hidDevices)if(!h.active()){
        h=HID{};h.file=file;h.path=path;h.buffer.assign(caps.InputReportByteLength,0);
        h.overlapped.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        // Report 1 carries all six axes when it contains Rx (usage 0x33).
        std::vector<HIDP_VALUE_CAPS> values(caps.NumberInputValueCaps);USHORT count=caps.NumberInputValueCaps;
        if(count&&HidP_GetValueCaps(HidP_Input,values.data(),&count,preparsed)==HIDP_STATUS_SUCCESS)
            for(USHORT i=0;i<count;++i){const auto& v=values[i];
                USAGE low=v.IsRange?v.Range.UsageMin:v.NotRange.Usage,high=v.IsRange?v.Range.UsageMax:v.NotRange.Usage;
                if(v.ReportID==1&&v.UsagePage==1&&low<=0x33&&high>=0x33)h.sixAxisReport=true;
            }
        // LED: generic indicator usage (page 8, usage 0x4b) in an output report.
        std::vector<HIDP_BUTTON_CAPS> buttons(caps.NumberOutputButtonCaps);count=caps.NumberOutputButtonCaps;
        bool keepPreparsed=false;
        if(count&&HidP_GetButtonCaps(HidP_Output,buttons.data(),&count,preparsed)==HIDP_STATUS_SUCCESS)
            for(USHORT i=0;i<count;++i){const auto& b=buttons[i];
                USAGE low=b.IsRange?b.Range.UsageMin:b.NotRange.Usage,high=b.IsRange?b.Range.UsageMax:b.NotRange.Usage;
                if(b.UsagePage==8&&low<=0x4b&&high>=0x4b){h.led=std::make_shared<LEDOutput>(file,preparsed,b.ReportID,b.LinkCollection,caps.OutputReportByteLength);keepPreparsed=true;break;}
            }
        if(!keepPreparsed)HidD_FreePreparsedData(preparsed);
        auto& e=h.decoder.state;e.device=nextDevice++;e.vendor=attributes.VendorID;e.product=attributes.ProductID;
        HID* self=&h;loop.add(h.overlapped.hEvent,[self]{completed(*self);});
        selectProfile();
        Event connected=e;connected.kind=Kind::added;connected.received=now();route(connected);
        readNext(h);return;
    }
    HidD_FreePreparsedData(preparsed);CloseHandle(file);
}
void scanDevices() {
    if(mockMode)return;
    auto paths=hidPaths();
    for(auto& h:hidDevices)if(h.active()&&std::find(paths.begin(),paths.end(),h.path)==paths.end())removeDevice(h);
    for(const auto& path:paths){
        bool known=std::any_of(hidDevices.begin(),hidDevices.end(),[&](const HID& h){return h.active()&&h.path==path;});
        if(!known)openDevice(path);
    }
}

// ---- Event socket ------------------------------------------------------
std::string userSID(){HANDLE self=GetCurrentProcess();return sn::processUser(self);}
bool secureDirectory(const std::string& directory) {
    // Owner-only access for the socket directory created by the service.
    std::wstring path=wide(directory);
    if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES){
        std::wstring sddl=L"D:P(A;OICI;FA;;;"+wide(serviceUser)+L")(A;OICI;FA;;;SY)";
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))return false;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
        std::error_code ignored;std::filesystem::create_directories(std::filesystem::path(path).parent_path(),ignored);
        bool created=CreateDirectoryW(path.c_str(),&attributes)||GetLastError()==ERROR_ALREADY_EXISTS;
        LocalFree(descriptor);if(!created)return false;
    }
    DWORD attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT);
}
Socket listener(bool control) {
    auto path=socketPath(control);
    Socket fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd==INVALID_SOCKET)return fd;socketOptions(fd);
    sockaddr_un a{};a.sun_family=AF_UNIX;
    if(path.size()>=sizeof(a.sun_path)){closesocket(fd);return INVALID_SOCKET;}
    memcpy(a.sun_path,path.c_str(),path.size()+1);DeleteFileW(wide(path).c_str());
    if(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))||listen(fd,32)){closesocket(fd);return INVALID_SOCKET;}
    return fd;
}
void readPeer(Peer& p) {
    while(p.fd!=INVALID_SOCKET) {
        int n=recv(p.fd,reinterpret_cast<char*>(&p.incoming)+p.readOffset,int(sizeof(Event)-p.readOffset),0);
        if(n<0&&socketError()==WSAEWOULDBLOCK)return;
        if(n<0&&socketError()==WSAEINTR)continue;
        if(n<=0){closePeer(p);return;}
        p.readOffset+=size_t(n);if(p.readOffset<sizeof(Event))continue;p.readOffset=0;
        Event e=p.incoming;
        if(!valid(e)){closePeer(p);return;}
        if(!p.registered) {
            if(e.kind!=Kind::hello){closePeer(p);return;}
            p.registered=true;p.observe=e.flags&Flags::monitor;p.injecting=e.flags&Flags::replay;
            if(p.injecting&&!mockMode){closePeer(p);return;}
            if(!p.injecting){
                for(const auto& h:hidDevices)if(h.active()){Event a=h.decoder.state;a.kind=Kind::added;enqueue(p,a);if(p.fd==INVALID_SOCKET)return;}
                for(const auto& a:virtualDevices)if(a.device){Event b=a;b.kind=Kind::added;enqueue(p,b);if(p.fd==INVALID_SOCKET)return;}
            }
        } else if(p.injecting) {
            if(e.kind==Kind::hello){closePeer(p);return;}
            if(e.device){
                auto v=std::find_if(virtualDevices.begin(),virtualDevices.end(),[&](const auto& item){return item.device==e.device;});
                if(v==virtualDevices.end()&&e.kind!=Kind::removed)v=std::find_if(virtualDevices.begin(),virtualDevices.end(),[](const auto& item){return !item.device;});
                if(v!=virtualDevices.end()){
                    if(!v->device||e.kind==Kind::added||e.kind==Kind::removed)virtualButtonState[size_t(v-virtualDevices.begin())]=0;
                    *v=e.kind==Kind::removed?Event{}:e;
                }
            }
            e.decoded=now(); // Marks service entry for optional benchmark diagnostics.
            route(e);
        } else {closePeer(p);return;}
    }
}
void peerReady(Peer& p) {
    WSANETWORKEVENTS events{};
    if(WSAEnumNetworkEvents(p.fd,p.event,&events)!=0){closePeer(p);return;}
    if(events.lNetworkEvents&(FD_READ|FD_CLOSE))readPeer(p);
    if(p.fd!=INVALID_SOCKET&&(events.lNetworkEvents&FD_WRITE))flush(p);
    if(p.fd!=INVALID_SOCKET&&(events.lNetworkEvents&FD_CLOSE))closePeer(p);
}
bool sameUser(DWORD pid,sn::ProcessIdentity& identity){identity=sn::processIdentity(pid);return identity.pid&&identity.user==serviceUser;}
void acceptEvents(Socket listenerFD,HANDLE listenerEvent) {
    WSANETWORKEVENTS events{};WSAEnumNetworkEvents(listenerFD,listenerEvent,&events);
    for(;;){
        Socket fd=accept(listenerFD,nullptr,nullptr);if(fd==INVALID_SOCKET)return;socketOptions(fd);
        DWORD pid=peerProcess(fd);sn::ProcessIdentity identity;
        if(!pid||!sameUser(pid,identity)||!loop.capacity()){closesocket(fd);continue;}
        auto slot=std::find_if(peers.begin(),peers.end(),[](const Peer& p){return p.fd==INVALID_SOCKET;});
        if(slot==peers.end()){closesocket(fd);continue;}
        Peer& p=*slot;p=Peer{};p.fd=fd;p.pid=pid;p.identity=identity;resolveForeground(p);
        int buffer=4096;setsockopt(fd,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<char*>(&buffer),sizeof(buffer));
        p.event=WSACreateEvent();WSAEventSelect(fd,p.event,FD_READ|FD_WRITE|FD_CLOSE);
        Peer* ptr=&p;loop.add(p.event,[ptr]{peerReady(*ptr);});
    }
}

// ---- Configuration -----------------------------------------------------
bool number(const json::value* x){return x&&x->is_number()&&std::isfinite(x->to_number<double>());}
std::unique_ptr<Configuration> parseConfig(const json::value& value) {
    auto d=value.if_object();if(!d)return nullptr;
    auto version=d->if_contains("version");auto profiles=d->if_contains("profiles");
    if(!number(version)||version->to_number<double>()!=1||!profiles||!profiles->is_object())return nullptr;
    if(profiles->as_object().size()>128)return nullptr;
    auto c=std::make_unique<Configuration>();
    c->web.enabled=!mockMode;
    auto webDirectory=mockMode?std::filesystem::path(wide(settingsPath)).parent_path()/L"web":std::filesystem::path(wide(dataDirectory()))/L"Web";
    c->web.certificate=utf8((webDirectory/L"server.crt").wstring());c->web.key=utf8((webDirectory/L"server.key").wstring());
    if(auto web=d->if_contains("web")){
        if(!web->is_object())return nullptr;auto enabled=web->as_object().if_contains("enabled");
        if(enabled&&!enabled->is_bool())return nullptr;if(enabled)c->web.enabled=enabled->as_bool();
    }
    for(const auto& [key,item]:profiles->as_object()){
        if(key.empty()||key.size()>255||!item.is_object())return nullptr;
        const auto& v=item.as_object();Profile p;
        for(const char* field:{"gain","deadzone","invert"}){
            auto a=v.if_contains(field);if(!a)continue;
            if(!a->is_array()||a->as_array().size()!=6)return nullptr;
            for(int i=0;i<6;++i){const auto& x=a->as_array()[i];
                if(std::string(field)=="invert"){if(!x.is_bool())return nullptr;p.motion.invert[i]=x.as_bool();continue;}
                if(!number(&x))return nullptr;double n=x.to_number<double>();
                if(std::string(field)=="gain"){if(n<0||n>20)return nullptr;p.motion.gain[i]=float(n);}
                else{if(n<0||n>1000)return nullptr;p.motion.deadzone[i]=float(n);}
            }
        }
        for(const char* field:{"dominant","translation","rotation","orbit","led"})if(auto x=v.if_contains(field);x&&!x->is_bool())return nullptr;
        auto flag=[&](const char* field,bool fallback){auto x=v.if_contains(field);return x?x->as_bool():fallback;};
        p.motion.dominant=flag("dominant",false);p.motion.translation=flag("translation",true);
        p.motion.rotation=flag("rotation",true);p.motion.orbit=flag("orbit",true);p.led=flag("led",true);
        std::array<std::string,32> commands{};
        if(auto buttons=v.if_contains("buttons")){
            if(!buttons->is_array()||buttons->as_array().size()>32)return nullptr;
            const auto& list=buttons->as_array();
            for(size_t i=0;i<list.size();++i){
                if(!list[i].is_object())return nullptr;const auto& b=list[i].as_object();
                auto keyCode=b.if_contains("keyCode"),modifiers=b.if_contains("modifiers"),command=b.if_contains("command"),label=b.if_contains("label"),action=b.if_contains("action");
                if(keyCode){if(!number(keyCode))return nullptr;double key=keyCode->to_number<double>();if(key<1||key>254||std::floor(key)!=key)return nullptr;p.keys[i]=int(key);}
                if(modifiers){if(!number(modifiers))return nullptr;double flags=modifiers->to_number<double>();if(flags<0||flags>0xffffffff||std::floor(flags)!=flags)return nullptr;p.modifiers[i]=uint64_t(flags);}
                if(command&&(!command->is_string()||command->as_string().size()>4096||keyCode))return nullptr;
                if(label&&(!label->is_string()||label->as_string().size()>128))return nullptr;
                if(action){
                    if(!action->is_string()||keyCode||command)return nullptr;
                    static const char* actions[]={"","dominant","translation","rotation","faster","slower","fit"};
                    auto found=std::find_if(std::begin(actions),std::end(actions),[&](const char* a){return action->as_string()==a;});
                    if(found==std::end(actions))return nullptr;p.actions[i]=Action(found-std::begin(actions));
                }
                if(command)commands[i]=std::string(command->as_string());
            }
        }
        c->web.commands.emplace(std::string(key),std::move(commands));
        c->profiles.emplace(std::string(key),p);
    }
    return c;
}
bool writeSettings(const json::value& value,std::string& error) {
    auto path=std::filesystem::path(wide(settingsPath));std::error_code ec;
    std::filesystem::create_directories(path.parent_path(),ec);
    auto temporary=path;temporary+=L".tmp";
    {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
        file<<json::serialize(value);file.flush();if(!file){error="Could not save settings";return false;}}
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){error="Could not save settings";DeleteFileW(temporary.c_str());return false;}
    return true;
}

struct Snapshot {
    std::array<Event,16> latest{};
    size_t count=0,clients=0;
    uint64_t reports=0,overflows=0,rejected=0;
    DWORD foreground=0;std::string app;
    std::array<bool,16> ledSupported{};
    std::array<int,16> ledState{};
    std::array<uint32_t,16> ledError{};
};
json::object status() {
    Snapshot s=loop.sync([]{
        Snapshot snapshot;
        for(const auto& h:hidDevices)if(h.active()&&snapshot.count<16){size_t i=snapshot.count++;snapshot.latest[i]=h.decoder.state;snapshot.ledSupported[i]=bool(h.led);snapshot.ledState[i]=h.led?h.led->state.load():-1;snapshot.ledError[i]=h.led?h.led->error.load():0;}
        for(const auto& e:virtualDevices)if(e.device&&snapshot.count<16)snapshot.latest[snapshot.count++]=e;
        for(const auto& p:peers)if(p.fd!=INVALID_SOCKET&&p.registered)++snapshot.clients;
        snapshot.reports=reports;snapshot.overflows=overflows;snapshot.rejected=rejected;snapshot.foreground=foregroundPID;snapshot.app=foregroundApp;
        return snapshot;
    });
    json::array items;
    for(size_t i=0;i<s.count;++i){const auto& e=s.latest[i];auto spec=deviceSpec(e.vendor,e.product);
        json::array axes;for(int16_t x:e.axes)axes.push_back(x);
        items.push_back(json::object{{"id",e.device},{"vendor",e.vendor},{"product",e.product},{"name",spec?spec->name:"Unknown"},
            {"buttonCount",spec?spec->buttons.size():0},{"axes",axes},{"buttons",e.buttons},{"ledSupported",s.ledSupported[i]},
            {"ledState",s.ledState[i]},{"ledError",s.ledError[i]}});
    }
    PROCESS_MEMORY_COUNTERS memory{};GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory));
    FILETIME created,exited,kernel,user;GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);
    auto ticks=[](FILETIME t){return double((uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime)/1e7;};
    boost::system::error_code ec;auto web=json::parse(webServer->status(),ec);
    return json::object{{"version",1},{"mock",mockMode},{"devices",items},{"clients",s.clients},{"reports",s.reports},
        {"overflows",s.overflows},{"rejected",s.rejected},{"foregroundPID",s.foreground},{"foregroundApp",s.app},
        // Windows has no Accessibility permission for SendInput; shortcuts cannot
        // reach applications running elevated unless Axial runs elevated too.
        {"accessibility",true},{"elevated",[]{HANDLE token;TOKEN_ELEVATION e{};DWORD n=0;bool r=false;
            if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)){if(GetTokenInformation(token,TokenElevation,&e,sizeof(e),&n))r=e.TokenIsElevated;CloseHandle(token);}return r;}()},
        {"residentBytes",uint64_t(memory.WorkingSetSize)},{"cpuSeconds",ticks(kernel)+ticks(user)},{"web",ec?json::value(json::object{}):web}};
}
std::atomic<bool> stopRequested{false};
json::value handle(const json::value& request) {
    auto object=request.if_object();if(!object)return json::object{{"error","Expected a JSON object"}};
    auto opValue=object->if_contains("op");std::string op=opValue&&opValue->is_string()?std::string(opValue->as_string()):"";
    if(op=="status")return status();
    if(op=="retryWeb"){webServer->retry();return json::object{{"ok",true}};}
    if(op=="requestAccessibility")return json::object{{"trusted",true}};
    if(op=="stop"){stopRequested=true;return json::object{{"ok",true}};}
    if(op=="getConfig"){std::lock_guard lock(documentMutex);return document;}
    if(op=="getCommands"){std::lock_guard lock(documentMutex);return commandCatalog;}
    if(op=="commands"){
        auto app=object->if_contains("app");auto commands=object->if_contains("commands");
        if(!app||!app->is_string()||app->as_string().empty()||app->as_string().size()>255||!commands||!commands->is_array()||commands->as_array().size()>4096)return json::object{{"error","Invalid commands"}};
        for(const auto& command:commands->as_array()){
            auto c=command.if_object();if(!c)return json::object{{"error","Invalid command"}};
            auto id=c->if_contains("id"),label=c->if_contains("label");
            if(!id||!id->is_string()||id->as_string().empty()||id->as_string().size()>4096||!label||!label->is_string()||label->as_string().size()>4096)return json::object{{"error","Invalid command"}};
        }
        std::lock_guard lock(documentMutex);
        if(!commandCatalog.contains(app->as_string())&&commandCatalog.size()>=128)return json::object{{"error","Command catalog is full"}};
        auto candidate=commandCatalog;candidate[app->as_string()]=*commands;
        if(json::serialize(candidate).size()>=1024*1024)return json::object{{"error","Command catalog exceeds the response size limit"}};
        commandCatalog=std::move(candidate);return json::object{{"ok",true}};
    }
    if(op=="setConfig"){
        auto config=object->if_contains("config");auto c=config?parseConfig(*config):nullptr;
        if(!c)return json::object{{"error","Invalid version 1 configuration"}};
        std::string error;
        {std::lock_guard lock(documentMutex);if(!writeSettings(*config,error))return json::object{{"error",error}};document=*config;}
        webServer->configure(c->web);
        Configuration* ptr=c.release();
        // Configuration changes execute between reports, never while decoding one.
        loop.sync([ptr]{resetFocus();configuration.reset(ptr);selectProfile();});
        return json::object{{"ok",true}};
    }
    return json::object{{"error","Unknown operation"}};
}
void controlLoop(Socket fd) {
    for(;;){Socket client=accept(fd,nullptr,nullptr);if(client==INVALID_SOCKET){if(socketError()==WSAEINTR)continue;return;}
        socketOptions(client);sn::ProcessIdentity identity;
        DWORD pid=peerProcess(client);if(!pid||!sameUser(pid,identity)){closesocket(client);continue;}
        setNonblocking(client,true);
        const uint64_t deadline=now()+2000000000;
        std::string input;bool complete=false;
        while(now()<deadline){
            char buffer[4096];int n=recv(client,buffer,sizeof(buffer),0);
            if(n>0){
                const auto* end=static_cast<const char*>(memchr(buffer,'\n',size_t(n)));
                input.append(buffer,end?size_t(end-buffer):size_t(n));
                if(input.size()>256*1024)break;
                if(end){complete=true;break;}
                continue;
            }
            int error=socketError();
            if(n<0&&error==WSAEINTR)continue;
            if(n<0&&error==WSAEWOULDBLOCK&&waitSocket(client,false,deadline))continue;
            break;
        }
        boost::system::error_code ec;json::value request=complete?json::parse(input,ec):json::value{};
        json::value response=complete&&!ec&&request.is_object()?handle(request):json::value(json::object{{"error","Expected a JSON object"}});
        std::string output=json::serialize(response)+"\n";
        size_t sent=0;
        while(sent<output.size()&&now()<deadline){
            int n=send(client,output.data()+sent,int(output.size()-sent),0);
            if(n>0){sent+=size_t(n);continue;}
            int error=socketError();
            if(n<0&&error==WSAEINTR)continue;
            if(n<0&&error==WSAEWOULDBLOCK&&waitSocket(client,true,deadline))continue;
            break;
        }
        shutdown(client,SD_SEND);closesocket(client);
        if(stopRequested.exchange(false))loop.post([]{loop.stop();});
    }
}

// ---- Foreground application and device arrival -------------------------
void updateFocus(DWORD pid) {
    auto identity=sn::processIdentity(pid);
    std::string app=identity.executable.empty()?"":executableName(identity.executable);
    loop.post([pid,identity,app]{
        if(foregroundPID!=pid||foregroundIdentity.birth!=identity.birth){
            resetFocus();foregroundPID=pid;foregroundIdentity=identity;foregroundApp=app;
            for(auto& p:peers)if(p.fd!=INVALID_SOCKET)resolveForeground(p);
            selectProfile();
        }
    });
}
void CALLBACK foregroundChanged(HWINEVENTHOOK,DWORD,HWND window,LONG,LONG,DWORD,DWORD) {
    DWORD pid=0;if(window)GetWindowThreadProcessId(window,&pid);
    if(pid)updateFocus(pid);
}
LRESULT CALLBACK shellWindow(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    if(message==WM_DEVICECHANGE&&(wParam==DBT_DEVICEARRIVAL||wParam==DBT_DEVICEREMOVECOMPLETE)){loop.post(scanDevices);return TRUE;}
    if(message==WM_QUERYENDSESSION)return TRUE;
    if(message==WM_ENDSESSION&&wParam){loop.post([]{loop.stop();});return 0;}
    return DefWindowProcW(window,message,wParam,lParam);
}
// Window-station work (foreground hook, device notifications) needs a thread
// with a message loop. It only posts work to the input loop.
void shellThread() {
    WNDCLASSW type{};type.lpfnWndProc=shellWindow;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"AxialServiceShell";
    RegisterClassW(&type);
    // A top-level hidden window (not message-only) receives WM_ENDSESSION.
    HWND window=CreateWindowExW(0,type.lpszClassName,L"Axial service",0,0,0,0,0,nullptr,nullptr,type.hInstance,nullptr);
    if(!mockMode){
        DEV_BROADCAST_DEVICEINTERFACE_W filter{};filter.dbcc_size=sizeof(filter);filter.dbcc_devicetype=DBT_DEVTYP_DEVICEINTERFACE;HidD_GetHidGuid(&filter.dbcc_classguid);
        RegisterDeviceNotificationW(window,&filter,DEVICE_NOTIFY_WINDOW_HANDLE);
    }
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,foregroundChanged,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    if(HWND foreground=GetForegroundWindow())foregroundChanged(nullptr,0,foreground,0,0,0,0);
    MSG message;while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
}
BOOL WINAPI consoleControl(DWORD){loop.post([]{loop.stop();});Sleep(5000);return TRUE;}
}

int main(int argc,char** argv) {
    bool appOwned=false;
    for(int i=1;i<argc;++i){std::string arg=argv[i];if(arg=="--mock")mockMode=true;else if(arg=="--app-owned")appOwned=true;else{fprintf(stderr,"usage: axial-service [--mock] [--app-owned]\n");return 2;}}
    if(!winsock()){fprintf(stderr,"Winsock is unavailable\n");return 1;}
    serviceUser=userSID();if(serviceUser.empty()){fprintf(stderr,"Cannot identify the current user\n");return 1;}
    HANDLE owner=nullptr;
    if(appOwned){
        auto self=sn::processIdentity(GetCurrentProcessId());auto parent=sn::processIdentity(self.parent);
        // The parent must be older than this process, or its PID was reused.
        if(parent.pid&&parent.birth<=self.birth)owner=OpenProcess(SYNCHRONIZE,FALSE,parent.pid);
        if(!owner){fprintf(stderr,"An owning app is required\n");return 2;}
    }
    auto path=socketPath();auto parent=path.substr(0,path.find_last_of("\\/"));
    if(!secureDirectory(parent)){fprintf(stderr,"Unsafe socket directory\n");return 1;}
    HANDLE lock=CreateFileW(wide(path+".lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(lock==INVALID_HANDLE_VALUE){fprintf(stderr,"Axial already running or socket directory unavailable\n");return 1;}
    Socket events=listener(false),controls=listener(true);
    if(events==INVALID_SOCKET||controls==INVALID_SOCKET){fprintf(stderr,"listen failed: %d\n",socketError());return 1;}
    keyWorker=new Worker;outputWorker=new Worker;
    auto custom=environment("AXIAL_CONFIG");
    settingsPath=custom.empty()?dataDirectory()+"\\settings.json":custom;
    {std::ifstream file(std::filesystem::path(wide(settingsPath)),std::ios::binary);
        if(file){std::stringstream text;text<<file.rdbuf();boost::system::error_code ec;document=json::parse(text.str(),ec);if(ec)document=nullptr;}}
    configuration=parseConfig(document);
    if(!configuration){document=json::object{{"version",1},{"profiles",json::object{{"*",json::object{}}}}};configuration=parseConfig(document);}
    webServer=std::make_unique<WebServer>();webServer->configure(configuration->web);
    selectProfile();
    HANDLE listenerEvent=WSACreateEvent();WSAEventSelect(events,listenerEvent,FD_ACCEPT);
    loop.add(listenerEvent,[events,listenerEvent]{acceptEvents(events,listenerEvent);});
    if(owner)loop.add(owner,[]{loop.stop();}); // A crashed app must not leave a headless driver.
    std::thread(controlLoop,controls).detach();
    std::thread(shellThread).detach();
    SetConsoleCtrlHandler(consoleControl,TRUE);
    loop.post(scanDevices);
    fprintf(stderr,"Axial %s listening at %s\n",mockMode?"mock service":"USB service",path.c_str());
    loop.run();
    // Orderly shutdown: release held input, switch LEDs off and remove sockets.
    resetFocus();for(auto& p:peers)closePeer(p);
    for(auto& h:hidDevices)if(h.active())setLED(h,false);
    outputWorker->drain();keyWorker->post([]{drainKeys();clearHeld();});keyWorker->drain();
    closesocket(events);closesocket(controls);
    DeleteFileW(wide(socketPath()).c_str());DeleteFileW(wide(socketPath(true)).c_str());
    CloseHandle(lock);DeleteFileW(wide(path+".lock").c_str());
    fflush(stderr);
    // Worker threads are intentionally left to process exit.
    webServer.reset();
    ExitProcess(0);
}
