cmake_minimum_required(VERSION 3.14)
project(hpplite VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# glibc hides dladdr()/Dl_info, RTLD_NODELETE and memmem() behind this; a no-op elsewhere
add_compile_definitions(_GNU_SOURCE)

# Optimized by default (single-config generators); -DCMAKE_BUILD_TYPE=Debug to override
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Options
option(HPPLITE_BUILD_TESTS "Build test executables" ON)
option(HPPLITE_USE_REAL_L1 "Use real Ethereum L1 (vs mock)" OFF)
option(HPPLITE_BUILD_EXTENSION "Build the loadable extension (hpplite.so/.dylib)" ON)
set(HPPLITE_SQLITE_SRC "" CACHE PATH
    "Directory holding a stock SQLite amalgamation (sqlite3.c, sqlite3.h, sqlite3ext.h). Default: ../../build of the parent tree")

# Find dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)

# libcurl (for Web3 integration)
pkg_check_modules(CURL REQUIRED libcurl)

# Find threads
find_package(Threads REQUIRED)

# SQLite amalgamation. HPPLite needs no SQLite patches and no non-default
# compile options; any amalgamation >= 3.24 works. By default the one built
# in the parent tree is used (cd build && ../configure && make sqlite3.c).
set(SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
if(HPPLITE_SQLITE_SRC)
    set(SQLITE_BUILD_DIR "${HPPLITE_SQLITE_SRC}")
else()
    set(SQLITE_BUILD_DIR "${SQLITE_DIR}/build")
endif()

if(NOT EXISTS "${SQLITE_BUILD_DIR}/sqlite3.c")
    message(FATAL_ERROR
        "SQLite amalgamation not found at ${SQLITE_BUILD_DIR}/sqlite3.c\n"
        "Either build the parent tree (cd ${SQLITE_DIR}/build && ../configure && make sqlite3.c)\n"
        "or point HPPLITE_SQLITE_SRC at a directory with sqlite3.c/sqlite3.h/sqlite3ext.h"
    )
endif()

# SQLite library (build from amalgamation)
add_library(sqlite3 STATIC
    ${SQLITE_BUILD_DIR}/sqlite3.c
)
target_include_directories(sqlite3 PUBLIC
    ${SQLITE_BUILD_DIR}
)
target_compile_definitions(sqlite3 PUBLIC
    SQLITE_THREADSAFE=1
    # full-text search and R*Tree, as in the SQLite of Homebrew, macOS, Debian and
    # Python's builds: what applications use, row capture records (their tables)
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_RTREE
)
target_link_libraries(sqlite3 PUBLIC
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
# FTS5's ranking uses log(): libm, separate from libc outside Apple
if(NOT APPLE)
    target_link_libraries(sqlite3 PUBLIC m)
endif()

# HPPLite core library
set(HPPLITE_SOURCES
    hpplite.c
    sha256.c
    state_hash.c
    capture.c
    bookkeeping.c
    finalize.c
    replay.c
    status.c
    capture_rows.c
    batch.c
    crypto.c
    node.c
    da_uri.c
    config.c
    cipher.c
    clock.c
    # Web3 integration (eth/ subdirectory)
    eth/cJSON.c
    eth/keccak256.c
    eth/rlp.c
    eth/abi.c
    eth/eth_client.c
)

# L1 backends: every build has both; the endpoint picks one at run time (#35).
# HPPLITE_USE_REAL_L1 now only decides whether the tests and examples that
# need a chain (anvil, HPP Sepolia) are built, and keeps the test hooks out.
list(APPEND HPPLITE_SOURCES l1.c l1_eth.c l1_mock.c l1_dir.c)
if(HPPLITE_USE_REAL_L1)
    message(STATUS "L1 backends: eth + mock; chain tests and examples on")
else()
    add_compile_definitions(HPPLITE_TEST_HOOKS)   # HPPLITE_TEST_CRASH_AT crash points
    message(STATUS "L1 backends: eth + mock; test hooks on")
endif()

add_library(hpplite STATIC ${HPPLITE_SOURCES})

target_include_directories(hpplite PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/eth
    ${SECP256K1_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
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

# Loadable extension: the same sources, every sqlite3_* call routed through
# the host's sqlite3_api_routines (HPPLITE_EXTENSION), no SQLite linked in.
#   sqlite> .load ./hpplite
if(HPPLITE_BUILD_EXTENSION)
    add_library(hpplite_ext MODULE ${HPPLITE_SOURCES})
    set_target_properties(hpplite_ext PROPERTIES OUTPUT_NAME hpplite PREFIX "")
    if(APPLE)
        set_target_properties(hpplite_ext PROPERTIES SUFFIX ".dylib")
    endif()
    target_include_directories(hpplite_ext PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/eth
        ${SQLITE_BUILD_DIR}
        ${SECP256K1_INCLUDE_DIRS}
        ${CURL_INCLUDE_DIRS}
    )
    target_compile_definitions(hpplite_ext PRIVATE HPPLITE_EXTENSION)
    target_link_libraries(hpplite_ext PRIVATE
        Threads::Threads
        ${SECP256K1_LIBRARIES}
        ${CURL_LIBRARIES}
    )
    target_link_directories(hpplite_ext PRIVATE
        ${SECP256K1_LIBRARY_DIRS}
        ${CURL_LIBRARY_DIRS}
    )
endif()

# Test executables
# The sidecar sequencer for apps with short-lived connections (PHP, CGI)
add_executable(hpplite-sequencer tools/hpplite_sequencer.c)
target_link_libraries(hpplite-sequencer hpplite)
add_executable(hpplite-verify tools/hpplite_verify.c)
target_link_libraries(hpplite-verify hpplite)

if(HPPLITE_BUILD_TESTS)
    # Include tests directory for test_env.h
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tests)

    # The test library (#106): the TEST macros, the scratch directory, the node
    # open helper, the row dump and the replica check every suite used to carry
    # a copy of - tests/unit/test_lib.h
    add_library(hpplite_testlib STATIC tests/unit/test_lib.c)
    target_link_libraries(hpplite_testlib hpplite)
    target_include_directories(hpplite_testlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)

    # === Unit Tests (no network required) ===
    add_executable(test_hpplite tests/unit/test_hpplite.c)
    target_link_libraries(test_hpplite hpplite_testlib)

    add_executable(test_batch tests/unit/test_batch.c)
    target_link_libraries(test_batch hpplite_testlib)

    add_executable(test_node tests/unit/test_node.c)
    target_link_libraries(test_node hpplite)

    add_executable(test_da_uri tests/unit/test_da_uri.c)
    target_link_libraries(test_da_uri hpplite)

    add_executable(test_l1_dispatch tests/unit/test_l1_dispatch.c)
    target_link_libraries(test_l1_dispatch hpplite)

    add_executable(test_l1_dir tests/unit/test_l1_dir.c)
    target_link_libraries(test_l1_dir hpplite_testlib)

    add_executable(test_simple_open tests/unit/test_simple_open.c)
    target_link_libraries(test_simple_open hpplite)

    add_executable(test_cipher tests/unit/test_cipher.c)
    target_link_libraries(test_cipher hpplite_testlib)

    # Tests that use mock L1 - only build when not using real L1
    if(NOT HPPLITE_USE_REAL_L1)
        add_executable(test_l1_mock tests/unit/test_l1_mock.c)
        target_link_libraries(test_l1_mock hpplite)

        add_executable(test_checkpoint_flow tests/unit/test_checkpoint_flow.c)
        target_link_libraries(test_checkpoint_flow hpplite_testlib)
        add_executable(test_durability tests/unit/test_durability.c)
        target_link_libraries(test_durability hpplite_testlib)
        add_executable(test_faults tests/unit/test_faults.c)
        target_link_libraries(test_faults hpplite_testlib)
        add_executable(test_keys tests/unit/test_keys.c)
        target_link_libraries(test_keys hpplite_testlib)
        add_executable(test_abuse tests/unit/test_abuse.c)
        target_link_libraries(test_abuse hpplite_testlib)
        add_executable(test_foreign_keys tests/unit/test_foreign_keys.c)
        target_link_libraries(test_foreign_keys hpplite_testlib)
        add_executable(test_big_txn tests/unit/test_big_txn.c)
        target_link_libraries(test_big_txn hpplite_testlib)
        add_executable(test_clock tests/unit/test_clock.c)
        target_link_libraries(test_clock hpplite_testlib)
        add_executable(test_env_enable tests/unit/test_env_enable.c)
        target_link_libraries(test_env_enable hpplite_testlib)
        add_executable(test_reorg tests/unit/test_reorg.c)
        target_link_libraries(test_reorg hpplite_testlib)
        add_executable(test_big_stmt tests/unit/test_big_stmt.c)
        target_link_libraries(test_big_stmt hpplite_testlib)
        add_executable(test_concurrency tests/unit/test_concurrency.c)
        target_link_libraries(test_concurrency hpplite_testlib)
        add_executable(test_rows tests/unit/test_rows.c)
        target_link_libraries(test_rows hpplite_testlib)
        add_executable(test_rows_e2e tests/unit/test_rows_e2e.c)
        target_link_libraries(test_rows_e2e hpplite_testlib)

        # Microbenchmark (not a test): `make bench` or ./bench_hpplite [--rows N] [--mb 1,10,50]
        add_executable(bench_hpplite tests/bench/bench_hpplite.c)
        target_link_libraries(bench_hpplite hpplite)
        add_custom_target(bench COMMAND bench_hpplite DEPENDS bench_hpplite USES_TERMINAL)

        # Stress test (not a test): 100 MB through encryption, DA chunking, checkpoints, reconstruction
        add_executable(stress_hpplite tests/stress/stress_hpplite.c)
        target_link_libraries(stress_hpplite hpplite)
        add_custom_target(stress COMMAND stress_hpplite DEPENDS stress_hpplite USES_TERMINAL)
        # Crash injection: kill -9 at deterministic and random points, reopen, check (issue #26)
        add_executable(crash_hpplite tests/stress/crash_hpplite.c)
        target_link_libraries(crash_hpplite hpplite)
    endif()

    # Loadable extension against stock SQLite: links only the amalgamation,
    # loads hpplite.so/.dylib at runtime
    if(HPPLITE_BUILD_EXTENSION)
        add_executable(test_loadable tests/unit/test_loadable.c)
        target_link_libraries(test_loadable sqlite3)
        target_compile_definitions(test_loadable PRIVATE
            HPPLITE_EXT_PATH="$<TARGET_FILE:hpplite_ext>")
        add_dependencies(test_loadable hpplite_ext)
    endif()

    # Web3 primitive tests (eth/ subdirectory - no network required)
    add_executable(test_keccak eth/test_keccak.c eth/keccak256.c)
    add_executable(test_rlp eth/test_rlp.c eth/rlp.c)
    add_executable(test_abi eth/test_abi.c eth/abi.c eth/keccak256.c)
    add_executable(test_tx_encoding eth/test_tx_encoding.c)
    target_link_libraries(test_tx_encoding hpplite)

    # === Integration Tests (require real L1 + network + HPPLITE_PRIVATE_KEY) ===
    if(HPPLITE_USE_REAL_L1)
        add_executable(test_eth_client eth/test_eth_client.c)
        target_link_libraries(test_eth_client hpplite)

        add_executable(test_l1_primitives tests/integration/test_l1_primitives.c)
        target_link_libraries(test_l1_primitives hpplite)

        add_executable(test_single_node tests/integration/test_single_node.c)
        target_link_libraries(test_single_node hpplite)

        # Real contracts on a local anvil chain (issue #47): free, deterministic, in ctest
        add_executable(test_anvil tests/l1/test_anvil.c)
        target_link_libraries(test_anvil hpplite_testlib)
        target_include_directories(test_anvil PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME test_anvil COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/l1/anvil_test.sh $<TARGET_FILE:test_anvil>)
        set_tests_properties(test_anvil PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    endif()

    # === CTest Registration ===
    enable_testing()

    add_test(NAME test_hpplite COMMAND test_hpplite)
    add_test(NAME test_batch COMMAND test_batch)
    add_test(NAME test_node COMMAND test_node)
    add_test(NAME test_da_uri COMMAND test_da_uri)
    add_test(NAME test_l1_dispatch COMMAND test_l1_dispatch)
    add_test(NAME test_l1_dir COMMAND test_l1_dir $<TARGET_FILE:hpplite-verify>)
    add_test(NAME test_simple_open COMMAND test_simple_open)
    add_test(NAME test_cipher COMMAND test_cipher)
    add_test(NAME test_keccak COMMAND test_keccak)
    add_test(NAME test_rlp COMMAND test_rlp)
    add_test(NAME test_abi COMMAND test_abi)
    add_test(NAME test_tx_encoding COMMAND test_tx_encoding)
    # The RPC fault proxy of the anvil suite, alone: every fault kind a mode lists happens (#127)
    find_program(HPPLITE_PYTHON3 python3)
    if(HPPLITE_PYTHON3)
        add_test(NAME test_rpc_proxy COMMAND ${HPPLITE_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/l1/test_rpc_proxy.py
                                           ${CMAKE_CURRENT_SOURCE_DIR}/tests/l1/rpc_proxy.py)
    endif()
    if(HPPLITE_BUILD_EXTENSION)
        add_test(NAME test_loadable COMMAND test_loadable)
        # Python's sqlite3 module and the sqlite3 shell driving the module (issue #27)
        add_test(NAME smoke_python_cli
                 COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke/smoke.sh $<TARGET_FILE:hpplite_ext>)
        set_tests_properties(smoke_python_cli PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME smoke_python_cli_text
                 COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke/smoke.sh $<TARGET_FILE:hpplite_ext>)
        set_tests_properties(smoke_python_cli_text PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT HPPLITE_CAPTURE=text)
        set_tests_properties(smoke_python_cli smoke_python_cli_text PROPERTIES RESOURCE_LOCK smoke)
        if(CMAKE_C_FLAGS MATCHES "sanitize")
            # an instrumented module cannot be loaded into a stock python/sqlite3 process
            set_tests_properties(smoke_python_cli smoke_python_cli_text PROPERTIES DISABLED TRUE)
        endif()
    endif()

    # Mock L1 tests - only register when not using real L1
    if(NOT HPPLITE_USE_REAL_L1)
        add_test(NAME test_l1_mock COMMAND test_l1_mock)
        add_test(NAME test_checkpoint_flow COMMAND test_checkpoint_flow)
        add_test(NAME test_durability COMMAND test_durability)
        add_test(NAME test_faults COMMAND test_faults)
        add_test(NAME test_keys COMMAND test_keys)
        add_test(NAME test_abuse COMMAND test_abuse)
        add_test(NAME test_foreign_keys COMMAND test_foreign_keys)
        add_test(NAME test_big_txn COMMAND test_big_txn)
        add_test(NAME test_clock COMMAND test_clock)
        add_test(NAME test_env_enable COMMAND test_env_enable)
        add_test(NAME test_reorg COMMAND test_reorg)
        add_test(NAME test_big_stmt COMMAND test_big_stmt)
        add_test(NAME test_concurrency COMMAND test_concurrency)
        add_test(NAME test_rows COMMAND test_rows)
        add_test(NAME test_rows_e2e COMMAND test_rows_e2e)
        add_test(NAME test_crash COMMAND crash_hpplite)

        # Every suite above again with statement capture (capture=text, v0.1's;
        # rows are the default, #19): the cases about statement text run there
        foreach(t test_checkpoint_flow test_durability test_faults test_keys test_abuse test_foreign_keys
                  test_big_txn test_clock test_reorg test_big_stmt test_concurrency)
            add_test(NAME ${t}_text COMMAND ${t})
            # the same scratch directory: never at once (ctest -j)
            set_tests_properties(${t} ${t}_text PROPERTIES RESOURCE_LOCK ${t})
            set_tests_properties(${t}_text PROPERTIES ENVIRONMENT HPPLITE_CAPTURE=text)
        endforeach()
        add_test(NAME test_crash_text COMMAND crash_hpplite)
        # the same harness on the directory backend (l1=file://, #35)
        add_test(NAME test_crash_dir COMMAND crash_hpplite --l1 dir)
        add_test(NAME test_crash_dir_text COMMAND crash_hpplite --l1 dir)
        set_tests_properties(test_crash test_crash_text test_crash_dir test_crash_dir_text PROPERTIES RESOURCE_LOCK test_crash)
        set_tests_properties(test_crash_text test_crash_dir_text PROPERTIES ENVIRONMENT HPPLITE_CAPTURE=text)

    # Docs must name the same factory address as the hpp-sepolia alias in da_uri.c
    add_test(NAME docs_factory_address
             COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_docs.sh ${CMAKE_CURRENT_SOURCE_DIR})
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
message(STATUS "  SQLite amalgamation: ${SQLITE_BUILD_DIR}")
message(STATUS "  Loadable extension: ${HPPLITE_BUILD_EXTENSION}")
message(STATUS "  secp256k1: ${SECP256K1_LIBRARIES}")
message(STATUS "  libcurl: ${CURL_LIBRARIES}")
message(STATUS "  Real L1: ${HPPLITE_USE_REAL_L1}")
message(STATUS "  Build tests: ${HPPLITE_BUILD_TESTS}")
message(STATUS "  Build examples: ${HPPLITE_BUILD_EXAMPLES}")
message(STATUS "")
