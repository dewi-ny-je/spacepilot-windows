#include "axial/core.hpp"
#include "axial/navigation.hpp"
#include "axial/keys.hpp"
#include <iostream>
#include <cstdlib>
#include <new>
#include <fstream>
#include <sstream>
static bool trackAllocations=false;
static size_t allocations=0;
void* operator new(size_t size){if(trackAllocations)++allocations;if(void* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
#define CHECK(x) do{if(!(x)){std::cerr<<__FILE__<<":"<<__LINE__<<" " #x "\n";std::exit(1);}}while(0)
#include "device_cases.hpp"
int main(int argc,char** argv){
    deviceCases(argc>2?argv[2]:"tests/fixtures/upstream-device-buttons.toml");
    sn::Decoder d;sn::Event e;
    // Adapted MIT fixtures: nytamin/spacemouse 787929591f8cc282c8823681934e6f7f8a401ae7.
    const uint8_t down[]={1,0,0,0,0,0x34,0};
    CHECK(d.decode(down,10,e));CHECK(e.axes[2]==52);CHECK(e.axes[0]==0);
    const uint8_t rotate[]={2,0,0,0,0,0x2d,0};
    CHECK(d.decode(rotate,11,e));CHECK(e.axes[5]==45);CHECK(e.axes[2]==52);
    const uint8_t combined[]={1,0,0,0,0,0x19,0,0,0,0,0,0x30,0};
    CHECK(d.decode(combined,12,e));CHECK(e.axes[2]==25);CHECK(e.axes[5]==48);
    const uint8_t extremes[]={1,0,0x80,0xff,0x7f,0xff,0xff};
    CHECK(d.decode(extremes,13,e));CHECK(e.axes[0]==-32768);CHECK(e.axes[1]==32767);CHECK(e.axes[2]==-1);
    auto before=d.state;
    for(size_t length=0;length<7;++length)CHECK(!d.decode(std::span(extremes,length),14,e));
    CHECK(d.state.sequence==before.sequence);
    const uint8_t unknown[]={0x17,0x64,1};CHECK(!d.decode(unknown,0,e));
    const uint8_t button[]={3,0x01,0x40};CHECK(d.decode(button,15,e));CHECK(e.buttons==0x4001);
    CHECK(!d.decode(button,16,e));
    const uint8_t release[]={3,0,0};CHECK(d.decode(release,17,e));CHECK(e.buttons==0);
    const uint8_t neutral[]={1,0,0,0,0,0,0,0,0,0,0,0,0};CHECK(d.decode(neutral,18,e));
    CHECK(std::all_of(e.axes.begin(),e.axes.end(),[](int x){return x==0;}));
    sn::Settings settings;e.axes={10,-30,40,100,20,-5};
    settings.deadzone[0]=10;settings.gain[1]=2;settings.invert[2]=true;
    auto f=sn::filter(e,settings);CHECK(f.axes[0]==0&&f.axes[1]==-60&&f.axes[2]==-40);
    settings.dominant=true;f=sn::filter(e,settings);CHECK(f.axes[3]==100&&f.axes[2]==0);
    settings.rotation=false;f=sn::filter(e,settings);CHECK(f.axes[1]==-60&&f.axes[3]==0);
    settings.dominant=false;settings.gain.fill(20);e.axes.fill(-32768);f=sn::filter(e,settings);CHECK(f.axes[1]==-32768);
    CHECK(sn::deviceSpec(0x046d,0xc627));CHECK(!sn::deviceSpec(0x256f,0xc671));
    sn::KeyState keys;std::array<int,32> mappings;mappings.fill(-1);mappings[0]=12;mappings[1]=12;
    std::array<uint64_t,32> modifiers{};modifiers[0]=0x100000;
    struct KeyEvent {int code;bool down;uint64_t modifiers;};std::array<KeyEvent,16> keyEvents{};size_t keyCount=0;
    auto post=[&](int code,bool down,uint64_t flags){CHECK(keyCount<keyEvents.size());keyEvents[keyCount++]={code,down,flags};};
    keys.update(1,1,mappings,modifiers,post);keys.update(1,1,mappings,modifiers,post);CHECK(keyCount==1&&keyEvents[0].down&&keyEvents[0].modifiers==0x100000);
    keys.update(2,1,mappings,modifiers,post);keys.release(1,post);CHECK(keyCount==1); // second device still holds the same key
    keys.release(2,post);CHECK(keyCount==2&&!keyEvents[1].down);
    keys.update(1,3,mappings,modifiers,post);keys.update(1,2,mappings,modifiers,post);CHECK(keyCount==3);
    keys.update(1,0,mappings,modifiers,post);CHECK(keyCount==4&&!keyEvents[3].down);
    keys.update(1,1,mappings,modifiers,post);mappings[0]=13;keys.update(1,0,mappings,modifiers,post);CHECK(keyEvents[5].code==12&&!keyEvents[5].down); // release original mapping
    keys.update(1,1,mappings,modifiers,post);keys.release(0,post);CHECK(keyCount==8&&!keyEvents[7].down);keys.release(0,post);CHECK(keyCount==8);
    sn::EventQueue<4> q;e.kind=sn::Kind::motion;e.axes.fill(1);e.device=1;
    CHECK(q.push(e));e.sequence=2;CHECK(q.push(e));CHECK(q.size()==1&&q.front().sequence==2);
    e.kind=sn::Kind::buttons;CHECK(q.push(e));e.kind=sn::Kind::motion;CHECK(q.push(e));e.axes={};CHECK(q.push(e));CHECK(q.size()==4);
    e.kind=sn::Kind::buttons;CHECK(!q.push(e));q.pop();CHECK(q.front().kind==sn::Kind::buttons);
    sn::Camera camera;auto original=camera;std::array<int16_t,6> axes{};
    sn::navigate(camera,{},axes,0.01,10,true,true,true);CHECK(camera.position.z==original.position.z);
    axes[0]=350;sn::navigate(camera,{},axes,0.01,10,true,true,true);CHECK(std::abs(camera.position.x-0.1)<1e-12);
    camera=original;axes={0,0,0,0,0,350};
    for(int i=0;i<10000;++i)sn::navigate(camera,{},axes,0.001,10,true,true,true);
    CHECK(std::abs(sn::length(camera.position)-10)<1e-8);CHECK(std::abs(sn::length(camera.right)-1)<1e-8);CHECK(std::abs(sn::dot(camera.right,camera.up))<1e-8);
    camera=original;sn::navigate(camera,{},axes,0.01,10,true,false,true);CHECK(camera.position.z==10&&camera.right.x==1);
    axes={0,350,0,0,0,0};sn::navigate(camera,{},axes,0.01,10,true,true,false);CHECK(camera.position.z==10);
    sn::NavigationView view{original,{},false,true,true,true,{-2,-1,-10,2,1,10}};
    view.advance(axes,.01,2);CHECK(view.camera.position.z==10);CHECK(view.extents[4]>1);CHECK(std::abs(view.extents[3]/view.extents[4]-2)<1e-12);
    // Standard views: right-handed orthonormal axes relative to the front view.
    sn::View commanded;CHECK(sn::commandedView(sn::viewCommand(sn::View::iso2),commanded)&&commanded==sn::View::iso2);
    CHECK(!sn::commandedView(sn::commandFit,commanded));CHECK(!sn::commandedView(sn::commandView|(9u<<24),commanded));
    for(int n=1;n<=8;++n){auto c=sn::orient(sn::View(n),original);
        CHECK(std::abs(sn::length(c.right)-1)<1e-12&&std::abs(sn::length(c.up)-1)<1e-12&&std::abs(sn::dot(c.right,c.up))<1e-12);
        CHECK(sn::length(sn::cross(c.right,c.up)-c.back)<1e-12);CHECK(c.position.z==original.position.z);}
    auto oriented=sn::orient(sn::View::left,original);CHECK(oriented.back.x==-1&&oriented.right.z==1&&oriented.up.y==1);
    oriented=sn::orient(sn::View::top,original);CHECK(oriented.back.y==1&&oriented.up.z==-1&&oriented.right.x==1);
    oriented=sn::orient(sn::View::iso1,original);CHECK(oriented.back.x>0&&oriented.back.y>0&&oriented.back.z>0&&std::abs(oriented.right.y)<1e-12);
    // A Z-up client (navlib Y = client Z) looks at its front from -Y.
    const double zUp[16]={1,0,0,0,0,0,-1,0,0,1,0,0,0,0,0,1};sn::Camera front;CHECK(sn::frontFromCoordinateSystem(zUp,front));
    CHECK(front.up.z==1&&front.back.y==-1&&front.right.x==1);CHECK(sn::orient(sn::View::top,front).back.z==1);
    const double mirrored[16]={-1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};CHECK(!sn::frontFromCoordinateSystem(mirrored,front));
    sn::Event corrupt;corrupt.version=2;CHECK(!sn::valid(corrupt));
    trackAllocations=true;uint64_t checksum=0;
    for(int i=0;i<100000;++i){d.decode(combined,i,e);auto output=sn::filter(e,settings);checksum+=uint16_t(output.axes[2]);}
    trackAllocations=false;CHECK(allocations==0);CHECK(checksum>0);
    // Actual SpaceExplorer USB capture: all six axes, both signs, and release.
    std::ifstream capture(argc>1?argv[1]:"tests/fixtures/spaceexplorer-motion.txt");CHECK(capture.good());
    std::string line;sn::Decoder physical;sn::Event decoded;
    std::array<int,6> minima{32767,32767,32767,32767,32767,32767},maxima{-32768,-32768,-32768,-32768,-32768,-32768};
    size_t reportCount=0;
    while(std::getline(capture,line)){
        if(line.empty()||line[0]=='#')continue;
        std::istringstream row(line);uint64_t timestamp;unsigned vendor,product,id,byte;
        row>>timestamp>>std::hex>>vendor>>product>>id;CHECK(vendor==0x046d&&product==0xc627);
        std::array<uint8_t,32> bytes{};size_t length=0;
        while(row>>byte){CHECK(length<bytes.size());bytes[length++]=uint8_t(byte);}
        CHECK(length==7&&bytes[0]==id);CHECK(physical.decode(std::span(bytes.data(),length),timestamp,decoded));
        for(int i=0;i<6;++i){minima[i]=std::min(minima[i],int(decoded.axes[i]));maxima[i]=std::max(maxima[i],int(decoded.axes[i]));}
        ++reportCount;
    }
    CHECK(reportCount>=4000);
    CHECK((minima==std::array<int,6>{-194,-185,-384,-381,-173,-322}));
    CHECK((maxima==std::array<int,6>{267,311,424,420,139,386}));
    CHECK((decoded.axes==std::array<int16_t,6>{}));
    std::cout<<"Core: HID replay, signed bounds, malformed input, filtering, queue ordering, ABI, navigation passed\n";
}
