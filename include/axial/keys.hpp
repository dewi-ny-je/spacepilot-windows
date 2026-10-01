#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace sn {
// Pure state machine. Key injection (SendInput) is supplied by the caller, allowing
// press/release behavior to be tested without sending keys to real applications.
class KeyState {
    struct Device {
        uint32_t id=0;
        std::array<int,32> keys;
        std::array<uint64_t,32> modifiers{};
        Device(){keys.fill(-1);}
    };
    std::array<Device,16> devices;
    std::array<unsigned,256> references{};
public:
    template<class Post> void release(uint32_t id,Post post) {
        for(auto& d:devices)if(d.id&&(!id||d.id==id)) {
            for(size_t i=0;i<d.keys.size();++i)if(d.keys[i]>=0) {
                int key=d.keys[i];if(references[key]&&!--references[key])post(key,false,d.modifiers[i]);d.keys[i]=-1;
            }
            d.id=0;
        }
    }
    template<class Post> void update(uint32_t id,uint32_t buttons,const std::array<int,32>& keys,
                                     const std::array<uint64_t,32>& modifiers,Post post) {
        if(!id)return;
        Device* state=nullptr;
        for(auto& d:devices)if(d.id==id){state=&d;break;}
        if(!state)for(auto& d:devices)if(!d.id){d.id=id;state=&d;break;}
        if(!state)return;
        for(size_t i=0;i<32;++i) {
            bool down=buttons&(1u<<i);
            if(down&&state->keys[i]<0&&keys[i]>=0&&keys[i]<256) {
                int key=keys[i];if(!references[key]++)post(key,true,modifiers[i]);
                state->keys[i]=key;state->modifiers[i]=modifiers[i];
            } else if(!down&&state->keys[i]>=0) {
                int key=state->keys[i];if(references[key]&&!--references[key])post(key,false,state->modifiers[i]);
                state->keys[i]=-1;
            }
        }
        if(!buttons)state->id=0;
    }
};
}
