package rpc

import (
	"context"
	"fmt"
	"io"
	"net"
	"testing"
	"time"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/common/interrupt"
	"github.com/sagernet/sing-box/include"
	"github.com/sagernet/sing-box/option"
	"github.com/sagernet/sing-box/protocol/group"
	"github.com/sagernet/sing/common/json"
	M "github.com/sagernet/sing/common/metadata"
)

// Real fork Selector + one real shared VLESS outbound, TCP and UDP through a
// loopback VLESS inbound. No mock selector, dependency changes, TUN or live RPC.
func TestSelectorSharingInterruption(t *testing.T) {
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
		b := make([]byte, 4096)
		for {
			n, a, e := udp.ReadFrom(b)
			if e != nil {
				return
			}
			_, _ = udp.WriteTo(b[:n], a)
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
	raw := fmt.Sprintf(`{"log":{"disabled":true},"inbounds":[{"type":"vless","tag":"server","listen":"127.0.0.1","listen_port":%d,"users":[{"uuid":"11111111-2222-3333-4444-555555555555"}]}],"outbounds":[{"type":"direct","tag":"direct"},{"type":"vless","tag":"shared","server":"127.0.0.1","server_port":%d,"uuid":"11111111-2222-3333-4444-555555555555"},{"type":"selector","tag":"groupA","outbounds":["shared","direct"],"default":"shared","interrupt_exist_connections":true},{"type":"selector","tag":"groupB","outbounds":["shared","direct"],"default":"shared","interrupt_exist_connections":true}],"route":{"final":"direct"}}`, port, port)
	options, err := json.UnmarshalExtendedContext[option.Options](ctx, []byte(raw))
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
	get := func(tag string) adapter.Outbound {
		o, ok := instance.Outbound().Outbound(tag)
		if !ok {
			t.Fatalf("missing %s", tag)
		}
		return o
	}
	a := get("groupA").(*group.Selector)
	b := get("groupB").(*group.Selector)
	shared := get("shared")
	if _, bypass := shared.(adapter.ConnectionHandler); bypass {
		t.Fatal("VLESS bypasses selector TCP wrapper")
	}
	if _, bypass := shared.(adapter.PacketConnectionHandler); bypass {
		t.Fatal("VLESS bypasses selector UDP wrapper")
	}
	if a.Now() != "shared" || b.Now() != "shared" {
		t.Fatal("independent initial picks")
	}
	external := interrupt.ContextWithIsExternalConnection(ctx)
	ac, err := a.DialContext(external, "tcp", M.ParseSocksaddr(tcp.Addr().String()))
	if err != nil {
		t.Fatal(err)
	}
	defer ac.Close()
	bc, err := b.DialContext(external, "tcp", M.ParseSocksaddr(tcp.Addr().String()))
	if err != nil {
		t.Fatal(err)
	}
	defer bc.Close()
	ap, err := a.ListenPacket(external, M.ParseSocksaddr(udp.LocalAddr().String()))
	if err != nil {
		t.Fatal(err)
	}
	defer ap.Close()
	bp, err := b.ListenPacket(external, M.ParseSocksaddr(udp.LocalAddr().String()))
	if err != nil {
		t.Fatal(err)
	}
	defer bp.Close()
	tcpEcho := func(c net.Conn) {
		t.Helper()
		_ = c.SetDeadline(time.Now().Add(3 * time.Second))
		if _, e := c.Write([]byte("ping")); e != nil {
			t.Fatal(e)
		}
		var buf [4]byte
		if _, e := io.ReadFull(c, buf[:]); e != nil {
			t.Fatal(e)
		}
		if string(buf[:]) != "ping" {
			t.Fatal("TCP data")
		}
	}
	udpEcho := func(c net.PacketConn) {
		t.Helper()
		_ = c.SetDeadline(time.Now().Add(3 * time.Second))
		if _, e := c.WriteTo([]byte("ping"), udp.LocalAddr()); e != nil {
			t.Fatal(e)
		}
		var buf [16]byte
		n, _, e := c.ReadFrom(buf[:])
		if e != nil {
			t.Fatal(e)
		}
		if string(buf[:n]) != "ping" {
			t.Fatal("UDP data")
		}
	}
	tcpEcho(ac)
	tcpEcho(bc)
	udpEcho(ap)
	udpEcho(bp)
	if !a.SelectOutbound("direct") || a.Now() != "direct" || b.Now() != "shared" {
		t.Fatal("switch A changed B")
	}
	_ = ac.SetDeadline(time.Now().Add(time.Second))
	var data [1]byte
	if _, e := ac.Read(data[:]); e == nil {
		t.Fatal("A TCP remained open")
	} else if timeout, ok := e.(net.Error); ok && timeout.Timeout() {
		t.Fatal("A TCP not interrupted, only timed out")
	}
	_ = ap.SetDeadline(time.Now().Add(time.Second))
	if _, _, e := ap.ReadFrom(data[:]); e == nil {
		t.Fatal("A UDP remained open")
	} else if timeout, ok := e.(net.Error); ok && timeout.Timeout() {
		t.Fatal("A UDP not interrupted, only timed out")
	}
	tcpEcho(bc)
	udpEcho(bp)
	if !a.SelectOutbound("shared") {
		t.Fatal("switch A back")
	}
	tcpEcho(bc)
	udpEcho(bp)
	if !b.SelectOutbound("shared") {
		t.Fatal("B no-op pick")
	}
	tcpEcho(bc)
	udpEcho(bp)
}
