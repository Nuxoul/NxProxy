package rpc

import (
	"regexp"
	"runtime"
	"runtime/debug"
	"strings"
	"unicode"
	"unicode/utf8"
)

// Set by the diagnostic package build, alongside the existing core version.
var tunDiagnosticBuildID = "development"

const tunErrorDetailLimit = 3072
const tunErrorScanLimit = 16384

// Some dependency wrappers flatten causes into one leaf string. This only
// establishes relevant diagnostic scope, never a Win32 code or retry authority.
var flattenedTunConfigure = regexp.MustCompile(`(?:^|: )start inbound/tun\[[^\]\r\n]{1,128}\]: configure tun interface: `)

var tunDetailPatterns = []*regexp.Regexp{
	regexp.MustCompile(`(?s)\{.*\}|\[\s*\{.*`),
	regexp.MustCompile(`(?i)[a-z][a-z0-9+.-]*://[^\s<>"']+`),
	regexp.MustCompile(`(?i)(?:password|passwd|token|secret|authorization|uuid|private[_ -]?key|subscription)\s*[:=]\s*(?:"[^"]*"|'[^']*'|[^\s,;)]+)`),
	regexp.MustCompile(`"[^"\r\n]*"|'[^'\r\n]*'`),
	regexp.MustCompile(`(?i)\b[a-z]:[\\/][^;|)\r\n]+|\\\\[^;|)\r\n]+|(?:^|\s)/[^;|)\r\n]+`),
	regexp.MustCompile(`\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?::[0-9]+)?\b`),
	regexp.MustCompile(`(?i)\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b`),
	regexp.MustCompile(`(?i)(?:[0-9a-f]{0,4}:){2,}[0-9a-f:.%]*`),
	regexp.MustCompile(`(?i)\b(?:[a-z0-9-]+\.)+[a-z]{2,63}\b`),
}

func trimTunUTF8(text string, limit int) (string, bool) {
	if len(text) <= limit {
		return text, false
	}
	text = text[:limit]
	for !utf8.ValidString(text) {
		text = text[:len(text)-1]
	}
	return text, true
}

// Relevant configure failures only. Preserve error order/stages/OS descriptions,
// not dynamic paths, addresses, quoted config values or credential assignments.
func sanitizeTunErrorDetail(text string) (string, bool, bool) {
	text, cut := trimTunUTF8(strings.ToValidUTF8(text, "�"), tunErrorScanLimit)
	text = strings.Map(func(r rune) rune {
		if unicode.IsControl(r) {
			return ' '
		}
		return r
	}, text)
	redacted := false
	for _, pattern := range tunDetailPatterns {
		next := pattern.ReplaceAllString(text, "<redacted>")
		redacted = redacted || next != text
		text = next
	}
	detail, short := trimTunUTF8(text, tunErrorDetailLimit)
	return detail, cut || short, redacted
}
func tunDiagnosticBuild() string {
	build := "diagnostic=" + tunDiagnosticBuildID + ";go=" + runtime.Version()
	if info, ok := debug.ReadBuildInfo(); ok {
		for _, setting := range info.Settings {
			if setting.Key == "vcs.revision" {
				build += ";vcs=" + setting.Value
			}
			if setting.Key == "vcs.modified" {
				build += ";modified=" + setting.Value
			}
		}
	}
	value, _ := trimTunUTF8(build, 240)
	return value
}
