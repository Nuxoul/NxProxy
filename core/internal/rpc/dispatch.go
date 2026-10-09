package rpc

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"log"
	"net"
	runtimeDebug "runtime/debug"
	"sync"
	"time"

	"ThroneCore/gen"
	"google.golang.org/protobuf/proto"
)

type server struct {
	gen.UnimplementedLibcoreServiceServer
}

var globalServer = &server{}
var debugLog bool

// Wire budgets, mirrored by RPC.cpp. 96 MiB requests allow a 64 MiB
// configuration plus protobuf/secondary-config overhead; 128 MiB responses
// allow 32 MiB diagnostics attachments, 32 MiB trace and profiling overhead.
const (
	maxMethodBytes     = 128
	maxRequestBytes    = 96 << 20
	maxResponseBytes   = 128 << 20
	maxControlBytes    = 64 << 10
	ordinarySlots      = 8
	controlSlots       = 4
	requestBudgetBytes = 128 << 20
	outputBudgetBytes  = 192 << 20
	frameTimeout       = 30 * time.Second
)

// Process-wide, not per reconnect: atomic-completion handlers on an old
// connection must not let a reconnect create another unbounded work pool.
var ordinaryWork = make(chan struct{}, ordinarySlots)
var controlWork = make(chan struct{}, controlSlots)
var requestBudget = byteBudget{limit: requestBudgetBytes}
var controlRequestBudget = byteBudget{limit: controlSlots * maxControlBytes}
var outputBudget = byteBudget{limit: outputBudgetBytes}
var controlOutputBudget = byteBudget{limit: controlSlots * maxControlBytes}

type byteBudget struct {
	mu          sync.Mutex
	used, limit int64
}

func (b *byteBudget) take(n int64) bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	if n > b.limit-b.used {
		return false
	}
	b.used += n
	return true
}
func (b *byteBudget) give(n int64) { b.mu.Lock(); b.used -= n; b.mu.Unlock() }

func isControl(method string) bool {
	switch method {
	case "Stop", "StopTest", "StopDiagnostics", "CancelRequest", "CancelVPNChallenge", "QueryCapabilities":
		return true
	}
	return false
}
func isCancelable(method string) bool {
	switch method {
	case "Test", "IPTest", "SpeedTest", "CaptureDiagnostics", "WarpRegister", "UpdateRuleSets", "QueryVPNStatus":
		return true
	}
	return false
}

type requestKey struct{}
type requestState struct {
	owner         *connectionState
	cancel        context.CancelFunc
	cancelable    bool
	output        *byteBudget
	responseBytes int64
}
type connectionState struct {
	mu       sync.Mutex
	requests map[uint32]*requestState
}

type responseFrame struct {
	id      uint32
	status  byte
	data    []byte
	written chan struct{}
}

func writeFull(w io.Writer, data []byte) error {
	for len(data) > 0 {
		n, err := w.Write(data)
		if n < 0 || n > len(data) {
			return io.ErrShortWrite
		}
		data = data[n:]
		if err != nil {
			return err
		}
		if n == 0 {
			return io.ErrShortWrite
		}
	}
	return nil
}

// Existing little-endian framing is unchanged. No waiting work queue: ordinary
// saturation returns status=1 / "rpc busy". Control has independent slots,
// byte budgets and a priority output queue. A peer that doesn't consume output
// is disconnected rather than holding resources indefinitely.
func Serve(conn net.Conn, debug bool) { debugLog = debug; serve(conn, handlers) }
func serve(conn net.Conn, table map[string]handlerFn) {
	root, disconnect := context.WithCancel(context.Background())
	defer disconnect()
	defer conn.Close()
	state := &connectionState{requests: make(map[uint32]*requestState)}
	ordinaryResponses := make(chan responseFrame, ordinarySlots)
	controlResponses := make(chan responseFrame, controlSlots)
	fail := func() { disconnect(); _ = conn.Close() }
	go func() {
		defer fail()
		for {
			var response responseFrame
			select {
			case <-root.Done():
				return
			case response = <-controlResponses:
			default:
				select {
				case <-root.Done():
					return
				case response = <-controlResponses:
				case response = <-ordinaryResponses:
				}
			}
			var header [9]byte
			binary.LittleEndian.PutUint32(header[:4], response.id)
			header[4] = response.status
			binary.LittleEndian.PutUint32(header[5:], uint32(len(response.data)))
			if err := conn.SetWriteDeadline(time.Now().Add(frameTimeout)); err != nil {
				return
			}
			if err := writeFull(conn, header[:]); err != nil {
				return
			}
			if err := writeFull(conn, response.data); err != nil {
				return
			}
			if response.written != nil {
				close(response.written)
			}
		}
	}()
	enqueue := func(control bool, response responseFrame) bool {
		queue := ordinaryResponses
		if control {
			queue = controlResponses
		}
		select {
		case <-root.Done():
			return false
		case queue <- response:
			return true
		default:
			fail()
			return false // bounded rejection/output queue
		}
	}
	for {
		// Idle connections may stay open; once a frame starts it must finish.
		_ = conn.SetReadDeadline(time.Time{})
		var fixed [6]byte
		if _, err := io.ReadFull(conn, fixed[:1]); err != nil {
			return
		}
		_ = conn.SetReadDeadline(time.Now().Add(frameTimeout))
		if _, err := io.ReadFull(conn, fixed[1:]); err != nil {
			return
		}
		id := binary.LittleEndian.Uint32(fixed[:4])
		nameLen := binary.LittleEndian.Uint16(fixed[4:])
		if nameLen == 0 || nameLen > maxMethodBytes {
			return
		}
		var name [maxMethodBytes]byte
		if _, err := io.ReadFull(conn, name[:nameLen]); err != nil {
			return
		}
		method := string(name[:nameLen])
		var length [4]byte
		if _, err := io.ReadFull(conn, length[:]); err != nil {
			return
		}
		size := int64(binary.LittleEndian.Uint32(length[:]))
		control := isControl(method)
		if size > maxRequestBytes || (control && size > maxControlBytes) {
			return
		}
		slots, input, output := ordinaryWork, &requestBudget, &outputBudget
		if control {
			slots, input, output = controlWork, &controlRequestBudget, &controlOutputBudget
		}
		state.mu.Lock()
		_, duplicate := state.requests[id]
		state.mu.Unlock()
		if duplicate {
			return
		} // ambiguous IDs cannot be safely answered
		admitted := false
		select {
		case slots <- struct{}{}:
			admitted = true
		default:
		}
		if admitted && !input.take(size) {
			<-slots
			admitted = false
		}
		if !admitted {
			if _, err := io.CopyN(io.Discard, conn, size); err != nil {
				return
			}
			if !enqueue(control, responseFrame{id: id, status: 1, data: []byte("rpc busy: request budget")}) {
				return
			}
			continue
		}
		payload := make([]byte, int(size))
		if _, err := io.ReadFull(conn, payload); err != nil {
			input.give(size)
			<-slots
			return
		}
		// Only the explicitly supported long calls inherit disconnect/cancel.
		// Start, Stop, DB/filesystem mutations keep atomic completion semantics.
		parent := context.WithoutCancel(root)
		if isCancelable(method) {
			parent = root
		}
		ctx, cancel := context.WithCancel(parent)
		// Pin the legacy StopTest generation in read/admission order, before
		// another control frame can run while this goroutine is still queued.
		if method == "Test" || method == "IPTest" || method == "SpeedTest" {
			testCtx, stopTestCtx := requestTestContext(ctx)
			requestCancel := cancel
			ctx = testCtx
			cancel = func() { stopTestCtx(); requestCancel() }
		}
		request := &requestState{owner: state, cancel: cancel, cancelable: isCancelable(method), output: output}
		ctx = context.WithValue(ctx, requestKey{}, request)
		state.mu.Lock()
		state.requests[id] = request
		state.mu.Unlock()
		go func(id uint32, method string, payload []byte, size int64, ctx context.Context,
			request *requestState, slots chan struct{}, input *byteBudget, control bool) {
			defer func() {
				request.cancel()
				state.mu.Lock()
				delete(state.requests, id)
				state.mu.Unlock()
				input.give(size)
				request.output.give(request.responseBytes)
				<-slots
			}()
			response := responseFrame{id: id, written: make(chan struct{})}
			func() {
				defer func() {
					if r := recover(); r != nil {
						log.Printf("panic in %s: %v\n%s", method, r, runtimeDebug.Stack())
						response.status = 1
						response.data = []byte(fmt.Sprintf("core panic in %s", method))
					}
				}()
				h, found := table[method]
				if !found {
					response.status = 1
					response.data = []byte("unknown method: " + method)
					return
				}
				data, err := h(ctx, payload)
				if err != nil {
					response.status = 1
					response.data = []byte(err.Error())
				} else {
					response.data = data
				}
				limit := maxResponseBytes
				if control {
					limit = maxControlBytes
				}
				if len(response.data) > limit {
					response.status = 1
					response.data = []byte("rpc response exceeds byte budget")
				}
			}()
			if enqueue(control, response) {
				select {
				case <-response.written:
				case <-root.Done():
				}
			}
		}(id, method, payload, size, ctx, request, slots, input, control)
	}
}

func (s *server) QueryCapabilities(context.Context, *gen.EmptyReq) (*gen.RPCCapabilities, error) {
	return &gen.RPCCapabilities{
		ProtocolVersion: To(uint32(1)), CancelRequest: To(true), ConnectionDelta: To(true),
		MaxMethodBytes: To(uint32(maxMethodBytes)), MaxRequestBytes: To(uint32(maxRequestBytes)),
		MaxResponseBytes: To(uint32(maxResponseBytes)), OrdinarySlots: To(uint32(ordinarySlots)),
		ControlSlots: To(uint32(controlSlots)), CancelableMethods: []string{
			"Test", "IPTest", "SpeedTest", "CaptureDiagnostics", "WarpRegister", "UpdateRuleSets", "QueryVPNStatus",
		},
	}, nil
}
func (s *server) CancelRequest(ctx context.Context, in *gen.CancelRequestReq) (*gen.CancelRequestResp, error) {
	request, ok := ctx.Value(requestKey{}).(*requestState)
	if !ok {
		return &gen.CancelRequestResp{Accepted: To(false)}, nil
	}
	owner := request.owner
	owner.mu.Lock()
	defer owner.mu.Unlock()
	target := owner.requests[in.GetReqId()]
	accepted := target != nil && target.cancelable
	if accepted {
		target.cancel()
	}
	return &gen.CancelRequestResp{Accepted: To(accepted)}, nil
}

type handlerFn func(context.Context, []byte) ([]byte, error)

// PReq pins Req to the pointer type the generated code implements proto.Message on.
func handle[Req any, PReq interface {
	*Req
	proto.Message
}, Resp proto.Message](fn func(context.Context, PReq) (Resp, error)) handlerFn {
	return func(ctx context.Context, payload []byte) ([]byte, error) {
		req := PReq(new(Req))
		if err := proto.Unmarshal(payload, req); err != nil {
			return nil, err
		}
		resp, err := fn(ctx, req)
		if err != nil {
			return nil, err
		}
		if proto.Size(resp) > maxResponseBytes {
			return nil, fmt.Errorf("rpc response exceeds %d bytes", maxResponseBytes)
		}
		if state, ok := ctx.Value(requestKey{}).(*requestState); ok {
			size := int64(proto.Size(resp))
			if !state.output.take(size) {
				return nil, fmt.Errorf("rpc busy: output byte budget")
			}
			state.responseBytes = size
		}
		return proto.Marshal(resp)
	}
}

var handlers = map[string]handlerFn{
	"QueryCapabilities":     handle(globalServer.QueryCapabilities),
	"CancelRequest":         handle(globalServer.CancelRequest),
	"Start":                 handle(globalServer.Start),
	"Stop":                  handle(globalServer.Stop),
	"CheckConfig":           handle(globalServer.CheckConfig),
	"Test":                  handle(globalServer.Test),
	"StopTest":              handle(globalServer.StopTest),
	"QueryURLTest":          handle(globalServer.QueryURLTest),
	"IPTest":                handle(globalServer.IPTest),
	"QueryIPTest":           handle(globalServer.QueryIPTest),
	"QueryStats":            handle(globalServer.QueryStats),
	"QueryConnections":      handle(globalServer.QueryConnections),
	"QueryConnectionsDelta": handle(globalServer.QueryConnectionsDelta),
	"CloseConnections":      handle(globalServer.CloseConnections),
	"QueryAutoSelectors":    handle(globalServer.QueryAutoSelectors),
	"AutoSelectorAction":    handle(globalServer.AutoSelectorAction),
	"SelectOutbound":        handle(globalServer.SelectOutbound),
	"IsPrivileged":          handle(globalServer.IsPrivileged),
	"SetSystemDNS":          handle(globalServer.SetSystemDNS),
	"GetDefaultInterface":   handle(globalServer.GetDefaultInterface),
	"SpeedTest":             handle(globalServer.SpeedTest),
	"QuerySpeedTest":        handle(globalServer.QuerySpeedTest),
	"QueryCountryTest":      handle(globalServer.QueryCountryTest),
	"GenWgKeyPair":          handle(globalServer.GenWgKeyPair),
	"WarpRegister":          handle(globalServer.WarpRegister),
	"InstallDashboard":      handle(globalServer.InstallDashboard),
	"QueryVPNStatus":        handle(globalServer.QueryVPNStatus),
	"SubmitVPNChallenge":    handle(globalServer.SubmitVPNChallenge),
	"CancelVPNChallenge":    handle(globalServer.CancelVPNChallenge),
	"CaptureDiagnostics":    handle(globalServer.CaptureDiagnostics),
	"StopDiagnostics":       handle(globalServer.StopDiagnostics),
	"UpdateRuleSets":        handle(globalServer.UpdateRuleSets),
}

func dispatch(ctx context.Context, methodName string, payload []byte) ([]byte, error) {
	h, found := handlers[methodName]
	if !found {
		return nil, fmt.Errorf("unknown method: %s", methodName)
	}
	return h(ctx, payload)
}

// The wire path goes through the table above, not gRPC; keep the interface honest.
var _ gen.LibcoreServiceServer = globalServer
