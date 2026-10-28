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
// makes the timeout case real: stop the thread, and the poll bound is the only thing that ends the
// wait -- which is the path a driver on a dead bus takes, and the one this test can only exercise by
// killing the server.
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
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../PowerArb.h"

static volatile uint32_t core[kSpmiCoreSize / 4];   // version at +0x00, the APID map at +0x900
static volatile uint32_t cnfg[kSpmiCnfgSize / 4];   // the ownership table at +0x700
static volatile uint32_t obs[kSpmiObsBlockMapSize / 4];   // EE 0's observer block, 512 channels
static uint8_t pmicA[0x100];                        // the peripheral behind sid 0 (PM660)
static uint8_t pmicB[0x100];                        // the peripheral behind sid 2 (PM660L)

static volatile int stopModel, denyNext, failNext, dropNext, neverAnswer;
static volatile unsigned readsServed;
static volatile uint32_t lastAddr;
static volatile int lastSid;

// Bit 8 of STATUS is nothing the vendor driver defines (DONE/FAILURE/DENIED/DROPPED are bits 0..3),
// so the model uses it as its own "this command has been served" marker. pa_status_code() only ever
// looks at the low four bits, which is also the point: an arbiter that reported something else in
// STATUS must not change the verdict.
#define kModelHandled (1U << 8)

// Which fake peripheral an APID reaches: the PPID's slave-ID nibble, the same nibble kPpidOf() put
// in bits 11:8 and the same one the driver's four target PPIDs differ by.
static uint8_t *periphOf (uint32_t apid)
{
  uint32_t map = core[(kPmicArbApidMap + kPmicArbApidMapStride * apid) / 4];
  uint32_t sid = pa_ppid_of (map) >> 8;
  lastSid = (int) sid;
  return sid == kSidPm660l ? pmicB : pmicA;
}

// One channel, if it has a command nobody has served. The command decode here is written out again
// from VEND:1025-1028 rather than calling pa_cmd(), on purpose: if the two ever disagree, the model
// serves something other than what the header asked for and an assertion fires.
static void serve (uint32_t apid)
{
  volatile uint32_t *ch = obs + (kPmicArbObsApidStride * apid) / 4;
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
  if (neverAnswer) {
    ch[kPmicArbStatus / 4] = kModelHandled;   // no DONE: the poll has to end this
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
    for (uint32_t apid = 0; apid < kPmicArbMapEntries; apid++) {
      if (core[(kPmicArbApidMap + kPmicArbApidMapStride * apid) / 4]) {
        serve (apid);
      }
    }
    usleep (50);
  }
  return NULL;
}

// Stand-in for the hardware's own per-command re-arm: clear every channel's STATUS (which drops both
// DONE and the model marker) and its latched command.
static void rearm (void)
{
  for (uint32_t apid = 0; apid < kPmicArbMapEntries; apid++) {
    volatile uint32_t *ch = obs + (kPmicArbObsApidStride * apid) / 4;
    ch[kPmicArbStatus / 4] = 0;
    ch[kPmicArbCmd / 4] = 0;
  }
}

static void setMap (uint32_t apid, uint32_t ppid, uint32_t owner, bool irqOwner)
{
  core[(kPmicArbApidMap + kPmicArbApidMapStride * apid) / 4] =
      (ppid << kPmicArbPpidShift) | (irqOwner ? (1U << kPmicArbIrqOwnerBit) : 0U);
  cnfg[(kPmicArbOwnerTable + kPmicArbOwnerStride * apid) / 4] = owner;
}

// A v5 arbiter with the four PPIDs this board actually asks about, three of them deliberately
// ambiguous: LDO A11 is mapped by APID 5 (owned by EE 1), 7 (owned by EE 0, and the IRQ claimer) and
// 9 (owned by EE 2), which is the shape VEND:930-932 exists to resolve. PM660's revid gets one
// mapping, PM660L's none -- a rail the tree does not populate has to be reported as unreachable,
// not read through somebody else's APID.
static void setUp (void)
{
  memset((void *) core, 0, sizeof core);
  memset((void *) cnfg, 0, sizeof cnfg);
  memset((void *) obs, 0, sizeof obs);
  memset(pmicA, 0, sizeof pmicA);
  memset(pmicB, 0, sizeof pmicB);
  stopModel = denyNext = failNext = dropNext = neverAnswer = 0;
  readsServed = 0; lastAddr = 0; lastSid = -1;
  core[kPmicArbVersion / 4] = 0x50010000;   // inside the v5 band the header accepts
  core[kPmicArbFeatures / 4] = 40;
  setMap (3, kPpidPm660Revid, 0, false);
  setMap (5, kPpidLdoA11, 1, false);
  setMap (7, kPpidLdoA11, 0, true);
  setMap (9, kPpidLdoA11, 2, false);
  setMap (11, kPpidLdoB3, 3, false);
  setMap (400, kPpidLdoB3, 0, true);
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

static struct pa_arbiter arbiter = { core, cnfg, obs, kPmicArbEe };
static const uint32_t kPolls = 200000;   // kPmicArbPolls; a host model answers in microseconds

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

  // 3. The version band. V6_MIN and above is SHENG's v7 floor plan (0x2000 map base, 0x20 stride),
  //    where the v5 offsets would return a confident, wrong APID instead of an error.
  setUp (); core[kPmicArbVersion / 4] = 0x30000000;
  assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_E_VERSION);
  setUp (); core[kPmicArbVersion / 4] = kPmicArbVersionV6Min;
  assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_E_VERSION);
  setUp (); assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_OK);

  // 4. A blank map is not "no candidate", it is "this window is not answering" -- and the two have to
  //    stay different codes, because one is a payload-geometry problem and the other is a board fact.
  setUp (); memset((void *) core, 0, sizeof core); core[kPmicArbVersion / 4] = 0x50010000;
  assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_E_NOENTRY);
  setUp (); assert(pa_scan_apids (&arbiter, kPpidPm660lRevid, &r) == PA_E_PPID);

  // 5. The tie-break. Three mappings for one PPID: the walk keeps the first (5), then replaces it
  //    with 7 because this EE owns that one, and does not fall back to 9 afterwards.
  setUp (); assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_OK);
  assert(r.apid == 7 && r.owner == 0 && r.irq_owner == 1 && r.candidates == 3);
  // The IRQ-owner answer is a separate rule (VEND:936+): the highest APID for this PPID that claims
  // the bit, which here is 7 -- and 400 claims it for B3, proving the rule is per-PPID.
  assert(r.irq_apid == 7);
  assert(pa_scan_apids (&arbiter, kPpidLdoB3, &r) == PA_OK);
  assert(r.apid == 400 && r.owner == 0 && r.irq_apid == 400 && r.candidates == 2);
  // Ownership is read per APID out of the cnfg table at 4 * apid, so a stride bug would show up here
  // as the high APID claiming the low one's owner.
  assert(pa_owner_of (&arbiter, 5) == 1 && pa_owner_of (&arbiter, 9) == 2);
  assert(pa_owner_of (&arbiter, 400) == 0);
  // Grouped counts, which is the "may we use this channel at all" summary the report prints.
  setUp (); assert(pa_scan_apids (&arbiter, kPpidPm660Revid, &r) == PA_OK);
  assert(r.words == kPmicArbMapEntries && r.mapped == 6 && r.apid_span == 401);
  assert(r.per_ee[0] == 3 && r.per_ee[1] == 1 && r.per_ee[2] == 1 && r.per_ee[3] == 1);
  assert(r.irq_per_ee[0] == 2 && r.irq_per_ee[1] == 0);
  // Ask the same PPID as a different EE and the tie-break has to follow that EE, not the first
  // mapping it happened to see.
  arbiter.ee = 1;
  assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_OK && r.apid == 5 && r.owner == 1);
  arbiter.ee = kPmicArbEe;
  // With the EE-0 mapping removed, the driver must report the first candidate and its owner -- the
  // report then says "reads ok, writes denied" instead of pretending the rail is ours.
  setUp (); core[(kPmicArbApidMap + 4 * 7) / 4] = 0; cnfg[(kPmicArbOwnerTable + 4 * 7) / 4] = 4;
  assert(pa_scan_apids (&arbiter, kPpidLdoA11, &r) == PA_OK);
  assert(r.apid == 5 && r.owner == 1 && r.candidates == 2 && r.irq_apid < 0);

  // 6. The channel bound. 512 channels of 0x80 fill one EE block exactly, and the largest APID the
  //    map can name is 447 -- so the last channel has to end inside the window the driver maps.
  assert(pa_channel (&arbiter, 447) < obs + kSpmiObsBlockMapSize / 4);
  assert(pa_channel (&arbiter, 0) == obs);
  assert(pa_obs_block_phys (0) == kSpmiObsrvrPhys);
  assert(pa_obs_block_phys (2) == kSpmiObsrvrPhys + 2 * kPmicArbObsEeStride);

  // 7. The length and range guards, checked before any register is touched: len 0 would be bc = -1,
  //    which the field encodes as 7 -- a 256-byte read nobody asked for. 9 would run past RDATA1.
  assert(pa_read (&arbiter, 3, 0x01, buf, 0, kPolls, &status) == PA_E_LEN);
  assert(pa_read (&arbiter, 3, 0x01, buf, 9, kPolls, &status) == PA_E_LEN);
  assert(pa_read (&arbiter, kPmicArbMapEntries, 0x01, buf, 1, kPolls, &status) == PA_E_APID);

  pthread_t thread;
  setUp ();
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
  setUp (); denyNext = 1; failNext = 1; pthread_create(&thread, NULL, device, NULL);
  rearm ();
  assert(pa_read8 (&arbiter, 7, kLdoRegEnable, buf, kPolls, &status) == PA_E_DENIED);
  assert((status & (kPmicArbStatusDenied | kPmicArbStatusFailure)) ==
         (kPmicArbStatusDenied | kPmicArbStatusFailure));
  stopModel = 1; pthread_join(thread, NULL); stopModel = 0;
  setUp (); failNext = 1; pthread_create(&thread, NULL, device, NULL);
  rearm ();
  assert(pa_read8 (&arbiter, 7, kLdoRegEnable, buf, kPolls, &status) == PA_E_FAILED);
  stopModel = 1; pthread_join(thread, NULL); stopModel = 0;
  setUp (); dropNext = 1; pthread_create(&thread, NULL, device, NULL);
  rearm ();
  assert(pa_read8 (&arbiter, 7, kLdoRegEnable, buf, kPolls, &status) == PA_E_DROPPED);
  stopModel = 1; pthread_join(thread, NULL);

  // 10. Nobody answers. With the server stopped the poll bound is the only thing that ends this, so
  //     it has to be a number and not an assumption: a bounded poll returns PA_E_POLL and reports the
  //     STATUS it saw -- which is the line the driver prints as `timeout polls=`, and the reason the
  //     driver calibrates that bound instead of guessing it.
  setUp ();
  rearm ();
  assert(pa_read8 (&arbiter, 7, kLdoRegEnable, buf, 5000, &status) == PA_E_POLL);
  assert(!(status & kPmicArbStatusDone));

  printf ("PASS: arbiter v5 PPID map (tie-break, per-EE ownership, IRQ rule, refused versions), "
          "command encoding, bounded poll, denied/failed/dropped split, and the little-endian "
          "5-byte burst across RDATA0/RDATA1\n");
  return 0;
}
