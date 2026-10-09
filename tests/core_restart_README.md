# Core automatic recovery (O4)

## Exact policy

`CoreRestartPolicy.hpp` uses monotonic caller-supplied milliseconds. A failure streak
allows **five automatic retries** at **200, 400, 800, 1600 and 3200 ms**. Exponential
arithmetic is capped at 5000 ms (the five-attempt budget reaches 3200 ms first).
The sixth unexpected exit stops recovery and logs its exit code/status/reason.
Unknown failures and unsolicited normal/code-0 exits use this same finite budget.
`QProcess::FailedToStart` and explicit configuration rejection stop recovery; no
log-text classification of configuration errors is used.

Only a successful configuration Start RPC followed by 30,000 ms of uninterrupted
configured process uptime resets a failure streak, evaluated at the next exit.
QProcess started, IPC connection, backoff time, idle process uptime and duplicate
success notifications do not reset it. New explicit user intent resets the policy;
automatic profile restoration must retain the same intent token.

One owned single-shot precise QTimer executes attempts. Generation and desired
profile are packed into one atomic token; cancellation invalidates it synchronously
before the owning-thread timer reset is queued. Intentional Kill/Restart suppress
exit recovery and wait at most 1000/500 ms respectively. A kill timeout refuses a
duplicate launch. No timer callback calls public Restart (which is manual intent).

## Parent integration (UI files intentionally not edited here)

Protect `core_process` lifetime with the existing `coreProcessMutex` when making
cross-thread hook calls. QProcess Start/Kill/Restart still belong on DS_cores.

1. At **manual stop**, call `CancelAutomaticRestart()` synchronously, before the
   `running == nullptr` early return. Do not cancel crash cleanup or the internal
   stop used to replace/reload a configuration. At **prepare_exit**, cancel before
   scheduling teardown. The process code also checks prepare_exit before recovery.
2. For a **new explicit user start**, cancel previous intent once, resolve the ID,
   then capture `token = NoteProfileExpected(id)`. Do not do that cancellation when
   profile_start is invoked by CoreStarted for automatic restoration. Repeated
   NoteProfileExpected(same ID) deliberately preserves the generation and budget.
3. Capture the token **before the configuration Start RPC/build**, not on success.
   Reject stale completion with `IsRestartCurrent(token)` before publishing any UI
   running state. After successful RPC/config publication call
   `NoteProfileRunning(id, token)`; it is thread-safe and marshals policy mutation
   to DS_cores. Never call it from setup_rpc/newConnection alone.
4. For an explicit config-build/validation rejection, call
   `NoteConfigurationRejected(id, token)`. This immediately cancels only that still
   current intent; a stale error cannot cancel a newer profile. Do not classify
   transport failures or free-form core log text as permanent config errors.
5. Replace setup.cpp's direct legacy field consume with
   `ConsumeProfileToRestore()` and direct initial-remember assignment with
   `SetProfileToRestore(id)`. Replace lifecycle.cpp's direct assignment before
   `Restart()` with `SetProfileToRestore(id)`. The atomic legacy field is retained
   for transitional source compatibility; new call sites must use the methods.
6. `Restart()` changes the generation; when the restored profile starts after its
   IPC connection, capture the **new** token (not the pre-Restart one). Carry the
   token into any queued automatic CoreStarted/profile_start action and check it
   immediately before invocation so a later UI stop cannot revive a consumed ID.

Crash handling captures expected profile BEFORE `profile_stop(true, true, false)`;
manual=false preserves SettingsRepo::started_id. Parent must keep that cleanup
classified as non-manual, including its early-return path.

## Isolated tests

`core_restart_policy.cpp` is pure standard C++, uses fake time, checks exact retry
sequence/budget, stable-window boundary, duplicate readiness, no-IPC-reset,
configuration/launch rejection, cancelled/stale/ABA intent and concurrent intent
publication. `core_restart_process.cpp` compiles the production Process.cpp with
small UI/settings adapters under core_restart_stubs; it uses real independent
copies of its own executable as children, never an installed core or live config.
It checks one bounded retry streak, profile preservation, cancellation during cleanup
and across threads, profile switch, stale rejection, shutdown, repeated Kill,
duplicate Start, manual Restart races and failed launch. Stable 30 s is tested only
with fake time, not a real sleep. The real child exits with code 23, not an OS abort.

To build without changing the application's CMakeLists.txt, copy
`tests/core_restart_CMakeLists.txt` to a scratch-source `CMakeLists.txt`, then run:

```powershell
cmake -S <scratch-source> -B <scratch-build> -G Ninja `
  -DNXPROXY_SOURCE=D:/Code/NxProxy -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH=D:/Tools/Qt/6.11.2/msvc2022_64
cmake --build <scratch-build>
# Use an MSVC developer environment and add Qt bin to PATH on Windows.
ctest --test-dir <scratch-build> --output-on-failure --repeat until-fail:3 -V
```

Validated on Windows / MSVC 19.44 / Qt 6.11.2, Release: 69 pure-policy and 48
production-QProcess assertions per run, both tests passed three consecutive runs.
Observed complete real-child streaks: 7446, 7397, 7137 ms including a 400 ms
post-streak observation; all were initial launch + exactly five automatic retries.
No remaining fixture processes. These are timing/safety observations, not throughput
or performance-improvement claims. Full GUI build/review regressions are deferred
until the parent integrates hooks and concurrent tasks settle.