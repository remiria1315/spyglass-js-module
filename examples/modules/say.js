import { packet, Packets, BinaryWriter } from "spyglass";

setTimeout(() => {
  packet.send(Packets.textPacket,
    new BinaryWriter().bool(false).byte(1).byte(1).string("").string("sent by say.js").string("").string("").bool(false));
}, 5000);
