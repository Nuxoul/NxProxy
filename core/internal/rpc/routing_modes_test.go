package rpc

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/miekg/dns"
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
	"github.com/sagernet/sing/service"
)

type generatedRoutingTracker struct {
	tags chan string
	legs chan string
}

func (g *generatedRoutingTracker) observe(m adapter.InboundContext, out adapter.Outbound) {
	if m.Inbound == "generated-fixture" {
		g.tags <- out.Tag()
	}
	if g.legs != nil && (m.Inbound == "relay-server-A" || m.Inbound == "relay-server-B") {
		g.legs <- m.Inbound
	}
}

func (g *generatedRoutingTracker) RoutedConnection(_ context.Context, c net.Conn, m adapter.InboundContext, _ adapter.Rule, out adapter.Outbound) net.Conn {
	g.observe(m, out)
	return c
}
func (g *generatedRoutingTracker) RoutedPacketConnection(_ context.Context, c N.PacketConn, m adapter.InboundContext, _ adapter.Rule, out adapter.Outbound) N.PacketConn {
	g.observe(m, out)
	return c
}
func (*generatedRoutingTracker) RoutedFlow(context.Context, adapter.InboundContext, adapter.Rule, adapter.Outbound) tun.FlowTracker {
	return nil
}

// Exercises the real fork router and data path using exact production-generated
// route rules. Only listeners/outbound transport and unused auxiliary sections
// are replaced with a loopback fixture; no TUN/system proxy/DNS listener is opened.
func TestGeneratedLocalRouting(t *testing.T) { runGeneratedRouting(t, "local-generated.json", "local") }
func TestGeneratedDesktopModes(t *testing.T) {
	for _, mode := range []string{"rule", "global"} {
		t.Run(mode, func(t *testing.T) { runGeneratedRouting(t, mode+"-generated.json", mode) })
	}
}
func runGeneratedRouting(t *testing.T, file, scenario string) {
	dir := os.Getenv("NX_ROUTING_FIXTURE_DIR")
	if dir == "" {
		t.Skip("set NX_ROUTING_FIXTURE_DIR to production Qt regression output")
	}
	data, err := os.ReadFile(filepath.Join(dir, file))
	if err != nil {
		t.Fatal(err)
	}
	var generated map[string]any
	if err = json.Unmarshal(data, &generated); err != nil {
		t.Fatal(err)
	}
	route := generated["route"].(map[string]any)
	if scenario == "local" {
		route["final"] = "direct"
	}
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
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := listener.Addr().(*net.TCPAddr).Port
	_ = listener.Close()
	raw := map[string]any{
		"log":       map[string]any{"disabled": true},
		"inbounds":  []any{map[string]any{"type": "vless", "tag": "relay-server", "listen": "127.0.0.1", "listen_port": port, "users": []any{map[string]any{"uuid": "11111111-2222-3333-4444-555555555555"}}}},
		"outbounds": []any{map[string]any{"type": "direct", "tag": "direct"}, map[string]any{"type": "vless", "tag": "relay", "server": "127.0.0.1", "server_port": port, "uuid": "11111111-2222-3333-4444-555555555555", "packet_encoding": "xudp"}, map[string]any{"type": "selector", "tag": "proxy", "outbounds": []string{"relay", "direct"}, "default": "relay", "interrupt_exist_connections": true}},
		"route":     route,
		"dns":       map[string]any{"servers": []any{map[string]any{"type": "local", "tag": "dns-direct"}}},
	}
	var globalSelector *group.Selector
	var globalTag string
	if scenario != "local" {
		// Keep generated selector/member identity and rule order; replace only physical
		// transports/listeners. Relay ingress must bypass the tested global final.
		outbounds := generated["outbounds"].([]any)
		var selectorOptions map[string]any
		for _, item := range outbounds {
			o := item.(map[string]any)
			if o["type"] == "selector" {
				selectorOptions = o
				globalTag = o["tag"].(string)
			}
		}
		if scenario == "global" && selectorOptions == nil {
			t.Fatal("production Global selector absent")
		}
		if scenario == "global" {
			relayIn := []any{}
			members := selectorOptions["outbounds"].([]any)
			for index, member := range members {
				probe, e := net.Listen("tcp", "127.0.0.1:0")
				if e != nil {
					t.Fatal(e)
				}
				p := probe.Addr().(*net.TCPAddr).Port
				_ = probe.Close()
				leg := fmt.Sprintf("relay-server-%c", 'A'+index)
				relayIn = append(relayIn, map[string]any{"type": "vless", "tag": leg, "listen": "127.0.0.1", "listen_port": p, "users": []any{map[string]any{"uuid": "11111111-2222-3333-4444-555555555555"}}})
				for i, item := range outbounds {
					o := item.(map[string]any)
					if o["tag"] == member {
						outbounds[i] = map[string]any{"type": "vless", "tag": member, "server": "127.0.0.1", "server_port": p, "uuid": "11111111-2222-3333-4444-555555555555", "packet_encoding": "xudp"}
					}
				}
			}
			raw["inbounds"] = relayIn
			outbounds = append(outbounds, map[string]any{"type": "selector", "tag": "unrelated-group", "outbounds": members, "default": members[0], "interrupt_exist_connections": true})
		} else {
			raw["inbounds"] = []any{}
		}
		raw["outbounds"] = outbounds
		rules := route["rules"].([]any)
		route["rules"] = append([]any{map[string]any{"inbound": []string{"relay-server-A", "relay-server-B"}, "action": "route", "outbound": "direct"}}, rules...)
	}
	if scenario != "local" {
		socket, e := net.ListenPacket("udp", "127.0.0.1:0")
		if e != nil {
			t.Fatal(e)
		}
		dnsServer := &dns.Server{PacketConn: socket, Handler: dns.HandlerFunc(func(w dns.ResponseWriter, q *dns.Msg) {
			reply := new(dns.Msg)
			reply.SetReply(q)
			if len(q.Question) > 0 {
				reply.Answer = []dns.RR{&dns.A{Hdr: dns.RR_Header{Name: q.Question[0].Name, Rrtype: dns.TypeA, Class: dns.ClassINET, Ttl: 0}, A: net.IPv4(203, 0, 113, 9)}}
			}
			_ = w.WriteMsg(reply)
		})}
		go func() { _ = dnsServer.ActivateAndServe() }()
		defer dnsServer.Shutdown()
		generatedDNS := generated["dns"].(map[string]any)
		for _, item := range generatedDNS["servers"].([]any) {
			server := item.(map[string]any)
			if server["tag"] == "dns-direct" || server["tag"] == "dns-remote" {
				server["type"] = "udp"
				server["server"] = "127.0.0.1"
				server["server_port"] = socket.LocalAddr().(*net.UDPAddr).Port
			}
		}
		raw["dns"] = generatedDNS
	}
	encoded, _ := json.Marshal(raw)
	ctx, cancel := context.WithCancel(include.Context(context.Background()))
	defer cancel()
	options, err := SJ.UnmarshalExtendedContext[option.Options](ctx, encoded)
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
	tracker := &generatedRoutingTracker{tags: make(chan string, 32)}
	if scenario == "global" {
		tracker.legs = make(chan string, 32)
		out, ok := instance.Outbound().Outbound(globalTag)
		if !ok {
			t.Fatal("missing generated selector")
		}
		globalSelector = out.(*group.Selector)
	}
	instance.Router().AppendTracker(tracker)
	cases := []struct{ name, domain, process, want string }{
		{"domain override", "api.example.test", "", "proxy"},
		{"sibling unchanged", "www.example.test", "", "direct"},
		{"case insensitive process", "www.example.test", `C:\Apps\SaMpLe.ExE`, "proxy"},
		{"unrelated process", "www.example.test", `C:\Apps\other.exe`, "direct"},
	}
	if scenario != "local" {
		want := "direct"
		if scenario == "global" {
			want = globalTag
		}
		cases = []struct{ name, domain, process, want string }{{"imported direct destination", "www.example.test", "", want}, {"saved local direct destination", "local.test", "", want}}
	}
	selections := []string{""}
	if globalSelector != nil {
		selections = globalSelector.All()
	}
	for selectionIndex, selection := range selections {
		if globalSelector != nil && !globalSelector.SelectOutbound(selection) {
			t.Fatal("member switch rejected")
		}
		if scenario != "local" {
			t.Run(fmt.Sprintf("member%d/generated-DNS", selectionIndex), func(t *testing.T) {
				query := new(dns.Msg)
				query.SetQuestion(fmt.Sprintf("member%d.example.test.", selectionIndex), dns.TypeA)
				reply, e := service.FromContext[adapter.DNSRouter](ctx).Exchange(ctx, query, adapter.DNSQueryOptions{DisableCache: true, Timeout: 4 * time.Second})
				if e != nil || reply == nil || len(reply.Answer) != 1 {
					t.Fatalf("generated DNS exchange: %v %v", reply, e)
				}
				if globalSelector != nil {
					select {
					case leg := <-tracker.legs:
						if leg != fmt.Sprintf("relay-server-%c", 'A'+selectionIndex) {
							t.Fatalf("DNS egress %s", leg)
						}
					case <-time.After(4 * time.Second):
						t.Fatal("generated remote DNS did not traverse global selector")
					}
				}
			})
		}
		for _, test := range cases {
			for _, network := range []string{"tcp", "udp"} {
				t.Run(fmt.Sprintf("member%d/%s/%s", selectionIndex, test.name, network), func(t *testing.T) {
					meta := adapter.InboundContext{Inbound: "generated-fixture", InboundType: "direct", Network: network, Domain: test.domain, Source: M.ParseSocksaddr("127.0.0.1:32100")}
					if test.process != "" {
						meta.ProcessInfo = &adapter.ConnectionOwner{ProcessPath: test.process}
					}
					var client net.Conn
					if network == "tcp" {
						var server net.Conn
						client, server = net.Pipe()
						defer server.Close()
						meta.Destination = M.ParseSocksaddr(tcp.Addr().String())
						instance.Router().RouteConnectionEx(ctx, server, meta, N.OnceClose(func(error) {}))
					} else {
						packet, e := net.ListenPacket("udp", "127.0.0.1:0")
						if e != nil {
							t.Fatal(e)
						}
						defer packet.Close()
						ingress, e := net.DialUDP("udp", nil, packet.LocalAddr().(*net.UDPAddr))
						if e != nil {
							t.Fatal(e)
						}
						defer ingress.Close()
						meta.Source = M.SocksaddrFromNet(packet.LocalAddr())
						meta.Destination = M.ParseSocksaddr(udp.LocalAddr().String())
						instance.Router().RoutePacketConnectionEx(ctx, bufio.NewUnbindPacketConnWithAddr(ingress, meta.Destination), meta, N.OnceClose(func(error) {}))
						_ = packet.SetDeadline(time.Now().Add(4 * time.Second))
						if _, e = packet.WriteTo([]byte("ping"), ingress.LocalAddr()); e != nil {
							t.Fatal(e)
						}
						var reply [16]byte
						n, _, e := packet.ReadFrom(reply[:])
						if e != nil || string(reply[:n]) != "ping" {
							t.Fatalf("UDP echo %q: %v", reply[:n], e)
						}
					}
					if client != nil {
						defer client.Close()
						_ = client.SetDeadline(time.Now().Add(4 * time.Second))
						if _, e := client.Write([]byte("ping")); e != nil {
							t.Fatal(e)
						}
						var reply [4]byte
						if _, e := io.ReadFull(client, reply[:]); e != nil || string(reply[:]) != "ping" {
							t.Fatalf("TCP echo: %v", e)
						}
					}
					select {
					case tag := <-tracker.tags:
						if tag != test.want {
							t.Fatalf("actual outbound %s, want %s", tag, test.want)
						}
					case <-time.After(4 * time.Second):
						t.Fatal(fmt.Sprintf("expected actual outbound %s", test.want))
					}

					if globalSelector != nil {
						select {
						case leg := <-tracker.legs:
							if leg != fmt.Sprintf("relay-server-%c", 'A'+selectionIndex) {
								t.Fatalf("actual relay %s", leg)
							}
						case <-time.After(4 * time.Second):
							t.Fatal("no actual VLESS relay traffic")
						}
						other, _ := instance.Outbound().Outbound("unrelated-group")
						if other.(*group.Selector).Now() != selections[0] {
							t.Fatal("global member switch changed unrelated group")
						}
					}
				})
			}
		}
	}
}
