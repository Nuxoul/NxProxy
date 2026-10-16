//go:build !windows

package rpc

func retryableTunAdapterError(error) bool { return false }
