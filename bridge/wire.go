package main

import "strings"

// escapeField encodes a value so it fits in one TAB-separated field (see PROTOCOL.md).
func escapeField(s string) string {
	if !strings.ContainsAny(s, "\\\t\n\r") {
		return s
	}
	var b strings.Builder
	b.Grow(len(s) + 8)
	for _, r := range s {
		switch r {
		case '\\':
			b.WriteString(`\\`)
		case '\t':
			b.WriteString(`\t`)
		case '\n':
			b.WriteString(`\n`)
		case '\r':
			b.WriteString(`\r`)
		default:
			b.WriteRune(r)
		}
	}
	return b.String()
}

// unescapeField reverses escapeField. Unknown escapes are kept literally.
func unescapeField(s string) string {
	if !strings.Contains(s, `\`) {
		return s
	}
	var b strings.Builder
	b.Grow(len(s))
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c != '\\' || i+1 == len(s) {
			b.WriteByte(c)
			continue
		}
		i++
		switch s[i] {
		case '\\':
			b.WriteByte('\\')
		case 't':
			b.WriteByte('\t')
		case 'n':
			b.WriteByte('\n')
		case 'r':
			b.WriteByte('\r')
		default:
			b.WriteByte('\\')
			b.WriteByte(s[i])
		}
	}
	return b.String()
}

// line joins escaped fields into one protocol line (without the trailing newline).
func line(fields ...string) string {
	esc := make([]string, len(fields))
	for i, f := range fields {
		esc[i] = escapeField(f)
	}
	return strings.Join(esc, "\t")
}

// splitLine splits a received line into unescaped fields.
func splitLine(l string) []string {
	parts := strings.Split(strings.TrimRight(l, "\r\n"), "\t")
	for i, p := range parts {
		parts[i] = unescapeField(p)
	}
	return parts
}
