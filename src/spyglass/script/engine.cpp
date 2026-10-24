#include "spyglass/script/engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <deque>
#include <format>
#include <fstream>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#include <quickjs.h>

#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>
#endif

#include "spyglass/core/output.h"
#include "spyglass/script/system.h"

namespace spyglass::script {
namespace {

constexpr int kIdLimit = 1024;
constexpr std::size_t kLogLimit = 500;
constexpr std::size_t kTimerLimit = 256;
constexpr std::size_t kPendingLimit = 16;
constexpr std::uintmax_t kFileLimit = 16U * 1024U * 1024U;
constexpr std::array<std::string_view, 4> kPermissions{"fs", "fs.all", "net", "process"};

constexpr std::string_view kPreludeHead = R"js(
const native = globalThis.__spyglass;
delete globalThis.__spyglass;

const directions = { c2s: 1, s2c: 2, both: 3 };

const utf8 = (text) => {
  const out = [];
  for (const char of text) {
    const code = char.codePointAt(0);
    if (code < 0x80) out.push(code);
    else if (code < 0x800) out.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
    else if (code < 0x10000) out.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
    else out.push(0xf0 | (code >> 18), 0x80 | ((code >> 12) & 0x3f), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
  }
  return out;
};

const fromUtf8 = (bytes) => {
  let text = "";
  for (let i = 0; i < bytes.length;) {
    const lead = bytes[i];
    const width = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    let code = width === 1 ? lead : lead & (0xff >> (width + 1));
    for (let k = 1; k < width; k++) code = (code << 6) | (bytes[i + k] & 0x3f);
    text += String.fromCodePoint(code);
    i += width;
  }
  return text;
};

export class BinaryWriter {
  #bytes = [];
  byte(value) { this.#bytes.push(value & 0xff); return this; }
  bool(value) { return this.byte(value ? 1 : 0); }
  uint8(value) { return this.byte(value); }
  uint16(value) { return this.byte(value).byte(value >> 8); }
  uint32(value) { return this.uint16(value).uint16(value >>> 16); }
  int32(value) { return this.uint32(value); }
  float32(value) {
    const view = new DataView(new ArrayBuffer(4));
    view.setFloat32(0, value, true);
    return this.bytes(new Uint8Array(view.buffer));
  }
  varint(value) {
    let rest = value >>> 0;
    while (rest >= 0x80) {
      this.byte((rest & 0x7f) | 0x80);
      rest >>>= 7;
    }
    return this.byte(rest);
  }
  string(text) {
    const encoded = utf8(text);
    this.varint(encoded.length);
    return this.bytes(encoded);
  }
  bytes(data) {
    for (const value of data) this.#bytes.push(value);
    return this;
  }
  int8(value) { return this.byte(value); }
  int16(value) { return this.uint16(value); }
  uint64(value) {
    let rest = BigInt.asUintN(64, BigInt(value));
    for (let i = 0; i < 8; i++) {
      this.byte(Number(rest & 0xffn));
      rest >>= 8n;
    }
    return this;
  }
  int64(value) { return this.uint64(value); }
  float64(value) {
    const view = new DataView(new ArrayBuffer(8));
    view.setFloat64(0, value, true);
    return this.bytes(new Uint8Array(view.buffer));
  }
  svarint(value) { return this.varint(((value << 1) ^ (value >> 31)) >>> 0); }
  varuint32(value) { return this.varint(value); }
  varuint8(value) {
    if (!Number.isInteger(value) || value < 0 || value > 0xff) throw new RangeError("varuint8 is 0 to 255");
    return this.varint(value);
  }
  varint8(value) {
    if (!Number.isInteger(value) || value < -128 || value > 127) throw new RangeError("varint8 is -128 to 127");
    return this.svarint(value);
  }
  varint64(value) {
    let rest = BigInt.asUintN(64, BigInt(value));
    while (rest >= 0x80n) {
      this.byte(Number(rest & 0x7fn) | 0x80);
      rest >>= 7n;
    }
    return this.byte(Number(rest));
  }
  varuint64(value) { return this.varint64(value); }
  svarint64(value) {
    const signed = BigInt.asIntN(64, BigInt(value));
    return this.varint64((signed << 1n) ^ (signed >> 63n));
  }
  bitset(value, size, encoding = "varint") {
    let rest = BigInt(value);
    if (rest < 0n || rest >> BigInt(size) !== 0n) throw new RangeError(`the value does not fit in ${size} bits`);
    if (encoding === "bytes") {
      for (let i = 0; i < Math.ceil(size / 8); i++) {
        this.byte(Number(rest & 0xffn));
        rest >>= 8n;
      }
      return this;
    }
    if (encoding === "list") {
      const indices = [];
      for (let i = 0; rest >> BigInt(i) !== 0n; i++) if ((rest >> BigInt(i)) & 1n) indices.push(i);
      this.varint(indices.length);
      for (const index of indices) this.svarint(index);
      return this;
    }
    if (encoding !== "varint") throw new TypeError("encoding is varint, bytes or list");
    while (rest >= 0x80n) {
      this.byte(Number(rest & 0x7fn) | 0x80);
      rest >>= 7n;
    }
    return this.byte(Number(rest));
  }
  toBytes() { return Uint8Array.from(this.#bytes); }
}

export class BinaryReader {
  #data;
  #at = 0;
  constructor(data) { this.#data = data instanceof Uint8Array ? data : new Uint8Array(data); }
  get remaining() { return this.#data.length - this.#at; }
  #take(count) {
    if (count > this.remaining) throw new RangeError("read past the end of the packet");
    const start = this.#at;
    this.#at += count;
    return this.#data.subarray(start, start + count);
  }
  byte() { return this.#take(1)[0]; }
  bool() { return this.byte() !== 0; }
  uint8() { return this.byte(); }
  uint16() { const b = this.#take(2); return b[0] | (b[1] << 8); }
  uint32() { const b = this.#take(4); return (b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)) >>> 0; }
  int32() { return this.uint32() | 0; }
  float32() { const b = this.#take(4); return new DataView(b.buffer, b.byteOffset, 4).getFloat32(0, true); }
  varint() {
    let value = 0;
    for (let shift = 0; shift < 35; shift += 7) {
      const b = this.byte();
      value |= (b & 0x7f) << shift;
      if ((b & 0x80) === 0) return value >>> 0;
    }
    throw new RangeError("varint is too long");
  }
  int8() { return (this.byte() << 24) >> 24; }
  int16() { return (this.uint16() << 16) >> 16; }
  uint64() { const b = this.#take(8); return new DataView(b.buffer, b.byteOffset, 8).getBigUint64(0, true); }
  int64() { const b = this.#take(8); return new DataView(b.buffer, b.byteOffset, 8).getBigInt64(0, true); }
  float64() { const b = this.#take(8); return new DataView(b.buffer, b.byteOffset, 8).getFloat64(0, true); }
  svarint() { const value = this.varint(); return (value >>> 1) ^ -(value & 1); }
  varuint32() { return this.varint(); }
  varuint8() {
    const value = this.varint();
    if (value > 0xff) throw new RangeError("varuint8 is 0 to 255");
    return value;
  }
  varint8() {
    const value = this.svarint();
    if (value < -128 || value > 127) throw new RangeError("varint8 is -128 to 127");
    return value;
  }
  varint64() {
    let value = 0n;
    for (let shift = 0n; shift < 70n; shift += 7n) {
      const b = this.byte();
      value |= BigInt(b & 0x7f) << shift;
      if ((b & 0x80) === 0) return value;
    }
    throw new RangeError("varint64 is too long");
  }
  svarint64() { const value = this.varint64(); return (value >> 1n) ^ -(value & 1n); }
  varuint64() { return this.varint64(); }
  bitset(size, encoding = "varint") {
    let value = 0n;
    if (encoding === "bytes") {
      const data = this.#take(Math.ceil(size / 8));
      for (let i = data.length - 1; i >= 0; i--) value = (value << 8n) | BigInt(data[i]);
    }
    else if (encoding === "list") {
      for (let count = this.varint(); count > 0; count--) {
        const index = this.svarint();
        if (index < 0 || index >= size) throw new RangeError(`bit ${index} is outside ${size} bits`);
        value |= 1n << BigInt(index);
      }
    }
    else if (encoding === "varint") {
      for (let shift = 0n, last = false; !last; shift += 7n) {
        if (shift >= BigInt(size) + 7n) throw new RangeError("bitset is too long");
        const b = this.byte();
        value |= BigInt(b & 0x7f) << shift;
        last = (b & 0x80) === 0;
      }
    }
    else throw new TypeError("encoding is varint, bytes or list");
    if (value >> BigInt(size) !== 0n) throw new RangeError(`the value does not fit in ${size} bits`);
    return value;
  }
  string() { return fromUtf8(this.#take(this.varint())); }
  bytes(count) { return this.#take(count).slice(); }
}

export class Packet {}
)js";

constexpr std::string_view kPreludeTail = R"js(
native.setFactory((id, fields) => Object.assign(new (classes.get(id) ?? Packet)(), fields));

export const packet = Object.freeze({
  subscribe(target, callback, options = {}) {
    const id = typeof target === "function" ? target.id : target;
    const direction = options.direction ?? "both";
    if (!Number.isInteger(id)) throw new TypeError("subscribe expects a packet id or a packet class");
    if (typeof callback !== "function") throw new TypeError("subscribe expects a function");
    if (!Object.hasOwn(directions, direction)) throw new TypeError("direction is c2s, s2c or both");
    const token = native.subscribe(id, callback, directions[direction]);
    return () => native.unsubscribe(token);
  },
  send(target, body, options = {}) {
    const id = typeof target === "function" ? target.id : target;
    const direction = options.direction ?? (native.side === "server" ? "s2c" : "c2s");
    if (!Number.isInteger(id)) throw new TypeError("send expects a packet id or a packet class");
    if (direction !== "c2s" && direction !== "s2c") throw new TypeError("direction is c2s or s2c");
    const to = options.to === undefined || options.to === "all" ? -1 : options.to;
    if (!Number.isInteger(to)) throw new TypeError("to is a player number or \"all\"");
    native.send(id, directions[direction], typeof body?.toBytes === "function" ? body.toBytes() : body, to);
  },
});

const primitive = (name) => ({ read: (reader) => reader[name](), write: (writer, value) => writer[name](value) });

export const Types = Object.freeze({
  byte: primitive("byte"),
  uint8: primitive("uint8"),
  bool: primitive("bool"),
  int8: primitive("int8"),
  int16: primitive("int16"),
  uint16: primitive("uint16"),
  int32: primitive("int32"),
  uint32: primitive("uint32"),
  int64: primitive("int64"),
  uint64: primitive("uint64"),
  float32: primitive("float32"),
  float64: primitive("float64"),
  varint: primitive("varint"),
  svarint: primitive("svarint"),
  varuint8: primitive("varuint8"),
  varint8: primitive("varint8"),
  varuint32: primitive("varuint32"),
  varuint64: primitive("varuint64"),
  varint64: primitive("varint64"),
  svarint64: primitive("svarint64"),
  string: primitive("string"),
  bitset: (size, encoding) => ({
    read: (reader) => reader.bitset(size, encoding),
    write: (writer, value) => writer.bitset(value, size, encoding),
  }),
  bytes: { read: (reader) => reader.bytes(reader.remaining), write: (writer, value) => writer.bytes(value) },
  uuid: {
    read: (reader) => {
      const h = reader.uint64().toString(16).padStart(16, "0") + reader.uint64().toString(16).padStart(16, "0");
      return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
    },
    write: (writer, value) => {
      const h = String(value).replaceAll("-", "");
      if (!/^[0-9a-fA-F]{32}$/.test(h)) throw new TypeError("a uuid is 32 hex digits");
      return writer.uint64(BigInt(`0x${h.slice(0, 16)}`)).uint64(BigInt(`0x${h.slice(16)}`));
    },
  },
  array: (item, length = primitive("varint")) => ({
    read: (reader) => Array.from({ length: length.read(reader) }, () => item.read(reader)),
    write: (writer, value) => {
      length.write(writer, value.length);
      for (const element of value) item.write(writer, element);
      return writer;
    },
  }),
  fixed: (item, count) => ({
    read: (reader) => Array.from({ length: count }, () => item.read(reader)),
    write: (writer, value) => {
      if (value.length !== count) throw new RangeError(`expected ${count} elements, got ${value.length}`);
      for (const element of value) item.write(writer, element);
      return writer;
    },
  }),
  optional: (item) => ({
    read: (reader) => (reader.bool() ? item.read(reader) : undefined),
    write: (writer, value) => {
      writer.bool(value !== undefined && value !== null);
      if (value !== undefined && value !== null) item.write(writer, value);
      return writer;
    },
  }),
  struct: (fields) => ({
    read: (reader) => {
      const value = {};
      for (const [name, type] of Object.entries(fields)) value[name] = type.read(reader);
      return value;
    },
    write: (writer, value) => {
      for (const [name, type] of Object.entries(fields)) {
        if (!(name in value)) throw new TypeError(`missing field '${name}'`);
        type.write(writer, value[name]);
      }
      return writer;
    },
  }),
  custom: (read, write) => ({ read, write }),
});

export const variant = (value) => {
  const keys = value !== null && typeof value === "object" ? Object.keys(value) : [];
  if (keys.length !== 1) return null;
  const full = keys[0];
  return { name: full.slice(full.lastIndexOf("::") + 2), full, value: value[full] };
};

export const decode = (type, data, options = {}) => {
  const reader = new BinaryReader(data);
  const value = type.read(reader);
  if (reader.remaining !== 0 && !options.partial) throw new RangeError(`${reader.remaining} bytes were not read`);
  return value;
};

export const encode = (type, value) => type.write(new BinaryWriter(), value).toBytes();

const CommandEnum = Types.struct({ name: Types.string, values: Types.array(Types.uint32) });

const CommandData = (() => {
  const Parameter = Types.struct({ name: Types.string, type: Types.uint32, optional: Types.bool, options: Types.byte });
  const Overload = Types.struct({ chaining: Types.bool, parameters: Types.array(Parameter) });
  return Types.struct({
    name: Types.string,
    description: Types.string,
    flags: Types.uint16,
    permission: Types.string,
    alias: Types.int32,
    chained: Types.array(Types.uint32),
    overloads: Types.array(Overload),
  });
})();

const AvailableCommands = (() => {
  const strings = Types.array(Types.string);
  const Chained = Types.struct({
    name: Types.string,
    values: Types.array(Types.struct({ first: Types.varint, second: Types.varint })),
  });
  const Soft = Types.struct({ name: Types.string, values: strings });
  const Constraint = Types.struct({ value: Types.int32, enum: Types.int32, constraints: Types.array(Types.byte) });
  return Types.struct({
    enumValues: strings,
    chainedValues: strings,
    suffixes: strings,
    enums: Types.array(CommandEnum),
    chained: Types.array(Chained),
    commands: Types.array(CommandData),
    softEnums: Types.array(Soft),
    constraints: Types.array(Constraint),
  });
})();

const scanCommands = (body) => {
  const reader = new BinaryReader(body);
  const position = () => body.length - reader.remaining;
  const skipString = (r) => { r.bytes(r.varint()); };
  const skipList = (r, item) => { for (let count = r.varint(); count > 0; count--) item(r); };
  const section = (read) => {
    const start = position();
    const count = reader.varint();
    const payload = position();
    const items = [];
    for (let i = 0; i < count; i++) {
      const from = position();
      items.push({ item: read(reader), from, to: position() });
    }
    return { start, count, payload, end: position(), items };
  };
  const enumValues = section((r) => r.string());
  const chainedValues = section(skipString);
  const suffixes = section(skipString);
  const enums = section((r) => { skipString(r); r.bytes(r.varint() * 4); });
  const chained = section((r) => { skipString(r); skipList(r, (x) => { x.varint(); x.varint(); }); });
  const commands = section((r) => {
    const name = r.string();
    skipString(r);
    r.uint16();
    skipString(r);
    r.int32();
    r.bytes(r.varint() * 4);
    skipList(r, (o) => { o.bool(); skipList(o, (p) => { skipString(p); p.uint32(); p.bool(); p.byte(); }); });
    return name;
  });
  const softEnums = section((r) => { skipString(r); skipList(r, skipString); });
  const constraints = section((r) => { r.int32(); r.int32(); r.bytes(r.varint()); });
  if (reader.remaining !== 0) throw new Error(`${reader.remaining} bytes follow the constraints`);
  return { enumValues, chainedValues, suffixes, enums, chained, commands, softEnums, constraints };
};

const RawText = Types.struct({
  needsTranslation: Types.bool,
  variant: Types.byte,
  type: Types.byte,
  source: Types.string,
  message: Types.string,
  xuid: Types.string,
  platformChatId: Types.string,
  filtered: Types.optional(Types.string),
});

const CommandRequest = Types.struct({ command: Types.string, rest: Types.bytes });

export const Layouts = Object.freeze({ availableCommands: AvailableCommands, commandRequest: CommandRequest, rawText: RawText, chat: RawText });

export const ParamTypes = Object.freeze({
  int: 1, float: 3, value: 4, target: 8, string: 48, intPosition: 64, position: 65, message: 67, text: 70, json: 74, command: 87,
});

const registered = new Map();
let hooked = false;
let warned = false;

const tell = (message, player) => {
  packet.send(Packets.textPacket, encode(RawText, { needsTranslation: false, variant: 1, type: 1, source: "Spyglass", message: String(message), xuid: "", platformChatId: "", filtered: undefined }), { direction: "s2c", to: player ?? "all" });
};

const permissionNames = ["any", "gamedirectors", "admin", "host", "owner", "internal"];

const enumOf = (list, name, options) => {
  const indices = options.map((option) => {
    let at = list.enumValues.indexOf(option);
    if (at < 0) at = list.enumValues.push(option) - 1;
    return at;
  });
  list.enums.push({ name, values: indices });
  return list.enumBase + list.enums.length - 1;
};

const addCommand = (list, definition) => {
  const alias = definition.aliases?.length ? enumOf(list, `${definition.name}Aliases`, [definition.name, ...definition.aliases]) : -1;
  const parameters = (definition.parameters ?? []).map((parameter) => {
    let type = typeof parameter.type === "number" ? parameter.type : ParamTypes[parameter.type ?? "string"];
    if (parameter.enum) type = 0x300000 | enumOf(list, `${definition.name}_${parameter.name}`, parameter.enum);
    else if (type === undefined) throw new TypeError(`unknown parameter type '${parameter.type}'`);
    else type |= 0x100000;
    return { name: parameter.name, type, optional: !!parameter.optional, options: 0 };
  });
  list.commands.push({
    name: definition.name,
    description: definition.description ?? "",
    flags: definition.flags ?? 0,
    permission: typeof definition.permission === "number" ? permissionNames[definition.permission] ?? "any" : String(definition.permission ?? "any").toLowerCase(),
    alias,
    chained: [],
    overloads: [{ chaining: false, parameters }],
  });
};

const hook = () => {
  if (hooked) return;
  if (Packets.availableCommandsPacket === undefined || Packets.commandRequestPacket === undefined) {
    throw new Error("this client does not have AvailableCommandsPacket and CommandRequestPacket");
  }
  hooked = true;
  packet.subscribe(Packets.availableCommandsPacket, (pkt, ctx, cancel, override) => {
    if (registered.size === 0) return;
    try {
      const body = ctx.body;
      const scanned = scanCommands(body);
      const list = {
        enumValues: scanned.enumValues.items.map((entry) => entry.item),
        enumBase: scanned.enums.count,
        enums: [],
        commands: [],
      };
      const known = list.enumValues.length;
      const names = new Set();
      for (const entry of registered.values()) {
        addCommand(list, entry.definition);
        names.add(entry.definition.name);
      }
      const kept = scanned.commands.items.filter((entry) => !names.has(entry.item));
      const written = (type, values) => {
        const writer = new BinaryWriter();
        for (const value of values) type.write(writer, value);
        return writer.toBytes();
      };
      const counted = (count) => new BinaryWriter().varint(count).toBytes();
      const parts = [
        counted(list.enumValues.length),
        body.subarray(scanned.enumValues.payload, scanned.enumValues.end),
        written(Types.string, list.enumValues.slice(known)),
        body.subarray(scanned.enumValues.end, scanned.enums.start),
        counted(scanned.enums.count + list.enums.length),
        body.subarray(scanned.enums.payload, scanned.enums.end),
        written(CommandEnum, list.enums),
        body.subarray(scanned.enums.end, scanned.commands.start),
        counted(kept.length + list.commands.length),
      ];
      let cursor = scanned.commands.payload;
      for (const entry of scanned.commands.items) {
        if (!names.has(entry.item)) continue;
        parts.push(body.subarray(cursor, entry.from));
        cursor = entry.to;
      }
      parts.push(body.subarray(cursor, scanned.commands.end), written(CommandData, list.commands), body.subarray(scanned.commands.end));
      const out = new Uint8Array(parts.reduce((sum, part) => sum + part.length, 0));
      let at = 0;
      for (const part of parts) {
        out.set(part, at);
        at += part.length;
      }
      override(out);
    } catch (error) {
      if (!warned) {
        warned = true;
        console.error(`AvailableCommandsPacket was left alone, commands are not listed: ${error.message}`);
      }
    }
  }, { direction: "s2c" });
  packet.subscribe(Packets.commandRequestPacket, (pkt, ctx, cancel) => {
    const { command } = decode(CommandRequest, ctx.body, { partial: true });
    const line = command.trim();
    if (!line.startsWith("/")) return;
    const words = Array.from(line.slice(1).matchAll(/"([^"]*)"|(\S+)/g), (match) => match[1] ?? match[2]);
    const name = (words.shift() ?? "").toLowerCase();
    const entry = [...registered.values()].find((item) => item.names.includes(name));
    if (!entry) return;
    cancel();
    const player = ctx.player ?? undefined;
    const fail = (error) => { console.error(`/${name}: ${error?.message ?? error}`); tell(`§c${error?.message ?? error}`, player); };
    try {
      const result = entry.handler(words, { name, line, player, reply: (text) => tell(text, player) });
      Promise.resolve(result).then((text) => { if (typeof text === "string") tell(text, player); }, fail);
    } catch (error) {
      fail(error);
    }
  }, { direction: "c2s" });
};

let requestTail;
const waiting = [];
const sendCommand = (line) => {
  const text = String(line).startsWith("/") ? String(line) : `/${line}`;
  packet.send(Packets.commandRequestPacket, new BinaryWriter().string(text).bytes(requestTail).toBytes());
};
if (Packets.commandRequestPacket !== undefined) {
  packet.subscribe(Packets.commandRequestPacket, (pkt, ctx) => {
    try {
      const reader = new BinaryReader(ctx.body);
      reader.string();
      requestTail = reader.bytes(reader.remaining);
      for (const line of waiting.splice(0)) sendCommand(line);
    } catch (error) {
      requestTail = undefined;
    }
  }, { direction: "c2s" });
}

export const commands = Object.freeze({
  get ready() { return requestTail !== undefined; },
  run(line, options = {}) {
    if (requestTail !== undefined) sendCommand(line);
    else if (options.wait) waiting.push(line);
    else throw new Error("run a command in chat once so that its header can be copied, or pass { wait: true }");
  },
  register(definition, handler) {
    if (typeof definition?.name !== "string" || !/^[a-z0-9_]+$/.test(definition.name)) {
      throw new TypeError("a command name is lower case letters, digits and underscores");
    }
    if (typeof handler !== "function") throw new TypeError("register expects a handler function");
    hook();
    registered.set(definition.name, { definition, handler, names: [definition.name, ...(definition.aliases ?? [])].map((n) => n.toLowerCase()) });
  },
  unregister(name) { registered.delete(name); },
});

export const fs = Object.freeze({
  readText: (path) => native.fs(0, String(path)),
  readBytes: (path) => native.fs(1, String(path)),
  writeText: (path, text) => native.fs(2, String(path), String(text)),
  writeBytes: (path, bytes) => native.fs(3, String(path), bytes),
  appendText: (path, text) => native.fs(4, String(path), String(text)),
  exists: (path) => native.fs(5, String(path)),
  list: (path = ".") => native.fs(6, String(path)),
  mkdir: (path) => native.fs(7, String(path)),
  remove: (path) => native.fs(8, String(path)),
  stat: (path) => native.fs(9, String(path)),
});

export class Response {
  #bytes;
  constructor(raw) {
    this.status = raw.status;
    this.ok = raw.status >= 200 && raw.status < 300;
    this.headers = {};
    for (const line of raw.headers.split(/\r?\n/)) {
      const colon = line.indexOf(":");
      if (colon > 0) this.headers[line.slice(0, colon).trim().toLowerCase()] = line.slice(colon + 1).trim();
    }
    this.#bytes = new Uint8Array(raw.body);
  }
  text() { return fromUtf8(this.#bytes); }
  json() { return JSON.parse(this.text()); }
  bytes() { return this.#bytes.slice(); }
}

export const fetch = async (url, options = {}) => {
  const headers = [];
  for (const [key, value] of Object.entries(options.headers ?? {})) headers.push(String(key), String(value));
  const body = options.body === undefined || options.body === null ? "" : String(options.body);
  const timeout = Math.min(Math.max(options.timeout ?? 15000, 1), 60000);
  return new Response(await native.request(String(url), String(options.method ?? "GET").toUpperCase(), headers, body, timeout));
};

export class WebSocket {
  static CONNECTING = 0;
  static OPEN = 1;
  static CLOSING = 2;
  static CLOSED = 3;
  #id = -1;
  #listeners = new Map();
  constructor(url, options = {}) {
    this.url = String(url);
    this.readyState = 0;
    this.onopen = null;
    this.onmessage = null;
    this.onerror = null;
    this.onclose = null;
    const headers = [];
    for (const [key, value] of Object.entries(options.headers ?? {})) headers.push(String(key), String(value));
    const timeout = Math.min(Math.max(options.timeout ?? 15000, 1), 60000);
    native.wsOpen(this.url, headers, timeout).then(
      (id) => {
        if (this.readyState === 3) {
          native.wsClose(id, 1000, "");
          return;
        }
        this.#id = id;
        this.readyState = 1;
        this.#emit("open", {});
        this.#receive();
      },
      (error) => this.#fail(error),
    );
  }
  send(data) {
    if (this.readyState !== 1) throw new Error("the WebSocket is not open");
    if (typeof data === "string") native.wsSend(this.#id, data, false);
    else native.wsSend(this.#id, typeof data?.toBytes === "function" ? data.toBytes() : data, true);
  }
  close(code = 1000, reason = "") {
    if (this.readyState >= 2) return;
    if (this.#id < 0) {
      this.readyState = 3;
      this.#emit("close", { code, reason: String(reason), wasClean: true });
      return;
    }
    this.readyState = 2;
    native.wsClose(this.#id, code, String(reason));
  }
  addEventListener(type, listener) {
    const list = this.#listeners.get(type) ?? [];
    list.push(listener);
    this.#listeners.set(type, list);
  }
  removeEventListener(type, listener) {
    this.#listeners.set(type, (this.#listeners.get(type) ?? []).filter((item) => item !== listener));
  }
  #receive() {
    native.wsReceive(this.#id).then(
      (message) => {
        if (message.closed) {
          this.readyState = 3;
          this.#emit("close", { code: message.code, reason: message.data, wasClean: message.code !== 1006 });
          return;
        }
        this.#emit("message", { data: message.binary ? new Uint8Array(message.data) : message.data });
        if (this.readyState < 3) this.#receive();
      },
      (error) => this.#fail(error),
    );
  }
  #fail(error) {
    this.readyState = 3;
    this.#emit("error", { message: error?.message ?? String(error) });
    this.#emit("close", { code: 1006, reason: "", wasClean: false });
  }
  #emit(type, event) {
    event.type = type;
    event.target = this;
    for (const handler of [this[`on${type}`], ...(this.#listeners.get(type) ?? [])]) {
      if (typeof handler !== "function") continue;
      try {
        handler.call(this, event);
      } catch (error) {
        console.error(`WebSocket ${type}: ${error?.message ?? error}`);
      }
    }
  }
}

export const net = Object.freeze({ fetch, WebSocket });

export const process = Object.freeze({
  platform: native.platform,
  run: (file, args = [], options = {}) =>
    native.run(String(file), args.map(String), String(options.cwd ?? ""), Math.min(Math.max(options.timeout ?? 30000, 1), 300000)),
});



// remiria's module

export let LocalPlayer;
export let World;
{
  //#region LocalPlayer
  const inputDatas = {
    6: "jumping",
    8: "sneaking",
    10: "up",
    11: "down",
    12: "left",
    13: "right",
    20: "sprinting",
    38: "emoting",
  };

  LocalPlayer = class {
    constructor() {
      this.name = "remiria1315";
      this.pos = { x: 0, y: 0, z: 0 };
      this.rot = { x: 0, y: 0 };
      this.move = { x: 0, y: 0 };
      this.playerheadRot = 0;
      this.inputData = {
        jumping: false,
        sneaking: false,
        up: false,
        down: false,
        left: false,
        right: false,
        sprinting: false,
        emoting: false,
      };
      //#region playerAuthInput
      packet.subscribe(
        Packets.playerAuthInputPacket,
        (pkt, ctx, c, o) => {
          const reader = new BinaryReader(ctx.body);
          const rotx = reader.float32();
          const roty = reader.float32();
          this.rot = { x: rotx, y: roty };

          const posx = reader.float32();
          const posy = reader.float32();
          const posz = reader.float32();
          this.pos = {
            x: posx,
            y: posy,
            z: posz,
          };

          const movex = reader.float32();
          const movey = reader.float32();
          this.move = {
            x: movex,
            y: movey,
          };

          const playerHeadRotation = reader.float32();
          this.playerheadRot = playerHeadRotation;
          const bits = reader.bitset(66, "list");
          const inputData = {};
          Object.entries(inputDatas).forEach(([k, v]) => {
            inputData[v] = ((bits >> BigInt(parseInt(k))) & 1n) === 1n;
          });
          this.inputData = inputData;
        },
        { direction: "c2s" },
      );
      //#endregion

      //#region Request
      this.requestTail = null;
      this.waiting = [];
      packet.subscribe(
        Packets.commandRequestPacket,
        (pkt, ctx) => {
          const reader = new BinaryReader(ctx.body);
          reader.string();
          this.requestTail = reader.bytes(reader.remaining);
          for (const line of this.waiting.splice(0)) {
            this.sendCommand(line);
          }
        },
        { direction: "c2s" },
      );
      //#endregion
      //#region eventFunctions
      this.event_Click = (ev) => {};
      this.event_Attack = (ev) => {};
      this.event_RightClick = () => {};
      this.event_Drop = () => {};
      this.event_Use = (ev) => {};
      this.event_Animation = (ev) => {};
      //#endregion
      //#region eventFromAnimate
      packet.subscribe(
        Packets.animatePacket,
        (pkt, ctx, c, o) => {
          const reader = new BinaryReader(ctx.body);

          const action = [
            "noAction",
            "swing",
            "wakeUp",
            "criticalHit",
            "MagicCriticalHit",
          ][reader.uint8()]; // uint8
          const targetActor = reader.varuint64();
          const data = reader.float32();
          const swingSource = reader.string();
          const hand = ["mainhand", "offhand"][reader.uint8()];
          //#region action
          if (action == "swing" && ["Mine", "Attack"].includes(swingSource)) {
            this.event_Click({ action, swingSource, hand });
          }
          if (action == "swing" && swingSource == "Attack") {
            this.event_Attack({ action, swingSource, hand });
          }
          if (swingSource == "DropItem") {
            this.event_Drop();
          } else if (swingSource == "ThrowItem") {
            this.event_Use();
            this.event_RightClick();
          }
          this.event_Animation({ action, targetActor, swingSource, hand });
        },
        { direction: "c2s" },
      );
      //#endregion
    }
    //#region sendCommand
    sendCommand(line) {
      if (!this.requestTail) {
        this.waiting.push(line);
        return;
      }
      const text = String(line).startsWith("/") ? String(line) : `/${line}`;
      packet.send(
        Packets.commandRequestPacket,
        new BinaryWriter().string(text).bytes(this.requestTail).toBytes(),
      );
    }
    //#region sendMessage
    sendMessage(str) {
      const body = new BinaryWriter()
        .byte(0)
        .byte(1)
        .bool(true)
        .string(this.name)
        .string(str)
        // .byte(0)
        .string("2535456620588248")
        .byte(0)
        .byte(0)
        .toBytes();

      packet.send(Packets.textPacket, body, { direction: "c2s" });
    }
  };
  //#endregion

  //#region World
  World = class {
    constructor() {
      this.playerList = {};
      this.self = null; // uuid
      this.setting = {};
      this.level = {};
      this.gamerules = {};
      this.blocks = {};
      this.playerPropertyData = null
      //#region startGamePacket
      packet.subscribe(
        Packets.startGamePacket,
        (pkt, ctx, c, o) => {
          const reader = new BinaryReader(ctx.body);
          const entityId = reader.svarint64();
          const runtimeId = reader.varuint64();
          const gameType = [
            "undefined",
            "survival",
            "creative",
            "adventure",
            "default",
            "spectator",
            "default",
          ][reader.varint()];
          //#region pos,rot
          const posx = reader.float32();
          const posy = reader.float32();
          const posz = reader.float32();
          const rotx = reader.float32();
          const roty = reader.float32();
          //#region setting
          const setting = (() => {
            const seed = reader.uint64();
            const spawnSetting = {
              biomeType: ["default", "userDefined"][reader.int16()],
              userDefinedBiomeName: reader.string(),
              dimension: reader.varint(),
            };
            const generatorType = [
              //#region type
              "legacy",
              "overworld",
              "flat",
              "nether",
              "theend",
              "void",
              "undefined",
              //#endregion
            ][reader.varint()];
            const globalGameType = [
              //#region type
              "undefined",
              "survival",
              "creative",
              "adventure",
              "default",
              "spectator",
              "default",
              //#endregion
            ][reader.varint()];
            const isHardcore = reader.bool();
            const difficulty = [
              //#region type
              "peaceful",
              "easy",
              "normal",
              "hard",
              "count",
              "unknown",
              //#endregion
            ][reader.varint()];
            const defaultSpawnBlockPosition = {
              x: reader.varint(),
              y: reader.varint(),
              z: reader.varint(),
            };
            const achievementDisabled = reader.bool();
            const editorWorldType = [
              //#region type
              "nonEditor",
              "editorProject",
              "editorTestLevel",
              "editorRealmsUpload",
              //#endregion
            ][reader.varint()];
            const isCreatedInEditor = reader.bool();
            const isExportedFromEditor = reader.bool();
            const dayCycleStopTime = reader.varint();
            const educationEditionOffer = [
              //#region type
              "none",
              "restOfWorld",
              "china_Deprecated",
              //#endregion
            ][reader.varuint32()];
            const educationFeaturesEnable = reader.bool();
            const educationProductId = reader.string();
            const rainLevel = reader.float32();
            const lightningLevel = reader.float32();
            const hasConfirmedPlatformLockedContent = reader.bool();
            const multiplayerGameIntent = reader.bool();
            const LANBroadcastInitent = reader.bool();
            const XLBSetting = [
              //#region type
              "noMultiPlay",
              "inviteOnly",
              "friendsOnly",
              "friendsOfFriends",
              "public",
              //#endregion
            ][reader.varint()];
            const platformBroadcastSetting = [
              //#region type
              "noMultiPlay",
              "inviteOnly",
              "friendsOnly",
              "friendsOfFriends",
              "public",
              //#endregion
            ][reader.varint()];
            const commandsEnabled = reader.bool();
            const texturePackRequired = reader.bool();
            const gameRules = (() => {
              const gameRule = {};
              const size = reader.varuint32();
              for (let i = 0; i < size; i++) {
                const name = reader.string();
                const canBeModifiedByPlayer = reader.bool();
                const ruleType = reader.varint();
                let value;
                switch (ruleType) {
                  case 0:
                    value = void reader.byte();
                    break;
                  case 1:
                    value = reader.bool();
                    break;
                  case 2:
                    value = reader.varint();
                    break;
                  case 3:
                    value = reader.int32();
                    break;
                  case 4:
                    value = reader.float32();
                    break;
                  default:
                    console.log("unknown rule type");
                    break;
                }
                gameRule[name] = { canBeModifiedByPlayer, ruleType, value };
              }
              return gameRule;
            })();
            const experiments = {
              toggles: (() => {
                const experiment = {};
                const size = reader.varuint32();
                for (let i = 0; i < size; i++) {
                  const name = reader.string();
                  const value = reader.bool();
                  experiment[name] = value;
                }
                return experiment;
              })(),
              experimentsEverToggled: reader.bool(),
            };
            const hasBonusChestEnabled = reader.bool();
            const startWithMapEnabled = reader.bool();
            const playerPermissions = [
              //#region type
              "visitor",
              "member",
              "operator",
              "custom",
              //#endregion
            ][reader.int8()];
            const serverChunkTickRange = reader.int32();
            const hasLockedBehaviorPack = reader.bool();
            const hasLockedResourcePack = reader.bool();
            const isFromWorldTemplate = reader.bool();
            const isWorldTemplateOptionLocked = reader.bool();
            const onlySpawnV1Villagers = reader.bool();
            const personaDisabled = reader.bool();
            const customSkinDisabled = reader.bool();
            const emoteChatMuted = reader.bool();
            const baseGameVersion = reader.string();
            const limitedWorldWidth = reader.int32();
            const netherType = reader.bool();
            const eduSharedUriResource = {
              name: reader.string(),
              link: reader.string(),
            };
            const overrideForceExperimentalGameplay =
              reader.bool() == true ? reader.bool() : null; // optional
            const chatRestrictionLevel = ["none", "dropped", "disabled"][
              reader.uint8()
            ];
            const disablePlayerInteractions = reader.bool();
            const serverEditorConnectionPolicy = [
              //#region type
              "matchworldtype",
              "editoronly",
              "vanillaonly",
              "mixed",
              //#endregion
            ][reader.varint()];
            const allowAnonymousBlockDropsInEditorWorlds = reader.bool();
            return {
              //#region return
              seed,
              spawnSetting,
              generatorType,
              globalGameType,
              isHardcore,
              difficulty,
              defaultSpawnBlockPosition,
              achievementDisabled,
              editorWorldType,
              isCreatedInEditor,
              isExportedFromEditor,
              dayCycleStopTime,
              educationEditionOffer,
              educationFeaturesEnable,
              educationProductId,
              rainLevel,
              lightningLevel,
              hasConfirmedPlatformLockedContent,
              multiplayerGameIntent,
              LANBroadcastInitent,
              XLBSetting,
              platformBroadcastSetting,
              commandsEnabled,
              texturePackRequired,
              gameRules,
              experiments,
              hasBonusChestEnabled,
              startWithMapEnabled,
              playerPermissions,
              serverChunkTickRange,
              hasLockedBehaviorPack,
              hasLockedResourcePack,
              isFromWorldTemplate,
              isWorldTemplateOptionLocked,
              onlySpawnV1Villagers,
              personaDisabled,
              customSkinDisabled,
              emoteChatMuted,
              baseGameVersion,
              limitedWorldWidth,
              netherType,
              eduSharedUriResource,
              overrideForceExperimentalGameplay,
              chatRestrictionLevel,
              disablePlayerInteractions,
              serverEditorConnectionPolicy,
              allowAnonymousBlockDropsInEditorWorlds,
              //#endregion
            };
          })();
          //#endregion

          const levelId = reader.string();
          const worldName = reader.string();
          const templateContentIdentity = reader.string();
          const isTrial = reader.bool();

          const movementSettings = {
            rewindHistorySize: reader.varint(),
            serverAuthoritativeBlockBreaking: reader.bool(),
          };
          const levelCurrentTime = reader.uint64();
          const enchantmentSeed = reader.varuint32();
          //#region blockProperty
          const blockProperties = (() => {
            const size = reader.varuint32();
            let blocks = {};
            for (let i = 0; i < size; i++) {
              const name = reader.string();
              const defs = {};

              const propSize = reader.varuint32();
              for (let j = 0; j < propSize; j++) {
                const propName = reader.string();
                const type = reader.byte();
                defs[propName] = this.#parseTagByType(reader, type);
              }

              blocks[name] = defs;
            }
            return blocks;
          })();
          const multiplayerCorrelationId = reader.string();
          const enableItemStackNetManager = reader.bool();
          const serverVersion = reader.string();
          reader.byte();
          reader.string();
          const playerPropertyData = this.#parseTagByType(reader, 10);
          //#region fortest


          this.playerPropertyData = playerPropertyData




          //#endregion
          const serverBlockTypeRegistryChecksum = reader.uint64();
          const worldTemplateId = [reader.uint64(), reader.uint64()];
          const serverEnabledClientSideGeneration = reader.bool();
          const blockNetworkIdsAreHashes = reader.bool();
          const networkPermissions = { serverAuthSoundenabled: reader.bool() };
          //#region serverConfigurationJoinInfo
          const serverConfigurationJoinInfo = {
            // optional
            //#region gathering
            gathering:
              reader.bool() == true
                ? {
                    // optional
                    experienceId: [reader.uint64(), reader.uint64()],
                    experienceName: reader.string(),
                    worldId:
                      reader.bool() == true
                        ? [
                            // optional
                            reader.uint64(),
                            reader.uint64(),
                          ]
                        : null,
                    worldName: reader.bool() == true ? reader.string() : null, // optional
                    creatorId: reader.string(),
                    targetId:
                      reader.bool() == true
                        ? [reader.uint64(), reader.uint64()]
                        : null, // optional
                    scenarioId: reader.bool() == true ? reader.string() : null, // optional
                    serverId: reader.bool() == true ? reader.string() : null, // optional
                  }
                : null,
            //#region clientStoreEntryPoint
            clientStoreEntryPoint:
              reader.bool() == true
                ? {
                    // optional
                    storeId: reader.string(),
                    storeName: reader.string(),
                  }
                : null,
            //#region presence
            presence:
              reader.bool() == true
                ? {
                    // optional
                    richPresenceId: reader.string(), // optional
                  }
                : null,
          };
          //#region serverTelemetryData
          const serverTelemetryData = {
            serverId: reader.string(),
            scenarioId: reader.string(),
            worldId: reader.string(),
            ownerId: reader.string(),
          };

          //#region apply
          this.setting = setting;
          this.setting.gameMode = gameType;
          this.setting.movement = movementSettings;
          this.setting.network = {
            //#region network
            multiplayerCorrelationId,
            enableItemStackNetManager,
            serverVersion,
            serverBlockTypeRegistryChecksum,
            serverEnabledClientSideGeneration,
            blockNetworkIdsAreHashes,
            networkPermissions,
            serverConfigurationJoinInfo,
            serverTelemetryData,
            //#endregion
          };
          this.blocks = blockProperties;
          this.level = {
            levelid: levelId,
            worldName,
            templateContentIdentity,
            isTrial,
            levelCurrentTime,
            enchantmentSeed,
            worldTemplateId,
          };
        },

        { direction: "s2c" },
      );
      packet.subscribe(Packets.playerListPacket, (pkt, ctx, c, o) => {
        const reader = new BinaryReader(ctx.body);
        const action = reader.byte();
        // const entriesCount = reader.varint
        const entriesCount = reader.varuint32();
        if (action == 0) {
          // remove
          for (let i = 0; i < entriesCount; i++) {
            const uuid = [reader.uint64(), reader.uint64()];
            delete this.playerList[uuid];
          }
        } else if (action == 1) {
          //add
          for (let i = 0; i < entriesCount; i++) {
            const uuid = [reader.uint64(), reader.uint64()];
            const actorUniqueId = reader.svarint64();
            const playerName = reader.string();
            const xuid = reader.string();
            const prefabid = reader.string();
            const platformid = reader.string();
            const platformNum = reader.int32();
            const platform =
              platformNum == -1
                ? "unknown"
                : [
                    //#region enum
                    "none(0)",
                    "google",
                    "ios",
                    "osx",
                    "amazon",
                    "win32",
                    "dedicated",
                    "sony",
                    "nintendo",
                    "xbox",
                    "linux",
                    //#endregion
                  ][platformNum];
            const serializedSkin = this.#serializeSkin(reader);
            const isTeacher = reader.bool();
            const isHost = reader.bool();
            const isSubClient = reader.bool();
            const PlayerColor = reader.int32();
            this.playerList[uuid] = {
              //#region objectify
              uuid,
              actorUniqueId,
              playerName,
              xuid,
              prefabid,
              platformid,
              platform,
              serializedSkin,
              isTeacher,
              isHost,
              isSubClient,
              PlayerColor,
              //#endregion
            };
          }
        }
      });
    }
    //#region parseTagByType
    #parseTagByType(reader, t) {
      if (t === 0) return null;
      if (t === 1) return reader.byte();
      if (t === 2) return reader.int16();
      if (t === 3) return reader.svarint();
      if (t === 4) return reader.svarint64();
      if (t === 5) return reader.float32();
      if (t === 6) return reader.float64(); // double
      if (t === 7) {
        // ByteArray
        const size2 = reader.svarint();
        const bytes = [];
        for (let j = 0; j < size2; j++) {
          bytes.push(reader.byte());
        }
        return bytes;
      }
      if (t === 8) return reader.string();
      if (t === 9) {
        // ListTag
        const listType = reader.byte();
        const size2 = reader.svarint();
        const listValues = [];
        for (let j = 0; j < size2; j++) {
          listValues.push(this.#parseTagByType(reader, listType));
        }
        return listValues;
      }
      if (t === 10) {
        // CompoundTag 再帰
        const compound = {};
        while (true) {
          const subType = reader.byte();
          if (subType === 0) break; // TAG_End
          const subName = reader.string();
          compound[subName] = this.#parseTagByType(reader, subType);
        }
        return compound;
      }
      if (t === 11) {
        // IntArray (varint[])
        const size2 = reader.svarint();
        const ints = [];
        for (let j = 0; j < size2; j++) {
          ints.push(reader.svarint());
        }
        return ints;
      }
      throw new Error(`Unknown definition type: ${t}`);
    }
    //#region SkinData
    #skinData(reader) {
      return {
        width: reader.uint32(),
        height: reader.uint32(),
        data: reader.string(),
      };
    }
    //#region serializeSkin
    #serializeSkin(reader) {
      return {
        //#region type
        id: reader.string(),
        resourcePatch: reader.string(),
        imageData: this.#skinData(reader),
        //#region AnimatedImageData
        animatedImageData: {
          skinimage: this.#skinData(reader),
          animatedTextureType: ["none", "face", "body32", "body128"][
            reader.varuint32()
          ],
          frames: reader.float32(),
          animationExpression: ["linear", "blinking"][reader.varuint32()],
        },
        capeImageData: this.#skinData(reader),
        geometrydata: reader.string(),
        geometryDataMinEngineVersion: reader.string(),
        animationData: reader.string(),
        capeId: reader.string(),
        fullId: reader.string(),
        armsize: ["slim", "wide"][reader.uint8()],
        skinColor: reader.int32(),
        //#region personaPieces
        personaPieces: (() => {
          const size = reader.uint32();
          const piece = {};
          for (let i = 0; i < size; i++) {
            piece[i] = {
              id: reader.string(),
              type: reader.string(),
              packId: reader.string(),
              isDefault: reader.bool(),
              productId: reader.string(),
            };
          }
          return piece;
        })(),
        //#region personaTintColors
        personaTintColors: (() => {
          const size = reader.uint32();
          const piece = {};
          for (let i = 0; i < size; i++) {
            piece[i] = {
              paletteType: reader.varuint32(),
              paletteColors: (() => {
                const size2 = reader.uint32();
                const colors = new Array(size2);
                for (let j = 0; j < size2; j++) {
                  colors[j] = [
                    reader.string(),
                    reader.string(),
                    reader.string(),
                    reader.string(),
                  ];
                }
                return colors;
              })(),
            };
          }
          return piece;
        })(),
        isPremium: reader.bool(),
        isPersonaCapeOnClassicSkin: reader.bool(),
        isPrimaryUser: reader.bool(),
        overridesPlayerAppearance: reader.bool(),
        trustedSkinFlag: reader.string(), // Unset False True
        profileHash: reader.string(),
        //#endregion
      };
    }
  }
  //#endregion
}


)js";

constexpr std::string_view kDeclarationsHead = R"ts(declare module "spyglass" {
  export class BinaryWriter {
    byte(value: number): this;
    uint8(value: number): this;
    bool(value: boolean): this;
    uint16(value: number): this;
    uint32(value: number): this;
    int32(value: number): this;
    float32(value: number): this;
    varint(value: number): this;
    int8(value: number): this;
    int16(value: number): this;
    int64(value: number | bigint): this;
    uint64(value: number | bigint): this;
    float64(value: number): this;
    svarint(value: number): this;
    varuint8(value: number): this;
    varint8(value: number): this;
    varuint32(value: number): this;
    varuint64(value: number | bigint): this;
    varint64(value: number | bigint): this;
    svarint64(value: number | bigint): this;
    bitset(value: bigint, size: number, encoding?: "varint" | "bytes" | "list"): this;
    string(text: string): this;
    bytes(data: ArrayLike<number>): this;
    toBytes(): Uint8Array;
  }
  export class BinaryReader {
    constructor(data: Uint8Array | ArrayBuffer);
    readonly remaining: number;
    byte(): number;
    uint8(): number;
    bool(): boolean;
    uint16(): number;
    uint32(): number;
    int32(): number;
    float32(): number;
    varint(): number;
    int8(): number;
    int16(): number;
    int64(): bigint;
    uint64(): bigint;
    float64(): number;
    svarint(): number;
    varuint8(): number;
    varint8(): number;
    varuint32(): number;
    varuint64(): bigint;
    varint64(): bigint;
    svarint64(): bigint;
    bitset(size: number, encoding?: "varint" | "bytes" | "list"): bigint;
    string(): string;
    bytes(count: number): Uint8Array;
  }
  export interface Type<T = any> {
    read(reader: BinaryReader): T;
    write(writer: BinaryWriter, value: T): BinaryWriter;
  }
  export const Types: {
    byte: Type<number>; uint8: Type<number>; bool: Type<boolean>; int8: Type<number>; int16: Type<number>; uint16: Type<number>;
    int32: Type<number>; uint32: Type<number>; int64: Type<bigint>; uint64: Type<bigint>;
    float32: Type<number>; float64: Type<number>; varint: Type<number>; svarint: Type<number>; varuint8: Type<number>; varint8: Type<number>; varuint32: Type<number>; varuint64: Type<bigint>;
    varint64: Type<bigint>; svarint64: Type<bigint>; string: Type<string>; bytes: Type<Uint8Array>; uuid: Type<string>;
    bitset(size: number, encoding?: "varint" | "bytes" | "list"): Type<bigint>;
    array<T>(item: Type<T>, length?: Type<number>): Type<T[]>;
    fixed<T>(item: Type<T>, count: number): Type<T[]>;
    optional<T>(item: Type<T>): Type<T | undefined>;
    struct<F extends Record<string, Type>>(fields: F): Type<{ [K in keyof F]: F[K] extends Type<infer T> ? T : never }>;
    custom<T>(read: (reader: BinaryReader) => T, write: (writer: BinaryWriter, value: T) => BinaryWriter | void): Type<T>;
  };
  export function variant(value: any): { name: string; full: string; value: any } | null;
  export function decode<T>(type: Type<T>, data: Uint8Array | ArrayBuffer, options?: { partial?: boolean }): T;
  export function encode<T>(type: Type<T>, value: T): Uint8Array;
  export interface CommandParameter {
    name: string;
    type?: keyof typeof ParamTypes | number;
    enum?: string[];
    optional?: boolean;
  }
  export interface CommandDefinition {
    name: string;
    description?: string;
    aliases?: string[];
    permission?: number | "any" | "gamedirectors" | "admin" | "host" | "owner" | "internal";
    flags?: number;
    parameters?: CommandParameter[];
  }
  export interface CommandContext { name: string; line: string; reply(message: string): void }
  export const ParamTypes: Record<"int" | "float" | "value" | "target" | "string" | "intPosition" | "position" | "message" | "text" | "json" | "command", number>;
  export const Layouts: { availableCommands: Type; commandRequest: Type; rawText: Type };
  export const commands: {
    register(definition: CommandDefinition, handler: (args: string[], context: CommandContext) => string | void | Promise<string | void>): void;
    unregister(name: string): void;
    readonly ready: boolean;
    run(line: string, options?: { wait?: boolean }): void;
  };
  export class Packet { [field: string]: any; static id: number; }
  export interface PacketContext {
    direction: "c2s" | "s2c";
    id: number;
    name: string;
    subClientId: number;
    size: number;
    cancelled: boolean;
    body: Uint8Array;
  }
  export type PacketClass = { new (): Packet; id: number };
  export const packet: {
    subscribe(
      target: number | PacketClass,
      callback: (pkt: any, ctx: PacketContext, cancel: () => void, override: (body: Uint8Array | ArrayBuffer) => void) => void,
      options?: { direction?: "c2s" | "s2c" | "both" },
    ): () => void;
    send(target: number | PacketClass, body: Uint8Array | BinaryWriter, options?: { direction?: "c2s" | "s2c" }): void;
  };
  export const fs: {
    readText(path: string): string;
    readBytes(path: string): Uint8Array;
    writeText(path: string, text: string): void;
    writeBytes(path: string, bytes: Uint8Array | ArrayBuffer): void;
    appendText(path: string, text: string): void;
    exists(path: string): boolean;
    list(path?: string): string[];
    mkdir(path: string): void;
    remove(path: string): void;
    stat(path: string): { isFile: boolean; isDirectory: boolean; size: number } | null;
  };
  export class Response {
    status: number;
    ok: boolean;
    headers: Record<string, string>;
    text(): string;
    json(): any;
    bytes(): Uint8Array;
  }
  export interface FetchOptions { method?: string; headers?: Record<string, string>; body?: string; timeout?: number }
  export function fetch(url: string, options?: FetchOptions): Promise<Response>;
  export interface WebSocketOptions { headers?: Record<string, string>; timeout?: number }
  export interface WebSocketEvent { type: string; target: WebSocket }
  export interface WebSocketMessageEvent extends WebSocketEvent { data: string | Uint8Array }
  export interface WebSocketErrorEvent extends WebSocketEvent { message: string }
  export interface WebSocketCloseEvent extends WebSocketEvent { code: number; reason: string; wasClean: boolean }
  export class WebSocket {
    static readonly CONNECTING: 0;
    static readonly OPEN: 1;
    static readonly CLOSING: 2;
    static readonly CLOSED: 3;
    readonly url: string;
    readyState: 0 | 1 | 2 | 3;
    onopen: ((event: WebSocketEvent) => void) | null;
    onmessage: ((event: WebSocketMessageEvent) => void) | null;
    onerror: ((event: WebSocketErrorEvent) => void) | null;
    onclose: ((event: WebSocketCloseEvent) => void) | null;
    constructor(url: string, options?: WebSocketOptions);
    send(data: string | Uint8Array | ArrayBuffer | BinaryWriter): void;
    close(code?: number, reason?: string): void;
    addEventListener(type: "open" | "message" | "error" | "close", listener: (event: any) => void): void;
    removeEventListener(type: "open" | "message" | "error" | "close", listener: (event: any) => void): void;
  }
  export const net: { fetch: typeof fetch; WebSocket: typeof WebSocket };
  export const process: {
    platform: string;
    run(file: string, args?: string[], options?: { cwd?: string; timeout?: number }): Promise<{ code: number; output: string }>;
  };

  export type NbtValue = null | number | bigint | string | number[] | NbtValue[] | { [name: string]: NbtValue };
  export type Uuid = [bigint, bigint];
  export interface Vec2 { x: number; y: number }
  export interface Vec3 { x: number; y: number; z: number }
  export interface InputFlags {
    jumping: boolean; sneaking: boolean; up: boolean; down: boolean;
    left: boolean; right: boolean; sprinting: boolean; emoting: boolean;
  }
  export type Hand = "mainhand" | "offhand";
  export type AnimateAction = "noAction" | "swing" | "wakeUp" | "criticalHit" | "MagicCriticalHit";
  export interface SwingEvent { action: "swing"; swingSource: string; hand: Hand }
  export interface AnimationEvent { action: AnimateAction; targetActor: bigint; swingSource: string; hand: Hand }
  export class LocalPlayer {
    name: string;
    pos: Vec3;
    rot: Vec2;
    move: Vec2;
    playerheadRot: number;
    inputData: InputFlags;
    requestTail: Uint8Array | null;
    waiting: string[];
    event_Click: (event: SwingEvent) => void;
    event_Attack: (event: SwingEvent) => void;
    event_RightClick: () => void;
    event_Drop: () => void;
    event_Use: () => void;
    event_Animation: (event: AnimationEvent) => void;
    sendCommand(line: string): void;
    sendMessage(text: string): void;
  }
  export interface SkinImage { width: number; height: number; data: string }
  export interface PersonaPiece { id: string; type: string; packId: string; isDefault: boolean; productId: string }
  export interface PersonaTintColor { paletteType: number; paletteColors: [string, string, string, string][] }
  export interface SerializedSkin {
    id: string;
    resourcePatch: string;
    imageData: SkinImage;
    animatedImageData: {
      skinimage: SkinImage;
      animatedTextureType: "none" | "face" | "body32" | "body128";
      frames: number;
      animationExpression: "linear" | "blinking";
    };
    capeImageData: SkinImage;
    geometrydata: string;
    geometryDataMinEngineVersion: string;
    animationData: string;
    capeId: string;
    fullId: string;
    armsize: "slim" | "wide";
    skinColor: number;
    personaPieces: Record<number, PersonaPiece>;
    personaTintColors: Record<number, PersonaTintColor>;
    isPremium: boolean;
    isPersonaCapeOnClassicSkin: boolean;
    isPrimaryUser: boolean;
    overridesPlayerAppearance: boolean;
    trustedSkinFlag: string;
    profileHash: string;
  }
  export interface PlayerListEntry {
    uuid: Uuid;
    actorUniqueId: bigint;
    playerName: string;
    xuid: string;
    prefabid: string;
    platformid: string;
    platform: string;
    serializedSkin: SerializedSkin;
    isTeacher: boolean;
    isHost: boolean;
    isSubClient: boolean;
    PlayerColor: number;
  }
  export type GameType = "undefined" | "survival" | "creative" | "adventure" | "default" | "spectator";
  export type BroadcastSetting = "noMultiPlay" | "inviteOnly" | "friendsOnly" | "friendsOfFriends" | "public";
  export interface GameRule { canBeModifiedByPlayer: boolean; ruleType: number; value: boolean | number | undefined }
  export interface WorldNetwork {
    multiplayerCorrelationId: string;
    enableItemStackNetManager: boolean;
    serverVersion: string;
    serverBlockTypeRegistryChecksum: bigint;
    serverEnabledClientSideGeneration: boolean;
    blockNetworkIdsAreHashes: boolean;
    networkPermissions: { serverAuthSoundenabled: boolean };
    serverConfigurationJoinInfo: {
      gathering: {
        experienceId: Uuid;
        experienceName: string;
        worldId: Uuid | null;
        worldName: string | null;
        creatorId: string;
        targetId: Uuid | null;
        scenarioId: string | null;
        serverId: string | null;
      } | null;
      clientStoreEntryPoint: { storeId: string; storeName: string } | null;
      presence: { richPresenceId: string } | null;
    };
    serverTelemetryData: { serverId: string; scenarioId: string; worldId: string; ownerId: string };
  }
  export interface WorldSetting {
    seed: bigint;
    spawnSetting: { biomeType: "default" | "userDefined"; userDefinedBiomeName: string; dimension: number };
    generatorType: "legacy" | "overworld" | "flat" | "nether" | "theend" | "void" | "undefined";
    globalGameType: GameType;
    isHardcore: boolean;
    difficulty: "peaceful" | "easy" | "normal" | "hard" | "count" | "unknown";
    defaultSpawnBlockPosition: Vec3;
    achievementDisabled: boolean;
    editorWorldType: "nonEditor" | "editorProject" | "editorTestLevel" | "editorRealmsUpload";
    isCreatedInEditor: boolean;
    isExportedFromEditor: boolean;
    dayCycleStopTime: number;
    educationEditionOffer: "none" | "restOfWorld" | "china_Deprecated";
    educationFeaturesEnable: boolean;
    educationProductId: string;
    rainLevel: number;
    lightningLevel: number;
    hasConfirmedPlatformLockedContent: boolean;
    multiplayerGameIntent: boolean;
    LANBroadcastInitent: boolean;
    XLBSetting: BroadcastSetting;
    platformBroadcastSetting: BroadcastSetting;
    commandsEnabled: boolean;
    texturePackRequired: boolean;
    gameRules: Record<string, GameRule>;
    experiments: { toggles: Record<string, boolean>; experimentsEverToggled: boolean };
    hasBonusChestEnabled: boolean;
    startWithMapEnabled: boolean;
    playerPermissions: "visitor" | "member" | "operator" | "custom";
    serverChunkTickRange: number;
    hasLockedBehaviorPack: boolean;
    hasLockedResourcePack: boolean;
    isFromWorldTemplate: boolean;
    isWorldTemplateOptionLocked: boolean;
    onlySpawnV1Villagers: boolean;
    personaDisabled: boolean;
    customSkinDisabled: boolean;
    emoteChatMuted: boolean;
    baseGameVersion: string;
    limitedWorldWidth: number;
    netherType: boolean;
    eduSharedUriResource: { name: string; link: string };
    overrideForceExperimentalGameplay: boolean | null;
    chatRestrictionLevel: "none" | "dropped" | "disabled";
    disablePlayerInteractions: boolean;
    serverEditorConnectionPolicy: "matchworldtype" | "editoronly" | "vanillaonly" | "mixed";
    allowAnonymousBlockDropsInEditorWorlds: boolean;
    gameMode: GameType;
    movement: { rewindHistorySize: number; serverAuthoritativeBlockBreaking: boolean };
    network: WorldNetwork;
  }
  export interface WorldLevel {
    levelid: string;
    worldName: string;
    templateContentIdentity: string;
    isTrial: boolean;
    levelCurrentTime: bigint;
    enchantmentSeed: number;
    worldTemplateId: Uuid;
  }
  export class World {
    playerList: Record<string, PlayerListEntry>;
    self: string | null;
    setting: WorldSetting;
    level: WorldLevel;
    gamerules: Record<string, GameRule>;
    blocks: Record<string, Record<string, NbtValue>>;
    playerPropertyData: NbtValue;
  }
)ts";

constexpr std::string_view kDeclarationsGlobals = R"ts(
declare function setTimeout(callback: (...args: any[]) => void, delay?: number, ...args: any[]): number;
declare function setInterval(callback: (...args: any[]) => void, delay?: number, ...args: any[]): number;
declare function clearTimeout(id: number): void;
declare function clearInterval(id: number): void;
declare const console: { log(...a: any[]): void; info(...a: any[]): void; warn(...a: any[]): void; error(...a: any[]): void };
)ts";

constexpr std::string_view kJsConfig = R"json({
  "compilerOptions": { "module": "esnext", "target": "es2022", "checkJs": true, "noEmit": true, "useUnknownInCatchVariables": false },
  "include": ["*.js", "*/**/*.js", "spyglass.d.ts"]
}
)json";

struct Subscription {
    int token{0};
    int id{0};
    std::uint32_t directions{0};
    JSValue callback{JS_UNDEFINED};
};

struct Timer {
    int id{0};
    std::chrono::steady_clock::time_point due;
    std::chrono::milliseconds interval{0};
    JSValue callback{JS_UNDEFINED};
    std::vector<JSValue> arguments;
};

struct Pending {
    JSValue resolve{JS_UNDEFINED};
    JSValue reject{JS_UNDEFINED};
};

struct Completion {
    enum class Kind { Run, Http, SocketOpen, SocketMessage };
    std::uint64_t module{0};
    int id{0};
    Kind kind{Kind::Run};
    int socket{0};
    bool closed{false};
    bool binary{false};
    std::string error;
    int number{0};
    std::string headers;
    std::string body;
};

struct Module {
    std::uint64_t uid{0};
    Side side{Side::Client};
    std::string name;
    std::filesystem::path root;
    std::filesystem::path entry;
    JSContext *ctx{nullptr};
    JSValue factory{JS_UNDEFINED};
    std::vector<Subscription> subscriptions;
    std::vector<Timer> timers;
    std::map<int, Pending> pending;
    std::vector<int> sockets;
};

struct Active {
    Verdict *verdict;
    Direction direction;
    std::int64_t serial;
};

JSRuntime *g_runtime = nullptr;
std::recursive_mutex g_mutex;
std::vector<std::unique_ptr<Module>> g_loaded;
std::set<std::string, std::less<>> g_enabled;
std::map<std::string, std::set<std::string, std::less<>>, std::less<>> g_grants;
std::map<std::string, std::string, std::less<>> g_failures;
std::vector<std::string> g_packet_names;
std::array<std::atomic<std::uint8_t>, kIdLimit> g_wanted{};
std::chrono::steady_clock::time_point g_deadline;
Active *g_active = nullptr;
Sender g_sender = nullptr;
std::int64_t g_serial = 0;
int g_next_token = 1;
int g_next_timer = 1;
int g_next_request = 1;
std::uint64_t g_next_uid = 1;

std::mutex g_done_mutex;
std::vector<Completion> g_done;

std::mutex g_log_mutex;
std::deque<LogLine> g_log;
std::atomic<std::uint64_t> g_log_serial{0};
std::atomic_bool g_attention{false};

void add_log(std::string module, std::string text, const bool error)
{
    const std::lock_guard lock{g_log_mutex};
    g_log.push_back({std::move(module), std::move(text), error});
    if (g_log.size() > kLogLimit) {
        g_log.pop_front();
    }
    g_log_serial.fetch_add(1, std::memory_order_relaxed);
}

std::string text_of(JSContext *ctx, JSValueConst value)
{
    const auto *const chars = JS_ToCString(ctx, value);
    if (chars == nullptr) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return "<unprintable>";
    }
    std::string text{chars};
    JS_FreeCString(ctx, chars);
    return text;
}

std::string exception_text(JSContext *ctx)
{
    const auto exception = JS_GetException(ctx);
    auto text = text_of(ctx, exception);
    if (JS_IsObject(exception)) {
        const auto stack = JS_GetPropertyStr(ctx, exception, "stack");
        if (!JS_IsUndefined(stack)) {
            text += '\n';
            text += text_of(ctx, stack);
        }
        JS_FreeValue(ctx, stack);
    }
    JS_FreeValue(ctx, exception);
    return text;
}

Module *module_of(JSContext *ctx)
{
    return static_cast<Module *>(JS_GetContextOpaque(ctx));
}

std::uint32_t wire_bit(const Side side, const Direction network)
{
    return (side == Side::Client) == (network == Direction::Outbound) ? 1U : 2U;
}

std::uint32_t network_bit(const Side side, const Direction network)
{
    return 1U << ((side == Side::Server ? 2U : 0U) + (network == Direction::Outbound ? 0U : 1U));
}

void rebuild_wanted()
{
    for (auto &entry : g_wanted) {
        entry.store(0, std::memory_order_relaxed);
    }
    for (const auto &module : g_loaded) {
        for (const auto &subscription : module->subscriptions) {
            for (const auto network : {Direction::Outbound, Direction::Inbound}) {
                if ((subscription.directions & wire_bit(module->side, network)) != 0) {
                    g_wanted[static_cast<std::size_t>(subscription.id)].fetch_or(
                        static_cast<std::uint8_t>(network_bit(module->side, network)), std::memory_order_relaxed);
                }
            }
        }
    }
}

bool valid_name(const std::string_view name)
{
    return !name.empty() && name.size() <= 64 && name != "." && name != ".." &&
           std::ranges::all_of(name, [](const char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                      c == '-' || c == '.';
           });
}

std::optional<std::filesystem::path> find_entry(const std::string_view name)
{
    if (!valid_name(name)) {
        return std::nullopt;
    }
    std::error_code ec;
    const auto file = modules_directory() / path_of(std::format("{}.js", name));
    if (std::filesystem::is_regular_file(file, ec)) {
        return file;
    }
    const auto index = modules_directory() / path_of(name) / "index.js";
    if (std::filesystem::is_regular_file(index, ec)) {
        return index;
    }
    return std::nullopt;
}

JSValue js_subscribe(JSContext *ctx, JSValueConst, const int argc, JSValueConst *argv)
{
    auto *const module = module_of(ctx);
    if (module == nullptr) {
        return JS_ThrowInternalError(ctx, "the module is unloaded");
    }
    std::int32_t id = 0;
    std::uint32_t directions = 0;
    if (argc < 3 || JS_ToInt32(ctx, &id, argv[0]) != 0 || !JS_IsFunction(ctx, argv[1]) ||
        JS_ToUint32(ctx, &directions, argv[2]) != 0) {
        return JS_ThrowTypeError(ctx, "subscribe expects a packet id, a function and a direction mask");
    }
    if (id < 0 || id >= kIdLimit) {
        return JS_ThrowRangeError(ctx, "packet id %d is out of range", id);
    }
    const auto token = g_next_token++;
    module->subscriptions.push_back({token, id, directions, JS_DupValue(ctx, argv[1])});
    rebuild_wanted();
    return JS_NewInt32(ctx, token);
}

JSValue js_unsubscribe(JSContext *ctx, JSValueConst, const int argc, JSValueConst *argv)
{
    auto *const module = module_of(ctx);
    std::int32_t token = 0;
    if (module == nullptr || argc < 1 || JS_ToInt32(ctx, &token, argv[0]) != 0) {
        return JS_UNDEFINED;
    }
    const auto found = std::ranges::find(module->subscriptions, token, &Subscription::token);
    if (found != module->subscriptions.end()) {
        JS_FreeValue(ctx, found->callback);
        module->subscriptions.erase(found);
        rebuild_wanted();
    }
    return JS_UNDEFINED;
}

JSValue js_set_factory(JSContext *ctx, JSValueConst, const int argc, JSValueConst *argv)
{
    auto *const module = module_of(ctx);
    if (module == nullptr || argc < 1) {
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx, module->factory);
    module->factory = JS_DupValue(ctx, argv[0]);
    return JS_UNDEFINED;
}

JSValue js_console(JSContext *ctx, JSValueConst, const int argc, JSValueConst *argv, const int magic)
{
    auto *const module = module_of(ctx);
    std::string line;
    for (int i = 0; i < argc; ++i) {
        if (i != 0) {
            line += ' ';
        }
        if (JS_IsObject(argv[i]) && !JS_IsFunction(ctx, argv[i])) {
            const auto json = JS_JSONStringify(ctx, argv[i], JS_UNDEFINED, JS_UNDEFINED);
            line += JS_IsException(json) ? text_of(ctx, argv[i]) : text_of(ctx, json);
            JS_FreeValue(ctx, json);
        }
        else {
            line += text_of(ctx, argv[i]);
        }
    }
    add_log(module == nullptr ? std::string{} : module->name, std::move(line), magic >= 2);
    return JS_UNDEFINED;
}

bool is_stale(JSContext *ctx, JSValueConst *data)
{
    std::int64_t serial = 0;
    JS_ToInt64(ctx, &serial, data[0]);
    return g_active == nullptr || g_active->serial != serial;
}

JSValue js_cancel(JSContext *ctx, JSValueConst, int, JSValueConst *, int, JSValueConst *data)
{
    if (is_stale(ctx, data)) {
        return JS_ThrowInternalError(ctx, "cancel was called after the packet was handed on");
    }
    g_active->verdict->cancelled = true;
    return JS_UNDEFINED;
}

std::optional<std::string> bytes_of(JSContext *ctx, JSValueConst value)
{
    std::size_t offset = 0;
    std::size_t length = 0;
    std::size_t element = 0;
    auto buffer = JS_DupValue(ctx, value);
    if (JS_GetTypedArrayType(value) >= 0) {
        JS_FreeValue(ctx, buffer);
        buffer = JS_GetTypedArrayBuffer(ctx, value, &offset, &length, &element);
        if (JS_IsException(buffer)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return std::nullopt;
        }
    }
    std::size_t size = 0;
    const auto *const data = JS_GetArrayBuffer(ctx, &size, buffer);
    if (data == nullptr) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, buffer);
        return std::nullopt;
    }
    std::string bytes{reinterpret_cast<const char *>(data) + offset, element == 0 ? size : length};
    JS_FreeValue(ctx, buffer);
    return bytes;
}

JSValue js_override(JSContext *ctx, JSValueConst, const int argc, JSValueConst *argv, int, JSValueConst *held)
{
    if (is_stale(ctx, held)) {
        return JS_ThrowInternalError(ctx, "override was called after the packet was handed on");
    }
    auto bytes = argc < 1 ? std::nullopt : bytes_of(ctx, argv[0]);
    if (!bytes) {
        return JS_ThrowTypeError(ctx, "override expects the new packet body as a Uint8Array or an ArrayBuffer");
    }
    g_active->verdict->body = std::move(bytes);
    return JS_UNDEFINED;
}

bool granted(const std::string &module, const std::string_view permission)
{
    const auto found = g_grants.find(module);
    return found != g_grants.end() &&
           (found->second.contains(permission) || (permission == "fs" && found->second.contains("fs.all")));
}

JSValue denied(JSContext *ctx, const std::string &module, const char *permission)
{
    return JS_ThrowInternalError(ctx, "module '%s' has no '%s' permission; run: !!module grant %s %s", module.c_str(),
                                 permission, module.c_str(), permission);
}

void free_timer(JSContext *ctx, Timer &timer)
{
    JS_FreeValue(ctx, timer.callback);
    for (const auto &argument : timer.arguments) {
        JS_FreeValue(ctx, argument);
    }
    timer.arguments.clear();
}

JSValue js_timer(JSContext *ctx, JSValueConst, const int argc, JSValueConst *argv, const int repeat)
{
    auto *const module = module_of(ctx);
    if (module == nullptr) {
        return JS_ThrowInternalError(ctx, "the module is unloaded");
    }
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "expected a function");
    }
    if (module->timers.size() >= kTimerLimit) {
        return JS_ThrowRangeError(ctx, "too many timers");
    }
    double delay = 0;
    if (argc > 1) {
        JS_ToFloat64(ctx, &delay, argv[1]);
    }
    const std::chrono::milliseconds wait{static_cast<std::int64_t>(delay >= 1 ? std::min(delay, 2147483647.0) : 1)};
    Timer timer{.id = g_next_timer++,
                .due = std::chrono::steady_clock::now() + wait,
                .interval = repeat != 0 ? wait : std::chrono::milliseconds{0},
                .callback = JS_DupValue(ctx, argv[0])};
    for (int i = 2; i < argc; ++i) {
        timer.arguments.push_back(JS_DupValue(ctx, argv[i]));
    }
    const auto id = timer.id;
    module->timers.push_back(std::move(timer));
    return JS_NewInt32(ctx, id);
}

JSValue js_clear_timer(JSContext *ctx, JSValueConst, const int argc, JSValueConst *argv)
{
    auto *const module = module_of(ctx);
    std::int32_t id = 0;
    if (module == nullptr || argc < 1 || JS_ToInt32(ctx, &id, argv[0]) != 0) {
        return JS_UNDEFINED;
    }
    const auto found = std::ranges::find(module->timers, id, &Timer::id);
    if (found != module->timers.end()) {
        free_timer(ctx, *found);
        module->timers.erase(found);
    }
    return JS_UNDEFINED;
}

JSValue js_fs(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    auto *const module = module_of(ctx);
    if (module == nullptr) {
        return JS_ThrowInternalError(ctx, "the module is unloaded");
    }
    if (!granted(module->name, "fs")) {
        return denied(ctx, module->name, "fs");
    }
    std::int32_t operation = 0;
    if (argc < 2 || JS_ToInt32(ctx, &operation, argv[0]) != 0) {
        return JS_ThrowTypeError(ctx, "expected an operation and a path");
    }
    ++argv;
    --argc;

    const auto root = output_directory() / "module-data" / path_of(module->name);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    const auto requested = text_of(ctx, argv[0]);
    auto target = path_of(requested);
    if (target.is_relative()) {
        target = root / target;
    }
    if (!granted(module->name, "fs.all")) {
        const auto inside = std::filesystem::weakly_canonical(target, ec)
                                .lexically_relative(std::filesystem::weakly_canonical(root, ec));
        if (ec || inside.empty() || *inside.begin() == "..") {
            return JS_ThrowRangeError(ctx, "'%s' is outside the data folder of '%s'; grant fs.all to lift this",
                                      requested.c_str(), module->name.c_str());
        }
    }

    const auto fail = [&](const char *action) {
        return JS_ThrowInternalError(ctx, "cannot %s '%s'%s%s", action, requested.c_str(), ec ? ": " : "",
                                     ec ? ec.message().c_str() : "");
    };

    switch (operation) {
    case 0:
    case 1: {
        const auto size = std::filesystem::is_regular_file(target, ec) ? std::filesystem::file_size(target, ec) : 0;
        std::ifstream file{target, std::ios::binary};
        if (ec || !file || size > kFileLimit) {
            return size > kFileLimit ? JS_ThrowRangeError(ctx, "'%s' is larger than 16 MB", requested.c_str())
                                     : fail("read");
        }
        std::ostringstream contents;
        contents << file.rdbuf();
        const auto text = contents.str();
        return operation == 0 ? JS_NewStringLen(ctx, text.data(), text.size())
                              : JS_NewUint8ArrayCopy(ctx, reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
    }
    case 2:
    case 3:
    case 4: {
        if (argc < 2) {
            return JS_ThrowTypeError(ctx, "expected the data to write");
        }
        const auto bytes = operation == 3 ? bytes_of(ctx, argv[1]) : std::optional{text_of(ctx, argv[1])};
        if (!bytes) {
            return JS_ThrowTypeError(ctx, "expected a Uint8Array or an ArrayBuffer");
        }
        std::filesystem::create_directories(target.parent_path(), ec);
        std::ofstream file{target, std::ios::binary | (operation == 4 ? std::ios::app : std::ios::trunc)};
        file.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
        return file ? JS_UNDEFINED : fail("write");
    }
    case 5:
        return JS_NewBool(ctx, std::filesystem::exists(target, ec));
    case 6: {
        std::vector<std::string> names;
        for (const auto &entry : std::filesystem::directory_iterator{target, ec}) {
            names.push_back(path_text(entry.path().filename()));
        }
        if (ec) {
            return fail("list");
        }
        std::ranges::sort(names);
        const auto list = JS_NewArray(ctx);
        for (std::uint32_t i = 0; i < names.size(); ++i) {
            JS_SetPropertyUint32(ctx, list, i, JS_NewStringLen(ctx, names[i].data(), names[i].size()));
        }
        return list;
    }
    case 7:
        std::filesystem::create_directories(target, ec);
        return ec ? fail("create") : JS_UNDEFINED;
    case 8:
        if (target.lexically_normal() == root.lexically_normal()) {
            return JS_ThrowRangeError(ctx, "the data folder itself cannot be removed");
        }
        std::filesystem::remove_all(target, ec);
        return ec ? fail("remove") : JS_UNDEFINED;
    default: {
        const auto status = std::filesystem::status(target, ec);
        if (ec || !std::filesystem::exists(status)) {
            return JS_NULL;
        }
        const auto info = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, info, "isFile", JS_NewBool(ctx, std::filesystem::is_regular_file(status)));
        JS_SetPropertyStr(ctx, info, "isDirectory", JS_NewBool(ctx, std::filesystem::is_directory(status)));
        JS_SetPropertyStr(
            ctx, info, "size",
            JS_NewInt64(ctx, std::filesystem::is_regular_file(status)
                                 ? static_cast<std::int64_t>(std::filesystem::file_size(target, ec))
                                 : 0));
        return info;
    }
    }
}

std::vector<std::string> strings_of(JSContext *ctx, JSValueConst array)
{
    std::vector<std::string> strings;
    std::uint32_t length = 0;
    const auto value = JS_GetPropertyStr(ctx, array, "length");
    JS_ToUint32(ctx, &length, value);
    JS_FreeValue(ctx, value);
    for (std::uint32_t i = 0; i < length; ++i) {
        const auto item = JS_GetPropertyUint32(ctx, array, i);
        strings.push_back(text_of(ctx, item));
        JS_FreeValue(ctx, item);
    }
    return strings;
}

JSValue start_work(JSContext *ctx, Module &module, std::function<Completion()> work)
{
    if (module.pending.size() >= kPendingLimit) {
        return JS_ThrowRangeError(ctx, "too many requests are still running");
    }
    std::array<JSValue, 2> functions{};
    const auto promise = JS_NewPromiseCapability(ctx, functions.data());
    if (JS_IsException(promise)) {
        return promise;
    }
    const auto id = g_next_request++;
    module.pending.emplace(id, Pending{functions[0], functions[1]});
    std::thread{[uid = module.uid, id, work = std::move(work)] {
        auto done = work();
        done.module = uid;
        done.id = id;
        const std::lock_guard lock{g_done_mutex};
        g_done.push_back(std::move(done));
    }}.detach();
    return promise;
}

JSValue js_send(JSContext *ctx, JSValueConst, const int argc, JSValueConst *argv)
{
    auto *const module = module_of(ctx);
    std::int32_t id = 0;
    std::int32_t wire = 0;
    std::int32_t player = -1;
    auto bytes = argc < 3 ? std::nullopt : bytes_of(ctx, argv[2]);
    if (module == nullptr || argc < 3 || JS_ToInt32(ctx, &id, argv[0]) != 0 || JS_ToInt32(ctx, &wire, argv[1]) != 0 ||
        !bytes || (argc > 3 && JS_ToInt32(ctx, &player, argv[3]) != 0)) {
        return JS_ThrowTypeError(ctx, "send expects a packet id, a direction, the packet body as bytes and a player");
    }
    if (id < 1 || id >= kIdLimit) {
        return JS_ThrowRangeError(ctx, "packet id %d is out of range", id);
    }
    const auto network = (module->side == Side::Client) == (wire == 1) ? Direction::Outbound : Direction::Inbound;
    if (g_sender == nullptr || !g_sender(module->side, network, id, *bytes, player)) {
        return JS_ThrowInternalError(ctx,
                                     "this packet cannot be sent: the hook for it is missing, the packet id is "
                                     "unknown, there is no such player, or this direction is not supported");
    }
    return JS_UNDEFINED;
}

JSValue js_request(JSContext *ctx, JSValueConst, const int argc, JSValueConst *argv)
{
    auto *const module = module_of(ctx);
    if (module == nullptr) {
        return JS_ThrowInternalError(ctx, "the module is unloaded");
    }
    if (!granted(module->name, "net")) {
        return denied(ctx, module->name, "net");
    }
    std::int32_t timeout = 0;
    if (argc < 5 || !JS_IsObject(argv[2]) || JS_ToInt32(ctx, &timeout, argv[4]) != 0) {
        return JS_ThrowTypeError(ctx, "request expects a url, a method, headers, a body and a timeout");
    }
    HttpRequest request{.method = text_of(ctx, argv[1]), .url = text_of(ctx, argv[0]), .body = text_of(ctx, argv[3])};
    if (!request.url.starts_with("http://") && !request.url.starts_with("https://")) {
        return JS_ThrowTypeError(ctx, "only http:// and https:// URLs can be fetched");
    }
    const auto headers = strings_of(ctx, argv[2]);
    for (std::size_t i = 0; i + 1 < headers.size(); i += 2) {
        request.headers.emplace_back(headers[i], headers[i + 1]);
    }
    return start_work(ctx, *module, [request = std::move(request), timeout] {
        Completion done;
        done.kind = Completion::Kind::Http;
        try {
            auto response = http_request(request, timeout);
            done.number = response.status;
            done.headers = std::move(response.headers);
            done.body = std::move(response.body);
        }
        catch (const std::exception &e) {
            done.error = e.what();
        }
        return done;
    });
}

JSValue js_ws_open(JSContext *ctx, JSValueConst, const int argc, JSValueConst *argv)
{
    auto *const module = module_of(ctx);
    if (module == nullptr) {
        return JS_ThrowInternalError(ctx, "the module is unloaded");
    }
    if (!granted(module->name, "net")) {
        return denied(ctx, module->name, "net");
    }
    std::int32_t timeout = 0;
    if (argc < 3 || !JS_IsObject(argv[1]) || JS_ToInt32(ctx, &timeout, argv[2]) != 0) {
        return JS_ThrowTypeError(ctx, "wsOpen expects a url, headers and a timeout");
    }
    auto url = text_of(ctx, argv[0]);
    if (!url.starts_with("ws://") && !url.starts_with("wss://")) {
        return JS_ThrowTypeError(ctx, "only ws:// and wss:// URLs can be opened");
    }
    std::vector<std::pair<std::string, std::string>> headers;
    const auto words = strings_of(ctx, argv[1]);
    for (std::size_t i = 0; i + 1 < words.size(); i += 2) {
        headers.emplace_back(words[i], words[i + 1]);
    }
    return start_work(ctx, *module, [url = std::move(url), headers = std::move(headers), timeout] {
        Completion done;
        done.kind = Completion::Kind::SocketOpen;
        try {
            done.number = websocket_open(url, headers, timeout);
        }
        catch (const std::exception &e) {
            done.error = e.what();
        }
        return done;
    });
}

JSValue js_ws_receive(JSContext *ctx, JSValueConst, const int argc, JSValueConst *argv)
{
    auto *const module = module_of(ctx);
    if (module == nullptr) {
        return JS_ThrowInternalError(ctx, "the module is unloaded");
    }
    if (!granted(module->name, "net")) {
        return denied(ctx, module->name, "net");
    }
    std::int32_t socket = 0;
    if (argc < 1 || JS_ToInt32(ctx, &socket, argv[0]) != 0) {
        return JS_ThrowTypeError(ctx, "wsReceive expects a WebSocket id");
    }
    return start_work(ctx, *module, [socket] {
        Completion done;
        done.kind = Completion::Kind::SocketMessage;
        done.socket = socket;
        try {
            auto message = websocket_receive(socket);
            done.closed = message.closed;
            done.binary = message.binary;
            done.number = message.code;
            done.body = std::move(message.data);
        }
        catch (const std::exception &e) {
            done.error = e.what();
        }
        return done;
    });
}

JSValue js_ws_send(JSContext *ctx, JSValueConst, const int argc, JSValueConst *argv)
{
    auto *const module = module_of(ctx);
    if (module == nullptr) {
        return JS_ThrowInternalError(ctx, "the module is unloaded");
    }
    if (!granted(module->name, "net")) {
        return denied(ctx, module->name, "net");
    }
    std::int32_t socket = 0;
    if (argc < 3 || JS_ToInt32(ctx, &socket, argv[0]) != 0) {
        return JS_ThrowTypeError(ctx, "wsSend expects a WebSocket id, the data and whether it is binary");
    }
    const auto binary = JS_ToBool(ctx, argv[2]) > 0;
    const auto data = binary ? bytes_of(ctx, argv[1]) : std::optional{text_of(ctx, argv[1])};
    if (!data) {
        return JS_ThrowTypeError(ctx, "a binary message is a Uint8Array or an ArrayBuffer");
    }
    try {
        websocket_send(socket, *data, binary);
    }
    catch (const std::exception &e) {
        return JS_ThrowInternalError(ctx, "%s", e.what());
    }
    return JS_UNDEFINED;
}

JSValue js_ws_close(JSContext *ctx, JSValueConst, const int argc, JSValueConst *argv)
{
    auto *const module = module_of(ctx);
    if (module == nullptr) {
        return JS_ThrowInternalError(ctx, "the module is unloaded");
    }
    if (!granted(module->name, "net")) {
        return denied(ctx, module->name, "net");
    }
    std::int32_t socket = 0;
    std::int32_t code = 1000;
    if (argc < 3 || JS_ToInt32(ctx, &socket, argv[0]) != 0 || JS_ToInt32(ctx, &code, argv[1]) != 0) {
        return JS_ThrowTypeError(ctx, "wsClose expects a WebSocket id, a code and a reason");
    }
    try {
        websocket_close(socket, code, text_of(ctx, argv[2]));
    }
    catch (const std::exception &e) {
        return JS_ThrowInternalError(ctx, "%s", e.what());
    }
    return JS_UNDEFINED;
}

JSValue js_run(JSContext *ctx, JSValueConst, const int argc, JSValueConst *argv)
{
    auto *const module = module_of(ctx);
    if (module == nullptr) {
        return JS_ThrowInternalError(ctx, "the module is unloaded");
    }
    if (!granted(module->name, "process")) {
        return denied(ctx, module->name, "process");
    }
    std::int32_t timeout = 0;
    if (argc < 4 || !JS_IsObject(argv[1]) || JS_ToInt32(ctx, &timeout, argv[3]) != 0) {
        return JS_ThrowTypeError(ctx, "run expects a file, arguments, a directory and a timeout");
    }
    auto file = text_of(ctx, argv[0]);
    auto arguments = strings_of(ctx, argv[1]);
    const auto directory = text_of(ctx, argv[2]);
    auto cwd = directory.empty() ? modules_directory() : path_of(directory);
    return start_work(ctx, *module,
                      [file = std::move(file), arguments = std::move(arguments), cwd = std::move(cwd), timeout] {
                          Completion done;
                          try {
                              auto result = run_process(file, arguments, cwd, timeout);
                              done.number = result.code;
                              done.body = std::move(result.output);
                          }
                          catch (const std::exception &e) {
                              done.error = e.what();
                          }
                          return done;
                      });
}

char *normalize_module(JSContext *ctx, const char *base, const char *name, void *)
{
    const std::string_view requested{name};
    if (requested == "spyglass") {
        return js_strdup(ctx, name);
    }
    auto *const module = module_of(ctx);
    if (module == nullptr || (!requested.starts_with("./") && !requested.starts_with("../"))) {
        JS_ThrowReferenceError(ctx, "cannot import '%s': only \"spyglass\" and relative paths are supported", name);
        return nullptr;
    }

    auto target = (path_of(base).parent_path() / path_of(requested)).lexically_normal();
    if (!target.has_extension()) {
        target += ".js";
    }
    const auto inside = target.lexically_relative(module->root);
    if (inside.empty() || *inside.begin() == "..") {
        JS_ThrowReferenceError(ctx, "cannot import '%s': it is outside the module folder", name);
        return nullptr;
    }
    return js_strdup(ctx, path_text(target).c_str());
}

std::vector<std::pair<std::string, std::size_t>> exported_packets()
{
    std::vector<std::pair<std::string, std::size_t>> packets;
    std::set<std::string> seen;
    for (std::size_t id = 1; id < g_packet_names.size(); ++id) {
        const auto &packet = g_packet_names[id];
        const auto word = [](const char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        };
        if (!packet.empty() && !(packet[0] >= '0' && packet[0] <= '9') && std::ranges::all_of(packet, word) &&
            seen.insert(packet).second) {
            packets.emplace_back(packet, id);
        }
    }
    return packets;
}

JSModuleDef *load_module_source(JSContext *ctx, const char *name, void *)
{
    std::string source;
    if (std::string_view{name} == "spyglass") {
        source = std::string{kPreludeHead};
        std::string exports;
        std::string classes;
        for (const auto &[packet, id] : exported_packets()) {
            auto key = packet;
            key[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])));
            exports += std::format("  {}: {},\n", key, id);
            source += std::format("export class {} extends Packet {{ static id = {}; }}\n", packet, id);
            classes += std::format("[{}, {}], ", id, packet);
        }
        source += std::format("export const Packets = Object.freeze({{\n{}}});\n", exports);
        source += std::format("const classes = new Map([{}]);\n", classes);
        source += kPreludeTail;
    }
    else {
        std::ifstream file{path_of(name), std::ios::binary};
        if (!file) {
            JS_ThrowReferenceError(ctx, "cannot read '%s'", name);
            return nullptr;
        }
        std::ostringstream contents;
        contents << file.rdbuf();
        source = contents.str();
    }

    const auto compiled =
        JS_Eval(ctx, source.data(), source.size(), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) {
        return nullptr;
    }
    auto *const definition = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(compiled));
    JS_FreeValue(ctx, compiled);
    return definition;
}

void unload(const std::string_view name)
{
    const auto found = std::ranges::find_if(g_loaded, [&](const auto &module) { return module->name == name; });
    if (found == g_loaded.end()) {
        return;
    }
    auto &module = **found;
    for (auto &subscription : module.subscriptions) {
        JS_FreeValue(module.ctx, subscription.callback);
    }
    module.subscriptions.clear();
    for (auto &timer : module.timers) {
        free_timer(module.ctx, timer);
    }
    for (const auto &[id, pending] : module.pending) {
        JS_FreeValue(module.ctx, pending.resolve);
        JS_FreeValue(module.ctx, pending.reject);
    }
    for (const auto socket : module.sockets) {
        try {
            websocket_close(socket, 1001, "");
        }
        catch (const std::exception &) {
        }
    }
    JS_FreeValue(module.ctx, module.factory);
    JS_SetContextOpaque(module.ctx, nullptr);
    JS_FreeContext(module.ctx);
    g_loaded.erase(found);
    rebuild_wanted();
}

std::string load(const std::string &name)
{
    const auto entry = find_entry(name);
    if (!entry) {
        return std::format("no module named '{}' in {}", name, path_text(modules_directory()));
    }

    std::ifstream file{*entry, std::ios::binary};
    if (!file) {
        return std::format("cannot read {}", path_text(*entry));
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    const auto source = contents.str();

    auto &module = *g_loaded.emplace_back(std::make_unique<Module>());
    module.uid = g_next_uid++;
    module.side = name.ends_with(".server") ? Side::Server : Side::Client;
    module.name = name;
    module.entry = *entry;
    module.root = entry->parent_path();
    module.ctx = JS_NewContext(g_runtime);
    JS_SetContextOpaque(module.ctx, &module);

    const auto global = JS_GetGlobalObject(module.ctx);
    const auto native = JS_NewObject(module.ctx);
    JS_SetPropertyStr(module.ctx, native, "subscribe", JS_NewCFunction(module.ctx, js_subscribe, "subscribe", 3));
    JS_SetPropertyStr(module.ctx, native, "unsubscribe", JS_NewCFunction(module.ctx, js_unsubscribe, "unsubscribe", 1));
    JS_SetPropertyStr(module.ctx, native, "setFactory", JS_NewCFunction(module.ctx, js_set_factory, "setFactory", 1));
    JS_SetPropertyStr(module.ctx, native, "fs", JS_NewCFunction(module.ctx, js_fs, "fs", 3));
    JS_SetPropertyStr(module.ctx, native, "send", JS_NewCFunction(module.ctx, js_send, "send", 3));
    JS_SetPropertyStr(module.ctx, native, "side",
                      JS_NewString(module.ctx, module.side == Side::Client ? "client" : "server"));
    JS_SetPropertyStr(module.ctx, native, "request", JS_NewCFunction(module.ctx, js_request, "request", 5));
    JS_SetPropertyStr(module.ctx, native, "run", JS_NewCFunction(module.ctx, js_run, "run", 4));
    JS_SetPropertyStr(module.ctx, native, "wsOpen", JS_NewCFunction(module.ctx, js_ws_open, "wsOpen", 3));
    JS_SetPropertyStr(module.ctx, native, "wsReceive", JS_NewCFunction(module.ctx, js_ws_receive, "wsReceive", 1));
    JS_SetPropertyStr(module.ctx, native, "wsSend", JS_NewCFunction(module.ctx, js_ws_send, "wsSend", 3));
    JS_SetPropertyStr(module.ctx, native, "wsClose", JS_NewCFunction(module.ctx, js_ws_close, "wsClose", 3));
    JS_SetPropertyStr(module.ctx, native, "platform",
#ifdef _WIN32
                      JS_NewString(module.ctx, "windows")
#else
                      JS_NewString(module.ctx, "android")
#endif
    );
    JS_SetPropertyStr(module.ctx, global, "__spyglass", native);
    JS_SetPropertyStr(module.ctx, global, "setTimeout",
                      JS_NewCFunctionMagic(module.ctx, js_timer, "setTimeout", 2, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(module.ctx, global, "setInterval",
                      JS_NewCFunctionMagic(module.ctx, js_timer, "setInterval", 2, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(module.ctx, global, "clearTimeout", JS_NewCFunction(module.ctx, js_clear_timer, "clearTimeout", 1));
    JS_SetPropertyStr(module.ctx, global, "clearInterval",
                      JS_NewCFunction(module.ctx, js_clear_timer, "clearInterval", 1));

    const auto console = JS_NewObject(module.ctx);
    const std::array<const char *, 4> levels{"log", "info", "warn", "error"};
    for (std::size_t level = 0; level < levels.size(); ++level) {
        JS_SetPropertyStr(module.ctx, console, levels[level],
                          JS_NewCFunctionMagic(module.ctx, js_console, levels[level], 0, JS_CFUNC_generic_magic,
                                               static_cast<int>(level)));
    }
    JS_SetPropertyStr(module.ctx, global, "console", console);
    JS_FreeValue(module.ctx, global);

    JS_UpdateStackTop(g_runtime);
    g_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    const auto result =
        JS_Eval(module.ctx, source.data(), source.size(), path_text(*entry).c_str(), JS_EVAL_TYPE_MODULE);

    std::string error;
    if (JS_IsException(result)) {
        error = exception_text(module.ctx);
    }
    else {
        JSContext *job_ctx = nullptr;
        for (auto status = JS_ExecutePendingJob(g_runtime, &job_ctx); status != 0;
             status = JS_ExecutePendingJob(g_runtime, &job_ctx)) {
            if (status < 0) {
                add_log(name, exception_text(job_ctx), true);
            }
        }
        if (JS_PromiseState(module.ctx, result) == JS_PROMISE_REJECTED) {
            const auto reason = JS_PromiseResult(module.ctx, result);
            error = text_of(module.ctx, reason);
            if (JS_IsObject(reason)) {
                const auto stack = JS_GetPropertyStr(module.ctx, reason, "stack");
                if (!JS_IsUndefined(stack)) {
                    error += '\n';
                    error += text_of(module.ctx, stack);
                }
                JS_FreeValue(module.ctx, stack);
            }
            JS_FreeValue(module.ctx, reason);
        }
    }
    JS_FreeValue(module.ctx, result);

    if (!error.empty()) {
        unload(name);
    }
    return error;
}

void save_config()
{
    auto config = nlohmann::json::object();
    config["enabled"] = g_enabled;
    config["permissions"] = g_grants;
    std::ofstream out{modules_directory() / "modules.json", std::ios::binary | std::ios::trunc};
    out << config.dump(2) << '\n';
}

std::vector<std::string> discover()
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator{modules_directory(), ec}) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".js") {
            names.push_back(path_text(entry.path().stem()));
        }
        else if (entry.is_directory(ec) && std::filesystem::is_regular_file(entry.path() / "index.js", ec)) {
            names.push_back(path_text(entry.path().filename()));
        }
    }
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());
    std::erase_if(names, [](const std::string &name) { return !valid_name(name); });
    return names;
}

}  // namespace

std::filesystem::path modules_directory()
{
    static const auto directory = [] {
        auto path = output_directory() / "modules";
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return path;
    }();
    return directory;
}

void set_sender(const Sender sender)
{
    g_sender = sender;
}

void start(std::vector<std::string> packet_names)
{
    const std::lock_guard lock{g_mutex};
    if (g_runtime != nullptr) {
        return;
    }
    g_packet_names = std::move(packet_names);

    g_runtime = JS_NewRuntime();
    JS_SetMemoryLimit(g_runtime, 128U * 1024U * 1024U);
    JS_SetMaxStackSize(g_runtime, 256U * 1024U);
    JS_SetInterruptHandler(
        g_runtime, [](JSRuntime *, void *) { return std::chrono::steady_clock::now() > g_deadline ? 1 : 0; }, nullptr);
    JS_SetModuleLoaderFunc(g_runtime, normalize_module, load_module_source, nullptr);

    try {
        std::ifstream file{modules_directory() / "modules.json", std::ios::binary};
        if (file) {
            const auto config = nlohmann::json::parse(file);
            for (const auto &name : config.value("enabled", std::vector<std::string>{})) {
                g_enabled.insert(name);
            }
            const auto permissions = config.value("permissions", nlohmann::json::object());
            for (const auto &[name, list] : permissions.items()) {
                for (const auto &permission : list.get<std::vector<std::string>>()) {
                    if (std::ranges::find(kPermissions, permission) != kPermissions.end()) {
                        g_grants[name].insert(permission);
                    }
                }
            }
        }
    }
    catch (const std::exception &e) {
        add_log("", std::format("modules.json is unreadable: {}", e.what()), true);
    }

    for (const auto &name : g_enabled) {
        if (const auto error = load(name); !error.empty()) {
            g_failures[name] = error;
            add_log(name, error, true);
        }
    }
}

bool wants(const Side side, const Direction direction, const int id)
{
    if (id < 0 || id >= kIdLimit) {
        return false;
    }
    return (g_wanted[static_cast<std::size_t>(id)].load(std::memory_order_relaxed) & network_bit(side, direction)) != 0;
}

Verdict dispatch(const Event &event, const std::function<nlohmann::ordered_json()> &fields)
try {
    Verdict verdict;
    if (!wants(event.side, event.direction, event.id)) {
        return verdict;
    }

    const std::lock_guard lock{g_mutex};
    JS_UpdateStackTop(g_runtime);

    const auto bit = wire_bit(event.side, event.direction);
    std::vector<std::pair<std::string, int>> targets;
    for (const auto &module : g_loaded) {
        if (module->side != event.side) {
            continue;
        }
        for (const auto &subscription : module->subscriptions) {
            if (subscription.id == event.id && (subscription.directions & bit) != 0) {
                targets.emplace_back(module->name, subscription.token);
            }
        }
    }

    std::string fields_text{"null"};
    try {
        fields_text = fields().dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
    }
    catch (const std::exception &) {
    }

    Active active{&verdict, event.direction, ++g_serial};
    g_active = &active;
    for (const auto &[name, token] : targets) {
        const auto owner = std::ranges::find_if(g_loaded, [&](const auto &module) { return module->name == name; });
        if (owner == g_loaded.end()) {
            continue;
        }
        auto &module = **owner;
        const auto subscription = std::ranges::find(module.subscriptions, token, &Subscription::token);
        if (subscription == module.subscriptions.end() || JS_IsUndefined(module.factory)) {
            continue;
        }

        auto *const ctx = module.ctx;
        g_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
        const auto parsed = JS_ParseJSON(ctx, fields_text.data(), fields_text.size(), "<fields>");
        auto factory_args = std::array{JS_NewInt32(ctx, event.id), JS_IsException(parsed) ? JS_NULL : parsed};
        if (JS_IsException(parsed)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        const auto packet = JS_Call(ctx, module.factory, JS_UNDEFINED, 2, factory_args.data());
        JS_FreeValue(ctx, factory_args[1]);
        if (JS_IsException(packet)) {
            add_log(name, exception_text(ctx), true);
            continue;
        }

        const auto info = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, info, "direction", JS_NewString(ctx, bit == 1U ? "c2s" : "s2c"));
        JS_SetPropertyStr(ctx, info, "side", JS_NewString(ctx, event.side == Side::Client ? "client" : "server"));
        JS_SetPropertyStr(ctx, info, "player", event.player < 0 ? JS_NULL : JS_NewInt32(ctx, event.player));
        JS_SetPropertyStr(ctx, info, "id", JS_NewInt32(ctx, event.id));
        JS_SetPropertyStr(ctx, info, "name", JS_NewStringLen(ctx, event.name.data(), event.name.size()));
        JS_SetPropertyStr(ctx, info, "subClientId", JS_NewInt32(ctx, event.sub_id));
        const std::string_view current = verdict.body ? std::string_view{*verdict.body} : event.body;
        JS_SetPropertyStr(ctx, info, "size", JS_NewInt64(ctx, static_cast<std::int64_t>(current.size())));
        JS_SetPropertyStr(ctx, info, "cancelled", JS_NewBool(ctx, verdict.cancelled));
        JS_SetPropertyStr(
            ctx, info, "body",
            JS_NewUint8ArrayCopy(ctx, reinterpret_cast<const std::uint8_t *>(current.data()), current.size()));

        auto serial = JS_NewInt64(ctx, active.serial);
        auto arguments = std::array{packet, info, JS_NewCFunctionData2(ctx, js_cancel, "cancel", 0, 0, 1, &serial),
                                    JS_NewCFunctionData2(ctx, js_override, "override", 1, 0, 1, &serial)};
        g_deadline = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds{100 + static_cast<std::int64_t>(std::min<std::size_t>(current.size() / 256, 1900))};
        const auto outcome = JS_Call(ctx, subscription->callback, JS_UNDEFINED, 4, arguments.data());
        if (JS_IsException(outcome)) {
            add_log(name, exception_text(ctx), true);
        }
        JS_FreeValue(ctx, outcome);
        for (const auto &argument : arguments) {
            JS_FreeValue(ctx, argument);
        }
    }
    g_active = nullptr;

    g_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
    JSContext *job_ctx = nullptr;
    for (auto status = JS_ExecutePendingJob(g_runtime, &job_ctx); status != 0;
         status = JS_ExecutePendingJob(g_runtime, &job_ctx)) {
        if (status < 0) {
            add_log("", exception_text(job_ctx), true);
        }
    }
    return verdict;
}
catch (const std::exception &e) {
    g_active = nullptr;
    add_log("", std::format("packet dispatch failed: {}", e.what()), true);
    return {};
}

void tick()
{
    std::unique_lock lock{g_mutex, std::try_to_lock};
    if (!lock.owns_lock() || g_runtime == nullptr) {
        return;
    }
    std::vector<Completion> done;
    {
        const std::lock_guard guard{g_done_mutex};
        done.swap(g_done);
    }
    const auto now = std::chrono::steady_clock::now();
    const auto has_timers = std::ranges::any_of(g_loaded, [](const auto &module) { return !module->timers.empty(); });
    if (done.empty() && !has_timers) {
        return;
    }
    JS_UpdateStackTop(g_runtime);

    for (auto &completion : done) {
        const auto owner =
            std::ranges::find(g_loaded, completion.module, [](const auto &module) { return module->uid; });
        if (owner == g_loaded.end()) {
            continue;
        }
        auto &module = **owner;
        const auto pending = module.pending.find(completion.id);
        if (pending == module.pending.end()) {
            continue;
        }
        auto *const ctx = module.ctx;
        const auto ok = completion.error.empty();
        auto value = JS_UNDEFINED;
        if (!ok) {
            value = JS_NewError(ctx);
            JS_SetPropertyStr(ctx, value, "message", JS_NewStringLen(ctx, completion.error.data(), completion.error.size()));
        }
        else if (completion.kind == Completion::Kind::SocketOpen) {
            module.sockets.push_back(completion.number);
            value = JS_NewInt32(ctx, completion.number);
        }
        else if (completion.kind == Completion::Kind::SocketMessage) {
            if (completion.closed) {
                std::erase(module.sockets, completion.socket);
            }
            value = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, value, "closed", JS_NewBool(ctx, completion.closed));
            JS_SetPropertyStr(ctx, value, "binary", JS_NewBool(ctx, completion.binary));
            JS_SetPropertyStr(ctx, value, "code", JS_NewInt32(ctx, completion.number));
            JS_SetPropertyStr(ctx, value, "data",
                              completion.binary ? JS_NewArrayBufferCopy(
                                                      ctx, reinterpret_cast<const std::uint8_t *>(completion.body.data()),
                                                      completion.body.size())
                                                : JS_NewStringLen(ctx, completion.body.data(), completion.body.size()));
        }
        else if (completion.kind == Completion::Kind::Http) {
            value = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, value, "status", JS_NewInt32(ctx, completion.number));
            JS_SetPropertyStr(ctx, value, "headers",
                              JS_NewStringLen(ctx, completion.headers.data(), completion.headers.size()));
            JS_SetPropertyStr(ctx, value, "body",
                              JS_NewArrayBufferCopy(ctx, reinterpret_cast<const std::uint8_t *>(completion.body.data()),
                                                    completion.body.size()));
        }
        else {
            value = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, value, "code", JS_NewInt32(ctx, completion.number));
            JS_SetPropertyStr(ctx, value, "output",
                              JS_NewStringLen(ctx, completion.body.data(), completion.body.size()));
        }
        g_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
        const auto settled = JS_Call(ctx, ok ? pending->second.resolve : pending->second.reject, JS_UNDEFINED, 1, &value);
        JS_FreeValue(ctx, settled);
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, pending->second.resolve);
        JS_FreeValue(ctx, pending->second.reject);
        module.pending.erase(pending);
    }

    for (const auto &owner : g_loaded) {
        auto &module = *owner;
        std::vector<int> due;
        for (const auto &timer : module.timers) {
            if (timer.due <= now) {
                due.push_back(timer.id);
            }
        }
        for (const auto id : due) {
            const auto found = std::ranges::find(module.timers, id, &Timer::id);
            if (found == module.timers.end()) {
                continue;
            }
            auto *const ctx = module.ctx;
            auto callback = JS_DupValue(ctx, found->callback);
            std::vector<JSValue> arguments;
            for (const auto &argument : found->arguments) {
                arguments.push_back(JS_DupValue(ctx, argument));
            }
            if (found->interval.count() > 0) {
                found->due = now + found->interval;
            }
            else {
                free_timer(ctx, *found);
                module.timers.erase(found);
            }
            g_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
            const auto outcome =
                JS_Call(ctx, callback, JS_UNDEFINED, static_cast<int>(arguments.size()), arguments.data());
            if (JS_IsException(outcome)) {
                add_log(module.name, exception_text(ctx), true);
            }
            JS_FreeValue(ctx, outcome);
            JS_FreeValue(ctx, callback);
            for (const auto &argument : arguments) {
                JS_FreeValue(ctx, argument);
            }
        }
    }

    g_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
    JSContext *job_ctx = nullptr;
    for (auto status = JS_ExecutePendingJob(g_runtime, &job_ctx); status != 0;
         status = JS_ExecutePendingJob(g_runtime, &job_ctx)) {
        if (status < 0) {
            add_log("", exception_text(job_ctx), true);
        }
    }
}

bool handle_chat(const std::string_view message)
{
    constexpr std::string_view kCommand = "!!module";
    if (!message.starts_with(kCommand) || (message.size() > kCommand.size() && message[kCommand.size()] != ' ')) {
        return false;
    }
    execute(message.substr(kCommand.size()));
    return true;
}

void execute(const std::string_view arguments)
try {
    std::vector<std::string> words;
    std::istringstream stream{std::string{arguments}};
    for (std::string word; stream >> word;) {
        words.push_back(std::move(word));
    }
    g_attention.store(true, std::memory_order_relaxed);
    auto echo = std::string{"!!module"};
    for (const auto &word : words) {
        echo += ' ';
        echo += word;
    }
    add_log("", std::move(echo), false);

    const std::lock_guard lock{g_mutex};
    const auto verb = words.empty() ? std::string{} : words[0];
    const auto name = words.size() > 1 ? words[1] : std::string{};

    if (verb == "folder") {
        std::error_code ec;
        std::filesystem::create_directories(modules_directory(), ec);
#ifdef _WIN32
        ShellExecuteW(nullptr, L"open", modules_directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
        add_log("", std::format("modules folder: {}", path_text(modules_directory())), false);
    }
    else if (verb == "list") {
        const auto infos = modules();
        add_log("",
                std::format("{} module{} in {}", infos.size(), infos.size() == 1 ? "" : "s",
                            path_text(modules_directory())),
                false);
        for (const auto &info : infos) {
            std::string granted_text;
            for (const auto &permission : info.permissions) {
                granted_text += granted_text.empty() ? permission : ", " + permission;
            }
            add_log("",
                    std::format("  {}: {}{}{}", info.name,
                                info.loaded ? "enabled" : (info.enabled ? "enabled, not loaded" : "disabled"),
                                granted_text.empty() ? "" : std::format(" [{}]", granted_text),
                                info.error.empty() ? ""
                                                   : std::format(" ({})", info.error.substr(0, info.error.find('\n')))),
                    !info.error.empty());
        }
    }
    else if (verb == "autocomplete") {
        std::string text{kDeclarationsHead};
        std::string names;
        for (const auto &[packet, id] : exported_packets()) {
            auto key = packet;
            key[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])));
            names += std::format("    {}: {};\n", key, id);
            text += std::format("  export class {} extends Packet {{ static id: {}; }}\n", packet, id);
        }
        text += std::format("  export const Packets: {{\n{}  }};\n}}\n", names);
        text += kDeclarationsGlobals;
        std::ofstream{modules_directory() / "spyglass.d.ts", std::ios::binary | std::ios::trunc} << text;
        std::error_code ec;
        if (!std::filesystem::exists(modules_directory() / "jsconfig.json", ec)) {
            std::ofstream{modules_directory() / "jsconfig.json", std::ios::binary} << kJsConfig;
        }
        add_log("", std::format("wrote spyglass.d.ts in {}", path_text(modules_directory())), false);
    }
    else if (verb == "grant" || verb == "revoke") {
        const auto permission = words.size() > 2 ? words[2] : std::string{};
        if (!valid_name(name) || std::ranges::find(kPermissions, permission) == kPermissions.end()) {
            add_log("", std::format("usage: !!module {} <name> <fs | fs.all | net | process>", verb), true);
        }
        else {
            if (verb == "grant") {
                g_grants[name].insert(permission);
            }
            else if (const auto held = g_grants.find(name); held != g_grants.end()) {
                held->second.erase(permission);
                if (held->second.empty()) {
                    g_grants.erase(held);
                }
            }
            save_config();
            add_log("", std::format("{} {} for {}", permission, verb == "grant" ? "granted" : "revoked", name), false);
        }
    }
    else if (verb == "enable" || verb == "disable" || verb == "reload") {
        if (name.empty()) {
            add_log("", std::format("usage: !!module {} <name>", verb), true);
        }
        else if (verb == "disable") {
            if (!g_enabled.erase(name) &&
                !std::ranges::any_of(g_loaded, [&](const auto &module) { return module->name == name; })) {
                add_log("", std::format("{} is not enabled", name), true);
            }
            else {
                unload(name);
                g_failures.erase(name);
                save_config();
                add_log("", std::format("{} disabled", name), false);
            }
        }
        else if (verb == "reload" && !g_enabled.contains(name)) {
            add_log("", std::format("{} is not enabled", name), true);
        }
        else {
            unload(name);
            if (!find_entry(name)) {
                add_log("", std::format("no module named '{}' in {}", name, path_text(modules_directory())), true);
            }
            else if (const auto error = load(name); !error.empty()) {
                g_failures[name] = error;
                add_log(name, error, true);
                add_log("", std::format("{} failed to load", name), true);
            }
            else {
                g_failures.erase(name);
                g_enabled.insert(name);
                save_config();
                add_log("", std::format("{} {}", name, verb == "reload" ? "reloaded" : "enabled"), false);
            }
        }
    }
    else {
        add_log("", "usage: !!module folder | list | enable <name> | disable <name> | reload <name> | grant <name> <permission> | revoke <name> <permission> | autocomplete", !verb.empty());
    }
}
catch (const std::exception &e) {
    add_log("", std::format("command failed: {}", e.what()), true);
}

std::vector<std::string> permissions_of(const std::string &name)
{
    const auto found = g_grants.find(name);
    return found == g_grants.end() ? std::vector<std::string>{}
                                   : std::vector<std::string>{found->second.begin(), found->second.end()};
}

std::vector<ModuleInfo> modules()
{
    const std::lock_guard lock{g_mutex};
    auto names = discover();
    for (const auto &name : g_enabled) {
        if (std::ranges::find(names, name) == names.end()) {
            names.push_back(name);
        }
    }
    std::vector<ModuleInfo> infos;
    for (auto &name : names) {
        const auto failure = g_failures.find(name);
        infos.push_back({
            .name = name,
            .enabled = g_enabled.contains(name),
            .loaded = std::ranges::any_of(g_loaded, [&](const auto &module) { return module->name == name; }),
            .permissions = permissions_of(name),
            .error = failure == g_failures.end() ? (find_entry(name) ? "" : "missing") : failure->second,
        });
    }
    return infos;
}

std::vector<LogLine> log_lines()
{
    const std::lock_guard lock{g_log_mutex};
    return {g_log.begin(), g_log.end()};
}

std::uint64_t log_serial()
{
    return g_log_serial.load(std::memory_order_relaxed);
}

bool take_attention()
{
    return g_attention.exchange(false, std::memory_order_relaxed);
}

}  // namespace spyglass::script
