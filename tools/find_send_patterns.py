import argparse
import itertools
import re
import struct
import sys

import capstone
import pefile
from capstone import x86

REFERENCE_SEND = (
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 08 01 00 00 48 8D AC "
    "24 80 00 00 00 48 C7 85 80 00 00 00 FE FF FF FF 45 89 CD 4C 89 C6 "
    "48 89 D7 48 89 CB 48 8D 4D B0 41 B8 B0 00 00 00 E8 ? ? ? ? 48 8B 0D "
    "? ? ? ? 48 8B 01 48 8B 40 08 BA B8 00 00 00 FF 15 ? ? ? ?"
).split()

REFERENCE_FIXED = [(i, int(t, 16)) for i, t in enumerate(REFERENCE_SEND) if t != "?"]
SLOT_FIVE_CALL = re.compile(rb"\x41?\xff(?:[\x50-\x57]\x28|[\x90-\x97]\x28\x00\x00\x00|\x54\x24\x28)")
SEND_INTERNAL_WINDOW = 0x400
EXECUTE_FLAG = 0x20000000
EXCEPTION_DIRECTORY = 3


def tokens_of(insn):
    masked = set()
    for op in insn.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
            masked.update(range(insn.disp_offset, insn.disp_offset + insn.disp_size))
    if insn.group(x86.X86_GRP_BRANCH_RELATIVE):
        masked.update(range(insn.imm_offset, insn.imm_offset + insn.imm_size))
    return ["?" if i in masked else f"{b:02X}" for i, b in enumerate(insn.bytes)]


def count_matches(image, tokens, start, end):
    body = re.compile(
        b"".join(b"." if t == "?" else re.escape(bytes([int(t, 16)])) for t in tokens),
        re.DOTALL,
    )
    literal = bytes(int(t, 16) for t in itertools.takewhile(lambda t: t != "?", tokens))
    count = 0
    position = image.find(literal, start, end)
    while position != -1:
        if body.match(image, position, end):
            count += 1
        position = image.find(literal, position + 1, end)
    return count


def cpp_string(field, tokens):
    lines = [" ".join(tokens[i:i + 22]) for i in range(0, len(tokens), 22)]
    quoted = [f'"{line} "' for line in lines[:-1]] + [f'"{lines[-1]}"']
    return f"    .{field} = " + f"\n{' ' * (len(field) + 8)}".join(quoted) + ","


def main():
    parser = argparse.ArgumentParser(
        description="Find NetworkSystem::send and its _sendInternal call in Minecraft.Windows.exe"
    )
    parser.add_argument("exe")
    parser.add_argument("--top", type=int, default=3)
    args = parser.parse_args()

    pe = pefile.PE(args.exe, fast_load=True)
    image = pe.get_memory_mapped_image()
    directory = pe.OPTIONAL_HEADER.DATA_DIRECTORY[EXCEPTION_DIRECTORY]
    if directory.Size == 0:
        sys.exit("the file has no exception table, so function boundaries are unknown")
    functions = [
        (begin, end)
        for begin, end, _ in struct.iter_unpack(
            "<III", image[directory.VirtualAddress:directory.VirtualAddress + directory.Size]
        )
        if begin != 0
    ]
    starts = {begin for begin, _ in functions}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    ranked = []
    for begin, end in functions:
        size = end - begin
        if not 0x200 <= size <= 0x900:
            continue
        similarity = sum(image[begin + i] == v for i, v in REFERENCE_FIXED) / len(REFERENCE_FIXED)
        slot_five = SLOT_FIVE_CALL.search(image, begin, end) is not None
        score = similarity * 10 + 2 * slot_five + (0x300 <= size <= 0x500)
        ranked.append((score, similarity, slot_five, begin, end))
    ranked.sort(reverse=True)
    if not ranked:
        sys.exit("no function of a plausible size was found")
    if ranked[0][1] < 0.5:
        print("warning: nothing resembles the 1.26.50 send closely, the ranking below rests on structure alone\n")

    shown = 0
    for score, similarity, slot_five, begin, end in ranked[:30]:
        insns = list(md.disasm(image[begin:end], begin))
        calls = [
            i
            for i, insn in enumerate(insns)
            if insn.id == x86.X86_INS_CALL
            and insn.operands[0].type == x86.X86_OP_IMM
            and insn.operands[0].imm in starts
            and insn.operands[0].imm != begin
        ]
        if not calls:
            continue
        last = calls[-1]

        tokens = []
        for insn in insns:
            tokens += tokens_of(insn)
            if len(tokens) >= 32 and count_matches(image, tokens, 0, len(image)) == 1:
                break
        send_unique = count_matches(image, tokens, 0, len(image)) == 1

        call_tokens = None
        for k in range(3, 9):
            candidate = [t for insn in insns[last - k:last] for t in tokens_of(insn)] + ["E8"]
            if count_matches(image, candidate, begin, begin + SEND_INTERNAL_WINDOW) == 1:
                call_tokens = candidate
                break

        target = insns[last].operands[0].imm
        offset = insns[last].address - begin
        print(f"candidate {shown + 1}: rva 0x{begin:x}, size 0x{end - begin:x}")
        print(f"  similarity to 1.26.50 send {similarity:.0%}, call through vtable slot 5: {'yes' if slot_five else 'no'}")
        print(f"  last call at +0x{offset:x} goes to rva 0x{target:x}")
        if offset >= SEND_INTERNAL_WINDOW:
            print(f"  that call lies past the first 0x{SEND_INTERNAL_WINDOW:x} bytes, so find_within cannot reach it")
        if send_unique:
            print(cpp_string("send", tokens))
        else:
            print("  no unique pattern for the start of this function")
        if call_tokens:
            print(cpp_string("send_internal_call", call_tokens))
        else:
            print("  no unique send_internal_call pattern within the first 0x400 bytes")
        print()
        shown += 1
        if shown == args.top:
            break

    if shown == 0:
        sys.exit("no candidate ends in a call to another function")


if __name__ == "__main__":
    main()
