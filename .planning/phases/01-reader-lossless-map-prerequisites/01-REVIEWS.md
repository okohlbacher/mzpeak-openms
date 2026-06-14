# Phase 1 — Cross-AI Plan Reviews

**Reviewed:** 2026-06-14 · Plans 01-01/02/03 · Reviewers: Gemini, Codex (Claude skipped — self)

## Synthesis (concerns to resolve, by severity)

**HIGH (must resolve before execution — Codex):**
- **H1 · Chunked-Arrow join correctness.** The metadata join uses a row-local index `r`;
  if helper row indexes are applied to the wrong chunk/array (global-row assumptions
  leaking into a chunked column), precursor/selected_ion/scan metadata is silently
  misassigned. **Fix:** join via an explicit `source_index -> vector<PrecursorInfo>`
  key map (not positional row alignment); add tests over MULTIPLE rows (ideally all 34
  MS2 spectra) asserting chunk-local indexing, not a single happy-path spectrum.
- **H2 · Selected-ion attachment via `precursors.back()` is not DIA-safe.** The plan
  claims `vector<PrecursorInfo>` DIA-readiness but attaches selected ions to the *last*
  precursor and assumes one precursor per MS2 with ordered rows. **Fix:** attach by
  explicit `(source_index, precursor_index)`, not `back()`.

**MEDIUM (address):**
- **M1 · RDR-9b raw-byte value decode is an unvalidated path.** Bundled fixtures have
  zero populated aux arrays, yet the plan implements `float32/float64/int32` byte
  decoding — contradicting CONTEXT.md's "do not write unvalidated decode paths."
  **Fix:** implement schema parsing + empty-list handling now; put the raw-byte VALUE
  decode behind a fixture-gated follow-up (keep the byte-length guard if any decode ships).
- **M2 · CvParam value stringification underspecified.** `std::to_string(double)` yields
  `"35.000000"` while a fixture value is `35` — fragile string asserts. **Fix:** define a
  canonical stringification (or compare numerically after parse) for the value union.
- **M3 · Scan fields (`scan_parameters`/`scan_windows`) add risk to RDR-10c** (Codex +
  the plan-checker both flagged). Already justified+recorded (Q3 RESOLVED), but keep
  RDR-10c's precursor work the focus; make scan fields an explicitly-accepted add-on.

**LOW (note):**
- Gemini: log/handle `source_index` pointing to a non-existent spectrum (no silent loss).
- Gemini/Codex: document the empty-`values`-on-corrupt API contract so Phase 2 doesn't
  read an empty vector as "successfully decoded".
- Codex: `data_processing_ref` as `std::string` loses null-vs-empty (null in all fixtures
  today; revisit if Phase 2 needs the distinction).
- Codex: add a `number_of_auxiliary_arrays == auxiliary_arrays.size()` consistency assert.
- Gemini: factor the one-CvParam helper early (01-01) to avoid duplication.

## Overall risk
- **Gemini: LOW** — technically sound, respects boundaries, robust validation.
- **Codex: MED** — directionally strong; nested-Arrow chunking, selected-ion association,
  and unvalidated aux decode are real implementation risks. Pyarrow ground truth is sound
  *provided tests assert enough rows and relationships, not one happy-path spectrum.*

Both agree pyarrow ground-truth is the right Phase-1 authority and the OpenMS-scope
boundary is respected. The decisive feedback is **H1/H2 (join correctness + DIA-safety)**
and **M1 (don't ship unvalidated decode)** — incorporate via `/gsd:plan-phase 1 --reviews`.

---

## Gemini (full)

(Risk LOW.) Strengths: avoids redundant Parquet I/O via in-memory joins; reuses `CvParam`
+ `opt_*` idioms; `vector<PrecursorInfo>` DIA-ready; pyarrow ground-truth oracle is the
most reliable Parquet-decoder check. Concerns (all LOW): join-safety logging for absent
`source_index`; risk of permanent IM/aux stubs; document empty-on-corrupt contract.
Suggestions: factor the one-CvParam helper early; assert a non-empty `scan_parameters`.

## Codex (full)

(Risk MED.) Strengths: clear 01-01→02→03 dependency ordering; no OpenMS leakage; pyarrow
authority + explicit Rust deferral; RDR-10c adapts to research (precursor where fixtures
exist, IM deferred); good nullability posture. Concerns: **HIGH** chunked-Arrow join
misassignment; **HIGH** `precursors.back()` selected-ion attachment not DIA-safe; **MED**
over-scoped unvalidated aux decode; **MED** fragile `to_string(double)` value formatting;
**MED** scan-field scope creep; LOW null-vs-empty `data_processing_ref`, float-only aux
values. Suggestions: explicit `source_index` key-map join + `(source_index, precursor_index)`
selected-ion attach; test all 34 MS2 spectra; schema-only aux now; deterministic value
stringification; `number_of_auxiliary_arrays == size()` assert.
