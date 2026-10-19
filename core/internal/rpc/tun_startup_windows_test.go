//go:build windows

package rpc

import (
	"context"
	"fmt"
	"reflect"
	"testing"

	"ThroneCore/gen"
	"ThroneCore/internal/boxbox"

	E "github.com/sagernet/sing/common/exceptions"
	"golang.org/x/sys/windows"
)

func transientTunAdapterError() error {
	return E.Cause(E.Cause(E.Cause(E.Errors(
		E.Cause(windows.ERROR_ALREADY_EXISTS, "create adapter"),
		E.Cause(windows.ERROR_NOT_FOUND, "open existing adapter")), "configure tun interface"), "start inbound/tun[tun-in]"), "start service")
}

func TestTunStartupFailureResponseReclaimsAttempt(t *testing.T) {
	stopLifecycle(t)
	t.Cleanup(func() { stopLifecycle(t) })
	req := &gen.LoadConfigReq{CoreConfig: To(lifecycleBoxConfig)}
	deps, conn, path := lifecycleChild(t, req)
	deps.createBox = func([]byte, func(*boxbox.Box)) (*boxbox.Box, context.CancelFunc, error) {
		return nil, nil, transientTunAdapterError()
	}
	response, err := (&server{}).start(context.Background(), req, *deps)
	if err != nil || response.GetError() == "" {
		t.Fatalf("wrong failure response: %v %v", response, err)
	}
	// The flag is an acknowledgment that the attempt's resources no longer own a port/child.
	assertLifecycleChildStopped(t, *conn, *path)
	assertLifecycleEmpty(t)
	if !response.GetTunAdapterRetryable() {
		t.Fatal("transient Windows TUN creation conflict was not marked recoverable")
	}
	info := response.GetTunStartupDiagnostic()
	if info == nil || !info.GetTunStage() || !info.GetConfigureStage() || info.GetCreateCode() != 183 || info.GetOpenCode() != 1168 || info.GetReason() != gen.TunStartupReason_TUN_ADAPTER_EXACT_PAIR {
		t.Fatalf("wrong bounded diagnostic: %v", info)
	}
	// A new configuration instance, without restarting the core process, must work.
	response, err = (&server{}).Start(context.Background(), &gen.LoadConfigReq{CoreConfig: To(lifecycleBoxConfig)})
	if err != nil || response.GetError() != "" || currentBox() == nil {
		t.Fatalf("restart after recovered TUN failure: %v %s", err, response.GetError())
	}
}

func TestTunStartupPermanentFailureResponse(t *testing.T) {
	stopLifecycle(t)
	t.Cleanup(func() { stopLifecycle(t) })
	req := &gen.LoadConfigReq{CoreConfig: To(lifecycleBoxConfig)}
	deps := lifecycleDependencies{prepareXray: xrayPreparer, createBox: func([]byte, func(*boxbox.Box)) (*boxbox.Box, context.CancelFunc, error) {
		return nil, nil, fmt.Errorf("configure tun interface: %w", windows.ERROR_ACCESS_DENIED)
	}}
	response, err := (&server{}).start(context.Background(), req, deps)
	if err != nil || response.GetError() == "" || response.GetTunAdapterRetryable() {
		t.Fatalf("permission failure retried: %v %v", response, err)
	}
	assertLifecycleEmpty(t)
}

// Same errno with arbitrary localized messages; no English Win32 text dependency.
type localizedWinError struct {
	code error
	text string
}

func (e localizedWinError) Error() string { return e.text }
func (e localizedWinError) Unwrap() error { return e.code }
func TestTunStartupTypedClassification(t *testing.T) {
	wrap := func(inner error) error {
		return E.Cause(E.Cause(E.Cause(inner, "configure tun interface"), "start inbound/tun[tun-in]"), "start service")
	}
	pair := func(create, open error) error {
		return E.Errors(E.Cause(create, "create adapter"), E.Cause(open, "open existing adapter"))
	}
	tests := []struct {
		name string
		err  error
		want bool
	}{
		{"actual", transientTunAdapterError(), true},
		{"localized", wrap(pair(localizedWinError{windows.ERROR_ALREADY_EXISTS, "同名设备已存在"}, localizedWinError{windows.ERROR_NOT_FOUND, "元素未找到"})), true},
		{"file exists code", wrap(pair(windows.ERROR_FILE_EXISTS, windows.ERROR_NOT_FOUND)), true},
		{"permission", wrap(pair(windows.ERROR_ACCESS_DENIED, windows.ERROR_NOT_FOUND)), false},
		{"open permission", wrap(pair(windows.ERROR_ALREADY_EXISTS, windows.ERROR_ACCESS_DENIED)), false},
		{"text only", fmt.Errorf("start inbound/tun[tun-in]: configure tun interface: (create adapter: Cannot create a file when that file already exists. | open existing adapter: Element not found.)"), false},
		{"no tun stage", E.Cause(pair(windows.ERROR_ALREADY_EXISTS, windows.ERROR_NOT_FOUND), "configure tun interface"), false},
		{"no configure stage", E.Cause(pair(windows.ERROR_ALREADY_EXISTS, windows.ERROR_NOT_FOUND), "start inbound/tun[tun-in]"), false},
		{"wrong labels", wrap(E.Errors(E.Cause(windows.ERROR_ALREADY_EXISTS, "create file"), E.Cause(windows.ERROR_NOT_FOUND, "open file"))), false},
		{"nil", nil, false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			analysis := analyzeTunAdapterError(tt.err)
			if analysis.retryable != tt.want {
				t.Fatalf("analysis changed policy: %v", analysis)
			}
			if tt.want && (analysis.diagnostic == nil || analysis.diagnostic.GetReason() != gen.TunStartupReason_TUN_ADAPTER_EXACT_PAIR) {
				t.Fatal("typed exact diagnostic missing")
			}
			if tt.name == "text only" && analysis.diagnostic != nil && analysis.diagnostic.GetCreateCode() != 0 {
				t.Fatal("text became typed errno")
			}
			if got := retryableTunAdapterError(tt.err); got != tt.want {
				t.Fatalf("classification=%v want=%v err=%v", got, tt.want, tt.err)
			}
		})
	}
}

func TestTunStartupDiagnosticField(t *testing.T) {
	if _, ok := reflect.TypeOf(gen.ErrorResp{}).FieldByName("TunStartupDiagnostic"); !ok {
		t.Fatal("bounded TUN startup diagnostic response is missing")
	}
}
