#!/usr/bin/env python3
"""Regenerate the declares_sorted_{honest,lies}.dir fixtures.

The reader's hot path binary-searches the entity index whenever the row group
DECLARES that column sorted ascending (Parquet `sorting_columns`).  That
declaration is taken on trust: verifying it per query would cost the O(n) scan
the search exists to avoid.

So the interesting question is what happens when a writer declares sorted and
then writes unsorted data -- a malformed file this reader cannot detect up
front, and which a third-party writer could plausibly produce.

`lies` is `honest` with the spectrum GROUPS reordered 0,2,1,3.  Every
spectrum's points stay contiguous and unmodified, only their order in the file
changes, so the index column is no longer non-decreasing while the file still
declares that it is.  Nothing else differs -- same schema, same key/value
metadata, same per-spectrum declared point counts.

Usage (needs pyarrow; the fixtures are committed, so this is only for
regenerating them):

    mzp-bench gen /tmp/honest 4 5
    python3 test/files/make_declares_sorted_fixtures.py /tmp/honest /tmp/lies
"""

import shutil
import sys

import pyarrow as pa
import pyarrow.parquet as pq

# The order the spectrum groups are written in: 1 and 2 swap places.
GROUP_ORDER = [0, 2, 1, 3]


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    source, dest = sys.argv[1], sys.argv[2]
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(source, dest)

    path = f"{dest}/spectra_data.parquet"
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    key_value = parquet.metadata.metadata

    point = table.column(0).combine_chunks()
    index = point.field("spectrum_index").to_pylist()
    mz = point.field("mz").to_pylist()
    intensity = point.field("intensity").to_pylist()

    rows = []
    for spectrum in GROUP_ORDER:
        rows += [i for i, v in enumerate(index) if v == spectrum]

    reordered = pa.StructArray.from_arrays(
        [
            pa.array([index[i] for i in rows], pa.uint64()),
            pa.array([mz[i] for i in rows], pa.float64()),
            pa.array([intensity[i] for i in rows], pa.float32()),
        ],
        names=["spectrum_index", "mz", "intensity"],
    )

    schema = pa.schema([pa.field("point", reordered.type)]).with_metadata(key_value)
    out = pa.Table.from_arrays([reordered], schema=schema)

    # The lie: still declare the index column sorted ascending, nulls last.
    pq.write_table(
        out,
        path,
        sorting_columns=[pq.SortingColumn(0, descending=False, nulls_first=False)],
        store_schema=True,
        write_statistics=True,
        write_page_index=True,
    )

    check = pq.ParquetFile(path)
    values = check.read().column(0).combine_chunks().field("spectrum_index").to_pylist()
    assert values != sorted(values), "fixture must NOT be sorted"
    assert check.metadata.row_group(0).sorting_columns, "fixture must declare sorted"
    print(f"wrote {path}: index={values}, still declares sorted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
