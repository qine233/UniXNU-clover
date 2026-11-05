// Host model of the SPMI PMIC arbiter v5, driving drivers/CloverPower/PowerArb.h against software
// registers, so that the transaction logic is proven before it is put next to a PMIC. Ported in shape
// from ~/UniXNU-sheng/package/tests/boot_reset_host.c, which does the same thing for the v7 arbiter
// with a device thread that serves the channels; the reason that split is worth copying is the one
// sheng learned the hard way -- the model caught the ordering questions (which APID wins a tie, what
// a denied transaction looks like, in what byte order the data comes back) before any of it was
// written to hardware that could not be un-broken.
//
// WHAT THIS IS FOR, SPECIFICALLY
//
// CloverPower's whole report is derived from four things: which APID serves a PPID, whether this EE
// owns it, whether a transaction came back, and what bytes came back in what order. The first three
// are register arithmetic a wrong constant silently gets wrong; the fourth is the one that turns a
// correct-looking report into a lie, because a burst read assembled byte-first versus word-first
// would print `type=1b subtype=51` and the identity line would then say the PM660 is a PM660L. None
// of those four can be distinguished on the board without a second instrument, so they are
// distinguished here.
//
// WHY THE MODEL SERVES FROM A SECOND THREAD
//
// pa_wait() polls STATUS until DONE, exactly as the vendor driver does (VEND:258-284), and a
// single-threaded model would have nobody to set that bit. The thread is the PMIC. It is also what
// makes the timeout cases real: stop the thread and the poll bound is the only thing that ends the
// wait -- a driver on a dead bus -- and a channel marked handled without DONE ends it the way a bus
// that answers the channel and has nothing to report does. Leg 10 and leg 10b are both on the books
// because those two shapes are what a `timeout polls=` line on the card has to split apart.
//
// Round331b changed this thread's wait from usleep (50) to sched_yield (), and the reason is the same
// lesson the card-side instruments keep re-learning: the answer's latency and the reader's patience
// were two different clocks. A timer sleep is coalesced by the kernel, so on a host already running a
// build the answer could land milliseconds late -- later than 200000 poll iterations take in wall time
// -- and the gate went red over a scheduling fact about this machine rather than anything about the
// arbiter. scripts/test-clover-power.py now runs the binary a second time with every core busy, so
// that claim is tested and not assumed.
//
// WHAT THE MODEL DELIBERATELY DOES NOT REPRODUCE
//
// It re-arms a channel only when the harness clears STATUS and CMD (rearm()), whereas the real v5
// re-arms on every command word, which is why SHENG can read, write and read again over one APID
// without clearing anything. The model therefore cannot show two identical back-to-back reads on one
// channel -- and the driver never issues one: every repeated read differs in at least the address.
// A single byte register page stands in for a real PMIC's per-physics register space; byte addresses
// wrap at 0x100 because the command word masks them to 8 bits anyway (kPmicArbCmdAddrMask), which is
// the behaviour under test, not the peripheral's own geometry.
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../PowerArb.h"

// Which floor plan this run of the model is pretending to be. Round313 made the harness parameterised
// because the header it tests stopped being one table: the same setMap()/serve() calls have to write
// and read a v5 arbiter (map at 0x900, channels 0x80 apart) and a v3 one (map at 0x800, channels
// 0x8000 apart). Case 3 is the part that was not obvious and is now on the record: reading one band's
// map through the other band's table does not fail in both directions. Writing v5 words and walking
// them at the v3 base silently succeeds with every APID shifted by 64, and only the reverse direction
// -- v3 words below the v5 table's start -- actually refuses. A model that served whichever offset the
// table asked for would have let that asymmetry stay theoretical.
static const struct pa_layout *gL = &pa_layout_v5;
static volatile uint32_t core[kSpmiCoreSize / 4];   // version at +0x00, the APID map at gL->mapBase
static volatile uint32_t cnfg[kSpmiCnfgSize / 4];   // the ownership table at +0x700
// Sized for the widest stride the two bands use: the v5 pass reaches APID 400 at 0x80 apart (0xC800),
// the v3 pass reaches its high APID at 0x8000 apart, so this covers both without pretending the
// driver could map a 16 MB observer window -- see kModelMaxApid below.
static volatile uint32_t obs[0x80000 / 4];          // EE 0's observer block
static uint8_t pmicA[0x100];                        // the peripheral behind sid 0 (PM660)
static uint8_t pmicB[0x100];                        // the peripheral behind sid 2 (PM660L)

static volatile int stopModel, denyNext, failNext, dropNext;
static volatile unsigned readsServed;
static volatile uint32_t lastAddr;
static volatile int lastSid;
// Round331b. rearm() clears every channel's STATUS and CMD while the fake PMIC thread is scanning,
// and serve() writes STATUS from a command it read before that clear. Without a lock the two can
// interleave so that a stale DONE|HANDLED lands after the clear, and the next transaction is answered
// out of the previous command's bytes -- an assertion that passes on the wrong answer. The mutex is
// the harness's own; pa_read() and pa_wait() stay exactly as the driver runs them.
static pthread_mutex_t channelLock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t gModelApidMax;   // the highest APID setMap() has been asked to populate
static uint32_t gHighApid = 400; // the model's second B3 mapping, low in the v3 band (see setUp)

// The three pointers the pure core works on, with the floor plan the model is currently pretending
// to be. Declared here rather than below because setUp() installs the table before any run uses it.
static struct pa_arbiter arbiter = { core, cnfg, obs, kPmicArbEe, &pa_layout_v5 };

// Bit 8 of STATUS is nothing the vendor driver defines (DONE/FAILURE/DENIED/DROPPED are bits 0..3),
// so the model uses it as its own "this command has been served" marker. pa_status_code() only ever
// looks at the low four bits, which is also the point: an arbiter that reported something else in
// STATUS must not change the verdict.
#define kModelHandled (1U << 8)

// Which fake peripheral an APID reaches: the PPID's slave-ID nibble, the same nibble kPpidOf() put
// in bits 11:8 and the same one the driver's four target PPIDs differ by.
static uint8_t *periphOf (uint32_t apid)
{
  uint32_t map = core[(gL->mapBase + gL->mapStride * apid) / 4];
  uint32_t sid = pa_ppid_of (map) >> 8;
  lastSid = (int) sid;
  return sid == kSidPm660l ? pmicB : pmicA;
}

// One channel, if it has a command nobody has served. The command decode here is written out again
// from VEND:1025-1028 rather than calling pa_cmd(), on purpose: if the two ever disagree, the model
// serves something other than what the header asked for and an assertion fires.
static void serve (uint32_t apid)
{
  volatile uint32_t *ch = obs + (gL->obsApidStride * apid) / 4;
  uint32_t cmd = ch[kPmicArbCmd / 4];
  if (!cmd || (ch[kPmicArbStatus / 4] & kModelHandled)) {
    return;   // idle, or already served and waiting for rearm()
  }
  ch[kPmicArbStatus / 4] = kModelHandled;
  const uint32_t op = (cmd >> kPmicArbCmdOpShift) & 0x1f;
  const uint32_t addr = (cmd >> kPmicArbCmdAddrShift) & kPmicArbCmdAddrMask;
  const uint32_t len = (cmd & kPmicArbCmdBcMask) + 1;
  // A write opcode never reaches this model, because PowerArb.h has no write path and the driver is
  // read-only. If either ever grows one, the host test is where it is caught.
  assert(op == kPmicArbReadOp);
  lastAddr = addr;
  if (denyNext) {
    // The failure bit is added when the harness asked for both, which is the only way to ask the
    // arbiter logic what it does when an answer is simultaneously a denial and a fault.
    ch[kPmicArbStatus / 4] = kModelHandled | kPmicArbStatusDone | kPmicArbStatusDenied |
                             (failNext ? kPmicArbStatusFailure : 0U);
    return;
  }
  if (failNext) {
    ch[kPmicArbStatus / 4] = kModelHandled | kPmicArbStatusDone | kPmicArbStatusFailure;
    return;
  }
  if (dropNext) {
    ch[kPmicArbStatus / 4] = kModelHandled | kPmicArbStatusDone | kPmicArbStatusDropped;
    return;
  }
  uint8_t *periph = periphOf (apid);
  // VEND:376-381 memcpy()s from the register word, so RDATA0 holds the first four bytes with the
  // lowest address in the low byte and RDATA1 continues. That little-endian packing is the exact
  // thing the identity burst depends on; assembling it big-endian here would make pa_read() look
  // wrong instead of looking right, which is the point of writing the model by hand.
  uint32_t word0 = 0, word1 = 0;
  for (uint32_t i = 0; i < len; i++) {
    uint8_t value = periph[(addr + i) & 0xff];
    if (i < 4) {
      word0 |= (uint32_t) value << (8 * i);
    } else {
      word1 |= (uint32_t) value << (8 * (i - 4));
    }
  }
  ch[kPmicArbRdata0 / 4] = word0;
  ch[kPmicArbRdata1 / 4] = word1;
  ch[kPmicArbStatus / 4] = kModelHandled | kPmicArbStatusDone;
  readsServed++;
}

static void *device (void *arg)
{
  (void) arg;
  while (!stopModel) {
    // The model only walks the APIDs its own map actually names, because on the v3 stride a full
    // 512-entry sweep would reach 0x1000000 bytes of observer window -- an address the harness has no
    // array for and the driver would have no business mapping on a host either.
    for (uint32_t apid = 0; apid <= gModelApidMax && apid < gL->mapEntries; apid++) {
      if (core[(gL->mapBase + gL->mapStride * apid) / 4]) {
        pthread_mutex_lock (&channelLock);
        serve (apid);
        pthread_mutex_unlock (&channelLock);
      }
    }
    // Round331b: a yield, not a sleep. The thread's wake latency and the reader's patience are two
    // different clocks, and the model had been relying on the first being shorter than the second:
    // usleep (50) is a *timer*, which the kernel is free to coalesce, and on a host already running a
    // kernel build the wake landed milliseconds late -- later than the poll bound spends in wall time.
    // The read then returns the poll's own exit code while the fake PMIC was merely unscheduled, which
    // is r305's two-clocks mistake replayed inside a test, and it made this gate red for a reason that
    // had nothing to do with the arbiter. A runnable thread is scheduled on any free core in
    // microseconds; the thread only lives for the milliseconds each block below needs it.
    sched_yield ();
  }
  return NULL;
}

// Stand-in for the hardware's own per-command re-arm: clear every channel's STATUS (which drops both
// DONE and the model marker) and its latched command.
static void rearm (void)
{
  pthread_mutex_lock (&channelLock);
  for (uint32_t apid = 0; apid <= gModelApidMax && apid < gL->mapEntries; apid++) {
    volatile uint32_t *ch = obs + (gL->obsApidStride * apid) / 4;
    ch[kPmicArbStatus / 4] = 0;
    ch[kPmicArbCmd / 4] = 0;
  }
  pthread_mutex_unlock (&channelLock);
}

static void setMap (uint32_t apid, uint32_t ppid, uint32_t owner, bool irqOwner)
{
  core[(gL->mapBase + gL->mapStride * apid) / 4] =
      (ppid << kPmicArbPpidShift) | (irqOwner ? (1U << kPmicArbIrqOwnerBit) : 0U);
  cnfg[(kPmicArbOwnerTable + kPmicArbOwnerStride * apid) / 4] = owner;
  if (apid > gModelApidMax) {
    gModelApidMax = apid;
  }
}

// One arbiter, in one of the two bands this port claims to know. The APID shapes are the same in both
// -- three mappings for A11 so the tie-break has something to resolve, one for pm660's revid, none for
// pm660l's, and a high-numbered second mapping for B3 so the "highest IRQ claimer" rule has a second
// data point -- and only the high APID differs, because on the v3 stride a 400th channel would sit
// 12 MB into the observer window. What is *not* allowed to differ is the answer: the same map read
// through either table has to produce the same APID, owner and tie-break, and that is what the run
// below asserts twice rather than once.
static void setUp (int v3)
{
  gL = v3 ? &pa_layout_v3 : &pa_layout_v5;
  arbiter.l = gL;
  gHighApid = v3 ? 12u : 400u;
  gModelApidMax = 0;
  memset((void *) core, 0, sizeof core);
  memset((void *) cnfg, 0, sizeof cnfg);
  memset((void *) obs, 0, sizeof obs);
  memset(pmicA, 0, sizeof pmicA);
  memset(pmicB, 0, sizeof pmicB);
  stopModel = denyNext = failNext = dropNext = 0;
  readsServed = 0; lastAddr = 0; lastSid = -1;
  core[kPmicArbVersion / 4] = v3 ? 0x30010000 : 0x50010000;   // inside each band's own range
  core[kPmicArbFeatures / 4] = 40;
  setMap (3, kPpidPm660Revid, 0, false);
  setMap (5, kPpidLdoA11, 1, false);
  setMap (7, kPpidLdoA11, 0, true);
  setMap (9, kPpidLdoA11, 2, false);
  setMap (11, kPpidLdoB3, 3, false);
  setMap (gHighApid, kPpidLdoB3, 0, true);
  // PM660's revid block, 0x100..0x104 read as byte offsets 0x01..0x05 after the command's mask:
  // fab-independent rev2, minor, major, then PMIC_TYPE_VALUE and the PM660 subtype.
  pmicA[0x01] = 0x01; pmicA[0x02] = 0x00; pmicA[0x03] = 0x02;
  pmicA[0x04] = kPmicTypeValue; pmicA[0x05] = kPm660Subtype;
  pmicA[0xf2] = 0x37;                                  // fab_id, only read for PM660 subtypes
  // A PM660L on the other peripheral, so a burst that came back from the wrong APID cannot by
  // accident spell the identity the report claims.
  pmicB[0x04] = kPmicTypeValue; pmicB[0x05] = kPm660lSubtype;
  // LDO A11's enable and voltage pair, at the offsets the dig-LDO map says (kLdoRegEnable 0x46,
  // 0x40/0x41 the voltage pair).
  pmicA[0x40] = 0x4b; pmicA[0x41] = 0x07;              // 0x074b = 1867, times 1000 uV
  pmicA[0x45] = 0x12;
  pmicA[0x46] = 0x81;                                   // ON, with EN0 tracking
  // LDO B3 on the pm660l side, fixed 3.0 V, OFF: the pair the report is supposed to distinguish.
  pmicB[0x40] = 0xc0; pmicB[0x41] = 0x0b;
  pmicB[0x46] = 0x00;
}

// Two bounds, because the two legs ask different questions and one number cannot answer both.
//
// kPolls is the bound for every transaction the model DOES answer. It is not a claim about the PMIC;
// it is a claim about this host: the answer comes from a second thread, so the budget is spent by one
// clock while the answer arrives on another. Round331 measured what the old 200000 meant -- a few
// hundred microseconds of polling, shorter than a loaded host's wake latency -- and the model went red
// under a kernel build for a reason that had nothing to do with the arbiter's arithmetic. The number
// now spans several scheduler quanta, and it stays a number rather than an assumption because
// scripts/test-clover-power.py runs the binary idle AND with one busy process per core: a bound that a
// loaded host can outlast fails the gate instead of reading as a driver defect. The budget is only
// actually spent when something is wrong -- a served read leaves the loop on its first DONE.
//
// kPollsNoAnswer is the bound for the two legs that assert the poll ENDS -- leg 10's channel nobody
// touches, and leg 10b's channel that carries the model's handled marker without DONE. Neither waits
// on anything this machine has to schedule, so the number stays small on purpose: those legs are
// about the bound existing, not about a loaded host's wake latency, and a big one here would only
// make the model slow at saying nothing new.
static const uint32_t kPolls = 100000000;
static const uint32_t kPollsNoAnswer = 5000;

int main (void)
{
  struct pa_scan r;
  uint8_t buf[8];
  uint32_t status = 0;

  // 1. The PPID formula itself. Four named PPIDs the driver looks up, and the arithmetic that has to
  //    produce them from the DT's (sid, base address) pair -- get the shift wrong and every lookup
  //    below silently asks for somebody else's peripheral.
  assert(kPpidOf (kSidPm660, kPmicRevidAddr) == kPpidPm660Revid);
  assert(kPpidOf (kSidPm660l, kPmicRevidAddr) == kPpidPm660lRevid);
  assert(kPpidOf (kLdoA11Sid, kLdoA11Addr) == kPpidLdoA11);
  assert(kPpidOf (kLdoB3Sid, kLdoB3Addr) == kPpidLdoB3);

  // 2. The command word, and the address mask that makes 0x101 and 0x01 the same question. bc is
  //    len-1, so a 5-byte burst is bc=4; a driver that sent bc=5 would read six bytes.
  assert(pa_cmd (kPmicArbReadOp, 0x01, 4) ==
         (uint32_t) ((kPmicArbReadOp << 27) | (0x01 << 4) | 4));
  assert((pa_cmd (kPmicArbReadOp, 0x101 & kPmicArbCmdAddrMask, 0) &
          (kPmicArbCmdAddrMask << kPmicArbCmdAddrShift)) == (0x01u << 4));

  // 3. The version band, which is what round313 changed the meaning of. Below V2_MIN and at or above
  //    V6_MIN still have no floor plan here -- the second is SHENG's v7 family (0x2000 map base, 0x20
  //    stride) and reading it at a v5 address would return a confident, wrong APID instead of an
  //    error -- but [V3_MIN, V5_MIN) is no longer a refusal, it is v3, and VEND:1221-1222 says so.
  setUp (0); core[kPmicArbVersion / 4] = 0x00010000;      // below V2_MIN: v1, unknown
  assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_E_VERSION);
  setUp (0); core[kPmicArbVersion / 4] = kPmicArbVersionV6Min;
  assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_E_VERSION);
  setUp (0); assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_OK && r.l == &pa_layout_v5);
  // The row selector itself, branch for branch against VEND:1212-1224 / MAIN:2102-2110.
  assert(pa_layout_for (0x00010000) == 0);
  assert(pa_layout_for (kPmicArbVersionV2Min) == &pa_layout_v2);
  assert(pa_layout_for (0x20010001) == &pa_layout_v2);
  assert(pa_layout_for (kPmicArbVersionV3Min) == &pa_layout_v3);
  assert(pa_layout_for (0x30010000) == &pa_layout_v3);   // what round183's scan line read
  assert(pa_layout_for (0x03000000) == 0);               // ... and what its refusal line read
  assert(pa_layout_for (0x4fffffff) == &pa_layout_v3);
  assert(pa_layout_for (kPmicArbVersionV5Min) == &pa_layout_v5);
  assert(pa_layout_for (kPmicArbVersionV6Min) == 0);
  // The two tables are not interchangeable -- and, as this pair of assertions found, they do not
  // refuse each other symmetrically. The v3 row walks 0x800..0xfff, which *contains* the v5 row's
  // 0x900..0xfff, so a v5-shaped map read under a v3 version word does not come back empty: it comes
  // back with every APID shifted by (0x900 - 0x800) / 4 == 64, which is a confidently wrong channel
  // number rather than an error. The ownership table is indexed by the shifted APID, so the tie-break
  // changes shape too (all three shifted candidates read owner 0, and the last of them wins). This is
  // the whole reason band= is a printed line and not an assumption: nothing downstream catches it.
  setUp (0);                                        // map written at the v5 base, 0x900
  core[kPmicArbVersion / 4] = 0x30010000;           // ... but the version word says v3
  arbiter.l = &pa_layout_v3;
  assert(pa_scan_apids (&arbiter, kPpidPm660Revid, &r) == PA_OK);
  assert(r.l == &pa_layout_v3 && r.apid == 3 + 64 && r.candidates == 1);
  assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_OK);
  assert(r.candidates == 3 && r.apid == 9 + 64);    // v5's own answer here is 7, not 73
  assert(r.irq_from == kPaIrqFromOwner);            // the IRQ rule moved with the table, silently
  arbiter.l = &pa_layout_v5;
  // The reverse direction does refuse, and that asymmetry is arithmetic: the v5 row starts at 0x900,
  // so the words a v3-shaped map wrote at 0x800..0x830 sit below its first entry and are never read.
  setUp (1);                                        // map written at the v3 base, 0x800
  core[kPmicArbVersion / 4] = 0x50010000;           // ... and the version word says v5
  arbiter.l = &pa_layout_v5;
  assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_E_NOENTRY);
  gL = &pa_layout_v5; arbiter.l = gL;

  // 4. A blank map is not "no candidate", it is "this window is not answering" -- and the two have to
  //    stay different codes, because one is a payload-geometry problem and the other is a board fact.
  setUp (0); memset((void *) core, 0, sizeof core); core[kPmicArbVersion / 4] = 0x50010000;
  assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_E_NOENTRY);
  setUp (0); assert(pa_scan_apids (&arbiter, kPpidPm660lRevid, &r) == PA_E_PPID);

  // 5. The tie-break. Three mappings for one PPID: the walk keeps the first (5), then replaces it
  //    with 7 because this EE owns that one, and does not fall back to 9 afterwards.
  setUp (0); assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_OK);
  assert(r.apid == 7 && r.owner == 0 && r.irq_ee == 0 && r.candidates == 3);
  assert(r.irq_from == kPaIrqFromMapBit);   // v5: the claim is bit 24 of the map word
  // The IRQ-owner answer is a separate rule (VEND:936+): the highest APID for this PPID that claims
  // the bit, which here is 7 -- and 400 claims it for B3, proving the rule is per-PPID.
  assert(r.irq_apid == 7);
  assert(pa_scan_apids (&arbiter, kPpidLdoB3, &r) == PA_OK);
  assert(r.apid == (int) gHighApid && r.owner == 0 && r.irq_apid == (int) gHighApid &&
         r.candidates == 2);
  // Ownership is read per APID out of the cnfg table at 4 * apid, so a stride bug would show up here
  // as the high APID claiming the low one's owner.
  assert(pa_owner_of (&arbiter, 5) == 1 && pa_owner_of (&arbiter, 9) == 2);
  assert(pa_owner_of (&arbiter, gHighApid) == 0);
  // Grouped counts, which is the "may we use this channel at all" summary the report prints.
  setUp (0); assert(pa_scan_apids (&arbiter, kPpidPm660Revid, &r) == PA_OK);
  assert(r.words == gL->mapEntries && r.mapped == 6 && r.apid_span == gHighApid + 1);
  assert(r.per_ee[0] == 3 && r.per_ee[1] == 1 && r.per_ee[2] == 1 && r.per_ee[3] == 1);
  assert(r.irq_per_ee[0] == 2 && r.irq_per_ee[1] == 0);   // bit-24 claimers, counted per EE
  assert(r.irq_none == 4);   // the other four words leave bit 24 clear: VEND:924 calls that INVALID_EE
  // Ask the same PPID as a different EE and the tie-break has to follow that EE, not the first
  // mapping it happened to see.
  arbiter.ee = 1;
  assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_OK && r.apid == 5 && r.owner == 1);
  arbiter.ee = kPmicArbEe;
  // With the EE-0 mapping removed, the driver must report the first candidate and its owner -- the
  // report then says "reads ok, writes denied" instead of pretending the rail is ours.
  setUp (0); core[(gL->mapBase + 4 * 7) / 4] = 0; cnfg[(kPmicArbOwnerTable + 4 * 7) / 4] = 4;
  assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_OK);
  assert(r.apid == 5 && r.owner == 1 && r.candidates == 2 && r.irq_apid < 0);

  // 6. The channel bound, per band. On v5, 512 channels of 0x80 fill one 0x10000 EE block exactly and
  //    the largest APID the map can name is 447, so the last channel ends inside the mapped block. On
  //    v2/v3 the stride is 0x8000, which is why that band's mapping is the whole declared observer
  //    window instead (PowerRegs.h's mapping paragraph) -- and the assertion below checks the *entry
  //    count* each band can name, because a v3 walk that stopped at 448 would silently lose the last
  //    64 channels that band has.
  assert(gL == &pa_layout_v5);
  assert(pa_channel (&arbiter, 447) < obs + gL->obsMapSize / 4);
  assert(pa_channel (&arbiter, 0) == obs);
  assert(pa_obs_block_phys (&pa_layout_v5, 0) == kSpmiObsrvrPhys);
  assert(pa_obs_block_phys (&pa_layout_v5, 2) == kSpmiObsrvrPhys + 2 * kPmicArbObsEeStride);
  assert(pa_obs_block_phys (&pa_layout_v3, 2) == kSpmiObsrvrPhys + 2 * kPmicArbObsEeStrideV23);
  assert(kPmicArbMapEntries == 448 && kPmicArbMapEntriesV23 == 512);
  // The address the owner/ACC-status word is read at, which round182 indexed by APID. VEND:522-523
  // indexes it by 32-APID group, so APID 40 and APID 41 are the same word and APID 0 and 32 are not.
  assert(pa_owner_acc_addr (&pa_layout_v5, 0, 0) == 0x0u);
  assert(pa_owner_acc_addr (&pa_layout_v5, 0, 40) == 4u);    // 40 >> 5 == 1
  assert(pa_owner_acc_addr (&pa_layout_v5, 0, 41) == 4u);
  assert(pa_owner_acc_addr (&pa_layout_v5, 0, 64) == 8u);
  assert(pa_owner_acc_addr (&pa_layout_v3, 0, 40) == 0x200004u);
  assert(pa_owner_acc_addr (&pa_layout_v3, 3, 40) == 0x200004u + 3 * kPmicArbOwnerAccEeStrideV23);
  assert(pa_acc_word_addr (&pa_layout_v5, 7, kPaAccWordEnable) == 0x100u + 7 * 0x10000u);
  assert(pa_acc_word_addr (&pa_layout_v3, 7, kPaAccWordEnable) == 7 * 0x1000u);
  assert(!pa_acc_window_is_intr (&pa_layout_v5) && pa_acc_window_is_intr (&pa_layout_v3));
  // The observer block a v3 board needs is not the one a v5 board needs; if these two ever print the
  // same number the mapping in CloverPower.cpp is lying about one of them.
  assert(gL->obsMapSize == kSpmiObsBlockMapSize && pa_layout_v3.obsMapSize == kSpmiObsrvrSize);

  // 7. The length and range guards, checked before any register is touched: len 0 would be bc = -1,
  //    which the field encodes as 7 -- a 256-byte read nobody asked for. 9 would run past RDATA1.
  assert(pa_read (&arbiter, 3, 0x01, buf, 0, kPolls, &status) == PA_E_LEN);
  assert(pa_read (&arbiter, 3, 0x01, buf, 9, kPolls, &status) == PA_E_LEN);
  assert(pa_read (&arbiter, gL->mapEntries, 0x01, buf, 1, kPolls, &status) == PA_E_APID);

  pthread_t thread;
  setUp (0);
  pthread_create(&thread, NULL, device, NULL);

  // 8. The identity burst, which is MAIN's spmi_regulator_match() shape: five bytes from 0x01 in one
  //    transaction, so they cross the RDATA0/RDATA1 boundary. The assertion is the byte order, not
  //    just the values -- word-first versus byte-first is invisible in a hex dump of the register and
  //    decisive in the report.
  rearm ();
  assert(pa_read (&arbiter, 3, kPmicRev2 & kPmicArbCmdAddrMask, buf, 5, kPolls, &status) == PA_OK);
  assert(buf[0] == 0x01 && buf[1] == 0x00 && buf[2] == 0x02);
  assert(buf[3] == kPmicTypeValue && buf[4] == kPm660Subtype);
  // The reported STATUS keeps the model's own bit 8, and that is worth asserting rather than
  // masking away: pa_status_code() only reads bits 0..3, so an arbiter that reports something else in
  // STATUS cannot change the verdict. lastAddr and lastSid say which peripheral the transaction
  // actually reached -- the APID, not the caller's belief about it.
  assert(status & kPmicArbStatusDone);
  assert((status & kPmicArbStatusErrors) == 0);
  assert(lastAddr == 0x01 && lastSid == (int) kSidPm660);

  // A single byte is the low byte of RDATA0, which is the same claim from the other side: if the
  // header took the high byte, the revid would read 0x00 and the type check would refuse a good part.
  rearm ();
  assert(pa_read8 (&arbiter, 3, kPmicType & kPmicArbCmdAddrMask, buf, kPolls, &status) == PA_OK);
  assert(buf[0] == kPmicTypeValue);
  rearm ();
  assert(pa_read8 (&arbiter, 3, kPmicFabId & kPmicArbCmdAddrMask, buf, kPolls, &status) == PA_OK);
  assert(buf[0] == 0x37);

  // A read through the PM660L's own APID has to see the PM660L's bytes. There is no such mapping in
  // this model -- that is case 4 -- so use B3's high APID, which the map puts on sid 2.
  rearm ();
  assert(pa_read (&arbiter, 400, 0x40, buf, 2, kPolls, &status) == PA_OK);
  assert(buf[0] == 0xc0 && buf[1] == 0x0b && lastSid == (int) kSidPm660l);
  // And A11's enable comes from the sid-0 side, ON with EN0 following, which is the pair the driver
  // decodes as state=ON follow=01.
  rearm ();
  assert(pa_read8 (&arbiter, 7, kLdoRegEnable, buf, kPolls, &status) == PA_OK);
  assert(buf[0] == 0x81 && (buf[0] & kLdoEnableMask) && lastSid == (int) kSidPm660);

  // 9. The three error bits stay three. DONE plus DENIED is an ownership answer, DONE plus FAILURE is
  //    a bus problem, DONE plus DROPPED means the arbiter never put it on the bus -- and a code that
  //    merged them would send the next round after the wrong conclusion. The order of the checks is
  //    asserted too: with both bits set, VEND:261-278 reports the denial first.
  stopModel = 1; pthread_join(thread, NULL); stopModel = 0;
  // Both bits at once is the case that decides the order: VEND:261-278 returns -EPERM for a denial
  // before it looks for a failure, and a code that reversed the two would call an ownership problem
  // a bus fault and send the next round to re-probe clocks that are fine.
  setUp (0); denyNext = 1; failNext = 1; pthread_create(&thread, NULL, device, NULL);
  rearm ();
  assert(pa_read8 (&arbiter, 7, kLdoRegEnable, buf, kPolls, &status) == PA_E_DENIED);
  assert((status & (kPmicArbStatusDenied | kPmicArbStatusFailure)) ==
         (kPmicArbStatusDenied | kPmicArbStatusFailure));
  stopModel = 1; pthread_join(thread, NULL); stopModel = 0;
  setUp (0); failNext = 1; pthread_create(&thread, NULL, device, NULL);
  rearm ();
  assert(pa_read8 (&arbiter, 7, kLdoRegEnable, buf, kPolls, &status) == PA_E_FAILED);
  stopModel = 1; pthread_join(thread, NULL); stopModel = 0;
  setUp (0); dropNext = 1; pthread_create(&thread, NULL, device, NULL);
  rearm ();
  assert(pa_read8 (&arbiter, 7, kLdoRegEnable, buf, kPolls, &status) == PA_E_DROPPED);
  stopModel = 1; pthread_join(thread, NULL);

  // 10. Nobody answers. With the server stopped the poll bound is the only thing that ends this, so
  //     it has to be a number and not an assumption: a bounded poll returns PA_E_POLL and reports the
  //     STATUS it saw -- which is the line the driver prints as `timeout polls=`, and the reason the
  //     driver calibrates that bound instead of guessing it.
  setUp (0);
  rearm ();
  assert(pa_read8 (&arbiter, 7, kLdoRegEnable, buf, kPollsNoAnswer, &status) == PA_E_POLL);
  assert(!(status & kPmicArbStatusDone));

  // 10b. The same bound on a channel that somebody HAS touched. #10 has no server running at all, so
  //      nothing ever writes that word; here the model's own "this command has been served" bit is
  //      present without DONE, which is the shape of a bus that answers the channel and has nothing to
  //      report -- what the card's `timeout polls=` line has to be able to mean. The two legs are not
  //      duplicates: STATUS is the proof, and a poll that only ended when nobody wrote the word would
  //      pass #10 and fail here. The harness writes the bit itself instead of waiting for the scanning
  //      thread to reach this channel on purpose: a claim about pa_wait() must not depend on when this
  //      machine schedules a second thread, and that coupling is what made this model flaky before
  //      round331b. The bound is kPollsNoAnswer, spent in full because this read is meant to end.
  setUp (0);
  rearm ();
  obs[(gL->obsApidStride * 7 + kPmicArbStatus) / 4] = kModelHandled;
  assert(pa_read8 (&arbiter, 7, kLdoRegEnable, buf, kPollsNoAnswer, &status) == PA_E_POLL);
  assert(!(status & kPmicArbStatusDone) && (status & kModelHandled));

  // 11. The same core, run against a v3 arbiter. Everything above is a v5 fake: the map lives at 0x900,
  //     channels are 0x80 apart, and an IRQ claim is bit 24 of a map word. Round313's whole premise is
  //     that this board's version word may well put it in the other row, so the identical questions
  //     are asked of a fake built to the v3 geometry -- map at 0x800, channels 0x8000 apart, no IRQ bit
  //     in the map word at all (VEND:860-861 makes the cnfg owner the IRQ owner) -- and the answers
  //     have to be the same APID, the same owner, the same tie-break and the same five bytes in the
  //     same order. If any of those differed, the difference would be in this port's code, not in the
  //     silicon's, and the board would be the place where it showed up.
  setUp (1);
  assert(gL == &pa_layout_v3);
  assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_OK);
  assert(r.l == &pa_layout_v3 && r.apid == 7 && r.owner == 0 && r.candidates == 3);
  // The band's own IRQ rule, asserted as a difference rather than a coincidence: under v3 the answer
  // comes from cnfg and there is no bit-24 claim to find, so irq_src is the owner and every mapped
  // APID has an answer (irq_none stays 0) -- the opposite of the v5 fake four cases above.
  assert(r.irq_from == kPaIrqFromOwner && r.irq_ee == 0);
  assert(pa_scan_apids (&arbiter, kPpidPm660Revid, &r) == PA_OK);
  assert(r.words == 512 && r.mapped == 6 && r.irq_none == 0 && r.apid == 3);
  pthread_create(&thread, NULL, device, NULL);
  rearm ();
  assert(pa_read (&arbiter, 3, kPmicRev2 & kPmicArbCmdAddrMask, buf, 5, kPolls, &status) == PA_OK);
  assert(buf[0] == 0x01 && buf[1] == 0x00 && buf[2] == 0x02);
  assert(buf[3] == kPmicTypeValue && buf[4] == kPm660Subtype && lastSid == (int) kSidPm660);
  // The APID that reaches the PM660L side is the low one under this band's map -- 12, where v5 put it
  // at 400 -- and the byte pair still has to come back from pmicB, which is the assertion that says
  // the channel arithmetic picked the right peripheral rather than the right-looking offset.
  rearm ();
  assert(pa_read (&arbiter, 12, 0x40, buf, 2, kPolls, &status) == PA_OK);
  assert(buf[0] == 0xc0 && buf[1] == 0x0b && lastSid == (int) kSidPm660l);
  rearm ();
  assert(pa_read8 (&arbiter, 7, kLdoRegEnable, buf, kPolls, &status) == PA_OK);
  assert(buf[0] == 0x81 && (buf[0] & kLdoEnableMask) && lastSid == (int) kSidPm660);
  stopModel = 1; pthread_join(thread, NULL);
  // A v3 channel is 0x8000 apart, so APID 7's block starts at 0x38000 of the observer window. If the
  // driver ever used the v5 stride under a v3 version word, it would be polling a command register
  // that belongs to somebody else's channel and the transaction below would never be answered by a
  // model that only serves the address the layout says. This is that failure made impossible.
  assert(pa_channel (&arbiter, 7) == obs + 7 * (kPmicArbObsApidStrideV23 / 4));

  printf ("PASS: arbiter v5 and v3 PPID maps (tie-break, per-EE ownership, band-specific IRQ rule, "
          "refused bands, the one direction a wrong table refuses and the other one it answers with "
          "an APID shifted by 64), owner/ACC-status group indexing, "
          "per-band channel and access-word arithmetic, command encoding, both poll bounds "
          "(kPollsNoAnswer ends a channel nobody touches and one that is marked handled without DONE, "
          "and every served read answers inside kPolls on a host this script also runs with one busy "
          "process per core), "
          "denied/failed/dropped split, and the little-endian 5-byte burst across RDATA0/RDATA1\n");
  return 0;
}
