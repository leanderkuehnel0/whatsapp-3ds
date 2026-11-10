Vendored from https://github.com/JotaDev66/WaCalls (MIT, see LICENSE.WaCalls)
commit edeb31f0427aba896639db503153b777a405eccf, with import paths rewritten. Keep local changes minimal so updates can be re-applied.

Local changes (marked `wa3ds` in the code):

- `call/callmanager_media.go`: `MediaStats` counters for incoming packets.
- `call/seqguard.go`, used in `onRelayData`: late and duplicate RTP packets are dropped before decryption.
- `call/callmanager_media.go`: the silence keepalive waits 400 ms (was 120 ms) without microphone audio, so short stalls on the console's connection are not filled with silence.
