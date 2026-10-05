cmake_minimum_required(VERSION 3.21)
project(cpb_host_tests LANGUAGES CXX)

# Standalone native build: do not import the Pico toolchain or firmware options.
# Match the previous g++ commands, which specified optimization per test.
set(CMAKE_BUILD_TYPE "" CACHE STRING "Host flags are set per test" FORCE)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-ffp-contract=off)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_program(CPB_CCACHE ccache REQUIRED)
set(CMAKE_CXX_COMPILER_LAUNCHER "${CPB_CCACHE}")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(CPB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(CPB_ASSERT_GUARD "${CMAKE_CURRENT_SOURCE_DIR}/assertions_enabled.hpp")
file(GLOB CPB_EXAMPLES CONFIGURE_DEPENDS "${CPB_ROOT}/examples/*.bas")
if(NOT CPB_EXAMPLES)
    message(FATAL_ERROR "No BASIC example fixtures found")
endif()
enable_testing()

# Keep test execution sequential even if a caller supplies ctest -j later.
# Explicit dependencies preserve the old execution order across repeated runs.
function(cpb_register_test name)
    add_test(NAME "${name}" COMMAND ${ARGN})
    set_tests_properties("${name}" PROPERTIES
        WORKING_DIRECTORY "${CPB_ROOT}" RUN_SERIAL TRUE TIMEOUT 120)
    get_property(previous GLOBAL PROPERTY CPB_PREVIOUS_TEST)
    if(previous)
        set_tests_properties("${name}" PROPERTIES DEPENDS "${previous}")
    endif()
    set_property(GLOBAL PROPERTY CPB_PREVIOUS_TEST "${name}")
endfunction()

function(cpb_test name)
    cmake_parse_arguments(T "NO_LEAKS;EXAMPLES" "" "SOURCES;INCLUDES;FLAGS;LINK;DEFINES;ARGS" ${ARGN})
    if(T_UNPARSED_ARGUMENTS OR NOT T_SOURCES)
        message(FATAL_ERROR "Invalid host test definition: ${name}")
    endif()
    set(sources)
    foreach(source IN LISTS T_SOURCES)
        list(APPEND sources "${CPB_ROOT}/${source}")
    endforeach()
    add_executable("${name}" ${sources})
    foreach(directory IN LISTS T_INCLUDES)
        target_include_directories("${name}" PRIVATE "${CPB_ROOT}/${directory}")
    endforeach()
    # Undefine NDEBUG last, and fail compilation if assertions are disabled.
    target_compile_options("${name}" PRIVATE ${T_FLAGS} -UNDEBUG -include "${CPB_ASSERT_GUARD}")
    target_compile_definitions("${name}" PRIVATE ${T_DEFINES})
    # Preserve sanitizer, pthread and linker-wrap flags from the original driver invocation.
    target_link_options("${name}" PRIVATE ${T_FLAGS} ${T_LINK})
    if(T_EXAMPLES)
        cpb_register_test("${name}" "$<TARGET_FILE:${name}>" ${CPB_EXAMPLES})
    else()
        cpb_register_test("${name}" "$<TARGET_FILE:${name}>" ${T_ARGS})
    endif()
    if(T_NO_LEAKS)
        set_tests_properties("${name}" PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
    endif()
endfunction()

set(SAN -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
set(WARN -Wall -Wextra)
set(SECTIONS -ffunction-sections -fdata-sections)

option(CPB_BENCHMARK_ONLY "Configure only benchmark targets in historical evidence checkouts" OFF)
if(NOT CPB_BENCHMARK_ONLY)
cpb_test(editor-workflow-model-test
    SOURCES tests/editor_workflow_model_test.cpp src/core/editor_model.cpp
    INCLUDES include FLAGS ${SAN} ${WARN} -Werror)
cpb_test(editor-workflow-integration-test
    SOURCES tests/editor_workflow_integration_test.cpp tests/runtime_stubs/editor_workflow_ui.cpp src/core/transfer_file.cpp tests/runtime_stubs/bluetooth_hid_keyboard.cpp tests/runtime_stubs/wireless.cpp tests/program_psram_stubs.cpp src/core/line_editor.cpp src/core/repl.cpp tests/runtime_stubs/network_operation.cpp src/core/editor_model.cpp src/core/psram_editor_history.cpp src/core/full_screen_editor.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp src/core/ymodem.cpp src/core/xmodem.cpp
    INCLUDES tests/runtime_stubs include FLAGS ${SAN} ${SECTIONS} LINK -Wl,--gc-sections)


cpb_test(editor-structure-test
    SOURCES tests/editor_structure_test.cpp src/core/editor_model.cpp
    INCLUDES include FLAGS ${SAN} ${WARN} -Werror)
cpb_test(editor-autoindent-test
    SOURCES tests/editor_autoindent_test.cpp src/core/editor_model.cpp
    INCLUDES include FLAGS ${SAN} ${WARN} -Werror)
cpb_test(file-path-test
    SOURCES tests/file_path_test.cpp src/core/file_management.cpp src/core/safe_file.cpp src/core/transfer_file.cpp
    INCLUDES include FLAGS ${SAN} ${WARN} -Werror)
cpb_test(program-pair-test
    SOURCES tests/program_pair_test.cpp tests/program_psram_stubs.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/file_management.cpp
    INCLUDES include FLAGS ${SAN} ${WARN}
    LINK -Wl,--wrap=fwrite,--wrap=rename)
cpb_register_test(stage1-source-contract
    "${Python3_EXECUTABLE}" "${CPB_ROOT}/tests/stage1_source_contract_test.py")
cpb_register_test(power-transition-test
    "${Python3_EXECUTABLE}" "${CPB_ROOT}/tests/power_transition_test.py")
cpb_register_test(audio-output-lifecycle-test
    "${Python3_EXECUTABLE}" "${CPB_ROOT}/tests/audio_output_lifecycle_test.py")

# All 31 native tests from build.yml at 93793f2, with original flags/stubs.
cpb_test(status-layout-test
    SOURCES tests/status_layout_test.cpp tests/runtime_stubs/psram.cpp src/core/program_store.cpp src/core/file_management.cpp src/core/safe_file.cpp src/core/il.cpp src/core/ymodem.cpp src/core/xmodem.cpp
    INCLUDES include FLAGS ${SECTIONS} LINK -Wl,--gc-sections)
cpb_test(system-controls-test
    SOURCES tests/system_controls_test.cpp INCLUDES include FLAGS ${WARN} -Werror)
cpb_test(display-scroll-test
    SOURCES tests/display_scroll_test.cpp INCLUDES tests/stubs include)
cpb_test(prompt-boundary-test
    SOURCES tests/prompt_boundary_test.cpp src/core/line_editor.cpp INCLUDES include)
cpb_test(xmodem-test NO_LEAKS
    SOURCES tests/xmodem_test.cpp src/core/xmodem.cpp src/core/safe_file.cpp src/core/transfer_file.cpp
    INCLUDES include FLAGS ${SAN} ${WARN})
cpb_test(ymodem-test NO_LEAKS
    SOURCES tests/ymodem_test.cpp src/core/ymodem.cpp src/core/xmodem.cpp src/core/safe_file.cpp src/core/transfer_file.cpp
    INCLUDES include FLAGS ${SAN} ${WARN})
cpb_test(file-transfer-menu-test
    SOURCES tests/file_transfer_menu_test.cpp INCLUDES include FLAGS ${WARN})
cpb_test(menu-scroll-test
    SOURCES tests/menu_scroll_test.cpp INCLUDES include FLAGS ${WARN})
cpb_test(command-history-test
    SOURCES tests/command_history_test.cpp INCLUDES include FLAGS ${WARN} -Werror)
cpb_test(file-management-test
    SOURCES tests/file_management_test.cpp src/core/file_management.cpp src/core/safe_file.cpp
    INCLUDES include FLAGS ${WARN} -Werror)
cpb_test(editor-model-test
    SOURCES tests/editor_model_test.cpp src/core/editor_model.cpp INCLUDES include FLAGS ${WARN} -Werror)
cpb_test(psram-layout-test
    SOURCES tests/psram_layout_test.cpp INCLUDES include FLAGS ${WARN} -Werror)
cpb_test(psram-allocator-test
    SOURCES tests/psram_allocator_test.cpp INCLUDES include FLAGS ${WARN} -Werror)
cpb_test(compiled-cache-test NO_LEAKS
    SOURCES tests/compiled_cache_test.cpp tests/program_psram_stubs.cpp src/core/compiled_cache.cpp src/core/il.cpp
    INCLUDES include FLAGS ${SAN} ${WARN} -Werror)
cpb_test(psram-editor-history-test
    SOURCES tests/psram_editor_history_test.cpp src/core/psram_editor_history.cpp INCLUDES include FLAGS ${WARN} -Werror)
cpb_test(usb-storage-menu-test
    SOURCES tests/usb_storage_menu_test.cpp INCLUDES include FLAGS ${WARN})
cpb_test(http-core-test
    SOURCES tests/http_file_server_core_test.cpp src/core/http_file_server_core.cpp src/core/safe_file.cpp
    INCLUDES include FLAGS ${WARN})
cpb_test(safe-file-test
    SOURCES tests/safe_file_test.cpp src/core/safe_file.cpp INCLUDES include FLAGS ${WARN})
cpb_test(serial-route-test NO_LEAKS
    SOURCES tests/serial_route_test.cpp src/platform/picocalc/serial_transfer.cpp
    INCLUDES tests/serial_stubs include FLAGS ${SAN} ${WARN})
cpb_test(graphics-text-test
    SOURCES tests/graphics_text_test.cpp src/core/graphics_text.cpp INCLUDES include FLAGS ${WARN})
cpb_test(audio-engine-test
    SOURCES tests/audio_engine_test.cpp src/core/audio_engine.cpp INCLUDES include FLAGS ${WARN})
cpb_test(rtc-codec-test
    SOURCES tests/rtc_codec_test.cpp INCLUDES include FLAGS ${WARN})
cpb_test(key-click-test
    SOURCES tests/key_click_test.cpp INCLUDES include FLAGS ${WARN})
cpb_test(bluetooth-hid-keyboard-core-test
    SOURCES tests/bluetooth_hid_keyboard_core_test.cpp src/core/bluetooth_hid_keyboard_core.cpp
    INCLUDES include FLAGS ${SAN} ${WARN})
cpb_test(bluetooth-device-registry-test
    SOURCES tests/bluetooth_device_registry_test.cpp src/core/bluetooth_device_registry.cpp
    INCLUDES include FLAGS ${SAN} ${WARN})

# Keep the source guard and both existing Python regressions in the test inventory.
cpb_register_test(removed-bluetooth-transport
    "${Python3_EXECUTABLE}" "${CPB_ROOT}/tools/ci_host_tests.py" --check-removed-transport)
cpb_register_test(ble-security-recovery
    "${Python3_EXECUTABLE}" "${CPB_ROOT}/tests/bluetooth_hid_ble_recovery_test.py")
cpb_test(bluetooth-registry-storage-test
    SOURCES tests/bluetooth_registry_storage_test.cpp src/core/bluetooth_device_registry.cpp src/platform/picocalc/bluetooth_device_registry_storage.cpp
    INCLUDES tests/ble_stubs include FLAGS ${SAN} ${WARN} -Werror)
cpb_test(runtime-test EXAMPLES
    SOURCES tests/runtime_workspace_test.cpp tests/runtime_stubs/bluetooth_hid_keyboard.cpp tests/runtime_stubs/wireless.cpp tests/program_psram_stubs.cpp src/core/repl.cpp tests/runtime_stubs/network_operation.cpp src/core/editor_model.cpp src/core/psram_editor_history.cpp src/core/full_screen_editor.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp src/core/ymodem.cpp src/core/xmodem.cpp
    INCLUDES tests/runtime_stubs include FLAGS ${SAN} ${SECTIONS} LINK -Wl,--gc-sections)
cpb_test(program-storage-test EXAMPLES
    SOURCES tests/program_storage_test.cpp tests/program_psram_stubs.cpp src/core/program_store.cpp src/core/file_management.cpp src/core/safe_file.cpp src/core/transfer_file.cpp src/core/basic_compiler.cpp src/core/il.cpp
    INCLUDES include FLAGS ${SAN}
    LINK -Wl,--wrap=fwrite,--wrap=fclose,--wrap=rename,--wrap=ftell,--wrap=remove)
cpb_test(program-session-test
    SOURCES tests/program_session_test.cpp tests/runtime_stubs/psram.cpp src/core/program_store.cpp src/core/file_management.cpp src/core/safe_file.cpp
    INCLUDES include FLAGS ${SAN} ${WARN})
cpb_register_test(ble-report-sdk-parser
    "${Python3_EXECUTABLE}" "${CPB_ROOT}/tests/bluetooth_hid_ble_report_test.py" "${CPB_ROOT}/pico-sdk/lib/btstack")
cpb_test(usb-msc-test NO_LEAKS
    SOURCES tests/usb_msc_test.cpp src/platform/picocalc/usb_device.cpp src/platform/picocalc/usb_msc.cpp src/platform/picocalc/usb_descriptors.cpp
    INCLUDES tests/usb_stubs pico-sdk/lib/tinyusb/src pico-sdk/src/rp2_common/pico_usb_reset/include pico-sdk/src/common/pico_usb_reset_interface_headers/include include
    FLAGS ${SAN} ${WARN}
    DEFINES __unused= PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=1 PICO_USB_RESET_INCLUDE_DEFAULT_APP_DRIVER_CB=1 PICO_USB_RESET_SUPPORT_MS_OS_20_DESCRIPTOR=1 PICO_USB_RESET_MS_OS_20_DESCRIPTOR_ITF=3)
cpb_test(storage-ownership-test NO_LEAKS
    SOURCES tests/storage_ownership_test.cpp src/platform/picocalc/storage.cpp src/core/image_io.cpp
    INCLUDES tests/storage_stubs include FLAGS ${SAN} ${WARN} -pthread ${SECTIONS} LINK -Wl,--gc-sections)

cpb_test(structured-source-test
    SOURCES tests/structured_source_test.cpp tests/program_psram_stubs.cpp src/core/program_store.cpp src/core/editor_model.cpp src/core/psram_editor_history.cpp src/core/safe_file.cpp src/core/file_management.cpp
    INCLUDES include FLAGS ${SAN} ${WARN}
    LINK -Wl,--wrap=fwrite,--wrap=rename)

cpb_test(path-compaction-test SOURCES tests/path_compaction_test.cpp INCLUDES include FLAGS ${WARN} -Werror)

cpb_test(files-ui-integration-test
    SOURCES tests/files_ui_integration_test.cpp tests/runtime_stubs/bluetooth_hid_keyboard.cpp tests/runtime_stubs/wireless.cpp tests/program_psram_stubs.cpp src/core/repl.cpp tests/runtime_stubs/network_operation.cpp src/core/editor_model.cpp src/core/psram_editor_history.cpp src/core/full_screen_editor.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp src/core/ymodem.cpp src/core/xmodem.cpp
    INCLUDES tests/runtime_stubs include FLAGS ${SAN} ${SECTIONS} LINK -Wl,--gc-sections)

cpb_test(image-io-test
    SOURCES tests/image_io_test.cpp src/platform/picocalc/storage.cpp src/core/image_io.cpp
    INCLUDES tests/storage_stubs include FLAGS ${SAN} ${WARN} -pthread ${SECTIONS}
    LINK -Wl,--gc-sections,--wrap=fopen)
cpb_test(image-io-resources-test
    SOURCES tests/image_io_test.cpp src/platform/picocalc/storage.cpp src/core/image_io.cpp
    INCLUDES tests/storage_stubs include FLAGS -O2 ${WARN} -pthread ${SECTIONS}
    LINK -Wl,--gc-sections,--wrap=fopen)
cpb_test(image-statement-test
    SOURCES tests/image_statement_test.cpp tests/runtime_stubs/editor_workflow_ui.cpp src/core/transfer_file.cpp tests/program_psram_stubs.cpp src/core/repl.cpp tests/runtime_stubs/network_operation.cpp tests/runtime_stubs/bluetooth_hid_keyboard.cpp tests/runtime_stubs/wireless.cpp src/core/editor_model.cpp src/core/psram_editor_history.cpp src/core/full_screen_editor.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp src/core/ymodem.cpp src/core/xmodem.cpp
    INCLUDES tests/runtime_stubs include FLAGS ${SAN} -fsanitize=float-cast-overflow ${SECTIONS}
    DEFINES RMB_OPTIMIZER_TEST=1 LINK -Wl,--gc-sections)
cpb_test(error-diagnostics-test
    SOURCES tests/error_diagnostics_test.cpp tests/runtime_stubs/editor_workflow_ui.cpp src/core/transfer_file.cpp tests/program_psram_stubs.cpp src/core/repl.cpp tests/runtime_stubs/network_operation.cpp tests/runtime_stubs/bluetooth_hid_keyboard.cpp tests/runtime_stubs/wireless.cpp src/core/editor_model.cpp src/core/psram_editor_history.cpp src/core/full_screen_editor.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp src/core/ymodem.cpp src/core/xmodem.cpp
    INCLUDES tests/runtime_stubs include FLAGS ${SAN} -fsanitize=float-cast-overflow ${SECTIONS}
    DEFINES RMB_OPTIMIZER_TEST=1 LINK -Wl,--gc-sections)

cpb_test(system-information-test SOURCES tests/system_information_test.cpp INCLUDES include FLAGS ${SAN} ${WARN} -Werror)
cpb_test(system-information-integration-test
    SOURCES tests/system_information_integration_test.cpp tests/runtime_stubs/editor_workflow_ui.cpp tests/runtime_stubs/bluetooth_hid_keyboard.cpp tests/runtime_stubs/wireless.cpp tests/program_psram_stubs.cpp src/core/repl.cpp tests/runtime_stubs/network_operation.cpp src/core/editor_model.cpp src/core/psram_editor_history.cpp src/core/full_screen_editor.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp src/core/ymodem.cpp src/core/xmodem.cpp
    INCLUDES tests/runtime_stubs include FLAGS ${SAN} ${SECTIONS} LINK -Wl,--gc-sections)

cpb_test(math-intops-test
    SOURCES tests/math_intops_test.cpp tests/program_psram_stubs.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp
    INCLUDES tests/runtime_stubs include FLAGS ${SAN} -fsanitize=float-cast-overflow ${SECTIONS}
    DEFINES RMB_OPTIMIZER_TEST=1 LINK -Wl,--gc-sections)

cpb_test(exit-loop-test
    SOURCES tests/exit_loop_test.cpp tests/program_psram_stubs.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp
    INCLUDES tests/runtime_stubs include FLAGS ${SAN} -fsanitize=float-cast-overflow ${SECTIONS}
    DEFINES RMB_OPTIMIZER_TEST=1 LINK -Wl,--gc-sections)

cpb_test(stability-language-test
    SOURCES tests/stability_language_test.cpp tests/program_psram_stubs.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp
    INCLUDES tests/runtime_stubs include FLAGS ${SAN} -fsanitize=float-cast-overflow ${SECTIONS}
    DEFINES RMB_OPTIMIZER_TEST=1 LINK -Wl,--gc-sections)
cpb_register_test(release-package-test "${Python3_EXECUTABLE}" "${CPB_ROOT}/tests/release_package_test.py")

cpb_test(daily-functions-test
    SOURCES tests/daily_functions_test.cpp tests/program_psram_stubs.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp
    INCLUDES tests/runtime_stubs include FLAGS ${SAN} -fsanitize=float-cast-overflow ${SECTIONS}
    DEFINES RMB_OPTIMIZER_TEST=1 LINK -Wl,--gc-sections)
cpb_test(program-protection-test
    SOURCES tests/program_protection_test.cpp src/core/line_editor.cpp tests/runtime_stubs/editor_workflow_ui.cpp src/core/transfer_file.cpp tests/program_psram_stubs.cpp src/core/repl.cpp tests/runtime_stubs/network_operation.cpp tests/runtime_stubs/bluetooth_hid_keyboard.cpp tests/runtime_stubs/wireless.cpp src/core/editor_model.cpp src/core/psram_editor_history.cpp src/core/full_screen_editor.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp src/core/ymodem.cpp src/core/xmodem.cpp
    INCLUDES tests/runtime_stubs include FLAGS ${SAN} -fsanitize=float-cast-overflow ${SECTIONS}
    DEFINES RMB_OPTIMIZER_TEST=1 CPB_REAL_LINE_EDITOR=1 LINK -Wl,--gc-sections)

cpb_test(data-read-test
    SOURCES tests/data_read_test.cpp tests/program_psram_stubs.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp
    INCLUDES tests/runtime_stubs include FLAGS ${SAN} -fsanitize=float-cast-overflow ${SECTIONS}
    DEFINES RMB_OPTIMIZER_TEST=1 LINK -Wl,--gc-sections,--wrap=realloc)

cpb_test(select-case-test
    SOURCES tests/select_case_test.cpp tests/program_psram_stubs.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp
    INCLUDES tests/runtime_stubs include FLAGS ${SAN} -fsanitize=float-cast-overflow ${SECTIONS}
    DEFINES RMB_OPTIMIZER_TEST=1 LINK -Wl,--gc-sections)

cpb_test(manual-examples-test
    SOURCES tests/manual_examples_test.cpp tests/program_psram_stubs.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp
    INCLUDES tests/runtime_stubs include FLAGS ${SAN} -fsanitize=float-cast-overflow ${SECTIONS}
    DEFINES RMB_OPTIMIZER_TEST=1 LINK -Wl,--gc-sections)

cpb_test(stage3-optimizer-test
    SOURCES tests/stage3_optimizer_test.cpp tests/program_psram_stubs.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp
    INCLUDES tests/runtime_stubs include FLAGS ${SAN} ${SECTIONS}
    DEFINES RMB_OPTIMIZER_TEST=1 LINK -Wl,--gc-sections,--wrap=calloc,--wrap=free)

endif()

cpb_test(classic-benchmark SOURCES tests/classic_benchmark.cpp FLAGS -O3 ARGS --verify-classic)

cpb_test(performance-benchmark ARGS --verify-only
    SOURCES tests/performance_benchmark.cpp tests/program_psram_stubs.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp
    INCLUDES tests/runtime_stubs include FLAGS -O3 -Werror=return-type ${SECTIONS} LINK -Wl,--gc-sections)

# The shared harness is also copied into frozen Stage 1-3 checkouts, whose
# platform API predates AudioDiagnostics. Keep those APIs and timed code intact.
file(STRINGS "${CPB_ROOT}/include/platform.hpp" CPB_AUDIO_DIAGNOSTICS_DECL
    REGEX "^struct AudioDiagnostics")
if(NOT CPB_AUDIO_DIAGNOSTICS_DECL)
    target_compile_definitions(performance-benchmark PRIVATE CPB_LEGACY_AUDIO_DIAGNOSTICS=1)
endif()

# Canonicalize diagnostic __FILE__ strings in every timing revision. Different
# checkout roots otherwise change host binary rodata/layout despite identical code.
# No timing repetitions, assertions, sanitizers or acceptance thresholds change.
target_compile_options(performance-benchmark PRIVATE "-ffile-prefix-map=${CPB_ROOT}=cpb-source")
# ci_host_tests uses CCACHE_BASEDIR, whereas baseline CMake runs do not.
# ccache rewrites compiler input paths before prefix mapping; bypass it for the
# timing target only so every revision receives the identical canonical strings.
set_property(TARGET performance-benchmark PROPERTY CXX_COMPILER_LAUNCHER "")

add_dependencies(classic-benchmark performance-benchmark)

cpb_register_test(paired-benchmark-protocol
    "${Python3_EXECUTABLE}" "${CPB_ROOT}/tools/paired_benchmark.py" --self-test "$<TARGET_FILE:performance-benchmark>")



# Current-only semantic checks use immutable accepted output fingerprints, no timers.
cpb_register_test(benchmark-semantic-contract
    "${Python3_EXECUTABLE}" "${CPB_ROOT}/tools/benchmark_correctness.py" "$<TARGET_FILE:performance-benchmark>")
cpb_register_test(performance-policy-regression
    "${Python3_EXECUTABLE}" "${CPB_ROOT}/tests/performance_policy_test.py")

# Research code is absent from the ordinary performance/firmware targets.
if(NOT CPB_BENCHMARK_ONLY)
option(CPB_STAGE5_INTEGER_FOR_EXPERIMENT "Build performance target with experimental integer FOR" OFF)
if(CPB_STAGE5_INTEGER_FOR_EXPERIMENT)
    target_compile_definitions(performance-benchmark PRIVATE RMB_STAGE5_INTEGER_FOR_EXPERIMENT=1)
endif()
foreach(kind IN ITEMS normal experimental)
    cpb_test(stage5-for-${kind}-test
        SOURCES tests/stage5_for_test.cpp tests/program_psram_stubs.cpp src/core/file_management.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp
        INCLUDES tests/runtime_stubs include FLAGS ${SAN} ${SECTIONS} -fsanitize=float-cast-overflow
        DEFINES RMB_OPTIMIZER_TEST=1 LINK -Wl,--gc-sections)
endforeach()
target_compile_definitions(stage5-for-experimental-test PRIVATE RMB_STAGE5_INTEGER_FOR_EXPERIMENT=1)

cpb_test(wifi-profiles-test
    SOURCES tests/wifi_profiles_test.cpp INCLUDES include FLAGS ${SAN} ${WARN} -Werror)
cpb_test(network-operation-test
    SOURCES tests/network_operation_test.cpp src/platform/picocalc/network.cpp tests/runtime_stubs/wireless.cpp
    INCLUDES tests/network_stubs include FLAGS ${SAN} ${WARN} -Werror)
endif()

if(NOT CPB_BENCHMARK_ONLY)
cpb_test(wifi-retry-integration-test
    SOURCES tests/wifi_retry_integration_test.cpp tests/runtime_stubs/editor_workflow_ui.cpp tests/runtime_stubs/bluetooth_hid_keyboard.cpp tests/runtime_stubs/wireless.cpp tests/program_psram_stubs.cpp src/core/line_editor.cpp src/core/repl.cpp src/platform/picocalc/network.cpp src/core/editor_model.cpp src/core/psram_editor_history.cpp src/core/full_screen_editor.cpp src/core/file_management.cpp src/core/transfer_file.cpp src/core/basic_compiler.cpp src/core/compiled_cache.cpp src/core/program_store.cpp src/core/safe_file.cpp src/core/il.cpp src/core/vm.cpp src/core/ymodem.cpp src/core/xmodem.cpp
    DEFINES CPB_PRODUCTION_NETWORK=1
    INCLUDES tests/network_stubs tests/runtime_stubs include FLAGS ${SAN} ${SECTIONS} LINK -Wl,--gc-sections)
endif()
