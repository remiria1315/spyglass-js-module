import { packet, Packets, Types, decode, encode } from "spyglass";

const Text = Types.struct({
  needsTranslation: Types.bool,
  variant: Types.byte,
  type: Types.byte,
  source: Types.string,
  message: Types.string,
  rest: Types.bytes,
});

packet.subscribe(Packets.textPacket, (pkt, ctx, cancel, override) => {
  const text = decode(Text, ctx.body);
  text.message = text.message.toUpperCase();
  override(encode(Text, text));
}, { direction: "c2s" });
