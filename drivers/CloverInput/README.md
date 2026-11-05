# CloverInput — asking the touch bus who is there, and refusing to guess

排期 4's question is small and load-bearing: is there a chip at the touch addresses at all. Round181
built this instrument because the answer changes the meaning of every later touch reading — a NACK from
an unpowered controller and a NACK from an absent one are the same waveform on the bus, and the port
spent rounds treating them as one event.

The verdict is deliberately not a boot-time print. It is *state on the beat*, written into the kernel
ring that `drivers/CloverSdhci` tees onto the card (round260's decision), so a late answer is still
readable and a missing line means something measurable rather than nothing.

## The bus, from this board's own device tree

`InputRegs.h:39-56` carries the node verbatim: `i2c@c177000`, `compatible = "qcom,i2c-msm-v2"`,
`reg = <0xc177000 0x600>`, `interrupts` = GIC SPI 97 ⇒ INTID 129, `qcom,clk-freq-out` = 400000,
`qcom,clk-freq-in` = 19200000, `qcom,i2c-dat`/`-clk` = TLMM gpio10/gpio11.

| constant | value | note |
| --- | --- | --- |
| `kQupPhys` | `0x0c177000` | seven hex digits, not six |
| `kQupSize` | `0x1000` | mapped extent — `IODeviceMemory::map()` works at page granularity |
| `kQupDtSize` | `0x600` | what the DT actually declares, printed so the two are never confused |
| `kQupIntid` | `129` | SPI 97 + 32 |

The header also records the mistake instead of quietly fixing it: round181 shipped `0x00c17700`, a base
a hundredth of the way there that lands where no QUP exists, decoded here from
`facts/81ffcaaa-20261022-210928/dt/soc-reg.txt` (`i2c@c177000/reg 0070170c 00060000`) and now refused at
build time by `check-clover.py`'s `check_device_tree_constants()` (`:8093`) through its two tables — the
`kQupPhys`/`kQupDtSize` row of `DT_BASE_CONTRACT` (`:7980-7981`) and the `kPinGpio*`/`kTlmm*` rows of
`DT_WINDOW_CONTRACT` (`:8031-8035`).

Two candidates are probed, in the order the vendor tree's config blob decides: Goodix `gt9xx` at 0x5d
(`InputRegs.h:327-328`) and EDT/FocalTech at 0x38 (`InputRegs.h:345-346`), the tie-break being that the
two vendor configuration blobs are byte-identical — the claim the source makes in its own
`WHY THIS FILE EXISTS` block.

## Three modes, and the first one that writes

`clover-input` is read by `PE_parse_boot_argn` at the head of `clover_input_run()`; any value other
than 1, 2 or 3 prints `mode %u is not implemented` and returns (`clover_input_run()`). The board ships
`clover-input=3` in `kernel_args` (`board.toml:2422`) and `check-clover.py:704-709` refuses a release
that drops it — round259 resumed this driver in the same flight as the synthetic digitizer frame,
because real fingers and synthetic fingers are two halves of one question.

| mode | what it does | first write? |
| --- | --- | --- |
| 1 | read-only dump — `dumpClocks()`, four `dumpPin()` calls, `dumpQup("before")`, then `mode 1 wrote nothing` | no (the `Mode 1 asks what state the firmware left the bus in` paragraph, the `mode == 1` branch of `clover_input_run()`) |
| 2 | bus bring-up: vote AHB, program the RCG, vote the apps branch, mux the pins — `voteAhb()` → `programRcg()` → `enableBranch()` → `programI2cPins()` → `bringUpCore()` | **yes** — the AHB vote inside `voteAhb()` (the `Mode 2` paragraph, the bring-up chain and its `bring-up ok fifo` row) |
| 3 | mode 2 plus the identity reads at both candidate addresses | as mode 2 (the `Mode 3` paragraph, the `probeCandidate()` calls in `clover_input_run()`) |

## The four ways this bus can lie, and what the code does about each

* **A done flag with no bytes.** Upstream's FIFO-mode write path lets any handled flag end a
  sub-transfer; here the done bit *and* both byte counts have to agree, so a transfer that ended early
  is a timeout and not short data that looks real (the `Stricter than upstream's FIFO-mode write path`
  comment and its test inside `runSegment()`), bounded at 100 ms by that loop's `deadline`.
* **Stale done flags.** Every transfer is headed by `SW_RESET` (`bringUpCore()`'s opening reset, and
  the `Upstream resets the mini-core at the head of every transfer` call inside `chipRead()`), the error
  path re-resets (the `qup_i2c_error()` comment in `runSegment()`), and a transfer ends only after
  `finishTransfer()` sees `I2C_STATUS` report bus-idle and `changeState()` confirm the reset state.
* **Reading a status that is not valid yet.** `changeState()` waits for `STATE_VALID` before and after
  every write, and `bringUpCore()` confirms `MASTER_GEN` through `STATE` bit 4 (`InputRegs.h:233`)
  rather than by reading the configuration word back.
* **Poling the wrong word.** `voteAhb()` proves an AHB vote by the halt register at `0x117004`, not by
  the vote register itself. A tag byte count of 0 means 256, so `chipRead()` refuses a zero-length read
  and the block comment above `printBytes()` — "a bare does-this-address-ACK ping is deliberately NOT
  provided" — records why: the smallest honest question this core can ask is one register byte read
  back, which is what `probeCandidate()` sends.

## The bus-free layer, and the reading round181 could not print

Round318 moved every decision this driver reads out of a word into `InputCore.h`, a header that includes
only `<stdint.h>` and its own `InputRegs.h` — so every mask is still derived from the vendor's named
register, and no second copy of a cut survives in `CloverInput.cpp`. The layer owns the QUP v2 tag
arithmetic (`input_write_tags()`, `input_read_tags()`), the two FIFO cursors (`input_tx_next()`,
`input_rx_word()`/`input_rx_pop()`), the per-transfer CONFIG word and done mask, the three TLMM pin cuts,
the RCG word, the verdict classes (`input_xfer_verdict()`) and the Goodix/FocalTech identification.

`tests/clover_input_host.cpp` drives it under `scripts/test-clover-input.py` (263 checks) and the build
gate **runs** that test rather than reading it: every refusal class — NACK, bus error, core error,
incomplete — is reached with a real word, the two Goodix reads are compared byte-wise before any
identification, a three-character pid keeps its blank fourth slot, and the DIRECTION cases show what the
wrong mask would report. The driver calls each decision; the pins that matter are listed in
`check-clover.py`'s Round318 block, and the second-copy needles (`kQupStatusErrorFlags`,
`kI2cStatusErrorMask`, `kQupNackFlag`, `kGtPidValidMax`, the tag encodings, the pin cuts, the per-segment
CONFIG expression) all grep to zero in the `.cpp`.

The new reading exists because of a measured blind spot. Round181's `bus=` column is
`I2C_STATUS & kI2cStatusErrorMask`, and MAIN's mask (`0x38000fc`) does **not** contain SDA `BIT(26)` or
SCL `BIT(27)`. The row prints `sda=`/`scl=` beside it, but those two come from a different cut, so the
masked column can never show a held bus. Round318 therefore prints, once per segment and on success as
well as failure:

```
clover r318: xfer %s class=%s status=%08x err=%08x vend=%08x res=%08x sda=%u scl=%u op=%08x t=%ums
clover r318: core %s class=%s flags=%08x err=%08x vend=%08x res=%08x iovr=%u ouvr=%u iur=%u oovr=%u
```

`status=` and `err=` are the raw words; `vend=`/`res=` and the four ERROR_FLAGS run bits are the layer's
decode of the same two reads; `class=` is the same verdict the failure branches and the retry policy are
keyed on. Both rows carry the new stamp and their own tee class `r318-input(32)` with a per-pass-cap
exemption, because the pair is one reading: a pass that kept the decoded row and ate the raw one would
put the blind spot back. Round181's rows are byte-identical, so boot 48's `verdict=102` stays comparable.

Carried by `v0.7.30` and later images only.

## What this driver does not claim

No power rails. The source's own `WHAT IT DOES NOT CLAIM` block states the whole position: both touch
nodes declare supplies (phandle `0x187` = `pm660l_l3`, fixed 3000000 µV; `0x17b` = `pm660_l11`,
1780000–1950000 µV), both are `qcom,rpm-smd-regulator` — held by RPM over SMD, not by an AP-side
register — and the earlier "PM8950 rails through SPMI" in that paragraph was a vendor-tree analogy and is withdrawn. What round313 changed
is the second half of that sentence: an RPM *binding* does not make the peripheral unaddressable, the
two rails are still SPMI PPIDs `0x242` and `0x04a`, and with the v3 floor plan in `drivers/CloverPower`
the enable byte at `0x46` is reachable pending one answer — which band this board's arbiter version word
really names (`notes/power-bringup.md` §8). Until then the report line still ends `rails unproven`
(the `no address ACKed` row in `clover_input_run()`), which is the honest shape: the electrical cause of
a NACK is neither proven nor excluded.

Every vote this driver makes is a bus clock, never a rail (`voteAhb()`, `programRcg()`,
`enableBranch()`, `programI2cPins()`), and `programI2cPins()` leaves the I2C pins' output-enable alone.
No HID, no `IOHIDDevice`, no registry publication happens here — that gap is named in the same
`WHAT IT DOES NOT CLAIM` block, and the HID work lives in `package/CloverHidInputProbe.cpp`. Every
report line carries `clover r181:`, first in `dumpClocks()` and last in `clover_input_run()`'s `done`
row — the house rule is that a new line takes the round token of the round that adds it, and
pre-existing lines keep their origin stamp. Its only courtesy to the transport is
`clover_emmc_flush_cache()` (declared against CloverSdhci and called at each of the four returns in
`clover_input_run()`).

## How this driver meets the UniXNU driver documentation

Layer, archive wiring and object ABI follow `docs/drivers/overview.md:11-16`,
`docs/drivers/xnu-drivers.md:118-124` (`build-arm-driver.py CloverInput`) and
`docs/drivers/xnu-drivers.md:106-116`, the last of which `scripts/build-arm-driver.py:120,129` actually
enforces. Its addresses are board constants exactly as `docs/drivers/xnu-drivers.md:215` prescribes for
a device below the `arm-io` range — this board's ADT publishes no node for the QUP, so there is no
provider to read `reg` from.

What it does not have is the class shape of `docs/drivers/xnu-drivers.md:19-64`, the personality of
`:147-168` and therefore the `Info.plist` that `:14` treats as that personality's record: it is
`extern "C" void clover_input_run(void)`, chained from `CloverPMGR` after the block
driver, and the reasons — with the three concrete prerequisites for changing that — are
`notes/unixnu-doc-conformance.md` §5. Against the worked example it is honest about its own gap:
`docs/drivers/sheng-novatek-touch.md:18-39` separates a pure core from a `struct nvt_bus`, and `:141-148`
makes "Run the existing host checks for the reused processing and clock helpers" rule 3. Round318 closes that gap:
the decode arithmetic now lives in `InputCore.h` with a host model the gate executes, and the MMIO that
remains in `CloverInput.cpp` reads words and hands them to the layer. The remaining §5 gap is the class
shape — `notes/unixnu-doc-conformance.md` §5 lists the three prerequisites for changing it.
