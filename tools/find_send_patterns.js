const fs = require("fs");
const zlib = require("zlib");
const { parseArgs } = require("util");
const { Capstone, Const, loadCapstone } = require("capstone-wasm");

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
const EXECUTE_FLAG = 0x20000000;
const EXCEPTION_DIRECTORY = 3;

// pefile の代わり。PE32+ / PE32 のヘッダーとセクション表だけ読む
function parsePE(data) {
  if (data.length < 0x40 || data.readUInt16LE(0) !== 0x5a4d) {
    throw new Error("not a PE file (no MZ header)");
  }
  const peOffset = data.readUInt32LE(0x3c);
  if (data.readUInt32LE(peOffset) !== 0x00004550) {
    throw new Error("not a PE file (no PE signature)");
  }
  const coff = peOffset + 4;
  const numberOfSections = data.readUInt16LE(coff + 2);
  const sizeOfOptionalHeader = data.readUInt16LE(coff + 16);
  const opt = coff + 20;
  const isPE32Plus = data.readUInt16LE(opt) === 0x20b;
  const sizeOfImage = data.readUInt32LE(opt + 56);
  const sizeOfHeaders = data.readUInt32LE(opt + 60);
  const directoryOffset = opt + (isPE32Plus ? 112 : 96);
  const directoryCount = data.readUInt32LE(directoryOffset - 4);

  const directories = [];
  for (let i = 0; i < Math.min(directoryCount, 16); i++) {
    directories.push({
      VirtualAddress: data.readUInt32LE(directoryOffset + i * 8),
      Size: data.readUInt32LE(directoryOffset + i * 8 + 4),
    });
  }

  const sections = [];
  const tableOffset = opt + sizeOfOptionalHeader;
  for (let i = 0; i < numberOfSections; i++) {
    const o = tableOffset + i * 40;
    sections.push({
      Misc_VirtualSize: data.readUInt32LE(o + 8),
      VirtualAddress: data.readUInt32LE(o + 12),
      SizeOfRawData: data.readUInt32LE(o + 16),
      PointerToRawData: data.readUInt32LE(o + 20),
      Characteristics: data.readUInt32LE(o + 36),
    });
  }
  return { sizeOfImage, sizeOfHeaders, directories, sections };
}

// pe.get_memory_mapped_image() 相当
function getMemoryMappedImage(data, pe) {
  const image = Buffer.alloc(pe.sizeOfImage);
  data.copy(image, 0, 0, Math.min(pe.sizeOfHeaders, data.length, image.length));
  for (const s of pe.sections) {
    if (s.PointerToRawData >= data.length || s.VirtualAddress >= image.length) continue;
    const length = Math.min(
      s.SizeOfRawData,
      data.length - s.PointerToRawData,
      image.length - s.VirtualAddress,
    );
    data.copy(image, s.VirtualAddress, s.PointerToRawData, s.PointerToRawData + length);
  }
  return image;
}

const PREFIXES = new Set([0x66, 0x67, 0xf0, 0xf2, 0xf3, 0x2e, 0x36, 0x3e, 0x26, 0x64, 0x65]);

// capstone の detail が使えないので、相対分岐の rel のバイト数をオペコードから判定する
// (X86_GRP_BRANCH_RELATIVE の imm_size 相当。0なら相対分岐ではない)
function relativeBranchSize(bytes) {
  let i = 0;
  while (i < bytes.length && PREFIXES.has(bytes[i])) i++;
  if (i < bytes.length && (bytes[i] & 0xf0) === 0x40) i++;
  const op = bytes[i];
  if (op === 0xe8 || op === 0xe9) return 4;
  if (op === 0xeb || (op >= 0x70 && op <= 0x7f) || (op >= 0xe0 && op <= 0xe3)) return 1;
  if (op === 0x0f && (bytes[i + 1] & 0xf0) === 0x80) return 4;
  return 0;
}

// [rip + disp] の disp32 の位置を探す
// op_str から disp の値を取り、modrm(mod=00, rm=101)の直後にある同じ4バイトを探す
function ripDispOffset(insn) {
  const m = /\[rip(?:\s*([+-])\s*0x([0-9a-f]+))?\]/.exec(insn.opStr);
  if (!m) return -1;
  let disp = m[1] ? parseInt(m[2], 16) : 0;
  if (m[1] === "-") disp = -disp;
  const encoded = Buffer.alloc(4);
  encoded.writeInt32LE(disp | 0);
  const bytes = insn.bytes;
  for (let i = 1; i + 4 <= bytes.length; i++) {
    if (
      (bytes[i - 1] & 0xc7) === 0x05 &&
      bytes[i] === encoded[0] &&
      bytes[i + 1] === encoded[1] &&
      bytes[i + 2] === encoded[2] &&
      bytes[i + 3] === encoded[3]
    ) {
      return i;
    }
  }
  return bytes.length - 4;
}

function tokensOf(insn) {
  const masked = new Set();
  const dispOffset = ripDispOffset(insn);
  if (dispOffset >= 0) {
    for (let i = dispOffset; i < dispOffset + 4; i++) masked.add(i);
  }
  const immSize = relativeBranchSize(insn.bytes);
  for (let i = insn.bytes.length - immSize; i < insn.bytes.length && immSize > 0; i++) {
    masked.add(i);
  }
  return Array.from(insn.bytes, (b, i) =>
    masked.has(i) ? "?" : b.toString(16).toUpperCase().padStart(2, "0"),
  );
}

// call 0x1234 のような直接callの宛先。直接callでなければ null
function callTarget(insn) {
  if (insn.mnemonic !== "call") return null;
  const m = /^0x([0-9a-f]+)$/.exec(insn.opStr.trim());
  return m ? parseInt(m[1], 16) : null;
}

function find(image, literal, start, end) {
  if (start > end) return -1;
  if (literal.length === 0) return start;
  const position = image.indexOf(literal, start);
  if (position === -1 || position + literal.length > end) return -1;
  return position;
}

function countMatches(image, tokens, start, end) {
  const values = tokens.map((t) => (t === "?" ? -1 : parseInt(t, 16)));
  let literalLength = 0;
  while (literalLength < values.length && values[literalLength] !== -1) literalLength++;
  const literal = Buffer.from(values.slice(0, literalLength));
  let count = 0;
  let position = find(image, literal, start, end);
  while (position !== -1) {
    if (position + values.length <= end) {
      let ok = true;
      for (let i = literalLength; i < values.length; i++) {
        if (values[i] !== -1 && image[position + i] !== values[i]) {
          ok = false;
          break;
        }
      }
      if (ok) count++;
    }
    position = find(image, literal, position + 1, end);
  }
  return count;
}

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

function cppString(field, tokens) {
  const lines = [];
  for (let i = 0; i < tokens.length; i += 22) lines.push(tokens.slice(i, i + 22).join(" "));
  const quoted = lines.slice(0, -1).map((line) => `"${line} "`).concat([`"${lines[lines.length - 1]}"`]);
  return `    .${field} = ` + quoted.join(`\n${" ".repeat(field.length + 8)}`) + ",";
}

function percent(x) {
  return `${Math.round(x * 100)}%`;
}

function hex(n, width = 0) {
  return n.toString(16).padStart(width, "0");
}

function die(message) {
  console.error(message);
  process.exit(1);
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

  const directory = pe.directories[EXCEPTION_DIRECTORY];
  if (!directory || directory.Size === 0) {
    die("the file has no exception table, so function boundaries are unknown");
  }
  const entries = [];
  const tableEnd = directory.VirtualAddress + directory.Size - 11;
  for (let o = directory.VirtualAddress; o < tableEnd && o + 12 <= image.length; o += 12) {
    entries.push([image.readUInt32LE(o), image.readUInt32LE(o + 4)]);
  }
  const functions = entries.filter(([begin, end]) => 0 < begin && begin < end && end <= image.length);
  const starts = new Set(functions.map(([begin]) => begin));

  await loadCapstone();
  const md = new Capstone(Const.CS_ARCH_X86, Const.CS_MODE_64);
  const disasm = (begin, end) => md.disasm(image.subarray(begin, end), { address: begin });

  if (disasmRva !== null) {
    const found = functions.find(([b, e]) => b <= disasmRva && disasmRva < e);
    if (!found) {
      die(`rva 0x${hex(disasmRva)} is not inside a function of the exception table`);
    }
    const [begin, end] = found;
    let text = `function rva 0x${hex(begin)}, size 0x${hex(end - begin)}\n`;
    for (const insn of disasm(begin, end)) {
      const address = Number(insn.address);
      const bytes = Array.from(insn.bytes, (b) => hex(b, 2)).join(" ");
      text += `+0x${hex(address - begin, 4)}  ${hex(address, 8)}  ${bytes.padEnd(32)} ${insn.mnemonic} ${insn.opStr}\n`;
    }
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
