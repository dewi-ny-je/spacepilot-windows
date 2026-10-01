#pragma once
#include "platform.hpp"
#include <tlhelp32.h>
#include <sddl.h>
#include <string>

namespace sn {
// birth is the process creation time (FILETIME ticks); together with the PID it
// identifies one process instance even after Windows reuses the PID.
struct ProcessIdentity {DWORD pid=0,parent=0;std::string user;uint64_t birth=0;std::string executable;};
inline std::string processUser(HANDLE process) {
    HANDLE token=nullptr;if(!OpenProcessToken(process,TOKEN_QUERY,&token))return {};
    std::string result;DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);
    if(size){std::string buffer(size,'\0');
        if(GetTokenInformation(token,TokenUser,buffer.data(),size,&size)){
            LPSTR text=nullptr;if(ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&text)){result=text;LocalFree(text);}
        }
    }
    CloseHandle(token);return result;
}
inline DWORD parentProcess(DWORD pid) {
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)return 0;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);DWORD parent=0;
    for(BOOL ok=Process32FirstW(snapshot,&entry);ok;ok=Process32NextW(snapshot,&entry))if(entry.th32ProcessID==pid){parent=entry.th32ParentProcessID;break;}
    CloseHandle(snapshot);return parent;
}
inline ProcessIdentity processIdentity(DWORD pid) {
    if(!pid)return {};
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!process)return {};
    ProcessIdentity result;result.pid=pid;
    FILETIME created{},exited{},kernel{},user{};
    if(GetProcessTimes(process,&created,&exited,&kernel,&user))result.birth=(uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    std::wstring path(32768,L'\0');DWORD size=DWORD(path.size());
    if(QueryFullProcessImageNameW(process,0,path.data(),&size)){path.resize(size);result.executable=utf8(path);}
    result.user=processUser(process);CloseHandle(process);
    result.parent=parentProcess(pid);
    if(!result.birth||result.executable.empty())return {};
    return result;
}
// The installation directory of an executable, used to recognise helper
// processes launched by the foreground application.
inline std::string applicationRoot(const std::string& path) {
    auto slash=path.find_last_of("\\/");if(slash==std::string::npos)return {};
    std::string root=path.substr(0,slash+1);
    for(auto& c:root)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
    return root;
}
inline bool sameRoot(const std::string& root,const std::string& path) {
    auto other=applicationRoot(path);return !root.empty()&&other.compare(0,root.size(),root)==0;
}
template<class Lookup>
bool belongsToApplication(const ProcessIdentity& peer,const ProcessIdentity& foreground,Lookup lookup) {
    if(!peer.pid||!foreground.pid||peer.user!=foreground.user)return false;
    if(peer.pid==foreground.pid)return peer.birth==foreground.birth;
    const auto root=applicationRoot(foreground.executable);
    if(root.empty()||!sameRoot(root,peer.executable))return false;
    auto process=peer;
    for(int depth=0;depth<32&&process.parent;++depth){
        auto parent=lookup(process.parent);
        // A parent created after its child is an unrelated process that reused the PID.
        if(!parent.pid||parent.user!=peer.user||parent.pid==process.pid||parent.birth>process.birth)return false;
        if(parent.pid==foreground.pid)return parent.birth==foreground.birth;
        process=parent;
    }
    return false;
}
}
