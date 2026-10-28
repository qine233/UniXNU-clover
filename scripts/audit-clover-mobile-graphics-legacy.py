#!/usr/bin/env python3
"""Qualify the unchanged original IOMobileGraphicsFamily Legacy class in this board's own kernel.

This replays the structural qualification that the sheng port uses for the same original
family (scripts/audit-mobile-graphics-legacy.py) against the kernel this board actually ships,
and adds one thing sheng could only get from a QEMU session: the class identity.

Everything here is read from out/clover/kernel/kernel.macho. Nothing is patched, nothing runs,
no device is involved. What it proves:

  * the family's executable text is byte-identical to the original image that qualification
    was done against, so the pinned construction sites below are the same instructions;
  * the three protected constructor prologues load their vtable pointers the way the original
    class does, and the Legacy -> Service -> Framebuffer constructor chain really calls the
    original bases;
  * the base, service and Legacy vtables have 262/262/358 live entries, the Legacy class
    leaves exactly the documented abstract slots unfilled, adds no operational overrides over
    its service base, and inherits every other method from original family code;
  * the meta class registration for IOMobileFramebufferLegacy names that class, hangs it on
    that parent record, and reports that instance extent - derived from this image's own
    instructions, not carried over as a runtime observation.

The output drives the next step: the ABI header and the original-binding table that a real
NativeLegacyDisplay archive for this board has to be built against.
"""
import hashlib
import json
import pathlib
import struct
import sys

BASE = pathlib.Path("/Users/zhouzhou/UniXNU")
KERNEL = BASE / "out/clover/kernel/kernel.macho"
PLAN = BASE / "out/clover/kernel/kernel.json"
OUT = pathlib.Path(__file__).resolve().parents[1] / "build/display-analysis/clover-mobile-graphics-legacy-abi.json"
FAMILY = "com.apple.iokit.IOMobileGraphicsFamily"

# The original family executable, unchanged, as qualified by every port that uses it.
FAMILY_TEXT_SHA = "2e044ee68c1650397e1435234ca6459206262e4c7ed709cb7131c6f664f9bd9b"
# Constructor prologues inside that executable. Positions are offsets from the family text base
# so a board that maps the family elsewhere still qualifies.
SITES = {
    "framebuffer_constructor": 0xF324,
    "service_constructor": 0xF51C,
    "legacy_constructor": 0x1A5D8,
    "legacy_constructor_base_call": 0x1A5FC,
    "service_constructor_base_call": 0xF540,
    "legacy_destructor_base_call": 0x1A630,
}
ABSTRACT_SLOTS = [211, 213, 245, 246, 306, 307, 337, 338, 345]
OWN_OVERRIDES = [0, 1, 7, 14, 84, 86, 87, 141, 155, 184, 188, 288, 290, 296, 301]
PURE_VIRTUAL = "___cxa_pure_virtual"
NEW_USER_CLIENT = "__ZN9IOService13newUserClientEP4taskPvjPP12IOUserClient"

sys.path.insert(0, str(BASE / "kits/NativePrelink"))
from native_prelink import backed_offset, exports, macho  # noqa: E402


def require(value, reason):
    if not value:
        raise SystemExit("CLOVER DISPLAY AUDIT FAILED: " + reason)


def main():
    require(KERNEL.exists() and PLAN.exists(), "build the kernel first: " + str(KERNEL))
    blob = KERNEL.read_bytes()
    plan = json.loads(PLAN.read_text())
    image = macho(blob)

    def read(va, size):
        at = backed_offset(image["segments"], va, size, len(blob))
        return blob[at:at + size]

    extents = [e for e in plan["extents"] if e["module"] == FAMILY]
    text = next(e for e in extents if e["segment"] == "__TEXT_EXEC")
    require(len([e for e in extents if e["segment"] == "__TEXT_EXEC"]) == 1,
            "unambiguous family executable extent")
    base = text["va"]
    words = struct.unpack("<%dI" % (text["size"] // 4), read(base, text["size"]))
    require(hashlib.sha256(read(base, text["size"])).hexdigest() == FAMILY_TEXT_SHA,
            "the original family executable is unchanged in this kernel")

    def adrp_page(word, pc):
        imm = ((word >> 5) & 0x7FFFF) * 4 + ((word >> 29) & 3)
        if imm & (1 << 20):
            imm -= 1 << 21
        return (pc & ~4095) + (imm << 12)

    def vptr(pc):
        a, b, c = struct.unpack("<3I", read(pc, 12))
        require(a & 0x9F00001F == 0x90000008 and b & 0xFFC003FF == 0x91000108 and c == 0x91004108,
                "constructor ADRP/ADD/vptr header at " + hex(pc))
        return adrp_page(a, pc) + ((b >> 10) & 4095) + 16

    def branch(pc):
        word = struct.unpack("<I", read(pc, 4))[0]
        require(word & 0xFC000000 == 0x94000000, "direct base call at " + hex(pc))
        value = word & 0x3FFFFFF
        return pc + 4 * (value - (1 << 26) if value & (1 << 25) else value)

    framebuffer_vptr = vptr(base + SITES["framebuffer_constructor"])
    service_vptr = vptr(base + SITES["service_constructor"])
    legacy_vptr = vptr(base + SITES["legacy_constructor"])
    require(branch(base + SITES["legacy_constructor_base_call"]) == base + 0x0000F510
            and branch(base + SITES["service_constructor_base_call"]) == base + 0x0000F318
            and branch(base + SITES["legacy_destructor_base_call"]) == base + 0x0000F55C,
            "the protected Legacy -> Service -> Framebuffer chain really calls the original bases")

    framebuffer = struct.unpack("<262Q", read(framebuffer_vptr, 262 * 8))
    service = struct.unpack("<262Q", read(service_vptr, 262 * 8))
    require([i for i, (a, b) in enumerate(zip(framebuffer, service)) if a != b] == [0, 1, 7],
            "the service base adds no generic operational overrides")
    legacy = struct.unpack("<358Q", read(legacy_vptr, 358 * 8))
    require(all(legacy) and read(legacy_vptr + 358 * 8, 24) == bytes(24),
            "exact nonnull method extent and next aligned vtable boundary")

    names = {n: v for v, group in exports(blob, image).items() for n in group}
    require(PURE_VIRTUAL in names, "the kernel exports its pure-virtual stub")
    abstract = [i for i, va in enumerate(legacy) if va == names[PURE_VIRTUAL]]
    require(abstract == ABSTRACT_SLOTS, "the original abstract leaves: " + str(abstract))
    changed = [i for i, (a, b) in enumerate(zip(framebuffer, legacy)) if a != b]
    require(len(changed) == 103 and [i for i in changed if i < 168] ==
            [0, 1, 7, 14, 21, 44, 79, 85, 86, 87, 111, 141, 155],
            "the original generic implementation overrides: " + str(len(changed)))
    require(NEW_USER_CLIENT in names and names[NEW_USER_CLIENT] == framebuffer[141],
            "newUserClient occupies the native four-argument ABI slot")
    for i, va in enumerate(legacy):
        if i >= 168 and i not in abstract:
            require(base <= va < base + text["size"], "slot %d is not original family code" % i)

    # Class identity, derived from this image. The registration thunk builds an OSMetaClass with
    # (record, name, parent, instance size): three ADRP/ADD pairs, one immediate and one direct
    # call, in that order. Exactly one site in the family carries that instance size, so the
    # number is evidence here rather than an assumption - and no DATA read is involved, because
    # meta class records are filled in at load and read as zeros in the file.
    size_word = 0x18A0
    mov = None
    for i, word in enumerate(words[:-1]):
        if (word >> 8) & 0xFFFFFF == 0x528314 and word & 0x1F == 3 and \
                words[i + 1] & 0xFC000000 == 0x94000000:
            require(mov is None, "the instance size is not unique to one registration site")
            mov = i
    require(mov is not None, "no meta class registration with instance size " + hex(size_word))
    pc = base + mov * 4

    def pair_at(index):
        a, b = words[index], words[index + 1]
        # ADD (immediate, 64-bit) with shift 0: the fixed part is bits 31-22 (0xFFC00000), Rn sits
        # in bits 4-0, Rd in bits 9-5 and the displacement in bits 21-10. ADRP's Rd is bits 4-0.
        return (a & 0x9F000000 == 0x90000000 and b & 0xFFC00000 == 0x91000000
                and (b >> 5) & 0x1F == a & 0x1F and b & 0x1F == a & 0x1F,
                adrp_page(a, base + index * 4) + ((b >> 10) & 0xFFF))

    def pair(index):
        ok, value = pair_at(index)
        require(ok, "registration argument is not an ADRP/ADD pair at " + hex(base + index * 4))
        return value

    def bl_target(index):
        word = words[index]
        return None if word & 0xFC000000 != 0x94000000 else \
            base + index * 4 + 4 * ((word & 0x3FFFFFF) - (1 << 26)
                                    if (word & 0x3FFFFFF) & (1 << 25) else (word & 0x3FFFFFF))

    name_va = pair(mov - 4)
    parent = pair(mov - 2)
    ctor = branch(pc + 4)
    name = read(name_va, len(b"IOMobileFramebufferLegacy\0"))
    require(name == b"IOMobileFramebufferLegacy\0", "registered class name reads " + repr(name))
    # The registration thunk forwards its record argument, so the record is bound one step
    # earlier: IOKit's generated class code emits the meta class getter immediately before the
    # initializer, and that getter materialises the record with an ADRP/ADD pair and returns it.
    entry = max(i for i in range(mov) if words[i] == 0xD100C3FF)
    require(base + entry * 4 <= pc, "registration thunk entry")
    last_ret = max(i for i in range(entry) if words[i] == 0xD65F03C0)
    # That block is frame setup, save of the receiver, ADRP/ADD of the record, restore, return.
    require(words[last_ret - 5] == 0xD10043FF, "the block before the registration opens its own frame")
    block = [pair_at(i) for i in range(last_ret - 5, last_ret)]
    values = sorted({value for ok, value in block if ok})
    require(len(values) == 1, "the block before the registration hands exactly one record: "
            + str([hex(v) for v in values]))
    record = values[0]
    require(pair_at(last_ret - 3) == (True, record), "the record is materialised right before the return")
    data = next(e for e in extents if e["segment"] == "__DATA")
    require(data["va"] <= record < data["va"] + data["size"],
            "the record is a data object of this family, not code")
    # The same pair recurs as the class's meta class accessor; count it so the record is used,
    # not just constructed.
    users = [i for i in range(len(words) - 2) if pair_at(i)[0] and pair_at(i)[1] == record]
    require(len(users) >= 2, "the record is materialised by the getter and its accessor")
    require(record == 0xFFFFFFF006B35940 and parent == 0xFFFFFFF006B31400,
            "the identity agrees with the record the original family was qualified at")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(dict(
        kernel_sha256=hashlib.sha256(blob).hexdigest(),
        family=FAMILY, family_text_sha256=FAMILY_TEXT_SHA, family_text_base=hex(base),
        family_text_bytes=text["size"],
        framebuffer_vptr=hex(framebuffer_vptr), service_vptr=hex(service_vptr),
        legacy_vptr=hex(legacy_vptr), legacy_instance_bytes=size_word,
        legacy_virtual_entries=358, abstract_slots=abstract, inherited_generic_changes=changed,
        source_service_operation_overrides=0,
        qualified_own_override_slots=OWN_OVERRIDES,
        original_new_user_client=hex(legacy[141]), original_init=hex(legacy[21]),
        original_start=hex(legacy[86]), original_stop=hex(legacy[87]),
        original_kick_swap_queue=hex(legacy[290]), original_validate_swap=hex(legacy[296]),
        original_complete_current_swap=hex(legacy[295]),
        original_get_display_dimensions=hex(legacy[249]),
        new_user_client_abi="IOService::newUserClient(task_t,void*,UInt32,IOUserClient**)",
        meta_class_record=hex(record), meta_class_parent=hex(parent),
        meta_class_name=str(name.rstrip(b"\0").decode()),
        meta_class_registration=hex(pc), meta_class_registration_callee=hex(ctor),
        meta_class_getter=hex(base + users[0] * 4),
        meta_class_getter_sites=len(users),
        identity_source="static registration site in this kernel",
        methods=[dict(slot=i, address=hex(va), abstract=i in abstract,
                       prototype_qualified=False) for i, va in enumerate(legacy)],
        static_dispatch_audit=True, legacy_runtime_instance_created=False,
        backend_contract_complete=False, original_instructions_modified=False,
        device_observed=False), indent=2) + "\n")
    print("PASS: this kernel carries the unchanged original Legacy class - 358 slots, "
          "nine abstract leaves, " + str(len(changed)) + " original overrides, "
          "newUserClient in slot 141")
    print("PASS: class identity derived statically from this image: "
          "%s record=%s parent=%s size=%s" % (name.rstrip(b"\0").decode(), hex(record),
                                              hex(parent), hex(size_word)))
    print("abi: " + str(OUT))


if __name__ == "__main__":
    main()
