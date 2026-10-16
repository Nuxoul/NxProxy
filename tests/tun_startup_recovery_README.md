# Windows TUN startup recovery

Base bcc2477e; task10-16-tun-startup-recovery. This is conditional software recovery, not a fixed autorun delay or adapter/driver deletion.

## Protocol and policy
`ErrorResp` adds optional `tun_adapter_retryable=2` default false. Windows Start only sets it after attempt resources are reclaimed, for the typed error pair under the TUN configure stage: named create adapter Win32 183/80 and named open existing adapter1168. Localized text alone never triggers it; permissions/config/other resource errors do not retry. Non-Windowsfalse. Legacy core field absence is false; legacy GUI ignores it.

GUI uses the same startup token, max3 total Start attempts, 1s/2s waits,60s client-call/wait budget, perCall min30s/remaining. Normal startup one call/no extra sleep. Failed box cleanup is already performed by Box.Start and sidecars by RPC; no double-close or driver reinstall. Fresh instances are created in the same core process. Failed attempts do not publish live UI/traffic/started_id. Intermediate recoverable errors log only; final failure uses existing dialog. Connecting button can cancel; stop/exit/token replacement prevents another attempt. An active atomic Start completes, with stale success unwound rather than published.

Start transport/decoding failure means unknown completion, not permission to retry. Each attempt captures core LaunchGeneration; owner-thread ResetUnconfirmedStart invalidates intent/restore handoff, resets only that exact process launch and starts an idle core only if no stop/exit intervened. No configuration restore. Older callbacks never kill a newer launch. The existing long-running RestartPrompt waits61s, beyond recovery budget.

## Reproducible checks
```powershell
# Pure production recovery module, no Qt/core/live settings:
powershell -NoProfile -ExecutionPolicy Bypass -File tests/run_tun_startup_recovery.ps1
# Typed Win32 error branches, permission/localized/text negatives, real sidecar cleanup + next real box:
Set-Location core
go test ./internal/rpc -run '^TestTunStartup' -count=5 -v
go test ./internal/rpc ./internal/probe ./internal/xray ./internal/xraydns
go vet ./internal/rpc ./internal/probe
```
`tests/tun_recovery_cases.hpp` and `tun_recovery_qt.hpp` are also called by the real opt-in review entry. The Qt case clicks the actual Connecting StartStopButton during production Wait; no second Start may happen. Static Qt uses QT_QPA_PLATFORM=windows; run3 times, restore NKR_REVIEW_REGRESSION_TESTS=OFF and unity-clean rebuild normal GUI.

`tests/run_rpc_transport.ps1 -Repeat 3` covers the new field and recover-on-second Start with real QLocalSocket, missing-field fallback, plus prior transport tests. Pure module uses fake monotonic time; real Qt wait is measured. None creates real TUN devices.

`tests/core_restart_CMakeLists.txt` standalone harness compiles production Process.cpp against small UI/settings adapters. It uses its own auxiliary children, not ThroneCore. Check wrong/current/old launch reset, no restored profile and Stop during reset teardown. See core_restart_README.md for build layout.

## Measured validation and limits
- New core response test red before flag implementation, green after; target/negative matrix repeated5.
- GUI recover-on-second test red before loop, green after. Real Connecting-button test red before enabling cancel, green after; stop observed around50–53ms.
- Existing69 restart-policy plus updated60 production-QProcess assertions pass; all fixture children reaped.
- Full Qt/SQLite review suite3 times and real QLocalSocket3 times pass; static GUI/tagged core build and isolated seven-file package HTTPS/normal-exit smoke pass.
- No real cold boot or administrator TUN adapter conflict was induced. The exact OS trigger is not fully proven because the first failure log was overwritten. Next actual boot is required to validate this user's symptom; do not call fault injection a real cold-boot pass.
- Start/Stop keep atomic transport semantics. Race detector unavailable without CGO compiler. No change to Scheduled Tasks, driver installation, adapter names, routing or user live database/processes.
