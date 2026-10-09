package rpc

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"runtime"
	"testing"
	"time"

	"ThroneCore/gen"
	"ThroneCore/internal/probe"
	"google.golang.org/protobuf/proto"
)

func requestFrame(id uint32, method string, payload []byte) []byte {
	var b bytes.Buffer
	_ = binary.Write(&b, binary.LittleEndian, id)
	_ = binary.Write(&b, binary.LittleEndian, uint16(len(method)))
	b.WriteString(method)
	_ = binary.Write(&b, binary.LittleEndian, uint32(len(payload)))
	b.Write(payload)
	return b.Bytes()
}

type testResponse struct {
	id     uint32
	status byte
	data   []byte
}

func readResponse(t testing.TB, c net.Conn) testResponse {
	t.Helper()
	_ = c.SetReadDeadline(time.Now().Add(5 * time.Second))
	var h [9]byte
	if _, err := io.ReadFull(c, h[:]); err != nil {
		t.Fatal(err)
	}
	n := binary.LittleEndian.Uint32(h[5:])
	if n > maxResponseBytes {
		t.Fatal("oversized response", n)
	}
	r := testResponse{id: binary.LittleEndian.Uint32(h[:4]), status: h[4], data: make([]byte, n)}
	if _, err := io.ReadFull(c, r.data); err != nil {
		t.Fatal(err)
	}
	return r
}
func sendRequest(t testing.TB, c net.Conn, id uint32, method string, payload []byte) {
	t.Helper()
	_ = c.SetWriteDeadline(time.Now().Add(5 * time.Second))
	if err := writeFull(c, requestFrame(id, method, payload)); err != nil {
		t.Fatal(err)
	}
}
func startPipe(t testing.TB, table map[string]handlerFn) (net.Conn, <-chan struct{}) {
	t.Helper()
	client, server := net.Pipe()
	done := make(chan struct{})
	go func() { serve(server, table); close(done) }()
	t.Cleanup(func() {
		_ = client.Close()
		select {
		case <-done:
		case <-time.After(5 * time.Second):
			t.Error("Serve leaked")
		}
	})
	return client, done
}
func waitIdle(t testing.TB) {
	t.Helper()
	end := time.Now().Add(5 * time.Second)
	for len(ordinaryWork) != 0 || len(controlWork) != 0 {
		if time.Now().After(end) {
			t.Fatal("work slots did not recover")
		}
		time.Sleep(time.Millisecond)
	}
	requestBudget.mu.Lock()
	used := requestBudget.used
	requestBudget.mu.Unlock()
	outputBudget.mu.Lock()
	output := outputBudget.used
	outputBudget.mu.Unlock()
	if used != 0 || output != 0 {
		t.Fatalf("byte budgets leaked: input=%d output=%d", used, output)
	}
}
func TestRPCFrameBounds(t *testing.T) {
	for _, tc := range []struct {
		name  string
		frame []byte
	}{
		{"halfHeader", []byte{1, 2}},
		{"halfPayload", requestFrame(1, "Test", []byte("payload"))[:16]},
		{"methodTooLong", requestFrame(1, string(bytes.Repeat([]byte{'x'}, 129)), nil)},
		{"zeroMethod", requestFrame(1, "", nil)},
		{"oversizePayload", func() []byte {
			p := requestFrame(1, "Test", nil)
			binary.LittleEndian.PutUint32(p[len(p)-4:], maxRequestBytes+1)
			return p
		}()},
		{"controlPayload", func() []byte {
			p := requestFrame(1, "StopTest", nil)
			binary.LittleEndian.PutUint32(p[len(p)-4:], maxControlBytes+1)
			return p
		}()},
	} {
		t.Run(tc.name, func(t *testing.T) {
			c, done := startPipe(t, handlers)
			go func() { _, _ = c.Write(tc.frame); _ = c.Close() }()
			select {
			case <-done:
			case <-time.After(2 * time.Second):
				t.Fatal("malformed frame did not terminate")
			}
			waitIdle(t)
		})
	}
}
func TestRPCBusyControlCancel(t *testing.T) {
	started := make(chan struct{}, ordinarySlots)
	table := map[string]handlerFn{
		"Test": func(ctx context.Context, _ []byte) ([]byte, error) {
			started <- struct{}{}
			<-ctx.Done()
			return nil, ctx.Err()
		},
		"CancelRequest":     handlers["CancelRequest"],
		"StopTest":          func(context.Context, []byte) ([]byte, error) { return []byte("control"), nil },
		"QueryCapabilities": handlers["QueryCapabilities"],
	}
	c, _ := startPipe(t, table)
	for i := uint32(1); i <= ordinarySlots; i++ {
		sendRequest(t, c, i, "Test", nil)
		<-started
	}
	sendRequest(t, c, 50, "Test", nil)
	r := readResponse(t, c)
	if r.id != 50 || r.status != 1 || !bytes.HasPrefix(r.data, []byte("rpc busy")) {
		t.Fatalf("busy=%+v", r)
	}
	sendRequest(t, c, 51, "StopTest", nil)
	if r = readResponse(t, c); r.id != 51 || string(r.data) != "control" {
		t.Fatalf("control=%+v", r)
	}
	p, _ := proto.Marshal(&gen.CancelRequestReq{ReqId: To(uint32(1))})
	sendRequest(t, c, 52, "CancelRequest", p)
	accepted := false
	canceled := false
	for i := 0; i < 2; i++ {
		r = readResponse(t, c)
		if r.id == 52 {
			var out gen.CancelRequestResp
			if err := proto.Unmarshal(r.data, &out); err != nil {
				t.Fatal(err)
			}
			accepted = out.GetAccepted()
		} else if r.id == 1 {
			canceled = r.status == 1
		}
	}
	if !accepted || !canceled {
		t.Fatal("cancel not acknowledged", accepted, canceled)
	}
	_ = c.Close()
	waitIdle(t)
}
func TestRPCDisconnectAtomicAndReconnect(t *testing.T) {
	started := make(chan context.Context, 1)
	release := make(chan struct{})
	table := map[string]handlerFn{"Start": func(ctx context.Context, _ []byte) ([]byte, error) { started <- ctx; <-release; return nil, nil }}
	c, done := startPipe(t, table)
	sendRequest(t, c, 1, "Start", nil)
	ctx := <-started
	_ = c.Close()
	<-done
	if ctx.Err() != nil {
		t.Fatal("Start forcibly canceled")
	}
	close(release)
	waitIdle(t)
	fresh, _ := startPipe(t, handlers)
	sendRequest(t, fresh, 1, "QueryCapabilities", nil)
	r := readResponse(t, fresh)
	var out gen.RPCCapabilities
	if err := proto.Unmarshal(r.data, &out); err != nil {
		t.Fatal(err)
	}
	if !out.GetCancelRequest() || out.GetMaxRequestBytes() != maxRequestBytes {
		t.Fatal("handshake", &out)
	}
	_ = fresh.Close()
	waitIdle(t)
}
func TestRPCCancelUnknownAndAtomic(t *testing.T) {
	ctx := context.WithValue(context.Background(), requestKey{}, &requestState{owner: &connectionState{requests: map[uint32]*requestState{1: {cancelable: false}}}})
	for _, id := range []uint32{1, 99} {
		out, err := globalServer.CancelRequest(ctx, &gen.CancelRequestReq{ReqId: To(id)})
		if err != nil || out.GetAccepted() {
			t.Fatal("unsafe cancellation", out, err)
		}
	}
}
func TestRPCConcurrentResponsesAndFragmentedRequest(t *testing.T) {
	c, _ := startPipe(t, map[string]handlerFn{"Echo": func(_ context.Context, p []byte) ([]byte, error) { return p, nil }})
	go func() {
		for id := uint32(1); id <= ordinarySlots; id++ {
			for _, b := range requestFrame(id, "Echo", []byte{byte(id)}) {
				_, _ = c.Write([]byte{b})
			}
		}
	}()
	seen := map[uint32]bool{}
	for i := 0; i < ordinarySlots; i++ {
		r := readResponse(t, c)
		if r.status != 0 || !bytes.Equal(r.data, []byte{byte(r.id)}) {
			t.Fatalf("corrupt frame %+v", r)
		}
		seen[r.id] = true
	}
	if len(seen) != ordinarySlots {
		t.Fatal("missing response")
	}
	_ = c.Close()
	waitIdle(t)
}

type shortWriter struct {
	bytes.Buffer
	zero, fail bool
}

func (w *shortWriter) Write(p []byte) (int, error) {
	if w.zero {
		return 0, nil
	}
	if w.fail {
		return 0, errors.New("write failure")
	}
	if len(p) > 3 {
		p = p[:3]
	}
	return w.Buffer.Write(p)
}
func TestRPCFullWrites(t *testing.T) {
	w := &shortWriter{}
	p := bytes.Repeat([]byte{'x'}, 99)
	if err := writeFull(w, p); err != nil || !bytes.Equal(w.Bytes(), p) {
		t.Fatal("short write corruption", err)
	}
	if !errors.Is(writeFull(&shortWriter{zero: true}, p), io.ErrShortWrite) {
		t.Fatal("zero write not rejected")
	}
	if writeFull(&shortWriter{fail: true}, p) == nil {
		t.Fatal("write error ignored")
	}
}
func TestRPCResponseSizeBeforeMarshal(t *testing.T) {
	h := handle(func(context.Context, *gen.EmptyReq) (*gen.DiagnosticsResponse, error) {
		return &gen.DiagnosticsResponse{Archive: make([]byte, maxResponseBytes+1)}, nil
	})
	if _, err := h(context.Background(), nil); err == nil {
		t.Fatal("oversize response accepted")
	}
}
func TestRPCRequestAndLegacyTestContext(t *testing.T) {
	parent, cancel := context.WithCancel(context.Background())
	ctx, closeCtx := requestTestContext(parent)
	defer closeCtx()
	cancel()
	select {
	case <-ctx.Done():
	case <-time.After(time.Second):
		t.Fatal("request cancel not propagated")
	}
	other, stop := requestTestContext(context.Background())
	defer stop()
	if other.Err() != nil {
		t.Fatal("request canceled unrelated tests")
	}
	probe.CancelTests()
	select {
	case <-other.Done():
	case <-time.After(time.Second):
		t.Fatal("StopTest lost")
	}
}
func TestRPCByteBudget(t *testing.T) {
	b := byteBudget{limit: 10}
	if !b.take(10) || b.take(1) {
		t.Fatal("budget admission")
	}
	b.give(10)
	if !b.take(10) {
		t.Fatal("budget not recovered")
	}
}
func TestRPCLimitedDiagnosticsWriter(t *testing.T) {
	var buf bytes.Buffer
	w := limitedWriter{writer: &buf, remaining: 4}
	if _, err := w.Write([]byte("four")); err != nil {
		t.Fatal(err)
	}
	if _, err := w.Write([]byte("x")); err == nil || buf.Len() != 4 {
		t.Fatal("diagnostics allocation unbounded")
	}
}

func BenchmarkRPCStress(b *testing.B) {
	before := runtime.NumGoroutine()
	var initial, peak, after runtime.MemStats
	runtime.GC()
	runtime.ReadMemStats(&initial)
	for n := 0; n < b.N; n++ {
		started := make(chan struct{}, ordinarySlots)
		c, done := startPipe(b, map[string]handlerFn{"Test": func(ctx context.Context, _ []byte) ([]byte, error) {
			started <- struct{}{}
			<-ctx.Done()
			return nil, ctx.Err()
		}, "StopTest": func(context.Context, []byte) ([]byte, error) { return nil, nil }})
		for id := uint32(1); id <= ordinarySlots; id++ {
			sendRequest(b, c, id, "Test", nil)
			<-started
		}
		for id := uint32(100); id < 1100; id++ {
			sendRequest(b, c, id, "Test", nil)
			r := readResponse(b, c)
			if r.status != 1 {
				b.Fatal("not busy")
			}
		}
		sendRequest(b, c, 2000, "StopTest", nil)
		if readResponse(b, c).id != 2000 {
			b.Fatal("control starvation")
		}
		runtime.ReadMemStats(&peak)
		if g := runtime.NumGoroutine(); g > before+ordinarySlots+6 {
			b.Fatal("goroutine growth", g, before)
		}
		_ = c.Close()
		<-done
		waitIdle(b)
	}
	runtime.GC()
	runtime.ReadMemStats(&after)
	b.ReportMetric(float64(peak.HeapAlloc)/1048576, "peak-heap-MiB")
	b.ReportMetric(float64(after.HeapAlloc)/1048576, "idle-heap-MiB")
	b.ReportMetric(float64(runtime.NumGoroutine()-before), "goroutine-delta")
	b.ReportMetric(float64(ordinarySlots), "peak-work-slots")
}

// Large input support is verified on the real framed reader, not by merely
// asserting a constant. A 64 MiB configuration fits before protobuf overhead.
func TestRPCLegalLargeConfiguration(t *testing.T) {
	c, _ := startPipe(t, map[string]handlerFn{"CheckConfig": func(_ context.Context, p []byte) ([]byte, error) {
		if len(p) != 64<<20 {
			return nil, errors.New("truncated configuration")
		}
		return nil, nil
	}})
	payload := make([]byte, 64<<20)
	sendRequest(t, c, 1, "CheckConfig", payload)
	if r := readResponse(t, c); r.status != 0 {
		t.Fatal(string(r.data))
	}
	_ = c.Close()
	waitIdle(t)
}
func TestRPCLegalDiagnosticsAttachments(t *testing.T) {
	// 32 MiB attachment plus an incompressible trace-sized archive and overhead.
	h := handle(func(context.Context, *gen.DiagnosticsRequest) (*gen.DiagnosticsResponse, error) {
		return &gen.DiagnosticsResponse{Archive: make([]byte, 65<<20)}, nil
	})
	input, _ := proto.Marshal(&gen.DiagnosticsRequest{Attachments: []*gen.DiagnosticsAttachment{{Name: To("sample"), Data: make([]byte, 32<<20)}}})
	if len(input) > maxRequestBytes {
		t.Fatal("legal attachments exceed request limit")
	}
	if out, err := h(context.Background(), input); err != nil || len(out) < 65<<20 {
		t.Fatal("legal diagnostics rejected", err)
	}
}

type deadlineConn struct{ net.Conn }

func (c deadlineConn) SetWriteDeadline(time.Time) error {
	return c.Conn.SetWriteDeadline(time.Now().Add(25 * time.Millisecond))
}
func TestRPCStalledOutputDeadline(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	done := make(chan struct{})
	go func() {
		serve(deadlineConn{server}, map[string]handlerFn{"Echo": func(context.Context, []byte) ([]byte, error) { return []byte("response"), nil }})
		close(done)
	}()
	sendRequest(t, client, 1, "Echo", nil)
	// Never consume output: writer must break the read loop and recover slots.
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("stalled writer held connection")
	}
	waitIdle(t)
}
func TestRPCControlBudgetAlsoBounded(t *testing.T) {
	release := make(chan struct{})
	started := make(chan struct{}, controlSlots)
	c, _ := startPipe(t, map[string]handlerFn{"Stop": func(context.Context, []byte) ([]byte, error) { started <- struct{}{}; <-release; return nil, nil }})
	for id := uint32(1); id <= controlSlots; id++ {
		sendRequest(t, c, id, "Stop", nil)
		<-started
	}
	sendRequest(t, c, 99, "Stop", nil)
	if r := readResponse(t, c); r.id != 99 || r.status != 1 {
		t.Fatal("unbounded control slots", r)
	}
	close(release)
	for i := 0; i < controlSlots; i++ {
		readResponse(t, c)
	}
	_ = c.Close()
	waitIdle(t)
}
