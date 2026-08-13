#!/usr/bin/env python3
"""Build a chunked archive whose chunk_end contradicts its chunk_start.

Used to verify that the chunked decoder's endpoint validation actually RUNS.
It did not, for a while: `ArrayIndex::Entry::needed_for_decoding()` returned
false for `ChunkEnd`, so `Signals` never projected the column, so `ends` was
always null in `Decoder::chunked()` -- and every check guarded by
`ends != nullptr` (start <= end, chunks ascending and non-overlapping, and the
start == end == 0 empty-chunk sentinel) silently never executed, while the
documentation claimed chunk ordering was validated on read.

The output is NOT committed: `small.chunked.mzpeak` unpacks to ~2.3 MB and the
corruption is one float.  Regenerate it when you need it.

    python3 test/files/make_bad_chunk_fixture.py test/files/small.chunked.mzpeak /tmp/chunkbad

Expected behaviour of a correct reader on the result: reading spectrum 0 throws

    chunk_start exceeds chunk_end (mz_chunk_start)

With the validation dead, spectrum 0 instead decodes "successfully" to 13589
points, which is the whole point of the fixture.
"""

import shutil
import sys
import zipfile

import pyarrow as pa
import pyarrow.parquet as pq


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    source, dest = sys.argv[1], sys.argv[2]
    shutil.rmtree(dest, ignore_errors=True)
    with zipfile.ZipFile(source) as archive:
        archive.extractall(dest)

    path = f"{dest}/spectra_data.parquet"
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    key_value = parquet.metadata.metadata

    chunk = table.column(0).combine_chunks()
    names = [field.name for field in chunk.type]
    columns = {name: chunk.field(name) for name in names}

    starts = columns["mz_chunk_start"].to_pylist()
    ends = columns["mz_chunk_end"].to_pylist()

    # First fully-populated chunk: make its end precede its start.
    row = next(
        i
        for i, (s, e) in enumerate(zip(starts, ends))
        if s is not None and e is not None
    )
    print(f"row {row}: start={starts[row]} end={ends[row]} -> end={starts[row] - 10.0}")
    ends[row] = starts[row] - 10.0
    columns["mz_chunk_end"] = pa.array(ends, pa.float64())

    corrupted = pa.StructArray.from_arrays(
        [columns[n] for n in names], names=names
    )
    schema = pa.schema([pa.field("chunk", corrupted.type)]).with_metadata(key_value)
    pq.write_table(pa.Table.from_arrays([corrupted], schema=schema), path)

    print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
