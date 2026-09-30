#!/usr/bin/env python3
"""Trim a grid-encoded Bruker diaPASEF run into the bundled `grid_diapasef.dir`.

PSI-MS "coordinate grid encoding" (MS:1003826) stores a chunk's m/z as integer
indices into a grid rather than as coordinates, and can do the same for ion
mobility.  This fixture is the read-path coverage for that: a chunked archive
whose m/z arrives through the Bruker timsTOF m/z model (MS:9999002) and whose
mobility arrives through the Bruker mobility model (MS:9999001).

It keeps THE SAME two MS2 spectra as `make_diapasef_fixture.py` keeps, from the
same acquisition converted both ways.  That is the point of it: the bundled
`diapasef.dir` holds those spectra with their coordinates written out, so it is
a value-for-value oracle for this one, and `grid_test.cpp` compares the two
array by array rather than against numbers pasted into the test.

Both fixtures must be built from the SAME converter run.  The writer's m/z
conversion changed by about one ulp between releases, so a grid fixture from one
release and a point fixture from another disagree in the last bits of a handful
of values -- which would look exactly like a decoder bug.

    cd ../hupo-mzpeak && cargo build --release --example convert
    ./target/release/examples/convert test/data/diaPASEF.d -o /tmp/diapasef.grid.mzpeak --grid
    python3 test/files/make_grid_fixture.py /tmp/diapasef.grid.mzpeak test/files/grid_diapasef.dir
"""

import hashlib
import json
import shutil
import sys
import zipfile
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

KEEP = [1, 2]  # the two MS2 spectra; the MS1 frame is 200x larger

RENUMBER = {old: new for new, old in enumerate(KEEP)}


def retally(kv, kept_spectra: int, kept_points: int | None):
    """Rewrite the declared counts so they describe THIS file."""
    out = dict(kv or {})
    if b"spectrum_count" in out:
        out[b"spectrum_count"] = str(kept_spectra).encode()
    if kept_points is not None and b"spectrum_data_point_count" in out:
        out[b"spectrum_data_point_count"] = str(kept_points).encode()
    return out


def filter_signals(path: Path, keep: set[int]) -> None:
    """Keep the rows of a point OR chunked signal table for `keep`.

    A chunked table's rows are chunks rather than peaks, so the declared point
    count is recomputed from the intensity lists; carrying the row count over
    would understate it by whatever the chunk size is.
    """
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    kv = parquet.metadata.metadata

    top = table.schema.names[0]  # "point" or "chunk"
    struct = table.column(0).combine_chunks()
    names = [f.name for f in struct.type]

    index = struct.field("spectrum_index").to_pylist()
    rows = [i for i, v in enumerate(index) if v in keep]
    picks = pa.array(rows, pa.int64())

    fields = []
    for n in names:
        column = struct.field(n).take(picks)
        if n == "spectrum_index":
            column = pa.array([RENUMBER[v] for v in column.to_pylist()], column.type)
        fields.append(column)

    out = pa.StructArray.from_arrays(fields, names=names)

    if top == "chunk":
        intensity = out.field("intensity")
        points = sum(len(v) for v in intensity.to_pylist() if v is not None)
    else:
        points = len(rows)

    schema = pa.schema([pa.field(top, out.type)]).with_metadata(
        retally(kv, len(keep), points))
    pq.write_table(pa.Table.from_arrays([out], schema=schema), path,
                   write_statistics=True)


def filter_rows(path: Path, column: str, keep: set[int]) -> None:
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    kv = parquet.metadata.metadata
    if column not in table.schema.names:
        return
    values = table.column(column).to_pylist()
    rows = [i for i, v in enumerate(values) if v in keep]
    picks = pa.array(rows, pa.int64())
    trimmed = table.take(picks)
    renumbered = pa.array([RENUMBER[v] for v in trimmed.column(column).to_pylist()],
                          trimmed.schema.field(column).type)
    trimmed = trimmed.set_column(trimmed.schema.get_field_index(column), column,
                                 renumbered)
    pq.write_table(trimmed.replace_schema_metadata(retally(kv, len(keep), None)),
                   path, write_statistics=True)


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
    for name in ("spectra_data.parquet", "spectra_peaks.parquet"):
        if (dest / name).exists():
            filter_signals(dest / name, keep)

    filter_rows(dest / "spectra_metadata.parquet", "index", keep)
    for facet in ("scans", "precursors", "selected_ions"):
        path = dest / f"spectra_metadata_{facet}.parquet"
        if path.exists():
            filter_rows(path, "source_index", keep)

    # Chromatograms are unrelated to what this fixture is for, and the TIC
    # spans the whole run; drop them rather than ship a truncated one.
    index_path = dest / "mzpeak_index.json"
    index = json.loads(index_path.read_text())
    kept_files = []
    for entry in index["files"]:
        name = entry.get("path") or entry.get("name")
        if entry.get("entity_type") == "chromatogram":
            (dest / name).unlink(missing_ok=True)
            continue
        entry["checksum"] = hashlib.sha512((dest / name).read_bytes()).hexdigest()
        kept_files.append(entry)
    index["files"] = kept_files
    index_path.write_text(json.dumps(index, indent=2))

    total = sum(f.stat().st_size for f in dest.iterdir())
    print(f"wrote {dest} ({total / 1024:.0f} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
