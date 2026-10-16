//go:build windows

package rpc

import (
	"errors"
	"strings"

	"golang.org/x/sys/windows"
)

// Only trust the typed Win32 errors in the two named sing-tun branches. Their
// localized Error() text is not a recovery policy. Restrict this to a TUN
// configure failure, never a similarly numbered file/configuration failure.
func retryableTunAdapterError(err error) bool {
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
