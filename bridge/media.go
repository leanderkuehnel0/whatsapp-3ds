package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"os"
	"sync"

	_ "image/gif"
	_ "image/png"

	_ "golang.org/x/image/webp"

	"golang.org/x/image/draw"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"google.golang.org/protobuf/proto"
)

// The 3DS top screen is 400×240; images are scaled to fit inside it.
const (
	maxW = 400
	maxH = 240
)

// MediaCache downloads WhatsApp images on demand and keeps 3DS-sized JPEGs on disk.
type MediaCache struct {
	dir string
	wa  *whatsmeow.Client
	mu  sync.Mutex // serializes conversions so a burst of requests can't exhaust memory
}

type ScreenImage struct {
	JPEG []byte
	W, H int
}

func (mc *MediaCache) Get(ctx context.Context, m Msg) (ScreenImage, error) {
	if len(m.Media) == 0 {
		return ScreenImage{}, errors.New("message has no media")
	}
	path := mc.path(m.Chat, m.ID, ".jpg")

	mc.mu.Lock()
	defer mc.mu.Unlock()

	if data, err := os.ReadFile(path); err == nil {
		if cfg, err := jpeg.DecodeConfig(bytes.NewReader(data)); err == nil {
			return ScreenImage{JPEG: data, W: cfg.Width, H: cfg.Height}, nil
		}
	}

	var msg waE2E.Message
	if err := proto.Unmarshal(m.Media, &msg); err != nil {
		return ScreenImage{}, err
	}
	raw, err := mc.fetch(ctx, &msg)
	if err != nil {
		return ScreenImage{}, err
	}
	img, _, err := image.Decode(bytes.NewReader(raw))
	if err != nil {
		return ScreenImage{}, fmt.Errorf("decode image: %w", err)
	}
	out := fitScreen(img)
	var buf bytes.Buffer
	if err := jpeg.Encode(&buf, out, &jpeg.Options{Quality: 82}); err != nil {
		return ScreenImage{}, err
	}
	if err := os.MkdirAll(mc.dir, 0o700); err == nil {
		_ = os.WriteFile(path, buf.Bytes(), 0o600)
	}
	b := out.Bounds()
	return ScreenImage{JPEG: buf.Bytes(), W: b.Dx(), H: b.Dy()}, nil
}

func (mc *MediaCache) fetch(ctx context.Context, msg *waE2E.Message) ([]byte, error) {
	switch {
	case msg.GetImageMessage() != nil:
		return mc.wa.Download(ctx, msg.GetImageMessage())
	case msg.GetStickerMessage() != nil:
		return mc.wa.Download(ctx, msg.GetStickerMessage())
	case msg.GetVideoMessage() != nil && len(msg.GetVideoMessage().GetJPEGThumbnail()) > 0:
		return msg.GetVideoMessage().GetJPEGThumbnail(), nil
	}
	return nil, errors.New("unsupported media type")
}

// fitScreen scales img to fit within maxW×maxH, flattening transparency onto white
// (stickers are transparent WebP and JPEG has no alpha).
func fitScreen(img image.Image) image.Image {
	b := img.Bounds()
	w, h := b.Dx(), b.Dy()
	if w <= 0 || h <= 0 {
		return image.NewRGBA(image.Rect(0, 0, 1, 1))
	}
	scale := min(float64(maxW)/float64(w), float64(maxH)/float64(h))
	if scale > 1 && w >= 64 && h >= 64 {
		scale = 1 // don't blow up normal images, only tiny ones
	}
	nw, nh := max(1, int(float64(w)*scale)), max(1, int(float64(h)*scale))

	dst := image.NewRGBA(image.Rect(0, 0, nw, nh))
	draw.Draw(dst, dst.Bounds(), &image.Uniform{C: color.White}, image.Point{}, draw.Src)
	draw.CatmullRom.Scale(dst, dst.Bounds(), img, b, draw.Over, nil)
	return dst
}
