#https: // mesonbuild.com/
project('MZPeak C++ Library', 'cpp',
        version : '1.0.0',
        license : 'BSD3',
        default_options : [
          # A list: meson takes the first one the compiler knows. MSVC has no
          # 'c++23' choice, only 'c++latest'.
          'cpp_std=c++23,c++latest',
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
  '/utf-8',  # MSVC: source is UTF-8 regardless of the code page
  '/bigobj', # MSVC: Arrow/Boost template TUs overflow the default section count
]), language: 'cpp')

is_windows = host_machine.system() == 'windows'
if is_windows
  # <windows.h> reaches in through Arrow; keep its min/max macros out.
  # Boost.JSON is a DLL on Windows (conda-forge ships no static one): without
  # DYN_LINK its headers do not declare the static data members dllimport,
  # and object::empty_, array::empty_ and default_resource::instance_ stay
  # unresolved. Functions link through the import library either way.
  add_project_arguments('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN',
                        '-DBOOST_JSON_DYN_LINK=1', language : 'cpp')
endif

#Source files included in the library:
lib_sources = [
  'src/data/array_index.cpp',
  'src/data/signals.cpp',
  'src/index.cpp',
  'src/io/directory.cpp',
  'src/io/file.cpp',
  'src/io/zip.cpp',
  'src/metadata/scans.cpp',
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
  'src/schema/psi/chunk_encoding.cpp',
  'src/schema/psi/data_type.cpp',
  'src/chromatogram.cpp',
  'src/chromatograms.cpp',
  'src/schema/psi/transform.cpp',
  'src/spectra.cpp',
  'src/spectrum.cpp',
  'src/wavelength_spectrum.cpp',
  'src/wavelength_spectra.cpp',
  'src/util/arrow.cpp',
  'src/util/decoders.cpp',
  'src/util/executor.cpp',
  'src/util/manager.cpp',
  'src/util/parquet.cpp',
  'src/util/json_writer.cpp',
  'src/util/metadata_model.cpp',
  'src/util/null_fill.cpp',
  'src/util/numpress.cpp',
  'src/util/parquet_writer.cpp',
  'src/util/planner.cpp',
  'src/util/row_group_cache.cpp',
  'src/util/projection.cpp',
  'src/util/query.cpp',
  'src/util/slice.cpp',
  'src/util/types.cpp',
  'src/writer.cpp',
]

# Header files to include in packages:
install_headers([
  'include/mzpeak.h',
  'include/mzpeak/data/array_index.h',
  'include/mzpeak/data/encoding.h',
  'include/mzpeak/data/null_marking.h',
  'include/mzpeak/data/signals.h',
  'include/mzpeak/data/transformer/primary.h',
  'include/mzpeak/data/transformer/secondary.h',
  'include/mzpeak/exception.h',
  'include/mzpeak/ims_calibration.h',
  'include/mzpeak/index.h',
  'include/mzpeak/io/archive.h',
  'include/mzpeak/io/directory.h',
  'include/mzpeak/io/file.h',
  'include/mzpeak/io/zip.h',
  'include/mzpeak/metadata/scans.h',
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
  'include/mzpeak/schema/psi/chunk_encoding.h',
  'include/mzpeak/schema/psi/data_type.h',
  'include/mzpeak/chromatogram.h',
  'include/mzpeak/chromatogram_metadata.h',
  'include/mzpeak/chromatograms.h',
  'include/mzpeak/schema/psi/transform.h',
  'include/mzpeak/spectra.h',
  'include/mzpeak/spectrum.h',
  'include/mzpeak/run_metadata.h',
  'include/mzpeak/spectrum_metadata.h',
  'include/mzpeak/wavelength_spectrum.h',
  'include/mzpeak/wavelength_spectrum_metadata.h',
  'include/mzpeak/wavelength_spectra.h',
  'include/mzpeak/util/algorithm.h',
  'include/mzpeak/util/arrow.h',
  'include/mzpeak/util/compat.h',
  'include/mzpeak/util/decoders.h',
  'include/mzpeak/util/delta_estimator.h',
  'include/mzpeak/util/enumerable_proxy.h',
  'include/mzpeak/util/executor.h',
  'include/mzpeak/util/manager.h',
  'include/mzpeak/util/numpress.h',
  'include/mzpeak/util/parquet.h',
  'include/mzpeak/util/json_writer.h',
  'include/mzpeak/util/metadata_model.h',
  'include/mzpeak/util/null_fill.h',
  'include/mzpeak/util/parquet_writer.h',
  'include/mzpeak/util/planner.h',
  'include/mzpeak/util/projection.h',
  'include/mzpeak/util/index_map.h',
  'include/mzpeak/util/query.h',
  'include/mzpeak/util/row_group_cache.h',
  'include/mzpeak/util/slice.h',
  'include/mzpeak/util/types.h',
  'include/mzpeak/writer.h',
], subdir : 'mzpeak')

# Sub-projects:
numpress = subproject('msnumpress')

# Library and executable targets:
include = include_directories('include')

arrow_ver_str = '>=21.0.0'

deps = [
    dependency('arrow',
               version : arrow_ver_str,
               required : true),

    dependency('boost',
               version : '>=1.89.0',
               modules : ['json', 'program_options'],
               required : true),

    dependency('libzip',
               version :'>=1.11.2',
               required : true),

    dependency('parquet', # Part of Arrow above.
               version : arrow_ver_str,
               required : true),

]

# The shared library links the shared numpress; the static one links the static
# numpress and pulls its objects IN with link_whole.
#
# Numpress used to be vendored as src/util/vendor/MSNumpress.cpp, i.e. compiled
# straight into libmzpeak.a, so the static library was self-contained.  Moving
# to the subproject left `decodeLinear`/`decodeSlof` UNDEFINED in libmzpeak.a,
# so a downstream static link that used to work now fails unless the consumer
# also links msnumpress.  link_whole restores the old property.
deps_a = deps + [numpress.get_variable('msnumpres_cpp_lib_a')]

libmzpeak_a = static_library(
  'mzpeak', lib_sources,
  dependencies : deps_a,
  link_whole : numpress.get_variable('cpp_lib_a'),
  include_directories : include,
  install : true)

# ponytail: nothing here carries dllexport annotations, so on Windows a DLL
# would export no symbols and link.exe would emit no import library. Windows
# builds the static library only; tests, tools and consumers link that.
if is_windows
  libmzpeak = libmzpeak_a
  libmzpeak_deps = deps_a
else
  deps_so = deps + [numpress.get_variable('msnumpres_cpp_lib_so')]

  libmzpeak_so = library(
    'mzpeak', lib_sources,
    dependencies : deps_so,
    include_directories : include,
    install : true)

  libmzpeak = libmzpeak_so
  libmzpeak_deps = deps_so
endif

#Testing
test_names = [
    'algorithm',
    'array_index',
    'archive_writer',
    'arrow',
    'chromatograms',
    'chunked',
    'chunked_chromatograms',
    'directory',
    'executor',
    'group',
    'group_hint',
    'index',
    'parquet',
    'record_count',
    'json_writer',
    'null_fill',
    'numpress',
    'parquet_writer',
    'planner',
    'query',
    'query_api',
    'roundtrip',
    'row_group',
    'row_group_cache',
    'run_writer',
    'sorting_declaration',
    'spectra',
    'spectrum_metadata',
    'util',
    'wavelength_spectra',
    'writer',
    'zero_peaks',
    'zip',
    'index_metadata',
    'writer_metadata',
    'eic',
    'spectra_query',
    'stream_writer',
    'term_marker',
]

# A timed-out test is reported as neither pass nor fail: it drops out of the
# count entirely, so the run still says `Fail: 0` and reads as green while
# having silently tested less. Not hypothetical -- one thread-sanitizer run
# reported 39 of 40 with no failure, and did not reproduce. Meson's default is
# 30 s and the slowest sanitised unit test measures 8.5 s, so a loaded machine
# need only be ~3.5x slower to lose one.
foreach test : test_names
    test(test,
         executable('@0@_test'.format(test),
                    'test/@0@_test.cpp'.format(test),
                    include_directories : include,
                    dependencies : libmzpeak_deps,
                    link_with : libmzpeak),
         timeout : 120,
         verbose : false)
endforeach

#Full end - to - end integration test : drives the whole reader API over every
#bundled fixture plus a reverse round trip through the writer.It decodes
#real profile fixtures in full, so it is slower than the unit tests and is
#given a generous timeout and its own `e2e` suite(run alone with
# `meson test -- suite e2e`).
test('e2e',
     executable('e2e_test',
                'test/e2e_test.cpp',
                include_directories : include,
                dependencies : libmzpeak_deps,
                link_with : libmzpeak),
     suite : 'e2e',
     timeout : 300,
     verbose : false)

#Examples and tools -- optional together, see meson_options.txt: both use
#<print>, and a consumer that only wants the library builds neither.
if get_option('tools')
  example_names = [
    'read_spectra'
  ]

  foreach example : example_names
    executable(
      example,
      'examples/@0@.cpp'.format(example),
      include_directories : include,
      dependencies : libmzpeak_deps,
      link_with : libmzpeak,
      install : true,
      install_dir : 'examples')
  endforeach

  tool_names = [
    'mzp-inspect',
    'mzp-bench'
  ]

  foreach tool : tool_names
    executable(
      tool,
      'bin/@0@.cpp'.format(tool),
      include_directories : include,
      dependencies : libmzpeak_deps,
      link_with : libmzpeak,
      install : true)
  endforeach
endif
