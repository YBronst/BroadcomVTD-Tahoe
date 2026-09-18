// Exact-resource EXPERIMENTAL TX mapper frontend. See POC/README.md.
#include "Trace.hpp"
#include "ReadPolicy.hpp"
#include "TargetGate.hpp"
#include "BinaryGate.hpp"
#include "Acquisition.hpp"
#include "MapperCore.hpp"
#include "MapperSelection.hpp"
#include "RxObservation.hpp"
#include "RxMapperCore.hpp"
#include "RxGeneration.hpp"
#include "TxCleanup.hpp"
#include "TxQualification.hpp"
#include "TxDisposition.hpp"
#include "TxQuiescence.hpp"
#include "TxPacket.hpp"
#include "TxStatusWord.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/plugin_start.hpp>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <kern/thread.h>
#include <sys/kpi_mbuf.h>
#include <mach-o/loader.h>

namespace bvp {
static mach_vm_address_t originals[HookCount];
static bool armed;
static bool registered;
static uint64_t selectedPCI, selectedController;
static uint64_t controllerVtable;
static bool lateBinding;
static IONotifier *boundNotifier; // Resident with this non-unloadable Lilu probe.
static uint32_t dispatchRecords;
static uint64_t imageBase;
static IOMapper *deviceMapper; // Retained for this boot, including quarantine.
static IOPCIDevice *retainedProvider;
static constexpr bool txChainsEnabled=true;
static constexpr bool nativeTxStatusWord=true;
static constexpr bool scopedReadPolicy=true;
static const char *paths[] = {
    "/System/Library/Extensions/IO80211FamilyLegacy.kext/Contents/PlugIns/AirPortBrcmNIC.kext/Contents/MacOS/AirPortBrcmNIC"
};
static KernelPatcher::KextInfo target {"com.apple.driver.AirPort.BrcmNIC", paths, 1, {}};

template <typename T> static T load(const void *p, size_t off) {
    T value; memcpy(&value, static_cast<const uint8_t *>(p)+off, sizeof(value)); return value;
}
template <typename T, typename... Args> static T original(Hook h, Args... args) {
    return reinterpret_cast<T (*)(Args...)>(originals[h])(args...);
}
static bool active() { return __atomic_load_n(&armed, __ATOMIC_ACQUIRE); }
// BVD-BEGIN: bounded mbuf tag lookup and an ABI-pinned read-only priority getter.
static bool dispositionTag(uint64_t packet,dispo::Tag &out) {
    size_t length=0;void *tag=nullptr;
    const auto id=load<uint32_t>(reinterpret_cast<void *>(imageBase+dispositionTagIDOffset),0);
    if (mbuf_tag_find(reinterpret_cast<mbuf_t>(packet),id,0,&length,&tag) || !tag || length<0x18) return false;
    out.cachedScb=load<uint64_t>(tag,0x10);
    out.flags1=load<uint8_t>(tag,1)&0x85;out.flags7=load<uint8_t>(tag,7)&0x10;
    out.bssIndex=load<int8_t>(tag,9);
    out.priority=reinterpret_cast<uint64_t (*)(void *)>(imageBase+dispositionPriorityOffset)(reinterpret_cast<void *>(packet));
    return true;
}
// BVD-END
static uint64_t selected() { return __atomic_load_n(&selectedPCI, __ATOMIC_ACQUIRE); }
static uint64_t controller() { return __atomic_load_n(&selectedController, __ATOMIC_ACQUIRE); }
static void setGate(uint32_t value) { __atomic_store_n(&gateStatus, value, __ATOMIC_RELEASE); }
static bool ours(const void *osh) {
    // osh is a live argument of a hash/layout-gated driver-local route, never
    // discovered by dereferencing a possibly stale private controller field.
    uint64_t pci = selected();
    if (!correctiveMode() || !active() || !pci || !osh || load<uint32_t>(osh,0)!=0x1234abcd ||
        load<uint64_t>(osh,8)!=pci) return false;
    if (tagGet(osh,Osl)==pci) return true;
    if (!__atomic_load_n(&lateBinding,__ATOMIC_ACQUIRE)) return false;
    // Missed osl_attach is explicitly incomplete history. Only our metadata is
    // changed. Rechecking tagGet preserves the fail-closed uncertainty latch.
    if (!tagSet(osh,Osl,pci) || tagGet(osh,Osl)!=pci) return false;
    auto e=event(OslBound,osh,nullptr,reinterpret_cast<void *>(pci),1); emit(e);
    return true;
}
static bool ourRing(const void *di) { return active() && selected() && di && ours(load<void *>(di, 0x30)); }

static privateTx::Geometry privateGeometry(const void *di) {
    privateTx::Geometry g;
    if(!di)return g;
    g.queue=reinterpret_cast<uint64_t>(di);g.owner=load<uint64_t>(di,0x30);
    g.regs=load<uint64_t>(di,0x48);g.maps=load<uint64_t>(di,0x80);
    g.packets=load<uint64_t>(di,0x70);g.descs=load<uint64_t>(di,0x58);
    g.count=load<uint16_t>(di,0x6a);g.base=load<uint32_t>(di,0xa0);g.mask=load<uint32_t>(di,0x11c);
    return g;
}
static tx::Admission txAdmission;
class TxLease {
    bool required=false, held=false, writer=false;
public:
    TxLease(bool targetMatches,const void *object,uint32_t operation,bool reset=false) {
        required=(txCleanupEnabled() || privateTx::enabled()) && targetMatches;
        if (!required) return;
        if (reset) { writer=txAdmission.write(); held=writer; }
        if (!held) held=txAdmission.read();
        if (!held) {
            haltMappings(24);
            auto e=event(TxCleanupDenied,object,nullptr,nullptr,operation); emit(e);
        } else if (reset && !writer) {
            // Other TX work is in progress: preserve the original reset, but
            // certify/release NOTHING. This is coverage, not ownership proof.
            auto e=event(TxCleanupDenied,object,nullptr,nullptr,100); emit(e);
        }
    }
    ~TxLease() { if (held) txAdmission.leave(writer); }
    bool allowed() const { return !required || held; }
    bool exclusive() const { return required && held && writer; }
};
struct TxResetObservation {
    uint64_t thread=0,owner=0,reg=0,epoch=0;
    uint32_t seen=0,status=0xffffffffU;
};
// One global exclusive reset/cleanup transaction, never a wait or hot allocation.
static TxResetObservation txResetObservation;
static void releaseResetPacket(uint64_t owner,uint64_t packet) {
    // The send=1 notification already ran at the original reclaim point.
    // Pinned osl_pktfree send=0 follows the identical backing free/recycle path
    // without repeating a callback into potentially changed higher-level state.
    auto e=event(FreeEnter,reinterpret_cast<void *>(owner),reinterpret_cast<void *>(packet),nullptr,0); emit(e);
    original<void>(Free,reinterpret_cast<void *>(owner),reinterpret_cast<void *>(packet),uint32_t(0));
    e=event(FreeExit,reinterpret_cast<void *>(owner),reinterpret_cast<void *>(packet),nullptr,0); emit(e);
}

// RX observation remains passive in TX-only mode. Opt-in RX uses the proven
// mapper path; 0.2.3 adds positively acknowledged, software-detached cleanup.
static uint32_t rxRecords;
static uint64_t rxFillCalls, rxReclaimCalls, rxStatusCalls, rxMapCalls;
static rx::OperationTable rxOperations;
class RxLease {
    rx::OperationSlot *slot=nullptr;
    bool nested=false, required=false;
public:
    RxLease(void *di,rx::Operation op) {
        required=rx::enabled() && ourRing(di);
        if (!required) return;
        slot=rxOperations.enter(reinterpret_cast<uint64_t>(di),reinterpret_cast<uint64_t>(current_thread()),op,nested);
        if (!slot) {
            rx::quarantineQueue(reinterpret_cast<uint64_t>(di),20); rx::halt(20);
            auto e=event(RxCleanupDenied,di,nullptr,nullptr,uint32_t(op)); emit(e);
        }
    }
    ~RxLease() { rxOperations.leave(slot,nested); }
    bool allowed() const { return !required || slot; }
};
// Observe only the original reset's reads. No added MMIO transaction or wait.
struct RxResetObservation {
    uint32_t busy=0,seen=0,status=0xffffffffU;
    uint64_t thread=0,owner=0,reg=0,epoch=0;
};
static RxResetObservation rxResetObservations[8];
static RxResetObservation *beginRxResetObservation(void *di,uint64_t epoch) {
    for (auto &r:rxResetObservations) {
        uint32_t zero=0;
        if (__atomic_compare_exchange_n(&r.busy,&zero,1,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) {
            r.owner=load<uint64_t>(di,0x30); r.reg=load<uint64_t>(di,0x50)+0x10;
            r.epoch=epoch; r.seen=0; r.status=0xffffffffU;
            __atomic_store_n(&r.thread,reinterpret_cast<uint64_t>(current_thread()),__ATOMIC_RELEASE); return &r;
        }
    }
    return nullptr;
}
// The candidate mode never changes TX mapping-table state or mapper selection.
// Synchronous per-thread fill context: no pointer outlives the original fill.
struct RxContext { uint32_t busy; uint64_t thread; void *di,*osh; };
static RxContext rxContexts[8];
static RxContext *rxContext(void *osh) {
    uint64_t thread=reinterpret_cast<uint64_t>(current_thread());
    for (auto &c:rxContexts) if (__atomic_load_n(&c.thread,__ATOMIC_ACQUIRE)==thread && c.osh==osh) return &c;
    return nullptr;
}
static void rxReject(const void *di,const void *packet,uint32_t reason) {
    auto e=event(RxMapperRejected,di,packet,nullptr,reason); emit(e);
}
static RxContext *beginRx(void *di) {
    auto osh=load<void *>(di,0x30);
    uint32_t n=load<uint16_t>(di,0xa4),size=load<uint16_t>(di,0xe4);
    uint32_t extra=size>=205?load<uint32_t>(di,0xe8):0;
    if (rx::halted() || !deviceMapper || !n || n>4096 || (n&(n-1)) || !size ||
        size>MaxPacketBytes || extra>MaxPacketBytes-size || size+extra>MaxPacketBytes-15 ||
        load<uint16_t>(di,0xa6)>=n || load<uint16_t>(di,0xa8)>=n ||
        !load<void *>(di,0x60) || !load<void *>(di,0xb0) || !load<void *>(di,0xc0) ||
        load<uint8_t>(di,0x42) || load<uint8_t>(di,0x136) || load<uint32_t>(di,0xfc) || rxContext(osh)) {
        rxReject(di,nullptr,2); return nullptr;
    }
    for (auto &c:rxContexts) {
        uint32_t zero=0;
        if (__atomic_compare_exchange_n(&c.busy,&zero,1,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) {
            c.di=di; c.osh=osh;
            __atomic_store_n(&c.thread,reinterpret_cast<uint64_t>(current_thread()),__ATOMIC_RELEASE); return &c;
        }
    }
    rxReject(di,nullptr,3); return nullptr;
}
static void *wrapRxAllocate(void *osh,uint32_t length,int32_t allocationType) {
    if(!correctiveMode())return original<void *>(RxAllocate,osh,length,allocationType);
    const uint64_t caller=reinterpret_cast<uint64_t>(__builtin_return_address(0))-imageBase;
    auto context=rx::enabled() && ours(osh) && (caller==rxAllocateFirstPC || caller==rxAllocateNextPC)?rxContext(osh):nullptr;
    if (!context) return original<void *>(RxAllocate,osh,length,allocationType);
    if (rx::halted()) { rxReject(context->di,nullptr,1); return nullptr; }
    void *packet=original<void *>(RxAllocate,osh,length,allocationType);
    if (!packet) return nullptr;
    auto mb=reinterpret_cast<mbuf_t>(packet);
    auto reject=[&](uint32_t why,rx::Mapping *m=nullptr) -> void * {
        rxReject(context->di,packet,why);
        // An allocator returning an already-owned identity is not a new packet
        // we may free. Preserve either core's ownership even on admission error.
        if (!m && (rx::find(reinterpret_cast<uint64_t>(packet)) || findMapping(reinterpret_cast<uint64_t>(packet)))) {
            rx::halt(19); return nullptr;
        }
        if (!m || rx::abortUnpublished(*m)) original<void>(Free,osh,packet,uint32_t(0));
        else rx::quarantine(*m,why);
        return nullptr; // Original RX-fill allocation-failure branch, BEFORE DMA publication.
    };
    uint64_t data=reinterpret_cast<uint64_t>(mbuf_data(mb));
    uint32_t size=load<uint16_t>(context->di,0xe4);
    uint32_t alignment=(load<uint32_t>(context->di,0xc)&0x10)?16:1;
    uint32_t extra=size>=205?load<uint32_t>(context->di,0xe8):0;
    uint64_t pull=((alignment-(data&(alignment-1)))&(alignment-1))+extra;
    if (!data || mbuf_next(mb) || mbuf_nextpkt(mb) || mbuf_maxlen(mb)>MaxPacketBytes ||
        length>MaxPacketBytes || mbuf_len(mb)<length || pull+size>length || data+pull<data ||
        data+pull+size<data) return reject(4);
    auto m=rx::reserve(reinterpret_cast<uint64_t>(osh),reinterpret_cast<uint64_t>(context->di),reinterpret_cast<uint64_t>(packet));
    if (!m) return reject(5);
    if (!rx::prepare(*m,deviceMapper,data+pull,size)) return reject(6,m);
    if ((m->address>>32)+load<uint32_t>(context->di,0x100)>0xffffffffULL ||
        ((m->address+size-1)>>32)+load<uint32_t>(context->di,0x100)>0xffffffffULL) return reject(7,m);
    return packet;
}
static bool rxObserving() { return __atomic_load_n(&rxRecords,__ATOMIC_RELAXED)<=RxRecordLimit; }
static void emitRx(Event &e, uint64_t ordinal) {
    bool limit,contended;
    if (reserveRxRecord(rxRecords,limit,contended)) {
        e.payload[17]=ordinal; emit(e);
    } else if (limit) {
        auto end=event(RxObservationLimit); end.payload[0]=RxRecordLimit; emit(end);
    } else if (contended) countTableDrop();
}
static Event rxRingEvent(uint32_t type, const void *di, const void *packet=nullptr, uint32_t flags=0) {
    auto e=event(type,di,packet,load<void *>(di,0x30),flags);
    e.payload[0]=load<uint16_t>(di,0xa4); // nrxd
    e.payload[1]=load<uint16_t>(di,0xa6); // rxin
    e.payload[2]=load<uint16_t>(di,0xa8); // rxout
    e.payload[3]=load<uint64_t>(di,0x60); // CPU descriptors; not packet data
    e.payload[4]=load<uint64_t>(di,0x50); // MMIO identity, never dereferenced here
    e.payload[5]=load<uint64_t>(di,0xc0); // RX map-record vector
    e.payload[6]=load<uint16_t>(di,0x128); // cached hardware completion index
    e.payload[7]=load<uint64_t>(di,0x28); // bounded name
    e.payload[8]=load<uint32_t>(di,0xfc);
    e.payload[9]=load<uint32_t>(di,0x100);
    e.payload[10]=load<uint16_t>(di,0xe4); // RX buffer length
    e.payload[11]=load<uint32_t>(di,0xec); // configured post target
    e.payload[12]=load<uint8_t>(di,0x42);
    e.payload[13]=load<uint8_t>(di,0x136);
    e.payload[14]=load<uint32_t>(di,0x130); // available slots
    e.payload[15]=load<uint32_t>(di,0x14); // allocation failure counter
    e.payload[16]=load<uint32_t>(di,0xe0); // RX descriptor pointer base
    return e;
}
static bool wrapRxFill(void *di) {
    if(!correctiveMode())return original<bool>(RxFill,di);
    RxLease lease(di,rx::Operation::Fill);
    if (!lease.allowed()) return false;
    bool targetRing=ourRing(di); RxContext *context=nullptr;
    if (targetRing && rx::enabled()) rx::invalidateQueue(reinterpret_cast<uint64_t>(di));
    if (targetRing && rx::enabled()) { context=beginRx(di); if (!context) return false; }
    bool match=rxObserving() && targetRing;
    uint64_t ordinal=0; uint32_t before=0;
    if (match) {
        ordinal=__atomic_add_fetch(&rxFillCalls,1,__ATOMIC_RELAXED);
        before=load<uint16_t>(di,0xa8);
        if (sparseRxPoll(ordinal)) { auto e=rxRingEvent(RxFillEnter,di); emitRx(e,ordinal); }
    }
    bool result=original<bool>(RxFill,di);
    if (context) { __atomic_store_n(&context->thread,0,__ATOMIC_RELEASE); __atomic_store_n(&context->busy,0,__ATOMIC_RELEASE); }
    if (match) {
        uint32_t n=load<uint16_t>(di,0xa4),after=load<uint16_t>(di,0xa8);
        uint32_t count=rxDescriptorCount(n,before,after);
        if (count || !result || sparseRxPoll(ordinal)) {
            auto e=rxRingEvent(RxFillExit,di,nullptr,result); emitRx(e,ordinal);
        }
        // Original fill has already published the tail. Observe CPU metadata
        // only; not a claim that RX hardware completed or received a frame.
        auto base=load<const uint8_t *>(di,0x60);
        auto packets=load<const uint8_t *>(di,0xb0);
        if (base && packets) for (uint32_t j=0;j<count && j<128;++j) {
            uint32_t index=(before+j)&(n-1);
            auto e=event(RxDescriptor,di,load<void *>(packets,index*8),base);
            e.payload[0]=index;
            for (unsigned k=0;k<4;++k) e.payload[1+k]=load<uint32_t>(base,index*16+k*4);
            e.payload[5]=count; e.payload[6]=load<uint64_t>(di,0xc0)+index*0x70;
            emitRx(e,ordinal);
        }
        if (count>128) {
            auto e=event(RxObservationLimit,di,nullptr,nullptr,1);
            e.payload[0]=count-128; e.payload[1]=before; e.payload[2]=after; emitRx(e,ordinal);
        }
    }
    return result;
}
static void *wrapRxReclaim(void *di, bool force) {
    if(!correctiveMode())return original<void *>(RxReclaim,di,force);
    RxLease lease(di,rx::Operation::Reclaim);
    if (!lease.allowed()) return nullptr;
    const bool cleanupCaller=force && rx::enabled() && ourRing(di) &&
        reinterpret_cast<uint64_t>(__builtin_return_address(0))==imageBase+rxForcedReclaimPC;
    bool match=rxObserving() && ourRing(di); uint64_t ordinal=0;
    if (match) {
        ordinal=__atomic_add_fetch(&rxReclaimCalls,1,__ATOMIC_RELAXED);
        if (force || sparseRxPoll(ordinal)) { auto e=rxRingEvent(RxReclaimEnter,di,nullptr,force); emitRx(e,ordinal); }
    }
    for (unsigned i=0;i<=rx::Capacity;++i) {
        uint32_t index=0,n=0; const uint8_t *packets=nullptr,*descs=nullptr; void *expected=nullptr;
        uint64_t maps=0;
        if (cleanupCaller) {
            n=load<uint16_t>(di,0xa4); index=load<uint16_t>(di,0xa6);
            if (n && n<=4096 && !(n&(n-1)) && index<n &&
                !load<uint8_t>(di,0x42) && !load<uint8_t>(di,0x136)) {
                packets=load<const uint8_t *>(di,0xb0); descs=load<const uint8_t *>(di,0x60);
                maps=load<uint64_t>(di,0xc0);
                if (packets && descs && maps) expected=load<void *>(packets,index*8);
            }
        }
        void *result=original<void *>(RxReclaim,di,force);
        if (match && (result || force || sparseRxPoll(ordinal))) {
            auto e=rxRingEvent(RxReclaimExit,di,result,force); emitRx(e,ordinal);
        }
        auto m=rx::find(reinterpret_cast<uint64_t>(result));
        if (!m) return result;
        if (!force && m->queue==reinterpret_cast<uint64_t>(di) && rx::completeNormally(*m)) return result;
        // Reset acknowledgement alone never frees a packet. Prove this exact
        // mbuf was just detached from its original software slot and descriptor.
        // The known caller next uses osl_pktfree(send=0), after our MD completes.
        const bool detached=cleanupCaller && result==expected && packets && descs &&
            m->queue==reinterpret_cast<uint64_t>(di) && m->owner==load<uint64_t>(di,0x30) &&
            m->startIndex==index && m->mapRecord==maps+index*0x70 &&
            load<uint16_t>(di,0xa4)==n && load<const uint8_t *>(di,0xb0)==packets &&
            load<const uint8_t *>(di,0x60)==descs && load<uint64_t>(di,0xc0)==maps &&
            load<uint16_t>(di,0xa6)==((index+1)&(n-1)) && !load<void *>(packets,index*8) &&
            load<uint32_t>(descs,index*16+8)==0xdeadbeefU && load<uint32_t>(descs,index*16+12)==0xdeadbeefU;
        if (rx::completeResetReclaim(*m,reinterpret_cast<uint64_t>(current_thread()),detached)) return result;
        rx::quarantine(*m,force?1:2);
        // Never return an uncertain hardware-owned buffer for free or reuse.
    }
    rx::halt(14); return nullptr;
}
static bool wrapRxReset(void *di) {
    if(!correctiveMode())return original<bool>(RxReset,di);
    RxLease lease(di,rx::Operation::Reset);
    if (!lease.allowed()) return false;
    const bool match=rx::enabled() && ourRing(di);
    const uint64_t thread=reinterpret_cast<uint64_t>(current_thread());
    const uint64_t queue=reinterpret_cast<uint64_t>(di);
    const uint64_t epoch=match?rx::beginReset(queue,thread):0;
    const uint32_t n=match?load<uint16_t>(di,0xa4):0;
    const uint64_t regs=match?load<uint64_t>(di,0x50):0;
    auto observation=match && n && regs?beginRxResetObservation(di,epoch):nullptr;
    if (match) { auto e=rxRingEvent(RxResetEnter,di); e.payload[14]=epoch; emit(e); }
    const bool result=original<bool>(RxReset,di);
    if (match) {
        const bool seen=observation && observation->seen;
        const uint32_t status=observation?observation->status:0xffffffffU;
        const bool disabled=rx::disabledAcknowledged(result,n,seen,status) &&
            load<uint16_t>(di,0xa4)==n && load<uint64_t>(di,0x50)==regs &&
            observation && observation->owner==load<uint64_t>(di,0x30);
        const auto count=rx::acknowledgeReset(queue,thread,epoch,disabled);
        auto e=rxRingEvent(RxResetExit,di,nullptr,result);
        e.payload[13]=count; e.payload[14]=epoch; e.payload[15]=status; e.payload[16]=seen; emit(e);
    }
    if (observation) {
        __atomic_store_n(&observation->thread,0,__ATOMIC_RELEASE);
        __atomic_store_n(&observation->busy,0,__ATOMIC_RELEASE);
    }
    return result;
}
static void wrapRxInit(void *di) {
    if(!correctiveMode()) {original<void>(RxInit,di);return;}
    RxLease lease(di,rx::Operation::Init);
    if (!lease.allowed()) return;
    rx::quarantineQueue(reinterpret_cast<uint64_t>(di),17);
    rx::invalidateQueue(reinterpret_cast<uint64_t>(di));
    bool match=rx::enabled() && ourRing(di);
    if (match) { auto e=rxRingEvent(RxInitEnter,di); emit(e); }
    original<void>(RxInit,di);
    if (match) { auto e=rxRingEvent(RxInitExit,di); emit(e); }
}
static void wrapRxEnable(void *di) {
    if(!correctiveMode()) {original<void>(RxEnable,di);return;}
    RxLease lease(di,rx::Operation::Enable);
    if (!lease.allowed()) return;
    bool match=rx::enabled() && ourRing(di);
    if (match) { rx::invalidateQueue(reinterpret_cast<uint64_t>(di)); auto e=rxRingEvent(RxEnableEnter,di); emit(e); }
    original<void>(RxEnable,di);
    if (match) { auto e=rxRingEvent(RxEnableExit,di); emit(e); }
}

static uint32_t property(IORegistryEntry *p, const char *name) {
    if (auto d = OSDynamicCast(OSData, p->getProperty(name))) {
        if (d->getLength() == 4) return load<uint32_t>(d->getBytesNoCopy(), 0);
    }
    if (auto n = OSDynamicCast(OSNumber, p->getProperty(name))) return n->unsigned32BitValue();
    return 0xffffffff;
}
static IOPCIDevice *exactPCI(IOService *p) {
    auto pci = OSDynamicCast(IOPCIDevice, p);
    if (!pci || !pciIdentityMatches(property(pci,"vendor-id"),property(pci,"device-id"),
                                   property(pci,"class-code"))) return nullptr;
    return pci;
}

struct RingState { uint64_t key, maps; uint32_t count, reserved; };
static RingState rings[32];
static uint32_t ringLock;
static bool ringIdentityUncertain;
static bool takeRings() {
    uint32_t z = 0;
    return __atomic_compare_exchange_n(&ringLock, &z, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static void dropRings() { __atomic_store_n(&ringLock, 0, __ATOMIC_RELEASE); }

static Event ringEvent(uint32_t type, const void *di, const void *p = nullptr, uint32_t flags = 0) {
    auto e = event(type, di, p, load<void *>(di, 0x30), flags);
    e.payload[0] = load<uint16_t>(di, 0x6a); // ntxd
    e.payload[1] = load<uint16_t>(di, 0x6c); // txin
    e.payload[2] = load<uint16_t>(di, 0x6e); // txout
    e.payload[3] = load<uint64_t>(di, 0x58); // CPU descriptor storage, NOT read MMIO
    e.payload[4] = load<uint64_t>(di, 0x48); // register-window identity only
    e.payload[5] = load<uint64_t>(di, 0x80); // per-entry DMA-map vector
    e.payload[6] = load<uint16_t>(di, 0x12a); // cached completion index
    e.payload[7] = load<uint64_t>(di, 0x28); // bounded 8-byte ring name
    e.payload[8] = load<uint32_t>(di, 0xfc); // descriptor address offsets
    e.payload[9] = load<uint32_t>(di, 0x100);
    return e;
}
static void rememberRing(const void *di) {
    uint32_t n = load<uint16_t>(di, 0x6a);
    if (!n || n > 4096 || (n & (n-1))) return;
    if (!takeRings()) { countTableDrop(); return; }
    for (auto &r : rings) {
        if (!r.key || r.key == reinterpret_cast<uint64_t>(di)) {
            r.key = reinterpret_cast<uint64_t>(di); r.maps = load<uint64_t>(di, 0x80); r.count = n;
            dropRings(); return;
        }
    }
    dropRings(); countTableDrop();
}
static void mapRing(Event &e, const void *map) {
    if (__atomic_load_n(&ringIdentityUncertain,__ATOMIC_ACQUIRE)) return;
    if (!takeRings()) { countTableDrop(); return; }
    uint64_t m = reinterpret_cast<uint64_t>(map);
    for (const auto &r : rings) {
        if (r.key && r.maps && m >= r.maps && m-r.maps < uint64_t(r.count)*0x70 && (m-r.maps)%0x70 == 0) {
            e.object = r.key; e.payload[17] = (m-r.maps)/0x70; break;
        }
    }
    dropRings();
}
static void descriptors(const void *di, uint32_t begin, uint32_t end, const void *packet, uint32_t path) {
    uint32_t n = load<uint16_t>(di, 0x6a);
    const auto *base = load<const uint8_t *>(di, 0x58);
    if (!n || n > 4096 || (n & (n-1)) || begin >= n || end >= n || !base) return;
    uint32_t count = (end-begin)&(n-1);
    uint32_t limit = count > 16 ? 16 : count;
    for (uint32_t j = 0; j < limit; ++j) {
        uint32_t index = (begin+j)&(n-1);
        auto e = event(Descriptor, di, packet, base, path);
        e.payload[0] = index;
        // Only the four descriptor words; never dereference their bus addresses.
        e.payload[1] = load<uint32_t>(base, index*16);
        e.payload[2] = load<uint32_t>(base, index*16+4);
        e.payload[3] = load<uint32_t>(base, index*16+8);
        e.payload[4] = load<uint32_t>(base, index*16+12);
        e.payload[5] = count; emit(e);
    }
    if (count > limit) { auto e = event(Limitation, di, packet); e.payload[0] = count-limit; emit(e); }
}

static void configureCorrection(MapperSource source) {
    // Binding is serialized by selectedController; publish mode/provider only
    // after the coherent correction is ready. Never downgrade live/retained DMA
    // to native mode after a stop, failed rebind or allocation failure.
    if(correctiveMode())return;
    const auto mode=selectRuntimeMode(source);
    if(mode==RuntimeMode::AppleVTDCorrectiveExperimental) {
        privateTx::initialize(true); // Failure keeps private requested: fail closed.
        rx::enable(true);
        enableTxCleanup(true);
    }
    __atomic_store_n(&runtimeModeInfo,uint32_t(mode),__ATOMIC_RELEASE);
    auto e=event(RuntimeModeSelected,nullptr,nullptr,nullptr,uint32_t(mode));emit(e);
}
static bool bindTarget(IOService *self, IOService *provider, bool late) {
    bool match = active() && self && exactPCI(provider) &&
        exactControllerVtable(load<uint64_t>(self,0),controllerVtable);
    if (match) {
        uint64_t z = 0, id = reinterpret_cast<uint64_t>(provider), selfID = reinterpret_cast<uint64_t>(self);
        if (controller()) return selected()==id && controller()==selfID;
        // Claim the controller first and publish the provider LAST. Hot routes
        // cannot admit an incompletely initialized provider/controller tuple.
        match = __atomic_compare_exchange_n(&selectedController,&z,selfID,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED);
        if (match) {
            auto pci=exactPCI(provider);
            auto query=IOService::serviceMatching("AppleVTD");
            auto vtd=query ? IOService::getMatchingServices(query) : nullptr;
            if (query) query->release();
            auto observed=vtd ? OSDynamicCast(IOMapper,vtd->getNextObject()) : nullptr;
            if (observed) observed->retain();
            if (vtd) vtd->release();
            bool vtdActive=observed && !observed->isInactive();
            auto parent=pci->copyProperty("iommu-parent");
            bool declaredParent=parent!=nullptr;
            if (parent) parent->release();
            auto specific=vtdActive ? IOMapper::copyMapperForDevice(pci) : nullptr;
            // On this Intel kernel copyMapperForDevice checks iommu-parent
            // only; absent property is NOT proof that AppleVTD is unavailable.
            // The MD default is gSystem. Admit it explicitly only when it is
            // the very same observed, active AppleVTD IOMapper, and there is
            // no unresolved per-device override. No wait/poll/global write.
            auto system=__atomic_load_n(&IOMapper::gSystem,__ATOMIC_ACQUIRE);
            auto source=selectMapperSource(reinterpret_cast<uint64_t>(specific),declaredParent,
                reinterpret_cast<uint64_t>(system),reinterpret_cast<uint64_t>(observed),vtdActive);
            auto selection=event(MapperSelection,self,nullptr,pci,uint32_t(source));
            selection.payload[0]=reinterpret_cast<uint64_t>(specific);
            selection.payload[1]=reinterpret_cast<uint64_t>(system);
            selection.payload[2]=reinterpret_cast<uint64_t>(observed);
            selection.payload[3]=declaredParent; selection.payload[4]=vtdActive;
            emit(selection);
            if (source==MapperSource::VerifiedSystemAppleVTD) {
                observed->retain(); deviceMapper=observed;
            }
            if (specific) specific->release();
            if (observed) observed->release();
            configureCorrection(source);
            if (source!=MapperSource::VerifiedSystemAppleVTD) {
                __atomic_store_n(&selectedController,0,__ATOMIC_RELEASE);
                if(!correctiveMode())setGate(15); // Native, not an admission fault.
                return false;
            }
            // Boot-lifetime references; neither stop nor fatal recovery unmaps.
            pci->retain(); retainedProvider=pci;
            __atomic_store_n(&lateBinding,late,__ATOMIC_RELEASE);
            __atomic_store_n(&providerID, id, __ATOMIC_RELAXED);
            __atomic_store_n(&controllerID, selfID, __ATOMIC_RELAXED);
            __atomic_store_n(&selectedPCI,id,__ATOMIC_RELEASE);
            auto e = event(Bound, self, nullptr, provider,late?1:0); e.payload[0] = property(pci,"vendor-id"); e.payload[1] = property(pci,"device-id"); emit(e);
            e=event(MapperReady,self,nullptr,deviceMapper,uint32_t(source)); e.payload[0]=MappingCapacity; e.payload[1]=MaxRanges; e.payload[2]=MaxPacketBytes; emit(e);
            uint32_t unbound=__atomic_load_n(&gateStatus,__ATOMIC_ACQUIRE);
            if (unbound==5 || unbound==12 || unbound==13 || unbound==15)
                __atomic_compare_exchange_n(&gateStatus,&unbound,6,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
        }
    }
    return match;
}
static void clearSelection() {
    rx::quarantineOwner(0,4); if (rx::enabled()) rx::halt(4);
    quarantineOwner(0,4); haltMappings(4);
    __atomic_store_n(&selectedPCI,0,__ATOMIC_RELEASE);
    __atomic_store_n(&lateBinding,false,__ATOMIC_RELEASE);
    __atomic_store_n(&providerID,0,__ATOMIC_RELAXED);
    __atomic_store_n(&controllerID,0,__ATOMIC_RELAXED);
    __atomic_store_n(&selectedController,0,__ATOMIC_RELEASE);
    // Never clear an identity-uncertainty gate back to a successful state.
    uint32_t bound=6;
    __atomic_compare_exchange_n(&gateStatus,&bound,5,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
}
static bool wrapStart(IOService *self, IOService *provider) {
    bool match = bindTarget(self,provider,false);
    bool result = original<bool>(Start, self, provider);
    if (match) {
        auto e = event(StartResult, self, nullptr, provider); e.payload[0] = result; emit(e);
        if (!result) clearSelection();
    }
    return result;
}
static void wrapStop(IOService *self, IOService *provider) {
    if(!correctiveMode()) {original<void>(Stop,self,provider);return;}
    TxLease lease(active() && selected(),self,1); if (!lease.allowed()) return;
    bool match = reinterpret_cast<uint64_t>(self) == controller() && selected();
    if (match) { quarantineOwner(0,4); haltMappings(4); mappingDiagnostics(); auto e = event(Stopped, self); emit(e); }
    if (match) { rx::quarantineOwner(0,4); if (rx::enabled()) rx::halt(4); }
    original<void>(Stop, self, provider);
    if (match) clearSelection();
}
static void *wrapAttach(void *pci) {
    if(!correctiveMode())return original<void *>(Attach,pci);
    void *result = original<void *>(Attach, pci);
    if (active() && selected() == reinterpret_cast<uint64_t>(pci) && result &&
        load<uint32_t>(result, 0) == 0x1234abcd && load<void *>(result, 8) == pci) {
        tagSet(result, Osl, selected()); auto e = event(OslBound, result, nullptr, pci); emit(e);
    }
    return result;
}
static void wrapDetach(void *osh) {
    if(!correctiveMode()) {original<void>(Detach,osh);return;}
    TxLease lease(ours(osh),osh,2); if (!lease.allowed()) return;
    rx::quarantineOwner(reinterpret_cast<uint64_t>(osh),5);
    quarantineOwner(reinterpret_cast<uint64_t>(osh),5);
    if (ours(osh)) {
        auto e = event(OslDetached, osh); emit(e); tagErase(osh, Osl);
        if (takeRings()) { memset(rings, 0, sizeof(rings)); dropRings(); }
        else { __atomic_store_n(&ringIdentityUncertain,true,__ATOMIC_RELEASE); countTableDrop(); }
    }
    original<void>(Detach, osh);
}
static void wrapRingDetach(void *di) {
    if(!correctiveMode()) {original<void>(RingDetach,di);return;}
    TxLease lease(ourRing(di),di,3); if (!lease.allowed()) return;
    rx::quarantineQueue(reinterpret_cast<uint64_t>(di),6);
    quarantineQueue(reinterpret_cast<uint64_t>(di),6);
    if (ourRing(di)) {
        if (takeRings()) {
            for (auto &r:rings) if (r.key==reinterpret_cast<uint64_t>(di)) memset(&r,0,sizeof(r));
            dropRings();
        } else { __atomic_store_n(&ringIdentityUncertain,true,__ATOMIC_RELEASE); countTableDrop(); }
    }
    original<void>(RingDetach,di);
}
static uint64_t wrapMap(void *osh, void *data, uint32_t size, uint32_t direction, void *packet, void *map) {
    if(!correctiveMode())return original<uint64_t>(Map,osh,data,size,direction,packet,map);
    if (direction==0 && rx::enabled()) {
        auto r=rx::find(reinterpret_cast<uint64_t>(packet));
        if (r) {
            auto context=rxContext(osh);
            uint64_t caller=reinterpret_cast<uint64_t>(__builtin_return_address(0))-imageBase;
            uint64_t maps=context?load<uint64_t>(context->di,0xc0):0;
            uint64_t record=reinterpret_cast<uint64_t>(map);
            uint32_t n=context?load<uint16_t>(context->di,0xa4):0;
            bool valid=ours(osh) && context && r->owner==reinterpret_cast<uint64_t>(osh) &&
                r->queue==reinterpret_cast<uint64_t>(context->di) && caller==rxMapCallerPC &&
                r->range.address==reinterpret_cast<uint64_t>(data) && r->range.length==size &&
                maps && record>=maps && record-maps<uint64_t(n)*0x70 && (record-maps)%0x70==0;
            if (valid && rx::publish(*r,record,uint32_t((record-maps)/0x70))) {
                // Validated contiguous IOVA, one original-ABI segment. Original
                // RX fill alone writes descriptors and advances the hardware tail.
                memset(map,0,0x70); uint32_t count=1;
                memcpy(static_cast<uint8_t *>(map)+8,&size,4);
                memcpy(static_cast<uint8_t *>(map)+12,&count,4);
                memcpy(static_cast<uint8_t *>(map)+16,&r->address,8);
                memcpy(static_cast<uint8_t *>(map)+24,&size,4);
                return r->address;
            }
            // A violated invariant is NOT mapped-address publication. Retire
            // an unpublished extra MD if safe, latch admission OFF, and leave
            // this unexpected original ABI path unchanged. Never return a zero
            // DMA address (original RX fill does not check map failure).
            rxReject(context?context->di:nullptr,packet,8); rx::halt(8);
            if (!rx::abortUnpublished(*r)) rx::quarantine(*r,8);
        }
    }
    auto m=direction==1 ? findMapping(reinterpret_cast<uint64_t>(packet)) : nullptr;
    if (m && stateOf(*m)==MapState::Submitting && !m->consumed && m->owner==reinterpret_cast<uint64_t>(osh) &&
        m->thread==reinterpret_cast<uint64_t>(current_thread()) &&
        reinterpret_cast<uint64_t>(__builtin_return_address(0))==imageBase+mapCallerPC &&
        m->mapRecord==reinterpret_cast<uint64_t>(map) && m->inputLength==size &&
        (m->privateRecord.leased?m->privateRecord.inputData:m->pages[0].address+m->offsets[0])==reinterpret_cast<uint64_t>(data)) {
        // Original cursor covers the whole chain, but map+8 is mbuf_len(head).
        // Never substitute total chain length for this exact OSL input ABI.
        // No call to the original coalescing cursor after ranges are prepared.
        txpacket::encode(map,*m);
        for (unsigned i=0;i<m->count;++i) {
            auto e=event(MapperPrepared,reinterpret_cast<void *>(m->queue),packet,map,i);
            e.payload[0]=m->serial; e.payload[1]=m->segments[i].address; e.payload[2]=m->segments[i].length;
            e.payload[3]=m->count; e.payload[4]=m->startIndex;
            e.payload[5]=m->inputLength; e.payload[6]=m->bytes;
            e.payload[7]=txChainsEnabled; emit(e);
        }
        __atomic_store_n(&m->consumed,1,__ATOMIC_RELEASE);
        return m->segments[0].address;
    }
    if(privateTx::enabled() && direction==1 && ours(osh) && map &&
       reinterpret_cast<uint64_t>(__builtin_return_address(0))==imageBase+mapCallerPC) {
        // No safe translated map at the exact txfast adapter is a genuine ABI
        // fault, NEVER physical fallback. In the pinned caller count>available
        // takes +0x2c2649's consume/error branch before descriptor construction.
        // Count zero is NOT safe: it reaches final-descriptor publication.
        haltMappings(11);memset(map,0,0x70);
        const uint32_t impossibleCount=0xffffffffU;
        memcpy(static_cast<uint8_t *>(map)+12,&impossibleCount,4);return 0;
    }
    if (m && stateOf(*m)==MapState::Submitting) haltMappings(11);
    bool match = direction==1 && ours(osh);
    uint64_t result = original<uint64_t>(Map, osh, data, size, direction, packet, map);
    if (direction==0 && rxObserving() && ours(osh) && map &&
        reinterpret_cast<uint64_t>(__builtin_return_address(0))==imageBase+rxMapCallerPC) {
        uint64_t ordinal=__atomic_add_fetch(&rxMapCalls,1,__ATOMIC_RELAXED);
        auto e=event(RxMapOriginal,osh,packet,map);
        e.payload[0]=result; e.payload[1]=size; e.payload[2]=load<uint32_t>(map,0xc);
        e.payload[3]=reinterpret_cast<uint64_t>(data); e.payload[16]=rxMapCallerPC;
        for (unsigned i=0;i<e.payload[2] && i<6;++i) {
            e.payload[4+i*2]=load<uint64_t>(map,0x10+i*12);
            e.payload[5+i*2]=load<uint32_t>(map,0x18+i*12);
        }
        emitRx(e,ordinal);
        if (e.payload[2]>6 && e.payload[2]<=8) {
            e.flags=6;
            for (unsigned i=6;i<e.payload[2];++i) {
                e.payload[4+(i-6)*2]=load<uint64_t>(map,0x10+i*12);
                e.payload[5+(i-6)*2]=load<uint32_t>(map,0x18+i*12);
            }
            emitRx(e,ordinal);
        }
    }
    if (match && map) {
        auto e = event(MapResult, nullptr, packet, map, direction);
        e.payload[0] = result; e.payload[1] = size;
        uint32_t count = load<uint32_t>(map, 0xc); e.payload[2] = count;
        e.payload[3] = reinterpret_cast<uint64_t>(data); e.payload[17] = ~0ULL;
        for (uint32_t i = 0; i < count && i < 6; ++i) {
            e.payload[4+i*2] = load<uint64_t>(map, 0x10+i*12);
            e.payload[5+i*2] = load<uint32_t>(map, 0x18+i*12);
        }
        // Six inline segments; additional two get their own metadata event.
        mapRing(e, map); emit(e);
        if (count > 6 && count <= 8) {
            e.flags |= 0x100; e.payload[16] = 6;
            for (unsigned i = 6; i < count; ++i) {
                e.payload[4+(i-6)*2] = load<uint64_t>(map, 0x10+i*12);
                e.payload[5+(i-6)*2] = load<uint32_t>(map, 0x18+i*12);
            }
            emit(e);
        }
    }
    return result;
}
static int32_t wrapTx(void *di, void *packet, uint32_t commit) {
    if(!correctiveMode())return original<int32_t>(Tx,di,packet,commit);
    privateTx::lifecycle(reinterpret_cast<uint64_t>(packet),privateTx::TxAgain);
    TxLease lease(ourRing(di),di,4);
    if (!lease.allowed()) {
        // The call has not published this fresh packet. Preserve txfast's
        // consume-and-error convention; never free an already tracked packet.
        if (!findMapping(reinterpret_cast<uint64_t>(packet)))
            original<void>(Free,load<void *>(di,0x30),packet,uint32_t(1));
        return -1;
    }
    // Never permit a quarantined/in-flight packet to be re-used as a new TX.
    if (auto prior=findMapping(reinterpret_cast<uint64_t>(packet))) { qual::lifecycle(*prior,qual::TxAttempt); markQuarantine(*prior,7); return -1; }
    bool match = ourRing(di);
    if (match) {
        auto osh=load<void *>(di,0x30);
        uint32_t n=load<uint16_t>(di,0x6a),in=load<uint16_t>(di,0x6c),out=load<uint16_t>(di,0x6e);
        auto plan=txpacket::collect(reinterpret_cast<uint64_t>(packet),txChainsEnabled,[](uint64_t value) {
            auto mb=reinterpret_cast<mbuf_t>(value);
            return txpacket::View {reinterpret_cast<uint64_t>(mbuf_data(mb)),mbuf_len(mb),mbuf_maxlen(mb),
                reinterpret_cast<uint64_t>(mbuf_next(mb)),reinterpret_cast<uint64_t>(mbuf_nextpkt(mb))};
        });
        uint64_t rejected=plan.rejected;
        if (mappingsHalted()) rejected|=txpacket::Halted;
        if (!deviceMapper) rejected|=txpacket::NoMapper;
        if (!n || n>4096 || (n&(n-1))) rejected|=txpacket::RingCount;
        if (in>=n || out>=n) rejected|=txpacket::RingIndices;
        if (!load<void *>(di,0x58) || !load<void *>(di,0x80)) rejected|=txpacket::MissingVectors;
        if (load<uint32_t>(di,0xfc)!=0) rejected|=txpacket::AddressOffset;
        auto reject=[&](uint32_t why,Mapping *m=nullptr) {
            auto e=event(MapperRejected,di,packet,nullptr,why);
            e.payload[0]=rejected; e.payload[1]=txChainsEnabled;
            e.payload[2]=plan.head.length; e.payload[3]=plan.head.maximum;
            e.payload[4]=plan.head.next; e.payload[5]=plan.head.nextPacket;
            e.payload[6]=n; e.payload[7]=in; e.payload[8]=out;
            e.payload[9]=load<uint32_t>(di,0xfc); e.payload[10]=plan.count;
            e.payload[11]=plan.total; e.payload[12]=plan.pages;
            if (m) { e.payload[13]=m->serial; e.payload[14]=m->count; }
            emit(e);
            if (!m || abortUnpublished(*m)) original<void>(Free,osh,packet,uint32_t(1));
            else markQuarantine(*m,why);
            return int32_t(-1); // Same consume-and-error ownership as txfast's error branch.
        };
        if (rejected) return reject(2);
        auto m=reserveMapping(reinterpret_cast<uint64_t>(osh),reinterpret_cast<uint64_t>(di),reinterpret_cast<uint64_t>(packet));
        if (!m) return reject(1);
        m->inputLength=uint32_t(plan.head.length);
        if (privateTx::enabled()) {
            if(!preparePrivateMapping(*m,deviceMapper,plan.ranges,plan.count,privateGeometry(di)))return reject(3,m);
        } else if (!prepareMapping(*m,deviceMapper,plan.ranges,plan.count)) return reject(3,m);
        // Original flag-dependent splitting can consume two descriptors per
        // segment. Reserve its worst case without altering Broadcom encoding.
        unsigned available=(in-out-1)&(n-1);
        if (available<2*m->count+1) return reject(8,m);
        for (unsigned i=0;i<m->count;++i)
            if ((m->segments[i].address>>32)+load<uint32_t>(di,0x100)>0xffffffffULL ||
                !m->segments[i].length || m->segments[i].length>0x7fff) return reject(8,m);
        m->startIndex=out; m->mapRecord=load<uint64_t>(di,0x80)+out*0x70;
        m->thread=reinterpret_cast<uint64_t>(current_thread());
        if (!transition(*m,MapState::Prepared,MapState::Submitting)) return reject(10,m);
        auto shape=event(TxPacketShape,di,packet,nullptr,txChainsEnabled);
        shape.payload[0]=m->serial; shape.payload[1]=plan.count;
        shape.payload[2]=m->inputLength; shape.payload[3]=m->bytes; shape.payload[4]=m->count;
        for (unsigned i=0;i<plan.count;++i) shape.payload[5+i]=plan.ranges[i].length;
        emit(shape);
        rememberRing(di); auto e=ringEvent(TxEnter,di,packet,1); e.payload[10]=commit; e.payload[11]=m->serial; emit(e);
        int32_t result=original<int32_t>(Tx,di,packet,commit);
        if (!m->consumed) {
            // No replacement address escaped; extra descriptor was never DMA-owned.
            // The unchanged original path owns any original-address submission.
            if (!abortUnpublished(*m)) markQuarantine(*m,11);
            return result;
        }
        if (result || poisoned(*m)) {
            __atomic_store_n(&m->state,uint32_t(MapState::Quarantine),__ATOMIC_RELEASE); markQuarantine(*m,result?12:13);
        } else {
            m->endIndex=load<uint16_t>(di,0x6e);
            (void)transition(*m,MapState::Submitting,MapState::Owned);
            e=ringEvent(MapperSubmitted,di,packet,1); e.payload[10]=m->serial; emit(e);
            noteTxSubmission();
            descriptors(di,out,uint32_t(m->endIndex),packet,1);
        }
        e=event(TxExit,di,packet,nullptr,1); e.payload[10]=uint32_t(result); e.payload[11]=m->serial; emit(e);
        return result;
    }
    return original<int32_t>(Tx,di,packet,commit);
}
static int32_t wrapUnframed(void *di, void *buffer, uint32_t length, uint32_t commit) {
    if(!correctiveMode())return original<int32_t>(Unframed,di,buffer,length,commit);
    TxLease lease(ourRing(di),di,5); if (!lease.allowed()) return -1;
    bool match = ourRing(di); uint32_t before = 0;
    if (match) { rememberRing(di); auto e = ringEvent(TxEnter, di, buffer, 2); before=uint32_t(e.payload[2]); e.payload[10]=length; e.payload[11]=commit; emit(e); }
    int32_t result = original<int32_t>(Unframed, di, buffer, length, commit);
    if (match) { auto e=ringEvent(TxExit,di,buffer,2); e.payload[10]=uint32_t(result); emit(e); if (!result) descriptors(di,before,uint32_t(e.payload[2]),buffer,2); }
    return result;
}
static int32_t wrapMsgbuf(void *di, uint64_t address, uint32_t a2, uint32_t length, uint32_t a4, uint32_t a5) {
    if(!correctiveMode())return original<int32_t>(Msgbuf,di,address,a2,length,a4,a5);
    TxLease lease(ourRing(di),di,6); if (!lease.allowed()) return -1;
    bool match=ourRing(di); uint32_t before=0;
    if(match) { rememberRing(di); auto e=ringEvent(TxEnter,di,nullptr,3); before=uint32_t(e.payload[2]); e.payload[10]=address; e.payload[11]=length; e.payload[12]=a2; e.payload[13]=a4; e.payload[14]=a5; emit(e); }
    int32_t result=original<int32_t>(Msgbuf,di,address,a2,length,a4,a5);
    if(match) { auto e=ringEvent(TxExit,di,nullptr,3); e.payload[10]=uint32_t(result); emit(e); if(!result) descriptors(di,before,uint32_t(e.payload[2]),nullptr,3); }
    return result;
}
static void *wrapReclaim(void *di, uint32_t range) {
    if(!correctiveMode())return original<void *>(Reclaim,di,range);
    bool match=ourRing(di);
    TxLease lease(match,di,7); if (!lease.allowed()) return nullptr;
    const auto caller=reinterpret_cast<uint64_t>(__builtin_return_address(0))-imageBase;
    const bool cleanupCaller=txCleanupEnabled() && match && range==1 &&
        caller==txForcedReclaimPC && !(load<uint32_t>(di,0x0c)&8);
    if(match) { auto e=ringEvent(ReclaimEnter,di,nullptr,range); emit(e); }
    for (unsigned count=0;count<=MappingCapacity;++count) {
        Mapping *candidate=nullptr; uint32_t n=0,in=0,end=0,span=0;
        const uint8_t *packets=nullptr,*descs=nullptr; uint64_t maps=0,owner=0;
        // Private backing makes software detachment independent of disposition.
        // The native caller is OBSERVED, not whitelisted as a free/requeue policy.
        const bool privateReclaim=privateTx::enabled() && match && range==1;
        if (cleanupCaller || privateReclaim) {
            n=load<uint16_t>(di,0x6a); in=load<uint16_t>(di,0x6c);
            owner=load<uint64_t>(di,0x30); maps=load<uint64_t>(di,0x80);
            packets=load<const uint8_t *>(di,0x70); descs=load<const uint8_t *>(di,0x58);
            if (n && n<=4096 && !(n&(n-1)) && in<n && maps && packets && descs)
                for (unsigned i=0;i<MappingCapacity;++i) {
                    auto m=txMappingAt(i);
                    if (!m->packet || m->queue!=reinterpret_cast<uint64_t>(di) || m->owner!=owner ||
                        m->startIndex!=in || m->endIndex>=n || m->mapRecord!=maps+in*0x70) continue;
                    auto length=(uint32_t(m->endIndex)-in)&(n-1);
                    if (!length || length>2*MaxRanges ||
                        load<uint32_t>(reinterpret_cast<void *>(maps),in*0x70+0xc)!=length) continue;
                    bool exact=true;
                    for (unsigned j=0;j<length;++j) {
                        auto p=load<uint64_t>(packets,((in+j)&(n-1))*8);
                        if (p!=(j+1==length?m->packet:0)) exact=false;
                    }
                    if (exact) { candidate=m;end=uint32_t(m->endIndex);span=length;break; }
                }
        }
        // BVTQ-BEGIN: observation has no authority over the original pipeline.
        auto observation=match && range==1?qual::begin(di,range,caller,cleanupCaller,candidate):nullptr;
        // BVTQ-END
        void *result=original<void *>(Reclaim,di,range);
        if(match) { auto e=ringEvent(ReclaimExit,di,result,range); emit(e); }
        auto m=findMapping(reinterpret_cast<uint64_t>(result));
    // BVQ-BEGIN
        if (match && m) quiet::reclaimed(di,result,m->serial,range,caller);
    // BVQ-END
        // BVTQ-BEGIN
        if (observation) qual::returned(observation,result,m,candidate);
        if (observation && !m) qual::finish(observation,nullptr,false,false);
        // BVTQ-END
        if (!m) return result;
        // Range 1 consumes SW txout without hardware proof. Range 2 is the
        // exact status-derived transmitted path. Reset/invalid-read poisons it.
        if (range==2 && m->queue==reinterpret_cast<uint64_t>(di) && finishNormally(*m)) return result;
        // Preserve the parent's poison timing BEFORE post-reclaim reads when
        // the experimental split path is not selected for this record.
        if(!privateReclaim || !m->privateRecord.leased)markQuarantine(*m,range==1?1:2);
        bool detached=candidate==m && m->packet==reinterpret_cast<uint64_t>(result) &&
            packets && descs && load<uint16_t>(di,0x6a)==n && load<uint16_t>(di,0x6c)==end &&
            load<uint64_t>(di,0x30)==owner && load<uint64_t>(di,0x80)==maps &&
            load<const uint8_t *>(di,0x70)==packets && load<const uint8_t *>(di,0x58)==descs;
        if (detached) for (unsigned j=0;j<span;++j) {
            auto index=(in+j)&(n-1);
            if (load<uint64_t>(packets,index*8) || load<uint32_t>(descs,index*16+8)!=0xdeadbeefU ||
                load<uint32_t>(descs,index*16+12)!=0xdeadbeefU) detached=false;
        }
        if(privateReclaim && m->privateRecord.leased) {
            const bool exact=detached && privateTx::same(m->privateRecord.geometry,privateGeometry(di));
            if(detachPrivateAssociation(*m,exact)) {
                if(observation)qual::finish(observation,m,false,false,true);
                auto e=event(TxPrivate,di,result,nullptr,privateTx::NativeReturn);
                e.payload[0]=m->serial;e.payload[1]=m->privateRecord.generation;emit(e);
                return result; // Native decides packet disposition; OLD DMA stays alive.
            }
            // No experimental permission on ambiguous reclaim. Do not invoke
            // the parent's free-owning notification/reset path for this record.
            markQuarantine(*m,1);m->cleanupBlocked=1;
            if(observation)qual::finish(observation,m,false,false);
            continue;
        }
        noteTxDetached(*m,reinterpret_cast<uint64_t>(current_thread()),caller,detached);
        if (detached && m->detached && !m->notified && !m->cleanupBlocked) {
            // Exact osl_pktfree(send=1) branch, UUID/layout gated. Notify at its
            // original logical point, while BSS/packet callback context is live;
            // retain the mbuf/backing rather than delaying this notification.
            auto osh=reinterpret_cast<void *>(owner);
            auto notify=load<void (*)(void *,void *,uint32_t)>(osh,0x30);
            if (notify) notify(load<void *>(osh,0x38),result,0);
            m->notified=1;
            auto e=event(TxReclaimNotified,di,result,osh);e.payload[0]=m->serial;emit(e);
        }
        // BVTQ-BEGIN
        if (observation) qual::finish(observation,m,true,detached);
        // BVTQ-END
        // Consume the software return, do NOT hand the packet back to recovery
        // for free/recycle/retransmit. Continue draining through original logic.
    }
    haltMappings(14); return nullptr;
}
static void wrapFree(void *osh, void *packet, uint32_t send) {
    if(!correctiveMode()) {original<void>(Free,osh,packet,send);return;}
    if (auto r=rx::find(reinterpret_cast<uint64_t>(packet))) { rx::quarantine(*r,3); return; }
    if (auto m=findMapping(reinterpret_cast<uint64_t>(packet))) {
        // BVTQ-BEGIN
        qual::lifecycle(*m,qual::FreeAttempt);
        // BVTQ-END
        TxLease lease(ours(osh),osh,8); if (!lease.allowed()) return;
        // Also active after provider/OSL stop. No forwarding with live mapping.
        markQuarantine(*m,3); return;
    }
    privateTx::lifecycle(reinterpret_cast<uint64_t>(packet),privateTx::NativeFree);
    bool match=send==1 && ours(osh);
    if(match) { auto e=event(FreeEnter,osh,packet,nullptr,send); emit(e); }
    original<void>(Free,osh,packet,send);
    if(match) { auto e=event(FreeExit,osh,packet,nullptr,send); emit(e); } // identity only, packet may now be freed
}
static void wrapSuspend(void *di) {
    if(!correctiveMode()) {original<void>(Suspend,di);return;}
    TxLease lease(ourRing(di),di,9); if (!lease.allowed()) return;
    bool match=ourRing(di);
    if(match) { rememberRing(di); auto e=ringEvent(SuspendEnter,di); emit(e); }
    // BVQ-BEGIN
    quiet::control(di,9,false);
    // BVQ-END
    original<void>(Suspend,di);
    // BVQ-BEGIN
    quiet::control(di,9,true);
    // BVQ-END
    if(match) { auto e=ringEvent(SuspendExit,di); emit(e); }
}
// Original RMW control writers must not race reset acknowledgement/disposal
// with an old enable bit. Rotate/rewind must not relocate detached slots during
// the certificate transaction. No register value or algorithm is replaced.
static void wrapTxResume(void *di) {
    if(!correctiveMode()) {original<void>(TxResume,di);return;}
    TxLease lease(ourRing(di),di,13);if (!lease.allowed()) return;
    // BVQ-BEGIN
    quiet::control(di,13,false);
    // BVQ-END
    original<void>(TxResume,di);
    // BVQ-BEGIN
    quiet::control(di,13,true);
    // BVQ-END
}
static void wrapTxFlush(void *di) {
    if(!correctiveMode()) {original<void>(TxFlush,di);return;}
    TxLease lease(ourRing(di),di,14);if (!lease.allowed()) return;
    // BVQ-BEGIN
    quiet::control(di,14,false);
    // BVQ-END
    original<void>(TxFlush,di);
    // BVQ-BEGIN
    quiet::control(di,14,true);
    // BVQ-END
}
static void wrapTxFlushClear(void *di) {
    if(!correctiveMode()) {original<void>(TxFlushClear,di);return;}
    TxLease lease(ourRing(di),di,15);if (!lease.allowed()) return;
    // BVQ-BEGIN
    quiet::control(di,15,false);
    // BVQ-END
    original<void>(TxFlushClear,di);
    // BVQ-BEGIN
    quiet::control(di,15,true);
    // BVQ-END
}
static void wrapTxRotate(void *di) {
    if(!correctiveMode()) {original<void>(TxRotate,di);return;}
    TxLease lease(ourRing(di),di,16);if (!lease.allowed()) return;
    // BVQ-BEGIN
    quiet::control(di,16,false);
    // BVQ-END
    original<void>(TxRotate,di);
    // BVQ-BEGIN
    quiet::control(di,16,true);
    // BVQ-END
}
static void wrapTxRewind(void *di) {
    if(!correctiveMode()) {original<void>(TxRewind,di);return;}
    TxLease lease(ourRing(di),di,17);if (!lease.allowed()) return;
    // BVQ-BEGIN
    quiet::control(di,17,false);
    // BVQ-END
    original<void>(TxRewind,di);
    // BVQ-BEGIN
    quiet::control(di,17,true);
    // BVQ-END
}
static void wrapTxPioLoopback(void *osh,void *regs) {
    if(!correctiveMode()) {original<void>(TxPioLoopback,osh,regs);return;}
    TxLease lease(ours(osh),osh,18);if (!lease.allowed()) return;
    original<void>(TxPioLoopback,osh,regs);
}
static bool wrapReset(void *di) {
    if(!correctiveMode())return original<bool>(Reset,di);
    const auto caller=reinterpret_cast<uint64_t>(__builtin_return_address(0))-imageBase;
    bool match=ourRing(di);
    TxLease lease(match,di,10,true); if (!lease.allowed()) return false;
    const auto queue=reinterpret_cast<uint64_t>(di),thread=reinterpret_cast<uint64_t>(current_thread());
    const uint32_t n=match?load<uint16_t>(di,0x6a):0;
    const uint64_t regs=match?load<uint64_t>(di,0x48):0,owner=match?load<uint64_t>(di,0x30):0;
    // Capture eligible Owned identities BEFORE this reset poisons them. A
    // preexisting blocked/uncertain record never acquires active-reset authority.
    privateTx::Terminal terminal;
    if(privateTx::enabled() && match)terminal=privateTx::beginTerminal(privateGeometry(di),thread,caller,
        txCleanupEnabled() && lease.exclusive());
    if(privateTx::enabled() && match && !terminal.ticket)privateTx::invalidate(queue,false);
    quarantineQueue(queue,15);
    uint64_t epoch=0;
    if (lease.exclusive() && n && regs) {
        epoch=beginTxReset(queue);
        txResetObservation.owner=owner;txResetObservation.reg=regs+0x10;
        txResetObservation.epoch=epoch;txResetObservation.seen=0;txResetObservation.status=0xffffffffU;
        __atomic_store_n(&txResetObservation.thread,thread,__ATOMIC_RELEASE);
    }
    if(match) { auto e=ringEvent(ResetEnter,di); emit(e); }
    // BVQ-BEGIN
    quiet::control(di,10,false);
    // BVQ-END
    bool result=original<bool>(Reset,di);
    // BVQ-BEGIN
    quiet::control(di,10,true);
    // BVQ-END
    bool disabled=epoch && tx::disabledAcknowledged(result,n,txResetObservation.seen,txResetObservation.status) &&
        load<uint16_t>(di,0x6a)==n && load<uint64_t>(di,0x48)==regs && load<uint64_t>(di,0x30)==owner && ours(reinterpret_cast<void *>(owner));
    if(match) {
        auto e=ringEvent(ResetExit,di);e.payload[10]=result;e.payload[11]=epoch;
        e.payload[12]=epoch?txResetObservation.status:0xffffffffU;e.payload[13]=disabled;emit(e);
    }
    if (epoch) __atomic_store_n(&txResetObservation.thread,0,__ATOMIC_RELEASE);
    // Legacy free-owning cleanup still requires prior exact detachment. Private
    // terminal eligibility is separately captured before reset quarantine above.
    // BVTQ-BEGIN
    if (match) qual::reset(queue,thread,epoch,disabled);
    // BVTQ-END
    // Drain/retirement stay inside the exact D64 reset interception and its
    // exclusive lease, before ANY native continuation. This includes both
    // per-ring init and bulk core-reset -> native software-reclaim ordering.
    // Caller identity is not hardware authority. The same terminal premise
    // remains EXPERIMENTAL; no flush/disposition hint grants retirement.
    privateTx::finishTerminal(terminal,result && disabled,epoch && txResetObservation.seen,
        epoch?txResetObservation.status:0xffffffffU,privateGeometry,[](unsigned us){IODelay(us);});
    if (disabled) for (unsigned i=0;i<MappingCapacity;++i)
        completeTxReset(*txMappingAt(i),queue,thread,epoch,true,releaseResetPacket);
    return result;
}
// BVTQ-BEGIN: new route observes only arguments of one native lifecycle routine.
static void wrapTxSync(void *wlc,uint32_t bitmap,uint32_t mode) {
    if(!correctiveMode()) {original<void>(TxSync,wlc,bitmap,mode);return;}
    // BVQ-BEGIN
    int quietContext=-1;
    // BVQ-END
    int context=-1;
    if (active() && selected() && wlc) {
        // Native wlc->osh is consumed by this same routine's send=1 free paths.
        // Do not call tagSet/ours: observation cannot alter provider admission.
        auto osh=load<void *>(wlc,8);
        if (osh && load<uint32_t>(osh,0)==0x1234abcd && load<uint64_t>(osh,8)==selected())
            context=qual::enterSync(wlc,bitmap,mode,osh);
    }
    // BVQ-BEGIN
    if (context>=0) quietContext=quiet::enterSync(wlc,bitmap,mode,load<void *>(wlc,8));
    // BVQ-END
    original<void>(TxSync,wlc,bitmap,mode);
    // BVQ-BEGIN
    quiet::leaveSync(quietContext);
    // BVQ-END
    qual::leaveSync(context);
}
// BVTQ-END
static void wrapFlush(void *hw, uint32_t bitmap) {
    if(!correctiveMode()) {original<void>(Flush,hw,bitmap);return;}
    bool match=active() && selected() && hw && ours(load<void *>(hw,0x10));
    if(match) {
        tagSet(load<void *>(hw,0),WlcTag,selected());
        auto e=event(FlushEnter,hw,nullptr,nullptr,bitmap); emit(e);
        // Exact 43602 six-FIFO data ring array; no speculative register reads.
        for(unsigned i=0;i<6;++i) if(bitmap&(1U<<i)) {
            auto di=load<void *>(hw,0x20+i*8);
            if(ourRing(di)) {
                if(privateTx::enabled())privateTx::observeRing(privateGeometry(di),load<uint32_t>(hw,0x84));
                rememberRing(di); e=ringEvent(FifoBinding,di,nullptr,i); e.auxiliary=reinterpret_cast<uint64_t>(hw); emit(e);
            }
        }
    }
    // BVQ-BEGIN
    int quietContext=match?quiet::beginFlush(hw,bitmap):-1;
    // BVQ-END
    original<void>(Flush,hw,bitmap);
    // BVQ-BEGIN
    quiet::endFlush(quietContext);
    // BVQ-END
    if(match) { auto e=event(FlushExit,hw,nullptr,nullptr,bitmap); emit(e); }
}
static void wrapFatal(void *wlc, const char *function, uint32_t line) {
    if(!correctiveMode()) {original<void>(Fatal,wlc,function,line);return;}
    TxLease lease(active() && selected(),wlc,11); if (!lease.allowed()) return;
    // wlc->hw->osh chain, also consumed by the original fatal path.
    // Only a wlc identity observed through this provider's flush path is eligible.
    // Do not dereference arbitrary other controllers' fatal/recovery context.
    bool eligible=active() && selected() && tagGet(wlc,WlcTag)==selected();
    auto hw=eligible ? load<void *>(wlc,0x20) : nullptr;
    bool match=hw && ours(load<void *>(hw,0x10));
    if(match) {
        privateTx::invalidate(0,true);
    // BVQ-BEGIN
        quiet::fatal(load<void *>(hw,0x10));
    // BVQ-END
        quarantineOwner(reinterpret_cast<uint64_t>(load<void *>(hw,0x10)),16);
        rx::quarantineOwner(reinterpret_cast<uint64_t>(load<void *>(hw,0x10)),16);
        mappingDiagnostics();
        auto e=event(FatalEnter,wlc,nullptr,function); e.payload[0]=line; e.payload[1]=load<uint32_t>(hw,0x1c8); emit(e);
        if(e.payload[1]==12) freezeSoon(); // preserve pre-failure ring plus bounded recovery tail
    }
    original<void>(Fatal,wlc,function,line);
    if(match) { auto e=event(FatalExit,wlc); emit(e); }
}
static void wrapTxInit(void *di) {
    if(!correctiveMode()) {original<void>(TxInit,di);return;}
    TxLease lease(ourRing(di),di,12); if (!lease.allowed()) return;
    quarantineQueue(reinterpret_cast<uint64_t>(di),17); // A new ring generation never proves old DMA stopped.
    bool match=ourRing(di);
    if(match)privateTx::invalidate(reinterpret_cast<uint64_t>(di),false);
    if (match) { auto e=ringEvent(TxInitEnter,di); emit(e); }
    // BVQ-BEGIN
    quiet::control(di,12,false);
    // BVQ-END
    original<void>(TxInit,di);
    // BVQ-BEGIN
    quiet::control(di,12,true);
    // BVQ-END
    if (match) { auto e=ringEvent(TxInitExit,di); emit(e); }
}
static uint32_t wrapReadReg(void *osh,void *reg,uint32_t size) {
    bool match=ours(osh);
    uint32_t result=original<uint32_t>(ReadReg,osh,reg,size);
    if(!correctiveMode())return result;
    const uint64_t caller=reinterpret_cast<uint64_t>(__builtin_return_address(0))-imageBase;
    // BVQ-BEGIN
    if (match) quiet::read(osh,reg,size,result,caller);
    // BVQ-END
    if (match && size==4 && (caller==txResetFirstReadPC || caller==txResetPollReadPC) &&
        __atomic_load_n(&txResetObservation.thread,__ATOMIC_ACQUIRE)==reinterpret_cast<uint64_t>(current_thread()) &&
        txResetObservation.owner==reinterpret_cast<uint64_t>(osh) && txResetObservation.reg==reinterpret_cast<uint64_t>(reg)) {
        txResetObservation.seen=1;txResetObservation.status=result;
        auto e=event(TxResetStatus,osh,nullptr,reg);e.payload[0]=result;e.payload[1]=caller;e.payload[2]=txResetObservation.epoch;emit(e);
    }
    if (match && rx::enabled() && size==4 && (caller==rxResetFirstReadPC || caller==rxResetPollReadPC)) {
        const auto thread=reinterpret_cast<uint64_t>(current_thread());
        for (auto &r:rxResetObservations) if (__atomic_load_n(&r.thread,__ATOMIC_ACQUIRE)==thread &&
            r.owner==reinterpret_cast<uint64_t>(osh) && r.reg==reinterpret_cast<uint64_t>(reg)) {
            r.seen=1; r.status=result;
            auto e=event(RxResetStatus,osh,nullptr,reg); e.payload[0]=result; e.payload[1]=caller; e.payload[2]=r.epoch; emit(e);
            break;
        }
    }
    if (match && size==4 && rxObserving() &&
        reinterpret_cast<uint64_t>(__builtin_return_address(0))==imageBase+rxCompletionReadPC) {
        uint64_t ordinal=__atomic_add_fetch(&rxStatusCalls,1,__ATOMIC_RELAXED);
        if (sparseRxPoll(ordinal) || result==0xffffffffU) {
            auto e=event(RxStatusRead,osh,nullptr,reg);
            e.payload[0]=result; e.payload[1]=rxCompletionReadPC; emitRx(e,ordinal);
        }
    }
    if (match && size==4 && result==0xffffffffU) {
        const auto use=readUse(caller,ownershipReadSites);
        // Original read_bpt_reg already ran, including native backplane checks
        // and callback. A raw word is not a global DMA ownership verdict.
        // Retain fail-closed handling for actual completion/reset evidence;
        // leave other native consumers in charge of their returned values.
        if (nativeOwnsReadResult(scopedReadPolicy,use,size,result)) {
            auto e=event(NativeReadResult,osh,nullptr,reg);
            e.payload[0]=result;e.payload[1]=caller;e.payload[2]=uint32_t(use);emit(e);
            return result;
        }
        // Runtime-14: the second package's data word triggered this blanket
        // poison while native TX-status processing was returning real packets.
        // Let the native reader consume its opaque uint32_t unchanged. No extra
        // register read, fabricated success, reset or mapping release occurs.
        if (!scopedReadPolicy && returnNativeTxStatusWord(nativeTxStatusWord,txStatusDataWordPC,caller,size,result)) {
            auto e=event(NativeTxStatusData,osh,nullptr,reg);
            e.payload[0]=result; e.payload[1]=caller; emit(e);
            return result;
        }
        rx::quarantineOwner(reinterpret_cast<uint64_t>(osh),18); if (rx::enabled()) rx::halt(18);
        quarantineOwner(reinterpret_cast<uint64_t>(osh),18); haltMappings(18);
        auto e=event(InvalidRegister,osh,nullptr,reg); e.payload[0]=result;
        e.payload[2]=uint32_t(use);e.payload[3]=scopedReadPolicy;
        e.payload[1]=reinterpret_cast<uint64_t>(__builtin_return_address(0))-imageBase; emit(e);
    }
    return result;
}
static void acquisition(AcquisitionStage stage, uint64_t a=0, uint64_t b=0, uint64_t c=0, uint64_t d=0) {
    auto e=event(Acquisition,nullptr,nullptr,nullptr,stage);
    e.payload[0]=a; e.payload[1]=b; e.payload[2]=c; e.payload[3]=d;
    emit(e);
}
static bool matched(void *, void *, IOService *service, IONotifier *) {
    // The SDK guarantees notification replay of already-matched services.
    // No probe/start request, waitForService, private-field crawl or DMA call.
    if (!active() || !service || service->isInactive()) return true;
    auto provider=service->getProvider();
    if (!bindTarget(service,provider,true))
        acquisition(ProviderRejected,reinterpret_cast<uint64_t>(service),reinterpret_cast<uint64_t>(provider));
    return true;
}
static void patcherReady(void *, KernelPatcher &) {
    if (!registered) return;
    setGate(9);
    acquisition(PatcherReady,target.sys[KernelPatcher::KextInfo::Loaded],target.loadIndex);
}
static void loaded(void *, KernelPatcher &patcher, size_t id, mach_vm_address_t base, size_t size) {
    if (!registered) return;
    // A few dispatch records distinguish no notification from index mismatch.
    // This observes Lilu dispatch only; it installs no routes in other kexts.
    if (__atomic_fetch_add(&dispatchRecords,1,__ATOMIC_RELAXED)<32)
        acquisition(Dispatch,id,target.loadIndex,base,size);
    if(id!=target.loadIndex || active()) return;
    acquisition(TargetEntered,id,target.loadIndex,base,size);
    if(getKernelVersion()!=KernelVersion::Tahoe || !binaryUUIDMatches(base,size,targetUUID)) { setGate(2); return; }
    acquisition(BinaryAccepted,base,size);
    for(const auto &g:gates) {
        if(!instructionBytesMatch(base,size,g.offset,g.prefix,sizeof(g.prefix))) { setGate(3); return; }
        auto sym=patcher.solveSymbol(id,g.symbol,base,size);
        if(!sym || sym!=base+g.offset) {
            patcher.clearError(); setGate(3); return;
        }
    }
    for(const auto &w:windows) if(!instructionBytesMatch(base,size,w.offset,w.bytes,sizeof(w.bytes))) { setGate(3); return; }
    // BVD-BEGIN: extra observation ABI only, not another corrective caller rule.
    for (const auto &g:dispositionHelpers) {
        auto sym=patcher.solveSymbol(id,g.symbol,base,size);
        if (!sym || sym!=base+g.offset || !instructionBytesMatch(base,size,g.offset,g.prefix,sizeof(g.prefix))) {
            patcher.clearError();setGate(3);return;
        }
    }
    auto tagSymbol=patcher.solveSymbol(id,"__ZL17g_osl_mbuf_tag_id",base,size);
    if (!tagSymbol || tagSymbol!=base+dispositionTagIDOffset || dispositionTagIDOffset>size || size-dispositionTagIDOffset<4) {
        patcher.clearError();setGate(3);return;
    }
    // BVD-END
    // Itanium x86_64 vtable address point follows the two-word header. Resolve
    // from the same hash-pinned image; do not accept just a class-name string.
    auto vt=patcher.solveSymbol(id,controllerVtableSymbol,base,size);
    if (!vt || vt!=base+controllerVtableOffset || controllerVtableOffset>size ||
        size-controllerVtableOffset<24) { patcher.clearError(); setGate(3); return; }
    controllerVtable=vt+16;
    imageBase=base;
    // BVD-BEGIN: observation setup only; invoked after all target ABI checks.
    dispo::configure(dispositionTag,dispositionPriorityMap);
    // BVD-END
    acquisition(LayoutAccepted,controllerVtable);
    // All ABI/layout checks precede the first route. On partial route failure,
    // installed wrappers remain pass-through because armed stays false.
    KernelPatcher::RouteRequest routes[] = {
#define ROUTE(name) {gates[name].symbol,wrap##name,originals[name]}
        ROUTE(Start), ROUTE(Stop), ROUTE(Attach), ROUTE(Detach), ROUTE(Map), ROUTE(Tx),
        ROUTE(Unframed), ROUTE(Msgbuf), ROUTE(Reclaim), ROUTE(Free), ROUTE(Suspend), ROUTE(Reset),
        ROUTE(Flush), ROUTE(Fatal), ROUTE(RingDetach), ROUTE(TxInit), ROUTE(ReadReg),
        ROUTE(RxFill), ROUTE(RxReclaim), ROUTE(RxAllocate), ROUTE(RxReset), ROUTE(RxInit), ROUTE(RxEnable),
        ROUTE(TxResume), ROUTE(TxFlush), ROUTE(TxFlushClear), ROUTE(TxRotate), ROUTE(TxRewind), ROUTE(TxPioLoopback), ROUTE(TxSync)
#undef ROUTE
    };
    if(!patcher.routeMultiple(id,routes,HookCount,base,size)) { patcher.clearError(); setGate(4); return; }
    for(auto p:originals) if(!p) { setGate(4); return; }
    setGate(5); __atomic_store_n(&armed,true,__ATOMIC_RELEASE);
    acquisition(RoutesInstalled,HookCount);
    auto matching=IOService::serviceMatching("AirPort_BrcmNIC");
    if (matching) {
        boundNotifier=IOService::addMatchingNotification(gIOFirstMatchNotification,matching,matched,nullptr);
        matching->release();
    }
    if (!boundNotifier) {
        __atomic_store_n(&armed,false,__ATOMIC_RELEASE); setGate(11);
        acquisition(AcquisitionFailed,11); return;
    }
    acquisition(ObserverInstalled);
}
static void initialize() {
    initializeTrace();
    setRxSnapshot(rx::snapshot);
    if(getKernelVersion()!=KernelVersion::Tahoe) return;
    if(checkKernelArgument("-brcmvtdtrace")) { setGate(14); return; }
    // No corrective allocation or positive boot gate before verified binding.
    // The bounded RuntimeModeSelected record and footer identify the coherent
    // policy AFTER verified target binding, not speculative boot-time gates.
    setGate(1);
    configureTargetNotifications(target);
    auto p=lilu.onPatcherLoad(patcherReady);
    auto k=lilu.onKextLoad(&target,1,loaded);
    acquisition(Registered,uint64_t(p),uint64_t(k),target.sys[KernelPatcher::KextInfo::Loaded]);
    registered=p==LiluAPI::Error::NoError && k==LiluAPI::Error::NoError;
    if(!registered) { target.sys[KernelPatcher::KextInfo::Disabled]=true; setGate(10); }
}
} // namespace bvp

static const char *disableArgs[]={"-brcmvtdoff"};
PluginConfiguration ADDPR(config) {
    "BroadcomVTD", parseModuleVersion("0.2.25"), LiluAPI::AllowNormal,
    disableArgs, 1, nullptr, 0, nullptr, 0,
    KernelVersion::Tahoe, KernelVersion::Tahoe, bvp::initialize
};
