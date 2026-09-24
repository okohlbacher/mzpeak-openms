#!/usr/bin/env python3
"""Trim a converted Bruker diaPASEF run into the bundled `diapasef.dir` fixture.

Every ion-mobility path in this reader was pinned only by hand-built fixtures
and by its behaviour when the data was ABSENT, because no vendor file was
available -- the README said so in as many words.  The prototype now ships a
real diaPASEF acquisition, so this turns it into something small enough to
commit.

Two MS2 spectra are kept.  The MS1 frame alone carries 205,921 peaks and would
dominate the repository for no extra coverage: the MS2 frames exercise the
per-peak mobility array, the precursor and isolation window, the selected ion's
mobility value, and -- the reason this fixture exists -- an `activation` facet
whose `dissociation_method` and `collision_energy` are promoted COLUMNS rather
than entries in a parameter list.

Checksums are recomputed.  The source archive carries a SHA-512 per member and
this script rewrites members, so copying the digests across would produce a
fixture that fails the very verification this project just implemented.

    cd ../hupo-mzpeak && cargo build --release --example convert
    ./target/release/examples/convert test/data/diaPASEF.d -o /tmp/diapasef.mzpeak
    python3 test/files/make_diapasef_fixture.py /tmp/diapasef.mzpeak test/files/diapasef.dir
"""

import hashlib
import json
import shutil
import sys
import zipfile
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

KEEP = [1, 2]  # two MS2 spectra; the MS1 frame is 200x larger and adds nothing

# Kept spectra are renumbered to 0..n-1.  Leaving the original indices behind
# would make the collection size itself from the HIGHEST index, so a fixture of
# two spectra would report three, the first of them empty -- an artifact of
# trimming that a future reader of this fixture would have to rediscover.
RENUMBER = {old: new for new, old in enumerate(KEEP)}


def retally(kv, kept_spectra: int, kept_points: int | None):
    """Rewrite the declared counts so they describe THIS file.

    A stale count is not cosmetic: the reader sizes its collection from it, so
    leaving 9 behind would make the fixture claim seven spectra it does not
    contain, and each would read back empty.
    """
    out = dict(kv or {})
    if b"spectrum_count" in out:
        out[b"spectrum_count"] = str(kept_spectra).encode()
    if kept_points is not None and b"spectrum_data_point_count" in out:
        out[b"spectrum_data_point_count"] = str(kept_points).encode()
    return out


def filter_points(path: Path, keep: set[int]) -> None:
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    kv = parquet.metadata.metadata

    point = table.column(0).combine_chunks()
    names = [f.name for f in point.type]
    index = point.field("spectrum_index").to_pylist()
    rows = [i for i, v in enumerate(index) if v in keep]

    picks = pa.array(rows, pa.int64())
    fields = []
    for n in names:
        column = point.field(n).take(picks)
        if n == "spectrum_index":
            column = pa.array([RENUMBER[v] for v in column.to_pylist()],
                              column.type)
        fields.append(column)
    out = pa.StructArray.from_arrays(fields, names=names)
    schema = pa.schema([pa.field("point", out.type)]).with_metadata(
        retally(kv, len(keep), len(rows)))
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
            filter_points(dest / name, keep)

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
        # Rewritten members: the recorded digest is now stale.
        entry["checksum"] = hashlib.sha512((dest / name).read_bytes()).hexdigest()
        kept_files.append(entry)
    index["files"] = kept_files
    index_path.write_text(json.dumps(index, indent=2))

    total = sum(f.stat().st_size for f in dest.iterdir())
    print(f"wrote {dest} ({total / 1024:.0f} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
