//go:build !windows

package rpc

import "ThroneCore/gen"

type tunAdapterAnalysis struct {
	retryable  bool
	diagnostic *gen.TunStartupDiagnostic
}

func analyzeTunAdapterError(error) tunAdapterAnalysis { return tunAdapterAnalysis{} }
func retryableTunAdapterError(err error) bool         { return analyzeTunAdapterError(err).retryable }
