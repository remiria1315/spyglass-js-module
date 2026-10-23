import { packet, Packets, BinaryReader } from "spyglass";

packet.subscribe(Packets.textPacket, (pkt, ctx) => {
  const reader = new BinaryReader(ctx.body);
  reader.bool();
  reader.byte();
  reader.byte();
  const source = reader.string();
  const message = reader.string();
  console.log(`${source}: ${message}`);
}, { direction: "c2s" });
