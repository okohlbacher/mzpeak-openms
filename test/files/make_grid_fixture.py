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

    ends = cols["mz_chunk_end"].to_pylist()
    last = [i for i in range(len(si)) if i + 1 == len(si) or si[i + 1] != si[i]]
    for i in last:
        ends[i] = 0.0
    cols["mz_chunk_end"] = pa.array(ends, pa.float64())
    return f"mobility pair swapped on every row; chunk_end = 0.0 on rows {last}"


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


def write_variant(source: Path, name: str, mutate) -> None:
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

    out = pa.StructArray.from_arrays([cols[n] for n in names], names=names)
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
