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
- Binary payloads: a `BIN` or `AUD` line from the server, or an `AUD`, `IMG` or `VOICE` request from the client, has the payload length as its first argument, and exactly that many raw bytes follow right after its `\n`.

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
| `T IMG <length> <chat jid> [caption]` + JPEG bytes | `T OK <message id>`; sends a photo (max 4 MB) |
| `T VOICE <length> <chat jid>` + ADPCM frames | `T OK <message id>`; sends a voice note (16 kHz ADPCM frames as in calls, max 2 MB); the bridge converts it to Ogg/Opus |
| `T VOICEGET <chat jid> <message id>` | `T BIN <length> <duration ms> 0` + 16 kHz ADPCM frames of a voice/audio message (cut at 5 minutes), or `T ERR <reason>` |
| `T CALL <chat jid>` | `T OK <call id>`; starts an outgoing voice call (1:1 chats only) |
| `T ACCEPT <call id>` | `T OK`; answers a ringing incoming call |
| `T HANGUP <call id>` | `T OK`; declines a ringing incoming call, or ends the call |
| `T AUD <length>` + bytes | microphone audio frame during a call (see below); no reply |

## Records

`<chat>` = `jid  name  is_group(0/1)  last_ts  unread  preview`

`<msg>` = `chat_jid  msg_id  ts  from_me(0/1)  sender_name  kind  has_media(0/1)  text`

`kind` is one of `text`, `image`, `sticker`, `video`, `audio`, `document`, `other`, `deleted`.
`has_media` means `MEDIA` (images, stickers, video thumbnails) or `VOICEGET` (audio) can fetch it.
Timestamps are Unix seconds.

## Events (tag `*`)

| Event | Meaning |
|---|---|
| `* MSG <msg>` | new or edited message |
| `* CHAT <chat>` | chat metadata changed (name, unread, preview) |
| `* RELOAD` | bulk change (history sync); the client should re-fetch `CHATS` |
| `* STATUS <state>` | WhatsApp connection state: `connected`, `disconnected`, `logged_out` |
| `* CALLSTATE <call id> <chat jid> <name> <state> <in/out> <end reason>` | call progress; `state` is `ringing` (outgoing), `incoming`, `connecting`, `active` or `ended`. Sent again right after `AUTH` if a call is in progress. |
| `* AUD <length>` + bytes | audio frame from the other person during a call |

## Call audio

Both directions use 16 kHz mono audio in 20 ms frames (320 samples) encoded as IMA ADPCM:

- bytes 0–1: int16 LE predictor
- byte 2: step index (0–88)
- byte 3: reserved (0)
- then 160 bytes of 4-bit codes, low nibble first

Every frame starts from the state in its own header, so a lost frame does not affect the next one.
The bridge drops audio frames for a client whose connection can't keep up instead of buffering them.
