#!/usr/bin/env python3
"""Trim a grid-encoded timsTOF archive into the bundled `grid.dir` fixture.

Coordinate grid encoding (`MS:1003826`) stores no coordinate values at all: the
chunk's `mz_chunk_values` list is null and the coordinates are integer INDICES
into a model whose parameters ride in the same row, in a struct column
`<array>_grid { grid_type, parameters, indices }`.  Nothing the reference
implementation ships emits it -- `mzpeak_prototyping`'s own converter wrote
delta-encoded chunks when asked for a grid -- so the fixture comes from
mzPeakConverter, where the grid layout is the default for Bruker TDF since
0.13.0:

    mzpeak-convert <diaPASEF.d> -o /tmp/grid.mzpeak
    python3 test/files/make_grid_fixture.py /tmp/grid.mzpeak test/files/grid.dir

Two MS2 frames are kept; the MS1 frame is 205,921 points and adds no coverage
the MS2 frames do not already give.  Both grid models appear in it: the m/z axis
uses `MS:9999002` (Bruker timsTOF m/z, 7 parameters, DELTA-coded indices
because it is the main axis) and the ion mobility axis `MS:9999001` (TIMS 1/K0,
4 parameters, absolute indices because it is secondary).  The two placeholder
accessions are what the reference uses until PSI assigns terms.

The `chunk_start`/`chunk_end` bounds are left exactly as written: they are the
writer's own evaluation of the same models at each chunk's first and last index,
so they are the oracle this reader checks its decode against on every row.

Four variants follow, each differing from grid.dir in exactly one respect:

  grid_bad_bounds.dir    chunk_start[0] off by 1 Th           -> refused
  grid_bad_end.dir       chunk_end[0] off by 1 Th             -> refused
  grid_legacy.dir        pre-fix writer: mobility pair swapped,
                         chunk_end 0.0 on each last chunk     -> same values as grid.dir
  grid_bad_mobility.dir  1/K0 unphysical in either reading    -> refused
"""

import hashlib
import json
import math
import shutil
import sys
import zipfile
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

KEEP = [1, 2]
RENUMBER = {old: new for new, old in enumerate(KEEP)}


def retally(kv, spectra):
    out = dict(kv or {})
    if b"spectrum_count" in out:
        out[b"spectrum_count"] = str(spectra).encode()
    return out


def filter_chunks(path: Path, keep: set[int]) -> None:
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    kv = parquet.metadata.metadata
    st = table.column(0).combine_chunks()
    names = [f.name for f in st.type]

    index = st.field("spectrum_index").to_pylist()
    rows = pa.array([i for i, v in enumerate(index) if v in keep], pa.int64())

    fields = []
    for n in names:
        col = st.field(n).take(rows)
        if n == "spectrum_index":
            col = pa.array([RENUMBER[v] for v in col.to_pylist()], col.type)
        fields.append(col)

    out = pa.StructArray.from_arrays(fields, names=names)
    schema = pa.schema([pa.field(table.schema.names[0], out.type)]).with_metadata(
        retally(kv, len(keep))
    )
    pq.write_table(pa.Table.from_arrays([out], schema=schema), path,
                   write_statistics=True)


def filter_rows(path: Path, column: str, keep: set[int]) -> None:
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    kv = parquet.metadata.metadata
    if column not in table.schema.names:
        return
    values = table.column(column).to_pylist()
    rows = pa.array([i for i, v in enumerate(values) if v in keep], pa.int64())
    trimmed = table.take(rows)
    renum = pa.array([RENUMBER[v] for v in trimmed.column(column).to_pylist()],
                     trimmed.schema.field(column).type)
    trimmed = trimmed.set_column(trimmed.schema.get_field_index(column), column, renum)
    pq.write_table(trimmed.replace_schema_metadata(retally(kv, len(keep))), path,
                   write_statistics=True)


# ---------------------------------------------------------------------------
# Variants of grid.dir, each differing in ONE respect, so a failing test names
# the property that broke rather than a fixture that differs in several.
# ---------------------------------------------------------------------------

def _rows(struct_array):
    return struct_array.to_pylist()


def bad_start(cols, si):
    """chunk_start[0] moved by 1 Th: far outside rounding, inside the plausible
    range.  A reader that never compares the decode to the recorded bound reads
    this as good data."""
    starts = cols["mz_chunk_start"].to_pylist()
    starts[0] += 1.0
    cols["mz_chunk_start"] = pa.array(starts, pa.float64())
    return "mz_chunk_start[0] += 1.0"


def bad_end(cols, si):
    """chunk_end[0] moved by 1 Th.  Guards the zero-end relaxation: tolerating a
    ZERO end must not tolerate a WRONG one."""
    ends = cols["mz_chunk_end"].to_pylist()
    ends[0] += 1.0
    cols["mz_chunk_end"] = pa.array(ends, pa.float64())
    return "mz_chunk_end[0] += 1.0"


def legacy(cols, si):
    """The two defects of archives written before the reference's fixes, as
    they occur together in the wild:

    - the TIMS mobility parameter pair in the pre-eb08ba0 order
      [c6, c7, slope, offset] instead of [c6, c7, offset, slope];
    - chunk_end = 0.0 on the last chunk of each spectrum, the trace of the
      single-iterator bug in the writer's chunk_series.rs.

    Nothing in the archive distinguishes the old spelling, which is the point:
    it must decode to exactly what grid.dir decodes to."""
    field = cols["mean_inverse_reduced_ion_mobility_grid"]
    rows = _rows(field)
    for r in rows:
        if r is None:
            continue
        p = r["parameters"]
        p[2], p[3] = p[3], p[2]
    cols["mean_inverse_reduced_ion_mobility_grid"] = pa.array(rows, field.type)

    # Only ONE-POINT chunks: the writer bug read start and end from a single
    # iterator, so only a chunk whose one value was consumed for the start lost
    # its end.  Zeroing a longer chunk would model a different, unreal defect --
    # an earlier version of this generator did exactly that, and so demanded a
    # reader tolerant enough to accept a genuinely contradictory bound.
    ends = cols["mz_chunk_end"].to_pylist()
    points = [len(r["indices"]) if r else 0 for r in _rows(cols["mz_grid"])]
    single = [i for i, n in enumerate(points) if n == 1]
    for i in single:
        ends[i] = 0.0
    cols["mz_chunk_end"] = pa.array(ends, pa.float64())
    return f"mobility pair swapped on every row; chunk_end = 0.0 on one-point rows {single}"


def zero_end_multi(cols, si):
    """chunk_end = 0.0 on a chunk of SEVERAL points.  That is not the writer bug,
    whose trace is confined to one-point chunks, so it is a genuine contradiction
    -- start far above end -- and must be refused rather than excused."""
    points = [len(r["indices"]) if r else 0 for r in _rows(cols["mz_grid"])]
    row = next(i for i, n in enumerate(points) if n > 1)
    ends = cols["mz_chunk_end"].to_pylist()
    ends[row] = 0.0
    cols["mz_chunk_end"] = pa.array(ends, pa.float64())
    return f"chunk_end = 0.0 on row {row}, which has {points[row]} points"


def bad_mobility(cols, si):
    """c6 scaled by 1000, so 1/K0 comes out near 0.05 Vs/cm^2 whichever way the
    offset/slope pair is read.  The magnitude disambiguation cannot rescue it,
    so the physical-range check must refuse it rather than return it."""
    field = cols["mean_inverse_reduced_ion_mobility_grid"]
    rows = _rows(field)
    for r in rows:
        if r is not None:
            r["parameters"][0] *= 1000.0
    cols["mean_inverse_reduced_ion_mobility_grid"] = pa.array(rows, field.type)
    return "mobility c6 *= 1000"


# ---------------------------------------------------------------------------
# Mixed dimensions.  The specification tells a writer to fall back per chunk
# when a grid model's error is too large, so a dimension may hold grid rows
# beside plain ones.  These variants rewrite some of grid.dir's rows into plain
# encodings carrying the SAME values, so a correct reader decodes them to what
# grid.dir decodes to.  The values are evaluated here with the reference
# `grid.rs` arithmetic, independently of the reader under test.
# ---------------------------------------------------------------------------

def timstof_mz(p, i):
    """grid.rs `timstof_mz`, operation for operation (math.fma is exact)."""
    c0, beta, c2, c3, c4, timebase, delay = p
    tof = math.fma(i, timebase, delay)
    s0 = (tof - c0) / beta
    refined = s0
    if c3 != 0.0:
        s = s0
        c2_2, c3_3 = c2 * 2.0, c3 * 3.0
        for _ in range(8):
            s2 = s * s
            f = math.fma(s, beta, c0) + s2 * c2 + math.fma(s2 * s, c3, -tof)
            deriv = math.fma(s, c2_2, beta) + s2 * c3_3
            if deriv == 0.0:
                break
            delta = f / deriv
            s -= delta
            if abs(delta) < 1e-12:
                break
        refined = s
    elif c2 != 0.0:
        d = beta * beta - 4.0 * c2 * (c0 - tof)
        refined = s0 if d < 0.0 else (c0 - tof) / (-0.5 * (beta + math.sqrt(d)))
    return refined * refined - c4


def timstof_mobility(p, i):
    c6, c7, offset, slope = p
    return 1.0 / (c6 + c7 / (offset + slope * i))


def mixed_main(cols, si):
    """m/z rows cycle grid / uncompressed (MS:1000576) / delta (MS:1003089).

    Uncompressed stores the values after the start, which the reader prepends;
    delta stores successive differences.  Both carry the values the grid row
    held, so the whole dimension must decode to what grid.dir decodes to --
    exactly for the uncompressed rows, to rounding for the delta ones."""
    grid = _rows(cols["mz_grid"])
    enc = cols["chunk_encoding"].to_pylist()
    values = cols["mz_chunk_values"].to_pylist()
    kinds = []
    for r, g in enumerate(grid):
        kind = ("grid", "none", "delta")[r % 3]
        kinds.append(kind)
        if kind == "grid" or g is None:
            continue
        acc, coords = 0, []
        for d in g["indices"]:
            acc += d
            coords.append(timstof_mz(g["parameters"], float(acc)))
        if kind == "none":
            enc[r] = "MS:1000576"
            values[r] = coords[1:]
        else:
            enc[r] = "MS:1003089"
            values[r] = [b - a for a, b in zip(coords, coords[1:])]
        grid[r] = None
    cols["mz_grid"] = pa.array(grid, cols["mz_grid"].type)
    cols["chunk_encoding"] = pa.array(enc, cols["chunk_encoding"].type)
    cols["mz_chunk_values"] = pa.array(values, cols["mz_chunk_values"].type)
    n = {k: kinds.count(k) for k in ("grid", "none", "delta")}
    return f"m/z rows grid/uncompressed/delta = {n['grid']}/{n['none']}/{n['delta']}"


MOBILITY = "mean_inverse_reduced_ion_mobility"


def mixed_secondary(cols, si):
    """Ion mobility carried as a plain `chunk_secondary` list on odd rows and as
    a grid on even ones, both registered as the same array.  This is the shape
    that used to lose the grid rows SILENTLY: the dimension's values entry named
    the plain column and the grid was never consulted."""
    grid = _rows(cols[MOBILITY + "_grid"])
    plain = []
    for r, g in enumerate(grid):
        if r % 2 == 1 and g is not None:
            plain.append([timstof_mobility(g["parameters"], float(k)) for k in g["indices"]])
            grid[r] = None
        else:
            plain.append(None)
    cols[MOBILITY + "_grid"] = pa.array(grid, cols[MOBILITY + "_grid"].type)
    cols[MOBILITY] = pa.array(plain, pa.large_list(pa.float64()))
    return "mobility plain on odd rows, grid on even rows"


def add_secondary_entry(entries):
    grid = next(e for e in entries if e["path"].endswith(MOBILITY + "_grid"))
    plain = dict(grid, path="chunk." + MOBILITY, buffer_format="chunk_secondary",
                 transform=None)
    return entries + [plain]


def no_end(cols, si):
    """No `chunk_end` column at all, and no array-index entry for it.  Nothing
    requires one, and a reader that indexed the end bounds by the START column's
    length read past the end of an empty vector -- undefined behaviour, found by
    review, never by a test."""
    del cols["mz_chunk_end"]
    return "mz_chunk_end column and array-index entry removed"


def drop_end_entry(entries):
    return [e for e in entries if not e["path"].endswith("mz_chunk_end")]


def short_mobility(cols, si):
    """One chunk's mobility grid is null while its m/z and intensity are not, so
    the mobility array comes up short by that chunk's points and every mobility
    after it is paired with the wrong peak -- silently, unless the reader checks
    that the parallel arrays really are parallel."""
    grid = _rows(cols[MOBILITY + "_grid"])
    grid[1] = None
    cols[MOBILITY + "_grid"] = pa.array(grid, cols[MOBILITY + "_grid"].type)
    return "mobility grid null on row 1"


def write_variant(source: Path, name: str, mutate, edit_index=None) -> None:
    out_dir = source.parent / name
    shutil.rmtree(out_dir, ignore_errors=True)
    shutil.copytree(source, out_dir)

    peaks = out_dir / "spectra_peaks.parquet"
    parquet = pq.ParquetFile(peaks)
    table = parquet.read()
    kv = parquet.metadata.metadata
    st = table.column(0).combine_chunks()
    names = [f.name for f in st.type]
    cols = {n: st.field(n) for n in names}
    what = mutate(cols, st.field("spectrum_index").to_pylist())

    order = [n for n in names if n in cols] + [n for n in cols if n not in names]
    out = pa.StructArray.from_arrays([cols[n] for n in order], names=order)
    if edit_index is not None:
        kv = dict(kv)
        index = json.loads(kv[b"spectrum_array_index"])
        index["entries"] = edit_index(index["entries"])
        kv[b"spectrum_array_index"] = json.dumps(index).encode()
    schema = pa.schema([pa.field(table.schema.names[0], out.type)]).with_metadata(kv)
    pq.write_table(pa.Table.from_arrays([out], schema=schema), peaks,
                   write_statistics=True)

    # Digests recomputed so each variant fails on the property it perturbs and
    # not on a stale checksum.
    index_path = out_dir / "mzpeak_index.json"
    index = json.loads(index_path.read_text())
    for entry in index["files"]:
        member = entry.get("path") or entry.get("name")
        if (out_dir / member).exists():
            entry["checksum"] = hashlib.sha512((out_dir / member).read_bytes()).hexdigest()
    index_path.write_text(json.dumps(index, indent=2))
    print(f"wrote {out_dir} ({what})")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    source, dest = Path(sys.argv[1]), Path(sys.argv[2])
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    with zipfile.ZipFile(source) as archive:
        archive.extractall(dest)

    keep = set(KEEP)
    for name in ("spectra_peaks.parquet", "spectra_data.parquet"):
        if (dest / name).exists():
            filter_chunks(dest / name, keep)
    filter_rows(dest / "spectra_metadata.parquet", "index", keep)
    for facet in ("scans", "precursors", "selected_ions"):
        path = dest / f"spectra_metadata_{facet}.parquet"
        if path.exists():
            filter_rows(path, "source_index", keep)

    index_path = dest / "mzpeak_index.json"
    index = json.loads(index_path.read_text())
    kept = []
    for entry in index["files"]:
        name = entry.get("path") or entry.get("name")
        if entry.get("entity_type") in ("chromatogram", "vendor"):
            (dest / name).unlink(missing_ok=True)
            continue
        if not (dest / name).exists():
            continue
        entry["checksum"] = hashlib.sha512((dest / name).read_bytes()).hexdigest()
        kept.append(entry)
    index["files"] = kept
    index_path.write_text(json.dumps(index, indent=2))

    total = sum(f.stat().st_size for f in dest.iterdir())
    print(f"wrote {dest} ({total / 1024:.0f} KB)")

    write_variant(dest, "grid_bad_bounds.dir", bad_start)
    write_variant(dest, "grid_bad_end.dir", bad_end)
    write_variant(dest, "grid_legacy.dir", legacy)
    write_variant(dest, "grid_bad_mobility.dir", bad_mobility)
    write_variant(dest, "grid_zero_end_multi.dir", zero_end_multi)
    write_variant(dest, "grid_mixed.dir", mixed_main)
    write_variant(dest, "grid_mixed_secondary.dir", mixed_secondary,
                  add_secondary_entry)
    write_variant(dest, "grid_no_end.dir", no_end, drop_end_entry)
    write_variant(dest, "grid_short_mobility.dir", short_mobility)
    return 0


if __name__ == "__main__":
    sys.exit(main())
