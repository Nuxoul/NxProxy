# Selector panel concurrent refresh regression

Established by task 10-10-selector-panel-refresh on bdab1863. URL-test timeouts do not hide groups: the reproducible bug was a concurrent ID-list read while a background sort replaced that same QList. Persistent selector records remained intact, but one UI rebuild could omit rows until a later rebuild.

Production fix:
- Group::Profiles returns a value snapshot under its mutex (mutable for the const accessor).
- Selector panel and table refresh consume that value, release the group mutex before ProfilesRepo or widget work.
- No selection, node, route, test metric or deletion semantics changed.

## Real application regression
`tests/selector_panel_regressions.hpp` is called by the opt-in real review main before app-lifetime background shutdown. It uses real SQLite repositories/Group::SortProfiles, verifies snapshot waits behind writer lock, checks eight unique selector IDs, member selections and row geometry for 300 concurrent sort/read cycles. It does not create MainWindow or start a core.

Static CI Qt only automatically imports the windows platform plugin. Configure the normal review target ON, run the resulting Throne.exe with QT_QPA_PLATFORM=windows; use three repetitions. Restore OFF and clean/rebuild normal GUI before packaging.

## Exact production Qt panel probe
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/run_selector_panel_probe.ps1
```
Requires PI_SCRATCH_DIR (external temp), VS2022, static Qt6.11.2. Override QtRoot/VcVars/Ninja/CMake if needed. Generates/builds artifacts only in scratch.

The Python generator extracts the CURRENT unmodified refresh_selector_panel function and ElidedLabel from mainwindow_view.cpp. DB/profile/group types are fixture adapters. It starts no core/network, reads no live DB/credentials, and checks all eight distinct IDs/current group/row widget geometry while a writer replaces the ID list with the same lock as production sort. Repeats 300 cycles with resizing/scrolling.

Before fix: locked writer + direct group->profiles loop produced panel=1, expected=8, backing=8 at round0. After snapshot fix: same pressure 300 cycles passed, including eight unique IDs/current selection. First unsynchronized stress probe also failed at round0; only locked-writer evidence is used as the production concurrency comparison. Full app regression initially failed the new lock assertion and passed three times after fix.

The narrow probe tests the exact panel method/Qt rows but not RPC URL testing. Package smoke tests actual GUI/core startup/HTTPS/normal exit; the user's elevated running instance is untouched.
