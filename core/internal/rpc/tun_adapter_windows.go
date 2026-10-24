//go:build windows

package rpc

import (
	"ThroneCore/gen"
	"errors"
	"golang.org/x/sys/windows"
	"strings"
	"syscall"
)

type tunAdapterAnalysis struct {
	retryable  bool
	diagnostic *gen.TunStartupDiagnostic
}

// Retry policy below is intentionally unchanged. This separate bounded walk only
// collects evidence: its limits can never authorize or suppress a policy retry.
func analyzeTunAdapterError(err error) tunAdapterAnalysis {
	result := tunAdapterAnalysis{retryable: retryableTunAdapterError(err)}
	tunSeen, configureSeen, relevant := false, false, false
	var createCode, openCode uint32
	var causes []*gen.TunStartupCause
	nodes := 0
	truncated := false
	addCause := func(step gen.TunStartupStep, code *uint32) {
		for _, existing := range causes {
			if existing.GetStep() == step && (existing.Win32Code == nil) == (code == nil) && (code == nil || existing.GetWin32Code() == *code) {
				return
			}
		}
		if len(causes) >= 8 {
			truncated = true
			return
		}
		causes = append(causes, &gen.TunStartupCause{Step: To(step), Win32Code: code})
	}
	var visit func(error, bool, bool, gen.TunStartupStep, int)
	visit = func(current error, tun, configure bool, step gen.TunStartupStep, depth int) {
		if current == nil {
			return
		}
		nodes++
		if nodes > 64 || depth > 32 {
			truncated = true
			return
		}
		if one, ok := current.(interface{ Unwrap() error }); ok && one.Unwrap() != nil {
			cause := one.Unwrap()
			label := strings.TrimSuffix(current.Error(), ": "+cause.Error())
			switch label {
			case "create adapter":
				step = gen.TunStartupStep_TUN_CREATE_ADAPTER
			case "open existing adapter":
				step = gen.TunStartupStep_TUN_OPEN_ADAPTER
			case "set ipv4 address":
				step = gen.TunStartupStep_TUN_SET_IPV4_ADDRESS
			case "set ipv6 address":
				step = gen.TunStartupStep_TUN_SET_IPV6_ADDRESS
			case "set ipv4 dns":
				step = gen.TunStartupStep_TUN_SET_IPV4_DNS
			case "set ipv6 dns":
				step = gen.TunStartupStep_TUN_SET_IPV6_DNS
			case "set ipv4 options":
				step = gen.TunStartupStep_TUN_SET_IPV4_OPTIONS
			case "set ipv6 options":
				step = gen.TunStartupStep_TUN_SET_IPV6_OPTIONS
			}
		}
		flattened := flattenedTunConfigure.MatchString(current.Error())
		tun = tun || flattened || strings.HasPrefix(current.Error(), "start inbound/tun[")
		configure = configure || flattened || strings.HasPrefix(current.Error(), "configure tun interface: ")
		tunSeen = tunSeen || tun
		configureSeen = configureSeen || configure
		relevant = relevant || (tun && configure) // Same path, never combine unrelated siblings.
		if many, ok := current.(interface{ Unwrap() []error }); ok {
			for _, child := range many.Unwrap() {
				visit(child, tun, configure, step, depth+1)
			}
		} else if one, ok := current.(interface{ Unwrap() error }); ok && one.Unwrap() != nil {
			visit(one.Unwrap(), tun, configure, step, depth+1)
		} else if tun && configure {
			if code, ok := current.(syscall.Errno); ok {
				value := uint32(code)
				addCause(step, &value)
				if step == gen.TunStartupStep_TUN_CREATE_ADAPTER {
					createCode = recognizedTunWinCode(current)
				}
				if step == gen.TunStartupStep_TUN_OPEN_ADAPTER {
					openCode = recognizedTunWinCode(current)
				}
			} else {
				addCause(step, nil)
			}
		}
	}
	visit(err, false, false, gen.TunStartupStep_TUN_STEP_UNKNOWN, 0)
	if tunSeen || configureSeen {
		reason := gen.TunStartupReason_TUN_DIAGNOSTIC_UNAVAILABLE
		if retryableTunIPv4AddressError(err) {
			reason = gen.TunStartupReason_TUN_IPV4_ADDRESS_NOT_FOUND
		} else if result.retryable {
			reason = gen.TunStartupReason_TUN_ADAPTER_EXACT_PAIR
		} else if !relevant {
			reason = gen.TunStartupReason_TUN_WRONG_STAGE
		} else if createCode != 0 || openCode != 0 {
			reason = gen.TunStartupReason_TUN_ADAPTER_INCOMPLETE_PAIR
		}
		result.diagnostic = &gen.TunStartupDiagnostic{TunStage: To(tunSeen), ConfigureStage: To(configureSeen), CreateCode: To(createCode), OpenCode: To(openCode), Reason: To(reason)}
		if relevant {
			d := result.diagnostic
			d.Causes = causes
			d.TraversalTruncated = To(truncated)
			d.DiagnosticVersion = To(uint32(2))
			detail, cut, redacted := sanitizeTunErrorDetail(err.Error())
			d.ErrorDetail = To(detail)
			d.DetailTruncated = To(cut)
			d.DetailRedacted = To(redacted)
			d.CoreBuild = To(tunDiagnosticBuild())
		}
	}
	return result
}
func recognizedTunWinCode(err error) uint32 {
	for _, code := range []windows.Errno{windows.ERROR_ALREADY_EXISTS, windows.ERROR_FILE_EXISTS, windows.ERROR_NOT_FOUND, windows.ERROR_ACCESS_DENIED, windows.ERROR_INVALID_PARAMETER, windows.ERROR_GEN_FAILURE, windows.ERROR_NOT_ENOUGH_MEMORY, windows.ERROR_NO_SYSTEM_RESOURCES} {
		if errors.Is(err, code) {
			return uint32(code)
		}
	}
	return 0
}

// Field-confirmed IPv4 configuration subtype. Require real single-cause
// ancestry and an actual errno leaf, not text, unrelated siblings or mixed errors.
// The failed native configure path and Start-owned resources are closed before
// this result is published; the GUI then recreates the whole config, bounded.
func retryableTunIPv4AddressError(err error) bool {
	tun, configure, address := false, false, false
	for err != nil {
		if _, many := err.(interface{ Unwrap() []error }); many {
			return false
		}
		if one, ok := err.(interface{ Unwrap() error }); ok && one.Unwrap() != nil {
			cause := one.Unwrap()
			label := strings.TrimSuffix(err.Error(), ": "+cause.Error())
			switch {
			case strings.HasPrefix(label, "start inbound/tun[") && strings.HasSuffix(label, "]"):
				if tun || configure || address {
					return false
				}
				tun = true
			case label == "configure tun interface":
				if !tun || configure || address {
					return false
				}
				configure = true
			case label == "set ipv4 address":
				if !tun || !configure || address {
					return false
				}
				address = true
			case strings.HasPrefix(label, "set ipv"):
				return false
			}
			err = cause
			continue
		}
		code, typed := err.(syscall.Errno)
		return tun && configure && address && typed && code == windows.ERROR_NOT_FOUND
	}
	return false
}

// Original create/open policy remains intact, plus only the evidenced subtype.
func retryableTunAdapterError(err error) bool {
	if retryableTunIPv4AddressError(err) {
		return true
	}
	var visit func(error, bool, bool) bool
	visit = func(current error, tun, configure bool) bool {
		if current == nil {
			return false
		}
		tun = tun || strings.HasPrefix(current.Error(), "start inbound/tun[")
		configure = configure || strings.HasPrefix(current.Error(), "configure tun interface: ")
		if many, ok := current.(interface{ Unwrap() []error }); ok {
			children := many.Unwrap()
			if tun && configure && len(children) == 2 {
				create, open := false, false
				for _, child := range children {
					one, ok := child.(interface{ Unwrap() error })
					if !ok {
						continue
					}
					cause := one.Unwrap()
					if cause == nil {
						continue
					}
					label := strings.TrimSuffix(child.Error(), ": "+cause.Error())
					if label == "create adapter" && (errors.Is(cause, windows.ERROR_ALREADY_EXISTS) || errors.Is(cause, windows.ERROR_FILE_EXISTS)) {
						create = true
					}
					if label == "open existing adapter" && errors.Is(cause, windows.ERROR_NOT_FOUND) {
						open = true
					}
				}
				if create && open {
					return true
				}
			}
			for _, child := range children {
				if visit(child, tun, configure) {
					return true
				}
			}
		} else if one, ok := current.(interface{ Unwrap() error }); ok {
			return visit(one.Unwrap(), tun, configure)
		}
		return false
	}
	return visit(err, false, false)
}
