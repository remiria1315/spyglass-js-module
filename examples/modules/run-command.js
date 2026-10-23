import { packet, Packets, Types, decode, commands } from "spyglass";

const Chat = Types.struct({
  needsTranslation: Types.bool,
  variant: Types.byte,
  type: Types.byte,
  source: Types.string,
  message: Types.string,
  rest: Types.bytes,
});

packet.subscribe(Packets.textPacket, (pkt, ctx) => {
  const text = decode(Chat, ctx.body, { partial: true });
  if (text.message === "!run") commands.run("/say test");
}, { direction: "s2c" });
