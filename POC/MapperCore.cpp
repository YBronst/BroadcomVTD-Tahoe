#include "MapperCore.hpp"
#include "Trace.hpp"
#ifndef BVT_HOST_TEST
#include <mach/task.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#endif

namespace bvp {
static Mapping mappings[MappingCapacity];
static uint64_t serialCounter;
static uint32_t halted;
static bool cleanupEnabled;
static uint64_t resetCounter, resetCompleted;
static uint64_t quarantineEvents,detachedEvents,firstHaltTime,firstHaltSequence;
// Metadata only. No counter participates in admission or ownership decisions.
// Progress is cumulative AFTER observing the first no-credit event, not an
// atomic occupancy census or a per-episode completion/submission pairing.
static uint64_t noCreditEvents,reservationsAfterPressure,completionsAfterPressure,submissionsAfterPressure;
static uint64_t firstNoCreditTime,firstNoCreditSequence;
namespace privateTx {
static bool requested;
static uint8_t *pool;
static uint32_t lock;
static uint64_t generationCounter,ticketCounter,retiredTotal;
struct Ring { Geometry geometry;uint64_t generation=0,ticket=0;uint32_t revision=0; };
static Ring ringGenerations[32];
class Guard {
    bool held;
public:
    Guard() { uint32_t zero=0;held=__atomic_compare_exchange_n(&lock,&zero,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED); }
    ~Guard() { if(held)__atomic_store_n(&lock,0,__ATOMIC_RELEASE); }
    explicit operator bool() const { return held; }
};
bool enabled() { return requested; }
static Event recordValue(Stage stage,const Mapping *m=nullptr,uint64_t value=0) {
    auto e=event(TxPrivate,m?reinterpret_cast<void *>(m->queue):nullptr,
        m?reinterpret_cast<void *>(m->privateRecord.nativePacket):nullptr,nullptr,stage);
    if(m) {
        const auto &p=m->privateRecord;
        e.payload[0]=m->serial;e.payload[1]=p.generation;e.payload[2]=p.backing;
        e.payload[3]=p.pending;e.payload[4]=m->packet;e.payload[5]=m->count;
        e.payload[6]=m->segments[0].address;e.payload[7]=m->owner;
        e.payload[8]=p.geometry.regs;e.payload[9]=m->startIndex;e.payload[10]=m->endIndex;
    }
    e.payload[11]=value;e.payload[12]=__atomic_load_n(&retiredTotal,__ATOMIC_RELAXED);return e;
}
static void record(Stage stage,const Mapping *m=nullptr,uint64_t value=0) {
    auto e=recordValue(stage,m,value);emit(e);
}
bool initialize(bool value) {
    // Immutable boot setting. Allocation failure keeps requested=true: TX must
    // reject safely rather than fall back to the recyclable mbuf data path.
    if (requested || pool) return false;
    requested=value;
    if(value)pool=static_cast<uint8_t *>(IOMallocAligned(MappingCapacity*MaxPacketBytes,4096));
    record(Mode,nullptr,value?(pool?1:2):0);return !value || pool;
}
static Ring *ringFor(const Geometry &g,bool create) {
    Ring *empty=nullptr;
    for(auto &r:ringGenerations) {
        if(!r.generation && !empty)empty=&r;
        if(r.generation && r.geometry.queue==g.queue) {
            if(!same(r.geometry,g)) {
                if(!create)return nullptr;
                r.geometry=g;r.generation=++generationCounter;r.revision=0;r.ticket=0;
            }
            return &r;
        }
    }
    if(create && empty) {empty->geometry=g;empty->generation=++generationCounter;return empty;}
    return nullptr; // No eviction, including generation tombstones.
}
void observeRing(const Geometry &g,uint32_t revision) {
    if(!requested || !valid(g))return;
    Guard guard;if(!guard)return;
    auto r=ringFor(g,true);if(r)r->revision=revision;
}
void invalidate(uint64_t queue,bool blockRecords) {
    if(!requested)return;
    Guard guard;
    if(!guard) {haltMappings(25);return;} // Lost invalidation cannot grant retirement.
    for(auto &r:ringGenerations)if(r.generation && (!queue || r.geometry.queue==queue)) {
        r.generation=++generationCounter;r.ticket=0;
        // Revision describes the observed hardware binding, not the software
        // descriptor generation. Same-ring reset/init cannot change it. Fatal
        // invalidation or a changed Geometry (ringFor) still loses provenance.
        if(blockRecords)r.revision=0;
    }
    if(blockRecords)for(auto &m:mappings)if(m.privateRecord.leased && (!queue || m.queue==queue))m.cleanupBlocked=1;
    record(GenerationChanged,nullptr,queue);
}
Terminal beginTerminal(const Geometry &g,uint64_t thread,uint64_t caller,bool exclusive) {
    Terminal t;
    // The exact routed D64 reset, exclusive lease and observed reset result
    // establish this boundary; its native caller is telemetry, not authority.
    if(!requested || !exclusive || !thread || !valid(g) || mappingsHalted())return t;
    Guard guard;if(!guard)return t;
    auto r=ringFor(g,false);if(!r || (r->revision!=42 && r->revision!=43 && r->revision!=49) || r->ticket)return t;
    t.geometry=g;t.generation=r->generation;t.fence=__atomic_load_n(&serialCounter,__ATOMIC_RELAXED);
    t.thread=thread;t.ticket=++ticketCounter;t.nextGeneration=++generationCounter;
    r->generation=t.nextGeneration;r->ticket=t.ticket;
    for(auto &m:mappings) {
        auto &p=m.privateRecord;
        if(p.leased && !p.pending && p.generation==t.generation && m.serial<=t.fence &&
           same(p.geometry,g) && m.queue==g.queue && m.owner==g.owner &&
           m.packet && m.packet==p.nativePacket && m.consumed && m.prepared && m.descriptor &&
           stateOf(m)==MapState::Owned && !poisoned(m) && !m.cleanupBlocked)
            p.activeResetTicket=t.ticket;
    }
    auto e=event(TxPrivate,reinterpret_cast<void *>(g.queue),nullptr,reinterpret_cast<void *>(g.owner),ResetSelected);
    e.payload[0]=t.fence;e.payload[1]=t.generation;e.payload[2]=t.nextGeneration;
    e.payload[3]=t.ticket;e.payload[4]=thread;e.payload[5]=g.regs;e.payload[6]=g.count;
    e.payload[7]=g.base;e.payload[8]=g.mask;e.payload[9]=caller;e.payload[10]=r->revision;emit(e);return t;
}
static bool current(const Terminal &t) {
    auto r=ringFor(t.geometry,false);
    return t.ticket && r && r->ticket==t.ticket && r->generation==t.nextGeneration &&
        (r->revision==42 || r->revision==43 || r->revision==49) && !mappingsHalted();
}
void lifecycle(uint64_t packet,Stage stage) {
    if(!requested || !packet)return;
    for(const auto &m:mappings)if(m.privateRecord.pending && m.privateRecord.nativePacket==packet)
        record(stage,&m); // Pointer coincidence, not proof of executed requeue.
}
} // privateTx
static bool pressureObserved() { return __atomic_load_n(&noCreditEvents,__ATOMIC_RELAXED)!=0; }
// Keep the once-only Event workspace out of normal completion stack frames.
static __attribute__((noinline)) void emitFirstPressureProgress(uint32_t type) {
    auto e=event(type); e.payload[0]=1; emit(e);
}
static void pressureProgress(uint64_t &counter,uint32_t type) {
    if (!pressureObserved()) return;
    const auto count=__atomic_add_fetch(&counter,1,__ATOMIC_RELAXED);
    if (count==1) emitFirstPressureProgress(type);
}
void noteTxSubmission() { pressureProgress(submissionsAfterPressure,MapperAdmissionResumed); }
void enableTxCleanup(bool value) { cleanupEnabled=value; }
bool txCleanupEnabled() { return cleanupEnabled; }
Mapping *txMappingAt(unsigned index) { return index<MappingCapacity?&mappings[index]:nullptr; }

static void detail(uint32_t type,const Mapping &m,uint32_t reason=0) {
    auto e=event(type,reinterpret_cast<void *>(m.queue),reinterpret_cast<void *>(m.packet),m.descriptor,reason);
    e.payload[0]=m.serial; e.payload[1]=uint32_t(stateOf(m)); e.payload[2]=m.owner;
    e.payload[3]=m.count; e.payload[4]=m.bytes; e.payload[5]=m.startIndex; e.payload[6]=m.endIndex;
    e.payload[7]=m.mapRecord; e.payload[8]=poisoned(m);
    e.payload[11]=m.firstReason; e.payload[12]=m.detached; e.payload[13]=m.detachedCaller;
    e.payload[14]=m.detachedThread; e.payload[15]=m.cleanupBlocked; e.payload[16]=m.resetEpoch; emit(e);
}
bool mappingsHalted() { return __atomic_load_n(&halted,__ATOMIC_ACQUIRE)!=0; }
void haltMappings(uint32_t reason) {
    uint32_t z=0;
    if (__atomic_compare_exchange_n(&halted,&z,reason?reason:1,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) {
        auto e=event(MapperHalted,nullptr,nullptr,nullptr,reason); emit(e);
        __atomic_store_n(&firstHaltTime,e.timeNS,__ATOMIC_RELEASE);
        __atomic_store_n(&firstHaltSequence,e.sequence,__ATOMIC_RELEASE);
    }
}
Mapping *findMapping(uint64_t packet) {
    if (!packet) return nullptr;
    for (auto &m:mappings) if (__atomic_load_n(&m.packet,__ATOMIC_ACQUIRE)==packet) return &m;
    return nullptr;
}
Mapping *reserveMapping(uint64_t owner,uint64_t queue,uint64_t packet) {
    if (!owner || !queue || !packet || mappingsHalted() || findMapping(packet)) return nullptr;
    for (auto &m:mappings) if (transition(m,MapState::Empty,MapState::Preparing)) {
        m.owner=owner; m.queue=queue; m.serial=__atomic_add_fetch(&serialCounter,1,__ATOMIC_RELAXED);
        m.count=m.bytes=m.consumed=m.prepared=m.reported=0; m.poison=0;
        m.inputLength=0;
        m.mapRecord=m.startIndex=m.endIndex=m.thread=0; m.descriptor=nullptr;
        m.detachedThread=m.detachedCaller=m.resetEpoch=0;
        m.detached=m.cleanupBlocked=m.firstReason=m.notified=0;
        m.privateRecord={};
        __atomic_store_n(&m.packet,packet,__ATOMIC_RELEASE);
        if (pressureObserved()) __atomic_add_fetch(&reservationsAfterPressure,1,__ATOMIC_RELAXED);
        return &m;
    }
    // No Empty credit is a failed reservation, NOT a mapper/ownership fault.
    // The caller still consumes this unpublished packet with send=1 and -1;
    // it MUST NOT call original TX without a mapping. Qualified completions
    // can independently return credits; fresh packets may then reserve them.
    // No eviction, unmap, halt clearing, retry, or physical-address fallback.
    const auto count=__atomic_add_fetch(&noCreditEvents,1,__ATOMIC_RELAXED);
    // At most 64 ring records per uint64 counter lifetime, no hot-path logs.
    if (count && !(count&(count-1))) {
        auto e=event(MapperNoCredit,reinterpret_cast<void *>(queue),reinterpret_cast<void *>(packet),reinterpret_cast<void *>(owner));
        e.payload[0]=count; e.payload[1]=MappingCapacity; emit(e);
        if (count==1) {
            __atomic_store_n(&firstNoCreditTime,e.timeNS,__ATOMIC_RELAXED);
            __atomic_store_n(&firstNoCreditSequence,e.sequence,__ATOMIC_RELAXED);
        }
    }
    return nullptr;
}
void markQuarantine(Mapping &m,uint32_t reason) {
    // Ordinary forced reclaim/reset/init/fatal may subsequently reach a proven
    // disabled engine. Stop/detach, unexpected free/reuse, failed MD completion
    // and invalid registers permanently prohibit disposal for this entry.
    if (reason!=1 && reason!=2 && reason!=15 && reason!=16 && reason!=17)
        __atomic_store_n(&m.cleanupBlocked,1,__ATOMIC_RELEASE);
    quarantine(m);
    if (!__atomic_exchange_n(&m.reported,1,__ATOMIC_ACQ_REL)) {
        m.firstReason=reason; __atomic_add_fetch(&quarantineEvents,1,__ATOMIC_RELAXED); detail(MapperQuarantine,m,reason);
    }
}
void quarantineQueue(uint64_t queue,uint32_t reason) {
    for (auto &m:mappings) if ((__atomic_load_n(&m.packet,__ATOMIC_ACQUIRE) || m.privateRecord.pending) && m.queue==queue) markQuarantine(m,reason);
}
void quarantineOwner(uint64_t owner,uint32_t reason) {
    for (auto &m:mappings) if ((__atomic_load_n(&m.packet,__ATOMIC_ACQUIRE) || m.privateRecord.pending) && (!owner || m.owner==owner)) markQuarantine(m,reason);
}
static bool releasePrepared(Mapping &m) {
    if (m.prepared && m.descriptor->complete(kIODirectionOut)!=kIOReturnSuccess) return false;
    m.prepared=0;
    if (m.descriptor) { m.descriptor->release(); m.descriptor=nullptr; }
    // The wired pool stays boot-resident; only this transaction's slot lease
    // ends. Nothing touches native packet storage. No slot write until Empty.
    m.privateRecord.leased=0;m.privateRecord.pending=0;
    __atomic_store_n(&m.packet,0,__ATOMIC_RELEASE);
    __atomic_store_n(&m.state,uint32_t(MapState::Empty),__ATOMIC_RELEASE); return true;
}
void noteTxDetached(Mapping &m,uint64_t thread,uint64_t caller,bool proven) {
    if (!cleanupEnabled || !proven || !thread || !caller || !m.consumed || !m.prepared ||
        stateOf(m)!=MapState::Quarantine || m.cleanupBlocked) return;
    // A single exact forced-return transaction can grant ownership to this
    // frontend. Repeated ambiguous returns must not replace that certificate.
    if (m.detached) { m.cleanupBlocked=1; return; }
    m.detachedThread=thread; m.detachedCaller=caller;
    __atomic_store_n(&m.detached,1,__ATOMIC_RELEASE);
    __atomic_add_fetch(&detachedEvents,1,__ATOMIC_RELAXED);
    detail(TxSoftwareDetached,m);
}
uint64_t beginTxReset(uint64_t queue) {
    auto epoch=__atomic_add_fetch(&resetCounter,1,__ATOMIC_RELAXED);
    for (auto &m:mappings) if (m.packet && m.queue==queue) {
        markQuarantine(m,15); m.resetEpoch=epoch;
    }
    return epoch;
}
bool completeTxReset(Mapping &m,uint64_t queue,uint64_t thread,uint64_t epoch,
                     bool disabled,void (*releasePacket)(uint64_t,uint64_t)) {
    // A latched admission failure is not permission to resume. For this PoC
    // cleanup itself is also disabled after any halt; reboot remains required.
    if (m.privateRecord.leased || !cleanupEnabled || mappingsHalted() || !disabled || !epoch || !releasePacket ||
        !m.packet || m.queue!=queue || !m.detached || m.detachedThread!=thread ||
        m.resetEpoch!=epoch || m.cleanupBlocked || !m.notified || !m.consumed || !m.prepared ||
        !transition(m,MapState::Quarantine,MapState::Completing)) return false;
    auto e=event(TxResetCompleted,reinterpret_cast<void *>(queue),reinterpret_cast<void *>(m.packet),m.descriptor);
    e.payload[0]=m.serial; e.payload[1]=epoch; e.payload[2]=m.startIndex; e.payload[3]=m.endIndex;
    e.payload[4]=m.detachedCaller; e.payload[5]=thread;e.payload[6]=m.notified;
    // Keep packet identity reserved through the original release callback;
    // neither reserveMapping nor duplicate wrapFree may recycle/free it twice.
    m.detached=0;
    if (m.descriptor->complete(kIODirectionOut)!=kIOReturnSuccess) {
        __atomic_store_n(&m.state,uint32_t(MapState::Quarantine),__ATOMIC_RELEASE);
        markQuarantine(m,9); haltMappings(9); return false;
    }
    m.prepared=0; m.descriptor->release(); m.descriptor=nullptr;
    releasePacket(m.owner,m.packet);
    __atomic_store_n(&m.packet,0,__ATOMIC_RELEASE);
    __atomic_store_n(&m.state,uint32_t(MapState::Empty),__ATOMIC_RELEASE);
    __atomic_add_fetch(&resetCompleted,1,__ATOMIC_RELAXED); emit(e); return true;
}
bool abortUnpublished(Mapping &m) {
    // The frontend calls this only before original txfast, or after proving the
    // adapter never handed out this map. It is NOT failed-submit cleanup.
    if (m.consumed || stateOf(m)==MapState::Quarantine || poisoned(m)) return false;
    const bool wasPrivate=m.privateRecord.leased;
    Event privateEvent {};if(wasPrivate)privateEvent=privateTx::recordValue(privateTx::UnpublishedRetired,&m);
    bool ok=releasePrepared(m);
    if (!ok) { markQuarantine(m,9); haltMappings(9); }
    else if(wasPrivate) {privateEvent.payload[3]=privateEvent.payload[4]=0;emit(privateEvent);}
    return ok;
}
bool finishNormally(Mapping &m) {
    if (!canCompleteNormally(m,true) || !transition(m,MapState::Owned,MapState::Completing)) return false;
    // A concurrent/reentrant reset latches poison before any free is permitted.
    if (poisoned(m)) { __atomic_store_n(&m.state,uint32_t(MapState::Quarantine),__ATOMIC_RELEASE); return false; }
    auto e=event(MapperCompleted,reinterpret_cast<void *>(m.queue),reinterpret_cast<void *>(m.packet),m.descriptor);
    e.payload[0]=m.serial; e.payload[1]=m.startIndex; e.payload[2]=m.endIndex;
    const bool wasPrivate=m.privateRecord.leased;
    Event privateEvent {};if(wasPrivate)privateEvent=privateTx::recordValue(privateTx::NormalRetired,&m);
    bool ok=releasePrepared(m);
    if (!ok) { __atomic_store_n(&m.state,uint32_t(MapState::Quarantine),__ATOMIC_RELEASE); markQuarantine(m,9); haltMappings(9); }
    else { if(wasPrivate) {privateEvent.payload[3]=privateEvent.payload[4]=0;emit(privateEvent);}
        auto stamp=event(MapperCompleted); e.timeNS=stamp.timeNS; emit(e);
        pressureProgress(completionsAfterPressure,MapperCreditReturned); }
    return ok;
}
bool prepareMapping(Mapping &m,IOMapper *mapper,const VirtualRange *ranges,unsigned count) {
    if (!mapper || !ranges || !count || count>MaxRanges || stateOf(m)!=MapState::Preparing) return false;
    unsigned n=0; uint64_t total=0;
    for (unsigned r=0;r<count;++r) {
        uint64_t a=ranges[r].address,left=ranges[r].length;
        if (!a || !left || left>MaxPacketBytes || a+left<a || total+left>MaxPacketBytes) return false;
        total+=left;
        while (left) {
            if (n==MaxRanges) return false;
            uint32_t offset=uint32_t(a&4095),length=uint32_t(left<4096-offset?left:4096-offset);
            m.pages[n]={a&~uint64_t(4095),4096}; m.offsets[n]=offset; m.lengths[n]=length;
            a+=length; left-=length; ++n;
        }
    }
    if (!rangesValid(total,n)) return false;
    m.count=n; m.bytes=uint32_t(total);
    // Explicit per-device mapper from the frontend; never kIOMapperSystem/None.
    // Page-relative extraction matches the existing Mieze prepared-MD model.
    m.descriptor=IOMemoryDescriptor::withOptions(m.pages,n,0,kernel_task,
        kIOMemoryTypeVirtual|kIODirectionOut|kIOMemoryAsReference,mapper);
    if (!m.descriptor) return false;
    if (m.descriptor->prepare(kIODirectionOut|kIODirectionPrepareNoFault)!=kIOReturnSuccess) return false;
    m.prepared=1;
    for (unsigned i=0;i<n;++i) {
        IOByteCount length=0;
        auto address=m.descriptor->getPhysicalSegment(uint64_t(i)*4096,&length,0);
        if (!address || length<4096 || address+m.offsets[i]<address) return false;
        m.segments[i]={address+m.offsets[i],m.lengths[i]};
    }
    if (poisoned(m)) { markQuarantine(m,10); return false; }
    return transition(m,MapState::Preparing,MapState::Prepared);
}
bool preparePrivateMapping(Mapping &m,IOMapper *mapper,const VirtualRange *ranges,unsigned count,
                           const privateTx::Geometry &g) {
    if(!privateTx::enabled() || !privateTx::pool || !privateTx::valid(g) || !ranges ||
       !count || count>MaxRanges || stateOf(m)!=MapState::Preparing || m.queue!=g.queue || m.owner!=g.owner)return false;
    const auto address=reinterpret_cast<uint64_t>(&m),first=reinterpret_cast<uint64_t>(mappings);
    if(address<first || address-first>=sizeof(mappings) || (address-first)%sizeof(Mapping))return false;
    {
        privateTx::Guard guard;if(!guard)return false;
        auto r=privateTx::ringFor(g,true);if(!r || r->ticket)return false;
        m.privateRecord.generation=r->generation;
    }
    VirtualRange copied[MaxRanges] {};unsigned n=0;uint64_t bytes=0;
    // Validate the entire source plan BEFORE copying any byte.
    for(unsigned i=0;i<count;++i) {
        const auto a=ranges[i].address,len=ranges[i].length;
        if(!a || !len || len>MaxPacketBytes || a+len<a || bytes+len>MaxPacketBytes)return false;
        bytes+=len;n+=unsigned(((a&4095)+len+4095)/4096);
        if(n>MaxRanges)return false;
    }
    auto backing=privateTx::pool+((address-first)/sizeof(Mapping))*MaxPacketBytes;
    m.privateRecord.geometry=g;m.privateRecord.nativePacket=m.packet;
    m.privateRecord.inputData=ranges[0].address;m.privateRecord.backing=reinterpret_cast<uint64_t>(backing);
    m.privateRecord.leased=1;
    memset(backing,0,n*4096);n=0;
    for(unsigned i=0;i<count;++i) {
        auto a=ranges[i].address,left=ranges[i].length;
        while(left) {
            const auto offset=a&4095,length=left<4096-offset?left:4096-offset;
            auto dest=backing+n*4096+offset;
            memcpy(dest,reinterpret_cast<const void *>(a),length);
            copied[n++]={reinterpret_cast<uint64_t>(dest),length};a+=length;left-=length;
        }
    }
    privateTx::record(privateTx::BackingLeased,&m,n*4096);
    return prepareMapping(m,mapper,copied,n);
}
bool detachPrivateAssociation(Mapping &m,bool exact) {
    auto &p=m.privateRecord;
    if(!privateTx::enabled() || !exact || mappingsHalted() || !p.leased || p.pending || !p.generation ||
       !m.packet || m.packet!=p.nativePacket || !m.consumed || !m.prepared || !m.descriptor ||
       m.cleanupBlocked || stateOf(m)!=MapState::Owned || poisoned(m))return false;
    privateTx::Guard guard;if(!guard)return false;
    auto r=privateTx::ringFor(p.geometry,false);
    if(!r || r->generation!=p.generation || r->ticket)return false;
    markQuarantine(m,1);p.pending=1;
    // NO MD completion, notification, free or payload write. The opaque old
    // nativePacket is retained for diagnostics only, never admission/ownership.
    __atomic_store_n(&m.packet,0,__ATOMIC_RELEASE);
    privateTx::record(privateTx::AssociationDetached,&m);return true;
}
unsigned privateTx::finishTerminal(Terminal &t,bool returned,bool seen,uint32_t status,
    Geometry (*readGeometry)(const void *),void (*drain)(unsigned)) {
    if(!t.ticket || !readGeometry || !drain)return 0;
    auto deny=[&]() {
        record(ResetDenied,nullptr,t.ticket);
        Guard guard;if(guard) {auto r=ringFor(t.geometry,false);if(r && r->ticket==t.ticket)r->ticket=0;}
        t.ticket=0;return 0U;
    };
    bool ok=false;
    {Guard guard;ok=guard && current(t);}
    uint32_t rev=0;
    {
        Guard guard;
        auto r=ringFor(t.geometry,false);
        if(r) rev=r->revision;
    }
    if(!ok || t.thread!=reinterpret_cast<uint64_t>(current_thread()) ||
       !experimentalPredicate(requested,true,rev,t.geometry.count,returned,seen,status,mappingsHalted()) ||
       !same(t.geometry,readGeometry(reinterpret_cast<void *>(t.geometry.queue))))return deny();
    record(DrainBegin,nullptr,t.ticket);
    drain(ExperimentalDrainUS); // ONLY risk-accepted delay; never a timer/reset request.
    record(DrainEnd,nullptr,t.ticket);
    // Caller still holds exclusive TxLease. This is not a simple/spin lock
    // across IODelay/MD completion, and is not a global writer-census claim.
    {Guard guard;ok=guard && current(t);}
    if(!ok || !same(t.geometry,readGeometry(reinterpret_cast<void *>(t.geometry.queue))))return deny();
    unsigned completed=0;
    for(auto &m:mappings) {
        auto &p=m.privateRecord;
        const bool activeOld=p.activeResetTicket==t.ticket && !p.pending &&
            m.packet && m.packet==p.nativePacket;
        if(!(p.pending && !m.packet) && !activeOld)continue;
        if(!p.leased || p.generation!=t.generation || m.serial>t.fence ||
           !same(p.geometry,t.geometry) || m.owner!=t.geometry.owner || m.queue!=t.geometry.queue ||
           m.cleanupBlocked || !m.consumed ||
           !m.prepared || !m.descriptor || mappingsHalted())continue;
        if(!transition(m,MapState::Quarantine,MapState::Completing))continue;
        Event association {};
        if(activeOld)association=recordValue(ResetAssociationEnded,&m,t.ticket);
        if(!releasePrepared(m)) {
            __atomic_store_n(&m.state,uint32_t(MapState::Quarantine),__ATOMIC_RELEASE);
            markQuarantine(m,9);haltMappings(9);record(RetirementDenied,&m,t.ticket);break;
        }
        // MD retirement ends only OUR association/storage lease. Native packet
        // slots and packet disposition remain untouched; original init/free/
        // reclaim/requeue continue after this exclusive reset returns.
        if(activeOld) {association.payload[3]=association.payload[4]=0;emit(association);}
        ++completed;__atomic_add_fetch(&retiredTotal,1,__ATOMIC_RELAXED);
        record(ExperimentalRetired,&m,t.ticket);
    }
    {Guard guard;if(guard){auto r=ringFor(t.geometry,false);if(r && r->ticket==t.ticket)r->ticket=0;}}
    t.ticket=0;return completed;
}
void mappingDiagnostics() {
    uint32_t used=0,quarantined=0;
    for (const auto &m:mappings) {
        if (!__atomic_load_n(&m.packet,__ATOMIC_ACQUIRE) && !m.privateRecord.pending) continue;
        ++used; if (poisoned(m) || stateOf(m)==MapState::Quarantine) ++quarantined;
        detail(MapperInventory,m);
    }
    auto e=event(MapperInventory); e.payload[0]=used; e.payload[1]=quarantined;
    e.payload[2]=MappingCapacity; e.payload[3]=mappingsHalted(); e.flags=0xffffffff; emit(e);
}
void snapshotMapping(unsigned index,Event &e) {
    e=event(MapperInventory); e.flags=index;
    if (index==MappingCapacity) {
        e.flags=0xffffffffU;
        e.payload[0]=cleanupEnabled;e.payload[1]=__atomic_load_n(&halted,__ATOMIC_ACQUIRE);
        if(privateTx::enabled())e.payload[17]=1ULL<<63;
        e.payload[17]|=uint64_t(__atomic_load_n(&runtimeModeInfo,__ATOMIC_ACQUIRE))<<60;
        for (const auto &m:mappings) if (__atomic_load_n(&m.packet,__ATOMIC_ACQUIRE) || m.privateRecord.pending) {
            ++e.payload[2]; if (stateOf(m)==MapState::Quarantine || poisoned(m)) ++e.payload[3];
            if(m.privateRecord.pending)++e.payload[17];
        }
        e.payload[4]=MappingCapacity;e.payload[5]=__atomic_load_n(&resetCompleted,__ATOMIC_RELAXED);
        e.payload[6]=__atomic_load_n(&resetCounter,__ATOMIC_RELAXED);
        e.payload[7]=__atomic_load_n(&quarantineEvents,__ATOMIC_RELAXED);
        e.payload[8]=__atomic_load_n(&firstHaltTime,__ATOMIC_ACQUIRE);
        e.payload[9]=__atomic_load_n(&firstHaltSequence,__ATOMIC_ACQUIRE);
        e.payload[10]=__atomic_load_n(&detachedEvents,__ATOMIC_RELAXED);
        e.payload[11]=__atomic_load_n(&noCreditEvents,__ATOMIC_RELAXED);
        e.payload[12]=__atomic_load_n(&reservationsAfterPressure,__ATOMIC_RELAXED);
        e.payload[13]=__atomic_load_n(&completionsAfterPressure,__ATOMIC_RELAXED);
        e.payload[14]=__atomic_load_n(&submissionsAfterPressure,__ATOMIC_RELAXED);
        e.payload[15]=__atomic_load_n(&firstNoCreditTime,__ATOMIC_RELAXED);
        e.payload[16]=__atomic_load_n(&firstNoCreditSequence,__ATOMIC_RELAXED);return;
    }
    if (index>=MappingCapacity) return;
    const auto &m=mappings[index];
    auto packet=__atomic_load_n(&m.packet,__ATOMIC_ACQUIRE);
    auto serial=__atomic_load_n(&m.serial,__ATOMIC_RELAXED);
    if (!packet && !m.privateRecord.pending) return;
    e.object=__atomic_load_n(&m.queue,__ATOMIC_RELAXED); e.packet=packet;
    e.auxiliary=reinterpret_cast<uint64_t>(__atomic_load_n(&m.descriptor,__ATOMIC_RELAXED));
    e.payload[0]=serial; e.payload[1]=uint32_t(stateOf(m)); e.payload[2]=__atomic_load_n(&m.owner,__ATOMIC_RELAXED);
    e.payload[3]=__atomic_load_n(&m.count,__ATOMIC_RELAXED); e.payload[4]=__atomic_load_n(&m.bytes,__ATOMIC_RELAXED);
    e.payload[5]=__atomic_load_n(&m.startIndex,__ATOMIC_RELAXED); e.payload[6]=__atomic_load_n(&m.endIndex,__ATOMIC_RELAXED);
    e.payload[7]=__atomic_load_n(&m.mapRecord,__ATOMIC_RELAXED); e.payload[8]=poisoned(m);
    e.payload[9]=__atomic_load_n(&m.consumed,__ATOMIC_ACQUIRE); e.payload[10]=mappingsHalted();
    e.payload[11]=__atomic_load_n(&m.firstReason,__ATOMIC_RELAXED);
    e.payload[12]=__atomic_load_n(&m.detached,__ATOMIC_ACQUIRE);
    e.payload[13]=__atomic_load_n(&m.detachedCaller,__ATOMIC_RELAXED);
    e.payload[14]=__atomic_load_n(&m.detachedThread,__ATOMIC_RELAXED);
    e.payload[15]=__atomic_load_n(&m.cleanupBlocked,__ATOMIC_ACQUIRE);
    e.payload[16]=__atomic_load_n(&m.resetEpoch,__ATOMIC_RELAXED);
    e.payload[17]=__atomic_load_n(&resetCompleted,__ATOMIC_RELAXED);
    if(m.privateRecord.leased) {
        e.flags|=0x40000000U;e.packet=m.privateRecord.nativePacket;
        e.payload[12]=m.privateRecord.pending;e.payload[13]=m.privateRecord.generation;
        e.payload[14]=m.privateRecord.backing;e.payload[16]=packet;
        e.payload[17]=__atomic_load_n(&privateTx::retiredTotal,__ATOMIC_RELAXED);
    }
    if (packet!=__atomic_load_n(&m.packet,__ATOMIC_ACQUIRE) || serial!=__atomic_load_n(&m.serial,__ATOMIC_RELAXED))
        e.flags|=0x80000000U; // Concurrent slot reuse: explicitly unusable snapshot entry.
}
} // namespace bvp
