package rpc

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"net"
	"os"
	"strings"
	"testing"
	"time"

	"ThroneCore/gen"
	"ThroneCore/internal/process"
	"ThroneCore/internal/xray"
	"ThroneCore/internal/xraydns"

	"github.com/google/shlex"
	"github.com/xtls/xray-core/core"
	_ "github.com/xtls/xray-core/main/distro/all"
)

const lifecycleBoxConfig = `{"log":{"disabled":true},"outbounds":[{"type":"direct","tag":"direct"}]}`

// Run in a separate child, selected by arguments rather than the THRONE-filtered environment.
func TestLifecycleChild(t *testing.T) {
	for i, arg := range os.Args {
		if arg != "lifecycle-child" || i+2 >= len(os.Args) {
			continue
		}
		conn, err := net.DialTimeout("tcp", os.Args[i+1], 5*time.Second)
		if err != nil {
			os.Exit(2)
		}
		defer conn.Close()
		if _, err := os.ReadFile(os.Args[i+2]); err != nil {
			os.Exit(3)
		}
		fmt.Fprintln(conn, os.Args[i+2])
		var b [1]byte
		_, _ = conn.Read(b[:])
		return
	}
}

// Confirms a real child has read the generated config before allowing the next startup stage.
func lifecycleChild(t *testing.T, req *gen.LoadConfigReq) (*lifecycleDependencies, *net.Conn, *string) {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = listener.Close() })
	exe, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	req.NeedExtraProcess = To(true)
	req.ExtraProcessPath = To(exe)
	req.ExtraProcessArgs = To("-test.run=^TestLifecycleChild$ -- lifecycle-child " + listener.Addr().String() + ` "%s"`)
	req.ExtraProcessConf = To("lifecycle regression config")
	req.ExtraNoOut = To(true)
	var conn net.Conn
	var configPath string
	deps := &lifecycleDependencies{prepareXray: xrayPreparer}
	deps.startExtra = func(p *process.Process) error {
		if err := p.Start(); err != nil {
			return err
		}
		t.Cleanup(p.Stop)
		_ = listener.(*net.TCPListener).SetDeadline(time.Now().Add(10 * time.Second))
		var err error
		conn, err = listener.Accept()
		if err != nil {
			return fmt.Errorf("child readiness: %w", err)
		}
		t.Cleanup(func() { _ = conn.Close() })
		_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
		configPath, err = bufio.NewReader(conn).ReadString('\n')
		configPath = strings.TrimSpace(configPath)
		if err != nil {
			return fmt.Errorf("child config readiness: %w", err)
		}
		_ = conn.SetReadDeadline(time.Time{})
		return nil
	}
	return deps, &conn, &configPath
}

func assertLifecycleChildStopped(t *testing.T, conn net.Conn, configPath string) {
	t.Helper()
	if conn == nil || configPath == "" {
		t.Fatal("extra child never reached readiness; downstream fault was not exercised")
	}
	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	var b [1]byte
	if _, err := conn.Read(b[:]); err == nil {
		t.Fatal("child connection remained open")
	} else if e, ok := err.(net.Error); ok && e.Timeout() {
		t.Fatal("extra child was not terminated")
	}
	deadline := time.Now().Add(5 * time.Second)
	for {
		if _, err := os.Stat(configPath); os.IsNotExist(err) {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("temporary config still exists: %s", configPath)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func lifecycleXrayConfig(t *testing.T) (string, string) {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	address := listener.Addr().String()
	port := listener.Addr().(*net.TCPAddr).Port
	_ = listener.Close()
	return fmt.Sprintf(`{"log":{"loglevel":"none"},"inbounds":[{"listen":"127.0.0.1","port":%d,"protocol":"socks","settings":{"auth":"noauth"}}],"outbounds":[{"protocol":"freedom"}]}`, port), address
}

func assertLifecyclePortFree(t *testing.T, address string) {
	t.Helper()
	listener, err := net.Listen("tcp", address)
	if err != nil {
		t.Fatalf("sidecar port not released: %s: %v", address, err)
	}
	_ = listener.Close()
}

func assertLifecycleEmpty(t *testing.T) {
	t.Helper()
	stateMu.RLock()
	defer stateMu.RUnlock()
	if boxInstance != nil || instanceCancel != nil || xrayInstance != nil || xrayGate != nil || len(xrayFullGates) != 0 || extraProcess != nil || autoRedirectMark.Load() != 0 {
		t.Fatal("lifecycle retained resources after cleanup")
	}
}

func stopLifecycle(t *testing.T) {
	t.Helper()
	resp, err := (&server{}).Stop(context.Background(), &gen.EmptyReq{})
	if err != nil || resp.GetError() != "" {
		t.Fatalf("Stop: %v, %s", err, resp.GetError())
	}
}

func TestLifecycleStartFailureCleanup(t *testing.T) {
	for _, fault := range []string{"xray-config", "lazy-config", "prepare", "xray-start", "full-gate", "box-decode", "box-create", "box-start", "eager-box"} {
		t.Run(fault, func(t *testing.T) {
			stopLifecycle(t)
			t.Cleanup(func() { stopLifecycle(t) })
			cfg, address := lifecycleXrayConfig(t)
			fullCfg, fullAddress := lifecycleXrayConfig(t)
			req := &gen.LoadConfigReq{CoreConfig: To(lifecycleBoxConfig), NeedXray: To(true), XrayConfig: To(cfg)}
			deps, conn, configPath := lifecycleChild(t, req)
			var prepared *core.Instance
			basePrepare := deps.prepareXray
			deps.prepareXray = func(strategy string, provider xraydns.BoxProvider) func(*core.Instance) error {
				prepare := basePrepare(strategy, provider)
				return func(instance *core.Instance) error {
					prepared = instance
					if fault == "prepare" {
						return errors.New("injected prepare failure")
					}
					return prepare(instance)
				}
			}
			switch fault {
			case "xray-config":
				req.XrayConfig = To("{")
			case "lazy-config":
				req.XrayLazyStart = To(true)
				req.XrayConfig = To("{")
			case "xray-start":
				occupied, err := net.Listen("tcp", address)
				if err != nil {
					t.Fatal(err)
				}
				t.Cleanup(func() { _ = occupied.Close() })
			case "full-gate":
				req.XrayFullConfigs = []string{fullCfg, "{"}
			case "box-decode", "box-create", "box-start":
				req.XrayLazyStart = To(true)
				req.XrayFullConfigs = []string{fullCfg}
				switch fault {
				case "box-decode":
					req.CoreConfig = To("{")
				case "box-create":
					req.CoreConfig = To(`{"outbounds":[{"type":"socks","server":"127.0.0.1","server_port":1,"version":"invalid"}]}`)
				case "box-start":
					occupied, err := net.Listen("tcp", "127.0.0.1:0")
					if err != nil {
						t.Fatal(err)
					}
					t.Cleanup(func() { _ = occupied.Close() })
					req.CoreConfig = To(fmt.Sprintf(`{"log":{"disabled":true},"inbounds":[{"type":"mixed","listen":"127.0.0.1","listen_port":%d}],"outbounds":[{"type":"direct"}]}`, occupied.Addr().(*net.TCPAddr).Port))
				}
			case "eager-box":
				req.CoreConfig = To("{")
			}
			resp, err := (&server{}).start(context.Background(), req, *deps)
			if err != nil || resp.GetError() == "" {
				t.Fatalf("expected %s startup failure: %v, %s", fault, err, resp.GetError())
			}
			wantError := map[string]string{
				"xray-config": "failed to read config", "lazy-config": "parse config",
				"prepare": "injected prepare failure", "xray-start": "failed to listen",
				"full-gate": "parse config", "box-decode": "decode config",
				"box-create": "create service", "box-start": "start service", "eager-box": "decode config",
			}[fault]
			if !strings.Contains(resp.GetError(), wantError) {
				t.Fatalf("wrong fault stage for %s: %s", fault, resp.GetError())
			}
			if (fault == "prepare" || fault == "xray-start" || fault == "full-gate" || fault == "eager-box") && prepared == nil {
				t.Fatal("eager Xray never reached prepare")
			}
			t.Logf("%s: %s", fault, resp.GetError())
			assertLifecycleChildStopped(t, *conn, *configPath)
			assertLifecycleEmpty(t)
			if prepared != nil && prepared.IsRunning() {
				t.Fatal("eager Xray instance still running")
			}
			if fault != "xray-start" {
				assertLifecyclePortFree(t, address)
			}
			assertLifecyclePortFree(t, fullAddress)
			stopLifecycle(t)
			stopLifecycle(t)
			// A failed attempt must not poison a subsequent real Start.
			resp, err = (&server{}).Start(context.Background(), &gen.LoadConfigReq{CoreConfig: To(lifecycleBoxConfig)})
			if err != nil || resp.GetError() != "" || currentBox() == nil {
				t.Fatalf("restart after %s: %v, %s", fault, err, resp.GetError())
			}
			stopLifecycle(t)
			assertLifecycleEmpty(t)
		})
	}
}

func TestLifecycleDuplicateStartPreservesRunningInstance(t *testing.T) {
	for _, lazy := range []bool{false, true} {
		t.Run(fmt.Sprintf("lazy=%t", lazy), func(t *testing.T) {
			testLifecycleDuplicateStart(t, lazy)
		})
	}
}

func testLifecycleDuplicateStart(t *testing.T, lazy bool) {
	stopLifecycle(t)
	t.Cleanup(func() { stopLifecycle(t) })
	cfg, address := lifecycleXrayConfig(t)
	fullCfg, fullAddress := lifecycleXrayConfig(t)
	req := &gen.LoadConfigReq{CoreConfig: To(lifecycleBoxConfig), NeedXray: To(true), XrayConfig: To(cfg), XrayLazyStart: To(lazy), XrayFullConfigs: []string{fullCfg}}
	deps, conn, configPath := lifecycleChild(t, req)
	resp, err := (&server{}).start(context.Background(), req, *deps)
	if err != nil || resp.GetError() != "" {
		t.Fatalf("initial Start: %v, %s", err, resp.GetError())
	}
	box, cancel := currentInstance()
	instance, gate, fullGate, extra := xrayInstance, xrayGate, xrayFullGates[0], extraProcess
	autoRedirectMark.Store(0x1234) // The rejection must not reset an existing Linux exemption either.
	resp, err = (&server{}).Start(context.Background(), &gen.LoadConfigReq{CoreConfig: To("{")})
	if err != nil || resp.GetError() != "instance already started" {
		t.Fatalf("duplicate Start: %v, %s", err, resp.GetError())
	}
	if currentBox() != box || cancel == nil || box.Context().Err() != nil || xrayInstance != instance || xrayGate != gate || len(xrayFullGates) != 1 || xrayFullGates[0] != fullGate || extraProcess != extra || autoRedirectMark.Load() != 0x1234 {
		t.Fatal("duplicate Start modified original ownership")
	}
	if instance != nil && !instance.IsRunning() {
		t.Fatal("duplicate Start stopped the original eager Xray")
	}
	if _, err := os.Stat(*configPath); err != nil {
		t.Fatalf("duplicate Start deleted original config: %v", err)
	}
	// The control connection must still be alive; otherwise the original child was killed.
	_ = (*conn).SetReadDeadline(time.Now().Add(50 * time.Millisecond))
	var b [1]byte
	_, err = (*conn).Read(b[:])
	if e, ok := err.(net.Error); !ok || !e.Timeout() {
		t.Fatalf("original child died on duplicate Start: %v", err)
	}
	_ = (*conn).SetReadDeadline(time.Time{})
	stopLifecycle(t)
	stopLifecycle(t)
	assertLifecycleChildStopped(t, *conn, *configPath)
	assertLifecyclePortFree(t, address)
	assertLifecyclePortFree(t, fullAddress)
	assertLifecycleEmpty(t)
}

func TestLifecycleStopWithoutBoxCleansSidecars(t *testing.T) {
	stopLifecycle(t)
	t.Cleanup(func() { stopLifecycle(t) })
	req := &gen.LoadConfigReq{}
	deps, conn, configPath := lifecycleChild(t, req)
	// Build sidecars without publishing any box, reproducing the old partial-start state.
	conf, cleanupPath, err := process.CreateExtraConfig(req.GetExtraProcessConf())
	if err != nil {
		t.Fatal(err)
	}
	args, err := shlex.Split(req.GetExtraProcessArgs())
	if err != nil {
		t.Fatal(err)
	}
	args[len(args)-1] = conf
	extra := process.NewProcess(req.GetExtraProcessPath(), args, true)
	extra.SetCleanupPath(cleanupPath)
	if err = deps.startExtra(extra); err != nil {
		extra.Stop()
		t.Fatal(err)
	}
	extraProcess = extra
	cfg, address := lifecycleXrayConfig(t)
	gate, err := xray.StartGate(cfg, 0, nil)
	if err != nil {
		t.Fatal(err)
	}
	setXray(nil, gate)
	fullCfg, fullAddress := lifecycleXrayConfig(t)
	fullGate, err := xray.StartGate(fullCfg, 0, nil)
	if err != nil {
		t.Fatal(err)
	}
	setXrayFullGates([]*xray.Gate{fullGate})
	watchStops := 0
	stopEgressForwardingWatch = func() { watchStops++ }
	autoRedirectMark.Store(0x1234)
	stopLifecycle(t)
	stopLifecycle(t)
	if watchStops != 1 {
		t.Fatalf("watch cleanup called %d times, want 1", watchStops)
	}
	assertLifecycleChildStopped(t, *conn, *configPath)
	assertLifecyclePortFree(t, address)
	assertLifecyclePortFree(t, fullAddress)
	assertLifecycleEmpty(t)
}

func TestLifecycleResourcesCloseIdempotent(t *testing.T) {
	cfg, address := lifecycleXrayConfig(t)
	instance, err := xray.CreateXrayInstance(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if err := instance.Start(); err != nil {
		_ = instance.Close()
		t.Fatal(err)
	}
	gateCfg, gateAddress := lifecycleXrayConfig(t)
	gate, err := xray.StartGate(gateCfg, 0, nil)
	if err != nil {
		_ = instance.Close()
		t.Fatal(err)
	}
	cancels := 0
	resources := lifecycleResources{xray: instance, gate: gate, cancel: func() { cancels++ }}
	t.Cleanup(resources.close)
	resources.close()
	resources.close()
	if cancels != 1 || instance.IsRunning() || resources.xray != nil || resources.gate != nil || resources.cancel != nil {
		t.Fatal("resource cleanup was not idempotent")
	}
	assertLifecyclePortFree(t, address)
	assertLifecyclePortFree(t, gateAddress)
}
