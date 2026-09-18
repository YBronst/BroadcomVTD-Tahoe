#define BVT_HOST_TEST
#include "../POC/Trace.cpp"
#include "../POC/MapperCore.cpp"
#include "../POC/TxQualification.cpp"
#include "../POC/TxDisposition.cpp"
#include "../POC/TxPacket.hpp"
#include "../POC/TxCleanup.hpp"
#include <cassert>
#include <iostream>
#include <string>

// Actual wrapTx/TxLease/ringEvent + actual mapping/lifetime/packet code.
// Native kext, mbuf KPI and IOMMU calls are host mocks, NOT runtime proof.
namespace bvp {
struct Packet {
    uint64_t data=0x100123,length=80,maximum=256;
    Packet *next=nullptr,*nextPacket=nullptr;
    unsigned notifications=0,frees=0;
};
using mbuf_t=Packet *;
static void *mbuf_data(mbuf_t p) { return reinterpret_cast<void *>(p->data); }
static uint64_t mbuf_len(mbuf_t p) { return p->length; }
static uint64_t mbuf_maxlen(mbuf_t p) { return p->maximum; }
static Packet *mbuf_next(mbuf_t p) { return p->next; }
static Packet *mbuf_nextpkt(mbuf_t p) { return p->nextPacket; }
static IOMapper mapper;
static IOMapper *deviceMapper=&mapper;
static bool txChainsEnabled=true;
static unsigned originalTxCalls;
static bool originalReject;
template<typename T> static T load(const void *p,size_t offset) {
    T result;memcpy(&result,static_cast<const uint8_t *>(p)+offset,sizeof(result));return result;
}
template<typename T> static void put(void *p,size_t offset,T value) {
    memcpy(static_cast<uint8_t *>(p)+offset,&value,sizeof(value));
}
static bool ourRing(const void *di) { return di!=nullptr; }
static void rememberRing(const void *) {}
static void descriptors(const void *,uint32_t,uint32_t,const void *,uint32_t) {}
enum Hook { Free,Tx };
template<typename T> static T original(Hook,void *,void *,uint32_t);
template<> void original<void>(Hook h,void *osh,void *packet,uint32_t send) {
    assert(h==Free && osh && send==1);
    auto p=static_cast<Packet *>(packet);
    assert(!findMapping(reinterpret_cast<uint64_t>(packet)));
    assert(++p->notifications==1 && ++p->frees==1);
}
template<> int32_t original<int32_t>(Hook h,void *di,void *packet,uint32_t) {
    assert(h==Tx);++originalTxCalls;
    auto m=findMapping(reinterpret_cast<uint64_t>(packet));
    assert(m && m->descriptor && m->prepared && stateOf(*m)==MapState::Submitting);
    if (originalReject) { // Native consumes before the adapter is reached.
        auto p=static_cast<Packet *>(packet);assert(++p->notifications==1 && ++p->frees==1);
        return -1;
    }
    // Stand-in for the separately golden-tested OSL/descriptor adapter.
    uint8_t record[0x70];txpacket::encode(record,*m);
    assert(load<uint32_t>(record,8)==static_cast<Packet *>(packet)->length);
    m->consumed=1;
    put<uint16_t>(di,0x6e,uint16_t((m->startIndex+m->count)&2047));return 0;
}
#include "../build/poc-0.2.25/generated/FrontendAdmission.inc"
}

int main(int argc,char **argv) {
    using namespace bvp;
    runtimeModeInfo=uint32_t(RuntimeMode::AppleVTDCorrectiveExperimental);
    assert(argc==2);std::string mode=argv[1];
    uint8_t di[0x200] {};int owner;
    put<void *>(di,0x30,&owner);put<uint16_t>(di,0x6a,2048);
    put<uint64_t>(di,0x58,0x200000);put<uint64_t>(di,0x80,0x300000);
    Packet packets[70];
    auto used=[&]() { Event e {};snapshotMapping(64,e);return e; };
    auto submitted=[&](unsigned i) {
        assert(wrapTx(di,&packets[i],1)==0);
        auto m=findMapping(reinterpret_cast<uint64_t>(&packets[i]));
        assert(m && stateOf(*m)==MapState::Owned && m->consumed && m->prepared);return m;
    };
    auto rejected=[&](unsigned i) {
        auto calls=originalTxCalls;
        assert(wrapTx(di,&packets[i],1)==-1);
        assert(originalTxCalls==calls && packets[i].notifications==1 && packets[i].frees==1);
        assert(!findMapping(reinterpret_cast<uint64_t>(&packets[i])));
    };
    if (mode=="mixed" || mode=="quarantine") {
        Mapping *maps[64];
        for (unsigned i=0;i<64;++i) {
            maps[i]=submitted(i);
            if (i<18 || mode=="quarantine") markQuarantine(*maps[i],1);
        }
        auto before=IOMemoryDescriptor::completes,release=IOMemoryDescriptor::releases;
        rejected(64);rejected(65);
        assert(!mappingsHalted() && !firstFailureState && !stopAfter);
        assert(noCreditEvents==2 && IOMemoryDescriptor::completes==before && IOMemoryDescriptor::releases==release);
        for(unsigned i=0;i<64;++i) {
            assert(findMapping(reinterpret_cast<uint64_t>(&packets[i]))==maps[i]);
            assert(maps[i]->descriptor && !packets[i].frees);
        }
        if (mode=="mixed") {
            // Runtime-16 shape: 18 quarantined + 46 independently completed.
            for(unsigned i=18;i<64;++i) assert(finishNormally(*maps[i]));
            assert(IOMemoryDescriptor::completes==before+46 && completionsAfterPressure==46);
            Packet tail;tail.data=0x300ff0;tail.length=1460;tail.maximum=2048;
            packets[66].next=&tail;
            auto next=submitted(66);
            assert(next->inputLength==80 && next->bytes==1540 && next->count==3);
            assert(reservationsAfterPressure==1 && submissionsAfterPressure==1);
            assert(finishNormally(*next) && completionsAfterPressure==47);
            auto s=used();assert(s.payload[2]==18 && s.payload[3]==18 && s.payload[11]==2);
            assert(s.payload[12]==1 && s.payload[13]==47 && s.payload[14]==1);
            for(unsigned i=0;i<18;++i) assert(stateOf(*maps[i])==MapState::Quarantine && poisoned(*maps[i]) && maps[i]->descriptor);
            assert(!mappingsHalted() && !firstFailureState && !stopAfter);
            // A later real fault must still latch, even with 46 Empty credits.
            haltMappings(18);rejected(67);
            assert(mappingsHalted() && firstFailureState==2 && firstFailure.flags==18 && stopAfter);
            assert(noCreditEvents==2);
        } else {
            for(unsigned i=0;i<64;++i) assert(!finishNormally(*maps[i]) && !abortUnpublished(*maps[i]));
            rejected(66);
            assert(noCreditEvents==3 && !mappingsHalted() && !firstFailureState);
            assert(IOMemoryDescriptor::completes==before && IOMemoryDescriptor::releases==release);
        }
    } else if (mode=="fault") {
        auto m=submitted(0);IOMemoryDescriptor::failComplete=true;
        assert(!finishNormally(*m) && mappingsHalted() && halted==9);
        rejected(1);assert(noCreditEvents==0 && m->descriptor && poisoned(*m));
        assert(firstFailureState==2 && firstFailure.flags==9);
    } else {
        assert(mode=="failures");
        packets[0].nextPacket=&packets[1];rejected(0); // Existing shape guard.
        deviceMapper=nullptr;rejected(1);deviceMapper=&mapper;
        IOMemoryDescriptor::failCreate=true;rejected(2);IOMemoryDescriptor::failCreate=false;
        IOMemoryDescriptor::failPrepare=true;rejected(3);IOMemoryDescriptor::failPrepare=false;
        IOMemoryDescriptor::failSegment=true;rejected(4);IOMemoryDescriptor::failSegment=false;
        originalReject=true;assert(wrapTx(di,&packets[5],1)==-1);originalReject=false;
        assert(packets[5].frees==1 && packets[5].notifications==1 && IOMemoryDescriptor::live==0);
        auto m=submitted(6);auto calls=originalTxCalls;
        assert(wrapTx(di,&packets[6],1)==-1 && originalTxCalls==calls);
        assert(!packets[6].frees && m->descriptor && poisoned(*m));
        assert(!noCreditEvents && !mappingsHalted());
    }
    std::cout<<"PASS actual wrapTx admission/ownership: "<<mode<<"\n";
}
