#pragma once
#include "platform.hpp"
#include "core.hpp"
#include <algorithm>
#include <string>

namespace sn {
// Monotonic nanoseconds. QueryPerformanceCounter keeps counting across sleep,
// like the continuous clock used by the macOS implementation.
inline uint64_t now() noexcept {
    static const uint64_t frequency=[] {LARGE_INTEGER f;QueryPerformanceFrequency(&f);return uint64_t(f.QuadPart);}();
    LARGE_INTEGER counter;QueryPerformanceCounter(&counter);
    const uint64_t c=uint64_t(counter.QuadPart);
    return c/frequency*1000000000ull+(c%frequency)*1000000000ull/frequency;
}
// WSAStartup is reference counted and never called from DllMain.
inline bool winsock() {
    static const bool ready=[] {WSADATA data;return WSAStartup(MAKEWORD(2,2),&data)==0;}();
    return ready;
}
inline std::string socketPath(bool control=false) {
    auto custom=environment("AXIAL_SOCKET");
    std::string p=custom.empty()?dataDirectory()+"\\events":custom;
    return control?p+".control":p;
}
inline int socketError(){return WSAGetLastError();}
inline void closeSocket(Socket fd){if(fd!=INVALID_SOCKET)closesocket(fd);}
inline void socketOptions(Socket fd){SetHandleInformation(reinterpret_cast<HANDLE>(fd),HANDLE_FLAG_INHERIT,0);}
inline bool setNonblocking(Socket fd,bool enabled){u_long mode=enabled?1:0;return ioctlsocket(fd,FIONBIO,&mode)==0;}
// Wait for readiness until an absolute deadline. select() is used rather than
// WSAPoll because it reliably reports failed nonblocking connects.
inline bool waitSocket(Socket fd,bool write,uint64_t deadline) {
    for(;;) {
        uint64_t time=now();if(time>=deadline){WSASetLastError(WSAETIMEDOUT);return false;}
        uint64_t remaining=std::min<uint64_t>(deadline-time,2000000000ull);
        timeval timeout{long(remaining/1000000000ull),long((remaining%1000000000ull+999)/1000)};
        fd_set ready,failed;FD_ZERO(&ready);FD_ZERO(&failed);FD_SET(fd,&ready);FD_SET(fd,&failed);
        int result=select(0,write?nullptr:&ready,write?&ready:nullptr,&failed,&timeout);
        if(result<0&&socketError()==WSAEINTR)continue;
        if(result>0)return !FD_ISSET(fd,&failed)||!write;
        if(result==0)continue;
        return false;
    }
}
// Connect before restoring blocking mode, so a full listener backlog cannot
// hang callers indefinitely. Event-driven readers use the nonblocking mode.
inline Socket connectSocket(bool control=false,int timeoutMS=250,bool nonblocking=false) {
    if(!winsock())return INVALID_SOCKET;
    sockaddr_un a{};a.sun_family=AF_UNIX;auto p=socketPath(control);
    if(p.size()>=sizeof(a.sun_path)){WSASetLastError(WSAENAMETOOLONG);return INVALID_SOCKET;}
    Socket fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd==INVALID_SOCKET)return fd;
    socketOptions(fd);
    memcpy(a.sun_path,p.c_str(),p.size()+1);
    setNonblocking(fd,true);
    if(connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))!=0){
        int error=socketError();
        if((error!=WSAEWOULDBLOCK&&error!=WSAEINPROGRESS)||timeoutMS<=0||!waitSocket(fd,true,now()+uint64_t(timeoutMS)*1000000)){closesocket(fd);WSASetLastError(error);return INVALID_SOCKET;}
        int size=sizeof(error);error=0;
        if(getsockopt(fd,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&size)||error){closesocket(fd);if(error)WSASetLastError(error);return INVALID_SOCKET;}
    }
    if(!nonblocking)setNonblocking(fd,false);
    return fd;
}
inline bool writeAll(Socket fd,const void* data,size_t size) {
    const char* p=static_cast<const char*>(data);
    while(size){int n=send(fd,p,int(std::min<size_t>(size,1<<30)),0);
        if(n<0&&socketError()==WSAEINTR)continue;
        if(n<=0)return false;p+=n;size-=size_t(n);}
    return true;
}
inline bool readAll(Socket fd,void* data,size_t size) {
    char* p=static_cast<char*>(data);
    while(size){int n=recv(fd,p,int(std::min<size_t>(size,1<<30)),0);
        if(n<0&&socketError()==WSAEINTR)continue;
        if(n<=0)return false;p+=n;size-=size_t(n);}
    return true;
}
inline Socket openEvents(uint32_t flags=0,bool nonblocking=false) {
    Socket fd=connectSocket(false,250,false);if(fd==INVALID_SOCKET)return fd;
    Event e;e.kind=Kind::hello;e.pid=GetCurrentProcessId();e.flags=flags;
    if(!writeAll(fd,&e,sizeof(e))){closesocket(fd);return INVALID_SOCKET;}
    if(nonblocking)setNonblocking(fd,true);
    return fd;
}
// The peer process of an accepted AF_UNIX connection, as reported by the kernel.
inline DWORD peerProcess(Socket fd) {
    ULONG pid=0;DWORD bytes=0;
    if(WSAIoctl(fd,SIO_AF_UNIX_GETPEERPID,nullptr,0,&pid,sizeof(pid),&bytes,nullptr,nullptr)!=0)return 0;
    return pid;
}
inline std::string request(const std::string& json) {
    Socket fd=connectSocket(true,250,true);if(fd==INVALID_SOCKET)return "{\"error\":\"Axial service is not running\"}";
    const uint64_t deadline=now()+2000000000;
    std::string line=json+"\n",result;
    size_t sent=0;
    while(sent<line.size()&&now()<deadline) {
        int n=send(fd,line.data()+sent,int(line.size()-sent),0);
        if(n>0){sent+=size_t(n);continue;}
        int error=socketError();
        if(n<0&&error==WSAEINTR)continue;
        if(n<0&&error==WSAEWOULDBLOCK&&waitSocket(fd,true,deadline))continue;
        break;
    }
    bool complete=false;
    if(sent==line.size())while(now()<deadline) {
        char buf[4096];int n=recv(fd,buf,sizeof(buf),0);
        if(n>0){
            const auto* end=static_cast<const char*>(memchr(buf,'\n',size_t(n)));
            result.append(buf,end?size_t(end-buf):size_t(n));
            if(result.size()>1024*1024)break;
            if(end){complete=true;break;}
            continue;
        }
        int error=socketError();
        if(n<0&&error==WSAEINTR)continue;
        if(n<0&&error==WSAEWOULDBLOCK&&waitSocket(fd,false,deadline))continue;
        break;
    }
    closesocket(fd);
    return complete?result:"{\"error\":\"Service did not respond\"}";
}
}
