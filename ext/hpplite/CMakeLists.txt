cmake_minimum_required(VERSION 3.14)
project(hpplite VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Options
option(HPPLITE_BUILD_TESTS "Build test executables" ON)
option(HPPLITE_ENABLE_ZMQ "Enable ZeroMQ networking (M2+)" ON)

# Find dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)

# ZeroMQ (optional, for M2 multi-process)
if(HPPLITE_ENABLE_ZMQ)
    pkg_check_modules(ZMQ libzmq)
    if(ZMQ_FOUND)
        message(STATUS "ZeroMQ found: ${ZMQ_VERSION}")
    else()
        message(WARNING "ZeroMQ not found, disabling ZMQ support")
        set(HPPLITE_ENABLE_ZMQ OFF)
    endif()
endif()

# Find threads
find_package(Threads REQUIRED)

# SQLite paths (use pre-built from parent directory)
set(SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(SQLITE_BUILD_DIR "${SQLITE_DIR}/build")

# Check SQLite exists
if(NOT EXISTS "${SQLITE_BUILD_DIR}/sqlite3.c")
    message(FATAL_ERROR
        "SQLite amalgamation not found at ${SQLITE_BUILD_DIR}/sqlite3.c\n"
        "Please build SQLite first:\n"
        "  cd ${SQLITE_DIR}/build && ../configure && make sqlite3.c"
    )
endif()

# SQLite library (build from amalgamation)
add_library(sqlite3 STATIC
    ${SQLITE_BUILD_DIR}/sqlite3.c
)
target_include_directories(sqlite3 PUBLIC
    ${SQLITE_DIR}
    ${SQLITE_BUILD_DIR}
)
target_compile_definitions(sqlite3 PUBLIC
    SQLITE_ENABLE_PREUPDATE_HOOK
    SQLITE_THREADSAFE=1
)
target_link_libraries(sqlite3 PUBLIC
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

# HPPLite core library
set(HPPLITE_SOURCES
    hpplite.c
    batch.c
    fs_storage.c
    crypto.c
    node.c
    l1_mock.c
)

# Add ZMQ networking source if enabled
if(HPPLITE_ENABLE_ZMQ)
    list(APPEND HPPLITE_SOURCES zmq_transport.c l1_service.c)
endif()

add_library(hpplite STATIC ${HPPLITE_SOURCES})

target_include_directories(hpplite PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SECP256K1_INCLUDE_DIRS}
)

target_compile_definitions(hpplite PUBLIC
    SQLITE_ENABLE_PREUPDATE_HOOK
)

# Add ZMQ definitions and includes
if(HPPLITE_ENABLE_ZMQ)
    target_compile_definitions(hpplite PUBLIC HPPLITE_ENABLE_ZMQ)
    target_include_directories(hpplite PUBLIC ${ZMQ_INCLUDE_DIRS})
endif()

target_link_libraries(hpplite PUBLIC
    sqlite3
    ${SECP256K1_LIBRARIES}
)

# Add ZMQ libraries
if(HPPLITE_ENABLE_ZMQ)
    target_link_libraries(hpplite PUBLIC ${ZMQ_LIBRARIES})
    target_link_directories(hpplite PUBLIC ${ZMQ_LIBRARY_DIRS})
endif()

target_link_directories(hpplite PUBLIC
    ${SECP256K1_LIBRARY_DIRS}
)

# Test executables
if(HPPLITE_BUILD_TESTS)
    # Basic state tracking test
    add_executable(test_hpplite test_hpplite.c)
    target_link_libraries(test_hpplite hpplite)

    # Batch serialization test
    add_executable(test_batch test_batch.c)
    target_link_libraries(test_batch hpplite)

    # Sequencer API test
    add_executable(test_sequencer test_sequencer.c)
    target_link_libraries(test_sequencer hpplite)

    # Integrated flow test
    add_executable(test_integrated test_integrated.c)
    target_link_libraries(test_integrated hpplite)

    # Full node test
    add_executable(test_node test_node.c)
    target_link_libraries(test_node hpplite)

    # L1 mock test
    add_executable(test_l1_mock test_l1_mock.c)
    target_link_libraries(test_l1_mock hpplite)

    # M1 integration test
    add_executable(test_m1_integration test_m1_integration.c)
    target_link_libraries(test_m1_integration hpplite)

    # M2 multi-process test (requires ZMQ)
    if(HPPLITE_ENABLE_ZMQ)
        add_executable(test_m2_multiprocess test_m2_multiprocess.c)
        target_link_libraries(test_m2_multiprocess hpplite Threads::Threads)

        # M2 node-to-node ZMQ test
        add_executable(test_m2_nodes test_m2_nodes.c)
        target_link_libraries(test_m2_nodes hpplite Threads::Threads)
    endif()

    # Add tests to CTest
    enable_testing()
    add_test(NAME test_hpplite COMMAND test_hpplite)
    add_test(NAME test_batch COMMAND test_batch)
    add_test(NAME test_sequencer COMMAND test_sequencer)
    add_test(NAME test_integrated COMMAND test_integrated)
    add_test(NAME test_node COMMAND test_node)
    add_test(NAME test_l1_mock COMMAND test_l1_mock)
    add_test(NAME test_m1_integration COMMAND test_m1_integration)
    if(HPPLITE_ENABLE_ZMQ)
        add_test(NAME test_m2_multiprocess COMMAND test_m2_multiprocess)
        add_test(NAME test_m2_nodes COMMAND test_m2_nodes)
    endif()
endif()

# Benchmarks (not in ctest, run with 'make speedtest')
add_executable(bench_replay bench_replay.c)
target_link_libraries(bench_replay hpplite)

# speedtest target (like SQLite's make speedtest)
add_custom_target(speedtest
    COMMAND bench_replay --batches 100 --stats
    DEPENDS bench_replay
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running HPPLite replay benchmark..."
)

# Print configuration
message(STATUS "")
message(STATUS "HPPLite Configuration:")
message(STATUS "  SQLite dir: ${SQLITE_DIR}")
message(STATUS "  secp256k1: ${SECP256K1_LIBRARIES}")
message(STATUS "  ZeroMQ: ${HPPLITE_ENABLE_ZMQ}")
message(STATUS "  Build tests: ${HPPLITE_BUILD_TESTS}")
message(STATUS "")
