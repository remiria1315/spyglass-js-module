import { commands } from "spyglass";

commands.register({
  name: "hello",
  description: "Say hello from a module",
  aliases: ["hi"],
  parameters: [
    { name: "who", type: "string", optional: true },
    { name: "mood", enum: ["happy", "sleepy"], optional: true },
  ],
}, (args) => `hello, ${args[0] ?? "world"}${args[1] ? ` (${args[1]})` : ""}`);
