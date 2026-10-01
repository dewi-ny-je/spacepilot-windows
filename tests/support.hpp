#pragma once
#include "axial/transport.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <thread>
// A private temporary directory, removed with its contents.
struct TemporaryDirectory {
    std::filesystem::path path;
    explicit TemporaryDirectory(const char* prefix) {
        wchar_t base[MAX_PATH+1];DWORD n=GetTempPathW(MAX_PATH+1,base);if(!n)throw std::runtime_error("GetTempPath failed");
        for(int attempt=0;attempt<100;++attempt){
            auto candidate=std::filesystem::path(base)/sn::wide(std::string(prefix)+"-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()%100000+attempt));
            if(CreateDirectoryW(candidate.c_str(),nullptr)){path=candidate;return;}
        }
        throw std::runtime_error("Cannot create a temporary directory");
    }
    std::string string() const {return sn::utf8(path.wstring());}
    ~TemporaryDirectory(){std::error_code ec;std::filesystem::remove_all(path,ec);}
};
inline HANDLE launch(const std::string& executable,const std::string& arguments,bool inherit=false,DWORD* pid=nullptr) {
    std::wstring command=L"\""+sn::wide(executable)+L"\" "+sn::wide(arguments);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION info{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,inherit,0,nullptr,nullptr,&startup,&info))return nullptr;
    CloseHandle(info.hThread);if(pid)*pid=info.dwProcessId;return info.hProcess;
}
struct MockService {
    HANDLE process=nullptr;TemporaryDirectory directory{"axial-tests"};
    explicit MockService(const char* executable,bool appOwned=false) {
        sn::setEnvironment("AXIAL_SOCKET",directory.string()+"\\events");
        sn::setEnvironment("AXIAL_CONFIG",directory.string()+"\\settings.json");
        process=launch(executable,appOwned?"--mock --app-owned":"--mock");
        if(!process)throw std::runtime_error("Cannot launch mock service");
        // The event listener can accept connections before the control listener
        // is ready. Tests use both, so wait for an actual control response.
        for(int i=0;i<500;++i){
            sn::Socket fd=sn::connectSocket();
            if(fd!=INVALID_SOCKET){
                sn::closeSocket(fd);
                if(sn::request("{\"op\":\"status\"}").find("\"mock\":true")!=std::string::npos)return;
            }
            if(WaitForSingleObject(process,10)==WAIT_OBJECT_0)break;
        }
        stop();throw std::runtime_error("Mock service did not start");
    }
    void stop() {
        if(!process)return;
        sn::request("{\"op\":\"stop\"}");
        if(WaitForSingleObject(process,5000)!=WAIT_OBJECT_0){TerminateProcess(process,1);WaitForSingleObject(process,5000);}
        CloseHandle(process);process=nullptr;
    }
    ~MockService(){stop();}
};
