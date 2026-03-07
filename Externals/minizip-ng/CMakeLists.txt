set(MZ_COMPAT OFF)
set(MZ_BUILD_TESTS OFF)
set(MZ_BUILD_UNIT_TESTS OFF)
set(MZ_BUILD_FUZZ_TESTS OFF)
set(MZ_CODE_COVERAGE OFF)
set(SKIP_INSTALL_ALL ON)

# Prevent minizip-ng from fetching its own copies of libraries that Dolphin
# already provides (bzip2, zstd).  On Windows MZ_FETCH_LIBS defaults to ON,
# which causes duplicate CMake target errors.
set(MZ_FETCH_LIBS OFF)
set(MZ_FORCE_FETCH_LIBS OFF)

add_subdirectory(minizip-ng)
dolphin_disable_warnings(minizip-ng)
