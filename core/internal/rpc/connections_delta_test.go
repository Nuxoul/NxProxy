package rpc

import (
	"ThroneCore/gen"
	"fmt"
	"google.golang.org/protobuf/proto"
	"testing"
	"time"
)

func deltaRecord(id string, up, down, closed int64) *gen.ConnectionMetaData {
	return &gen.ConnectionMetaData{Id: To(id), CreatedAt: To(int64(100)), Upload: To(up), Download: To(down), ClosedAt: To(closed), Outbound: To("route-shared"), Network: To("tcp"), Dest: To("203.0.113.1:443"), Domain: To("example.com"), Process: To("test.exe"), ProcessPath: To(`C:\fixed\test.exe`), Chain: []string{"route-shared", "selector-31"}, Source: To("127.0.0.1:12345")}
}
func deltaReq(id string, r *gen.QueryConnectionsDeltaResponse) *gen.QueryConnectionsDeltaRequest {
	q := &gen.QueryConnectionsDeltaRequest{ClientId: To(id)}
	if r != nil {
		q.Epoch = r.Epoch
		q.Cursor = r.Cursor
	}
	return q
}
func applyDeltaTest(records map[string]*gen.ConnectionMetaData, d *gen.QueryConnectionsDeltaResponse) map[string]*gen.ConnectionMetaData {
	if d.GetFull() {
		records = make(map[string]*gen.ConnectionMetaData)
	}
	for _, id := range d.Removed {
		delete(records, id)
	}
	for _, r := range d.Added {
		records[r.GetId()] = proto.Clone(r).(*gen.ConnectionMetaData)
	}
	for _, v := range d.Changed {
		r := records[v.GetId()]
		r.Upload = v.Upload
		r.Download = v.Download
		r.ClosedAt = v.ClosedAt
	}
	return records
}
func TestConnectionsDeltaLifecycle(t *testing.T) {
	c := connectionDeltaCache{}
	now := time.Unix(1, 0)
	instance := new(int)
	snap := &gen.QueryConnectionsResp{Active: []*gen.ConnectionMetaData{deltaRecord("a", 10, 20, 0)}}
	first := c.diff(instance, deltaReq("one", nil), snap, now)
	if !first.GetFull() || len(first.Added) != 1 {
		t.Fatal("initial full")
	}
	records := applyDeltaTest(nil, first)
	unchanged := c.diff(instance, deltaReq("one", first), snap, now)
	if unchanged.GetFull() || len(unchanged.Added)+len(unchanged.Changed)+len(unchanged.Removed) != 0 {
		t.Fatal("unchanged repeated metadata")
	}
	second := c.diff(instance, deltaReq("two", nil), snap, now)
	if !second.GetFull() {
		t.Fatal("independent consumer")
	}
	// Final counters plus a short-lived record unseen in active polls remain in the ring.
	snap = &gen.QueryConnectionsResp{Closed: []*gen.ConnectionMetaData{deltaRecord("a", 15, 40, 200), deltaRecord("short", 3, 7, 201)}}
	closed := c.diff(instance, deltaReq("one", unchanged), snap, now)
	if len(closed.Changed) != 1 || len(closed.Added) != 1 {
		t.Fatal("close delta")
	}
	records = applyDeltaTest(records, closed)
	if records["a"].GetUpload()+records["short"].GetUpload() != 18 || records["a"].GetDownload()+records["short"].GetDownload() != 47 {
		t.Fatal("final byte equality")
	}
	ring := c.diff(instance, deltaReq("two", second), snap, now)
	if len(ring.Added) != 1 || len(ring.Changed) != 1 {
		t.Fatal("ring was drained by first consumer")
	}
	// Client retries an old cursor after losing a response: explicit full resync.
	missed := c.diff(instance, deltaReq("one", unchanged), snap, now)
	if !missed.GetFull() || len(missed.Added) != 2 {
		t.Fatal("lost response resync")
	}
	evicted := c.diff(instance, deltaReq("one", missed), &gen.QueryConnectionsResp{}, now)
	if len(evicted.Removed) != 2 {
		t.Fatal("closed ring eviction")
	}
	restarted := c.diff(new(int), deltaReq("one", evicted), snap, now)
	if !restarted.GetFull() || restarted.GetEpoch() == evicted.GetEpoch() {
		t.Fatal("instance replacement")
	}
	expired := c.diff(c.instance, deltaReq("one", restarted), snap, now.Add(deltaClientLifetime+time.Second))
	if !expired.GetFull() {
		t.Fatal("client expiration")
	}
	for i := 0; i < maxDeltaClients+5; i++ {
		c.diff(c.instance, deltaReq(fmt.Sprint(i), nil), snap, now.Add(deltaClientLifetime+2*time.Second))
	}
	if len(c.clients) > maxDeltaClients {
		t.Fatal("unbounded consumers")
	}
	// Same ID metadata changes require replacement, not just numeric fields.
	old := c.diff(c.instance, deltaReq("meta", nil), snap, now)
	replacement := proto.Clone(snap).(*gen.QueryConnectionsResp)
	replacement.Closed[0].Chain = []string{"route-shared", "selector-32"}
	changed := c.diff(c.instance, deltaReq("meta", old), replacement, now)
	if len(changed.Added) != 1 {
		t.Fatal("chain metadata replacement")
	}
	t.Log("PASS full/delta/close/short-lived/multi-consumer/cursor/epoch/TTL/capacity/chain")
}
func TestConnectionsDeltaActiveWins(t *testing.T) {
	c := connectionDeltaCache{}
	snap := &gen.QueryConnectionsResp{Active: []*gen.ConnectionMetaData{deltaRecord("a", 10, 20, 0)}, Closed: []*gen.ConnectionMetaData{deltaRecord("a", 5, 10, 200)}}
	r := c.diff(new(int), deltaReq("one", nil), snap, time.Now())
	if len(r.Added) != 1 || r.Added[0].GetClosedAt() != 0 {
		t.Fatal("duplicate ring won active snapshot")
	}
}
func deltaDataset(n int) *gen.QueryConnectionsResp {
	r := &gen.QueryConnectionsResp{}
	for i := 0; i < n; i++ {
		r.Active = append(r.Active, deltaRecord(fmt.Sprint(i), int64(i), int64(i*2), 0))
	}
	return r
}
func TestConnectionsDeltaMeasurements(t *testing.T) {
	for _, n := range []int{100, 1000, 5000} {
		snap := deltaDataset(n)
		for repeat := 0; repeat < 3; repeat++ {
			c := connectionDeltaCache{}
			instance := new(int)
			resp := c.diff(instance, deltaReq("bench", nil), snap, time.Now())
			q := deltaReq("bench", resp)
			start := time.Now()
			var fullBytes int
			for i := 0; i < 100; i++ {
				data, e := proto.Marshal(snap)
				if e != nil {
					t.Fatal(e)
				}
				fullBytes = len(data)
			}
			fullTime := time.Since(start)
			start = time.Now()
			var deltaBytes int
			for i := 0; i < 100; i++ {
				resp = c.diff(instance, q, snap, time.Now())
				q = deltaReq("bench", resp)
				data, e := proto.Marshal(resp)
				if e != nil {
					t.Fatal(e)
				}
				deltaBytes = len(data)
			}
			deltaTime := time.Since(start)
			// Changing all counters still avoids immutable strings.
			changed := proto.Clone(snap).(*gen.QueryConnectionsResp)
			for _, r := range changed.Active {
				r.Upload = To(r.GetUpload() + 1)
				r.Download = To(r.GetDownload() + 1)
			}
			response := c.diff(instance, q, changed, time.Now())
			data, e := proto.Marshal(response)
			if e != nil {
				t.Fatal(e)
			}
			t.Logf("MEASURE n=%d repeat=%d full_bytes=%d unchanged_delta_bytes=%d all_counters_delta_bytes=%d full_100_us=%d delta_100_us=%d", n, repeat+1, fullBytes, deltaBytes, len(data), fullTime.Microseconds(), deltaTime.Microseconds())
		}
	}
}
func BenchmarkConnectionsDelta(b *testing.B) {
	for _, n := range []int{100, 1000, 5000} {
		snap := deltaDataset(n)
		b.Run(fmt.Sprintf("full_%d", n), func(b *testing.B) {
			b.ReportAllocs()
			for i := 0; i < b.N; i++ {
				_, _ = proto.Marshal(snap)
			}
		})
		b.Run(fmt.Sprintf("unchanged_%d", n), func(b *testing.B) {
			c := connectionDeltaCache{}
			instance := new(int)
			resp := c.diff(instance, deltaReq("bench", nil), snap, time.Now())
			q := deltaReq("bench", resp)
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				resp = c.diff(instance, q, snap, time.Now())
				q = deltaReq("bench", resp)
				_, _ = proto.Marshal(resp)
			}
		})
	}
}
