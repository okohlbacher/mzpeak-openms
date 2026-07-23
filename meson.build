# https://mesonbuild.com/
project('MZPeak C++ Library', 'cpp',
        version : '1.0.0',
        license : 'BSD3',
        default_options : [
          'cpp_std=c++23',
          'warning_level=3',
        ])

compiler = meson.get_compiler('cpp')
add_project_arguments(compiler.get_supported_arguments([
  '-Wno-abi-tag', # What lib is causing this?
  '-Wno-missing-declarations',
  '-Wno-noexcept', # Review at some point
  '-Wno-padded', # Let the compiler do its thing
  '-Wno-switch-default',
  '-Wswitch-enum',
  '/w14061',
  '/w14062',
]), language: 'cpp')

# Source files included in the library:
lib_sources = [
  'src/data/array_index.cpp',
  'src/data/signals.cpp',
  'src/index.cpp',
  'src/io/directory.cpp',
  'src/io/file.cpp',
  'src/io/zip.cpp',
  'src/metadata/spectrum.cpp',
  'src/metadata/table.cpp',
  'src/open.cpp',
  'src/run_metadata.cpp',
  'src/schema/buffer_format.cpp',
  'src/schema/cv.cpp',
  'src/schema/data_kind.cpp',
  'src/schema/entity_type.cpp',
  'src/schema/file.cpp',
  'src/schema/group.cpp',
  'src/schema/psi/array_type.cpp',
  'src/schema/psi/data_type.cpp',
  'src/chromatogram.cpp',
  'src/chromatograms.cpp',
  'src/schema/psi/transform.cpp',
  'src/spectra.cpp',
  'src/spectrum.cpp',
  'src/wavelength_spectrum.cpp',
  'src/wavelength_spectra.cpp',
  'src/util/arrow.cpp',
  'src/util/executor.cpp',
  'src/util/parquet.cpp',
  'src/util/json_writer.cpp',
  'src/util/metadata_model.cpp',
  'src/util/null_fill.cpp',
  'src/util/numpress.cpp',
  'src/util/parquet_writer.cpp',
  'src/util/planner.cpp',
  'src/util/projection.cpp',
  'src/util/query.cpp',
  'src/util/slice.cpp',
  'src/util/types.cpp',
  'src/util/vendor/MSNumpress.cpp',
  'src/writer.cpp',
]

# Header files to include in packages:
install_headers([
  'include/mzpeak.h',
  'include/mzpeak/data/array_index.h',
  'include/mzpeak/data/encoding.h',
  'include/mzpeak/data/null_marking.h',
  'include/mzpeak/data/signals.h',
  'include/mzpeak/exception.h',
  'include/mzpeak/index.h',
  'include/mzpeak/io/archive.h',
  'include/mzpeak/io/directory.h',
  'include/mzpeak/io/file.h',
  'include/mzpeak/io/zip.h',
  'include/mzpeak/metadata/spectrum.h',
  'include/mzpeak/metadata/table.h',
  'include/mzpeak/open.h',
  'include/mzpeak/run_metadata.h',
  'include/mzpeak/schema/buffer_format.h',
  'include/mzpeak/schema/cv.h',
  'include/mzpeak/schema/data_kind.h',
  'include/mzpeak/schema/entity_type.h',
  'include/mzpeak/schema/file.h',
  'include/mzpeak/schema/group.h',
  'include/mzpeak/schema/psi/array_type.h',
  'include/mzpeak/schema/psi/data_type.h',
  'include/mzpeak/chromatogram.h',
  'include/mzpeak/chromatograms.h',
  'include/mzpeak/schema/psi/transform.h',
  'include/mzpeak/spectra.h',
  'include/mzpeak/spectrum.h',
  'include/mzpeak/run_metadata.h',
  'include/mzpeak/spectrum_metadata.h',
  'include/mzpeak/wavelength_spectrum.h',
  'include/mzpeak/wavelength_spectra.h',
  'include/mzpeak/util/algorithm.h',
  'include/mzpeak/util/arrow.h',
  'include/mzpeak/util/compat.h',
  'include/mzpeak/util/decoders.h',
  'include/mzpeak/util/delta_estimator.h',
  'include/mzpeak/util/enumerable_proxy.h',
  'include/mzpeak/util/executor.h',
  'include/mzpeak/util/parquet.h',
  'include/mzpeak/util/json_writer.h',
  'include/mzpeak/util/metadata_model.h',
  'include/mzpeak/util/null_fill.h',
  'include/mzpeak/util/numpress.h',
  'include/mzpeak/util/parquet_writer.h',
  'include/mzpeak/util/planner.h',
  'include/mzpeak/util/projection.h',
  'include/mzpeak/util/query.h',
  'include/mzpeak/util/slice.h',
  'include/mzpeak/util/types.h',
  'include/mzpeak/util/vendor/MSNumpress.hpp',
  'include/mzpeak/writer.h',
], subdir : 'mzpeak')

# Library and executable targets:
include = include_directories('include')

arrow_ver_str = '>=23.0.0'

deps = [
    dependency('arrow',
               version : arrow_ver_str,
               required : true),

    dependency('boost',
               version : '>=1.89.0',
               modules : ['json', 'program_options'],
               required : true),

    dependency('libzip',
               version :'>=1.11.4',
               required : true),

    dependency('parquet', # Part of Arrow above.
               version : arrow_ver_str,
               required : true),
]

libmzpeak_so = library(
  'mzpeak', lib_sources,
  dependencies : deps,
  include_directories : include,
  install : true)

libmzpeak_a = static_library(
  'mzpeak', lib_sources,
  dependencies : deps,
  include_directories : include,
  install : true)

# Testing
test_names = [
    'array_index',
    'archive_writer',
    'arrow',
    'chromatograms',
    'directory',
    'executor',
    'group',
    'index',
    'parquet',
    'record_count',
    'json_writer',
    'null_fill',
    'numpress',
    'parquet_writer',
    'planner',
    'query',
    'roundtrip',
    'spectra',
    'spectrum_metadata',
    'util',
    'wavelength_spectra',
    'writer',
    'zip',
]

foreach test : test_names
    test(test,
         executable('@0@_test'.format(test),
                    'test/@0@_test.cpp'.format(test),
                    include_directories : include,
                    dependencies : deps,
                    link_with : libmzpeak_so),
         verbose : false)
endforeach

# Full end-to-end integration test: drives the whole reader API over every
# bundled fixture plus a reverse round trip through the writer.  It decodes
# real profile fixtures in full, so it is slower than the unit tests and is
# given a generous timeout and its own `e2e` suite (run alone with
# `meson test --suite e2e`).
test('e2e',
     executable('e2e_test',
                'test/e2e_test.cpp',
                include_directories : include,
                dependencies : deps,
                link_with : libmzpeak_so),
     suite : 'e2e',
     timeout : 300,
     verbose : false)

# Examples
example_names = [
  'read_spectra'
]

foreach example : example_names
  executable(
    example,
    'examples/@0@.cpp'.format(example),
    include_directories : include,
    dependencies : deps,
    link_with : libmzpeak_so,
    install : true,
    install_dir : 'examples')
endforeach

# Tools
tool_names = [
  'mzp-inspect'
]

foreach tool : tool_names
  executable(
    tool,
    'bin/@0@.cpp'.format(tool),
    include_directories : include,
    dependencies : deps,
    link_with : libmzpeak_so,
    install : true)
endforeach
