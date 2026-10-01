// Device catalog exported to the settings app. Portable, so the managed model
// tests can load it on any host.
#include "axial/devices.hpp"
#include <cstdint>
#include <iterator>
#if defined(_WIN32)
#define AXIAL_EXPORT extern "C" __declspec(dllexport)
#else
#define AXIAL_EXPORT extern "C" __attribute__((visibility("default")))
#endif
AXIAL_EXPORT uint32_t AxialDeviceCount(){return uint32_t(std::size(sn::devices));}
AXIAL_EXPORT uint32_t AxialDeviceIdentity(uint32_t index){
    if(index>=std::size(sn::devices))return 0;
    const auto& d=sn::devices[index];return uint32_t(d.vendor)<<16|d.product;
}
AXIAL_EXPORT const char* AxialDeviceName(uint32_t identity){
    auto d=sn::deviceSpec(uint16_t(identity>>16),uint16_t(identity));return d?d->name:nullptr;
}
AXIAL_EXPORT const char* AxialDeviceButtonName(uint32_t identity,uint32_t slot){
    auto d=sn::deviceSpec(uint16_t(identity>>16),uint16_t(identity));auto b=d?d->button(slot):nullptr;return b?b->name:nullptr;
}
AXIAL_EXPORT uint32_t AxialDeviceButtonSlots(uint32_t identity,uint8_t* slots,uint32_t capacity){
    auto d=sn::deviceSpec(uint16_t(identity>>16),uint16_t(identity));if(!d||!slots||capacity<d->buttons.size())return 0;
    for(size_t i=0;i<d->buttons.size();++i)slots[i]=d->buttons[i].slot;return uint32_t(d->buttons.size());
}
