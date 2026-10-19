//go:build windows

package rpc

import (
	"ThroneCore/gen"
	"errors"
	"golang.org/x/sys/windows"
	"strings"
)

type tunAdapterAnalysis struct {
	retryable  bool
	diagnostic *gen.TunStartupDiagnostic
}

// One owner of the typed unwrap analysis. Text identifies only the existing
// sing-tun stage/branch wrappers; localized Win32 messages never grant retry.
func analyzeTunAdapterError(err error) tunAdapterAnalysis {
	result := tunAdapterAnalysis{}
	tunSeen, configureSeen := false, false
	var createCode, openCode uint32
	var visit func(error, bool, bool) bool
	visit = func(current error, tun, configure bool) bool {
		if current == nil {
			return false
		}
		tun = tun || strings.HasPrefix(current.Error(), "start inbound/tun[")
		configure = configure || strings.HasPrefix(current.Error(), "configure tun interface: ")
		tunSeen = tunSeen || tun
		configureSeen = configureSeen || configure
		if many, ok := current.(interface{ Unwrap() []error }); ok {
			children := many.Unwrap()
			if len(children) == 2 {
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
					// Only recognized Win32 numeric codes, no arbitrary unwrap text payload.
					code := recognizedTunWinCode(cause)
					if label == "create adapter" {
						createCode = code
						create = errors.Is(cause, windows.ERROR_ALREADY_EXISTS) || errors.Is(cause, windows.ERROR_FILE_EXISTS)
					}
					if label == "open existing adapter" {
						openCode = code
						open = errors.Is(cause, windows.ERROR_NOT_FOUND)
					}
				}
				if tun && configure && create && open {
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
	result.retryable = visit(err, false, false)
	if tunSeen || configureSeen {
		reason := gen.TunStartupReason_TUN_DIAGNOSTIC_UNAVAILABLE
		if result.retryable {
			reason = gen.TunStartupReason_TUN_ADAPTER_EXACT_PAIR
		} else if !tunSeen || !configureSeen {
			reason = gen.TunStartupReason_TUN_WRONG_STAGE
		} else if createCode != 0 || openCode != 0 {
			reason = gen.TunStartupReason_TUN_ADAPTER_INCOMPLETE_PAIR
		}
		result.diagnostic = &gen.TunStartupDiagnostic{TunStage: To(tunSeen), ConfigureStage: To(configureSeen), CreateCode: To(createCode), OpenCode: To(openCode), Reason: To(reason)}
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
func retryableTunAdapterError(err error) bool { return analyzeTunAdapterError(err).retryable }
