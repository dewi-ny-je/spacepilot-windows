#include "support.hpp"
#include <filesystem>
#include <iostream>
#include <thread>
// The service watches the process that launched it with --app-owned. This test
// runs an intermediate owner (this executable in --owner mode) that launches the
// service, reports its PID, and exits without any cleanup when stdin closes.
int ownerMode(const char* service){
    DWORD pid=0;HANDLE process=launch(service,"--mock --app-owned",false,&pid);if(!process)return 2;
    printf("%lu\n",pid);fflush(stdout);
    char byte;while(fread(&byte,1,1,stdin)==1){}
    _exit(0); // Deliberately leave helper cleanup to its owner watch.
}
int main(int argc,char** argv){
    if(argc==3&&std::string(argv[1])=="--owner")return ownerMode(argv[2]);
    if(argc!=2)return 2;
    TemporaryDirectory directory("axial-owner");
    std::string path=directory.string()+"\\events";
    sn::setEnvironment("AXIAL_SOCKET",path);sn::setEnvironment("AXIAL_CONFIG",directory.string()+"\\settings.json");
    SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
    HANDLE inputRead,inputWrite,outputRead,outputWrite;
    if(!CreatePipe(&inputRead,&inputWrite,&inherit,0)||!CreatePipe(&outputRead,&outputWrite,&inherit,0))return 1;
    SetHandleInformation(inputWrite,HANDLE_FLAG_INHERIT,0);SetHandleInformation(outputRead,HANDLE_FLAG_INHERIT,0);
    std::wstring command=L"\""+sn::wide(sn::currentExecutable())+L"\" --owner \""+sn::wide(argv[1])+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=inputRead;startup.hStdOutput=outputWrite;startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION owner{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,0,nullptr,nullptr,&startup,&owner))return 1;
    CloseHandle(owner.hThread);CloseHandle(inputRead);CloseHandle(outputWrite);
    char text[32]{};DWORD read=0;bool ok=ReadFile(outputRead,text,sizeof(text)-1,&read,nullptr)&&read>0;CloseHandle(outputRead);
    DWORD servicePID=ok?DWORD(strtoul(text,nullptr,10)):0;ok&=servicePID!=0;
    HANDLE service=servicePID?OpenProcess(SYNCHRONIZE|PROCESS_TERMINATE,FALSE,servicePID):nullptr;
    bool connected=false;
    for(int i=0;i<100&&!connected;++i){auto fd=sn::connectSocket();connected=fd!=INVALID_SOCKET;if(connected)sn::closeSocket(fd);else std::this_thread::sleep_for(std::chrono::milliseconds(20));}
    ok&=connected;CloseHandle(inputWrite);WaitForSingleObject(owner.hProcess,5000);CloseHandle(owner.hProcess);
    // The service removes the two sockets separately. Wait for both rather
    // than treating the instant between those operations as a cleanup failure.
    auto socketsGone=[&]{return !std::filesystem::exists(sn::wide(path))&&!std::filesystem::exists(sn::wide(path+".control"));};
    for(int i=0;i<150&&!socketsGone();++i)std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ok&=socketsGone();
    ok&=service&&WaitForSingleObject(service,2000)==WAIT_OBJECT_0;
    if(!ok&&service)TerminateProcess(service,1);
    if(service){WaitForSingleObject(service,2000);CloseHandle(service);}
    std::cout<<(ok?"PASS: native helper exits and releases sockets when its owning app exits\n":"FAIL: helper outlived its owning app\n");
    return ok?0:1;
}
