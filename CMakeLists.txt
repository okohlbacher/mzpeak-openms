# CMake build of libmzpeak. Mirrors meson.build (the reference build); keep the source
# list in step with it. Consumers (OpenMS) add this directory with add_subdirectory()
# or FetchContent and link the `mzpeak` target.
cmake_minimum_required(VERSION 3.24)
project(mzpeak LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# No version argument: Arrow's config is SameMajorVersion, so a requested 21 would reject 25.
# The minimum is checked below.
find_package(Arrow CONFIG REQUIRED)
find_package(Parquet CONFIG REQUIRED)
if(ARROW_VERSION VERSION_LESS 21.0.0)
  message(FATAL_ERROR "mzpeak needs Arrow >= 21.0.0, found ${ARROW_VERSION}")
endif()
find_package(Boost 1.89.0 REQUIRED COMPONENTS json program_options)
# A host (OpenMS) may already have imported libzip::zip; do not import it twice.
if(NOT TARGET libzip::zip)
  find_package(libzip 1.11.2 CONFIG)
endif()
if(NOT TARGET libzip::zip)
  find_library(LIBZIP_LIB zip REQUIRED)
  find_path(LIBZIP_INCLUDE_DIR zip.h REQUIRED)
  add_library(libzip::zip UNKNOWN IMPORTED)
  set_target_properties(libzip::zip PROPERTIES IMPORTED_LOCATION "${LIBZIP_LIB}" INTERFACE_INCLUDE_DIRECTORIES "${LIBZIP_INCLUDE_DIR}")
endif()

# msnumpress, vendored as a subproject (see subprojects/msnumpress).
add_library(numpress STATIC subprojects/msnumpress/src/main/cpp/MSNumpress.cpp)
target_include_directories(numpress PUBLIC subprojects/msnumpress/src/main/cpp)

set(MZPEAK_SOURCES
  src/data/array_index.cpp
  src/data/signals.cpp
  src/index.cpp
  src/io/directory.cpp
  src/io/file.cpp
  src/io/zip.cpp
  src/metadata/scans.cpp
  src/metadata/spectrum.cpp
  src/metadata/table.cpp
  src/open.cpp
  src/run_metadata.cpp
  src/schema/buffer_format.cpp
  src/schema/cv.cpp
  src/schema/data_kind.cpp
  src/schema/entity_type.cpp
  src/schema/file.cpp
  src/schema/group.cpp
  src/schema/psi/array_type.cpp
  src/schema/psi/chunk_encoding.cpp
  src/schema/psi/data_type.cpp
  src/chromatogram.cpp
  src/chromatograms.cpp
  src/schema/psi/transform.cpp
  src/spectra.cpp
  src/spectrum.cpp
  src/wavelength_spectrum.cpp
  src/wavelength_spectra.cpp
  src/util/arrow.cpp
  src/util/decode_pool.cpp
  src/util/decoders.cpp
  src/util/executor.cpp
  src/util/manager.cpp
  src/util/parquet.cpp
  src/util/sha512.cpp
  src/util/json_writer.cpp
  src/util/key_runs.cpp
  src/util/metadata_model.cpp
  src/util/null_fill.cpp
  src/util/numpress.cpp
  src/util/parquet_writer.cpp
  src/util/planner.cpp
  src/util/row_group_cache.cpp
  src/util/projection.cpp
  src/util/query.cpp
  src/util/slice.cpp
  src/util/types.cpp
  src/writer.cpp
)

add_library(mzpeak ${MZPEAK_SOURCES})
target_include_directories(mzpeak PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(mzpeak PUBLIC
  Arrow::arrow_shared Parquet::parquet_shared
  Boost::json Boost::program_options
  libzip::zip numpress)
target_compile_options(mzpeak PRIVATE -Wall -Wextra -Wno-abi-tag -Wno-missing-declarations -Wno-padded -Wno-switch-default -Wswitch-enum)
