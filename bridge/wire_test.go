package main

import (
	"reflect"
	"testing"
)

func TestEscapeRoundTrip(t *testing.T) {
	cases := []string{"", "plain", "tab\there", "multi\nline\r\n", `back\slash`, `\t literal`, "emoji 😀\tö"}
	for _, c := range cases {
		if got := unescapeField(escapeField(c)); got != c {
			t.Errorf("round trip %q: got %q", c, got)
		}
	}
}

func TestLineSplit(t *testing.T) {
	fields := []string{"7", "SEND", "123@s.whatsapp.net", "hi\tthere\nfriend"}
	if got := splitLine(line(fields...) + "\n"); !reflect.DeepEqual(got, fields) {
		t.Fatalf("got %q", got)
	}
}
