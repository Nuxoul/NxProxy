# Narrow test-result persistence (O1)

`ProfilesRepo::SaveTestResults(const std::vector<TestResult>&)` consumes immutable value snapshots, not mutable `Profile` pointers. It returns `{success, appliedIDs}` synchronously; IDs are unique and published only after commit. Empty batches succeed. Invalid IDs, missing/deleted IDs and records with no eligible fields are skipped without inserting anything.

| Kind | Eligible columns |
|---|---|
| URL / health | latency, latency_at (paired, explicit Unix-second measurement time) |
| IP | ip_out, test_country |
| Speed / country | explicitly present dl_speed, ul_speed, test_country; explicit latency + latency_at |

Absent fields preserve previous values; present empty strings clear only that field. Kind filtering also applies to the identity-map update. No path serializes or updates name/type/gid/outbound_json/traffic/created_at/updated_at. Each nonempty batch uses one SQLite transaction with reusable prepared statements, on a dedicated connection so asynchronous traffic writes cannot enter the result transaction. The repo mutex orders configuration saves and committed cache publication. A statement/commit failure rolls back all results, leaves cached fields untouched, reports via NotifyError and returns false with no IDs.

URL cleanup uses only explicit failed IDs present in a committed batch. Save failure marks the whole sweep incomplete and prohibits earlier failures from authorizing cleanup. Existing cancellation, generation, VPN connect-only and per-group guards remain. URL polling is advisory only. IP and speed/country polls use narrow batches; queued speed ticks check their generation and final speed publication invalidates older ticks. Traffic accounting keeps its separate SaveTraffic ownership. Download-only/upload-only/country-only tests do not clear other speed fields; speed errors are not URL failure verdicts.

## Validation

- `tests/review_regressions.cpp::testNarrowTestResults` exercises real application repos (opt-in existing CTest target).
- `tests/run_test_results_sqlite.ps1 -Scratch <absolute-build-directory>` compiles exact unchanged production method bodies plus the vendored SQLite/SQLiteCpp against a small profile/application shim. It runs field independence, committed cache, rollback triggers, missing IDs and competing SQLite subscription updates. It does **not** build the GUI.
- The same executable measures real SQLite `Save` vs `SaveTestResults`, seed `0x01337`, 100/1000 synthetic profiles with 2 KiB configuration payloads, three repetitions, WAL/NORMAL. Each measured path starts from identical latency fields. SQLite commit hooks count transactions; the outbound shim counts actual calls to ExportToJson. Output: scratch `o1_benchmark.csv`. Shims limit these timings to the persistence boundary; they are not end-to-end GUI/network performance claims.
