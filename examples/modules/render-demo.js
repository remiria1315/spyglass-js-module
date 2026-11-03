import { packet, Packets, BinaryReader, render } from "spyglass";

const camera = { x: 0, y: 0, z: 0, yaw: 0, pitch: 0, fov: 70 };
let packets = 0;

packet.subscribe(Packets.playerAuthInputPacket, (pkt, ctx) => {
  const reader = new BinaryReader(ctx.body);
  camera.pitch = reader.float32();
  camera.yaw = reader.float32();
  camera.x = reader.float32();
  camera.y = reader.float32();
  camera.z = reader.float32();
  packets++;
}, { direction: "c2s" });

render.on((draw) => {
  const label = `${packets} movement packets, at ${camera.x.toFixed(1)} ${camera.y.toFixed(1)} ${camera.z.toFixed(1)}`;
  const { width, height } = draw.textSize(label, 16);
  draw.rect(16, 16, width + 16, height + 12, { fill: true, color: "#000000b0", rounding: 4 });
  draw.text(24, 22, label, { size: 16 });

  draw.setCamera(camera);
  draw.block({ x: 0, y: camera.y - 2, z: 0 }, { color: "#ff4040", fill: true });

  const mark = draw.worldToScreen({ x: 0.5, y: camera.y - 0.5, z: 0.5 });
  if (mark) {
    const size = Math.max(10, 0.4 * mark.scale);
    draw.text(mark.x, mark.y, `${mark.depth.toFixed(1)}m`, { size, color: "#ff4040" });
  }
});
