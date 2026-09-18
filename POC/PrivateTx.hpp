#pragma once
#include <stdint.h>

namespace bvp { namespace privateTx {
// Software identity, immutable DMA record and native ring generation are
// separate. These CPU metadata values are NOT hardware quiescence evidence.
struct Geometry {
    uint64_t queue=0,owner=0,regs=0,maps=0,packets=0,descs=0;
    uint32_t count=0,base=0,mask=0;
};
inline bool same(const Geometry &a,const Geometry &b) {
    return a.queue==b.queue && a.owner==b.owner && a.regs==b.regs &&
        a.maps==b.maps && a.packets==b.packets && a.descs==b.descs &&
        a.count==b.count && a.base==b.base && a.mask==b.mask;
}
inline bool valid(const Geometry &g) {
    return g.queue && g.owner && g.regs && g.maps && g.packets && g.descs &&
        g.count && g.count<=4096 && !(g.count&(g.count-1));
}
struct Record {
    Geometry geometry {};
    uint64_t generation=0,nativePacket=0,inputData=0,backing=0;
    uint32_t pending=0,leased=0;
    // Only an unpoisoned Owned record observed BEFORE this exclusive reset
    // may end its active association at this ticket's experimental terminal.
    uint64_t activeResetTicket=0;
};
struct Terminal {
    Geometry geometry {};
    uint64_t generation=0,nextGeneration=0,fence=0,thread=0,ticket=0;
};
enum Stage : uint32_t { Mode=1, BackingLeased, AssociationDetached,
    NativeReturn, NativeFree, TxAgain, NormalRetired, UnpublishedRetired,
    ResetSelected, ResetDenied, DrainBegin, DrainEnd, ExperimentalRetired,
    RetirementDenied, GenerationChanged, Inventory, ResetAssociationEnded };
// Sole risk-accepted policy. NOT a rev49 specification or release proof.
// Neither elapsed time alone, a zero-ring shortcut nor reset name grants it.
constexpr unsigned ExperimentalDrainUS=300;
inline bool experimentalPredicate(bool gate,bool exclusive,
    uint32_t revision,uint32_t count,bool returned,bool seen,uint32_t status,bool halted) {
    return gate && exclusive && (revision==42 || revision==43 || revision==49) && count &&
        returned && seen && status!=0xffffffffU && !(status&0xf0000000U) && !halted;
}
bool enabled();
bool initialize(bool requested); // One boot-lifetime wired pool, no TX allocation.
void observeRing(const Geometry &,uint32_t revision);
void invalidate(uint64_t queue,bool blockRecords);
Terminal beginTerminal(const Geometry &,uint64_t thread,uint64_t caller,bool exclusive);
// Stable reader reads CPU ring metadata only. Delay is the ONE experimental
// policy dependency; frontend invokes this inside the exclusive reset lease.
unsigned finishTerminal(Terminal &,bool returned,bool seen,uint32_t status,
    Geometry (*readGeometry)(const void *),void (*drain)(unsigned));
void lifecycle(uint64_t packet,Stage);
} }
