# Ingest Synthesis Summary

Entry point for `gsd-roadmapper`. Net-new (mode: new) bootstrap of the mzPeak C++
project from 4 classified planning docs.

## Doc counts by type
- SPEC: 2 — roadmap-remaining.md (precedence 0, authoritative phasing spine),
  openms-integration-plan.md (precedence 1, RDR-19 design detail)
- DOC: 2 — reader-backlog.md (precedence 2), e2e-testing.md (precedence 3)
- ADR: 0 · PRD: 0

## Decisions
- Locked: 0 (no ADRs ingested)
- De-facto design decisions extracted (proposed, overridable): 8 — see decisions.md
  - DEC-arch-hybrid-handler, DEC-data-layer-not-xml, DEC-metadata-mirror-mzml,
    DEC-whole-experiment-first, DEC-pr-target-openms-repo, DEC-rdr4-first,
    DEC-defer-blocked-infra, DEC-split-28-and-22

## Requirements extracted: 16 (see requirements.md)
- OPEN (remaining active work): REQ-rdr19-openms-integration, REQ-openms-build,
  REQ-reader-prereqs-10b10c9b, REQ-cpp20-port
- DONE this round / earlier: REQ-rdr10-spectrum-metadata, REQ-rdr24-file-metadata,
  REQ-rdr4-typesystem (4a), REQ-rdr28a-chunked-chromatograms, REQ-rdr25-detail-level,
  REQ-rdr22-buffer-source, REQ-wrt2-run-metadata-emit
- DEFERRED (blocked / no-benefit / multi-month): REQ-wrt1-chunked-emit,
  REQ-rdr20-aes-decryption, REQ-rdr21-cache-rdr23-pageindex, REQ-rdr14-remote-cloud
- Headline remaining work: RDR-19 (wire reader/writer into OpenMS data structures)
  — feasible and scoped, gated on the local libOpenMS build + reader prereqs
  RDR-10b/10c/9b + a small C++23→C++20 port of the reused cores.

## Constraints: 10 (see constraints.md)
- api-contract: 2 (CON-mzpeakfile-api, CON-mzml-cv-dispatch)
- schema: 2 (CON-data-model-mapping, CON-impedance-mismatches)
- nfr: 3 (CON-openms-cpp20, CON-openms-deps-present, CON-rdr4-blast-radius)
- protocol: 3 (CON-semantic-compare, CON-structural-conformance, CON-e2e-matrix)

## Context topics: 9 (see context.md)
Project state · current reader coverage · reader dependency graph ·
adversarial-review workflow · RDR-19 feasibility flip · two-layer template insight ·
validation philosophy · local OpenMS build state · external references.

## Conflicts: 0 blockers, 0 competing-variants, 3 auto-resolved (INFO)
- INFO 1: RDR-19 architecture-location — plan refines roadmap's open question (not
  a contradiction).
- INFO 2: RDR-22 buffer-source status — higher-precedence roadmap (DONE) over the
  earlier DEFERRED plan note.
- INFO 3: RDR-19 prerequisites external/blocked — recorded for routing, not a
  doc-vs-doc conflict.
Detail: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/.planning/INGEST-CONFLICTS.md

## Cycle detection
Cross-ref graph acyclic. Edges among classified docs: roadmap-remaining → reader-
backlog; e2e-testing → reader-backlog. No back-edges. Max depth 2 (≪ 50-cap).

## Per-type intel files
- /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/.planning/intel/decisions.md
- /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/.planning/intel/requirements.md
- /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/.planning/intel/constraints.md
- /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/.planning/intel/context.md

## Routing posture
READY — safe to route. The remaining-work spine is RDR-19 plus its prerequisites;
the bulk of reader/writer items are DONE. Roadmapper should treat roadmap-remaining
phasing (Phase 0–5) as the spine and openms-integration-plan §8 (P0–P5) as the
RDR-19 sub-phasing.
