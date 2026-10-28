// #antiad.js
// 宣伝を記録してワールドから追放するexampleプログラム
// 実行には!!module grant antiad fsが必要
import { BinaryReader, BinaryWriter, fs, packet, Packets } from "spyglass";
function sendMessage(str) {
  const body = new BinaryWriter() // パケットを書く
    .byte(0)
    .byte(1)
    .bool(true)
    .string("remiria1315")
    .string(str)
    // .byte(0)
    .string("2535456620588248")
    .byte(0)
    .byte(0)
    .toBytes();

  packet.send(Packets.textPacket, body, { direction: "c2s" }); //パケットをc2s(client to server)で送る
}
function runCommand(line, cb = (params, successful) => {}, timeoutMs = 10000) {
  const text = String(line).startsWith("/") ? String(line) : `/${line}`;
  return new Promise((resolve, reject) => {
    const rand64 = () =>
      (BigInt((Math.random() * 2 ** 32) >>> 0) << 32n) |
      BigInt((Math.random() * 2 ** 32) >>> 0);
    const requestId = [rand64(), rand64()];
    let unsub = () => {};
    let done = false;

    const finish = (fn, value) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      unsub();
      fn(value);
    };

    const timer = setTimeout(
      () => finish(reject, new Error(`command timeout: ${text}`)),
      timeoutMs,
    );

    unsub = packet.subscribe(
      Packets.commandOutputPacket,
      (pkt, ctx, c, o) => {
        try {
          const reader = new BinaryReader(ctx.body);

          reader.string();
          reader.uint64();
          reader.uint64();
          const reqstr = reader.string();
          reader.int64();

          const outputType = reader.string();
          const successCount = reader.uint32();
          const size = reader.varuint32();

          const messages = [];
          const returns = {};
          for (let i = 0; i < size; i++) {
            const messageId = reader.string();
            const successful = reader.bool();
            const n = reader.varuint32();
            const params = [];
            for (let j = 0; j < n; j++) params.push(reader.string());

            messages.push({ successful, messageId, params });
            returns[messageId] = { successful, params };
            cb(params, successful);
          }

          const dataSet = reader.bool() ? reader.string() : null;

          finish(resolve, {
            requestId,
            outputType,
            successCount,
            messages,
            returns,
            dataSet,
          });
        } catch (e) {
          finish(reject, e);
        }
      },
      { direction: "both" },
    );

    try {
      setTimeout(() => {
        packet.send(
          Packets.commandRequestPacket,
          new BinaryWriter()
            .string(text)
            .string("player")
            .uint64(requestId[0])
            .uint64(requestId[1])
            .string("")
            .int64(0)
            .bool(false)
            .string("latest")
            .toBytes(),
          { direction: "c2s" },
        );
      }, 100);
    } catch (e) {
      finish(reject, e);
    }
  });
}
let ad = {};
let count = {}
setInterval(() => {
  Object.entries(count).forEach(([k,v]) => {
    //v=array num
    v--
    if(v<0)delete count[k]
  })
}, 1000);
packet.subscribe(Packets.textPacket, (pkt, ctx, c, o) => {
  const reader = new BinaryReader(ctx.body);
  reader.bool();
  const type = reader.varint();
  switch (type) {
    case 1:
      // authorandmessage
      {
        const messageType = reader.uint8();
        console.log(messageType)
        if(![1, 7].includes(messageType)) return// chatとwhisper以外を除外する
        const name = reader.string();
        console.log(name)
        const message = reader.string();
        const xuid = reader.string();
        if (xuid == "2535456620588248") return;
        if (/(https?:\/\/|tpmultiplayer|NYX|nyx)/g.test(message)) {
          if (!name) {
            sendMessage(
              "匿名からの宣伝を検知しました: " +
                message.slice(0, 8) +
                "... by " +
                xuid,
            );
            ad = { name, xuid, message };
            runCommand('kick "' + xuid + '"', (param) => {});
          } else if(name) {
            if (!Array.isArray(count[name])) {
              count[name] = [];// カウントを初期化する
            }
            count[name].push(10)            
            if(count[name].length>5){
              runCommand(`kick "${xuid}"`)
              sendMessage(`プレイヤー'${name}'が規制回数以上宣伝したため追放しました`)
            } else {
              sendMessage(`プレイヤー'${name}'からの宣伝を確認しました 残り宣伝可能数${5 - count[name].length}回`)
            }
          }
        }
      }
      break;
    case 2:
      //messageandparams
      {
        // プレイヤーが抜けるのを監視して名前を取得し記録する
        console.log(ad);
        if (!ad.xuid) return;
        const messageType = reader.uint8();
        if (messageType !== 2) return;
        const message = reader.string();

        const paramsSize = reader.varint();
        const params = [];
        for (let j = 0; j < paramsSize; j++) params.push(reader.string());
        console.log(message);
        if (message.includes("%multiplayer.player.left")) {
          fs.appendText(
            "adBots.txt",
            `\n[${new Date(Date.now()).toISOString()}] XUID: ${ad.xuid}, NAME: ${params[0]}, MESSAGE: ${ad.message}`,
          );
          ad = {};
        }
      }
      break;
  }
});
