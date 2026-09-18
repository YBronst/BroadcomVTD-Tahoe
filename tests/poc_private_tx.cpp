#define BVT_HOST_TEST
#include "../POC/Trace.cpp"
#include "../POC/MapperCore.cpp"
#include "../POC/TxQualification.cpp"
#include "../POC/TxDisposition.cpp"
#include "../POC/TxQuiescence.cpp"
#include "../POC/TxPacket.hpp"
#include "../POC/TxCleanup.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <thread>

namespace bvp {
static unsigned checks;
static void check(bool v) {assert(v);++checks;}
struct Packet {
    alignas(4096) uint8_t bytes[8192] {};
    uint64_t offset=0x123,length=80,maximum=8192;
    Packet *next=nullptr,*nextPacket=nullptr;
    unsigned frees=0,notifications=0;
};
using mbuf_t=Packet *;
static void *mbuf_data(mbuf_t p) {return p->bytes+p->offset;}
static uint64_t mbuf_len(mbuf_t p){return p->length;}
static uint64_t mbuf_maxlen(mbuf_t p){return p->maximum;}
static Packet *mbuf_next(mbuf_t p){return p->next;}
static Packet *mbuf_nextpkt(mbuf_t p){return p->nextPacket;}
static IOMapper mapper;
static IOMapper *deviceMapper=&mapper;
static bool txChainsEnabled=true,resetSuccess=true,resetSeen=true,corruptDetach=false;
static uint32_t resetStatus=0;
static uint64_t imageBase=0,testCaller=0x14709f;
static constexpr uint64_t txForcedReclaimPC=0x2c2f43;
static unsigned txCalls,drains,initCalls,initBeforeReset;
static bool drainMutation=false,drainReentry=false;
static void *currentDi;
template<typename T> static T load(const void *p,size_t off){T v;memcpy(&v,static_cast<const uint8_t *>(p)+off,sizeof(v));return v;}
template<typename T> static void put(void *p,size_t off,T v){memcpy(static_cast<uint8_t *>(p)+off,&v,sizeof(v));}
static bool ourRing(const void *p){return p!=nullptr;}
static bool ours(const void *p){return p!=nullptr;}
static void rememberRing(const void *){}
static void descriptors(const void *,uint32_t,uint32_t,const void *,uint32_t){}
namespace rx {
struct Mapping {};
static bool modeEnabled;
static void enable(bool value){modeEnabled=value;}
static Mapping *find(uint64_t){return nullptr;}
static void quarantine(Mapping &,uint32_t){assert(false);}
}
enum Hook {Tx,Free,Reclaim,Reset,TxInit};
template<typename T,typename... A>static T original(Hook,A...);
template<> void original<void>(Hook,void *,void *,uint32_t);
template<> int32_t original<int32_t>(Hook,void *,void *,uint32_t);
template<> void *original<void *>(Hook,void *,uint32_t);
template<> bool original<bool>(Hook,void *);
template<> void original<void>(Hook,void *);
static void IODelay(unsigned);
#define __builtin_return_address(n) reinterpret_cast<void *>(testCaller)
#include "../build/poc-0.2.25/generated/FrontendPrivate.inc"
#undef __builtin_return_address

template<> void original<void>(Hook h,void *owner,void *packet,uint32_t send) {
    check(h==Free && owner && packet);
    auto p=static_cast<Packet *>(packet);check(!findMapping(reinterpret_cast<uint64_t>(packet)));
    check(++p->frees==1);if(send)check(++p->notifications==1);
    // Recycling packet bytes is deliberately destructive: OLD private storage
    // must remain independent after native software disposition.
    memset(p->bytes,0xee,sizeof(p->bytes));
}
template<> int32_t original<int32_t>(Hook h,void *di,void *packet,uint32_t) {
    check(h==Tx);++txCalls;
    if(!correctiveMode()){check(!findMapping(reinterpret_cast<uint64_t>(packet)));return 73;}
    auto m=findMapping(reinterpret_cast<uint64_t>(packet));check(m && m->descriptor && m->privateRecord.leased);
    check(stateOf(*m)==MapState::Submitting && m->prepared);
    auto plan=txpacket::collect(reinterpret_cast<uint64_t>(packet),true,[](uint64_t v){
        auto p=reinterpret_cast<Packet *>(v);return txpacket::View {reinterpret_cast<uint64_t>(mbuf_data(p)),p->length,p->maximum,
            reinterpret_cast<uint64_t>(p->next),reinterpret_cast<uint64_t>(p->nextPacket)};
    });
    unsigned segment=0;
    for(unsigned i=0;i<plan.count;++i) {
        auto a=plan.ranges[i].address,left=plan.ranges[i].length;
        while(left) {
            auto len=left<4096-(a&4095)?left:4096-(a&4095);
            check(m->lengths[segment]==len && m->offsets[segment]==(a&4095));
            check(!memcmp(reinterpret_cast<void *>(m->pages[segment].address+m->offsets[segment]),reinterpret_cast<void *>(a),len));
            a+=len;left-=len;++segment;
        }
    }
    check(segment==m->count);
    auto maps=load<uint8_t *>(di,0x80),descs=load<uint8_t *>(di,0x58);
    auto packets=load<uint64_t *>(di,0x70);auto n=load<uint16_t>(di,0x6a);
    txpacket::encode(maps+m->startIndex*0x70,*m);
    check(load<uint32_t>(maps+m->startIndex*0x70,8)==static_cast<Packet *>(packet)->length);
    for(unsigned i=0;i<m->count;++i) {
        auto index=(m->startIndex+i)&(n-1);
        put<uint32_t>(descs,index*16+8,uint32_t(m->segments[i].address));
        put<uint32_t>(descs,index*16+12,uint32_t(m->segments[i].address>>32));
        packets[index]=i+1==m->count?reinterpret_cast<uint64_t>(packet):0;
    }
    m->consumed=1;put<uint16_t>(di,0x6e,uint16_t((m->startIndex+m->count)&(n-1)));return 0;
}
template<> void *original<void *>(Hook h,void *di,uint32_t range) {
    check(h==Reclaim && (range==1 || range==2));
    if(!correctiveMode())return di;
    auto n=load<uint16_t>(di,0x6a),in=load<uint16_t>(di,0x6c),out=load<uint16_t>(di,0x6e);
    if(in==out)return nullptr;
    auto maps=load<uint8_t *>(di,0x80),descs=load<uint8_t *>(di,0x58);
    auto packets=load<uint64_t *>(di,0x70);
    auto span=load<uint32_t>(maps,in*0x70+12);check(span && span<=8);
    uint64_t result=0;
    for(unsigned j=0;j<span;++j) {
        auto i=(in+j)&(n-1);result=packets[i];packets[i]=0;
        put<uint32_t>(descs,i*16+8,corruptDetach?0:0xdeadbeefU);put<uint32_t>(descs,i*16+12,0xdeadbeefU);
    }
    put<uint16_t>(di,0x6c,uint16_t((in+span)&(n-1)));return reinterpret_cast<void *>(result);
}
template<> bool original<bool>(Hook h,void *) {
    check(h==Reset);
    if(txResetObservation.thread) {txResetObservation.seen=resetSeen;txResetObservation.status=resetStatus;}
    if(resetStatus==0xffffffffU)haltMappings(18); // actual read-policy separately compiled/audited
    return resetSuccess;
}
template<> void original<void>(Hook h,void *di) {
    check(h==TxInit);++initCalls;
    put<uint16_t>(di,0x6c,0);put<uint16_t>(di,0x6e,0);
    memset(load<void *>(di,0x58),0,load<uint16_t>(di,0x6a)*16);
}
static void IODelay(unsigned us) {
    check(us==privateTx::ExperimentalDrainUS && us==300);++drains;
    check(initCalls==initBeforeReset); // Next native init has not started inside the delay.
    if(drainMutation)privateTx::invalidate(reinterpret_cast<uint64_t>(currentDi),false);
    if(drainReentry)wrapTxInit(currentDi); // exclusive lease refuses and halts, NO init
}
}

int main(int argc,char **argv) {
    using namespace bvp;check(argc==2);const std::string mode=argv[1];
    failPrivateAllocation=mode=="allocation";
    if(mode=="native" || mode=="native-specific") {
        // Actual mode configuration + actual frontend fast paths; force pool
        // allocation failure to prove native mode does not depend on the pool.
        failPrivateAllocation=true;
        auto source=mode=="native"?selectMapperSource(0,false,0,0,false):
            selectMapperSource(0x2000,true,0x4000,0x4000,true);
        configureCorrection(source);
        check(!correctiveMode() && !privateTx::enabled() && !privateTx::pool);
        check(!rx::modeEnabled && !txCleanupEnabled());
        Packet p;int nativeRing=0;
        check(wrapTx(&nativeRing,&p,1)==73 && txCalls==1);
        check(wrapReclaim(&nativeRing,1)==&nativeRing);
        check(wrapReset(&nativeRing) && !drains);
        wrapFree(&nativeRing,&p,1);check(p.frees==1 && p.notifications==1);
        check(!IOMemoryDescriptor::live && !mappingsHalted());
        Event totals;snapshotMapping(64,totals);check(totals.payload[17]==(1ULL<<60));
        std::cout<<"PASS private TX "<<mode<<": "<<checks<<" checks\n";return 0;
    }
    configureCorrection(selectMapperSource(0,false,0x4000,0x4000,true));
    check(correctiveMode() && privateTx::enabled() && rx::modeEnabled && txCleanupEnabled());
    check(bool(privateTx::pool)==!failPrivateAllocation);
    configureCorrection(MapperSource::Unavailable);check(correctiveMode()); // No unsafe downgrade.
    IOMemoryDescriptor::uniqueAddresses=true;
    static Packet packet[70];
    static uint8_t di[0x200] {},owner[0x80] {},mapTable[2048*0x70] {},descs[2048*16] {};
    static uint64_t packetTable[2048] {};
    currentDi=di;
    put<void *>(di,0x30,owner);put<void *>(di,0x48,di+0x180);put<void *>(di,0x58,descs);
    put<void *>(di,0x70,packetTable);put<void *>(di,0x80,mapTable);put<uint16_t>(di,0x6a,2048);
    put<uint32_t>(di,0x11c,0x7fff);
    auto submit=[&](unsigned i) {
        memset(packet[i].bytes,uint8_t(0x31+i),sizeof(packet[i].bytes));
        check(wrapTx(di,&packet[i],1)==0);auto m=findMapping(reinterpret_cast<uint64_t>(&packet[i]));check(m!=nullptr);return m;
    };
    auto reclaim=[&](unsigned i) {
        auto m=findMapping(reinterpret_cast<uint64_t>(&packet[i]));check(m!=nullptr);
        check(wrapReclaim(di,1)==&packet[i]);
        check(!m->packet && m->privateRecord.pending && m->privateRecord.leased);
        check(!findMapping(reinterpret_cast<uint64_t>(&packet[i])) && m->prepared && m->descriptor);return m;
    };
    auto reset=[&]() {
        privateTx::observeRing(privateGeometry(di),49);testCaller=0x10716c;
        initBeforeReset=initCalls;
        check(wrapReset(di)==resetSuccess);
    };
    if(mode=="bulk-cycles" || mode=="bulk-requeue" || mode=="bulk-failure" || mode=="bulk-callers") {
        // Exact native bulk ordering: all TX resets, then RX/core work, then
        // range-1 reclaim/free; init is LATER, never between the six resets.
        // Observe revision ONCE, then exercise init -> active TX -> bulk reset
        // without manufacturing a new flush/revision observation per cycle.
        struct Storage {
            uint8_t di[0x200] {},maps[2048*0x70] {},descriptors[2048*16] {};
            uint64_t packets[2048] {};
        };
        static Storage extra[5];void *rings[6]={di};
        for(unsigned i=1;i<6;++i) {
            auto &s=extra[i-1];rings[i]=s.di;
            put<void *>(s.di,0x30,owner);put<void *>(s.di,0x48,s.di+0x180);
            put<void *>(s.di,0x58,s.descriptors);put<void *>(s.di,0x70,s.packets);
            put<void *>(s.di,0x80,s.maps);put<uint16_t>(s.di,0x6a,2048);
            put<uint32_t>(s.di,0x11c,0x7fff);
        }
        for(auto ring:rings) {privateTx::observeRing(privateGeometry(ring),49);wrapTxInit(ring);}
        const uint64_t callers[]={0xfb7e3,0x10716c,0x10f6d4,0x89198,0x892a5,0x89e76};
        uint64_t priorSerial[6]={},priorIOVA[6]={};
        const unsigned cycles=mode=="bulk-cycles"?80:mode=="bulk-callers"?6:2;
        for(unsigned cycle=0;cycle<cycles;++cycle) {
            Mapping *active[6];
            for(unsigned i=0;i<6;++i) {
                packet[i].frees=packet[i].notifications=0;
                check(wrapTx(rings[i],&packet[i],1)==0);
                auto m=findMapping(reinterpret_cast<uint64_t>(&packet[i]));active[i]=m;check(m);
                check(m->serial>priorSerial[i] && m->segments[0].address!=priorIOVA[i]);
                priorSerial[i]=m->serial;priorIOVA[i]=m->segments[0].address;
            }
            initBeforeReset=initCalls;testCaller=mode=="bulk-callers"?callers[cycle]:0xfb7e3;
            for(unsigned i=0;i<6;++i) {
                currentDi=rings[i];resetSuccess=!(mode=="bulk-failure" && cycle==0 && i==2);
                const auto before=drains;check(wrapReset(rings[i])==resetSuccess);
                check(initCalls==initBeforeReset);check(!packet[i].frees && !packet[i].notifications);
                if(resetSuccess) {
                    check(drains==before+1 && !active[i]->descriptor && !active[i]->packet);
                    check(!active[i]->privateRecord.leased);
                } else {
                    check(drains==before && active[i]->descriptor && active[i]->packet);
                }
            }
            resetSuccess=true;testCaller=txForcedReclaimPC;
            for(unsigned i=0;i<6;++i) {
                if(mode=="bulk-failure" && i==2) {
                    check(wrapReclaim(rings[i],1)==nullptr);check(active[i]->descriptor);
                } else {
                    check(wrapReclaim(rings[i],1)==&packet[i]);
                    if(mode!="bulk-requeue")wrapFree(owner,&packet[i],1);
                    check(packet[i].frees==(mode=="bulk-requeue"?0U:1U));
                    check(packet[i].notifications==packet[i].frees);
                }
            }
            for(auto ring:rings)wrapTxInit(ring);
            Event total;snapshotMapping(64,total);
            if(mode=="bulk-failure") {
                check(total.payload[2]==1 && IOMemoryDescriptor::live==1);
                currentDi=rings[2];testCaller=0xfb7e3;initBeforeReset=initCalls;
                check(wrapReset(rings[2]));check(active[2]->descriptor && active[2]->packet);
                check(IOMemoryDescriptor::live==1);break; // Never retroactive success.
            }
            check(!total.payload[2] && !noCreditEvents && !IOMemoryDescriptor::live);
        }
        check(!mappingsHalted());
        if(mode!="bulk-failure")check(IOMemoryDescriptor::completes==6*cycles);
    } else if(mode=="revision-unknown" || mode=="revision-changed" || mode=="revision-fatal") {
        if(mode!="revision-unknown")privateTx::observeRing(privateGeometry(di),49);
        wrapTxInit(di);
        if(mode=="revision-changed")put<uint32_t>(di,0xa0,0x10000); // geometry breaks provenance
        auto m=submit(0);
        if(mode=="revision-fatal")privateTx::invalidate(reinterpret_cast<uint64_t>(di),true);
        testCaller=0xfb7e3;initBeforeReset=initCalls;
        check(wrapReset(di));check(!drains && m->descriptor && m->packet);
        check(!IOMemoryDescriptor::completes);
    } else if(mode=="golden") {
        packet[0].next=&packet[1];packet[1].offset=4090;packet[1].length=1460;
        for(unsigned i=0;i<578;++i) {
            auto m=submit(0);check(m->count==3 && m->bytes==1540 && m->inputLength==80);
            check(m->offsets[0]==0x123 && m->offsets[1]==4090 && m->offsets[2]==0);
            check(m->lengths[0]==80 && m->lengths[1]==6 && m->lengths[2]==1454);
            for(unsigned j=0;j<m->count;++j)check(m->segments[j].address%4096==m->offsets[j]);
            check(wrapReclaim(di,2)==&packet[0]);check(stateOf(*m)==MapState::Empty && !m->privateRecord.leased);
        }
        check(IOMemoryDescriptor::completes==578 && IOMemoryDescriptor::live==0);
    } else if(mode=="allocation" || mode=="prepare") {
        if(mode=="prepare")IOMemoryDescriptor::failPrepare=true;
        check(wrapTx(di,&packet[0],1)==-1 && !txCalls);
        check(packet[0].frees==1 && packet[0].notifications==1 && !IOMemoryDescriptor::live);
        check(!mappingsHalted());
    } else if(mode=="segment-failure") {
        IOMemoryDescriptor::failSegment=true;
        check(wrapTx(di,&packet[0],1)==-1 && !txCalls && packet[0].frees==1);
        check(!IOMemoryDescriptor::live && IOMemoryDescriptor::completes==1 && !mappingsHalted());
    } else if(mode=="pressure") {
        for(unsigned i=0;i<64;++i){submit(i);reclaim(i);}
        check(wrapTx(di,&packet[64],1)==-1 && noCreditEvents==1 && !mappingsHalted());
        check(IOMemoryDescriptor::live==64 && !IOMemoryDescriptor::completes);
        reset();check(!mappingsHalted() && !IOMemoryDescriptor::live && IOMemoryDescriptor::completes==64);
        wrapTxInit(di);auto m=submit(65);check(wrapReclaim(di,2)==&packet[65]);check(!m->privateRecord.leased);
        check(reservationsAfterPressure==1 && submissionsAfterPressure==1);
    } else if(mode=="bad-detach") {
        auto m=submit(0);corruptDetach=true;check(wrapReclaim(di,1)==nullptr);
        check(m->packet && !m->privateRecord.pending && m->cleanupBlocked);
        reset();check(m->descriptor && m->privateRecord.leased && !IOMemoryDescriptor::completes);
    } else if(mode=="split") {
        auto old=submit(0);auto oldSerial=old->serial,oldIOVA=old->segments[0].address,oldBacking=old->privateRecord.backing;
        reclaim(0);auto oldDescriptor=old->descriptor;
        check(*reinterpret_cast<uint8_t *>(oldBacking+0x123)==0x31);
        wrapFree(owner,&packet[0],1);check(packet[0].frees==1 && !IOMemoryDescriptor::completes);
        check(*reinterpret_cast<uint8_t *>(oldBacking+0x123)==0x31); // Native recycle cannot overwrite OLD bytes.
        packet[0].frees=packet[0].notifications=0;
        auto fresh=submit(0);check(old!=fresh && oldSerial!=fresh->serial && oldDescriptor!=fresh->descriptor);
        check(oldIOVA!=fresh->segments[0].address && oldBacking!=fresh->privateRecord.backing);
        check(wrapReclaim(di,2)==&packet[0]);check(old->descriptor && old->privateRecord.pending);
        auto second=submit(1);reclaim(1);
        reset();check(!old->descriptor && !second->descriptor && IOMemoryDescriptor::completes==3);
        check(packet[0].frees==0 && packet[1].frees==0); // No synthetic packet disposal at reset.
        wrapTxInit(di);auto after=submit(0);check(after->serial>oldSerial);
        check(wrapReclaim(di,2)==&packet[0]);
    } else if(mode=="newer-active") {
        auto old=submit(0);reclaim(0);auto fresh=submit(0);
        check(old!=fresh && old->serial<fresh->serial);
        // A later serial BEFORE this reset fence is still OLD-generation DMA,
        // not a post-ticket/new-generation transaction. Both may now retire.
        reset();check(!old->descriptor && !fresh->descriptor && !fresh->privateRecord.leased);
        check(!fresh->packet && !fresh->privateRecord.pending);
        check(IOMemoryDescriptor::completes==2 && !packet[0].frees);
    } else if(mode=="active-cycles") {
        for(unsigned i=0;i<80;++i) {
            packet[0].frees=packet[0].notifications=0;
            auto m=submit(0);const auto serial=m->serial,iova=m->segments[0].address;
            reset();check(!m->descriptor && !m->packet && !m->privateRecord.leased);
            check(!packet[0].frees && !packet[0].notifications);
            // Native packet slot is untouched by BroadcomVTD retirement.
            check(packetTable[m->startIndex]==reinterpret_cast<uint64_t>(&packet[0]));
            wrapTxInit(di);wrapFree(owner,&packet[0],1);
            check(packet[0].frees==1 && packet[0].notifications==1);
            packet[0].frees=packet[0].notifications=0;
            auto fresh=submit(0);check(fresh->serial>serial && fresh->segments[0].address!=iova);
            check(wrapReclaim(di,2)==&packet[0]);
            Event totals;snapshotMapping(64,totals);check(!totals.payload[2] && !noCreditEvents);
        }
        check(IOMemoryDescriptor::completes==160 && !mappingsHalted());
    } else if(mode=="active-requeue") {
        auto old=submit(0);auto serial=old->serial,iova=old->segments[0].address;
        reset();check(!findMapping(reinterpret_cast<uint64_t>(&packet[0])));
        // Native range-1 software reclaim AFTER reset can return the packet
        // normally: no stale Mapping shadows it, no plugin disposal needed.
        check(wrapReclaim(di,1)==&packet[0]);wrapTxInit(di);
        auto fresh=submit(0);check(fresh->serial>serial && fresh->segments[0].address!=iova);
        check(!packet[0].frees && !packet[0].notifications);
        check(wrapReclaim(di,2)==&packet[0]);
    } else if(mode.compare(0,7,"active-")==0) {
        auto old=submit(0);const auto key=mode.substr(7);
        if(key=="false")resetSuccess=false;
        else if(key=="unseen")resetSeen=false;
        else if(key=="all-ones")resetStatus=0xffffffffU;
        else if(key=="zero")put<uint16_t>(di,0x6a,0);
        else if(key=="halt")haltMappings(18);
        else if(key=="blocked")old->cleanupBlocked=1;
        else if(key=="prequarantine")markQuarantine(*old,15);
        else if(key=="mutation")drainMutation=true;
        else if(key=="reentry")drainReentry=true;
        else if(key=="complete-failure")IOMemoryDescriptor::failComplete=true;
        else if(key=="new-generation" || key=="fence" || key=="wrong-thread" || key=="owner" || key=="ring") {
            privateTx::observeRing(privateGeometry(di),49);
            auto t=privateTx::beginTerminal(privateGeometry(di),reinterpret_cast<uint64_t>(current_thread()),0x10716c,true);
            check(t.ticket && old->privateRecord.activeResetTicket==t.ticket);
            beginTxReset(reinterpret_cast<uint64_t>(di));
            if(key=="new-generation")++old->privateRecord.generation;
            if(key=="fence")old->serial=t.fence+1;
            if(key=="owner")++old->owner;
            if(key=="ring")++old->queue;
            if(key=="wrong-thread") {std::thread thread([&]{check(!privateTx::finishTerminal(t,true,true,0,privateGeometry,IODelay));});thread.join();}
            else check(!privateTx::finishTerminal(t,true,true,0,privateGeometry,IODelay));
            check(old->descriptor && old->packet && old->privateRecord.leased);
            check(!IOMemoryDescriptor::completes);
            std::cout<<"PASS private TX "<<mode<<": "<<checks<<" checks\n";return 0;
        } else check(false);
        reset();check(old->descriptor && old->packet && old->privateRecord.leased);
        check(!old->privateRecord.pending && !packet[0].frees && !packet[0].notifications);
        check(IOMemoryDescriptor::completes==(key=="complete-failure"?1U:0U));
    } else if(mode=="two-generations") {
        auto old=submit(0);reclaim(0);resetSuccess=false;reset();wrapTxInit(di);
        auto fresh=submit(0);reclaim(0);check(old->privateRecord.generation!=fresh->privateRecord.generation);
        resetSuccess=true;reset();check(old->descriptor && !fresh->descriptor);
        check(old->privateRecord.pending && IOMemoryDescriptor::completes==1);
    } else {
        auto old=submit(0);reclaim(0);auto generation=old->privateRecord.generation;
        privateTx::observeRing(privateGeometry(di),49);
        if(mode=="reset-false")resetSuccess=false;
        else if(mode=="unseen")resetSeen=false;
        else if(mode=="all-ones")resetStatus=0xffffffffU;
        else if(mode=="zero")put<uint16_t>(di,0x6a,0);
        else if(mode=="generation")++old->privateRecord.generation;
        else if(mode=="owner")++old->privateRecord.geometry.owner;
        else if(mode=="ring")++old->privateRecord.geometry.queue;
        else if(mode=="stale")privateTx::invalidate(reinterpret_cast<uint64_t>(di),false);
        else if(mode=="halt")haltMappings(18);
        else if(mode=="mutation")drainMutation=true;
        else if(mode=="reentry")drainReentry=true;
        else if(mode=="complete-failure")IOMemoryDescriptor::failComplete=true;
        else if(mode=="concurrent") {
            check(txAdmission.read());std::thread resetter(reset);resetter.join();txAdmission.leave(false);
            check(!drains && old->descriptor && old->privateRecord.leased);
            std::cout<<"PASS private TX "<<mode<<": "<<checks<<" checks\n";return 0;
        } else if(mode=="new-generation" || mode=="wrong-thread") {
            auto g=privateGeometry(di);auto t=privateTx::beginTerminal(g,reinterpret_cast<uint64_t>(current_thread()),0x10716c,true);check(t.ticket!=0);
            // Tests the real selector with a NEW transaction record. No production
            // TX is permitted inside this exclusive reset, but a selector bug
            // must still not retire a newer generation even if supplied one.
            if(mode=="new-generation")++old->privateRecord.generation;
            if(mode=="wrong-thread") {std::thread other([&]{check(privateTx::finishTerminal(t,true,true,0,privateGeometry,IODelay)==0);});other.join();}
            else check(privateTx::finishTerminal(t,true,true,0,privateGeometry,IODelay)==0);
            check(!t.ticket && privateTx::finishTerminal(t,true,true,0,privateGeometry,IODelay)==0);
            check(old->descriptor && old->privateRecord.leased && !IOMemoryDescriptor::completes);
            std::cout<<"PASS private TX "<<mode<<": "<<checks<<" checks\n";return 0;
        } else check(mode=="duplicate");
        reset();
        if(mode=="duplicate") {
            check(!old->descriptor && IOMemoryDescriptor::completes==1);reset();check(IOMemoryDescriptor::completes==1);
        } else {
            check(old->descriptor && old->privateRecord.leased && old->privateRecord.pending);
            check(IOMemoryDescriptor::completes==(mode=="complete-failure"?1U:0U));
            if(mode=="reset-false" || mode=="unseen" || mode=="all-ones" || mode=="zero" || mode=="halt")check(!drains);
        }
        check(!packet[0].frees && !packet[0].notifications);
        (void)generation;
    }
    std::cout<<"PASS private TX "<<mode<<": "<<checks<<" checks\n";
}
