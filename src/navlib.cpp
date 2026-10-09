// TDxNavLib.dll: the 3Dconnexion Navigation Library. Applications describe
// their view through property accessors; Axial drives the camera from the
// service's event stream at display rate on the thread that created the
// session (or a private thread for multithreaded sessions).
#include <cerrno>
#include <navlib/navlib.h>
#include "axial/navigation.hpp"
#include "axial/stream.hpp"
#include "dispatcher.hpp"
#include <boost/json/src.hpp>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace navlib;
namespace {
namespace json=boost::json;
struct Slot {std::string name;fnGetProperty_t get=nullptr;fnSetProperty_t set=nullptr;param_t param=0;value_t cached;bool hasValue=false;std::string text;};
struct DeviceBindings {uint32_t id=0,buttons=0;uint16_t vendor=0,product=0;std::array<std::string,32> commands;};
struct Session;
std::mutex registryMutex;
std::map<nlHandle_t,std::shared_ptr<Session>> sessions;
std::atomic<nlHandle_t> activeHandle{0};
nlHandle_t nextHandle=1;
long failure(unsigned code){return make_result_code(code);}
bool finite(sn::Vec v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
bool validBox(const box_t& b){
    return finite({b.min.x,b.min.y,b.min.z})&&finite({b.max.x,b.max.y,b.max.z})&&
        b.max.x>=b.min.x&&b.max.y>=b.min.y&&b.max.z>=b.min.z;
}
std::shared_ptr<Session> find(nlHandle_t h){std::lock_guard lock(registryMutex);auto it=sessions.find(h);return it==sessions.end()?nullptr:it->second;}
// Profiles are keyed by the lowercased executable name, as the service sees
// the foreground application.
std::string applicationID(){return sn::executableName(sn::currentExecutable());}
struct Session:std::enable_shared_from_this<Session> {
    nlHandle_t handle=0;
    std::string app;
    sn::Dispatcher dispatcher;
    std::map<std::string,Slot,std::less<>> properties;
    std::unique_ptr<sn::Stream> stream;
    std::unique_ptr<sn::FrameClock> clock;
    bool active=true,focused=true,moving=false,rowMajor=false,closed=false,clientTiming=false;
    bool bindingsPending=false,bindingsDirty=false;
    std::array<DeviceBindings,16> devices{};
    uint64_t lastFrame=0,lastInput=0;
    double clientFrame=0;
    sn::Event input;
    std::string commandRequest;
    Slot* slot(const char* name){auto it=properties.find(name);return it==properties.end()?nullptr:&it->second;}
    bool get(const char* name,value_t& value) {
        if(closed)return false;
        Slot* s=slot(name);if(!s)return false;
        if(s->get){if(s->get(s->param,s->name.c_str(),&value)==0)return true;if(closed)return false;}
        if(s->hasValue){value=s->cached;return true;}return false;
    }
    bool set(const char* name,const value_t& value){if(closed)return false;Slot* s=slot(name);if(!s||!s->set)return false;return s->set(s->param,s->name.c_str(),&value)==0;}
    void updateFrameClock(){
        bool run=moving&&active&&focused&&!closed&&!clientTiming;
        if(clock)clock->running(run);
    }
    void stopMotion(){input.axes={};if(moving){moving=false;updateFrameClock();set("motion",value_t(false));}lastFrame=0;clientFrame=0;}
    // The front view's orientation: the client's views.front, else the
    // navlib's own axes in the client's coordinate system.
    sn::Camera front(){
        auto index=[&](int i){return rowMajor?(i%4)*4+i/4:i;};
        sn::Camera c;value_t v;double m[16];
        auto column=[&](int i){return sn::Vec{m[i*4],m[i*4+1],m[i*4+2]};};
        if(get("views.front",v)&&v.type==matrix_type&&!closed){
            for(int i=0;i<16;++i)m[i]=v.matrix[index(i)];
            sn::Camera f;f.right=sn::normalized(column(0));f.up=sn::normalized(column(1));f.back=sn::normalized(column(2));
            if(sn::length(f.right)>0&&sn::length(f.up)>0&&sn::length(f.back)>0)return f;
        }
        if(!closed&&get("coordinateSystem",v)&&v.type==matrix_type){
            for(int i=0;i<16;++i)m[i]=v.matrix[index(i)];
            sn::frontFromCoordinateSystem(m,c);
        }
        return c;
    }
    // Fits the model in the view; with a view, first turns the camera to it.
    void fit(const sn::View* view=nullptr){
        value_t bounds,affine;
        if(!get("model.extents",bounds)||bounds.type!=box_type||!get("view.affine",affine)||affine.type!=matrix_type||closed)return;
        const auto& b=bounds.box;if(!validBox(b))return;
        for(int i=0;i<16;++i)if(!std::isfinite(affine.matrix[i]))return;
        sn::Vec centre{b.min.x/2+b.max.x/2,b.min.y/2+b.max.y/2,b.min.z/2+b.max.z/2};
        double radius=std::max(1e-6,sn::length(sn::Vec{b.max.x-b.min.x,b.max.y-b.min.y,b.max.z-b.min.z})/2);
        if(!std::isfinite(radius))return;
        auto index=[&](int i){return rowMajor?(i%4)*4+i/4:i;};
        if(view){
            auto c=sn::orient(*view,front());if(closed)return;
            const sn::Vec axes[]={c.right,c.up,c.back};
            for(int i=0;i<3;++i){if(!finite(axes[i]))return;affine.matrix[index(i*4)]=axes[i].x;affine.matrix[index(i*4+1)]=axes[i].y;affine.matrix[index(i*4+2)]=axes[i].z;}
        }
        sn::Vec back{affine.matrix[index(8)],affine.matrix[index(9)],affine.matrix[index(10)]};
        double n=sn::length(back);if(!std::isfinite(n)||n<1e-12)return;back=back*(1/n);
        value_t v;double fov=0.7853981633974483;if(get("view.fov",v)&&v.type==double_type&&std::isfinite(v.d))fov=std::clamp(v.d,0.05,3.0);
        sn::Vec position=centre+back*(radius/std::sin(fov/2)*1.05);
        if(!finite(position))return;
        affine.matrix[index(12)]=position.x;affine.matrix[index(13)]=position.y;affine.matrix[index(14)]=position.z;
        set("transaction",value_t(long(1)));if(closed)return;set("view.affine",affine);if(closed)return;
        value_t extents;if(get("view.extents",extents)&&extents.type==box_type&&validBox(extents.box)){
            double height=extents.box.max.y-extents.box.min.y;double aspect=height>0?(extents.box.max.x-extents.box.min.x)/height:1;
            aspect=std::max(.01,aspect);extents.box.min.x=-radius*std::max(1.,aspect);extents.box.max.x=-extents.box.min.x;extents.box.min.y=-radius*std::max(1.,1/aspect);extents.box.max.y=-extents.box.min.y;if(validBox(extents.box))set("view.extents",extents);
        }
        if(closed)return;set("pivot.position",value_t(point_t{centre.x,centre.y,centre.z}));if(closed)return;set("transaction",value_t(long(0)));
    }
    void frame(double explicitDT=0) {
        if(closed||!active||!focused||!moving)return;
        if(activeHandle.load(std::memory_order_relaxed)!=handle){stopMotion();return;}
        uint64_t time=sn::now();
        if(lastInput&&time-lastInput>250000000){stopMotion();return;}
        double dt=explicitDT>0?explicitDT:(lastFrame?double(time-lastFrame)/1e9:1.0/120.0);
        lastFrame=time;
        value_t affine;
        if(!get("view.affine",affine)||affine.type!=matrix_type||closed)return;
        double m[16];for(int i=0;i<16;++i)m[i]=affine.matrix[rowMajor?(i%4)*4+i/4:i];
        for(double x:m)if(!std::isfinite(x))return;
        sn::Camera c{{m[0],m[1],m[2]},{m[4],m[5],m[6]},{m[8],m[9],m[10]},{m[12],m[13],m[14]}};
        bool perspective=true,rotatable=true;value_t v;
        if(get("view.perspective",v)&&v.type==bool_type)perspective=v.b;
        if(get("view.rotatable",v)&&v.type==bool_type)rotatable=v.b;
        if(closed)return;
        if(get("view.constructionPlane",v)&&v.type==plane_type&&!perspective){
            sn::Vec normal{v.plane.n.x,v.plane.n.y,v.plane.n.z};
            double n=sn::length(normal);if(n>1e-12&&std::abs(sn::dot(c.back,normal)/n)>0.9999)rotatable=false;
        }
        sn::Vec pivot{};bool supplied=false;
        if(get("pivot.position",v)&&v.type==point_type){pivot={v.point.x,v.point.y,v.point.z};supplied=true;}
        if(!supplied&&get("view.target",v)&&v.type==point_type){pivot={v.point.x,v.point.y,v.point.z};supplied=true;}
        if(!supplied&&get("model.extents",v)&&v.type==box_type)pivot={(v.box.min.x+v.box.max.x)/2,(v.box.min.y+v.box.max.y)/2,(v.box.min.z+v.box.max.z)/2};
        if(!finite(pivot))return;
        double scale=std::max(1e-3,sn::length(c.position-pivot));
        if(get("view.focusDistance",v)&&v.type==double_type&&std::isfinite(v.d)&&v.d>0)scale=v.d;
        value_t extents;bool hasExtents=!perspective&&get("view.extents",extents)&&extents.type==box_type;
        if(hasExtents&&!validBox(extents.box))return;
        if(hasExtents)scale=std::max(1e-3,extents.box.max.y-extents.box.min.y);
        if(closed||!std::isfinite(scale))return;
        sn::NavigationView view{c,pivot,perspective,rotatable,bool(input.flags&sn::Flags::orbit),hasExtents};
        if(hasExtents)view.extents={extents.box.min.x,extents.box.min.y,extents.box.min.z,extents.box.max.x,extents.box.max.y,extents.box.max.z};
        view.advance(input.axes,dt,scale);c=view.camera;
        double next[16]={c.right.x,c.right.y,c.right.z,0,c.up.x,c.up.y,c.up.z,0,c.back.x,c.back.y,c.back.z,0,c.position.x,c.position.y,c.position.z,1};
        for(double x:next)if(!std::isfinite(x))return;
        for(int i=0;i<16;++i)affine.matrix[rowMajor?(i%4)*4+i/4:i]=next[i];
        set("transaction",value_t(long(1)));if(closed)return;
        set("view.affine",affine);if(closed)return;
        if(hasExtents&&input.axes[1]){
            extents.box.min.x=view.extents[0];extents.box.min.y=view.extents[1];extents.box.max.x=view.extents[3];extents.box.max.y=view.extents[4];
            if(validBox(extents.box))set("view.extents",extents);if(closed)return;
        }
        set("transaction",value_t(long(0)));
    }
    void receive(const sn::Event& e){
        if(closed)return;
        if(e.kind==sn::Kind::added){
            if(!e.device)return;
            auto found=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.id==e.device;});
            if(found==devices.end())for(auto& d:devices)if(!d.id){d.id=e.device;d.vendor=e.vendor;d.product=e.product;break;}
            loadBindings();if(!commandRequest.empty()){auto request=commandRequest;sn::background([request]{sn::request(request);});}return;
        }
        if(e.kind==sn::Kind::removed||e.kind==sn::Kind::reset){
            if(e.flags&sn::Flags::disconnected)devices={};
            else for(auto& d:devices)if(!e.device||d.id==e.device){if(e.kind==sn::Kind::removed)d={};else d.buttons=0;}
            stopMotion();value_t release;release.type=string_type;release.string={const_cast<char*>(""),1};set("commands.activeCommand",release);if(!closed)loadBindings();return;
        }
        if(!active||!focused||activeHandle.load(std::memory_order_relaxed)!=handle)return;
        if(e.kind==sn::Kind::command){
            sn::View view;
            if(sn::commandedView(e.flags,view))fit(&view);else if(e.flags&sn::commandFit)fit();
            return;
        }
        if(e.kind==sn::Kind::motion){
            input=e;lastInput=sn::now();
            bool nonzero=std::any_of(e.axes.begin(),e.axes.end(),[](int x){return x!=0;});
            if(nonzero&&!moving){moving=true;lastFrame=sn::now();set("motion",value_t(true));
                if(!clientTiming)frame(1.0/120);updateFrameClock();}
            else if(!nonzero&&moving)stopMotion();
        } else if(e.kind==sn::Kind::buttons){
            auto found=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.id==e.device;});
            if(found==devices.end())return;
            uint32_t changed=e.buttons^found->buttons;found->buttons=e.buttons;
            for(int i=0;i<32&&!closed;++i)if((changed&(1u<<i))&&!found->commands[i].empty()){
                // Copy before the client callback, which may reenter and reset the session.
                std::string text=(e.buttons&(1u<<i))?found->commands[i]:"";
                value_t command;command.type=string_type;command.string={text.data(),text.size()+1};set("commands.activeCommand",command);
            }
        }
    }
    static void resolveBindings(const std::string& data,const std::string& app,std::array<DeviceBindings,16>& resolved,bool& found){
        boost::system::error_code ec;auto doc=json::parse(data,ec);
        const json::object* profiles=nullptr;
        if(!ec&&doc.is_object())if(auto p=doc.as_object().if_contains("profiles"))profiles=p->if_object();
        found=profiles!=nullptr;
        for(auto& device:resolved){
            device.commands={};if(!device.id||!profiles)continue;
            char suffix[16];snprintf(suffix,sizeof(suffix),"@%04x:%04x",device.vendor,device.product);
            const json::value* profile=nullptr;
            for(const std::string& key:{app+suffix,std::string("*")+suffix,app,std::string("*")})if((profile=profiles->if_contains(key)))break;
            const json::object* object=profile?profile->if_object():nullptr;
            const json::value* buttons=object?object->if_contains("buttons"):nullptr;
            if(buttons&&buttons->is_array()){
                const auto& list=buttons->as_array();
                for(size_t i=0;i<std::min<size_t>(list.size(),32);++i)if(auto b=list[i].if_object())if(auto command=b->if_contains("command"))if(command->is_string())device.commands[i]=std::string(command->as_string());
            }
        }
    }
    void loadBindings(){
        if(closed)return;
        if(bindingsPending){bindingsDirty=true;return;}
        auto self=find(handle);if(!self)return;
        bindingsPending=true;
        auto snapshot=devices;
        sn::background([self,snapshot] {
            std::string data=sn::request("{\"op\":\"getConfig\"}");
            auto resolved=snapshot;bool found=false;
            resolveBindings(data,self->app,resolved,found);
            self->dispatcher.post([self,resolved,found] {
                if(self->closed)return;
                self->bindingsPending=false;
                if(found)for(auto& device:self->devices)for(const auto& value:resolved)if(device.id&&device.id==value.id)device.commands=value.commands;
                if(self->bindingsDirty){self->bindingsDirty=false;self->loadBindings();}
            });
        });
    }
    bool start(bool multithreaded){
        Session* self=this;
        if(!dispatcher.create(weak_from_this(),multithreaded,
            [self]{if(!self->closed&&!self->clientTiming)self->frame();},
            [self]{if(auto s=find(self->handle))s->loadBindings();}))return false;
        if(multithreaded)dispatcher.sync([&]{dispatcher.setTimer(2000);});else dispatcher.setTimer(2000);
        clock=std::make_unique<sn::FrameClock>([self]{self->dispatcher.requestFrame();});
        stream=std::make_unique<sn::Stream>([](void* ctx,const sn::Event& e) {
            auto* s=static_cast<Session*>(ctx);
            s->dispatcher.post([s,e]{if(!s->closed)s->receive(e);});
        },this);
        stream->start();
        return true;
    }
    // Runs on the session thread.
    void close(){
        if(closed)return;closed=true;
        if(stream)stream->stop();
        if(clock)clock->stop();
        dispatcher.destroy();
    }
};
void publishCommands(Session& s,const SiActionNodeEx_t* root){
    json::array commands;
    std::vector<const SiActionNodeEx_t*> todo;if(root)todo.push_back(root);
    size_t visited=0;
    while(!todo.empty()&&visited++<4096){auto n=todo.back();todo.pop_back();
        if(n->size<sizeof(SiActionNodeEx_t))break;
        if(n->type==SI_ACTION_NODE&&n->id)commands.push_back(json::object{{"id",n->id},{"label",n->label?n->label:n->id}});
        if(n->next)todo.push_back(n->next);if(n->children)todo.push_back(n->children);
    }
    std::string request=json::serialize(json::object{{"op","commands"},{"app",s.app},{"commands",std::move(commands)}});
    s.commandRequest=request;
    sn::background([request]{sn::request(request);});
}
}
extern "C" {
long __cdecl NlCreate(nlHandle_t* out,const char* app,const accessor_t accessors[],size_t count,const nlCreateOptions_t* options){
    static const bool mapped=[] {sn::keepCallbackCodeMapped(reinterpret_cast<const void*>(&failure));return true;}();(void)mapped;
    if(!out)return failure(EINVAL);*out=0;
    if(!app||(!accessors&&count)||count>256||(options&&options->size<sizeof(nlCreateOptions_t)))return failure(EINVAL);
    if(!sn::winsock())return failure(EIO);
    auto s=std::make_shared<Session>();s->app=applicationID();
    s->rowMajor=options&&(options->options&row_major_order);
    for(size_t i=0;i<count;++i){if(!accessors[i].name)return failure(EINVAL);Slot slot;slot.name=accessors[i].name;slot.get=accessors[i].fnGet;slot.set=accessors[i].fnSet;slot.param=accessors[i].param;s->properties.emplace(slot.name,std::move(slot));}
    {std::lock_guard lock(registryMutex);s->handle=nextHandle++;sessions.emplace(s->handle,s);}
    if(!s->start(options&&options->bMultiThreaded)){
        std::lock_guard lock(registryMutex);sessions.erase(s->handle);return failure(EIO);
    }
    *out=s->handle;activeHandle=s->handle;return 0;
}
long __cdecl NlClose(nlHandle_t handle){
    auto s=find(handle);if(!s)return failure(EINVAL);
    nlHandle_t expected=handle;activeHandle.compare_exchange_strong(expected,0);
    if(!s->dispatcher.sync([&]{s->stopMotion();s->close();})){s->closed=true;if(s->stream)s->stream->stop();if(s->clock)s->clock->stop();}
    {std::lock_guard lock(registryMutex);sessions.erase(handle);}
    return 0;
}
propertyType_t __cdecl NlGetType(property_t name){
    if(!name)return unknown_type;
    for(const auto& p:navlib::propertyDescription)if(strcmp(p.name,name)==0)return p.type;
    return unknown_type;
}
long __cdecl NlReadValue(nlHandle_t handle,property_t name,value_t* out){
    auto s=find(handle);if(!s||!name||!out)return failure(EINVAL);
    long result=0;
    if(!s->dispatcher.sync([&] {
        if(s->closed){result=failure(EINVAL);return;}
        if(strcmp(name,"active")==0)*out=value_t(s->active&&activeHandle==handle);
        else if(strcmp(name,"focus")==0)*out=value_t(s->focused);
        else if(strcmp(name,"motion")==0)*out=value_t(s->moving);
        else if(strcmp(name,"device.present")==0)*out=value_t(std::any_of(s->devices.begin(),s->devices.end(),[](const auto& d){return d.id!=0;}));
        else if(strcmp(name,"frame.timingSource")==0)*out=value_t(long(s->clientTiming));
        else if(!s->get(name,*out))result=failure(navlib_errc::property_not_found);
    }))return failure(EINVAL);
    return result;
}
long __cdecl NlWriteValue(nlHandle_t handle,property_t name,const value_t* value){
    auto s=find(handle);if(!s||!name||!value)return failure(EINVAL);
    auto type=NlGetType(name);if(type==unknown_type)return failure(navlib_errc::property_not_found);
    if(value->type!=type&&!(type==string_type&&value->type==cstr_type))return failure(EINVAL);
    long result=0;
    if(!s->dispatcher.sync([&] {
        if(s->closed){result=failure(EINVAL);return;}
        if(strcmp(name,"active")==0||strcmp(name,"focus")==0){
            if(strcmp(name,"active")==0)s->active=value->b;else s->focused=value->b;
            if(value->b)activeHandle=handle;else{nlHandle_t expected=handle;activeHandle.compare_exchange_strong(expected,0);}
            if(!value->b)s->stopMotion();return;
        }
        if(strcmp(name,"motion")==0){if(!value->b)s->stopMotion();return;}
        if(strcmp(name,"frame.timingSource")==0){if(value->l!=0&&value->l!=1){result=failure(EINVAL);return;}s->clientTiming=value->l;s->lastFrame=0;s->clientFrame=0;s->updateFrameClock();return;}
        if(strcmp(name,"frame.time")==0){if(!std::isfinite(value->d)){result=failure(EINVAL);return;}double dt=s->clientFrame>0?(value->d-s->clientFrame)/1000:1.0/120;s->clientFrame=value->d;if(s->clientTiming&&dt>0)s->frame(dt);return;}
        if(strcmp(name,"commands.tree")==0){publishCommands(*s,value->pnode);return;}
        if(strcmp(name,"images")==0)return;
        Slot& slot=s->properties[name];slot.name=name;
        if(type==string_type){const char* p=value->type==cstr_type?value->cstr_.p:value->string.p;size_t n=value->type==cstr_type?value->cstr_.length:value->string.length;
            if(!p||n>65536){result=failure(EINVAL);return;}slot.text.assign(p,strnlen(p,n));slot.cached.type=string_type;slot.cached.string={slot.text.data(),slot.text.size()+1};
        } else slot.cached=*value;
        slot.hasValue=true;
    }))return failure(EINVAL);
    return result;
}
}
