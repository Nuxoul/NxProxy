# Bounded local RPC / O2 contract

The existing little-endian framing is unchanged:
request `[u32 id][u16 method length][method][u32 payload length][payload]`,
response `[u32 id][u8 status (0 success / 1 error)][u32 data length][data]`.

## Budgets

| Resource | Bound |
|---|---:|
| UTF-8 method name | 1–128 bytes |
| Ordinary request payload | 96 MiB |
| Ordinary response payload | 128 MiB |
| Control request / response payload | 64 KiB |
| Ordinary concurrent work / client pending | 8 |
| Reserved control work / client pending | 4 |
| Go waiting work queue | 0 (immediate status=1 `rpc busy`) |
| Go response queues | 8 ordinary / 4 control; control prioritized |
| Go process-wide active serialized request bytes | 128 MiB ordinary + 256 KiB control |
| Go process-wide serialized response bytes | 192 MiB ordinary + 256 KiB control |
| Client queued/current frames | 128 MiB ordinary / 4 × (64 KiB + 138) control; queues also 8 / 4 entries |
| Client held/current response bytes | 192 MiB ordinary + 256 KiB control |
| QLocalSocket read / write window | 1 MiB each (64 KiB pumps) |
| Partial frame / stalled write deadline | 30 seconds |
| Diagnostics compressed archive buffer | 127 MiB |
| Diagnostics CPU / trace staging buffer | 32 MiB each |

These are transport/serialized/staging budgets, **not a cap on the entire process heap**.
Handlers may create decoded configurations, result snapshots, network state and protobuf copies;
there are at most eight ordinary transport handlers. Go admission budgets are process-wide so
reconnect cannot grow a new pool while an old atomic-completion call finishes. There is no
ordinary waiting work queue; heavy calls never occupy reserved control admission/output budgets.
A frame already started cannot be preempted without violating compatibility: control responses
wait for that frame or its 30-second deadline. Saturation yields deterministic `rpc busy`; invalid
lengths/duplicate active IDs break the connection before allocation. A non-reading peer that
fills the bounded response rejection queue is disconnected. C++ errors -1920 and -1921 denote
client busy and limit rejection (-1919 remains disconnected).

96 MiB requests deliberately allow a 64 MiB subscription/configuration plus protobuf and
secondary-config overhead; the synthetic real-net.Pipe 64 MiB test passes. 128 MiB responses
allow diagnostics' existing 32 MiB attachments plus 32 MiB trace and profiling/zip overhead;
the 65 MiB synthetic diagnostic response and 32 MiB attachment tests pass. These are synthetic
compatibility samples, not measurements of private user subscriptions. A configuration that
expands beyond 96 MiB or a diagnostics capture beyond its archive cap fails explicitly rather
than truncating or allocating indefinitely.

## Capability / cancellation matrix

`QueryCapabilities(EmptyReq) -> RPCCapabilities` is additive; version 1 exposes limits,
slot counts, `cancel_request` and `cancelable_methods`. The C++ client negotiates once per
connection asynchronously, clearing capabilities on disconnect/reconnect. Unknown method,
malformed response or handshake timeout means conservative old-core mode (no speculative
CancelRequest); existing StopTests / StopDiagnostics methods remain available.

`CancelRequest { req_id } -> { accepted }` only addresses work belonging to the same connection.
Accepted is false for unknown/finished IDs or operations with atomic-completion semantics.
Supported: Test, IPTest, SpeedTest, CaptureDiagnostics, WarpRegister, UpdateRuleSets,
QueryVPNStatus. They inherit the per-request/per-connection context. Timeouts send a bounded,
capability-gated control request; `API::Client::CancelPendingCalls()` cooperatively cancels the
currently pending supported calls. This method does not cancel Start or DB/filesystem writes,
does not disconnect the transport and does not invoke legacy global stops implicitly. Background
owners targeting an old core should keep their existing StopTests/StopDiagnostics fallback.
The client retains at most eight cancellation intents, deduplicates them by connection/ID
and drains them as the four control slots free. A coalesced IO wakeup prevents an unbounded
Qt event backlog. Like any bounded admission path, continuing to create new work after
cancellation saturation can reject extra work/cancellation intents; callers should stop
producing work before cancellation. Bulk cancellation never creates an unbounded queue.


Start, Stop, configuration preparation and filesystem/DB mutations finish safely: no goroutine
is killed or lifecycle transaction interrupted. Probe environment construction/cleanup may
finish before observing cancellation; probe execution carries both the request context and the
captured legacy StopTest generation. A request cancel never resets/cancels another test generation.
Diagnostics cancellation stops the capture window then completes profiler/archive cleanup.
Rule-set workers are bounded to five; VPN status wait workers to sixteen; probe admission is
cancellation-aware. Cancellation during profiler/GC finalization is not an immediate abort.

## Validation

From `core`:
```
go test ./internal/rpc ./internal/probe ./internal/xray ./internal/xraydns
go vet ./internal/rpc ./internal/probe
go test ./internal/rpc -run '^TestRPC' -count=10
go test ./internal/rpc -run '^$' -bench BenchmarkRPCStress -benchtime=10x -benchmem
```

Windows repeatable standalone runner (existing configured build, no full GUI build):
```
powershell -File tests/run_rpc_transport.ps1 -Repeat 10
```
Override `-BuildDir`, `-QtRoot`, `-VcVars` when needed. Build intermediates go to
`PI_SCRATCH_DIR` (or the system temporary directory), never the source tree.

`tests/rpc_transport.cpp` is a standalone real-QLocalSocket test TU; it includes the actual
RPC.cpp rather than a mock transport. Compile C++20 against repo/root, build/root, generated
`myproto`, simple-protobuf headers, Qt Core/Network/Gui/Widgets and the existing dependency
include paths. It supplies an inert log callback and starts only a synthetic local peer, never
the application/core. Generate `myproto` through the normal CMake target first. No CMake GUI
main/source changes are needed. Go output is generated normally with protoc's `--go_out=.`
and `--go-grpc_out=.` from core/gen; C++ uses the existing simple-protobuf CMake pipeline.

Actual Windows/MSVC results (2026-10-09): Go focused packages and vet pass; ten repetitions
of TestRPC pass; standalone QLocalSocket test passes ten consecutive runs (fragmented responses,
oversize rejection, ordinary busy, reserved StopTest, timeout cancel, eight simultaneous
timeouts drained through four reserved control slots, late response, reconnect, old-core
fallback). Go tests also cover half/oversized requests, full/zero/error short writes,
stalled write deadline, bounded control admission, atomic Start completion, disconnect recovery,
large samples and cooperative legacy/request contexts.

Stress: 10 × 1000 excess requests with 8 held ordinary calls and reserved StopTest. 22.92 ms/op,
931374 B/op, 22367 allocs/op; observed final stress-cycle heap 2.408 MiB, recovered GC heap
1.781 MiB, goroutine delta 0, peak work slots 8. The heap sample is a cycle-end sample, not
continuous process peak telemetry. Windows `go test -race` is blocked by missing gcc/cgo;
this is not a race pass. Full GUI/CTest integration is intentionally deferred to the parent
while other agents edit GUI sources. No commit/push or live application action was performed.
