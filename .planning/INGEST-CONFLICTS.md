## Conflict Detection Report

Mode: new (net-new bootstrap). 4 docs ingested (2 SPEC, 2 DOC). No ADRs, no
LOCKED decisions, no PRDs. Cross-ref graph acyclic (max depth 2, well under the
50-cap). No UNKNOWN/low-confidence classifications. Precedence applied:
roadmap-remaining.md (0) > openms-integration-plan.md (1) > reader-backlog.md (2)
> e2e-testing.md (3).

### BLOCKERS (0)

(none)

### WARNINGS (0)

(none — the docs are complementary, not competing. No requirement is defined with
divergent acceptance criteria across sources; each REQ has a single source of
truth or fully consistent corroboration.)

### INFO (3)

[INFO] Auto-resolved: RDR-19 architecture-location, plan refines roadmap's "open"
  Found: roadmap-remaining.md (Phase 5) records the architecture decision as
    "still open: the adapter likely belongs in the OpenMS tree rather than adding
    a heavy OpenMS build-dep". openms-integration-plan.md (§1, §9) resolves it:
    native OpenMS hybrid handler (option C), PR to OpenMS/OpenMS.
  Note: Not a contradiction — the plan is the RDR-19 design detail that closes the
    roadmap's open question. Higher-precedence roadmap defines the slot; the plan
    fills it. Recorded as DEC-arch-hybrid-handler / DEC-pr-target-openms-repo.

[INFO] Auto-resolved: RDR-22 buffer-source status, roadmap (P0) over plan/backlog
  Found: roadmap-remaining.md (higher precedence) records RDR-22
    `MzPeak::open_buffer(bytes)` as DONE this round (commit 8f030a5, ZipBuffer over
    zip_source_buffer; "the reviewers' large worry didn't materialize").
    openms-integration-plan.md is silent; reader-backlog.md (lower precedence) and
    the roadmap's own earlier Phase-3 note call buffer-backed *zip* (RDR-22b)
    large/DEFERRED.
  Note: Higher-precedence roadmap "Implemented this round" section wins — buffer
    source landed. The DEFERRED note describes the pre-implementation plan state.
    Synthesized requirement marks RDR-22 DONE with the divergence flagged.

[INFO] Auto-resolved: RDR-19 prerequisites are external/blocked, not in-set conflicts
  Found: RDR-19 depends on REQ-openms-build (libOpenMS not built locally),
    REQ-reader-prereqs-10b10c9b (RDR-10b/10c/9b not yet landed), and REQ-cpp20-port.
    These are unmet prerequisites, not contradictions between docs.
  Note: Surfaced for routing visibility — RDR-19 cannot start until the OpenMS
    build chore (P0) completes and the reader prereqs land. No gate; recorded in
    requirements.md depends_on chains for the roadmapper.

GSD > No conflicts detected. (0 blockers, 0 warnings — safe to proceed; the 3 INFO
entries are recorded for transparency.)
