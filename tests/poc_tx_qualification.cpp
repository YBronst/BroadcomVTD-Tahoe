#define BVT_HOST_TEST
#include "../POC/Trace.cpp"
#include "../POC/MapperCore.cpp"
#include "../POC/TxQualification.cpp"
#include "../POC/TxDisposition.cpp"
#include "../POC/TxQuiescence.cpp"
#include "../POC/TxCleanup.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

namespace bvp {
static uint64_t imageBase=0,testCaller=0x2c2f43;
static constexpr uint64_t txForcedReclaimPC=0x2c2f43;
static unsigned nativeCalls,notifications,frees;
static bool breakClear;
static IOMapper mapper;
template<typename T> static T load(const void *p,size_t off) {T v;memcpy(&v,static_cast<const uint8_t *>(p)+off,sizeof(v));return v;}
template<typename T> static void put(void *p,size_t off,T v) {memcpy(static_cast<uint8_t *>(p)+off,&v,sizeof(v));}
static bool ourRing(const void *p) {return p!=nullptr;}
enum Hook {Reclaim};
template<typename T> static T original(Hook,void *di,uint32_t range) {
    ++nativeCalls;assert(range==1);
    auto n=load<uint16_t>(di,0x6a),in=load<uint16_t>(di,0x6c),out=load<uint16_t>(di,0x6e);
    if (in==out) return nullptr;
    auto maps=load<void *>(di,0x80),packets=load<void *>(di,0x70),descs=load<void *>(di,0x58);
    unsigned count=load<uint32_t>(maps,in*0x70+12);void *result=nullptr;
    // Exact relevant pinned native order: per-span address poison and packet
    // clear, last descriptor packet result, then software consumer advancement.
    for (unsigned i=0;i<count;++i) {
        result=load<void *>(packets,in*8);put<void *>(packets,in*8,nullptr);
        put<uint32_t>(descs,in*16+8,breakClear?0:0xdeadbeefU);put<uint32_t>(descs,in*16+12,0xdeadbeefU);
        in=(in+1)&(n-1);
    }
    put<uint16_t>(di,0x6c,in);return static_cast<T>(result);
}
static void notify(void *,void *,uint32_t status) {assert(!status);++notifications;}
// The ONLY replacement in these extracted source slices is the host return PC.
#define __builtin_return_address(n) reinterpret_cast<void *>(testCaller)
#include "../build/poc-0.2.25/generated/FrontendQualification.inc"
#undef __builtin_return_address
}

int main() {
    using namespace bvp;using namespace bvp::qual;
    runtimeModeInfo=uint32_t(RuntimeMode::AppleVTDCorrectiveExperimental);
    unsigned cases=0;
    auto check=[&](bool ok) {assert(ok);++cases;};
    Ring r;r.queue=2;r.owner=1;r.maps=0x1000;r.packets=0x2000;r.descs=0x3000;r.count=8;r.in=7;r.out=1;r.nativeSpan=2;
    MapView v;v.packet=9;v.queue=2;v.owner=1;v.start=7;v.end=1;v.record=0x1000+7*0x70;
    Slots s;s.count=2;s.packet[1]=9;
    check(entryReasons(true,FreePC,FreePC,1,0)==0);
    check(entryReasons(false,FreePC,FreePC,1,0)==CleanupOff);
    check(entryReasons(true,42,FreePC,1,0)==CallerMismatch);
    check(entryReasons(true,FreePC,FreePC,2,0)==RangeMismatch);
    check(entryReasons(true,FreePC,FreePC,1,8)==ControlFlag);
    check(preReasons(r,v,s)==0);
    auto rr=r;rr.count=7;check(preReasons(rr,v,s)&RingCount);
    rr=r;rr.in=8;check(preReasons(rr,v,s)&RingIndex);
    rr=r;rr.maps=0;check(preReasons(rr,v,s)&MissingVectors);
    auto vv=v;vv.packet=0;check(preReasons(r,vv,s)&NoMapping);
    vv=v;vv.owner=3;check(preReasons(r,vv,s)&QueueOwner);
    vv=v;vv.start=6;check(preReasons(r,vv,s)&StartIndex);
    vv=v;vv.end=8;check(preReasons(r,vv,s)&EndSpan);
    vv=v;vv.record=44;check(preReasons(r,vv,s)&MapRecord);
    rr=r;rr.nativeSpan=1;check(preReasons(rr,v,s)&NativeSpan);
    auto ss=s;ss.packet[0]=9;check(preReasons(r,v,ss)&PrePacketSlots);
    ss=s;ss.count=0;check(preReasons(r,v,ss)&ObservationIncomplete);
    Ring post=r;post.in=1;Slots cleared;cleared.count=2;
    for(unsigned i=0;i<2;++i)cleared.low[i]=cleared.high[i]=0xdeadbeefU;
    check(postReasons(r,post,v,cleared,9,true)==0);
    check(postReasons(r,post,v,cleared,10,true)&ReturnedMismatch);
    check(postReasons(r,post,v,cleared,9,false)&ReturnedMismatch);
    rr=post;rr.owner=3;check(postReasons(r,rr,v,cleared,9,true)&QueueOwner);
    rr=post;rr.descs=99;check(postReasons(r,rr,v,cleared,9,true)&RingIdentity);
    rr=post;rr.in=7;check(postReasons(r,rr,v,cleared,9,true)&PostIndex);
    ss=cleared;ss.packet[1]=9;check(postReasons(r,post,v,ss,9,true)&PostPacketSlots);
    ss=cleared;ss.high[0]=0;check(postReasons(r,post,v,ss,9,true)&DescriptorClear);
    check(disposition(FreePC,FreePC,0,false,false,0)==FreeOwningCaller);
    check(disposition(FreePC,FreePC,8,false,false,0)==Unknown);
    check(disposition(0x14709f,FreePC,0,true,true,1)==SyncDisposeBranch);
    check(disposition(0x14709f,FreePC,0,true,true,2)==SyncRequeueCapable);
    check(disposition(0x14709f,FreePC,0,true,false,1)==Unknown);
    enableTxCleanup(true);
    uint8_t di[0x200] {},osh[0x80] {},maps[8*0x70] {},descs[8*16] {};uint64_t packets[8] {};
    auto owner=reinterpret_cast<uint64_t>(osh),queue=reinterpret_cast<uint64_t>(di);
    put<void *>(di,0x30,osh);put<void *>(di,0x80,maps);put<void *>(di,0x70,packets);put<void *>(di,0x58,descs);
    put<uint16_t>(di,0x6a,8);put<void (*)(void *,void *,uint32_t)>(osh,0x30,notify);
    auto setup=[&](uint64_t packet) {
        memset(packets,0,sizeof(packets));memset(descs,0,sizeof(descs));
        put<uint16_t>(di,0x6c,7);put<uint16_t>(di,0x6e,1);put<uint32_t>(di,0xc,0);
        put<uint32_t>(maps,7*0x70+12,2);packets[0]=packet;
        auto m=reserveMapping(owner,queue,packet);assert(m);VirtualRange range{0x100123,5000};assert(prepareMapping(*m,&mapper,&range,1));
        m->consumed=1;m->startIndex=7;m->endIndex=1;m->mapRecord=reinterpret_cast<uint64_t>(maps)+7*0x70;
        assert(transition(*m,MapState::Prepared,MapState::Submitting));assert(transition(*m,MapState::Submitting,MapState::Owned));return m;
    };
    auto last=[&](Stage stage) {Event result{};for(auto &slot:bvp::ring)if(slot.value.type==TxQualification && slot.value.flags==stage && slot.value.sequence>result.sequence)result=slot.value;return result;};
    testCaller=0x14709f;auto m=setup(101);auto ctx=enterSync(reinterpret_cast<void *>(55),63,2,osh);
    auto before=*m;uint8_t oldDi[sizeof(di)],oldMaps[sizeof(maps)],oldDescs[sizeof(descs)];uint64_t oldPackets[8];
    memcpy(oldDi,di,sizeof(di));memcpy(oldMaps,maps,sizeof(maps));memcpy(oldDescs,descs,sizeof(descs));memcpy(oldPackets,packets,sizeof(packets));
    auto diag=begin(di,1,testCaller,false,nullptr);check(diag!=nullptr);
    check(!memcmp(m,&before,sizeof(before)));qual::give(diag->busy);
    check(!memcmp(oldDi,di,sizeof(di)) && !memcmp(oldMaps,maps,sizeof(maps)) &&
        !memcmp(oldDescs,descs,sizeof(descs)) && !memcmp(oldPackets,packets,sizeof(packets)));
    check(wrapReclaim(di,1)==nullptr);leaveSync(ctx);
    check(stateOf(*m)==MapState::Quarantine && !m->detached && !notifications);
    check(last(Entry).payload[6]==CallerMismatch && !last(Entry).payload[5]);
    // Last empty drain has no mapping; find the tracked outcome separately.
    Event tracked{};for(auto &slot:bvp::ring)if(slot.value.type==TxQualification && slot.value.flags==Outcome && slot.value.payload[1]==m->serial)tracked=slot.value;
    check(tracked.payload[9]==SyncRequeueCapable && tracked.payload[8]==1);
    auto epoch=beginTxReset(queue);qual::reset(queue,reinterpret_cast<uint64_t>(current_thread()),epoch,true);
    check(!completeTxReset(*m,queue,reinterpret_cast<uint64_t>(current_thread()),epoch,true,[](uint64_t,uint64_t){++frees;}));check(!frees);
    testCaller=FreePC;auto good=setup(102);check(wrapReclaim(di,1)==nullptr);
    check(good->detached && good->notified && notifications==1);
    epoch=beginTxReset(queue);
    check(completeTxReset(*good,queue,reinterpret_cast<uint64_t>(current_thread()),epoch,true,[](uint64_t,uint64_t){++frees;}));
    check(frees==1 && stateOf(*good)==MapState::Empty && stateOf(*m)==MapState::Quarantine);
    auto flagged=setup(103);put<uint32_t>(di,0xc,8);check(wrapReclaim(di,1)==nullptr);check(!flagged->detached && notifications==1);
    check(last(Entry).payload[6]==ControlFlag);
    auto uncleared=setup(104);breakClear=true;check(wrapReclaim(di,1)==nullptr);breakClear=false;
    check(!uncleared->detached && notifications==1);
    bool found=false;for(auto &slot:bvp::ring)if(slot.value.type==TxQualification && slot.value.flags==Post && slot.value.payload[1]==uncleared->serial)found=slot.value.payload[2]&DescriptorClear;
    check(found && !mappingsHalted());
    auto prior=*m;lifecycle(*m,FreeAttempt);check(!memcmp(m,&prior,sizeof(prior)));
    auto first=firstFailureState;check(first==0 && !stopAfter);
    // Saturation/nested context loss affects telemetry only, never driver authority.
    int contextSlots[16];for(auto &i:contextSlots)i=enterSync(reinterpret_cast<void *>(55),63,2,osh);
    check(enterSync(reinterpret_cast<void *>(55),63,2,osh)==-1);
    for(auto i:contextSlots)leaveSync(i);
    check(!mappingsHalted());
    check(begin(di,2,FreePC,false,nullptr)==nullptr);
    Observation *held[ScratchCount];
    for(auto &p:held) {p=begin(di,1,FreePC,false,nullptr);check(p!=nullptr);}
    check(begin(di,1,FreePC,false,nullptr)==nullptr);
    for(auto p:held)qual::give(p->busy);
    __atomic_store_n(&trackedRecords,uint64_t(RecordLimit),__ATOMIC_RELAXED);
    check(begin(di,1,FreePC,false,nullptr)==nullptr);
    check(!mappingsHalted() && !firstFailureState && !stopAfter);
    printf("Diagnostic storage: %zu bytes scratch, %zu bytes context, %zu bytes watches; no hot allocation.\n",sizeof(scratch),sizeof(contexts),sizeof(watches));
    printf("PASS TX qualification: %u focused assertions; real reclaim multi-descriptor free/mismatch/flag/clear cases; observation-only context/lifecycle.\n",cases);
}
