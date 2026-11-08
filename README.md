# WA3DS

A WhatsApp client for the Nintendo 2DS/3DS, made of two parts:

- **`bridge/`**: a Go server (whatsmeow) that runs on a VPS, links to WhatsApp as a companion device, and exposes a token-protected API to the console.
- **`3ds/`**: a homebrew app (libctru + citro2d) that talks to the bridge.

> Unofficial WhatsApp clients violate WhatsApp's terms of service. Use a secondary number if you can.

## Downloads

Every push to `main` is built in GitHub Actions and published to the [`rolling` release](../../releases/tag/rolling):

- `wa3ds.3dsx`: copy to `sd:/3ds/` and launch from the Homebrew Launcher
- `wa3ds-bridge-linux-amd64` / `wa3ds-bridge-linux-arm64`: bridge server binaries

## Building locally

3DS app (requires devkitPro with `3ds-dev`):

```sh
cd 3ds && make
```

Bridge (requires Go):

```sh
cd bridge && go build .
```
