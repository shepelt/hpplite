cmake_minimum_required(VERSION 3.14)
project(hpplite VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Options
option(HPPLITE_BUILD_TESTS "Build test executables" ON)
option(HPPLITE_ENABLE_ZMQ "Enable ZeroMQ networking (M2+)" ON)
option(HPPLITE_USE_REAL_L1 "Use real Ethereum L1 (vs mock)" OFF)

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

# libcurl (for M3 Web3 integration)
pkg_check_modules(CURL REQUIRED libcurl)

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
    da_uri.c
    config.c
    # M3 Web3 integration (eth/ subdirectory)
    eth/cJSON.c
    eth/keccak256.c
    eth/rlp.c
    eth/abi.c
    eth/eth_client.c
)

# L1 backend selection (mock vs real Ethereum)
if(HPPLITE_USE_REAL_L1)
    list(APPEND HPPLITE_SOURCES l1_eth.c)
    message(STATUS "L1 backend: Real Ethereum (l1_eth.c)")
else()
    list(APPEND HPPLITE_SOURCES l1_mock.c)
    message(STATUS "L1 backend: Mock (l1_mock.c)")
endif()

# Add ZMQ networking source if enabled
if(HPPLITE_ENABLE_ZMQ)
    list(APPEND HPPLITE_SOURCES zmq_transport.c l1_service.c)
endif()

add_library(hpplite STATIC ${HPPLITE_SOURCES})

target_include_directories(hpplite PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/eth
    ${SECP256K1_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
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
    ${CURL_LIBRARIES}
)

# Add ZMQ libraries
if(HPPLITE_ENABLE_ZMQ)
    target_link_libraries(hpplite PUBLIC ${ZMQ_LIBRARIES})
    target_link_directories(hpplite PUBLIC ${ZMQ_LIBRARY_DIRS})
endif()

target_link_directories(hpplite PUBLIC
    ${SECP256K1_LIBRARY_DIRS}
    ${CURL_LIBRARY_DIRS}
)

# Test executables
if(HPPLITE_BUILD_TESTS)
    # Basic state tracking test
    add_executable(test_hpplite test_hpplite.c)
    target_link_libraries(test_hpplite hpplite)

    # Batch serialization test
    add_executable(test_batch test_batch.c)
    target_link_libraries(test_batch hpplite)

    # Full node test
    add_executable(test_node test_node.c)
    target_link_libraries(test_node hpplite)

    # L1 mock test
    add_executable(test_l1_mock test_l1_mock.c)
    target_link_libraries(test_l1_mock hpplite)

    # Checkpoint flow test (sequencer + witnesses, single process)
    add_executable(test_checkpoint_flow test_checkpoint_flow.c)
    target_link_libraries(test_checkpoint_flow hpplite)

    # M2 multi-process test (requires ZMQ)
    if(HPPLITE_ENABLE_ZMQ)
        add_executable(test_m2_multiprocess test_m2_multiprocess.c)
        target_link_libraries(test_m2_multiprocess hpplite Threads::Threads)

        # M2 node-to-node ZMQ test
        add_executable(test_m2_nodes test_m2_nodes.c)
        target_link_libraries(test_m2_nodes hpplite Threads::Threads)
    endif()

    # M3 Web3 tests (eth/ subdirectory)
    add_executable(test_keccak eth/test_keccak.c eth/keccak256.c)

    add_executable(test_rlp eth/test_rlp.c eth/rlp.c)

    add_executable(test_abi eth/test_abi.c eth/abi.c eth/keccak256.c)

    # M3 eth_client test (requires network)
    add_executable(test_eth_client eth/test_eth_client.c)
    target_link_libraries(test_eth_client hpplite)

    # M3 L1 Ethereum client test (requires network)
    add_executable(test_l1_eth test_l1_eth.c)
    target_link_libraries(test_l1_eth hpplite)

    # M3 E2E test (requires network and deployed contract)
    add_executable(test_m3_e2e test_m3_e2e.c)
    target_link_libraries(test_m3_e2e hpplite)

    # M3 Checkpoint submission test (requires network)
    add_executable(test_m3_checkpoint test_m3_checkpoint.c)
    target_link_libraries(test_m3_checkpoint hpplite)

    # M3 L2 Reconstruction test (requires network + prior checkpoint data)
    add_executable(test_m3_reconstruct test_m3_reconstruct.c)
    target_link_libraries(test_m3_reconstruct hpplite)

    # M3 Witness attestation test
    add_executable(test_m3_witness test_m3_witness.c)
    target_link_libraries(test_m3_witness hpplite)

    # M3 Multi-node E2E test
    add_executable(test_m3_multinode test_m3_multinode.c)
    target_link_libraries(test_m3_multinode hpplite)

    # DA URI scheme test
    add_executable(test_da_uri test_da_uri.c)
    target_link_libraries(test_da_uri hpplite)

    # Simple open API test
    add_executable(test_simple_open test_simple_open.c)
    target_link_libraries(test_simple_open hpplite)

    # Add tests to CTest
    enable_testing()
    add_test(NAME test_hpplite COMMAND test_hpplite)
    add_test(NAME test_batch COMMAND test_batch)
    add_test(NAME test_node COMMAND test_node)
    add_test(NAME test_l1_mock COMMAND test_l1_mock)
    add_test(NAME test_checkpoint_flow COMMAND test_checkpoint_flow)
    if(HPPLITE_ENABLE_ZMQ)
        add_test(NAME test_m2_multiprocess COMMAND test_m2_multiprocess)
        add_test(NAME test_m2_nodes COMMAND test_m2_nodes)
    endif()

    # M3 Web3 tests (no network required)
    add_test(NAME test_keccak COMMAND test_keccak)
    add_test(NAME test_rlp COMMAND test_rlp)
    add_test(NAME test_abi COMMAND test_abi)
    add_test(NAME test_da_uri COMMAND test_da_uri)
    add_test(NAME test_simple_open COMMAND test_simple_open)
    # Note: test_eth_client requires network, not in ctest
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
message(STATUS "  libcurl: ${CURL_LIBRARIES}")
message(STATUS "  ZeroMQ: ${HPPLITE_ENABLE_ZMQ}")
message(STATUS "  Real L1: ${HPPLITE_USE_REAL_L1}")
message(STATUS "  Build tests: ${HPPLITE_BUILD_TESTS}")
message(STATUS "")
