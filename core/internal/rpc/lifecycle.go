package rpc

import (
	"context"
	"fmt"
	"log"
	"net/netip"
	"runtime"
	"strings"
	"time"

	"ThroneCore/gen"
	"ThroneCore/internal/boxbox"
	"ThroneCore/internal/boxmain"
	"ThroneCore/internal/process"
	"ThroneCore/internal/sysdns"
	"ThroneCore/internal/xray"
	"ThroneCore/internal/xraydns"

	"github.com/google/shlex"
	E "github.com/sagernet/sing/common/exceptions"
	"github.com/xtls/xray-core/core"
)

func (s *server) Start(ctx context.Context, in *gen.LoadConfigReq) (*gen.ErrorResp, error) {
	return s.start(ctx, in, lifecycleDependencies{
		startExtra:  (*process.Process).Start,
		prepareXray: xrayPreparer,
	})
}

// Dependencies are per attempt so fault tests need not mutate live global state.
type lifecycleDependencies struct {
	createBox   func([]byte, func(*boxbox.Box)) (*boxbox.Box, context.CancelFunc, error)
	startExtra  func(*process.Process) error
	prepareXray func(string, xraydns.BoxProvider) func(*core.Instance) error
}

func (s *server) start(ctx context.Context, in *gen.LoadConfigReq, deps lifecycleDependencies) (out *gen.ErrorResp, _ error) {
	lifecycleMu.Lock()
	defer lifecycleMu.Unlock()

	// Reject before acquiring ownership: a rejected attempt must not unpublish the live instance.
	if currentBox() != nil {
		return &gen.ErrorResp{Error: To("instance already started")}, nil
	}

	var err error
	var resources lifecycleResources
	committed := false

	defer func() {
		if !committed {
			resources.close()
			autoRedirectMark.Store(0)
		}
		out = &gen.ErrorResp{}
		if err != nil {
			out.Error = To(err.Error())
			// The failed box/sidecars have been reclaimed before retry is offered.
			analysis := analyzeTunAdapterError(err)
			out.TunAdapterRetryable = To(analysis.retryable)
			out.TunStartupDiagnostic = analysis.diagnostic
			if out.GetTunAdapterRetryable() {
				log.Printf("[Tun recovery] Failed startup reclaimed; adapter creation conflict is retryable: %v", err)
			}
		}
	}()

	if debugLog {
		log.Println("Start:", in.GetCoreConfig())
		if in.XrayConfig != nil {
			log.Println("Start Xray:", in.GetXrayConfig())
		}
	}

	if in.GetNeedExtraProcess() {
		args, e := shlex.Split(in.GetExtraProcessArgs())
		if e != nil {
			err = E.Cause(e, "Failed to parse args")
			return
		}
		var extraConfPath, extraCleanupPath string
		if in.ExtraProcessConf != nil {
			extraConfPath, extraCleanupPath, e = process.CreateExtraConfig(*in.ExtraProcessConf)
			if e != nil {
				err = E.Cause(e, "Failed to create extra.conf")
				return
			}
			for idx, arg := range args {
				if strings.Contains(arg, "%s") {
					args[idx] = fmt.Sprintf(arg, extraConfPath)
					break
				}
			}
		}

		resources.extra = process.NewProcess(in.GetExtraProcessPath(), args, in.GetExtraNoOut())
		resources.extra.SetCleanupPath(extraCleanupPath)
		err = deps.startExtra(resources.extra)
		if err != nil {
			return
		}
	}

	autoRedirectMark.Store(autoRedirectMarkFor([]byte(in.GetCoreConfig())))

	// Filled in below, once boxmain.Create has built the box these sidecars resolve through.
	var boxCtx boxContextHolder
	prepareXray := deps.prepareXray(in.GetXrayOutboundDnsStrategy(), boxCtx.get)

	if in.GetNeedXray() {
		if in.GetXrayLazyStart() {
			gate, e := xray.StartGate(in.GetXrayConfig(),
				time.Duration(in.GetXrayIdleSeconds())*time.Second, prepareXray)
			if e != nil {
				err = e
				return
			}
			resources.gate = gate
		} else {
			instance, e := xray.CreateXrayInstance(in.GetXrayConfig())
			if e != nil {
				err = e
				return
			}
			resources.xray = instance
			if e = prepareXray(instance); e != nil {
				err = e
				return
			}
			if e = instance.Start(); e != nil {
				err = e
				return
			}
		}
	}

	if fullConfigs := in.GetXrayFullConfigs(); len(fullConfigs) > 0 {
		gates, e := startXrayFullGates(fullConfigs,
			time.Duration(in.GetXrayFullIdleSeconds())*time.Second, prepareXray)
		if e != nil {
			err = e
			return
		}
		resources.fullGates = gates
	}

	createBox := deps.createBox
	if createBox == nil {
		createBox = func(config []byte, publish func(*boxbox.Box)) (*boxbox.Box, context.CancelFunc, error) {
			return boxmain.Create(config, publish)
		}
	}
	resources.box, resources.cancel, err = createBox([]byte(in.GetCoreConfig()), boxCtx.publish)
	if err != nil {
		return
	}
	box := resources.box

	if runtime.GOOS == "darwin" && in.GetTunIpv4Cidr() != "" {
		tunCIDR := in.GetTunIpv4Cidr()
		tunPrefix, parseErr := netip.ParsePrefix(tunCIDR)
		if parseErr != nil || !tunPrefix.Addr().Is4() {
			err = fmt.Errorf("invalid tun_ipv4_cidr %q", tunCIDR)
			return
		}

		tunDNS := tunPrefix.Addr()
		if !tunDNS.IsValid() || !tunDNS.Is4() {
			err = fmt.Errorf("got invalid DNS IP from tun_ipv4_cidr: %s", tunDNS)
			return
		}

		if err := sysdns.SetSystemDNS(tunDNS.String(), box.Network().InterfaceMonitor()); err != nil {
			log.Println("Failed to set system DNS:", err)
		}

		needUnsetDNS = true
	}

	if in.GetTunIpv4Cidr() != "" {
		stopEgressForwardingWatch = watchEgressForwarding(box.Network())
	}

	// Publish only once the attempt has completed. The running lifecycle now owns these resources.
	stateMu.Lock()
	boxInstance, instanceCancel = resources.box, resources.cancel
	xrayInstance, xrayGate, xrayFullGates = resources.xray, resources.gate, resources.fullGates
	stateMu.Unlock()
	extraProcess = resources.extra
	committed = true
	return
}

// Owned by one Start attempt or detached by Stop. Clearing each slot makes cleanup idempotent.
type lifecycleResources struct {
	box       *boxbox.Box
	cancel    context.CancelFunc
	extra     *process.Process
	xray      *core.Instance
	gate      *xray.Gate
	fullGates []*xray.Gate
}

func (r *lifecycleResources) close() {
	if r.box != nil {
		box, cancel := r.box, r.cancel
		r.box, r.cancel = nil, nil
		box.CloseWithTimeout(cancel, time.Second*2, log.Println, true)
	}
	if r.cancel != nil {
		cancel := r.cancel
		r.cancel = nil
		cancel()
	}
	if r.extra != nil {
		extra := r.extra
		r.extra = nil
		extra.Stop()
	}
	if r.gate != nil {
		gate := r.gate
		r.gate = nil
		gate.Close()
	}
	gates := r.fullGates
	r.fullGates = nil
	for _, gate := range gates {
		gate.Close()
	}
	if r.xray != nil {
		instance := r.xray
		r.xray = nil
		_ = instance.Close()
	}
}

func (s *server) Stop(ctx context.Context, in *gen.EmptyReq) (*gen.ErrorResp, error) {
	lifecycleMu.Lock()
	defer lifecycleMu.Unlock()

	box, _ := currentInstance()
	if stopEgressForwardingWatch != nil {
		stopEgressForwardingWatch()
		stopEgressForwardingWatch = nil
	}
	if needUnsetDNS {
		needUnsetDNS = false
		if box != nil {
			if err := sysdns.SetSystemDNS("Empty", box.Network().InterfaceMonitor()); err != nil {
				log.Println("Failed to unset system DNS:", err)
			}
		}
	}
	// Unpublish before closing so pollers cannot acquire dying instances, even if box is nil.
	stateMu.Lock()
	resources := lifecycleResources{
		box: boxInstance, cancel: instanceCancel,
		xray: xrayInstance, gate: xrayGate, fullGates: xrayFullGates,
		extra: extraProcess,
	}
	boxInstance, instanceCancel = nil, nil
	xrayInstance, xrayGate, xrayFullGates = nil, nil, nil
	stateMu.Unlock()
	extraProcess = nil
	resources.close()
	// The Tun and its nftables rules went down with the box.
	autoRedirectMark.Store(0)
	return &gen.ErrorResp{}, nil
}

func (s *server) CheckConfig(ctx context.Context, in *gen.LoadConfigReq) (out *gen.ErrorResp, _ error) {
	out = &gen.ErrorResp{}
	// boxmain.Check can panic on malformed configs; unrecovered it reaches main()'s os.Exit(0) and kills the core.
	defer func() {
		if r := recover(); r != nil {
			buf := make([]byte, 4096)
			n := runtime.Stack(buf, false)
			log.Printf("CheckConfig panic: %v\n%s", r, buf[:n])
			out.Error = To(fmt.Sprintf("CheckConfig panic: %v", r))
		}
	}()
	if in.GetNeedXray() {
		if err := xray.CheckXrayConfig(in.GetXrayConfig()); err != nil {
			out.Error = To(err.Error())
		}
		return
	}
	err := boxmain.Check([]byte(*in.CoreConfig))
	if err != nil {
		out.Error = To(err.Error())
	}
	return
}
