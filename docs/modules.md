# Modules

A module is a JavaScript file that runs inside the client and reacts to packets. Spyglass embeds
[QuickJS](https://github.com/quickjs-ng/quickjs) to run them.

## Folder

Modules live in `%LOCALAPPDATA%\spyglass\modules` on Windows and `~/.local/share/spyglass/modules` on the Linux
launcher. A module is either `<name>.js` or `<name>/index.js`. A name uses letters, digits, `_`, `-` and `.`.

Which modules are enabled is kept in `modules.json` in the same folder, and those are loaded when the client starts.

## Chat commands

Type these in game chat. The message is dropped before it reaches the server, and the output appears in the
modules window of the overlay.

| command | |
| --- | --- |
| `!!module folder` | opens the modules folder (Windows), or prints its path |
| `!!module list` | lists the modules and whether each is enabled |
| `!!module enable <name>` | loads a module and keeps it enabled across restarts |
| `!!module disable <name>` | unloads a module and keeps it disabled |
| `!!module reload <name>` | unloads an enabled module and loads it again from disk |

The modules window is under `Modules > Module Manager`, with the same actions as buttons.

## Script API

```js
import { packet, Packets, TextPacket, BinaryReader, BinaryWriter } from "spyglass";

packet.subscribe(Packets.textPacket, (pkt, ctx, cancel, override) => {
  cancel();
});
```

### `packet.subscribe(target, callback, options?)`

`target` is a packet id, an entry of `Packets`, or a packet class such as `TextPacket`. `options.direction` is
`"c2s"`, `"s2c"` or `"both"`, and defaults to `"both"`. It returns a function that removes the subscription.
Subscriptions are removed when the module is disabled or reloaded.

### Callback arguments

| argument | |
| --- | --- |
| `pkt` | an instance of the packet's class. Its properties are the fields the client's own reflection reports for that packet, the same ones the details pane shows. A packet the client does not reflect has no properties, so read `ctx.body` instead. |
| `ctx` | `direction` (`"c2s"` or `"s2c"`), `id`, `name`, `subClientId`, `size`, `cancelled`, and `body`, the packet body as a `Uint8Array` without the packet header. |
| `cancel()` | stops a packet. A packet the client sends is not sent. A packet from the server is read but not handled, so the client ignores it. |
| `override(bytes)` | replaces the packet body with a `Uint8Array` or `ArrayBuffer`. For a packet from the server the client reads the replacement in place of the original. |

`cancel` and `override` work only during the call they were handed to. Every subscriber for a packet runs, in the
order the modules were loaded, and `ctx.cancelled` shows whether an earlier one cancelled it.

### Exports

- `Packets`: packet ids by name, `Packets.textPacket`, `Packets.playerAuthInputPacket`. Built from the packets the
  client knows.
- A class per packet, named as the client names it: `TextPacket`, `StartGamePacket`. `TextPacket.id` is its id.
- `Packet`, the base class of those.
- `BinaryWriter` and `BinaryReader`, for building and reading a body. Integers are little-endian, `varint` is an
  unsigned 32-bit varint, and `string` is a varint length followed by UTF-8.

`console.log`, `console.info`, `console.warn` and `console.error` write to the modules window.

### Imports

A module can import `"spyglass"` and its own files by relative path, `import { x } from "./util.js"`. A path that
leaves the module's folder, or a bare name other than `"spyglass"`, fails the load. There is no file or network
access.

## Server-side modules

A module whose file name ends in `.server.js` (or a folder `name.server`) sees the packets of the server built into the
client, the one that runs when the player hosts a world, and a module without that ending sees only the client's own
packets. A client connected to somebody else's server has no server inside it, so a server module sees nothing there.

```js
import { packet, Packets } from "spyglass";

packet.subscribe(Packets.textPacket, (pkt, ctx, cancel) => {
  console.log(`player ${ctx.player} ${ctx.direction}`);
}, { direction: "c2s" });
```

- `direction` keeps its meaning on the wire. For a server module `"c2s"` is a packet a player sent to the server and
  `"s2c"` is a packet the server sends. `cancel()` and `override()` work in both.
- `ctx.side` is `"client"` or `"server"`. `ctx.player` is a number for the player on the other end of a server packet, and
  `null` in a client module. A number stays the same for a player until the game is closed.
- `packet.send` in a server module defaults to `{ direction: "s2c" }` and takes `to`, a player number or `"all"`.
  Sending a packet as if a player had sent it to the server is not supported.
- `commands.register` works in a server module as well. The command list is changed for every player, and a reply goes
  to the player who ran the command.
- A module is told apart from the other side by the first packets it sees, which are fixed in the protocol, so Spyglass
  has to be injected before the world is joined.

## Reading and writing structures

A `Type` describes how a value is laid out in a packet body. `decode(type, bytes)` turns bytes into a JavaScript value and
`encode(type, value)` turns a value back into bytes. Both throw if a layout does not match the bytes.

```js
import { Types, decode, encode } from "spyglass";

const Player = Types.struct({
  uuid: Types.uuid,
  name: Types.string,
  position: Types.struct({ x: Types.float32, y: Types.float32, z: Types.float32 }),
  tags: Types.array(Types.string),
  nickname: Types.optional(Types.string),
});
const Players = Types.array(Player);

const players = decode(Players, ctx.body, { partial: true });
packet.send(Packets.playerListPacket, encode(Players, players));
```

| type | value |
| --- | --- |
| `byte`, `uint8`, `int8`, `int16`, `uint16`, `int32`, `uint32` | number. `byte` and `uint8` are the same type. |
| `int64`, `uint64`, `varint64`, `svarint64`, `varuint64`, `bitset(size, encoding?)` | bigint. A bitset holds bit `n` as `1n << BigInt(n)`. `encoding` is `"varint"` (the default, 7 bits per byte, the low bits first), `"bytes"` (`ceil(size / 8)` bytes, little-endian) or `"list"` (a varint count, then the index of each set bit as a `svarint`, as `PlayerAuthInputPacket` writes its Input Data). |
| `varint`, `svarint`, `varuint32`, `varuint8`, `varint8` | number. `varint` and `varuint32` are the same unsigned varint. `svarint` is a zigzag varint. `varuint8` is an unsigned varint limited to 0 to 255, and `varint8` a zigzag varint limited to -128 to 127. |
| `float32`, `float64` | number |
| `bool`, `string`, `uuid` | boolean, string, string such as `12345678-1234-5678-9abc-def012345678`. A uuid is two little-endian 64-bit halves, so its bytes are not in the order of the text. |
| `bytes` | the rest of the body as a `Uint8Array` |
| `array(item, length?)` | array, with a `varint` count unless `length` is another type |
| `fixed(item, count)` | array of exactly `count` elements |
| `optional(item)` | the value, or `undefined` when the leading flag is false |
| `struct({ name: type })` | object, read and written in field order |
| `custom(read, write)` | whatever the two functions return and accept |

`decode` rejects bytes left over after the value unless `{ partial: true }` is passed. The layout of a packet is not
known to Spyglass, so take it from the fields the packet shows in the overlay, or from the protocol documentation.

## Commands

```js
import { commands } from "spyglass";

commands.register({
  name: "hello",
  description: "Say hello",
  aliases: ["hi"],
  parameters: [{ name: "who", type: "string", optional: true }, { name: "mood", enum: ["happy", "sleepy"] }],
}, (args, context) => `hello, ${args[0] ?? "world"}`);
```

A command is added to the list the server sends when the player joins, so it shows in the chat's completion, and a
`/name` typed in chat runs the handler instead of being sent to the server. Arguments are split on spaces, and quotes keep
words together. A string the handler returns, or resolves to, is shown to the player in chat. `context.reply(text)` shows
more.

- A module's commands reach the list the next time the player joins a world. Rejoin after enabling or reloading a module.
  A command still runs without being in the list.
- `parameters[].type` is one of `int`, `float`, `value`, `target`, `string`, `intPosition`, `position`, `message`,
  `text`, `json`, `command`, or a raw number. The numbers are those of the 1.21 protocol and can differ in another
  version, so `enum` is the safe choice.
- The list is only changed if Spyglass can read all of it, to the last byte. Otherwise the module log says the list
  was left alone, and commands still run but are not completed. A command with the same name as one in the list replaces it.
- `permission` is a number from 0 (`any`) to 5 (`internal`) or one of `"any"`, `"gamedirectors"`, `"admin"`, `"host"`, `"owner"` and `"internal"`.
- `commands.run("/say hi")` sends a command to the server. The header of a command request is copied from the last
  command the player typed, so a module that runs a command needs one command typed in chat first. Without that it
  throws an error that says so. `commands.run(line, { wait: true })` instead holds the command and sends it right after
  the player types their first command, and `commands.ready` tells whether a header has been copied.
- This is for commands handled by the client. Commands for other players, from a server module, are not supported yet.

## Reading the fields of a packet

The first argument of a callback holds the fields Spyglass read, with the client's own names. Some fields are variants, an
object with one key such as `TextPacketPayload::AuthorAndMessage`. `variant(value)` returns the short name and the
content, or `null` if the value is not a variant.

```js
import { packet, Packets, variant } from "spyglass";

packet.subscribe(Packets.textPacket, (pkt) => {
  const payload = variant(pkt.Body);
  if (payload?.name === "AuthorAndMessage") console.log(payload.value);
}, { direction: "s2c" });
```

## Sending packets

```js
packet.send(Packets.textPacket, new BinaryWriter().bool(false).byte(1).byte(1).string("").string("hello").string("").string("").bool(false));
```

`packet.send(target, body, { direction: "c2s" })` sends a packet to the server. `body` is the packet body without the
header, as a `Uint8Array`, an `ArrayBuffer` or a `BinaryWriter`. The packet leaves with the next packet the client
sends, so it needs a connected game. In a client module `direction: "s2c"` hands the packet to the client as if the server had sent it. It is delivered after the next packet
from the server, and modules do not see it in `packet.subscribe`.

## Autocomplete

`!!module autocomplete` writes `spyglass.d.ts` and, if there is none, `jsconfig.json` into the modules folder. An editor
that reads them, such as VS Code opened on that folder, completes the `spyglass` imports and the packet names.

## Timers

`setTimeout`, `setInterval`, `clearTimeout` and `clearInterval` are globals. Extra arguments are passed to the
callback. Timers run from the overlay's frame loop, so they fire at frame granularity and a callback that runs longer
than 100 ms is interrupted. A module can hold 256 timers.

## Permissions

`fs`, `net` and `process` are off until granted per module. A call without the permission throws an error that names
the command to run. Grants are saved in `modules.json` and apply without a reload.

```
!!module grant <name> <fs | fs.all | net | process>
!!module revoke <name> <fs | fs.all | net | process>
```

`!!module list` shows the permissions each module holds.

### `fs`

```js
import { fs } from "spyglass";
```

Paths are relative to `module-data/<name>` in the Spyglass output folder, and cannot leave it. `fs.all` also allows
absolute paths anywhere. Calls are synchronous and files are limited to 16 MB.

`readText`, `readBytes`, `writeText`, `writeBytes`, `appendText`, `exists`, `list`, `mkdir`, `remove`, `stat`.

### `net`

```js
import { fetch, net } from "spyglass";
const response = await fetch("https://example.com/api", { method: "POST", headers: {}, body: "...", timeout: 15000 });
response.status; response.ok; response.headers; response.text(); response.json(); response.bytes();
```

`net.fetch` is the same function. Only `http://` and `https://` work, and only on Windows. At most 16 requests per
module run at once.

### `process`

```js
import { process } from "spyglass";
const { code, output } = await process.run("git", ["status"], { cwd: "C:/repo", timeout: 30000 });
```

Runs a program without a shell and returns its exit code and combined output. `process` can do anything the player can,
so grant it only to modules you trust.

## Limits

- A callback that runs longer than 100 ms is interrupted, and a module takes 2 s at most to load.
- All modules share 128 MB.
- An error in a callback is written to the modules window, and the packet carries on.
- Cancelling a packet the client sends needs the send hook, which is cut against the Windows 1.26.50 release client. On a client without it, the errors window says so, `cancel()` does nothing, and a `!!module` message is sent to the server.

## Examples

`examples/modules` has small modules to copy into the modules folder: `log-chat`, `block-word`, `shout`, `count-packets`, `api-demo`, `say`, `fake-chat`, `hello-command`, `run-command` and `greet.server`.

Log every chat message the client sends, and drop the ones that contain a word:

```js
import { packet, Packets, BinaryReader } from "spyglass";

packet.subscribe(Packets.textPacket, (pkt, ctx, cancel) => {
  const reader = new BinaryReader(ctx.body);
  reader.bool();
  reader.byte();
  reader.byte();
  reader.string();
  const message = reader.string();
  console.log(message);
  if (message.includes("password")) {
    cancel();
  }
}, { direction: "c2s" });
```

Rewrite the message of a chat packet:

```js
import { packet, Packets, BinaryReader, BinaryWriter } from "spyglass";

packet.subscribe(Packets.textPacket, (pkt, ctx, cancel, override) => {
  const reader = new BinaryReader(ctx.body);
  const needsTranslation = reader.bool();
  const variant = reader.byte();
  const type = reader.byte();
  const source = reader.string();
  const message = reader.string();
  const rest = reader.bytes(reader.remaining);
  override(new BinaryWriter()
    .bool(needsTranslation).byte(variant).byte(type).string(source).string(message.toUpperCase()).bytes(rest)
    .toBytes());
}, { direction: "c2s" });
```

Count packets per second:

```js
import { packet, Packets } from "spyglass";

let count = 0;
packet.subscribe(Packets.playerAuthInputPacket, () => { count++; }, { direction: "c2s" });
```
