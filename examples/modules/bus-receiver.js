import { bus } from "spyglass";

bus.on("chat-count", (data, from) => {
  if (data.count % 10 === 0) console.log(`${from}: ${data.count} chat packets`);
});
