#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace sn {
// Hardware facts, independently transcribed. Slots are saved-profile / event-mask
// bits, never positions in the visible list. Preserve them when editing a layout.
struct ButtonSpec {uint8_t slot; const char* name; uint16_t usage=0;};
inline constexpr ButtonSpec twoButtons[]={{0,"Left"},{1,"Right"}};
inline constexpr ButtonSpec travelerButtons[]={
    {0,"1"},{1,"2"},{2,"3"},{3,"4"},{4,"5"},{5,"6"},{6,"7"},{7,"8"}
};
inline constexpr ButtonSpec spaceballButtons[]={
    {0,"1"},{1,"2"},{2,"3"},{3,"4"},{4,"5"},{5,"6"},{6,"7"},{7,"8"},{8,"9"},
    {9,"A"},{10,"B"},{11,"C"}
};
inline constexpr ButtonSpec explorerButtons[]={
    {0,"1"},{1,"2"},{2,"Top"},{3,"Left"},{4,"Right"},{5,"Front"},
    {6,"Esc"},{7,"Alt"},{8,"Shift"},{9,"Ctrl"},{10,"Fit"},{11,"Panel"},
    {12,"+"},{13,"−"},{14,"2D"}
};
inline constexpr ButtonSpec pilotButtons[]={
    {0,"1"},{1,"2"},{2,"3"},{3,"4"},{4,"5"},{5,"6"},
    {6,"Top"},{7,"Left"},{8,"Right"},{9,"Front"},{10,"Esc"},{11,"Alt"},
    {12,"Shift"},{13,"Ctrl"},{14,"Fit"},{15,"Panel"},{16,"+"},{17,"−"},
    {18,"Dominant"},{19,"3D lock"},{20,"Config"}
};
inline constexpr ButtonSpec proButtons[]={
    {12,"1"},{13,"2"},{14,"3"},{15,"4"},
    {0,"Menu"},{1,"Fit"},{2,"Top"},{4,"Right"},{5,"Front"},{8,"Roll"},
    {22,"Esc"},{23,"Alt"},{24,"Shift"},{25,"Ctrl"},{26,"Rotation lock"}
};
// 21 physical HID controls; alternate views/function-bank codes are not extra
// physical keys. LCD bezel keys use a separate, unsupported USB interface.
inline constexpr ButtonSpec pilotProButtons[]={
    {12,"1"},{13,"2"},{14,"3"},{15,"4"},{16,"5"},
    {0,"Menu"},{1,"Fit"},{2,"Top / Bottom"},{4,"Right / Left"},{5,"Front / Back"},
    {8,"Roll"},{10,"ISO"},{22,"Esc"},{23,"Alt"},{24,"Shift"},{25,"Ctrl"},
    {26,"Rotation"},{27,"Pan / Zoom"},{28,"Dominant"},{29,"+"},{30,"−"}
};
inline constexpr ButtonSpec pilotProAlternateButtons[]={
    {3,"Left"},{6,"Bottom"},{7,"Back"},{9,"Roll counterclockwise"},{11,"ISO 2"},
    {17,"6"},{18,"7"},{19,"8"},{20,"9"},{21,"10"}
};
// Enterprise report 0x1c uses sparse HID usages. Preserve the existing report-3
// slots for the 22 compatible controls; place its nine additional keys in holes.
inline constexpr ButtonSpec enterpriseButtons[]={
    {12,"1",13},{13,"2",14},{14,"3",15},{15,"4",16},{16,"5",17},
    {17,"6",18},{18,"7",19},{19,"8",20},{20,"9",21},{21,"10",22},
    {6,"11",77},{7,"12",78},
    {0,"Menu",1},{1,"Fit",2},{2,"Top",3},{4,"Right",5},{5,"Front",6},
    {8,"Roll",9},{10,"ISO",11},{22,"Esc",23},{23,"Alt",24},{24,"Shift",25},
    {25,"Ctrl",26},{26,"Rotation lock",27},{27,"Enter",36},{28,"Delete",37},
    {29,"Tab",175},{30,"Space",176},{3,"V1",103},{9,"V2",104},{11,"V3",105}
};
struct DeviceSpec {
    uint16_t vendor, product;
    const char* name;
    std::span<const ButtonSpec> buttons;
    std::span<const ButtonSpec> alternateButtons={};
    const ButtonSpec* button(unsigned slot) const {
        for(const auto& b:buttons)if(b.slot==slot)return &b;
        for(const auto& b:alternateButtons)if(b.slot==slot)return &b;
        return nullptr;
    }
};
inline constexpr DeviceSpec devices[]={
    {0x046d,0xc621,"Spaceball 5000 USB",spaceballButtons},
    {0x046d,0xc623,"SpaceTraveler",travelerButtons},
    {0x046d,0xc625,"SpacePilot",pilotButtons},
    {0x046d,0xc626,"SpaceNavigator",twoButtons},
    {0x046d,0xc627,"SpaceExplorer",explorerButtons},
    {0x046d,0xc628,"SpaceNavigator for Notebooks",twoButtons},
    {0x046d,0xc629,"SpacePilot Pro",pilotProButtons,pilotProAlternateButtons},
    {0x046d,0xc62b,"SpaceMouse Pro",proButtons},
    {0x256f,0xc62e,"SpaceMouse Wireless (USB)",twoButtons},
    {0x256f,0xc631,"SpaceMouse Pro Wireless (USB)",proButtons},
    {0x256f,0xc633,"SpaceMouse Enterprise",enterpriseButtons},
    {0x256f,0xc635,"SpaceMouse Compact",twoButtons},
    {0x256f,0xc636,"SpaceMouse Module",twoButtons},
    {0x256f,0xc638,"SpaceMouse Pro Wireless BT (USB)",proButtons},
    // Receiver descriptor: report 1 six axes, report 3 two buttons.
    {0x256f,0xc652,"SpaceMouse Wireless (Universal Receiver)",twoButtons}
};
inline const DeviceSpec* deviceSpec(uint16_t vendor,uint16_t product) {
    for(const auto& d:devices)if(d.vendor==vendor&&d.product==product)return &d;
    return nullptr;
}
inline constexpr bool enterprise(uint16_t vendor,uint16_t product) {
    return vendor==0x256f&&product==0xc633;
}
}
