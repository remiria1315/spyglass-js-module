const fs = require("fs");
const { parseArgs } = require("util");
const { Capstone, Const, loadCapstone } = require("capstone-wasm");
const {
  EXECUTE_FLAG,
  parsePE,
  getMemoryMappedImage,
  tokensOf,
  countMatches,
  cppString,
  hex,
  die,
  loadFunctions,
  disassemblyOf,
} = require("./pe");

const DEFAULT_FLOATS = [70, 110, 30, 0.017453292, 1.1];
const DEFAULT_STRINGS = ["field_of_view", "fieldofview", "fov"];
const FLOAT_OPCODES = new Set([0x10, 0x58, 0x59, 0x5c, 0x5d, 0x5e, 0x5f]);

async function main() {
  const { values, positionals } = parseArgs({
    allowPositionals: true,
    options: {
      top: { type: "string", default: "10" },
      memory: { type: "boolean", default: false },
      float: { type: "string", multiple: true },
      string: { type: "string", multiple: true },
      "max-size": { type: "string", default: "0x300" },
      "min-floats": { type: "string", default: "2" },
      disasm: { type: "string" },
      callers: { type: "string" },
      vtable: { type: "string" },
      refs: { type: "string" },
      base: { type: "string" },
      out: { type: "string", default: "fov_disasm.txt" },
    },
  });
  if (positionals.length < 1) {
    console.error(
      "usage: node find_fov_patterns.js exe [--top N] [--memory] [--float V]... [--string S]... " +
        "[--max-size N] [--min-floats N] [--disasm RVA] [--callers RVA] [--vtable RVA [--base HEX]] [--refs RVA] [--out FILE]",
    );
    process.exit(2);
  }
  const floats = (values.float ?? DEFAULT_FLOATS).map(Number);
  const needles = values.string ?? DEFAULT_STRINGS;
  const maxSize = Number(values["max-size"]);
  const minFloats = parseInt(values["min-floats"], 10);
  const top = parseInt(values.top, 10);

  const data = fs.readFileSync(positionals[0]);
  const pe = parsePE(data);
  const image = values.memory ? data : getMemoryMappedImage(data, pe);
  const { functions: unsorted } = loadFunctions(image, pe);
  const functions = unsorted.slice().sort((a, b) => a[0] - b[0]);

  await loadCapstone();
  const md = new Capstone(Const.CS_ARCH_X86, Const.CS_MODE_64);
  const disasm = (begin, end) => md.disasm(image.subarray(begin, end), { address: begin });

  if (values.disasm !== undefined) {
    const rva = Number(values.disasm);
    const dumped = disassemblyOf(disasm, functions, rva);
    if (!dumped) die(`rva 0x${hex(rva)} is not inside a function of the exception table`);
    fs.writeFileSync(values.out, dumped.text);
    console.log(`wrote the disassembly of 0x${hex(dumped.begin)} to ${values.out}`);
    return;
  }

  if (values.callers !== undefined) {
    const callee = Number(values.callers);
    let found = 0;
    for (const section of pe.sections) {
      if (!(section.Characteristics & EXECUTE_FLAG)) continue;
      const last = Math.min(section.VirtualAddress + section.Misc_VirtualSize, image.length) - 5;
      for (let i = section.VirtualAddress; i < last; i++) {
        if ((image[i] !== 0xe8 && image[i] !== 0xe9) || i + 5 + image.readInt32LE(i + 1) !== callee) continue;
        const owner = functions.find(([begin, end]) => begin <= i && i < end);
        const place = owner
          ? `in function 0x${hex(owner[0])}, size 0x${hex(owner[1] - owner[0])}, at +0x${hex(i - owner[0])}`
          : "outside the exception table";
        console.log(`${image[i] === 0xe8 ? "call" : "jmp"} at rva 0x${hex(i)} ${place}`);
        found++;
      }
    }
    if (found === 0) die(`no direct call or jmp to rva 0x${hex(callee)}; it may be reached through a vtable`);
    return;
  }

  if (values.vtable !== undefined) {
    const base = values.base !== undefined ? BigInt(values.base) : pe.imageBase;
    const wanted = base + BigInt(Number(values.vtable));
    const low = Number(wanted & 0xffffffffn);
    const high = Number(wanted >> 32n);
    const executable = pe.sections.filter((s) => s.Characteristics & EXECUTE_FLAG);
    const pointsIntoCode = (at) => {
      const rva = Number(image.readBigUInt64LE(at) - base);
      return executable.some((s) => s.VirtualAddress <= rva && rva < s.VirtualAddress + s.Misc_VirtualSize);
    };
    let found = 0;
    for (const section of pe.sections) {
      if (section.Characteristics & EXECUTE_FLAG) continue;
      const last = Math.min(section.VirtualAddress + section.Misc_VirtualSize, image.length) - 8;
      for (let i = section.VirtualAddress; i < last; i += 8) {
        if (image.readUInt32LE(i) !== low || image.readUInt32LE(i + 4) !== high) continue;
        let start = i;
        while (start >= 16 && pointsIntoCode(start - 8)) start -= 8;
        let name = "unknown";
        if (start >= 8) {
          const col = Number(image.readBigUInt64LE(start - 8) - base);
          if (col > 0 && col + 24 <= image.length && image.readUInt32LE(col) <= 1) {
            const descriptor = image.readUInt32LE(col + 12) + 16;
            const end = image.indexOf(0, descriptor);
            if (descriptor < image.length && end !== -1 && end - descriptor < 256) {
              name = image.toString("latin1", descriptor, end);
            }
          }
        }
        console.log(`slot ${(i - start) / 8} at rva 0x${hex(i)}, table starts at rva 0x${hex(start)}, class ${name}`);
        for (let at = start, slot = 0; at < i + 8 * 32 && pointsIntoCode(at); at += 8, slot++) {
          console.log(`  slot ${slot}: rva 0x${hex(Number(image.readBigUInt64LE(at) - base))}`);
        }
        found++;
      }
    }
    if (found === 0) {
      die(`no pointer to 0x${hex(Number(wanted))} in the data sections; pass --base with the base that dump_client.js printed`);
    }
    return;
  }

  if (values.refs !== undefined) {
    const wanted = Number(values.refs);
    let found = 0;
    for (const section of pe.sections) {
      if (!(section.Characteristics & EXECUTE_FLAG)) continue;
      const last = Math.min(section.VirtualAddress + section.Misc_VirtualSize, image.length) - 7;
      for (let i = section.VirtualAddress; i < last; i++) {
        if ((image[i] & 0xf8) !== 0x48 || image[i + 1] !== 0x8d || (image[i + 2] & 0xc7) !== 0x05) continue;
        if (i + 7 + image.readInt32LE(i + 3) !== wanted) continue;
        const owner = functions.find(([begin, end]) => begin <= i && i < end);
        const place = owner
          ? `in function 0x${hex(owner[0])}, size 0x${hex(owner[1] - owner[0])}, at +0x${hex(i - owner[0])}`
          : "outside the exception table";
        console.log(`lea at rva 0x${hex(i)} ${place}`);
        found++;
      }
    }
    if (found === 0) die(`no lea of rva 0x${hex(wanted)}`);
    return;
  }

  const strings = new Map();
  for (const needle of needles) {
    for (const variant of [needle, needle.toUpperCase()]) {
      const literal = Buffer.from(variant, "latin1");
      let at = image.indexOf(literal);
      while (at !== -1) {
        let start = at;
        while (start > 0 && image[start - 1] >= 0x20 && image[start - 1] < 0x7f) start--;
        let end = at;
        while (end < image.length && image[end] >= 0x20 && image[end] < 0x7f) end++;
        if (image[end] === 0 && end - start <= 96 && !strings.has(start)) {
          strings.set(start, image.toString("latin1", start, end));
        }
        at = image.indexOf(literal, at + 1);
      }
    }
  }

  const floatsByBits = new Map(floats.map((f) => [Buffer.from(Float32Array.of(f).buffer).readUInt32LE(0), f]));
  const byFunction = new Map();
  for (const section of pe.sections) {
    if (!(section.Characteristics & EXECUTE_FLAG)) continue;
    const last = Math.min(section.VirtualAddress + section.Misc_VirtualSize, image.length) - 9;
    for (let i = section.VirtualAddress; i < last; i++) {
      let j = i;
      let isFloat = false;
      let target = -1;
      let immediate;
      if (image[j] >= 0xb8 && image[j] <= 0xbf) {
        immediate = floatsByBits.get(image.readUInt32LE(j + 1));
        if (immediate === undefined) continue;
      } else {
        if (image[j] === 0xf3) {
          j++;
          if ((image[j] & 0xf0) === 0x40) j++;
          if (image[j] !== 0x0f || !FLOAT_OPCODES.has(image[j + 1])) continue;
          isFloat = true;
          j += 2;
        } else if ((image[j] & 0xf8) === 0x48 && image[j + 1] === 0x8d) {
          j += 2;
        } else {
          continue;
        }
        if ((image[j] & 0xc7) !== 0x05) continue;
        target = j + 5 + image.readInt32LE(j + 1);
        if (target < 0 || target + 4 > image.length) continue;
      }

      let low = 0;
      let high = functions.length - 1;
      let owner = null;
      while (low <= high && owner === null) {
        const mid = (low + high) >> 1;
        if (i < functions[mid][0]) high = mid - 1;
        else if (i >= functions[mid][1]) low = mid + 1;
        else owner = functions[mid];
      }
      if (owner === null || owner[1] - owner[0] > maxSize) continue;

      const value = isFloat ? image.readFloatLE(target) : 0;
      const wanted = isFloat ? floats.find((f) => Math.abs(f - value) <= Math.abs(f) * 1e-6) : immediate;
      if (wanted === undefined && !strings.has(target)) continue;
      if (!byFunction.has(owner[0])) {
        byFunction.set(owner[0], { begin: owner[0], end: owner[1], floats: new Set(), strings: new Set() });
      }
      const found = byFunction.get(owner[0]);
      if (wanted !== undefined) found.floats.add(wanted);
      else found.strings.add(strings.get(target));
    }
  }

  const ranked = [...byFunction.values()]
    .filter((f) => f.floats.size >= minFloats || f.strings.size > 0)
    .sort(
      (a, b) =>
        b.strings.size * 4 + b.floats.size - (a.strings.size * 4 + a.floats.size) || a.end - a.begin - (b.end - b.begin),
    );

  console.log(`strings containing ${needles.join(", ")}: ${strings.size}`);
  console.log(`functions up to 0x${hex(maxSize)} bytes with a match: ${ranked.length}\n`);
  if (ranked.length === 0) {
    die("nothing matched; widen --max-size, lower --min-floats or pass --float and --string values");
  }

  let shown = 0;
  for (const f of ranked.slice(0, top)) {
    let tokens = [];
    for (const insn of disasm(f.begin, f.end)) {
      tokens = tokens.concat(tokensOf(insn));
      if (tokens.length >= 32 && countMatches(image, tokens, 0, image.length) === 1) break;
    }
    const unique = countMatches(image, tokens, 0, image.length) === 1;
    console.log(`candidate ${++shown}: rva 0x${hex(f.begin)}, size 0x${hex(f.end - f.begin)}`);
    console.log(`  float constants: ${[...f.floats].join(", ") || "none"}`);
    if (f.strings.size > 0) console.log(`  strings: ${[...f.strings].join(", ")}`);
    console.log(unique ? cppString("fov", tokens) : "  no unique pattern for the start of this function");
    console.log();
  }
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
