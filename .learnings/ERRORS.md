# Errors

Command failures and integration errors.

---

## [ERR-20260924-001] edit-protocol

**Logged**: 2026-09-24T00:00:00Z
**Priority**: medium
**Status**: pending
**Area**: config

### Summary
Two early Edit calls used unified-diff-style context rows, which the line-edit protocol rejects.

### Error
```text
unknown operation ***; expected PUT, CUT, REM, or MV
```

### Context
- Operation attempted: replace a bounded section of GroupUpdater.cpp.
- The file was re-read after the failure and no source changes from those failed calls were retained.

### Suggested Fix
Use only PUT/CUT operation headers and re-ground on the returned tag after every successful write.

### Metadata
- Reproducible: yes
- Related Files: src/configs/sub/GroupUpdater.cpp

---

## [ERR-20260924-002] trellis-context

**Logged**: 2026-09-24T00:00:00Z
**Priority**: medium
**Status**: pending
**Area**: config

### Summary
The repository's requested Trellis context script is missing.

### Error
```text
get_context.py: No such file or directory
```

### Context
- Commands attempted: py -3 ./.trellis/scripts/get_context.py and --mode phase.
- Work continued from git status, existing task summary, and source inspection.

### Suggested Fix
Restore the missing script in the project fork or use the documented fallback workflow when continuing this task.

### Metadata
- Reproducible: yes
- Related Files: .trellis/scripts/get_context.py

---

## [ERR-20260924-003] clean-build-environment

**Logged**: 2026-09-24T00:00:00Z
**Priority**: high
**Status**: resolved
**Area**: infra

### Summary
Clean Windows build initially could not find the MSVC standard header `string_view`.

### Error
```text
fatal error C1083: cannot open include file: 'string_view': No such file or directory
```

### Context
- Command: ninja -C build-local -t clean; ninja -C build-local -j 4
- The generated command used MSVC cl.exe but the shell did not have the Visual Studio x64 include environment loaded.

### Suggested Fix
Run Ninja from a Visual Studio x64 Native Tools environment or invoke VsDevCmd.bat before the build.

### Metadata
- Reproducible: yes
- Related Files: build-local/build.ninja

### Resolution
- **Resolved**: 2026-09-27T13:24:03Z
- **Notes**: VsDevCmd.bat -arch=x64 loaded the MSVC environment and the full build linked successfully.

---

## [ERR-20260927-004] filter-edit-anchor

**Logged**: 2026-09-27T13:30:00Z
**Priority**: medium
**Status**: resolved
**Area**: frontend

### Summary
A line anchor drift placed the allowed-profile condition inside `portMatches()` during the final filter change.

### Error
```text
error C2065: 'profileId': undeclared identifier
error C2065: 'maxOk': undeclared identifier
```

### Context
- The clean build caught the issue in src/ui/utils/ProfilesFilterProxyModel.cpp.
- The live file was reread and the condition/maxOk line were restored to their correct functions.

### Suggested Fix
After every line-range edit, reread the affected function before compiling.

### Metadata
- Reproducible: yes
- Related Files: src/ui/utils/ProfilesFilterProxyModel.cpp

---
---
## [ERR-20260928-001] runtime-inspection-tools

**Logged**: 2026-09-28T00:00:00Z
**Priority**: low
**Status**: pending
**Area**: infra

### Summary
The `python` and `sqlite3` commands were unavailable in the PowerShell environment.

### Error
```text
python : The term 'python' is not recognized as the name of a cmdlet, function, or script file.
```

### Context
- The database inspection helper was invoked from the NxProxy repository root.
- PowerShell exposed only the `py.exe` launcher, so inspection can continue through `py -3`.

### Suggested Fix
Use the Windows Python launcher when available, or provide an alternate SQLite inspection path.

### Metadata
- Reproducible: yes
- Related Files: inspect_nxproxy_db.py

---
## [ERR-20260928-002] stale-process-permissions

**Logged**: 2026-09-28T00:00:00Z
**Priority**: medium
**Status**: pending
**Area**: infra

### Summary
The previously launched `Throne` and `ThroneCore` processes could not be stopped from the current shell because they run with higher privileges.

### Error
```text
Access is denied
```

### Context
- Process IDs: 5096 (`Throne.exe`) and 5544 (`ThroneCore.exe`).
- Runtime validation will use a separate executable directory and explicit `-appdata` directory instead of disturbing those processes.

### Suggested Fix
Stop elevated test processes from an elevated shell before rebuilding or reusing their runtime directory.

### Metadata
- Reproducible: yes
- Related Files: build-local/Throne.exe

---
---
## [ERR-20260928-003] isolated-runtime-e2e

**Logged**: 2026-09-28T00:00:00Z
**Priority**: medium
**Status**: resolved
**Area**: infra

### Summary
Isolated GUI/runtime probes repeatedly exceeded the shell timeout or connected to the wrong proxy instance.

### Error
```text
TOOL_TIMEOUT
curl: (7) Failed to connect to 127.0.0.1 port 21880
```

### Context
- The existing elevated `Throne`/`ThroneCore` pair owns the normal `2080` inbound and cannot be stopped from the current shell.
- Long-lived UI automation commands were also vulnerable to the desktop command host timeout.
- The workaround was to use fresh `-appdata` directories, unique ports, short-lived scripts, and exported configuration inspection.

### Suggested Fix
Run GUI probes in one bounded script, use a unique inbound port, and do not infer isolated traffic from a port owned by another elevated instance.

### Metadata
- Reproducible: yes
- Related Files: build-local/Throne.exe, build-local/config/logs/throne.log

### Resolution
- **Resolved**: 2026-09-28T00:00:00Z
- **Notes**: The latest build started successfully and the exported sing-box configuration verified `selector-57` plus its SG default member and AI rule mappings. A live request through the isolated mixed inbound remains blocked by the stale elevated runtime/port environment.

---
---
## [ERR-20260928-004] unity-build-stale-cpp

**Logged**: 2026-09-28T00:00:00Z
**Priority**: critical
**Status**: pending
**Area**: infra

### Summary
This project builds with CMake unity sources, so editing a single `.cpp` does **not** trigger a recompile.

### Error
```text
ninja: no work to do.        # after editing src/ui/mainWindow/mainwindow_setup.cpp
```

### Context
- `build-local/build.ninja` contains no reference to any individual `.cpp` (grep for `mainwindow_setup.cpp` = 0 hits); only `CMakeFiles/Throne.dir/Unity/unity_N_cxx.cxx` files are compiled, and they are inputs to nothing that tracks the `.cpp` files.
- A `.cpp`-only edit therefore stays invisible until a header that the same unity TU includes changes, or the target is cleaned.
- Consequence: a build can "succeed" while silently shipping old code, and a brace error in such an edit is not reported until a forced rebuild.
- Fix used here: `ninja -C build-local -t clean Throne && ninja -C build-local -j 4` (64/64, ~2 min).

### Suggested Fix
After any `.cpp`-only change, force the target: `ninja -C build-local -t clean Throne` before building, or verify the exe timestamp/strings changed. Never trust `ninja: no work to do` as proof that a `.cpp` edit is in the binary.

### Metadata
- Reproducible: yes
- Related Files: src/ui/mainWindow/mainwindow_setup.cpp, build-local/build.ninja

---
## [ERR-20260928-005] stale-line-range-clobber

**Priority**: high
**Status**: fixed
**Area**: edit-protocol

### Summary
Editing the same region twice with a line range that was read before the first edit deleted the
tail of my own replacement and left a stray `.arg(...)` line behind, breaking the build.

### Error
```text
src/stats/traffic/TrafficLooper.cpp(183): error C2065: "g": undeclared identifier
```
The reported line was ~60 lines below the damage: the stray line ended the statement early, so
everything that followed fell out of scope.

### Context
- First edit replaced line 120 (1 line) with a 5-line block, so lines 121+ shifted down by 4.
- Second edit reused the *old* range 120..124, which then covered only the first 5 lines of that
  block and left its 5th line orphaned in the file.
- The damage was committed and pushed before the first build, because the push was requested
  before verification.

### Suggested Fix
After any edit that changes a region's line count, re-read the region (or use the tag from the
successful edit) before touching it again. Build before pushing: a push is not a checkpoint if the
tree does not compile.

### Metadata
- Reproducible: yes
- Related Files: src/stats/traffic/TrafficLooper.cpp

---

## [ERR-20260928-006] persisted-column-widths-override-defaults

**Priority**: medium
**Status**: fixed
**Area**: ui

### Summary
New default table column widths never reached existing databases, so the 267px dead strip the
change was meant to remove was still on screen.

### Error
```text
pixel scan: header sections end at x=791, blank to x=1058 (267px) after the change built clean
```

### Context
- `MainWindow::refresh_proxy_list_column_size()` applies the new stretch/fixed modes only when
  `group->column_width` is empty. An older release had already persisted 129/174/151/289 for this
  group, so the `else` branch restored exactly the old layout on every refresh.
- A code change can therefore be present, built, and still invisible on a real profile.

### Suggested Fix
Keep respecting a stored layout, but close the gap it leaves: sum the sections and hand the
remainder to the node name column. Verify column changes against a database that has history, not
only a fresh one.

### Metadata
- Reproducible: yes
- Related Files: src/ui/mainWindow/mainwindow_view.cpp, .learnings/ui-modernize-2.md

---
