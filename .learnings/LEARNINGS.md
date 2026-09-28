# Learnings

Corrections, insights, and knowledge gaps captured during development.

**Categories**: correction | insight | knowledge_gap | best_practice

---

## [LRN-20260924-001] correction

**Logged**: 2026-09-24T00:00:00Z
**Priority**: high
**Status**: pending
**Area**: backend

### Summary
Subscription rule import must be wired after profile reconciliation and after selector creation.

### Details
The parser can emit nodes, proxy groups, and rules in one document, but route mapping depends on the final persisted group profile IDs. Calling rule application before reconciliation or before `applyProxyGroups()` either misses selectors or uses stale profile IDs.

### Suggested Action
Keep the import order explicit: parse, flush node inserts, reconcile subscription nodes, apply proxy groups, then apply proxy rules.

### Metadata
- Source: error
- Related Files: src/configs/sub/GroupUpdater.cpp
- Tags: subscription, selector, routing

---
