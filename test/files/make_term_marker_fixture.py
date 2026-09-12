#!/usr/bin/env python3
"""Build the term_markers.dir fixture.

No archive produced by this project's writer or by the reference writer carries
a `term_marker` column, so the reader's handling of them had nothing to test
against.  This adds both forms the specification defines to an otherwise
ordinary split-layout archive:

  opt_calibration_spectrum   boolean  MS:1000928  presence of a value-less term
  opt_dissociation_method    string   MS:1000044  CURIE of a CHILD of that term

and additionally sets `term_marker` on the STANDARDISED `spectrum_representation`
mapping (MS:1000525).  That column is already surfaced as a typed field, so a
reader that appends every marker blindly reports it twice; the fixture pins that
it does not.

Row layout, chosen so one archive covers every branch:

  row 0   calibration True    dissociation MS:1000422   -> both terms present
  row 1   calibration False   dissociation MS:1000598   -> only the child term
  row 2   calibration null    dissociation null         -> neither
  row 3   calibration False   dissociation ""           -> neither (empty CURIE)

The fixture is committed, so this is only needed to regenerate it:

    ./build/mzp-bench gen /tmp/tm_base 4 5
    python3 test/files/make_term_marker_fixture.py /tmp/tm_base test/files/term_markers.dir
"""

import json
import shutil
import sys

import pyarrow as pa
import pyarrow.parquet as pq

CALIBRATION = [True, False, None, False]
DISSOCIATION = ["MS:1000422", "MS:1000598", None, ""]

ADDED_MAPPINGS = [
    {
        "name": "calibration spectrum",
        "path": "opt_calibration_spectrum",
        "accession": "MS:1000928",
        "unit": None,
        "term_marker": True,
    },
    {
        "name": "dissociation method",
        "path": "opt_dissociation_method",
        "accession": "MS:1000044",
        "unit": None,
        "term_marker": True,
    },
]


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    source, dest = sys.argv[1], sys.argv[2]
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(source, dest)

    path = f"{dest}/spectra_metadata.parquet"
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    key_value = parquet.metadata.metadata

    if table.num_rows != len(CALIBRATION):
        print(f"expected {len(CALIBRATION)} spectra, got {table.num_rows}")
        return 1

    table = table.append_column(
        "opt_calibration_spectrum", pa.array(CALIBRATION, pa.bool_())
    )
    table = table.append_column(
        "opt_dissociation_method", pa.array(DISSOCIATION, pa.string())
    )
    pq.write_table(
        table.replace_schema_metadata(key_value), path, store_schema=True
    )

    index_path = f"{dest}/mzpeak_index.json"
    with open(index_path) as handle:
        index = json.load(handle)

    patched = False
    for entry in index["files"]:
        name = entry.get("path") or entry.get("name") or ""
        if not name.endswith("spectra_metadata.parquet"):
            continue
        if entry.get("data_kind") != "metadata":
            continue

        mapping = entry.setdefault("column_mapping", [])
        for existing in mapping:
            # The de-duplication case: a standardised, already-typed column that
            # the writer is entitled to flag as a marker.
            if existing.get("accession") == "MS:1000525":
                existing["term_marker"] = True
        mapping.extend(ADDED_MAPPINGS)
        patched = True

    if not patched:
        print("no spectrum metadata entry found in the index")
        return 1

    with open(index_path, "w") as handle:
        json.dump(index, handle, indent=2)

    print(f"wrote {dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
