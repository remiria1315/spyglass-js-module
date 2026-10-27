# module用 ミニコード集
# 以下のコードは、spyglass-moduleで動作します

## 権限取得
### 現在のコマンド権限を取得します。
```javascript
let permission = 0
packet.subscribe(Packets.updateAbilitiesPacket, (pkt, ctx) => {
  permission = pkt["Data"]["Command Permissions"];
}, { direction: "s2c" });
```
## getxuid
### xuidを取得します。
```javascript
async function getXuid(arg1) {
  if (xuidCache) return xuidCache;
  const res = await fetch("https://api.geysermc.org/v2/xbox/xuid/" + encodeURIComponent(arg1));
  if (!res.ok) throw new Error("xuid lookup failed: HTTP " + res.status);
  const m = res.text().match(/"xuid"\s*:\s*"?(\d+)"?/);
  if (!m) throw new Error("xuid not found in response");
  xuidCache = m[1];
  return xuidCache;
}
```
## sendchat
### チャットに新しく送ります
```javascript
async function sendChat(text) {
  if (!NAME) {
    console.log("sendchat: set NAME at the top of the file");
    return;
  }
  let xuid;
  try {
    xuid = await getXuid(NAME);
  } catch (e) {
    console.log("sendchat: " + e);
    return;
  }
  const w = new BinaryWriter();
  for (const b of HEADER) w.byte(b);
  w.string(NAME);
  w.string(text);
  w.string(xuid);
  w.string("");
  w.string("");
  sending = true;
  try {
    packet.send(TextPacket, w, { direction: "c2s" });
  } finally {
    sending = false;
  }
}
```
