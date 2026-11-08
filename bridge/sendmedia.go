package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"image"
	"image/jpeg"
	"os"
	"path/filepath"
	"time"

	"golang.org/x/image/draw"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
	"google.golang.org/protobuf/proto"
)

const (
	maxImageUpload = 4 << 20 // bytes of JPEG from the 3DS
	maxVoiceUpload = 2 << 20 // ~3.4 minutes of ADPCM
)

// recordSent stores a message we just sent and pushes it to clients, like an incoming event.
func (b *Bridge) recordSent(ctx context.Context, jid types.JID, resp whatsmeow.SendResponse, msg *waE2E.Message) {
	own := types.EmptyJID
	if b.wa.Store.ID != nil {
		own = b.wa.Store.ID.ToNonAD()
	}
	b.handleMessage(ctx, &events.Message{
		Info: types.MessageInfo{
			MessageSource: types.MessageSource{Chat: jid, Sender: own, IsFromMe: true, IsGroup: jid.Server == types.GroupServer},
			ID:            resp.ID,
			Timestamp:     resp.Timestamp,
		},
		Message: msg,
	}, true)
}

func parseChatJID(s string) (types.JID, error) {
	jid, err := types.ParseJID(s)
	if err != nil || jid.IsEmpty() {
		return types.EmptyJID, fmt.Errorf("bad jid %q", s)
	}
	return jid, nil
}

// SendImage uploads a JPEG taken on the 3DS and sends it as a photo message.
func (b *Bridge) SendImage(ctx context.Context, chatJID string, data []byte, caption string) (string, error) {
	jid, err := parseChatJID(chatJID)
	if err != nil {
		return "", err
	}
	img, err := jpeg.Decode(bytes.NewReader(data))
	if err != nil {
		return "", fmt.Errorf("not a JPEG: %w", err)
	}
	up, err := b.wa.Upload(ctx, data, whatsmeow.MediaImage)
	if err != nil {
		return "", fmt.Errorf("upload: %w", err)
	}
	bounds := img.Bounds()
	msg := &waE2E.Message{ImageMessage: &waE2E.ImageMessage{
		URL:           proto.String(up.URL),
		DirectPath:    proto.String(up.DirectPath),
		MediaKey:      up.MediaKey,
		FileEncSHA256: up.FileEncSHA256,
		FileSHA256:    up.FileSHA256,
		FileLength:    proto.Uint64(up.FileLength),
		Mimetype:      proto.String("image/jpeg"),
		Width:         proto.Uint32(uint32(bounds.Dx())),
		Height:        proto.Uint32(uint32(bounds.Dy())),
		JPEGThumbnail: thumbnail(img, 72),
	}}
	if caption != "" {
		msg.ImageMessage.Caption = proto.String(caption)
	}
	resp, err := b.wa.SendMessage(ctx, jid, msg)
	if err != nil {
		return "", err
	}
	// Prime the screen-size cache so viewing our own photo needs no download.
	b.media.Put(chatJID, resp.ID, img)
	b.recordSent(ctx, jid, resp, msg)
	return resp.ID, nil
}

// SendVoice converts ADPCM recorded on the 3DS to Ogg/Opus and sends it as a voice note.
func (b *Bridge) SendVoice(ctx context.Context, chatJID string, adpcm []byte) (string, error) {
	jid, err := parseChatJID(chatJID)
	if err != nil {
		return "", err
	}
	pcm, err := adpcmFramesToPCM(adpcm)
	if err != nil {
		return "", err
	}
	if len(pcm) < voiceRate/2 {
		return "", errors.New("voice message is too short")
	}
	ogg, err := encodeOggOpus(pcm)
	if err != nil {
		return "", fmt.Errorf("encode: %w", err)
	}
	up, err := b.wa.Upload(ctx, ogg, whatsmeow.MediaAudio)
	if err != nil {
		return "", fmt.Errorf("upload: %w", err)
	}
	msg := &waE2E.Message{AudioMessage: &waE2E.AudioMessage{
		URL:               proto.String(up.URL),
		DirectPath:        proto.String(up.DirectPath),
		MediaKey:          up.MediaKey,
		FileEncSHA256:     up.FileEncSHA256,
		FileSHA256:        up.FileSHA256,
		FileLength:        proto.Uint64(up.FileLength),
		Mimetype:          proto.String("audio/ogg; codecs=opus"),
		PTT:               proto.Bool(true),
		Seconds:           proto.Uint32(uint32((len(pcm) + voiceRate/2) / voiceRate)),
		Waveform:          waveform(pcm),
		MediaKeyTimestamp: proto.Int64(time.Now().Unix()),
	}}
	resp, err := b.wa.SendMessage(ctx, jid, msg)
	if err != nil {
		return "", err
	}
	b.media.PutVoice(chatJID, resp.ID, pcmToADPCMFrames(pcm))
	b.recordSent(ctx, jid, resp, msg)
	return resp.ID, nil
}

// thumbnail returns a small JPEG preview (WhatsApp shows it until the full photo loads).
func thumbnail(img image.Image, size int) []byte {
	b := img.Bounds()
	scale := float64(size) / float64(max(b.Dx(), b.Dy()))
	w, h := max(1, int(float64(b.Dx())*scale)), max(1, int(float64(b.Dy())*scale))
	dst := image.NewRGBA(image.Rect(0, 0, w, h))
	draw.ApproxBiLinear.Scale(dst, dst.Bounds(), img, b, draw.Src, nil)
	var buf bytes.Buffer
	_ = jpeg.Encode(&buf, dst, &jpeg.Options{Quality: 60})
	return buf.Bytes()
}

// ---------------------------------------------------------------- media cache additions

func (mc *MediaCache) path(chat, id, ext string) string {
	sum := sha256.Sum256([]byte(chat + "\x00" + id))
	return filepath.Join(mc.dir, hex.EncodeToString(sum[:16])+ext)
}

// Put stores the screen-size version of an image we already have.
func (mc *MediaCache) Put(chat, id string, img image.Image) {
	var buf bytes.Buffer
	if err := jpeg.Encode(&buf, fitScreen(img), &jpeg.Options{Quality: 82}); err != nil {
		return
	}
	if err := os.MkdirAll(mc.dir, 0o700); err == nil {
		_ = os.WriteFile(mc.path(chat, id, ".jpg"), buf.Bytes(), 0o600)
	}
}

// PutVoice stores the 3DS-format (ADPCM) version of a voice message.
func (mc *MediaCache) PutVoice(chat, id string, adpcm []byte) {
	if err := os.MkdirAll(mc.dir, 0o700); err == nil {
		_ = os.WriteFile(mc.path(chat, id, ".adpcm"), adpcm, 0o600)
	}
}

// Voice returns a voice/audio message as 16 kHz ADPCM frames for the 3DS, downloading and
// converting it on first use. Messages longer than maxVoiceSeconds are cut.
func (mc *MediaCache) Voice(ctx context.Context, m Msg) ([]byte, error) {
	if len(m.Media) == 0 {
		return nil, errors.New("message has no audio")
	}
	path := mc.path(m.Chat, m.ID, ".adpcm")

	mc.mu.Lock()
	defer mc.mu.Unlock()
	if data, err := os.ReadFile(path); err == nil && len(data) > 0 {
		return data, nil
	}

	var msg waE2E.Message
	if err := proto.Unmarshal(m.Media, &msg); err != nil {
		return nil, err
	}
	am := msg.GetAudioMessage()
	if am == nil {
		return nil, errors.New("not an audio message")
	}
	raw, err := mc.wa.Download(ctx, am)
	if err != nil {
		return nil, fmt.Errorf("download: %w", err)
	}
	pcm, err := decodeOggOpus(raw, voiceRate*maxVoiceSeconds)
	if err != nil {
		return nil, fmt.Errorf("unsupported audio format (%s): %w", am.GetMimetype(), err)
	}
	frames := pcmToADPCMFrames(pcm)
	PutVoiceFile(path, frames)
	return frames, nil
}

func PutVoiceFile(path string, data []byte) {
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err == nil {
		_ = os.WriteFile(path, data, 0o600)
	}
}
