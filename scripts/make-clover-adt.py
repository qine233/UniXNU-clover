#!/usr/bin/env python3
"""Apple Device Tree (ADT) generator for the Xiaomi Mi Pad 4 8" LTE (clover, SDM660).

Two public sources describe this board and they agree on every constant used here:

  * mainline   sdm660-mainline/linux  arch/arm64/boot/dts/qcom/sdm660-xiaomi-clover*.dts*
               (model "Xiaomi Mi Pad 4 LTE", compatible "xiaomi,clover-lte")
  * downstream  Xiaomi's CAF clover kernel (mirror E7G/clover-sdm660-ksb)
               arch/arm64/boot/dts/vendor/qcom/  (qcom,msm-id = <317 0x0>,
               cont_splash_mem = 0x9d500000 + 0x2400000)

The node set follows docs/porting/device-tree.md: /chosen (with random-seed, the NVRAM
proxy and the memory-map placeholders), /cpus, /psci, /arm-io with the GICv3 child,
/defaults and /options. Values only the unit itself knows are marked TODO-DEVICE and
come from facts/<serial>-<stamp>/ written by scripts/dump-clover-facts.sh.
"""
from pathlib import Path
import argparse
import os
import struct
import sys
import uuid
import zlib

# --- board constants -------------------------------------------------------
# SDM660 DRAM starts at 0x80000000 (gta4l's SM6115 uses 0x40000000).
DRAM_BASE = 0x80000000
# A 4 GiB Mi Pad 4 reports two banks: 0x80000000+2 GiB and 0x1800000000+2 GiB.
# TODO-DEVICE: confirm from dt/memory*.reg in the dump; the ADT describes the low
# bank only, which is the region the first bring-up needs.
DRAM_SIZE = 0x80000000
TIMEBASE_HZ = 19200000

# arm-io bus window. Child reg values are offsets inside it (device-tree-matching.md).
ARM_IO_BASE = 0x17000000
ARM_IO_SIZE = 0x01000000
GICD = (0x17A00000 - ARM_IO_BASE, 0x10000)
GICR = (0x17B00000 - ARM_IO_BASE, 0x100000)   # 8 redistributors, stride 0x20000

CLOVER_HARDWARE_SPEC = {
    "model": 'Xiaomi Mi Pad 4 8" LTE (2018)',
    "compatible": ["xiaomi,clover-lte", "qcom,sdm660"],
    "msm_id": [0x13D, 0x0],            # qcom,msm-id = <317 0x0>
    "board_id": [11, 0],               # qcom,board-id of sdm660-mtp-clover
                                       # TODO-DEVICE: the DTBO subtype of this unit
    "clock_frequency": TIMEBASE_HZ,
    "framebuffer": {
        # The stock ABL hands over cont_splash_mem; UniXNU draws in its head.
        "base": 0x9D500000,
        "window_size": 0x02400000,     # the whole cont_splash_mem region
        "size": 1200 * 1920 * 4,
        "width": 1200,
        "height": 1920,
        "stride": 1200 * 4,
        "format": "a8r8g8b8",
        # Portrait-native panel, Android runs it in landscape.
        # TODO-DEVICE: verify the handed-over orientation is 270 and not 90.
        "rotation": 270,
    },
    # Stock carve-outs, recorded so the UEFI memory map and check-clover.py can be reasoned
    # about. XNU learns its DRAM window from /chosen and the splash from display-base; these
    # entries are not read by the kernel.
    #
    # Note the collision the stock layout has with a UEFI firmware volume: ramoops at
    # 0x9fe00000 + 0x100000 ends where the 3 MiB firmware volume starting at 0x9fc00000
    # still lives, so under UniXNU there is no usable pstore region and the panic log goes to
    # the serial console instead. It is listed here because the dump script reads it off the
    # device and a future map change should not lose that fact.
    "reserved_memory": [
        {"name": "pil_adsp_mem",  "base": 0x92A00000, "size": 0x01E00000},
        {"name": "pil_mba_mem",   "base": 0x94900000, "size": 0x00200000},
        {"name": "pil_cdsp_mem",  "base": 0x94B00000, "size": 0x00600000},
        {"name": "pil_venus_mem", "base": 0x9CC00000, "size": 0x00800000},
        {"name": "framebuffer0",  "base": 0x9D500000, "size": 1200 * 1920 * 4},
        {"name": "ramoops",       "base": 0x9FE00000, "size": 0x00100000},
        {"name": "gpu_zap_mem",   "base": 0x0F0B00000, "size": 0x00002000},
    ],
    "uefi_fd": {"base": 0x9FC00000, "size": 0x300000},
    # Kryo 260: four Cortex-A53 (mpidr 0x0-0x3), four Kryo gold A73 (0x100-0x103).
    "fixed_frequency_khz": {"ecore": 1804800, "pcore": 2208000},
}


def die(message: str) -> None:
    print(f"[make-clover-adt] error: {message}", file=sys.stderr)
    raise SystemExit(2)


# --- Apple node encoding ---------------------------------------------------
def node(name, properties=(), children=()):
    properties = [("name", name.encode() + b"\0"), *properties]
    result = struct.pack("<II", len(properties), len(children))
    for key, data in properties:
        result += struct.pack("<32sI", key.encode(), len(data))
        result += data + bytes(-len(data) % 4)
    return result + b"".join(children)


def u32(val: int) -> bytes:
    return struct.pack("<I", val)


def u64(val: int) -> bytes:
    return struct.pack("<Q", val)


def cstr(val: str) -> bytes:
    return val.encode("utf-8") + b"\0"


def empty_nvram_proxy():
    """A volatile CHRP bank for IODTNVRAM; the board has no NVRAM controller."""
    def header(name, size):
        raw = bytearray(struct.pack("<BBH12s", 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b"nvram", 32)
    image[32:48] = header(b"common", 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b"free", len(image) - free_at)
    struct.pack_into("<I", image, 16, zlib.adler32(image[20:]))
    return [("nvram-bank-size", u32(len(image))),
            ("nvram-bank-count", u32(1)),
            ("nvram-current-bank", u32(0)),
            ("nvram-proxy-data", bytes(image))]


def build_cpu_node(cpu_idx: int, mpidr: int) -> bytes:
    spec = CLOVER_HARDWARE_SPEC
    silver = mpidr < 0x100
    freq = spec["fixed_frequency_khz"]["ecore"] if silver else spec["fixed_frequency_khz"]["pcore"]
    return node(f"cpu{cpu_idx}", [
        ("state", cstr("running" if cpu_idx == 0 else "waiting")),
        ("reg", u32(mpidr)),
        ("device_type", cstr("cpu")),
        # Kryo 260 is ARMv8.0 without pointer authentication, the same baseline as the
        # DEVELOPMENT_ARM64_BCM2837 kernel this board builds.
        ("compatible", cstr("arm,armv8")),
        ("cpu-id", u32(cpu_idx)),
        ("cluster-type", cstr("ECore" if silver else "PCore")),
        ("timebase-frequency", u32(spec["clock_frequency"])),
        ("fixed-frequency-khz", u32(freq)),
    ])


def generate_adt(args) -> bytes:
    spec = CLOVER_HARDWARE_SPEC
    cpu_regs = [int(x.strip(), 0) for x in args.cpu_regs.split(",") if x.strip()]
    if not cpu_regs:
        die("no cpu reg values")
    if args.dram_size % 0x200000 or not 0x10000000 <= args.dram_size <= 0x100000000:
        die("DRAM size must be 256 MiB to 4 GiB, aligned to 2 MiB")

    root_matching = []
    if args.root_match_volume_uuid:
        xml = ('<dict><key>IOProviderClass</key><string>IOMedia</string>'
               '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' +
               str(args.root_match_volume_uuid).upper() + '</string></dict></dict>\0')
        root_matching = [("root-matching", xml.encode())]

    writable_root = args.data_ramdisk or args.packed_data_ramdisk
    memory_map_props = []
    if args.ramdisk or writable_root:
        memory_map_props.append(("RAMDisk", bytes(16)))
    if args.packed_data_ramdisk:
        memory_map_props.append(("PackedRAMDisk", bytes(16)))
    if args.trust_cache or getattr(args, "owner_trust_cache", False):
        memory_map_props.append(("TrustCache", bytes(16)))

    chosen = node("chosen", [
        ("random-seed", os.urandom(64)),
        *([("ephemeral-storage", u32(1))] if writable_root else []),
        ("osenvironment", cstr("ramdisk" if args.data_ramdisk else "main")),
        ("darwinboot-memory-size", u64(args.owned_memory_size)),
        *root_matching,
        ("dram-base", u64(args.dram_base)),
        ("dram-size", u64(args.dram_size)),
        ("crypto-hash-method", cstr("sha2-384")),
        ("unique-chip-id", u64(args.unique_chip_id)),
        ("chip-id", u32(int(args.chip_id, 0))),
        *empty_nvram_proxy(),
        # Display: the splash window the stock ABL leaves powered.
        ("display-base", u64(spec["framebuffer"]["base"])),
        ("display-width", u32(spec["framebuffer"]["width"])),
        ("display-height", u32(spec["framebuffer"]["height"])),
        ("display-stride", u32(spec["framebuffer"]["stride"])),
        ("display-rotation", u32(spec["framebuffer"]["rotation"])),
    ], [node("memory-map", memory_map_props), node("manifest-properties")])

    cpus = node("cpus", [("#address-cells", u32(1)), ("#size-cells", u32(0))],
                [build_cpu_node(i, reg) for i, reg in enumerate(cpu_regs)])
    psci = node("psci", [("compatible", cstr("arm,psci-1.0")), ("method", cstr(args.psci_method))])

    # sleep-disabled: suspend/resume is not implemented on this port.
    defaults = node("defaults", [("sleep-disabled", u32(1))])
    options = node("options")
    product = node("product", [("product-name", cstr("Mi Pad 4 LTE")), ("model", cstr("iPad7,1"))])

    arm_io = node("arm-io", [
        ("device_type", cstr("arm-io")),
        ("#address-cells", u32(2)),
        ("#size-cells", u32(2)),
        ("ranges", u64(0) + u64(ARM_IO_BASE) + u64(ARM_IO_SIZE)),
    ], [node("intc", [("compatible", cstr("arm,gic-v3")),
                      ("reg", u64(GICD[0]) + u64(GICD[1]) + u64(GICR[0]) + u64(GICR[1]))])])

    # The iPad identity is what the restore image was built for; the SoC facts travel
    # in /chosen, /arm-io and the CPU nodes.
    return node("device-tree", [
        ("model", cstr("iPad7,1")),
        ("compatible", cstr("J120AP")),
        ("board-id", u32(12)),
        ("#address-cells", u32(2)),
        ("#size-cells", u32(2)),
    ], [chosen, cpus, psci, defaults, options, product, arm_io])


def main() -> int:
    parser = argparse.ArgumentParser(description="Emit the clover ADT for UniXNU")
    parser.add_argument("--output", "-o", type=Path, required=True)
    parser.add_argument("--dram-base", type=lambda v: int(v, 0), default=DRAM_BASE)
    parser.add_argument("--dram-size", type=lambda v: int(v, 0), default=DRAM_SIZE)
    parser.add_argument("--owned-memory-size", type=lambda v: int(v, 0), default=0x80000000)
    parser.add_argument("--root-match-volume-uuid", type=uuid.UUID, default=None)
    parser.add_argument("--cpu-regs", default="0x0,0x1,0x2,0x3,0x100,0x101,0x102,0x103")
    parser.add_argument("--psci-method", choices=["smc", "hvc"], default="smc")
    parser.add_argument("--unique-chip-id", type=lambda v: int(v, 0), default=None)
    parser.add_argument("--chip-id", default="0x13d")
    parser.add_argument("--trust-cache", action="store_true")
    # An owner-signed bundle replaces --trust-cache in the payload step, but the ADT still
    # has to reserve the TrustCache placeholder, because DarwinBoot refuses an input it has
    # nowhere to write.
    parser.add_argument("--owner-trust-cache", action="store_true")
    parser.add_argument("--data-ramdisk", action="store_true")
    parser.add_argument("--packed-data-ramdisk", action="store_true")
    parser.add_argument("--ramdisk", action="store_true")
    parser.add_argument("--ipad-identity", action="store_true")
    args, unknown = parser.parse_known_args()
    if unknown:
        print(f"[make-clover-adt] ignoring unsupported options: {' '.join(unknown)}", file=sys.stderr)

    # board.toml declares unique_chip_id = "required": the platform seed is a device
    # input and lives in no repository. A missing seed must not write an ADT that
    # silently gets IOPlatformUUID wrong.
    if args.unique_chip_id is None:
        die("--unique-chip-id is required (the platform seed is a device input)")
    # The contiguous DarwinBoot window the kernel owns cannot be larger than the DRAM
    # the ADT describes.
    if args.owned_memory_size > args.dram_size:
        die(f"owned memory 0x{args.owned_memory_size:x} exceeds DRAM 0x{args.dram_size:x}")

    blob = generate_adt(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    print(f"[make-clover-adt] wrote {len(blob)} bytes to {args.output} "
          f"(dram 0x{args.dram_base:x} + 0x{args.dram_size:x}, display 0x{CLOVER_HARDWARE_SPEC['framebuffer']['base']:x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
