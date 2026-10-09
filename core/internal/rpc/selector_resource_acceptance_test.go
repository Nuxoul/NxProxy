package rpc

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/include"
	"github.com/sagernet/sing-box/option"
	"github.com/sagernet/sing-box/protocol/group"
	"github.com/sagernet/sing-tun"
	"github.com/sagernet/sing/common/bufio"
	SJ "github.com/sagernet/sing/common/json"
	M "github.com/sagernet/sing/common/metadata"
	N "github.com/sagernet/sing/common/network"
)

// Same node runtime JSON in both modes; only tags/references/resource count differ.
// Idle, non-TUN boxes: no VLESS destination is ever dialed by this measurement.
func selectorResourceOptions(count int, shared bool) []byte {
	outbounds := []map[string]any{{"type": "direct", "tag": "direct"}}
	for g := 0; g < 8; g++ {
		members := make([]string, count)
		for n := 0; n < count; n++ {
			tag := fmt.Sprintf("node-%d-%d", g, n)
			if shared {
				tag = fmt.Sprintf("node-%d", n)
			}
			members[n] = tag
			if !shared || g == 0 {
				outbounds = append(outbounds, map[string]any{"type": "vless", "tag": tag, "server": "127.0.0.1", "server_port": 10000 + n, "uuid": "11111111-2222-3333-4444-555555555555", "packet_encoding": "xudp"})
			}
		}
		outbounds = append(outbounds, map[string]any{"type": "selector", "tag": fmt.Sprintf("group-%d", g), "outbounds": members, "default": members[g%count], "interrupt_exist_connections": true})
	}
	data, err := json.Marshal(map[string]any{"log": map[string]any{"disabled": true}, "outbounds": outbounds, "route": map[string]any{"final": "direct"}})
	if err != nil {
		panic(err)
	}
	return data
}

type selectorResourceMeasurement struct {
	Nodes             int    `json:"nodes"`
	Shared            bool   `json:"shared"`
	Repeat            int    `json:"repeat"`
	NewUS             int64  `json:"new_us"`
	StartUS           int64  `json:"start_us"`
	CloseUS           int64  `json:"close_us"`
	Outbounds         int    `json:"outbounds"`
	VLESS             int    `json:"vless"`
	Selectors         int    `json:"selectors"`
	HeapDelta         int64  `json:"heap_delta_bytes"`
	LiveHeapDelta     int64  `json:"post_gc_live_heap_delta_bytes"`
	TotalAllocDelta   uint64 `json:"total_alloc_delta_bytes"`
	MallocsDelta      uint64 `json:"mallocs_delta"`
	HeapObjectsDelta  int64  `json:"heap_objects_delta"`
	LiveObjectsDelta  int64  `json:"post_gc_live_objects_delta"`
	GoroutinesBefore  int    `json:"goroutines_before"`
	GoroutinesStarted int    `json:"goroutines_started"`
	GoroutinesClosed  int    `json:"goroutines_closed"`
}

// Each measurement runs in a fresh test child, avoiding prior box/cache retention.
func TestSelectorResourceLifecycleChild(t *testing.T) {
	index := -1
	for i, arg := range os.Args {
		if arg == "selector-resource-child" {
			index = i
			break
		}
	}
	if index < 0 {
		t.Skip("subprocess measurement helper")
	}
	if len(os.Args) <= index+3 {
		t.Fatal("missing measurement arguments")
	}
	count, err := strconv.Atoi(os.Args[index+1])
	if err != nil {
		t.Fatal(err)
	}
	shared := os.Args[index+2] == "shared"
	repeat, err := strconv.Atoi(os.Args[index+3])
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(include.Context(context.Background()))
	defer cancel()
	options, err := SJ.UnmarshalExtendedContext[option.Options](ctx, selectorResourceOptions(count, shared))
	if err != nil {
		t.Fatal(err)
	}
	runtime.GC()
	var before, started, live runtime.MemStats
	runtime.ReadMemStats(&before)
	m := selectorResourceMeasurement{Nodes: count, Shared: shared, Repeat: repeat, GoroutinesBefore: runtime.NumGoroutine()}
	stamp := time.Now()
	instance, err := box.New(box.Options{Options: options, Context: ctx})
	m.NewUS = time.Since(stamp).Microseconds()
	if err != nil {
		t.Fatal(err)
	}
	stamp = time.Now()
	err = instance.Start()
	m.StartUS = time.Since(stamp).Microseconds()
	if err != nil {
		_ = instance.Close()
		t.Fatal(err)
	}
	// Sampling after a fixed idle interval makes background startup comparable.
	time.Sleep(20 * time.Millisecond)
	runtime.ReadMemStats(&started)
	m.GoroutinesStarted = runtime.NumGoroutine()
	m.HeapDelta = int64(started.HeapAlloc) - int64(before.HeapAlloc)
	m.TotalAllocDelta = started.TotalAlloc - before.TotalAlloc
	m.MallocsDelta = started.Mallocs - before.Mallocs
	m.HeapObjectsDelta = int64(started.HeapObjects) - int64(before.HeapObjects)
	runtime.GC()
	runtime.ReadMemStats(&live)
	m.LiveHeapDelta = int64(live.HeapAlloc) - int64(before.HeapAlloc)
	m.LiveObjectsDelta = int64(live.HeapObjects) - int64(before.HeapObjects)
	actual := instance.Outbound().Outbounds()
	m.Outbounds = len(actual)
	for _, o := range actual {
		switch o.Type() {
		case "vless":
			m.VLESS++
		case "selector":
			m.Selectors++
		}
	}
	expected := 8 * count
	if shared {
		expected = count
	}
	if m.VLESS != expected || m.Selectors != 8 || m.Outbounds != expected+9 {
		t.Fatalf("unexpected actual resources: %+v", m)
	}
	for g := 0; g < 8; g++ {
		out, ok := instance.Outbound().Outbound(fmt.Sprintf("group-%d", g))
		if !ok {
			t.Fatal("missing independent selector object")
		}
		selector := out.(*group.Selector)
		selected := fmt.Sprintf("node-%d-%d", g, g%count)
		if shared {
			selected = fmt.Sprintf("node-%d", g%count)
		}
		if selector.Now() != selected || len(selector.All()) != count {
			t.Fatal("runtime independent selector default/member list changed")
		}
	}
	// Keep parsed options and the box reachable through the live-heap sample.
	runtime.KeepAlive(options)
	runtime.KeepAlive(instance)
	stamp = time.Now()
	err = instance.Close()
	m.CloseUS = time.Since(stamp).Microseconds()
	cancel()
	if err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(2 * time.Second)
	for runtime.NumGoroutine() > m.GoroutinesBefore && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	m.GoroutinesClosed = runtime.NumGoroutine()
	data, err := json.Marshal(m)
	if err != nil {
		t.Fatal(err)
	}
	fmt.Printf("SELECTOR_RESOURCE_METRIC %s\n", data)
}

func TestSelectorResourceLifecycleMeasurements(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	for _, count := range []int{15, 30} {
		for repeat := 1; repeat <= 3; repeat++ {
			// Reverse A/B order on even repetitions to reduce ordering bias.
			modes := []string{"independent", "shared"}
			if repeat%2 == 0 {
				modes = []string{"shared", "independent"}
			}
			for _, mode := range modes {
				ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
				output, err := exec.CommandContext(ctx, executable, "-test.run=^TestSelectorResourceLifecycleChild$", "-test.v", "--", "selector-resource-child", strconv.Itoa(count), mode, strconv.Itoa(repeat)).CombinedOutput()
				cancel()
				if err != nil {
					t.Fatalf("measurement child: %v\n%s", err, output)
				}
				found := false
				for _, line := range strings.Split(string(output), "\n") {
					if strings.HasPrefix(line, "SELECTOR_RESOURCE_METRIC ") {
						t.Log(line)
						found = true
					}
				}
				if !found {
					t.Fatalf("child omitted metrics: %s", output)
				}
			}
		}
	}
}

// Real router observer, not a replacement handler. Records which actual selector
// the router dispatches to (Selector implements both Handler interfaces).
type selectorResourceRouteTracker struct {
	mutex    sync.Mutex
	tcp, udp []string
}

func (o *selectorResourceRouteTracker) RoutedConnection(_ context.Context, c net.Conn, m adapter.InboundContext, _ adapter.Rule, out adapter.Outbound) net.Conn {
	o.mutex.Lock()
	defer o.mutex.Unlock()
	if m.Inbound == "fixtureA" || m.Inbound == "fixtureB" {
		o.tcp = append(o.tcp, out.Tag())
	}
	return c
}
func (o *selectorResourceRouteTracker) RoutedPacketConnection(_ context.Context, c N.PacketConn, m adapter.InboundContext, _ adapter.Rule, out adapter.Outbound) N.PacketConn {
	o.mutex.Lock()
	defer o.mutex.Unlock()
	if m.Inbound == "fixtureA" || m.Inbound == "fixtureB" {
		o.udp = append(o.udp, out.Tag())
	}
	return c
}
func (*selectorResourceRouteTracker) RoutedFlow(context.Context, adapter.InboundContext, adapter.Rule, adapter.Outbound) tun.FlowTracker {
	return nil
}

// Router -> Selector.NewConnection/NewPacketConnection -> real ConnectionManager
// -> Selector's interrupt-wrapped Dial/Listen -> shared real VLESS -> loopback echo.
func TestSelectorSharingRoutedHandlers(t *testing.T) {
	tcp, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer tcp.Close()
	go func() {
		for {
			c, e := tcp.Accept()
			if e != nil {
				return
			}
			go func() { defer c.Close(); _, _ = io.Copy(c, c) }()
		}
	}()
	udp, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer udp.Close()
	go func() {
		data := make([]byte, 4096)
		for {
			n, a, e := udp.ReadFrom(data)
			if e != nil {
				return
			}
			_, _ = udp.WriteTo(data[:n], a)
		}
	}()
	probe, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := probe.Addr().(*net.TCPAddr).Port
	_ = probe.Close()
	ctx, cancel := context.WithCancel(include.Context(context.Background()))
	defer cancel()
	raw := fmt.Sprintf(`{"log":{"disabled":true},"inbounds":[{"type":"vless","tag":"server","listen":"127.0.0.1","listen_port":%d,"users":[{"uuid":"11111111-2222-3333-4444-555555555555"}]}],"outbounds":[{"type":"direct","tag":"direct"},{"type":"vless","tag":"shared","server":"127.0.0.1","server_port":%d,"uuid":"11111111-2222-3333-4444-555555555555","packet_encoding":"xudp"},{"type":"selector","tag":"groupA","outbounds":["shared","direct"],"default":"shared","interrupt_exist_connections":true},{"type":"selector","tag":"groupB","outbounds":["shared","direct"],"default":"shared","interrupt_exist_connections":true}],"route":{"rules":[{"inbound":["fixtureA"],"outbound":"groupA"},{"inbound":["fixtureB"],"outbound":"groupB"}],"final":"direct"}}`, port, port)
	options, err := SJ.UnmarshalExtendedContext[option.Options](ctx, []byte(raw))
	if err != nil {
		t.Fatal(err)
	}
	instance, err := box.New(box.Options{Options: options, Context: ctx})
	if err != nil {
		t.Fatal(err)
	}
	defer instance.Close()
	if err = instance.Start(); err != nil {
		t.Fatal(err)
	}
	get := func(tag string) *group.Selector {
		o, ok := instance.Outbound().Outbound(tag)
		if !ok {
			t.Fatal("missing selector")
		}
		return o.(*group.Selector)
	}
	a, b := get("groupA"), get("groupB")
	shared, _ := instance.Outbound().Outbound("shared")
	if _, bypass := shared.(adapter.ConnectionHandler); bypass {
		t.Fatal("unexpected member TCP handler bypass")
	}
	if _, bypass := shared.(adapter.PacketConnectionHandler); bypass {
		t.Fatal("unexpected member UDP handler bypass")
	}
	tracker := new(selectorResourceRouteTracker)
	instance.Router().AppendTracker(tracker)
	openTCP := func(inbound string) (net.Conn, <-chan error) {
		client, server := net.Pipe()
		done := make(chan error, 1)
		t.Cleanup(func() { _ = client.Close(); _ = server.Close() })
		instance.Router().RouteConnectionEx(ctx, server, adapter.InboundContext{Inbound: inbound, InboundType: "direct", Network: "tcp", Source: M.ParseSocksaddr("127.0.0.1:32100"), Destination: M.ParseSocksaddr(tcp.Addr().String())}, N.OnceClose(func(e error) { done <- e }))
		return client, done
	}
	openUDP := func(inbound string) (net.PacketConn, net.Addr, <-chan error) {
		client, e := net.ListenPacket("udp", "127.0.0.1:0")
		if e != nil {
			t.Fatal(e)
		}
		server, e := net.DialUDP("udp", nil, client.LocalAddr().(*net.UDPAddr))
		if e != nil {
			_ = client.Close()
			t.Fatal(e)
		}
		t.Cleanup(func() { _ = client.Close(); _ = server.Close() })
		done := make(chan error, 1)
		// Connected ingress UDP: destination metadata is the echo target, not client socket.
		conn := bufio.NewUnbindPacketConnWithAddr(server, M.ParseSocksaddr(udp.LocalAddr().String()))
		instance.Router().RoutePacketConnectionEx(ctx, conn, adapter.InboundContext{Inbound: inbound, InboundType: "direct", Network: "udp", Source: M.SocksaddrFromNet(client.LocalAddr()), Destination: M.ParseSocksaddr(udp.LocalAddr().String())}, N.OnceClose(func(e error) { done <- e }))
		return client, server.LocalAddr(), done
	}
	ac, at := openTCP("fixtureA")
	bc, bt := openTCP("fixtureB")
	ap, aa, au := openUDP("fixtureA")
	bp, ba, bu := openUDP("fixtureB")
	tcpEcho := func(c net.Conn) {
		t.Helper()
		_ = c.SetDeadline(time.Now().Add(3 * time.Second))
		if _, e := c.Write([]byte("ping")); e != nil {
			t.Fatal(e)
		}
		var data [4]byte
		if _, e := io.ReadFull(c, data[:]); e != nil {
			t.Fatal(e)
		}
		if string(data[:]) != "ping" {
			t.Fatal("TCP payload")
		}
	}
	udpEcho := func(c net.PacketConn, to net.Addr) {
		t.Helper()
		_ = c.SetDeadline(time.Now().Add(3 * time.Second))
		if _, e := c.WriteTo([]byte("ping"), to); e != nil {
			t.Fatal(e)
		}
		var data [16]byte
		n, _, e := c.ReadFrom(data[:])
		if e != nil {
			t.Fatal(e)
		}
		if string(data[:n]) != "ping" {
			t.Fatal("UDP payload")
		}
	}
	waitClosed := func(done <-chan error) {
		t.Helper()
		select {
		case <-done:
		case <-time.After(3 * time.Second):
			t.Fatal("routed connection not closed after switch")
		}
	}
	assertOpen := func(done <-chan error) {
		t.Helper()
		select {
		case e := <-done:
			t.Fatalf("B closed unexpectedly: %v", e)
		default:
		}
	}
	tcpEcho(ac)
	tcpEcho(bc)
	udpEcho(ap, aa)
	udpEcho(bp, ba)
	tracker.mutex.Lock()
	dispatched := len(tracker.tcp) == 2 && tracker.tcp[0] == "groupA" && tracker.tcp[1] == "groupB" && len(tracker.udp) == 2 && tracker.udp[0] == "groupA" && tracker.udp[1] == "groupB"
	tracker.mutex.Unlock()
	if !dispatched {
		t.Fatal("router did not select actual Handler objects for both groups")
	}
	if !a.SelectOutbound("direct") || b.Now() != "shared" {
		t.Fatal("A switch changed B")
	}
	waitClosed(at)
	waitClosed(au)
	_ = ac.SetReadDeadline(time.Now().Add(time.Second))
	var data [1]byte
	if _, e := ac.Read(data[:]); e == nil {
		t.Fatal("A TCP ingress still open")
	} else if timeout, ok := e.(net.Error); ok && timeout.Timeout() {
		t.Fatal("A TCP timed out instead of closing")
	}
	assertOpen(bt)
	assertOpen(bu)
	tcpEcho(bc)
	udpEcho(bp, ba)
	if !a.SelectOutbound("shared") || !b.SelectOutbound("shared") {
		t.Fatal("reselect")
	}
	assertOpen(bt)
	assertOpen(bu)
	tcpEcho(bc)
	udpEcho(bp, ba)
	if !b.SelectOutbound("direct") {
		t.Fatal("switch B")
	}
	waitClosed(bt)
	waitClosed(bu)
}
