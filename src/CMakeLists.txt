cmake_minimum_required ( VERSION 2.6 )
add_definitions ( -Wall -fstrict-aliasing -Wstrict-aliasing=2 )

if(CMAKE_BUILD_TYPE MATCHES Debug)
  add_definitions( -DDEBUG )
else()
  add_definitions( -O3 )
endif()

set(CXX_FLAGS_RELEASE  "-O3") #this doesn't work, i need to remove -DNDEBUG from Release
set(C_FLAGS_RELEASE    "-O3")

SET(CMAKE_BUILD_TYPE Release CACHE STRING "ReleaseAssert/Debug")

find_package (Threads)
find_library(ZLIB z "Zlib library")

# large file support
execute_process(COMMAND getconf LFS_CFLAGS
      OUTPUT_VARIABLE LFS_CFLAGS)
if(LFS_CFLAGS)
   add_definitions(${LFS_CFLAGS})
endif(LFS_CFLAGS)

include_directories (
    ${CMAKE_SOURCE_DIR} 
    ${CMAKE_CURRENT_BINARY_DIR}
)

configure_file (
    "${PROJECT_SOURCE_DIR}/config.h.in"
    "${PROJECT_BINARY_DIR}/config.h"
)

add_library ( hiredis STATIC IMPORTED )
add_custom_command (
    OUTPUT libhiredis.a
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/hiredis
    COMMAND make static
)
set_target_properties ( hiredis PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/hiredis/libhiredis.a
)

#\
add_executable ( run_tests
    tests/test_file_io.cc
    tests/test_graph.cc
    tests/test_graph_algo.cc
    tests/test_redis_util.cc
    tests/test_sql_parser.cc
    gmock/gmock-gtest-all.cc
    tests/run_tests.cc
)
target_link_libraries ( run_tests
    ${CMAKE_THREAD_LIBS_INIT}
    hiredis
    z
)

#\
add_executable ( gen_graph
    libhiredis.a
    gen_graph.cc
)
target_link_libraries ( gen_graph
    ${CMAKE_THREAD_LIBS_INIT}
    hiredis
    z
)

#\
add_executable ( process_graph
    libhiredis.a
    process_graph.cc
)
target_link_libraries ( process_graph
    ${CMAKE_THREAD_LIBS_INIT}
    hiredis
    z
)

#\
add_executable ( print_graph
    print_graph.cc
)

