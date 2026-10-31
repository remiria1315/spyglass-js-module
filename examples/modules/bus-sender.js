import { bus, packet, Packets } from "spyglass";

let count = 0;
packet.subscribe(Packets.textPacket, () => {
  count += 1;
  bus.emit("chat-count", { count });
}, { direction: "s2c" });
