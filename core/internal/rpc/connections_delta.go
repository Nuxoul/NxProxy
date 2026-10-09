package rpc

import (
	"ThroneCore/gen"
	"context"
	"fmt"
	"google.golang.org/protobuf/proto"
	"sync"
	"time"
)

const maxDeltaClients = 4
const maxDeltaRecords = 10000
const deltaClientLifetime = 2 * time.Minute

type connectionDeltaClient struct {
	cursor  uint64
	seen    time.Time
	records map[string]*gen.ConnectionMetaData
}
type connectionDeltaCache struct {
	mu       sync.Mutex
	instance any
	epoch    uint64
	clients  map[string]*connectionDeltaClient
}

var deltaConnections = connectionDeltaCache{clients: make(map[string]*connectionDeltaClient)}

func sameConnectionIdentity(a, b *gen.ConnectionMetaData) bool {
	// Compare immutable fields without cloning metadata or serializing it again.
	if a.GetCreatedAt() != b.GetCreatedAt() || a.GetOutbound() != b.GetOutbound() || a.GetNetwork() != b.GetNetwork() || a.GetDest() != b.GetDest() || a.GetProtocol() != b.GetProtocol() || a.GetDomain() != b.GetDomain() || a.GetProcess() != b.GetProcess() || a.GetProcessPath() != b.GetProcessPath() || a.GetSource() != b.GetSource() || len(a.GetChain()) != len(b.GetChain()) {
		return false
	}
	for i, v := range a.GetChain() {
		if b.GetChain()[i] != v {
			return false
		}
	}
	return true
}

// Non-draining, bounded snapshots; full legacy QueryConnections remains available.
func (s *server) QueryConnectionsDelta(ctx context.Context, in *gen.QueryConnectionsDeltaRequest) (*gen.QueryConnectionsDeltaResponse, error) {
	if len(in.GetClientId()) == 0 || len(in.GetClientId()) > 128 {
		return nil, fmt.Errorf("invalid connection delta client id")
	}
	box := currentBox()
	snapshot, err := s.QueryConnections(ctx, &gen.EmptyReq{})
	if err != nil {
		return nil, err
	}
	if currentBox() != box {
		return nil, fmt.Errorf("connection instance changed during snapshot; resync")
	}
	if len(snapshot.Active)+len(snapshot.Closed) > maxDeltaRecords {
		return nil, fmt.Errorf("connection delta record budget exceeded; use full snapshot")
	}
	return deltaConnections.diff(box, in, snapshot, time.Now()), nil
}
func (c *connectionDeltaCache) diff(instance any, in *gen.QueryConnectionsDeltaRequest, snapshot *gen.QueryConnectionsResp, now time.Time) *gen.QueryConnectionsDeltaResponse {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.clients == nil {
		c.clients = make(map[string]*connectionDeltaClient)
	}
	if c.instance != instance {
		c.instance = instance
		c.epoch++
		c.clients = make(map[string]*connectionDeltaClient)
	}
	for id, client := range c.clients {
		if now.Sub(client.seen) > deltaClientLifetime {
			delete(c.clients, id)
		}
	}
	client := c.clients[in.GetClientId()]
	full := client == nil || in.GetEpoch() != c.epoch || in.GetCursor() != client.cursor
	if client == nil {
		if len(c.clients) >= maxDeltaClients {
			var oldest string
			var when time.Time
			for id, v := range c.clients {
				if oldest == "" || v.seen.Before(when) {
					oldest = id
					when = v.seen
				}
			}
			delete(c.clients, oldest)
		}
		client = &connectionDeltaClient{records: make(map[string]*gen.ConnectionMetaData)}
		c.clients[in.GetClientId()] = client
	}
	response := &gen.QueryConnectionsDeltaResponse{Full: To(full), Epoch: To(c.epoch), Cursor: To(client.cursor + 1)}
	records := make(map[string]*gen.ConnectionMetaData, len(snapshot.Active)+len(snapshot.Closed))
	add := func(record *gen.ConnectionMetaData) {
		id := record.GetId()
		if id == "" {
			return
		}
		// Active wins if a tracker is present in both snapshots during close transition.
		if _, exists := records[id]; exists {
			return
		}
		records[id] = record
		old := client.records[id]
		if full || old == nil || !sameConnectionIdentity(old, record) {
			response.Added = append(response.Added, record)
			return
		}
		if old.GetUpload() != record.GetUpload() || old.GetDownload() != record.GetDownload() || old.GetClosedAt() != record.GetClosedAt() {
			response.Changed = append(response.Changed, &gen.ConnectionCounterDelta{Id: To(id), Upload: To(record.GetUpload()), Download: To(record.GetDownload()), ClosedAt: To(record.GetClosedAt())})
		}
	}
	for _, record := range snapshot.Active {
		add(record)
	}
	for _, record := range snapshot.Closed {
		add(record)
	}
	if !full {
		for id := range client.records {
			if records[id] == nil {
				response.Removed = append(response.Removed, id)
			}
		}
	}
	client.records = records
	client.cursor++
	client.seen = now
	return response
}

// Compile-time payload contract assertion: additive messages remain protobuf messages.
var _ proto.Message = (*gen.QueryConnectionsDeltaResponse)(nil)
