#!/usr/bin/env python3
"""Derive `mixed_grid_diapasef.dir` from `grid_diapasef.dir`.

A grid-encoded column may hold chunks that are NOT grid encoded: the SCIEX
SWATH corpus has row groups with both, where `chunk_encoding` is MS:1000576,
the `*_grid` struct is null, and the chunk's values sit in `chunk_values` with
its first value in `chunk_start`.  A reader that only visits non-null struct
rows silently drops those peaks.

The only real archive exercising that shape is 738 MB, far too large to commit,
so this rewrites every other m/z chunk of the small grid fixture into exactly
that shape -- decoding the grid indices and writing the coordinates out, which
is what the writer would have stored.  The ion mobility column is left gridded,
so the fixture also covers a gridded secondary axis beside a plainly encoded
main axis.

The values are unchanged by construction, so this fixture must still decode
equal to `diapasef.dir`, and `grid_test.cpp` asserts exactly that.

    python3 test/files/make_mixed_grid_fixture.py test/files/grid_diapasef.dir \
                                                  test/files/mixed_grid_diapasef.dir
"""

import hashlib
import json
import math
import shutil
import sys
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq


def timstof_mz(index: int, p) -> float:
    """MS:9999002, the reference's scalar converter (mzdata MzCalibrationModel2)."""
    c0, beta, c2, c3, c4, timebase, delay = p
    tof = math.fma(float(index), timebase, delay)
    s0 = (tof - c0) / beta
    if c3 != 0.0:
        s, c2_2, c3_3 = s0, c2 * 2.0, c3 * 3.0
        for _ in range(8):
            s2 = s * s
            f = math.fma(s, beta, c0) + s2 * c2 + math.fma(s2 * s, c3, -tof)
            df = math.fma(s, c2_2, beta) + s2 * c3_3
            if df == 0.0:
                break
            step = f / df
            s -= step
            if abs(step) < 1e-12:
                break
        refined = s
    elif c2 != 0.0:
        d = beta * beta - 4.0 * c2 * (c0 - tof)
        refined = s0 if d < 0.0 else (c0 - tof) / (-0.5 * (beta + math.sqrt(d)))
    else:
        refined = s0
    return refined * refined - c4


def decode(grid) -> list[float]:
    """The chunk's coordinates.  Main-axis indices are delta coded."""
    assert grid["grid_type"] == "MS:9999002", grid["grid_type"]
    out, last = [], 0
    for i, v in enumerate(grid["indices"]):
        last = v if i == 0 else (last + v) & 0xFFFFFFFF
        out.append(timstof_mz(last, grid["parameters"]))
    return out


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    source, dest = Path(sys.argv[1]), Path(sys.argv[2])
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(source, dest)

    path = dest / "spectra_peaks.parquet"
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    kv = parquet.metadata.metadata

    chunk = table.column(0).combine_chunks()
    names = [f.name for f in chunk.type]

    grids = chunk.field("mz_grid").to_pylist()
    starts = list(chunk.field("mz_chunk_start").to_pylist())
    ends = list(chunk.field("mz_chunk_end").to_pylist())
    values = list(chunk.field("mz_chunk_values").to_pylist())
    encodings = list(chunk.field("chunk_encoding").to_pylist())

    converted = 0
    for row, grid in enumerate(grids):
        if grid is None or row % 2 == 1:
            continue
        coords = decode(grid)
        if len(coords) < 2:
            continue  # a one-value chunk has nothing left after chunk_start
        starts[row] = coords[0]
        ends[row] = coords[-1]
        values[row] = coords[1:]        # chunk_start carries the first value
        encodings[row] = "MS:1000576"   # no compression
        grids[row] = None
        converted += 1

    rebuilt = {
        "mz_chunk_start": pa.array(starts, pa.float64()),
        "mz_chunk_end": pa.array(ends, pa.float64()),
        "mz_chunk_values": pa.array(values, chunk.field("mz_chunk_values").type),
        "chunk_encoding": pa.array(encodings, chunk.field("chunk_encoding").type),
        "mz_grid": pa.array(grids, chunk.field("mz_grid").type),
    }

    fields = [rebuilt.get(n, chunk.field(n)) for n in names]
    out = pa.StructArray.from_arrays(fields, names=names)
    schema = pa.schema([pa.field("chunk", out.type)]).with_metadata(kv)
    pq.write_table(pa.Table.from_arrays([out], schema=schema), path,
                   write_statistics=True)

    index_path = dest / "mzpeak_index.json"
    index = json.loads(index_path.read_text())
    for entry in index["files"]:
        name = entry.get("path") or entry.get("name")
        entry["checksum"] = hashlib.sha512((dest / name).read_bytes()).hexdigest()
    index_path.write_text(json.dumps(index, indent=2))

    total = sum(f.stat().st_size for f in dest.iterdir())
    print(f"wrote {dest}: {converted} of {len(grids)} chunks un-gridded "
          f"({total / 1024:.0f} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
