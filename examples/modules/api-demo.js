import { fs, fetch, process, packet, Packets } from "spyglass";

let sent = 0;
packet.subscribe(Packets.textPacket, () => { sent++; }, { direction: "c2s" });

setInterval(() => {
  fs.writeText("chat-count.txt", String(sent));
}, 5000);

const response = await fetch("https://example.com");
console.log("example.com", response.status, response.text().length, "bytes");

const result = await process.run("cmd", ["/c", "echo", "hello"]);
console.log(result.output.trim());
