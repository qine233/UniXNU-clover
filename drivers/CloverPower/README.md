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
| core | `0x0800f000` | `0x1000` | version, `+0x0004`, and the APID map at `+0x900` (448 entries) |
| chnls | `0x08400000` | `0x1000000` | the *write* channels — mapped one page per rail APID, read-only use |
| obsrvr | `0x09400000` | `0x1000000` | this EE's observer block: `0x10000*ee + 0x80*apid` |
| intr | `0x0a400000` | `0x220000` | per-EE IRQ status, `owner_acc` at `+0x10000*ee + 4*n` |
| cnfg | `0x0800a000` | `0x3000` | the ownership table at `+0x700 + 4*apid`, bits 2:0 |

## The three questions, one per boot argument (`clover-power=N`)

Mode 1 asks what the arbiter itself says and starts **no transaction**: map the windows, read the
version — `VEND:1210` branches on exactly that word to choose its ops table — read `core + 0x0004`
once and print it under *both* decodings the sources give it (`PMIC_ARB_INT_EN` VEND:24, which that
tree never reads, and `PMIC_ARB_FEATURES` MAIN:34, whose bits 10:0 MAIN:1361-1380 treat as the APID
count), then walk the map bounded by the core window's own size, histogram the mapped entries by
write owner, and list the ones belonging to this board's two PMICs.

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
| `core=… ee=0 chan=… intid=…` with non-zero `obs00` | the arbiter answers at its DT address; proceed |
| `below V5_MIN …` / `at or above V6_MIN …` | the floor plan is not v5's — the map base and the channel stride are different there, so the driver stops rather than guess |
| `mode 1 started no transaction; mapped=0` | the boot left the APID table empty for this EE; no PMIC is reachable this boot, and that is a firmware question |
| `write-owner is EE %u, this EE is %u: reads ok, writes denied` | 排期 4's second gate, answered before any write is attempted |
| `scan … apid=… owner=… irq_owner=… irq_apid=…` | which APID was chosen for a PPID under the tie-break, and who gets its interrupt |
| `type is not PMIC_TYPE_VALUE; identity NOT established` | a transaction completed but the peripheral is not what the DT says — nothing downstream is decoded |
| `burst no, singles yes: sequential reads unproven on this arbiter` | the arbiter answers single bytes only — every later multi-byte read must be shaped that way |
| `enable=81 state=on follow=…` / `enable=00 state=off` | the answer round181's NACK needed: `0x80` is ON (VEND:212-214); an `00` in both rails with a NACKed bus means software, not silicon |
| `0x40/0x41 raw=… dt=…` | the raw selector bytes; microvolts are claimed only for MAIN's L660 subtypes, which use the FTSMPS426 `raw * 1000` layout |

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
window, where a v5 *write* command would go (VEND:1013), is mapped one page per rail APID solely to
read that page's `ACC_ENABLE` and `IRQ_STATUS` words (VEND:1067-1069, :1085-1087), and no CMD word is
ever written there. The one write in this driver is the command word that starts a read, into the
observer channel's CMD, which is what a read *is*: VEND:370 `pmic_arb_set_rd_cmd()` writes
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
| switch | `board.toml` → `kernel_args` | `clover-power=3` |
| host gate | `scripts/test-clover-power.py` | run by `scripts/check-clover.py`; a nonzero return is a failed round |
| verify | `scripts/verify-clover-payload.py` | `kernel.macho: pmic arbiter instrument linked`, seven strings |

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
