#pragma once
#include "devices.hpp"
namespace sn {
// Public application-event IDs used by Blender's macOS client. These are
// semantic IDs, not raw HID bit positions (especially on older devices).
inline int applicationButton(uint16_t vendor,uint16_t product,unsigned slot) {
    if(slot>=32||!deviceSpec(vendor,product))return 0;
    if(enterprise(vendor,product)){
        auto button=deviceSpec(vendor,product)->button(slot);return button?button->usage:0;
    }
    if(vendor==0x046d&&product==0xc627){
        constexpr int ids[]={13,14,3,4,5,6,23,24,25,26,2,1,30,31,27};
        return slot<std::size(ids)?ids[slot]:0;
    }
    if(vendor==0x046d&&product==0xc625){
        constexpr int ids[]={13,14,15,16,17,18,3,4,5,6,23,24,25,26,2,1,30,31,29,27};
        return slot<std::size(ids)?ids[slot]:0;
    }
    if(vendor==0x046d&&(product==0xc621||product==0xc623))return slot<9?13+slot:0;
    return deviceSpec(vendor,product)->button(slot)?int(slot+1):0;
}
}
