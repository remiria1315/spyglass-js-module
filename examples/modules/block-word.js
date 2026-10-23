import { packet, Packets, BinaryReader } from "spyglass";

const blocked = ["password", "token"];

packet.subscribe(Packets.textPacket, (pkt, ctx, cancel) => {
  const reader = new BinaryReader(ctx.body);
  reader.bool();
  reader.byte();
  reader.byte();
  reader.string();
  const message = reader.string().toLowerCase();
  if (blocked.some((word) => message.includes(word))) {
    console.warn("blocked a chat message");
    cancel();
  }
}, { direction: "c2s" });
