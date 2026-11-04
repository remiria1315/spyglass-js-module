const EXECUTE_FLAG = 0x20000000;
const EXCEPTION_DIRECTORY = 3;
const LEAF_WINDOW = 0x80;

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
  const imageBase = isPE32Plus ? data.readBigUInt64LE(opt + 24) : BigInt(data.readUInt32LE(opt + 28));
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
  return { imageBase, sizeOfImage, sizeOfHeaders, directories, sections };
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

function cppString(field, tokens) {
  const lines = [];
  for (let i = 0; i < tokens.length; i += 22) lines.push(tokens.slice(i, i + 22).join(" "));
  const quoted = lines.slice(0, -1).map((line) => `"${line} "`).concat([`"${lines[lines.length - 1]}"`]);
  return `    .${field} = ` + quoted.join(`\n${" ".repeat(field.length + 8)}`) + ",";
}

function hex(n, width = 0) {
  return n.toString(16).padStart(width, "0");
}

function die(message) {
  console.error(message);
  process.exit(1);
}

function loadFunctions(image, pe) {
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
  return { entries, functions };
}

function disassemblyOf(disasm, functions, rva) {
  const found = functions.find(([b, e]) => b <= rva && rva < e);
  const [begin, end] = found ?? [rva, rva + LEAF_WINDOW];
  let text = found
    ? `function rva 0x${hex(begin)}, size 0x${hex(end - begin)}\n`
    : `leaf function rva 0x${hex(begin)}, not in the exception table, up to the first ret\n`;
  for (const insn of disasm(begin, end)) {
    const address = Number(insn.address);
    const bytes = Array.from(insn.bytes, (b) => hex(b, 2)).join(" ");
    text += `+0x${hex(address - begin, 4)}  ${hex(address, 8)}  ${bytes.padEnd(32)} ${insn.mnemonic} ${insn.opStr}\n`;
    if (!found && insn.mnemonic === "ret") break;
  }
  return { begin, text };
}

module.exports = {
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
};
