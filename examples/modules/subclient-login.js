import { packet } from "spyglass";

let request = null;

packet.subscribe(1, (pkt, ctx) => {
  request = ctx.body.slice(4);
}, { direction: "c2s" });

export function loginSubClient(id = 1) {
  if (request === null) throw new Error("no LoginPacket was seen; reconnect with this module enabled");
  packet.send(94, request, { direction: "c2s", subClient: id });
}

setTimeout(() => loginSubClient(1), 5000);
