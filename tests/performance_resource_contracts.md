# Performance and Resource Contracts

## 1. Scope / Trigger
Applies to test-result persistence, local RPC, background task shutdown, core recovery, connection snapshots and selector generation. Source: D:\Code\NxProxy, baseline d3bc0e21, task 10-09-performance-stability. Preserve refresh/concurrency/lifecycle contracts; do not infer network latency gains from local benchmarks.

## 2. Signatures
- `TestResultSaveOutcome ProfilesRepo::SaveTestResults(const std::vector<TestResult>&)`; outcome has `success` and committed `appliedIDs`.
- `bool runOnBackgroundTask(callback, QObject* owner=nullptr)`, `waitForBackgroundWork(ms)`, `requestBackgroundStop()`, `finishBackgroundWork(timeoutMs)`.
- `CoreProcess::{CancelAutomaticRestart,NoteProfileExpected,IsRestartCurrent,NoteProfileRunning,NoteConfigurationRejected,SetProfileToRestore,ConsumeProfileToRestore}`.
- `MainWindow::profile_start(id, automaticRecovery=false, recoveryToken=0)`; replacement stop uses `replacingProfile=true`.
- Additive `QueryCapabilities`, `CancelRequest {req_id}` and `QueryConnectionsDelta {client_id,epoch,cursor}` RPCs.
- Delta response: `full,epoch,cursor,added(ConnectionMetaData),changed(id,upload,download,closed_at),removed(ids)`; counters are absolute, never incremental.
- `ConnectionSnapshot::{request,apply,snapshot,reset}`, `ConnectionAccounting::consume(snapshot,credit)`.
- `BuildSingBoxConfig(profile, BuildConfigOptions{shareSelectorOutbounds})`; one-argument overload retains independent resources for A/B comparisons. Production starts explicitly enable sharing.

## 3. Contracts
### Test results
Synchronous one-transaction UPDATE-only writes own latency/timestamp, requested speed fields, country/IP and health fields only. Never serialize/write configuration, identity or traffic. Deleted IDs remain absent. Commit precedes cache publication; rollback returns failure/empty IDs and denies cleanup. File-backed result connection shares the same DB path.

### Local RPC
LE framing is unchanged. Method 1–128 B; ordinary request 96 MiB; response 128 MiB; control request/response 64 KiB. Eight ordinary and four reserved control slots; ordinary serialized input/output budgets 128/192 MiB, separate control budgets; no unbounded waiting work queue. Validate lengths before allocation, handle complete/partial writes, use 30 s stalled-frame/write deadlines. Budgets constrain transport/staging, not all decoded heap.

Cancellation is connection-local and capability-gated: Test/IPTest/SpeedTest/CaptureDiagnostics/WarpRegister/UpdateRuleSets/QueryVPNStatus. Start/Stop and unsafe mutations finish atomically. Unknown method/handshake failure means legacy mode; keep StopTests/StopDiagnostics fallback. A wire frame already started cannot be preempted without breaking framing. See source `tests/rpc_transport_README.md` for precise queue/window/staging budgets.

### Background and recovery
Short work: four reusable workers, 64 admitted tasks including running work; false means caller must report rejection. Owner guard checks queued work before invocation. Blocking workers remain separate and tracked; safe exception unwind releases capacity. Wakeable waits replace uninterruptible long sleeps. Shutdown closes admission, cancels polling/tests, preserves synchronous stop/flush, kills core, pumps accepted completions with a deadline and records unfinished work. Never count forced fixture cleanup as normal exit validation.

Recovery: five retries at 200/400/800/1600/3200 ms, delay cap 5 s. Only 30 s of successfully configured uptime resets the streak; IPC readiness alone does not. Launch/configuration rejection stops current recovery. Reserve the start slot BEFORE changing intent; rejected duplicate starts must not invalidate in-flight work. Capture token before build/RPC; queued CoreStarted restoration carries the original token. Manual stop invalidates before early return, crash/replacement cleanup does not. Stale successful atomic Start must Stop/unwind rather than silently leave an unpublished config. Cross-thread core pointer hooks hold coreProcessMutex; QProcess actions run on DS_cores. Intentional kill is not an error-level unexpected crash.

### Connection accounting and UI
Delta cache: at most four consumers, 10,000 records each, TTL two minutes; core instance replacement changes epoch. Cursor mismatch/expiry returns a full snapshot. Full legacy method remains available; closed ring is non-draining. Reconnect invalidates client reconstruction; delta failure resets then falls back to full. Failed full RPC preserves accounting baselines rather than masquerading as an empty snapshot. Core still scans O(N) and creates metadata snapshots; serialization/transport reduction is not scan elimination.

Each poll is accounted before UI coalescing. Active wins a same-ID active/closed overlap, but MUST NOT mark that closed ID finally accounted: the next closed-only sample credits its tail against the previous active counters. Deduplicate closed ring and short-lived records. Caches self-prune to current snapshot/current rows; UI keeps one latest pending display snapshot. Fixed topology/order uses numeric dataChanged updates and preserves persistent indexes/close IDs; topology/sort changes use the existing structural path. Include actual chain in leaf grouping keys: a shared member reached through AI and Google must remain distinct. Display uses actual recorded chain, never current selected member.

### Selector resources
Build-local identity includes profile ID and complete effective emitted runtime JSON minus generated tag. Only compatible fixed VLESS/VMess/Trojan/Shadowsocks/SOCKS/HTTP selector members with empty detour are eligible; endpoints/custom/Xray/extra-core/chains/autoselectors retain independence. Each selector keeps its own tag/default/selection/interrupt group. Shared watch tags credit each same profile once. Member labels are node-only; selector labels group-only. Pair `(selectorID,memberID)` resolves live tags; never reverse-map a shared tag to one owning group.

## 4. Validation & Error Matrix
| Condition | Required behavior |
|---|---|
| Result write trigger fails | Transaction rollback, unchanged cached fields, no cleanup |
| RPC oversize/duplicate active ID | Reject/break before payload allocation |
| Ordinary saturation | Deterministic busy; reserved control capacity remains bounded |
| Unsupported request cancellation | accepted=false; atomic work continues safely |
| Stop while recovery/start is queued | Old token cannot restore profile |
| Cursor/epoch mismatch or reconnect | Full resync, no stale rows |
| Same ID active and closed | Credit active now; credit final tail later |
| Shared node in two strategy chains | Separate rows and independent A/B interruption |
| Unsupported runtime identity | Preserve independent resources |

## 5. Good / Base / Bad Cases
- Good: commit 1000 test records with zero configuration serializations and one transaction.
- Base: worker returns after a manual stop invalidated its token; it unwinds atomic Start.
- Bad: change the intent before testing the busy start mutex, cancelling a request that already owns it.
- Good: active counter 20 then closed counter 40 credits a total 40, even if both appear in one transition sample.
- Bad: mark the overlapping closed entry accounted while skipping its final counters.

## 6. Tests Required
- Opt-in real Qt/SQLite `NKR_REVIEW_REGRESSION_TESTS=ON`: nine-fix regressions plus narrow result fields/rollback/deleted IDs/concurrent refresh, background admission/owner/exception/wakeup, client reconstruction/accounting and tree persistent IDs/actual chain.
- Normal generation on both Go and C++ sides; `go test ./internal/rpc ./internal/probe ./internal/xray ./internal/xraydns`, focused vet/gofmt.
- Real net.Pipe and QLocalSocket pressure/fragmentation/old-core/reconnect/large legal frames.
- CoreRestartPolicy fake-time tests and actual Process.cpp real-child cancellation/budget/cleanup tests.
- Temporary-DB selector generation and real fork TCP/UDP Dial/Listen plus routed Handler A/B tests.
- Fixed 100/1000/5000 connection workloads; 8×15/30 selector comparisons; repeat measurements, report boundaries. Restore test option OFF and clean unity target before normal GUI build and isolated non-TUN smoke.
- Missing CGO compiler blocks `-race`; ordinary tests are not a race pass. Shared Qt verification is not a static release package.

## 7. Wrong vs Correct
```cpp
// Wrong: whole profile config can overwrite unrelated refresh/traffic changes.
repo.Save(profileWithNewLatency);
// Correct: narrow immutable fields, commit acknowledgment before cleanup.
auto saved = repo.SaveTestResults(records);
if (!saved.success) sweepComplete = false;
```
```cpp
// Wrong: skipped final counters are mistakenly marked accounted.
closedSeen.insert(id); if (activeSeen.contains(id)) continue;
// Correct: overlap remains eligible for the next final closed sample.
if (activeSeen.contains(id)) continue;
closedSeen.insert(id);
```
