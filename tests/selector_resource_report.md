# O6 selector resource sharing — focused verification

## Integration contract

- `BuildSingBoxConfig(ent, BuildConfigOptions{true})` enables the bounded generation path. The existing one-argument overload deliberately leaves sharing OFF until O5 installs chain-based connection display. Parent can opt in at its normal generation call or change the option default after integration acceptance. `false` is the independent-resource rollback/A-B path.
- `BuildConfigResult::outboundDisplayNames`: `selector-<profileID>` maps to **group name only**; concrete member outbound tags map to **node name only**. This applies to both paths. Compose labels using the actual connection chain (normalize its direction in O5), not selector's current pick: a pre-switch connection must retain its actual node. Do not reverse-map a shared node tag to a unique group.
- `selectorMemberTags[selectorID][memberID]` remains populated for every group/member, even when values share the same tag. Existing `SetSelectorMemberTags` / `SelectorMemberTag` need no implementation change; their key remains the pair of IDs.
- `chainGroups` contains one watchTag/one profile credit for each shared member. Duplicate selector references do not append duplicate watched groups. Selectors themselves were not credited by the old member path and are not added now.
- Fixed same-profile ordinary sing-box route-selector members only: VLESS, VMess, Trojan, Shadowsocks, SOCKS, HTTP. Complete effective emitted JSON minus generated tag plus profile ID is the exact identity; no lossy hash or user-visible name. DNS/TLS/dial/transport/mux fields are included. The registry is local to one build; settings/DNS backend are common to that build. No cross-build/version cache.
- Any nonempty detour prevents sharing. Main path, route chains, autoselectors, custom outbound/full configs, Xray, extra-core and endpoints are not shared. Existing exclusions/errors for full config/extra-core selector members remain; WireGuard and custom outbounds remain independent. No dependency fork edits.

## Actual BuildConfig against temporary SQLite DB

Command (MSVC + Qt, no full GUI build):

```powershell
& D:\Code\NxProxy\tests\selector_resource_run.ps1
```

Runner recompiles actual generation, repository/DB units and test entry in session scratch, links existing supporting build-local objects, and executes an offscreen QApplication against a fresh temporary database. The test-only unused GUI stop hook throws if unexpectedly invoked. It does not configure/build or write into build-local. Existing Qt/MSVC build-local supporting artifacts are a prerequisite, not a fully isolated clean build.

Final passing run, three repetitions each; counts and bytes deterministic:

| Dataset | Outbounds independent → shared | Compact config bytes independent → shared | Generation microseconds independent → shared (3 runs) |
|---|---:|---:|---|
| 8 selectors × 15 VLESS profiles | 130 → 25 (-80.8%) | 21,488 → 6,250 (-70.9%) | 6547→2267; 7721→2561; 7128→1801 |
| 8 selectors × 30 VLESS profiles | 250 → 40 (-84.0%) | 40,331 → 9,730 (-75.9%) | 12593→3673; 13514→3660; 12651→4102 |

Measured JSON/outbound counts are from real `BuildSingBoxConfig` results, not an extrapolated model. This compares actual independent/shared generation options, not a reconstructed historical binary. Times include focused test validation overhead and warm caches and are not core startup latency.

Assertions passed: unique tags/watchTags/profile credits; exact effective outbound JSON equivalence; involved profile set unchanged; each of 8 independent defaults/interrupt options; shared runtime member mappings; group/node display separation; port and TLS/dial edits reflected on rebuild; same-name/identical-config different profile IDs remain distinct; custom detour outbound stays independent; 8 independent WireGuard endpoints; full config ignored as member and existing standalone fallback unchanged; invalid selectedID repaired without DB mutation; one-argument build keeps sharing disabled.

Logs: `$env:PI_SCRATCH_DIR\selector-resource-harness\{compile.log,link.log,results.log}`.

## Actual fork selector TCP/UDP isolation

New `core/internal/rpc/selector_sharing_test.go` creates a real sing-box instance with one VLESS outbound shared by actual selectors A/B, a loopback VLESS inbound, and loopback TCP/UDP echo destinations. It asserts VLESS does not implement the Handler interfaces that bypass selector wrapping. Both groups send TCP and UDP successfully; switching A closes A's TCP/UDP immediately (timeout is explicitly a failure), while B continues echoing; switching A back and B's no-op selection do not close B.

```powershell
Set-Location D:\Code\NxProxy\core
$env:GOTOOLCHAIN='local'; $env:GOPROXY='off'
go test ./internal/rpc -run '^TestSelectorSharingInterruption$' -count=3 -v
```

PASS three repetitions, ~0.02–0.03 s/test. Uses the cached pinned fork `github.com/throneproj/sing-box@v1.11.16-0.20260922234629-af99ee4fc5d6`. Fork selector.go lines 47–59 allocate each interruptGroup, 140 interrupts only that group, 147–160 wrap TCP/UDP. Lines 163–180 bypass wrapping for Handler members; supported outbound implementations were inspected and have no such methods.

`go test -race ...` BLOCKED at build: cgo C compiler `gcc` is not on PATH. Not a race pass.

## Remaining acceptance limitations

- Direct Dial/Listen isolation and actual routed Selector Handler entrypoints are now covered on shared VLESS (see follow-up below). Not covered: mixed/SOCKS ingress protocol parsing, Handler-capable member bypass branch, multiplex, TLS/other ordinary protocols, connection-chain metadata or live connectionLister/traffic RPC end-to-end.
- Actual idle box lifecycle/Go heap/goroutine/resource measurements are now covered below. RSS, loaded/active-connection memory, switch-latency benchmarking, full GUI build and full regressions remain unmeasured. O5/parent owns display integration and enabling sharing globally.
- `git diff --check` passed for owned tracked files. No commits, pushes, live profile modifications or dependency changes.

## Follow-up: actual box lifecycle / runtime resource acceptance

Added `core/internal/rpc/selector_resource_acceptance_test.go`, without changing existing production code or the original Dial/Listen test. Environment: **Go 1.26.3, windows/amd64**, same pinned sing-box fork as above.

```powershell
Set-Location D:\Code\NxProxy\core
$env:GOTOOLCHAIN='local'; $env:GOPROXY='off'
# One parent run launches three fresh child processes per dataset/mode (12 total).
go test ./internal/rpc -run '^TestSelectorResourceLifecycleMeasurements$' -count=1 -v
# Actual routed Handler-path isolation, three repetitions.
go test ./internal/rpc -run '^TestSelectorSharingRoutedHandlers$' -count=3 -v
# Combined focused regression; lifecycle test itself still performs three repetitions.
go test ./internal/rpc -run '^(TestSelectorResourceLifecycleMeasurements|TestSelectorSharingInterruption|TestSelectorSharingRoutedHandlers)$' -count=1 -v
```

All commands **PASS**. Lifecycle fixture emits equivalent plain VLESS/xudp node configs for 8 independent selectors and 15/30 distinct nodes. Independent mode creates one node object per group/member; shared mode creates one object per node and changes selector references accordingly. Each of the eight actual selectors' member count/default is asserted. Actual outbound-manager counts: **129→24** for 15 nodes (**120→15 VLESS**, 8 selectors + 1 direct); **249→39** for 30 nodes (**240→30 VLESS**, 8 selectors + 1 direct). The earlier C++ generation dataset has an additional main outbound, hence counts differ by one.

Method: configuration JSON is decoded before baseline; GC + `runtime.ReadMemStats` establishes the baseline with parsed options/registries alive. Time `box.New`, `Start` and `Close` separately using `time.Since`, excluding JSON decoding, fixed 20 ms idle interval and measurement GC. Heap/objects/allocs are **process-wide deltas from baseline**, not isolated outbound allocations; post-GC live values keep options and the running box reachable. Separate fresh child process per measurement; even repetitions reverse independent/shared launch order. No TUN, no inbound listener in lifecycle fixture, no profile/network traffic and no real node dials. The box can inspect local OS interfaces during startup.

Final combined run at 2026-10-09 13:08:46; times in **microseconds**, heap in **bytes**. Zero time values mean below observed Windows timer resolution, not zero work. These small time samples are noisy and **do not establish a startup/Close or network-latency speedup**; 30-node Start is not consistently faster under sharing.

| Nodes | Mode | Run | New µs | Start µs | Close µs | HeapAlloc Δ | Post-GC live heap Δ | TotalAlloc Δ |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
|15|independent|1|1002|18997|534|256536|197256|694912|
|15|shared|1|0|19999|0|85768|57688|299752|
|15|independent|2|0|20748|0|251392|192304|689768|
|15|shared|2|533|18572|0|90704|62768|304848|
|15|independent|3|566|24019|0|251472|187288|689896|
|15|shared|3|999|19999|0|85512|57784|299656|
|30|independent|1|1053|22014|1000|526896|420104|1165920|
|30|shared|1|14|20571|0|123872|90776|378472|
|30|independent|2|999|20999|1000|522072|415280|1160936|
|30|shared|2|999|22655|0|123632|90552|378440|
|30|independent|3|998|21001|999|522040|415408|1161016|
|30|shared|3|1000|22001|0|128936|95888|383616|

| Nodes | Mode | Run | Mallocs Δ | HeapObjects Δ | Post-GC live objects Δ | Goroutines baseline→started→closed |
|---:|---|---:|---:|---:|---:|---|
|15|independent|1|5372|2108|1144|3→3→3|
|15|shared|1|1611|527|292|3→3→3|
|15|independent|2|5365|2101|1140|3→3→3|
|15|shared|2|1618|533|298|3→3→3|
|15|independent|3|5367|2102|1135|3→3→3|
|15|shared|3|1609|524|293|3→3→3|
|30|independent|1|9714|3917|2105|3→3→3|
|30|shared|1|2218|754|414|3→3→3|
|30|independent|2|9711|3911|2099|3→3→3|
|30|shared|2|2216|748|409|3→3→3|
|30|independent|3|9713|3915|2104|3→3→3|
|30|shared|3|2224|756|419|3→3→3|

Observed median post-GC live heap delta: 15 nodes **192,304→57,784 bytes** (~70.0% lower); 30 nodes **415,408→90,776 bytes** (~78.1% lower). Idle goroutines are unchanged, **not a goroutine-saving claim**. All 12 children returned to the baseline goroutine count after Close. HeapObjects are Go heap object deltas, separately reported from actual VLESS/selector/outbound object counts.

Raw final JSON metrics/log: `$env:PI_SCRATCH_DIR\selector-resource-acceptance.log`. Earlier standalone lifecycle log: `$env:PI_SCRATCH_DIR\selector-resource-lifecycle.log` (different noisy timings; final tables use combined run only).

### Follow-up: actual routed NewConnection / NewPacketConnection

The original test called Selector `DialContext` / `ListenPacket` directly and **did not** exercise its Handler entrypoints. The new `TestSelectorSharingRoutedHandlers` feeds an actual running fork router via `RouteConnectionEx` (TCP net.Pipe ingress) and `RoutePacketConnectionEx` (connected loopback UDP ingress). Real route rules select A/B using fixture inbound metadata. A thread-safe observer confirms the actual router-selected outbound objects/tags. The router dispatches through its actual Handler branches (`route/route.go:173–176`, `308–311`) into `Selector.NewConnection` / `NewPacketConnection`; VLESS implements neither member Handler interface, so Selector delegates through the real ConnectionManager to its own interrupt-wrapped Dial/Listen. No mocked selector, router, transport or dependency changes.

Both A and B concurrently carry TCP/UDP through the **same actual VLESS outbound**, a loopback VLESS inbound and TCP/UDP echo targets. Assertions: all four flows echo; switch A → both A close callbacks arrive within 3 s and A TCP ingress closes rather than timing out; B's close callbacks stay absent and its TCP/UDP continue echoing; A reselect and B no-op reselect leave B alive; finally switching B closes both B flows. Fresh routed run `-count=3` **PASS**, 0.05/0.03/0.04 s per test. This proves routed selector-wrapper interruption on VLESS, **not the bypass branch for Handler-capable member types**, which remains unsupported for sharing. UDP interruption is evidenced by real copy-loop close callback, not by waiting for an unconnected client socket to error.

Remaining boundaries: these ingress fixtures inject metadata and skip mixed/SOCKS protocol parsing; no full live RPC/UI/traffic/chain display acceptance, no TLS/mux/other protocol transport combination, active-load heap/RSS or switch/network latency measurement. Race checking remains blocked by the previously observed missing cgo `gcc`; no installation attempted. Parent owns O5 integration and activation. No production edits, commits or pushes in this follow-up.
