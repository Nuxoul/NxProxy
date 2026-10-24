//go:build windows

package rpc

import (
	"context"
	"errors"
	"fmt"
	"google.golang.org/protobuf/proto"
	"os"
	"reflect"
	"strings"
	"syscall"
	"testing"
	"unicode/utf8"

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

// Public Start response: previously this distinct typed failure collapsed to all zero codes.
func TestTunStartupConfigureEvidence(t *testing.T) {
	stopLifecycle(t)
	t.Cleanup(func() { stopLifecycle(t) })
	failure := E.Cause(E.Cause(E.Cause(E.Cause(windows.ERROR_DEVICE_NOT_CONNECTED, "set ipv4 address"), "configure tun interface"), "start inbound/tun[tun-in]"), "start service")
	deps := lifecycleDependencies{prepareXray: xrayPreparer, createBox: func([]byte, func(*boxbox.Box)) (*boxbox.Box, context.CancelFunc, error) { return nil, nil, failure }}
	response, err := (&server{}).start(context.Background(), &gen.LoadConfigReq{CoreConfig: To(lifecycleBoxConfig)}, deps)
	if err != nil || response.GetError() != failure.Error() || response.GetTunAdapterRetryable() {
		t.Fatalf("original response/policy changed: %v %v", response, err)
	}
	info := response.GetTunStartupDiagnostic()
	if info == nil {
		t.Fatal("configure evidence missing")
	}
	value := reflect.ValueOf(info).Elem()
	field := value.FieldByName("Causes")
	if !field.IsValid() || field.Len() != 1 {
		t.Fatal("configure failure omitted typed substage and Win32 code")
	}
	cause := field.Index(0).Elem()
	code := cause.FieldByName("Win32Code")
	stage := cause.FieldByName("Step")
	if !code.IsValid() || code.IsNil() || code.Elem().Uint() != 1167 || !stage.IsValid() || stage.IsNil() || stage.Elem().Int() != 3 {
		t.Fatalf("unexpected cause: %v", cause)
	}
	detail := value.FieldByName("ErrorDetail")
	build := value.FieldByName("CoreBuild")
	if !detail.IsValid() || detail.IsNil() || detail.Elem().String() != failure.Error() || !build.IsValid() || build.IsNil() || build.Elem().String() == "" {
		t.Fatal("original configure error detail/core identity omitted")
	}
	if path := os.Getenv("NX_TUN_DIAG_FIXTURE"); path != "" {
		encoded, e := proto.Marshal(response)
		if e != nil {
			t.Fatal(e)
		}
		if e = os.WriteFile(path, encoded, 0600); e != nil {
			t.Fatal(e)
		}
	}
	assertLifecycleEmpty(t)
}

func TestTunStartupDiagnosticSteps(t *testing.T) {
	stopLifecycle(t)
	t.Cleanup(func() { stopLifecycle(t) })
	cases := []struct {
		label string
		step  gen.TunStartupStep
		code  syscall.Errno
	}{
		{"set ipv4 address", gen.TunStartupStep_TUN_SET_IPV4_ADDRESS, 1167},
		{"set ipv6 address", gen.TunStartupStep_TUN_SET_IPV6_ADDRESS, 87},
		{"set ipv4 dns", gen.TunStartupStep_TUN_SET_IPV4_DNS, 5},
		{"set ipv6 dns", gen.TunStartupStep_TUN_SET_IPV6_DNS, 1168},
		{"set ipv4 options", gen.TunStartupStep_TUN_SET_IPV4_OPTIONS, 50},
		{"set ipv6 options", gen.TunStartupStep_TUN_SET_IPV6_OPTIONS, 1450},
		{"create adapter", gen.TunStartupStep_TUN_CREATE_ADAPTER, 31},
		{"open existing adapter", gen.TunStartupStep_TUN_OPEN_ADAPTER, 2},
	}
	for _, c := range cases {
		t.Run(c.label, func(t *testing.T) {
			failure := E.Cause(E.Cause(E.Cause(c.code, c.label), "configure tun interface"), "start inbound/tun[tun-in]")
			deps := lifecycleDependencies{prepareXray: xrayPreparer, createBox: func([]byte, func(*boxbox.Box)) (*boxbox.Box, context.CancelFunc, error) { return nil, nil, failure }}
			response, err := (&server{}).start(context.Background(), &gen.LoadConfigReq{CoreConfig: To(lifecycleBoxConfig)}, deps)
			d := response.GetTunStartupDiagnostic()
			if err != nil || response.GetTunAdapterRetryable() || d == nil || len(d.GetCauses()) != 1 || d.Causes[0].GetStep() != c.step || d.Causes[0].Win32Code == nil || d.Causes[0].GetWin32Code() != uint32(c.code) || d.GetErrorDetail() != failure.Error() {
				t.Fatalf("distinct evidence lost: %v %v", response, err)
			}
		})
	}
	// Localized descriptions keep typed code; plain text never becomes errno.
	for _, inner := range []error{localizedWinError{syscall.Errno(1167), "设备尚未连接"}, fmt.Errorf("设备尚未连接 (1167)")} {
		failure := E.Cause(E.Cause(E.Cause(inner, "set ipv4 address"), "configure tun interface"), "start inbound/tun[tun-in]")
		a := analyzeTunAdapterError(failure)
		d := a.diagnostic
		if a.retryable || len(d.Causes) != 1 || d.Causes[0].GetStep() != gen.TunStartupStep_TUN_SET_IPV4_ADDRESS {
			t.Fatal("localized/untyped stage lost")
		}
		_, typed := inner.(localizedWinError)
		if (d.Causes[0].Win32Code != nil) != typed {
			t.Fatal("text inferred numeric code or localized typing lost")
		}
	}
}
func TestTunStartupDiagnosticPrivacyAndBounds(t *testing.T) {
	raw := `start inbound/tun[tun-in]: configure tun interface: set ipv4 dns: failed https://user:pw@private.example/sub?token=abc password=secret uuid=11111111-2222-3333-4444-555555555555 C:\Private\file 203.0.113.9 "private-node" {"password":"jsonsecret"}`
	d, cut, redacted := sanitizeTunErrorDetail(raw)
	if cut || !redacted || !strings.Contains(d, "set ipv4 dns") {
		t.Fatalf("sanitization damaged stage: %q", d)
	}
	for _, secret := range []string{"user:pw", "private.example", "token=abc", "secret", "11111111-2222", "Private", "203.0.113.9", "private-node"} {
		if strings.Contains(d, secret) {
			t.Fatalf("secret retained: %s", secret)
		}
	}
	d, cut, _ = sanitizeTunErrorDetail(strings.Repeat("错误链", 9000))
	if !cut || len(d) > 3072 || !utf8.ValidString(d) {
		t.Fatal("UTF8 detail cap not explicit")
	}
	many := []error{}
	for i := 0; i < 12; i++ {
		many = append(many, E.Cause(syscall.Errno(1000+i), "set ipv4 dns"))
	}
	a := analyzeTunAdapterError(E.Cause(E.Cause(E.Errors(many...), "configure tun interface"), "start inbound/tun[tun-in]"))
	if len(a.diagnostic.Causes) != 8 || !a.diagnostic.GetTraversalTruncated() || a.retryable {
		t.Fatal("cause cap/policy wrong")
	}
	var deep error = syscall.Errno(1167)
	for i := 0; i < 40; i++ {
		deep = E.Cause(deep, "layer")
	}
	a = analyzeTunAdapterError(E.Cause(E.Cause(deep, "configure tun interface"), "start inbound/tun[tun-in]"))
	if !a.diagnostic.GetTraversalTruncated() {
		t.Fatal("deep tree not bounded")
	}
}

func TestTunStartupDiagnosticReviewBoundaries(t *testing.T) {
	wrap := func(inner error) error {
		return E.Cause(E.Cause(inner, "configure tun interface"), "start inbound/tun[tun-in]")
	}
	pair := E.Errors(E.Cause(windows.ERROR_ALREADY_EXISTS, "create adapter"), E.Cause(windows.ERROR_NOT_FOUND, "open existing adapter"))
	t.Run("deep exact pair keeps old retry policy", func(t *testing.T) {
		var inner error = pair
		for i := 0; i < 40; i++ {
			inner = E.Cause(inner, "layer")
		}
		a := analyzeTunAdapterError(wrap(inner))
		if !a.retryable || !a.diagnostic.GetTraversalTruncated() {
			t.Fatal("diagnostic depth cap changed exact-pair retry policy")
		}
	})
	t.Run("wide exact pair keeps old retry policy", func(t *testing.T) {
		children := []error{}
		for i := 0; i < 70; i++ {
			children = append(children, fmt.Errorf("unrelated %d", i))
		}
		children = append(children, pair)
		a := analyzeTunAdapterError(wrap(errors.Join(children...)))
		if !a.retryable || !a.diagnostic.GetTraversalTruncated() {
			t.Fatal("diagnostic node cap changed exact-pair retry policy")
		}
	})
	t.Run("wrong stage has no rich payload", func(t *testing.T) {
		for _, failure := range []error{E.Cause(pair, "configure tun interface"), E.Cause(pair, "start inbound/tun[tun-in]"), E.Errors(E.Cause(fmt.Errorf("a"), "configure tun interface"), E.Cause(fmt.Errorf("b"), "start inbound/tun[tun-in]"))} {
			a := analyzeTunAdapterError(failure)
			if a.retryable || a.diagnostic == nil || a.diagnostic.GetErrorDetail() != "" || len(a.diagnostic.Causes) != 0 || a.diagnostic.GetCoreBuild() != "" {
				t.Fatal("wrong-stage chain got free-form payload")
			}
		}
	})
	t.Run("space paths fully redacted", func(t *testing.T) {
		for _, path := range []string{`C:\Users\Alice Smith\Documents\profile`, `\\server\Alice Smith\private profile`} {
			detail, _, redacted := sanitizeTunErrorDetail("set ipv4 dns: " + path + " | device unavailable")
			if !redacted || strings.Contains(detail, "Smith") || strings.Contains(detail, "Documents") || strings.Contains(detail, "profile") {
				t.Fatalf("path suffix leaked: %q", detail)
			}
		}
	})
}

func TestTunStartupFlattenedErrorEvidence(t *testing.T) {
	stopLifecycle(t)
	t.Cleanup(func() { stopLifecycle(t) })
	failure := fmt.Errorf("start service: start inbound/tun[tun-in]: configure tun interface: set ipv4 dns: localized device error 1167")
	deps := lifecycleDependencies{prepareXray: xrayPreparer, createBox: func([]byte, func(*boxbox.Box)) (*boxbox.Box, context.CancelFunc, error) { return nil, nil, failure }}
	response, err := (&server{}).start(context.Background(), &gen.LoadConfigReq{CoreConfig: To(lifecycleBoxConfig)}, deps)
	d := response.GetTunStartupDiagnostic()
	if err != nil || response.GetTunAdapterRetryable() || d == nil || d.GetErrorDetail() != failure.Error() || len(d.Causes) != 1 || d.Causes[0].Win32Code != nil || d.Causes[0].GetStep() != gen.TunStartupStep_TUN_STEP_UNKNOWN {
		t.Fatal("flattened error lost original evidence or inferred typed code/step")
	}
}

func fieldTunIPv4NotFoundError(cause error) error {
	return E.Cause(E.Cause(E.Cause(E.Cause(cause, "set ipv4 address"), "configure tun interface"), "start inbound/tun[tun-in]"), "start service")
}

// Exact field error, public response and actual attempt-owned child cleanup.
func TestTunStartupIPv4NotFoundResponse(t *testing.T) {
	stopLifecycle(t)
	t.Cleanup(func() { stopLifecycle(t) })
	req := &gen.LoadConfigReq{CoreConfig: To(lifecycleBoxConfig)}
	deps, conn, path := lifecycleChild(t, req)
	failure := fieldTunIPv4NotFoundError(windows.ERROR_NOT_FOUND)
	deps.createBox = func([]byte, func(*boxbox.Box)) (*boxbox.Box, context.CancelFunc, error) { return nil, nil, failure }
	response, err := (&server{}).start(context.Background(), req, *deps)
	if err != nil || response.GetError() != failure.Error() {
		t.Fatalf("original field error changed: %v %v", response, err)
	}
	assertLifecycleChildStopped(t, *conn, *path)
	assertLifecycleEmpty(t)
	if !response.GetTunAdapterRetryable() {
		t.Fatal("field-confirmed typed IPv4 address1168 did not enter bounded recovery after cleanup")
	}
	info := response.GetTunStartupDiagnostic()
	if info == nil || int32(info.GetReason()) != 4 || len(info.Causes) != 1 || info.Causes[0].GetStep() != gen.TunStartupStep_TUN_SET_IPV4_ADDRESS || info.Causes[0].GetWin32Code() != 1168 || info.GetErrorDetail() != failure.Error() {
		t.Fatalf("IPv4 recovery evidence lost: %v", info)
	}
	if path := os.Getenv("NX_TUN_IPV4_FIXTURE"); path != "" {
		data, e := proto.Marshal(response)
		if e != nil {
			t.Fatal(e)
		}
		if e = os.WriteFile(path, data, 0600); e != nil {
			t.Fatal(e)
		}
	}
	response, err = (&server{}).Start(context.Background(), &gen.LoadConfigReq{CoreConfig: To(lifecycleBoxConfig)})
	if err != nil || response.GetError() != "" || currentBox() == nil {
		t.Fatalf("fresh configuration after IPv4 failure did not start: %v %v", response, err)
	}
}

func TestTunStartupIPv4NotFoundMatrix(t *testing.T) {
	stopLifecycle(t)
	t.Cleanup(func() { stopLifecycle(t) })
	scoped := func(label string, cause error) error {
		return E.Cause(E.Cause(E.Cause(E.Cause(cause, label), "configure tun interface"), "start inbound/tun[tun-in]"), "start service")
	}
	ipv4 := E.Cause(windows.ERROR_NOT_FOUND, "set ipv4 address")
	cases := []struct {
		name string
		err  error
		want bool
	}{
		{"field", fieldTunIPv4NotFoundError(windows.ERROR_NOT_FOUND), true},
		{"localized", fieldTunIPv4NotFoundError(localizedWinError{windows.ERROR_NOT_FOUND, "元素未找到"}), true},
		{"typed fmt wrapper", fieldTunIPv4NotFoundError(fmt.Errorf("native: %w", windows.ERROR_NOT_FOUND)), true},
		{"access denied", fieldTunIPv4NotFoundError(windows.ERROR_ACCESS_DENIED), false},
		{"invalid parameter", fieldTunIPv4NotFoundError(windows.ERROR_INVALID_PARAMETER), false},
		{"device not connected1167", fieldTunIPv4NotFoundError(windows.ERROR_DEVICE_NOT_CONNECTED), false},
		{"DNS1168", scoped("set ipv4 dns", windows.ERROR_NOT_FOUND), false},
		{"IPv6 address1168", scoped("set ipv6 address", windows.ERROR_NOT_FOUND), false},
		{"IPv4 options1168", scoped("set ipv4 options", windows.ERROR_NOT_FOUND), false},
		{"no tun", E.Cause(ipv4, "configure tun interface"), false},
		{"no configure", E.Cause(ipv4, "start inbound/tun[tun-in]"), false},
		{"wrong order", E.Cause(E.Cause(ipv4, "start inbound/tun[tun-in]"), "configure tun interface"), false},
		{"wrong label", scoped("set ipv4 addresses", windows.ERROR_NOT_FOUND), false},
		{"text lookalike", fmt.Errorf("start service: start inbound/tun[tun-in]: configure tun interface: set ipv4 address: Element not found."), false},
		{"untyped inner", fieldTunIPv4NotFoundError(fmt.Errorf("Element not found. (1168)")), false},
		{"mixed inner", fieldTunIPv4NotFoundError(errors.Join(windows.ERROR_NOT_FOUND, windows.ERROR_ACCESS_DENIED)), false},
		{"unrelated sibling", E.Errors(E.Cause(windows.ERROR_NOT_FOUND, "set ipv4 address"), E.Cause(fmt.Errorf("other"), "start inbound/tun[tun-in]: configure tun interface")), false},
		{"wrong nested operation", fieldTunIPv4NotFoundError(E.Cause(windows.ERROR_NOT_FOUND, "set ipv4 dns")), false},
		{"configure reentry", fieldTunIPv4NotFoundError(E.Cause(windows.ERROR_NOT_FOUND, "configure tun interface")), false},
		{"tun reentry", E.Cause(E.Cause(E.Cause(ipv4, "start inbound/tun[other]"), "configure tun interface"), "start inbound/tun[tun-in]"), false},
		{"duplicate outer tun", E.Cause(fieldTunIPv4NotFoundError(windows.ERROR_NOT_FOUND), "start inbound/tun[other]"), false},
		{"duplicate configure", E.Cause(E.Cause(E.Cause(ipv4, "configure tun interface"), "configure tun interface"), "start inbound/tun[tun-in]"), false},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			deps := lifecycleDependencies{prepareXray: xrayPreparer, createBox: func([]byte, func(*boxbox.Box)) (*boxbox.Box, context.CancelFunc, error) { return nil, nil, c.err }}
			response, err := (&server{}).start(context.Background(), &gen.LoadConfigReq{CoreConfig: To(lifecycleBoxConfig)}, deps)
			if err != nil || response.GetError() != c.err.Error() || response.GetTunAdapterRetryable() != c.want {
				t.Fatalf("policy=%v want=%v response=%v error=%v", response.GetTunAdapterRetryable(), c.want, response, err)
			}
			if c.want && int32(response.GetTunStartupDiagnostic().GetReason()) != 4 {
				t.Fatal("IPv4 cause reported as create/open pair")
			}
			assertLifecycleEmpty(t)
		})
	}
}
