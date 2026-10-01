#pragma once
#include <cstdio>
#include <map>
#include <set>

// Native test-only reader for the pinned MIT fixture's button triples. It does
// not ship in the app.
static void deviceCases(const char* fixturePath) {
    struct ExpectedDevice {uint16_t vendor,product;size_t buttons;};
    const ExpectedDevice expected[]={
        {0x046d,0xc621,12},{0x046d,0xc623,8},{0x046d,0xc625,21},
        {0x046d,0xc626,2},{0x046d,0xc627,15},{0x046d,0xc628,2},
        {0x046d,0xc629,21},{0x046d,0xc62b,15},{0x256f,0xc62e,2},
        {0x256f,0xc631,15},{0x256f,0xc633,31},{0x256f,0xc635,2},
        {0x256f,0xc636,2},{0x256f,0xc638,15},{0x256f,0xc652,2}
    };
    CHECK(std::size(sn::devices)==std::size(expected));
    std::set<uint32_t> identities;
    for(const auto& entry:expected) {
        auto spec=sn::deviceSpec(entry.vendor,entry.product);CHECK(spec&&spec->buttons.size()==entry.buttons);
        CHECK(identities.insert(uint32_t(entry.vendor)<<16|entry.product).second);
        uint32_t slots=0;
        for(auto buttons:{spec->buttons,spec->alternateButtons})for(const auto& b:buttons) {
            CHECK(b.slot<32&&b.name&&*b.name);CHECK(!(slots&(uint32_t(1)<<b.slot)));slots|=uint32_t(1)<<b.slot;
        }
        if(entry.product==0xc62b||entry.product==0xc631||entry.product==0xc638)CHECK(slots==0x07c0f137);
    }
    CHECK(std::string(sn::deviceSpec(0x046d,0xc626)->name)=="SpaceNavigator");
    CHECK(std::string(sn::deviceSpec(0x046d,0xc628)->name)=="SpaceNavigator for Notebooks");
    CHECK(std::string(sn::deviceSpec(0x256f,0xc652)->button(1)->name)=="Right");
    {   // Universal Receiver descriptor: report 1 is six int16 axes, report 3 two bits plus padding.
        sn::Decoder receiver;receiver.state.vendor=0x256f;receiver.state.product=0xc652;sn::Event event;
        const uint8_t motion[]={1,0x5e,0x01,0xa2,0xfe,1,0,2,0,3,0,4,0};
        CHECK(receiver.decode(motion,1,event)&&event.axes[0]==350&&event.axes[1]==-350&&event.axes[5]==4);
        const uint8_t right[]={3,2,0};CHECK(receiver.decode(right,2,event)&&event.buttons==2);
    }
    const std::map<std::string,uint32_t> sections={
        {"SpaceExplorer",0x046dc627},{"SpaceNavigator",0x046dc626},
        {"SpaceMouseCompact",0x256fc635},{"SpaceMousePro",0x046dc62b},
        {"SpaceMouseProWirelessBluetooth",0x256fc638},{"SpaceMouseWireless",0x256fc62e},
        {"SpaceMouseEnterprise",0x256fc633}
    };
    const std::map<std::string,std::string> names={
        {"SHIFT","Shift"},{"ESC","Esc"},{"CTRL","Ctrl"},{"ALT","Alt"},
        {"PANEL","Panel"},{"FIT","Fit"},{"MINUS","−"},{"PLUS","+"},
        {"T","Top"},{"L","Left"},{"R","Right"},{"F","Front"},{"BTN_2D","2D"},
        {"LEFT","Left"},{"RIGHT","Right"},{"MENU","Menu"},{"TOP","Top"},
        {"FRONT","Front"},{"REAR","Right"},{"ROLL_CW","Roll"},
        {"ROTATION","Rotation lock"},{"LOCK","Rotation lock"},{"ISO1","ISO"}
    };
    std::ifstream fixture(fixturePath);CHECK(fixture.good());std::string line,section;
    size_t compared=0;std::map<uint32_t,size_t> counts;
    while(std::getline(fixture,line)) {
        if(line.starts_with("[")) {section=line.substr(1,line.find(']')-1);continue;}
        if(!section.ends_with(".buttons"))continue;
        auto selected=sections.find(section.substr(0,section.size()-8));if(selected==sections.end())continue;
        char key[64]{};unsigned report=0,byte=0,bit=0;
        if(std::sscanf(line.c_str(),"%63s = [%u, %u, %u]",key,&report,&byte,&bit)!=4)continue;
        CHECK(report==3&&byte>=1&&byte<=4&&bit<8);unsigned slot=8*(byte-1)+bit;
        std::string expectedName;std::string rawName=key;
        if(rawName.starts_with("BTN_")&&rawName!="BTN_2D")expectedName=rawName.substr(4);
        else {auto name=names.find(rawName);CHECK(name!=names.end());expectedName=name->second;}
        uint32_t identity=selected->second;auto spec=sn::deviceSpec(identity>>16,identity&0xffff);
        CHECK(spec&&spec->button(slot)&&expectedName==spec->button(slot)->name);
        sn::Decoder decoder;decoder.state.vendor=identity>>16;decoder.state.product=identity&0xffff;
        sn::Event event;std::array<uint8_t,5> packet{3};packet[byte]=uint8_t(1<<bit);
        CHECK(decoder.decode(packet,1,event)&&event.buttons==(uint32_t(1)<<slot));
        packet[byte]=0;CHECK(decoder.decode(packet,2,event)&&event.buttons==0);
        ++compared;++counts[identity];
    }
    CHECK(compared==73&&counts.size()==7);
    CHECK(counts[0x046dc627]==15&&counts[0x256fc633]==22);
    // Independent Blender/hardware evidence resolves known upstream errors.
    CHECK(std::string(sn::deviceSpec(0x046d,0xc625)->button(16)->name)=="+");
    CHECK(std::string(sn::deviceSpec(0x046d,0xc625)->button(17)->name)=="−");
    CHECK(std::string(sn::deviceSpec(0x046d,0xc629)->button(12)->name)=="1");
    CHECK(std::string(sn::deviceSpec(0x046d,0xc629)->button(13)->name)=="2");

    // Enterprise hardware usage codes from ANTz captures / Linux button map.
    // These are usages, not byte/bit positions in the older report 3.
    struct Usage {uint16_t usage;uint8_t slot;const char* name;};
    const Usage usages[]={
        {1,0,"Menu"},{2,1,"Fit"},{3,2,"Top"},{5,4,"Right"},{6,5,"Front"},
        {9,8,"Roll"},{11,10,"ISO"},{13,12,"1"},{14,13,"2"},{15,14,"3"},
        {16,15,"4"},{17,16,"5"},{18,17,"6"},{19,18,"7"},{20,19,"8"},
        {21,20,"9"},{22,21,"10"},{23,22,"Esc"},{24,23,"Alt"},{25,24,"Shift"},
        {26,25,"Ctrl"},{27,26,"Rotation lock"},{36,27,"Enter"},{37,28,"Delete"},
        {77,6,"11"},{78,7,"12"},{103,3,"V1"},{104,9,"V2"},{105,11,"V3"},
        {175,29,"Tab"},{176,30,"Space"}
    };
    auto packet=[](uint8_t report,std::initializer_list<uint16_t> keys) {
        std::array<uint8_t,13> bytes{};bytes[0]=report;size_t i=1;
        for(auto key:keys){CHECK(i<bytes.size());bytes[i++]=key&0xff;bytes[i++]=key>>8;}
        return bytes;
    };
    sn::Decoder enterprise;enterprise.state.vendor=0x256f;enterprise.state.product=0xc633;
    sn::Event event;auto neutral=packet(0x1c,{});
    for(const auto& key:usages) {
        CHECK(enterprise.decode(packet(0x1c,{key.usage}),1,event));CHECK(event.buttons==(uint32_t(1)<<key.slot));
        CHECK(std::string(sn::deviceSpec(0x256f,0xc633)->button(key.slot)->name)==key.name);
        CHECK(enterprise.decode(neutral,2,event)&&event.buttons==0);
    }
    auto chord=packet(0x1c,{25,26,77,103,175,176});
    CHECK(enterprise.decode(chord,3,event));uint32_t held=event.buttons;
    const uint8_t legacyReleased[]={3,0,0,0,0};CHECK(!enterprise.decode(legacyReleased,4,event));CHECK(enterprise.state.buttons==held);
    const auto sequence=enterprise.state.sequence;
    for(size_t length=0;length<13;++length)CHECK(!enterprise.decode(std::span(chord.data(),length),5,event));
    CHECK(!enterprise.decode(packet(0x1c,{999}),6,event));CHECK(enterprise.state.buttons==held&&enterprise.state.sequence==sequence);
    CHECK(!enterprise.decode(packet(0x1d,{25,26,77,103,175,176}),7,event));
    CHECK(!enterprise.decode(neutral,8,event));CHECK(enterprise.state.buttons==held); // Long press outlives short report.
    CHECK(!enterprise.decode(legacyReleased,9,event));CHECK(enterprise.state.buttons==held);
    CHECK(enterprise.decode(packet(0x1d,{}),10,event)&&event.buttons==0);
    CHECK(enterprise.decode(packet(0x1c,{77,77}),11,event)&&event.buttons==(1u<<6)); // Duplicate usages are idempotent.
    CHECK(enterprise.decode(neutral,12,event)&&event.buttons==0);
    sn::Decoder other;other.state.vendor=0x046d;other.state.product=0xc627;
    CHECK(!other.decode(chord,13,event));CHECK(other.state.sequence==0);
    sn::Decoder fresh;fresh.state.vendor=0x256f;fresh.state.product=0xc633;
    CHECK(!fresh.decode(packet(0x1c,{65535}),14,event));CHECK(!fresh.enterpriseArraySeen);
    const uint8_t legacyFit[]={3,2,0,0,0};CHECK(fresh.decode(legacyFit,15,event)&&event.buttons==2);
    const uint8_t reserved[]={3,8,0,0,0};CHECK(fresh.decode(reserved,16,event)&&event.buttons==0);
    // Repeat worst-size array chords without allocating in the input callback.
    auto longNeutral=packet(0x1d,{});size_t prior=allocations;uint64_t checksum=0;
    trackAllocations=true;
    for(int i=0;i<100000;++i){enterprise.decode(chord,i,event);checksum+=event.buttons;enterprise.decode(neutral,i,event);enterprise.decode(longNeutral,i,event);}
    trackAllocations=false;CHECK(allocations==prior&&checksum>0);
    std::cout<<"Devices: 15 models, 73 upstream button replays, 31 Enterprise usages, long-press/chord release and zero allocations passed\n";
}
