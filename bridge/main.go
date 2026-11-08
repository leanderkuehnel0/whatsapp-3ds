// Command bridge links to WhatsApp as a companion device and exposes a
// token-protected API for the 3DS client. This is a placeholder until the
// whatsmeow integration lands.
package main

import (
	"flag"
	"fmt"
)

// version is set at build time via -ldflags "-X main.version=...".
var version = "dev"

func main() {
	showVersion := flag.Bool("version", false, "print version and exit")
	flag.Parse()

	if *showVersion {
		fmt.Println(version)
		return
	}
	fmt.Printf("wa3ds bridge %s (not implemented yet)\n", version)
}
