# CloverPower — asking a PMIC what state a rail is in, without touching it

round181 asked the touch controller who it was and closed with `rails unproven`. That line is the
reason this driver exists: both DT touch nodes declare supplies (`clover.dts` `focaltech@38`
`vdd-supply` / `vcc_i2c-supply`, and the same pair on `gt9xx@5d`), and a NACK on I2C only means
"no chip at that address" if the chip has power. Without a rail reading, "the controller is
missing" and "nobody turned it on" are one observation — and a round that cannot separate them
sends the next round after a phantom.

So this driver opens the only door a kernel has to a PMIC, the SPMI PMIC Arbiter, and reads in
order: the arbiter's own identity, the APID→PPID map the boot left behind, which execution
environment may write each APID, the two PMICs' revid blocks, and the enable and voltage registers
of the two rails in question.

It is also the prerequisite for 排期 4's second gate. That gate is "may XNU switch a rail on at
all", and the answer is not in the command encoding — it is in `cnfg + 0x700 + 4*apid`, which names
the EE allowed to write that APID (VEND:50-51, read at :920-922). If that number is not 0, XNU —
EE 0, per DTB `qcom,ee = <0x00>` — gets a hardware-denied transaction however correct its command
word is, and the port has to ask RPM instead. This round produces that number *before* anyone
writes anything.

## The windows, from this board's own device tree

`/proc/device-tree/soc/qcom,spmi@800f000/reg` (cells transcribed in `PowerRegs.h`, each bound in
`check-clover.py`'s DT contract):

| block | physical | size | what lives there |
| --- | --- | --- | --- |
| core | `0x0800f000` | `0x1000` | version, `+0x0004`, and the APID map — at `+0x900` (448 entries) on v5, at `+0x800` (512) on v2/v3 |
| chnls | `0x08400000` | `0x1000000` | the *write* channels, and on v5 also the per-APID `ACC_ENABLE`/`IRQ_STATUS` words |
| obsrvr | `0x09400000` | `0x1000000` | this EE's observer block: `0x10000*ee + 0x80*apid` on v5, `0x1000*ee + 0x8000*apid` on v2/v3 |
| intr | `0x0a400000` | `0x220000` | per-EE owner/ACC status; on v2/v3 also the `ACC_ENABLE`/`IRQ_STATUS` words |
| cnfg | `0x0800a000` | `0x3000` | the ownership table at `+0x700 + 4*apid`, bits 2:0 — same in every band |

The right-hand column is a function of the version word, not a constant. `PowerArb.h` keeps one row
per band (`pa_layout_v2/v3/v5`) and `pa_layout_for()` installs the row the way `VEND:1212-1224`
installs its ops table; round313 is the round that replaced round182's single "is it v5?" test with
that table, because the board's own version read lands in the v3 band and a v5-only floor plan made
every rail behind this door unanswerable for a reason that was really "we only wrote down one band's
addresses". `notes/power-bringup.md` carries the two archived trees this rests on.

## The three questions, one per boot argument (`clover-power=N`)

Mode 1 asks what the arbiter itself says and starts **no transaction**: read the version word twice,
print it in four renderings (hex, decimal, nibble-by-nibble, and the second read with a `stable=`
verdict), install the floor plan that word selects, print the band with both neighbouring floors,
then read `core + 0x0004` once and print it under *both* decodings the sources give it (`INT_EN`
VEND:24, which that tree never reads, and `FEATURES` MAIN:34, whose bits 10:0 MAIN:1361-1380 treat
as the APID count) with a `differs=` that says whether the two agree — the control that tells a
reader whether the core window is answering at all. Only then does it map the remaining windows at
the *band's* sizes, walk the map bounded by the core window's own size, histogram the mapped entries
by write owner, and list the ones belonging to this board's two PMICs.

The four renderings exist because round183's photograph of this one word gave two transcriptions of
it (`core=03000000` on one line, `ver=30000000` on the next) and no further look at the photo can
settle which is right: `0x03000000` is below `V2_MIN` and has no known table, `0x30000000` is v3 and
does. That difference is the whole round.

Mode 2 does mode 1 and asks the two PMICs who they are, as one 5-byte burst across the revid block
and, if the burst does not come back, as the same five bytes asked one at a time. A QPNP device
must report `0x51` at `+0x04` (MAIN:26 `PMIC_TYPE_VALUE`) and MAIN refuses to name the chip
otherwise; subtype `0x1b` is pm660 and `0x1a` is pm660l.

Mode 3 does mode 2 and reads the rails — `ldo-a11-vcc_i2c` (PPID `0x04a`, sid 0, DT range
1.78–1.95 V) and `ldo-b3-vdd` (PPID `0x242`, sid 2, DT 3.0 V fixed): the 5-byte identity burst, a
2-byte voltage burst at `0x40`, then mode `0x45` and enable `0x46`.

How to read the report:

| what appears | what it means |
| --- | --- |
| `arbiter_ver hex=… dec=… nib=…-…-… again=… stable=…` | the one word, four ways, plus the second read — `stable=0` means the word moved between the two reads and no floor plan below it can be trusted |
| `band=v3 floor=30000000 next=50000000 …` | which floor plan `pa_layout_for()` installed, named with the floors either side of it so the reader can re-derive the branch |
| `band=none floors v2=… v6=…` | the word is outside every table this port has (v1 below `V2_MIN`, v6 and up) — the driver stops and says so rather than reading a wrong layout confidently |
| `core+0004=… main_apids=… (mask …) differs=…` | the control: if `differs=1` the map walk's bound and MAIN's APID count disagree, which is a question about this silicon, not about this driver |
| `layout=… map=0x…+%u x%u entries=%u obs=ee*0x…+apid*0x…+%u …` | the floor plan in one line: every address formula the rest of the report was produced with |
| `%s apid=… group=… intr+0x… owner_acc=…` | the owner/ACC word, with the 32-APID **group** index it was read at spelled out (round182 indexed this by APID; the vendor indexes by group) |
| `acc words are intr offsets on v3; no chnls page mapped` | on v2/v3 the enable/IRQ words are not in `chnls` — this line is the refusal to print a `chnls` word under the name of an access-port enable |
| `mode 1 started no transaction; mapped=0` | the boot left the APID table empty for this EE; no PMIC is reachable this boot, and that is a firmware question |
| `write-owner is EE %u, this EE is %u: reads ok, writes denied` | 排期 4's second gate, answered before any write is attempted |
| `scan … apid=… owner=… irq_ee=… irq_src=… irq_apid=…` | which APID was chosen for a PPID under the tie-break, who gets its interrupt, **and which band rule produced that answer** (`map_bit24` on v5, `cnfg_owner` on v2/v3) |
| `type is not PMIC_TYPE_VALUE; identity NOT established` | a transaction completed but the peripheral is not what the DT says — nothing downstream is decoded |
| `burst no, singles yes: sequential reads unproven on this arbiter` | the arbiter answers single bytes only — every later multi-byte read must be shaped that way |
| `enable=81 state=on follow=…` / `enable=00 state=off` | the answer round181's NACK needed: `0x80` is ON (VEND:212-214); an `00` in both rails with a NACKed bus means software, not silicon |
| `0x40/0x41 raw=… dt=…` | the raw selector bytes; microvolts are claimed only for MAIN's L660 subtypes, which use the FTSMPS426 `raw * 1000` layout |

## Why the floor plan is data

Because the two tables this port knows are not equally safe against each other, and the host model
found that rather than the comment assuming it. The v2/v3 map starts at `0x800` and runs to the end
of the core window, so it *contains* v5's `0x900..0xfff`: a v5-shaped map walked under a v3 version
word still finds every PPID, at an APID number 64 too high, and because ownership and the IRQ rule
are indexed off that same APID the tie-break silently changes shape as well. The reverse direction
does refuse — v5's walk starts above the words a v3 map wrote, reads zeros, and reports
`PA_E_NOENTRY`. Nothing downstream of `pa_layout_for()` catches a mis-banded read in the direction
that *does* answer, so the version word is the only defence, and it is printed four ways.

## Why the transaction logic is not in `CloverPower.cpp`

Because sheng proved the split pays for itself. `~/UniXNU-sheng/package/BootResetCore.h` keeps its
arbiter logic as inline functions over plain `volatile uint32_t *`, and
`package/tests/boot_reset_host.c` drives those exact functions on a Mac against a software model of
the arbiter with a device thread serving the channels; the model caught the ordering bugs — which
APID wins when two map to one PPID, what a denial looks like — before any of it reached a PMIC that
could not be un-broken.

Here the same split is `PowerArb.h` (the map walk, the ownership lookup, the bounded read) plus
`drivers/CloverPower/tests/clover_power_host.cpp` and `scripts/test-clover-power.py`, which `scripts/check-clover.py`
runs as a gate. The model re-decodes every command the way `VEND:1025-1028` does and `assert`s the
opcode is a read, so a write path is caught on the host; and the poll bound is proven by a case that
runs with no server thread at all — if `pa_wait()` could spin forever, the harness hangs and the
gate fails.

## Why it is read-only, in the hard sense

Every transaction is an `EXT_READL` on an *observer* channel. VEND:124-125 states the rule — from v2
on `rd_base` is "obsrvr" and `wr_base` is "chnls" — so a read cannot land on a PMIC input. The chnls
window, where a v5 *write* command would go (VEND:1013), is mapped one page per rail APID on v5 only
and solely to read that page's `ACC_ENABLE` and `IRQ_STATUS` words (VEND:1067-1069, :1085-1087); on
v2/v3 those two words are `intr` offsets instead (VEND:1060-1064, :1078-1082), and the driver maps
no chnls page at all and prints which window it used. No CMD word is ever written to chnls in either
band. The one write in this driver is the command word that starts a read, into the observer
channel's CMD, which is what a read *is*: VEND:370 `pmic_arb_set_rd_cmd()` writes
`rd_base + PMIC_ARB_CMD`.

That guarantee is machine-checked, not just stated: `check-clover.py` fails if `windowWrite` or
`kPmicArbWriteOp` appears in the driver at all.

A rail that turns out to be off stays off. Enabling it is the round after this one, and that round
has to earn the ownership answer printed here first.

## Why nothing here panics

The reasoning round176 established and round181 applied: the kernel log reaches the host by itself
(`IOLog` writes msgbuf, CloverSdhci's tee copies any line containing `clover r` into the card-backed
kernel ring, `scripts/read-clover-log.py --tee` reads it off the card). A power reading is worth more
than a halt, and a boot that finds an empty APID map still reaches launchd with the emptiness
recorded. Every wait is bounded twice — a poll count calibrated against the generic timer, and the
arbiter's own status bits — and every refusal has its own code (`PA_E_VERSION`, `PA_E_LEN`,
`PA_E_APID`, `PA_E_POLL`, `PA_E_DENIED`, `PA_E_FAILED`, `PA_E_DROPPED`).

## Link wiring

| piece | file | note |
| --- | --- | --- |
| compile | `scripts/build-arm-driver.py CloverPower` | the recorded `IOPlatformExpert.cpo.json` flags, plus the ARM64/iOS 17 object audit |
| archive | `build/clover-power/libCloverPower.a` | plain archive path, no `force_load`; `_clover_power_run` is what pulls the member in |
| link | `configs/clover_defconfig` → `CONFIG_XNU_EXTRA_LD_ARCHIVES` | fourth entry, after CloverSdhci / CloverBlock / CloverInput |
| schedule | `drivers/CloverPMGR/CloverPMGR.cpp` | `clover_power_run()` at the very tail of `sheng_pmgr_run()`, after `clover_input_run()` |
| switch | `board.toml` → `kernel_args` | `clover-power=1` (round313 ships the pure-read control: it writes no byte, starts no transaction, and its whole job is to name the band) |
| host gate | `scripts/test-clover-power.py` | run by `scripts/check-clover.py`; a nonzero return is a failed round |
| verify | `scripts/verify-clover-payload.py` | `kernel.macho: pmic arbiter instrument linked`, fifteen strings |

It runs last in the PMGR tail on purpose: this report is the explanation of CloverInput's last line,
and both leave through the same card-backed ring, so their order on the card is the order they were
asked.

## What this is not

No clocks. The DTB's arbiter node declares none, VEND's probe requests none, and MAIN's read path
asks for none, so mode 1 tests that assumption by reading core and reporting what it gets rather
than inventing a GCC sequence to vote something that may not exist. No voltage setting, no enable
write, no mode write, no pull-down, no soft-start, no RPM protocol. And no claim about a rail's
microvolts for a subtype this port has not proven: for anything but MAIN's L660 parts `0x40` is a
range selector and `0x41` an index into a table nobody has read, so the report prints the raw bytes
and says so.
## How this driver meets the UniXNU driver documentation

Layer, wiring and object ABI are the documented ones: `docs/drivers/overview.md:11-16`,
`docs/drivers/xnu-drivers.md:118-124` (`build-arm-driver.py CloverPower` in `[kernel] archives`) and
`docs/drivers/xnu-drivers.md:106-116`, the last of which `build-arm-driver.py:120,129` checks. The
archive reaches the link through `CONFIG_XNU_EXTRA_LD_ARCHIVES` (`configs/clover_defconfig:70`), appended
raw at `build-xnu-4k-baseline.sh:1002` and so without the `-Wl,-force_load` of
`docs/drivers/xnu-drivers.md:128-145` — measured in `notes/unixnu-doc-conformance.md` §4. The five
windows are board constants because `docs/drivers/xnu-drivers.md:215` is what applies to a device below
the `arm-io` range, and each one is bound to the flashed tree by `check_device_tree_constants()`.

It has the class-shape gap of `docs/drivers/xnu-drivers.md:19-64` the other clover instruments have:
no IOService subclass, no personality, no `Info.plist` (`docs/drivers/xnu-drivers.md:14` treats the
plist as a personality's record), reached instead as
`extern "C" void clover_power_run(void)` chained from `CloverPMGR` — §5 of the conformance note.

What it does have, and what makes it the template rather than the exception, is the fourth test row of
`docs/drivers/xnu-drivers.md:230-239`: the floor-plan arithmetic is a pure header (`PowerArb.h`),
`tests/clover_power_host.cpp` compiles it against a fake arbiter on the host, and
`scripts/test-clover-power.py` runs it — rule 1 of `docs/drivers/sheng-novatek-touch.md:18-39` and
rule 3 of `:141-148`.
Round313 is the round that proves the value of that pairing: the v3-vs-v5 direction question ("reading
the wrong band's table silently shifts every APID instead of failing") was answered by the host model
before any board was asked, and the vendor sources it rests on are cited slot by slot
(`VEND:`/`MAIN:` against the two archived `spmi-pmic-arb.c` copies) rather than remembered. Its argument
is shipped at the level the logs have earned: `clover-power=1` (`board.toml:2489`) is reads only.
