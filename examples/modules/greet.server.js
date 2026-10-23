import { packet, Packets, Layouts, encode } from "spyglass";

const seen = new Set();

packet.subscribe(Packets.playerAuthInputPacket, (pkt, ctx) => {
  if (seen.has(ctx.player)) return;
  seen.add(ctx.player);
  const message = { needsTranslation: false, variant: 1, type: 1, source: "Spyglass", message: `hello, player ${ctx.player}`, xuid: "", platformChatId: "", filtered: undefined };
  packet.send(Packets.textPacket, encode(Layouts.rawText, message), { to: ctx.player });
}, { direction: "c2s" });
