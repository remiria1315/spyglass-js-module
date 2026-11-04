const fs = require("fs");
const zlib = require("zlib");
const { parseArgs } = require("util");
const { Capstone, Const, loadCapstone } = require("capstone-wasm");
const {
  EXECUTE_FLAG,
  parsePE,
  getMemoryMappedImage,
  tokensOf,
  callTarget,
  countMatches,
  cppString,
  hex,
  die,
  loadFunctions,
  disassemblyOf,
} = require("./pe");

const REFERENCE_SEND = (
  "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 08 01 00 00 48 8D AC " +
  "24 80 00 00 00 48 C7 85 80 00 00 00 FE FF FF FF 45 89 CD 4C 89 C6 " +
  "48 89 D7 48 89 CB 48 8D 4D B0 41 B8 B0 00 00 00 E8 ? ? ? ? 48 8B 0D " +
  "? ? ? ? 48 8B 01 48 8B 40 08 BA B8 00 00 00 FF 15 ? ? ? ?"
).split(" ").filter((t) => t !== "");

const REFERENCE_FIXED = [];
REFERENCE_SEND.forEach((t, i) => {
  if (t !== "?") REFERENCE_FIXED.push([i, parseInt(t, 16)]);
});

const SEND_INTERNAL_WINDOW = 0x400;

// SLOT_FIVE_CALL = rb"\x41?\xff(?:[\x50-\x57]\x28|[\x90-\x97]\x28\x00\x00\x00|\x54\x24\x28)"
function slotFiveAt(image, i, end) {
  const candidates = image[i] === 0x41 ? [i + 1, i] : [i];
  for (const j of candidates) {
    if (j + 3 > end || image[j] !== 0xff) continue;
    const b = image[j + 1];
    if (b >= 0x50 && b <= 0x57 && image[j + 2] === 0x28) return true;
    if (
      b >= 0x90 && b <= 0x97 && j + 6 <= end &&
      image[j + 2] === 0x28 && image[j + 3] === 0 && image[j + 4] === 0 && image[j + 5] === 0
    ) {
      return true;
    }
    if (b === 0x54 && j + 4 <= end && image[j + 2] === 0x24 && image[j + 3] === 0x28) return true;
  }
  return false;
}

function hasSlotFiveCall(image, begin, end) {
  for (let i = begin; i < end; i++) {
    if (slotFiveAt(image, i, end)) return true;
  }
  return false;
}

function percent(x) {
  return `${Math.round(x * 100)}%`;
}

async function main() {
  const { values, positionals } = parseArgs({
    allowPositionals: true,
    options: {
      top: { type: "string", default: "3" },
      memory: { type: "boolean", default: false },
      disasm: { type: "string" },
      out: { type: "string", default: "send_disasm.txt" },
    },
  });
  if (positionals.length < 1) {
    console.error("usage: node find_send.js exe [--top N] [--memory] [--disasm RVA] [--out FILE]");
    process.exit(2);
  }
  const exe = positionals[0];
  const top = parseInt(values.top, 10);
  const disasmRva = values.disasm !== undefined ? Number(values.disasm) : null;

  const data = fs.readFileSync(exe);
  const pe = parsePE(data);
  const image = values.memory ? data : getMemoryMappedImage(data, pe);

  const { entries, functions } = loadFunctions(image, pe);
  const starts = new Set(functions.map(([begin]) => begin));

  await loadCapstone();
  const md = new Capstone(Const.CS_ARCH_X86, Const.CS_MODE_64);
  const disasm = (begin, end) => md.disasm(image.subarray(begin, end), { address: begin });

  if (disasmRva !== null) {
    const dumped = disassemblyOf(disasm, functions, disasmRva);
    if (!dumped) {
      die(`rva 0x${hex(disasmRva)} is not inside a function of the exception table`);
    }
    const { begin, text } = dumped;
    fs.writeFileSync(values.out, text);
    console.log(`wrote the disassembly of 0x${hex(begin)} to ${values.out}`);
    return;
  }

  const ranked = [];
  for (const [begin, end] of functions) {
    const size = end - begin;
    if (!(0x200 <= size && size <= 0x900)) continue;
    let same = 0;
    for (const [i, v] of REFERENCE_FIXED) {
      if (image[begin + i] === v) same++;
    }
    const similarity = same / REFERENCE_FIXED.length;
    const slotFive = hasSlotFiveCall(image, begin, end);
    const score = similarity * 10 + 2 * slotFive + (0x300 <= size && size <= 0x500 ? 1 : 0);
    ranked.push([score, similarity, slotFive, begin, end]);
  }
  ranked.sort((a, b) => {
    for (let i = 0; i < 5; i++) {
      if (a[i] !== b[i]) return Number(b[i]) - Number(a[i]);
    }
    return 0;
  });
  if (ranked.length === 0) {
    const parts = [];
    for (const s of pe.sections) {
      if (s.Characteristics & EXECUTE_FLAG) {
        const length = Math.min(s.Misc_VirtualSize, 1 << 20);
        parts.push(image.subarray(s.VirtualAddress, s.VirtualAddress + length));
      }
    }
    const sample = Buffer.concat(parts);
    const compressed = zlib.deflateSync(sample).length / Math.max(sample.length, 1);
    console.log("no function of a plausible size was found");
    console.log(`  exception table entries: ${entries.length}, inside the image: ${functions.length}`);
    console.log(`  executable sections compress to ${percent(compressed)} of their size`);
    if (compressed > 0.9 || functions.length < Math.floor(entries.length / 2)) {
      console.log("  the code in this file looks encrypted or packed");
      console.log("  dump the running game with tools/dump_client.py and run this again with --memory");
    }
    process.exit(1);
  }
  if (ranked[0][1] < 0.5) {
    console.log("warning: nothing resembles the 1.26.50 send closely, the ranking below rests on structure alone\n");
  }

  let shown = 0;
  for (const [score, similarity, slotFive, begin, end] of ranked.slice(0, 30)) {
    const insns = disasm(begin, end);
    const calls = [];
    insns.forEach((insn, i) => {
      const target = callTarget(insn);
      if (target !== null && starts.has(target) && target !== begin) calls.push(i);
    });
    if (calls.length === 0) continue;
    const last = calls[calls.length - 1];

    let tokens = [];
    for (const insn of insns) {
      tokens = tokens.concat(tokensOf(insn));
      if (tokens.length >= 32 && countMatches(image, tokens, 0, image.length) === 1) break;
    }
    const sendUnique = countMatches(image, tokens, 0, image.length) === 1;

    let callTokens = null;
    for (let k = 3; k < 9; k++) {
      let candidate = [];
      for (const insn of insns.slice(Math.max(0, last - k), last)) {
        candidate = candidate.concat(tokensOf(insn));
      }
      candidate.push("E8");
      if (countMatches(image, candidate, begin, begin + SEND_INTERNAL_WINDOW) === 1) {
        callTokens = candidate;
        break;
      }
    }

    const target = callTarget(insns[last]);
    const offset = Number(insns[last].address) - begin;
    console.log(`candidate ${shown + 1}: rva 0x${hex(begin)}, size 0x${hex(end - begin)}`);
    console.log(`  similarity to 1.26.50 send ${percent(similarity)}, call through vtable slot 5: ${slotFive ? "yes" : "no"}`);
    console.log(`  last call at +0x${hex(offset)} goes to rva 0x${hex(target)}`);
    if (offset >= SEND_INTERNAL_WINDOW) {
      console.log(`  that call lies past the first 0x${hex(SEND_INTERNAL_WINDOW)} bytes, so find_within cannot reach it`);
    }
    if (sendUnique) {
      console.log(cppString("send", tokens));
    } else {
      console.log("  no unique pattern for the start of this function");
    }
    if (callTokens) {
      console.log(cppString("send_internal_call", callTokens));
    } else {
      console.log(`  no unique send_internal_call pattern within the first 0x${hex(SEND_INTERNAL_WINDOW)} bytes`);
    }
    console.log();
    shown++;
    if (shown === top) break;
  }

  if (shown === 0) {
    die("no candidate ends in a call to another function");
  }
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
