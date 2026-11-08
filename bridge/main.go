// Command wa3ds-bridge links to WhatsApp as a companion device and serves the
// 3DS client over a token-protected TLS connection (see PROTOCOL.md).
package main

import (
	"context"
	"crypto/rand"
	"crypto/tls"
	"database/sql"
	"encoding/base32"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"

	"github.com/mdp/qrterminal/v3"
	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/store/sqlstore"
	waLog "go.mau.fi/whatsmeow/util/log"
	"google.golang.org/protobuf/proto"
	_ "modernc.org/sqlite"
)

// version is set at build time via -ldflags "-X main.version=...".
var version = "dev"

func main() {
	var (
		listen      = flag.String("listen", ":7473", "address the 3DS connects to")
		dataDir     = flag.String("data", defaultDataDir(), "directory for the database, TLS certificate, token and media cache")
		pairPhone   = flag.String("pair-phone", "", "link with an 8-digit pairing code for this phone number (e.g. 491701234567) instead of a QR code")
		debug       = flag.Bool("debug", false, "verbose WhatsApp logging")
		showVersion = flag.Bool("version", false, "print version and exit")
	)
	flag.Parse()
	if *showVersion {
		fmt.Println(version)
		return
	}
	log.SetFlags(log.LstdFlags)
	log.Printf("wa3ds-bridge %s, data in %s", version, *dataDir)

	if err := run(*listen, *dataDir, *pairPhone, *debug); err != nil {
		log.Fatal(err)
	}
}

func defaultDataDir() string {
	if d := os.Getenv("WA3DS_DATA"); d != "" {
		return d
	}
	return "wa3ds-data"
}

func run(listen, dataDir, pairPhone string, debug bool) error {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	if err := os.MkdirAll(dataDir, 0o700); err != nil {
		return err
	}

	token, err := loadOrCreateToken(dataDir)
	if err != nil {
		return fmt.Errorf("token: %w", err)
	}
	cert, err := loadOrCreateCert(dataDir)
	if err != nil {
		return fmt.Errorf("tls certificate: %w", err)
	}

	dsn := "file:" + filepath.Join(dataDir, "wa3ds.db") +
		"?_pragma=foreign_keys(1)&_pragma=journal_mode(WAL)&_pragma=busy_timeout(10000)"
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return err
	}
	defer db.Close()

	level := "WARN"
	if debug {
		level = "DEBUG"
	}
	container := sqlstore.NewWithDB(db, "sqlite3", waLog.Stdout("DB", level, true))
	if err := container.Upgrade(ctx); err != nil {
		return fmt.Errorf("upgrade whatsmeow db: %w", err)
	}
	st, err := NewStore(ctx, db)
	if err != nil {
		return fmt.Errorf("init store: %w", err)
	}
	device, err := container.GetFirstDevice(ctx)
	if err != nil {
		return err
	}

	// Show up as "WA3DS (Nintendo 3DS)" in WhatsApp's linked devices list.
	store.DeviceProps.Os = proto.String("WA3DS")

	client := whatsmeow.NewClient(device, waLog.Stdout("WA", level, true))
	hub := NewHub()
	bridge := &Bridge{
		wa:    client,
		store: st,
		hub:   hub,
		media: &MediaCache{dir: filepath.Join(dataDir, "media"), wa: client},
	}
	client.AddEventHandler(bridge.handleEvent)

	if err := connectWhatsApp(ctx, client, pairPhone); err != nil {
		return err
	}
	defer client.Disconnect()

	ln, err := net.Listen("tcp", listen)
	if err != nil {
		return err
	}
	srv := &Server{
		bridge: bridge,
		token:  token,
		tlsCfg: &tls.Config{Certificates: []tls.Certificate{cert}, MinVersion: tls.VersionTLS12},
	}

	fmt.Println()
	fmt.Println("==================== 3DS setup ====================")
	fmt.Printf(" Port:        %s\n", portOf(listen))
	fmt.Printf(" Token:       %s\n", token)
	fmt.Printf(" Fingerprint: %s\n", fingerprint(cert))
	fmt.Println(" Compare the fingerprint with the one the 3DS shows")
	fmt.Println(" on first connect before accepting it.")
	fmt.Println("===================================================")
	fmt.Println()

	errc := make(chan error, 1)
	go func() { errc <- srv.Serve(ln) }()
	log.Printf("Listening for the 3DS on %s", listen)

	select {
	case <-ctx.Done():
		log.Println("Shutting down")
		_ = ln.Close()
		return nil
	case err := <-errc:
		return err
	}
}

// connectWhatsApp connects with the stored session, or links a new device via QR code or pairing code.
func connectWhatsApp(ctx context.Context, client *whatsmeow.Client, pairPhone string) error {
	if client.Store.ID != nil {
		return client.Connect()
	}

	qrChan, err := client.GetQRChannel(ctx)
	if err != nil {
		return err
	}
	if err := client.Connect(); err != nil {
		return err
	}

	if pairPhone != "" {
		code, err := client.PairPhone(ctx, strings.TrimPrefix(pairPhone, "+"), true, whatsmeow.PairClientChrome, "Chrome (Linux)")
		if err != nil {
			return fmt.Errorf("pair phone: %w", err)
		}
		fmt.Printf("\nOn your phone: WhatsApp > Linked devices > Link a device > Link with phone number instead\nEnter this code: %s\n\n", code)
	} else {
		fmt.Println("\nScan this QR code with WhatsApp > Linked devices > Link a device:")
	}

	for evt := range qrChan {
		switch evt.Event {
		case "code":
			if pairPhone == "" {
				qrterminal.GenerateHalfBlock(evt.Code, qrterminal.L, os.Stdout)
			}
		case "success":
			log.Println("Linked successfully")
			return nil
		case "timeout":
			return fmt.Errorf("linking timed out, run the bridge again")
		default:
			if evt.Error != nil {
				return fmt.Errorf("linking failed: %w", evt.Error)
			}
		}
	}
	// Channel closed without success (e.g. context cancelled).
	if client.Store.ID == nil {
		return fmt.Errorf("linking aborted")
	}
	return nil
}

// loadOrCreateToken returns the shared secret the 3DS must send, generating one on first run.
// WA3DS_TOKEN overrides the stored value.
func loadOrCreateToken(dir string) (string, error) {
	if t := os.Getenv("WA3DS_TOKEN"); t != "" {
		return t, nil
	}
	path := filepath.Join(dir, "token")
	if b, err := os.ReadFile(path); err == nil {
		if t := strings.TrimSpace(string(b)); t != "" {
			return t, nil
		}
	}
	// 16 base32 characters = 80 bits, short enough to type on the 3DS keyboard.
	buf := make([]byte, 10)
	if _, err := rand.Read(buf); err != nil {
		return "", err
	}
	t := strings.ToLower(base32.StdEncoding.EncodeToString(buf))
	if err := os.WriteFile(path, []byte(t+"\n"), 0o600); err != nil {
		return "", err
	}
	return t, nil
}

func portOf(addr string) string {
	if _, p, err := net.SplitHostPort(addr); err == nil {
		return p
	}
	return addr
}

