cmake_minimum_required(VERSION 3.14)
project(hpplite VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Options
option(HPPLITE_BUILD_TESTS "Build test executables" ON)
option(HPPLITE_USE_REAL_L1 "Use real Ethereum L1 (vs mock)" OFF)

# Find dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)

# libcurl (for Web3 integration)
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
    crypto.c
    node.c
    da_uri.c
    config.c
    # Web3 integration (eth/ subdirectory)
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

target_link_libraries(hpplite PUBLIC
    sqlite3
    ${SECP256K1_LIBRARIES}
    ${CURL_LIBRARIES}
)

target_link_directories(hpplite PUBLIC
    ${SECP256K1_LIBRARY_DIRS}
    ${CURL_LIBRARY_DIRS}
)

# Test executables
if(HPPLITE_BUILD_TESTS)
    # Include tests directory for test_env.h
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tests)

    # === Unit Tests (no network required) ===
    add_executable(test_hpplite tests/unit/test_hpplite.c)
    target_link_libraries(test_hpplite hpplite)

    add_executable(test_batch tests/unit/test_batch.c)
    target_link_libraries(test_batch hpplite)

    add_executable(test_node tests/unit/test_node.c)
    target_link_libraries(test_node hpplite)

    add_executable(test_da_uri tests/unit/test_da_uri.c)
    target_link_libraries(test_da_uri hpplite)

    add_executable(test_simple_open tests/unit/test_simple_open.c)
    target_link_libraries(test_simple_open hpplite)

    # Tests that use mock L1 - only build when not using real L1
    if(NOT HPPLITE_USE_REAL_L1)
        add_executable(test_l1_mock tests/unit/test_l1_mock.c)
        target_link_libraries(test_l1_mock hpplite)

        add_executable(test_checkpoint_flow tests/unit/test_checkpoint_flow.c)
        target_link_libraries(test_checkpoint_flow hpplite)
    endif()

    # Web3 primitive tests (eth/ subdirectory - no network required)
    add_executable(test_keccak eth/test_keccak.c eth/keccak256.c)
    add_executable(test_rlp eth/test_rlp.c eth/rlp.c)
    add_executable(test_abi eth/test_abi.c eth/abi.c eth/keccak256.c)

    # === Integration Tests (require real L1 + network + HPPLITE_PRIVATE_KEY) ===
    if(HPPLITE_USE_REAL_L1)
        add_executable(test_eth_client eth/test_eth_client.c)
        target_link_libraries(test_eth_client hpplite)

        add_executable(test_l1_primitives tests/integration/test_l1_primitives.c)
        target_link_libraries(test_l1_primitives hpplite)

        add_executable(test_single_node tests/integration/test_single_node.c)
        target_link_libraries(test_single_node hpplite)
    endif()

    # === CTest Registration ===
    enable_testing()

    add_test(NAME test_hpplite COMMAND test_hpplite)
    add_test(NAME test_batch COMMAND test_batch)
    add_test(NAME test_node COMMAND test_node)
    add_test(NAME test_da_uri COMMAND test_da_uri)
    add_test(NAME test_simple_open COMMAND test_simple_open)
    add_test(NAME test_keccak COMMAND test_keccak)
    add_test(NAME test_rlp COMMAND test_rlp)
    add_test(NAME test_abi COMMAND test_abi)

    # Mock L1 tests - only register when not using real L1
    if(NOT HPPLITE_USE_REAL_L1)
        add_test(NAME test_l1_mock COMMAND test_l1_mock)
        add_test(NAME test_checkpoint_flow COMMAND test_checkpoint_flow)
    endif()
endif()

# Examples (require real L1)
option(HPPLITE_BUILD_EXAMPLES "Build example executables" ${HPPLITE_USE_REAL_L1})
if(HPPLITE_BUILD_EXAMPLES)
    if(NOT HPPLITE_USE_REAL_L1)
        message(FATAL_ERROR "Examples require HPPLITE_USE_REAL_L1=ON")
    endif()

    add_executable(factory_demo examples/factory_demo.c)
    target_link_libraries(factory_demo hpplite)

    add_executable(da_uri_demo examples/da_uri_demo.c)
    target_link_libraries(da_uri_demo hpplite)

    add_executable(basic_sequencer examples/basic_sequencer.c)
    target_link_libraries(basic_sequencer hpplite)

    add_executable(l1_config examples/l1_config.c)
    target_link_libraries(l1_config hpplite)
endif()

# Print configuration
message(STATUS "")
message(STATUS "HPPLite Configuration:")
message(STATUS "  SQLite dir: ${SQLITE_DIR}")
message(STATUS "  secp256k1: ${SECP256K1_LIBRARIES}")
message(STATUS "  libcurl: ${CURL_LIBRARIES}")
message(STATUS "  Real L1: ${HPPLITE_USE_REAL_L1}")
message(STATUS "  Build tests: ${HPPLITE_BUILD_TESTS}")
message(STATUS "  Build examples: ${HPPLITE_BUILD_EXAMPLES}")
message(STATUS "")
