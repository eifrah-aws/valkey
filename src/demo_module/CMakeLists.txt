project(demo_module)

set(MODULE_NAME "demo_module")

message(STATUS "Building module: demo_module")
set(SRCS "${CMAKE_CURRENT_LIST_DIR}/demo.cpp")
add_library(${MODULE_NAME} SHARED ${SRCS})
target_include_directories(${MODULE_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/src")
set_target_properties(${MODULE_NAME} PROPERTIES PREFIX "")
target_link_libraries(${MODULE_NAME} PRIVATE rocksdb)

if (APPLE)
    # Some symbols can only be resolved during runtime (they exist in the executable)
    target_link_options(${MODULE_NAME} PRIVATE -undefined dynamic_lookup)
endif ()
