#include "axial/application.hpp"
#include "axial/application_buttons.hpp"
#include <map>
#include <iostream>
#define CHECK(x) do {if(!(x)){std::cerr<<__LINE__<<": " #x "\n";return 1;}}while(0)
int main(){
    using sn::ProcessIdentity;
    const std::string user="S-1-5-21-1-2-3-1001";
    ProcessIdentity app{100,1,user,123,"C:\\Program Files\\Test\\Test.exe"};
    ProcessIdentity helper{101,100,user,124,"C:\\Program Files\\Test\\Helpers\\Helper.exe"};
    std::map<DWORD,ProcessIdentity> processes{{100,app},{101,helper}};
    auto lookup=[&](DWORD pid){auto it=processes.find(pid);return it==processes.end()?ProcessIdentity{}:it->second;};
    CHECK(sn::belongsToApplication(app,app,lookup));
    CHECK(sn::belongsToApplication(helper,app,lookup));
    auto child=helper;child.pid=102;child.parent=101;CHECK(sn::belongsToApplication(child,app,lookup));
    child.user="S-1-5-21-1-2-3-1002";CHECK(!sn::belongsToApplication(child,app,lookup));
    child=helper;child.executable="C:\\Program Files\\Other\\Other.exe";CHECK(!sn::belongsToApplication(child,app,lookup));
    // Directory comparison is case-insensitive, as NTFS paths are.
    child=helper;child.executable="c:\\PROGRAM FILES\\test\\Helpers\\Helper.exe";CHECK(sn::belongsToApplication(child,app,lookup));
    // A sibling folder sharing a name prefix is a different application.
    child=helper;child.executable="C:\\Program Files\\Test Tools\\Helper.exe";CHECK(!sn::belongsToApplication(child,app,lookup));
    child=helper;child.parent=1;CHECK(!sn::belongsToApplication(child,app,lookup));
    processes[100].birth=999;CHECK(!sn::belongsToApplication(helper,app,lookup));
    processes.erase(100);CHECK(!sn::belongsToApplication(helper,app,lookup));
    auto self=sn::processIdentity(GetCurrentProcessId());
    CHECK(self.pid==GetCurrentProcessId());CHECK(!self.user.empty());CHECK(self.birth!=0);CHECK(!self.executable.empty());
    CHECK(sn::executableName(self.executable)==sn::executableName(sn::currentExecutable()));
    CHECK(sn::executableName("C:\\Apps\\FreeCAD.EXE")=="freecad.exe");
    CHECK(sn::applicationButton(0x046d,0xc627,10)==2);
    CHECK(sn::applicationButton(0x256f,0xc633,6)==77);
    CHECK(sn::applicationButton(0x256f,0xc635,0)==1);
    CHECK(sn::applicationButton(0,0,0)==0);
    std::cout<<"Application ownership and semantic button contracts passed\n";
}
