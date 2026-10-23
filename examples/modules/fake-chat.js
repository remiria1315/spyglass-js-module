import { packet, Packets, BinaryWriter } from "spyglass";

setTimeout(() => {
  packet.send(Packets.textPacket,
    new BinaryWriter().bool(false).byte(1).byte(1).string("Spyglass").string("this arrived from nowhere").string("").string("").bool(false),
    { direction: "s2c" });
}, 5000);
