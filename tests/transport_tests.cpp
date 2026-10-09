#include "support.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);} while(0)
using namespace std::chrono_literals;
struct Listener {
    sn::Socket fd=INVALID_SOCKET;
    TemporaryDirectory directory{"axial-transport"};
    Listener(){
        sn::setEnvironment("AXIAL_SOCKET",directory.string()+"\\events");
        CHECK(sn::winsock());
        fd=socket(AF_UNIX,SOCK_STREAM,0);CHECK(fd!=INVALID_SOCKET);sn::socketOptions(fd);
        sockaddr_un address{};address.sun_family=AF_UNIX;auto name=sn::socketPath(true);
        memcpy(address.sun_path,name.c_str(),name.size()+1);
        CHECK(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);CHECK(listen(fd,1)==0);
    }
    ~Listener(){sn::closeSocket(fd);}
};
std::pair<std::string,double> reply(std::function<void(sn::Socket)> writer){
    Listener listener;
    std::thread server([&]{sn::Socket fd=accept(listener.fd,nullptr,nullptr);sn::socketOptions(fd);char data[1024];recv(fd,data,sizeof(data),0);writer(fd);sn::closeSocket(fd);});
    auto start=sn::now();auto result=sn::request("{}");double seconds=double(sn::now()-start)/1e9;
    server.join();return {result,seconds};
}
int main(){try{
    auto [valid,elapsed]=reply([](sn::Socket fd){sn::writeAll(fd,"{\"ok\":",6);std::this_thread::sleep_for(5ms);sn::writeAll(fd,"true}\ntrailing",14);});
    CHECK(valid=="{\"ok\":true}");CHECK(elapsed<1);
    CHECK(reply([](sn::Socket fd){sn::writeAll(fd,"{\"ok\":true}",11);}).first.find("error")!=std::string::npos);
    CHECK(reply([](sn::Socket fd){std::string huge(1024*1024+1,'x');huge+='\n';sn::writeAll(fd,huge.data(),huge.size());}).first.find("error")!=std::string::npos);
    auto [trickled,duration]=reply([](sn::Socket fd){for(int i=0;i<30;++i){if(!sn::writeAll(fd," ",1))break;std::this_thread::sleep_for(100ms);}});
    CHECK(trickled.find("error")!=std::string::npos);CHECK(duration>=1.8&&duration<2.7);
    // Windows may size a small backlog generously, and a nonblocking AF_UNIX
    // connect can report WSAEWOULDBLOCK before the backlog is full, so each
    // attempt gets a short bound. Every attempt must still return promptly, and
    // once the backlog is full a bounded connect fails.
    Listener listener;std::vector<sn::Socket> clients;bool full=false;
    for(int i=0;i<512;++i){auto begin=sn::now();sn::Socket fd=sn::connectSocket(true,50,true);CHECK(sn::now()-begin<200000000);if(fd==INVALID_SOCKET){full=true;break;}clients.push_back(fd);}
    if(full){auto begin=sn::now();CHECK(sn::connectSocket(true,100)==INVALID_SOCKET);CHECK(sn::now()-begin<400000000);}
    for(auto fd:clients)sn::closeSocket(fd);
    std::cout<<"PASS: fragmented replies, framing, size limit, absolute deadline and "<<(full?"full listener backlog":"prompt connects ("+std::to_string(clients.size())+" queued)")<<"\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
