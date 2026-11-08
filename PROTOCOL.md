# WA3DS bridge protocol

The 3DS client keeps one TLS connection open to the bridge (default port **7473**).
The bridge uses a self-signed certificate. The client pins the SHA-256 fingerprint of the
certificate (DER), trusting it on first use, so no CA chain is involved.

## Framing

- UTF-8 text lines ending in `\n`.
- Fields are separated by a TAB.
- Inside a field, `\` → `\\`, TAB → `\t`, LF → `\n`, CR → `\r`.
- Every client request starts with a short **tag** chosen by the client (e.g. `7`).
  Every response line for that request starts with the same tag.
- Lines the server sends on its own (events) use the tag `*`.
- Binary payloads: a `BIN` line announces the length in bytes, and exactly that many raw bytes follow right after its `\n`.

## Requests

| Request | Response |
|---|---|
| `T AUTH <token>` | `T OK <own name> <own jid> <server unix time>` or `T ERR <reason>` (connection closed). Must be the first request. |
| `T PING` | `T OK` |
| `T CHATS` | zero or more `T CHAT <chat>` lines, then `T END` |
| `T MSGS <chat jid> <before ts> <limit>` | the newest `limit` messages strictly older than `before ts` (0 = newest), oldest first, as `T MSG <msg>`, then `T END` |
| `T SEND <chat jid> <text>` | `T OK <message id>`; the sent message is also broadcast as an event |
| `T READ <chat jid>` | `T OK`; sends read receipts and clears the unread counter |
| `T MEDIA <chat jid> <message id>` | `T BIN <length> <width> <height>` + JPEG bytes (fits within 400×240), or `T ERR <reason>` |

## Records

`<chat>` = `jid  name  is_group(0/1)  last_ts  unread  preview`

`<msg>` = `chat_jid  msg_id  ts  from_me(0/1)  sender_name  kind  has_media(0/1)  text`

`kind` is one of `text`, `image`, `sticker`, `video`, `audio`, `document`, `other`, `deleted`.
Timestamps are Unix seconds.

## Events (tag `*`)

| Event | Meaning |
|---|---|
| `* MSG <msg>` | new or edited message |
| `* CHAT <chat>` | chat metadata changed (name, unread, preview) |
| `* RELOAD` | bulk change (history sync); the client should re-fetch `CHATS` |
| `* STATUS <state>` | WhatsApp connection state: `connected`, `disconnected`, `logged_out` |
| `* CALL <chat jid> <caller name> <ts>` | incoming call (shown as a missed call; calls are not supported yet) |
