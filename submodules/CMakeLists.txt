set(ROCKSDB_BUILD_SHARED
    OFF
    CACHE BOOL "Build shared libraries")
set(WITH_SNAPPY
    ON
    CACHE BOOL "Enable Snappy compression")
set(WITH_GFLAGS
    OFF
    CACHE BOOL "Enable gflags")
set(WITH_TESTS
    OFF
    CACHE BOOL "With tests")
set(WITH_BENCHMARK_TOOLS
    OFF
    CACHE BOOL "Build benchmark tools")
set(WITH_CORE_TOOLS
    OFF
    CACHE BOOL "Build core tools")
set(WITH_TOOLS
    OFF
    CACHE BOOL "Build tools")
add_subdirectory(rocksdb)
