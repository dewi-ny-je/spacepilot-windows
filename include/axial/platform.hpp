#pragma once
// Windows platform prelude shared by the service, the compatibility DLLs and
// the tests. Winsock must precede windows.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <string>

// afunix.h is missing from some MinGW releases; the ABI is documented and fixed.
#if __has_include(<afunix.h>)
#include <afunix.h>
#endif
#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX 108
typedef struct sockaddr_un {
    ADDRESS_FAMILY sun_family;
    char sun_path[UNIX_PATH_MAX];
} SOCKADDR_UN, *PSOCKADDR_UN;
#endif
#ifndef SIO_AF_UNIX_GETPEERPID
#define SIO_AF_UNIX_GETPEERPID _WSAIOR(IOC_VENDOR,256)
#endif

namespace sn {
using Socket=SOCKET;
inline std::wstring wide(const std::string& text);
inline std::string utf8(const std::wstring& text);
// The process environment block, which child processes inherit.
inline std::string environment(const char* name) {
    auto key=wide(name);DWORD size=GetEnvironmentVariableW(key.c_str(),nullptr,0);
    if(!size)return {};
    std::wstring value(size,L'\0');size=GetEnvironmentVariableW(key.c_str(),value.data(),size);
    value.resize(size);return utf8(value);
}
inline void setEnvironment(const char* name,const std::string& value){SetEnvironmentVariableW(wide(name).c_str(),wide(value).c_str());}
inline std::string utf8(const std::wstring& text) {
    if(text.empty())return {};
    int size=WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(size_t(size),'\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),result.data(),size,nullptr,nullptr);
    return result;
}
inline std::wstring wide(const std::string& text) {
    if(text.empty())return {};
    int size=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);
    std::wstring result(size_t(size),L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),result.data(),size);
    return result;
}
// Per-user data root, %LOCALAPPDATA%\Axial. Only the logged-in user and
// SYSTEM/Administrators can read it with the default profile ACL.
inline std::string dataDirectory() {
    auto custom=environment("AXIAL_DATA");if(!custom.empty())return custom;
    auto local=environment("LOCALAPPDATA");
    if(local.empty())local=environment("TEMP");
    return local+"\\Axial";
}
inline std::string executableName(const std::string& path) {
    auto slash=path.find_last_of("\\/");
    std::string name=slash==std::string::npos?path:path.substr(slash+1);
    for(auto& c:name)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
    return name;
}
inline std::string currentExecutable() {
    std::wstring buffer(32768,L'\0');
    DWORD n=GetModuleFileNameW(nullptr,buffer.data(),DWORD(buffer.size()));
    buffer.resize(n);return utf8(buffer);
}
}
