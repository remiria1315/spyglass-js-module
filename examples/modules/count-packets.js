import { packet, Packets } from "spyglass";

let count = 0;

packet.subscribe(Packets.playerAuthInputPacket, () => {
  count++;
  if (count % 200 === 0) {
    console.log(`${count} movement packets sent`);
  }
}, { direction: "c2s" });
