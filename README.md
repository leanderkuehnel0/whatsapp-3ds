# WA3DS

A WhatsApp client for the Nintendo 2DS/3DS, made of two parts:

- **`bridge/`**: a Go server built on [whatsmeow](https://github.com/tulir/whatsmeow). It runs on a VPS, links to your WhatsApp account as a companion device (like WhatsApp Web), stores your chats, and serves them to the console.
- **`3ds/`**: a homebrew app (libctru, citro2d, mbedTLS). The chat list is on the touchscreen and the conversation on the top screen. You type with the system keyboard, and photos open full screen.

The two talk over a TLS connection protected by a token. The console pins the bridge's certificate the first time it connects. [PROTOCOL.md](PROTOCOL.md) describes the protocol.

> **Warning:** Unofficial WhatsApp clients violate WhatsApp's terms of service, and accounts using them can be banned. Use a secondary number if you can.

## Features

- 1:1 and group chats, with live updates pushed from the bridge
- Sending text messages (multi-line)
- Sending photos from the front or rear camera, with an optional caption
- Recording, sending and playing voice messages (converted to/from WhatsApp's Ogg/Opus on the bridge, in pure Go)
- Searching chats by name or phone number
- Viewing photos, stickers and video thumbnails, which the bridge shrinks to fit the screen
- Read receipts, unread counters, edits and deletions
- Recent chat history synced when you first link
- 1:1 voice calls, incoming and outgoing, using the console's microphone and speaker. Calling uses [WaCalls](https://github.com/JotaDev66/WaCalls)' VoIP stack (MIT), copied into `bridge/internal/voip`.

## Downloads

Every push to `main` is built in GitHub Actions and published to the [`rolling` release](../../releases/tag/rolling):

| File | What it's for |
|---|---|
| `wa3ds.cia` | the 3DS app as a HOME Menu title (install with FBI) |
| `wa3ds.3dsx` | the 3DS app for the Homebrew Launcher |
| `wa3ds-bridge-linux-amd64` | the bridge for x86-64 servers |
| `wa3ds-bridge-linux-arm64` | the bridge for ARM servers (e.g. Hetzner CAX, Oracle Ampere, Raspberry Pi) |

## 1. Set up the bridge on your VPS

Get the binary onto the server. The repo is private, so either download it with `gh`:

```sh
gh release download rolling -R leanderkuehnel0/whatsapp-3ds -p 'wa3ds-bridge-linux-amd64'
```

or download it on your computer and `scp` it over. Then on the server:

```sh
sudo useradd --system --home /var/lib/wa3ds --create-home wa3ds
sudo install -D -m 755 wa3ds-bridge-linux-amd64 /opt/wa3ds/wa3ds-bridge
```

Link it to WhatsApp. This step is interactive and only needed once:

```sh
sudo -u wa3ds /opt/wa3ds/wa3ds-bridge -data /var/lib/wa3ds
```

A QR code appears. On your phone, go to WhatsApp → Settings → Linked devices → Link a device and scan it. If the QR code doesn't render well in your terminal, use `-pair-phone 491701234567` (your number in international format, without `+`) to get an 8-character code to type in instead.

Once linked, the bridge prints a **3DS setup** box with the port, a **token** and a certificate **fingerprint**. Keep it open, or note these down. Then stop the bridge with Ctrl+C.

Run it as a service:

```sh
sudo cp deploy/wa3ds-bridge.service /etc/systemd/system/
sudo systemctl enable --now wa3ds-bridge
journalctl -u wa3ds-bridge -f      # shows the token and fingerprint again on every start
```

Open TCP port **7473** in your firewall (for example `sudo ufw allow 7473/tcp`, plus your provider's firewall if it has one).

The token is stored in `/var/lib/wa3ds/token`. To change it, edit that file (or set `WA3DS_TOKEN`) and restart the service.

## 2. Set up the 3DS

1. Install `wa3ds.cia` with FBI so WA3DS appears on the HOME Menu, or copy `wa3ds.3dsx` to `sd:/3ds/` to run it from the Homebrew Launcher. New `.cia` builds install over the old one, and your settings are kept. You can also send it over Wi-Fi: open the Homebrew Launcher, press Y, then run `3dslink wa3ds.3dsx` on your computer.
2. Start **WA3DS** from the Homebrew Launcher.
3. Enter the bridge address (domain or IP), the port (7473) and the token.
4. On first connect, the app shows the bridge's certificate fingerprint. Check that it matches the one the bridge printed, then press **A**.

Calls need the DSP firmware for sound. If you've never run **DSP1** on your console, run it once; it saves `sd:/3ds/dspfirm.cdc`. Headphones give the best calls. Without them the 3DS speaker and microphone are close together, so the app lowers your microphone while the other person is talking to keep them from hearing their own echo; you can't talk over each other in that mode.

The settings are saved in `sd:/3ds/wa3ds/config.ini`. You can also edit them there, or press **SELECT** on the chat list.

### Controls

| Where | Control | Action |
|---|---|---|
| Chat list | D-Pad / Circle Pad / drag on touchscreen | Scroll |
| | Y or tap the header | Search chats by name or number |
| | B | Clear the search |
| | A or tap | Open chat |
| | X | Refresh |
| | SELECT | Settings |
| Chat | D-Pad up/down | Select a message (going past the top loads older ones) |
| | Circle Pad | Scroll |
| | A | Write a message |
| | Y | View the selected photo or play the selected voice message |
| | X | Voice call (1:1 chats) |
| | R | Take and send a photo (X switches front/rear camera) |
| | SELECT | Record and send a voice message |
| | Y on a voice message | Play / stop it |
| | L | Load older messages |
| | B | Back |
| Call | A / B | Accept / decline an incoming call, B hangs up |
| | X | Mute or unmute the microphone |
| Anywhere | START | Quit (hangs up first if you're in a call) |

## Building locally

3DS app (requires devkitPro with `3ds-dev`, `3ds-mbedtls` and `3ds-libjpeg-turbo`):

```sh
cd 3ds && make
```

Bridge (requires Go 1.26+):

```sh
cd bridge && go test ./... && go build .
```
