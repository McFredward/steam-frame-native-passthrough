cmake_minimum_required(VERSION 3.16)
project(frame_mask LANGUAGES CXX)
option(FRAME_DASHBOARD_TRANSITION_PROBE "Prepare an uninstalled sparse dashboard transition diagnostic" OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(FRAME_DASHBOARD_TRANSITION_PROBE)
    find_path(DASHBOARD_VULKAN_HEADERS vulkan/vulkan.h REQUIRED)
    find_package(Threads REQUIRED)
    add_library(VkLayer_dashboard_transition_probe SHARED EXCLUDE_FROM_ALL
        layer/pixel_snapshot_layer.cpp layer/pixel_readback.cpp layer/pixel_sampler_probe.cpp
        layer/pixel_native_driver_lease.cpp layer/native_presenter_receiver.cpp
        probe/dashboard_transition_probe.cpp)
    target_include_directories(VkLayer_dashboard_transition_probe PRIVATE ${DASHBOARD_VULKAN_HEADERS})
    target_compile_definitions(VkLayer_dashboard_transition_probe PRIVATE
        FRAME_DASHBOARD_TRANSITION_PROBE=1 FRAME_PIXEL_SYNTHETIC_PRESENTER=1 FRAME_PIXEL_SSBO_DEFINITION_JOURNAL=1)
    target_compile_options(VkLayer_dashboard_transition_probe PRIVATE
        -Wall -Wextra -Wpedantic -Werror -Wno-missing-field-initializers -fvisibility=hidden)
    target_link_options(VkLayer_dashboard_transition_probe PRIVATE -Wl,-Bsymbolic-functions -Wl,--no-undefined
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/layer/pixel_snapshot_exports.map")
    target_link_libraries(VkLayer_dashboard_transition_probe PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_executable(test_dashboard_transition_probe EXCLUDE_FROM_ALL
        probe/dashboard_transition_probe_tests.cpp probe/dashboard_transition_probe.cpp)
    target_compile_options(test_dashboard_transition_probe PRIVATE
        -Wall -Wextra -Wpedantic -Werror -Wno-missing-field-initializers)
    target_include_directories(test_dashboard_transition_probe PRIVATE ${DASHBOARD_VULKAN_HEADERS})
    target_link_libraries(test_dashboard_transition_probe PRIVATE Threads::Threads)
endif()
include(CTest)
option(FRAME_STREAM_BRIDGE_ONLY
    "Build only the uninstalled negotiated stream-bridge prototype and its focused tests" OFF)
option(FRAME_STREAM_BRIDGE_RECEIVER "Build the negotiated full-resolution receiver (not automatically installed)" OFF)
if(BUILD_TESTING OR FRAME_STREAM_BRIDGE_ONLY OR FRAME_STREAM_BRIDGE_RECEIVER)
    include(cmake/StreamBridge.cmake)
endif()
if(FRAME_STREAM_BRIDGE_RECEIVER)
    include(cmake/StreamBridgeReceiver.cmake)
endif()
if(FRAME_STREAM_BRIDGE_ONLY)
    return()
endif()
option(FRAME_MASK_BUILD_TRACE "Build the opt-in passive compositor diagnostic layer" OFF)
option(FRAME_MASK_BUILD_PIXEL_SNAPSHOT
    "Build an uninstalled one-shot explicit-pattern GPU pixel diagnostic" OFF)
option(FRAME_PIXEL_SYNTHETIC_PRESENTER
    "Archived, uninstalled explicitly opted-in synthetic/scene diagnostic" OFF)
option(FRAME_PIXEL_RELEASE_RECEIVER
    "Retired legacy picture/mask-atlas receiver (unsupported)" OFF)
if(FRAME_PIXEL_RELEASE_RECEIVER)
    message(FATAL_ERROR "The legacy picture/mask-atlas release receiver is retired; use FRAME_STREAM_BRIDGE_RECEIVER")
endif()
option(FRAME_PIXEL_UNIFORM_COMPANION
    "Require a bounded same-command native uniform companion in the uninstalled diagnostic" OFF)
option(FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
    "Observe bounded cold native creation definitions without changing pipelines" OFF)
option(FRAME_PIXEL_SSBO_TRANSACTION
    "Build the uninstalled original-output same-invocation SSBO diagnostic" OFF)
if(FRAME_PIXEL_UNIFORM_COMPANION AND NOT FRAME_MASK_BUILD_PIXEL_SNAPSHOT)
    message(FATAL_ERROR "Native uniform companion requires the uninstalled pixel snapshot target")
endif()
if(FRAME_PIXEL_SSBO_DEFINITION_JOURNAL AND NOT FRAME_MASK_BUILD_PIXEL_SNAPSHOT)
    message(FATAL_ERROR "Cold SSBO definition observation requires the uninstalled pixel snapshot target")
endif()
if(FRAME_PIXEL_SSBO_TRANSACTION AND
   (NOT FRAME_MASK_BUILD_PIXEL_SNAPSHOT OR NOT FRAME_PIXEL_SSBO_DEFINITION_JOURNAL OR FRAME_PIXEL_UNIFORM_COMPANION))
    message(FATAL_ERROR "SSBO transaction requires the snapshot and definition journal, without the older uniform-copy companion")
endif()
if(FRAME_PIXEL_SYNTHETIC_PRESENTER)
    if(NOT FRAME_MASK_BUILD_PIXEL_SNAPSHOT OR FRAME_PIXEL_SSBO_TRANSACTION OR FRAME_PIXEL_UNIFORM_COMPANION)
        message(FATAL_ERROR "Synthetic output replacement requires the snapshot loader, without SSBO/uniform capture modes")
    endif()
    set(FRAME_PIXEL_SSBO_DEFINITION_JOURNAL ON)
endif()
option(FRAME_MASK_BUILD_RECEIVER_OBSERVER
    "Build the opt-in, version-pinned receiver scalar diagnostic (not installed)" OFF)
option(FRAME_MASK_BUILD_IMPORT_ENTRY
    "Build uninstalled AArch64 import entry forwarding (no observer activation)" OFF)
option(FRAME_MASK_LAYER_PRIVATE_EXPORTS
    "Use private layer interface exports (also usable by host preload ABI fixtures)" OFF)
option(FRAME_MASK_COMPOSITOR_RAW
    "Build a combined, inactive-by-default bounded compositor import diagnostic" OFF)
if(FRAME_MASK_COMPOSITOR_RAW AND NOT FRAME_MASK_BUILD_IMPORT_ENTRY)
    message(FATAL_ERROR "Compositor raw diagnosis requires the actual AArch64 entry veneers")
endif()
option(FRAME_MASK_BUILD_STARTUP_AUDIT
    "Build an uninstalled startup audit bridge with inactive default preparation" OFF)
option(FRAME_MASK_BUILD_CLIENT_STARTUP
    "Build the uninstalled, source-pinned AArch64 passive client startup observer" OFF)
option(FRAME_MASK_CLIENT_CONNECT_OBSERVATION
    "Add opt-in register-only supported-connect observations; no socket lifetime or pixels" OFF)
option(FRAME_MASK_CLIENT_RESOURCE_CENSUS
    "Census one register-only native import/reference timeline; no private or wire reads" OFF)
option(FRAME_MASK_CLIENT_RESOURCE_THREAD_PREPARATION
    "Prepare the actual decoder thread once before its first event wait; no transaction reads" OFF)
option(FRAME_MASK_CLIENT_RESOURCE_RAW_TRANSACTION
    "Capture one bounded raw import transaction; no resource/video/GPU admission" OFF)
if(FRAME_MASK_CLIENT_RESOURCE_RAW_TRANSACTION AND NOT FRAME_MASK_CLIENT_RESOURCE_THREAD_PREPARATION)
    message(FATAL_ERROR "Raw resource capture requires explicit cold decoder-thread preparation")
endif()
if(FRAME_MASK_CLIENT_RESOURCE_THREAD_PREPARATION AND NOT FRAME_MASK_CLIENT_RESOURCE_CENSUS)
    message(FATAL_ERROR "Resource thread preparation requires the explicit resource census scope")
endif()
if(FRAME_MASK_CLIENT_RESOURCE_CENSUS AND NOT FRAME_MASK_CLIENT_CONNECT_OBSERVATION)
    message(FATAL_ERROR "The resource census requires the explicit connect-observation scope")
endif()
set(FRAME_MASK_IMPORT_QEMU "" CACHE FILEPATH "Explicit local qemu-aarch64 for optional native ABI checks")
set(FRAME_MASK_IMPORT_PROVIDERS "" CACHE PATH "Private pinned Frame libc/loader prefix for native ABI checks")
set(FRAME_MASK_IMPORT_CLIENT_PROFILE "" CACHE FILEPATH "Private prepared client hash/layout profile")
set(FRAME_MASK_IMPORT_CLIENT_REFERENCE "" CACHE FILEPATH "Private reviewed client ELF for mapped identity checks")
if(FRAME_MASK_BUILD_IMPORT_ENTRY AND (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
    NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$"))
    message(FATAL_ERROR "Import entry forwarding requires the audited Linux AArch64 ABI")
endif()
if(FRAME_MASK_BUILD_CLIENT_STARTUP)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
       NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        message(FATAL_ERROR "The client startup observer requires the reviewed Linux AArch64 ABI")
    endif()
    find_package(Threads REQUIRED)
    enable_language(ASM)
    if(FRAME_MASK_CLIENT_CONNECT_OBSERVATION)
        set(client_libc_entries import/native_connection_entry.S)
    else()
        set(client_libc_entries import/native_mutex_entry.S)
    endif()
    add_library(frame_client_startup SHARED
        receiver/client_startup_observer.cpp receiver/client_startup_runtime.cpp
        ${client_libc_entries} import/native_startup_entry.S
        import/client_connect_observation.cpp import/client_resource_census.cpp import/client_resource_thread.cpp
        import/import_forwarding.cpp import/import_startup_forwarding.cpp
        import/import_main_preparation.cpp import/import_loader_reader.cpp
        import/import_module_identity.cpp import/import_startup_gate.cpp
        import/native_client_birth.cpp import/import_pipe_epoch.cpp)
    target_include_directories(frame_client_startup PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(frame_client_startup PRIVATE
        -Wall -Wextra -Wpedantic -fvisibility=hidden -ftls-model=initial-exec
        -fexceptions -fno-rtti)
    target_link_options(frame_client_startup PRIVATE -Wl,--no-undefined)
    target_link_libraries(frame_client_startup PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(FRAME_MASK_CLIENT_CONNECT_OBSERVATION)
        target_compile_definitions(frame_client_startup PRIVATE FRAME_CLIENT_CONNECT_OBSERVATION=1)
    endif()
    if(FRAME_MASK_CLIENT_RESOURCE_CENSUS)
        target_compile_definitions(frame_client_startup PRIVATE FRAME_CLIENT_RESOURCE_CENSUS=1)
        # Offline staging authenticates the hidden scope marker in a dedicated
        # read-only, non-executable segment, independently of code bytes.
        target_link_options(frame_client_startup PRIVATE -Wl,-z,separate-code)
    endif()
    if(FRAME_MASK_CLIENT_RESOURCE_THREAD_PREPARATION)
        target_compile_definitions(frame_client_startup PRIVATE FRAME_CLIENT_RESOURCE_THREAD_PREPARATION=1)
    endif()
    if(FRAME_MASK_CLIENT_RESOURCE_RAW_TRANSACTION)
        target_compile_definitions(frame_client_startup PRIVATE FRAME_CLIENT_RESOURCE_RAW_TRANSACTION=1)
        target_sources(frame_client_startup PRIVATE import/client_resource_raw.cpp
            import/client_resource_sparse.cpp import/native_client_route.cpp)
    endif()
endif()
if(FRAME_MASK_BUILD_STARTUP_AUDIT)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The startup audit bridge targets the reviewed Linux/glibc ABI")
    endif()
    add_library(frame_startup_audit SHARED import/import_startup_audit.cpp)
    target_compile_options(frame_startup_audit PRIVATE
        -Wall -Wextra -Wpedantic -fvisibility=hidden -fno-exceptions -fno-rtti
        -fno-builtin -fno-stack-protector)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        target_compile_options(frame_startup_audit PRIVATE -mno-outline-atomics)
    endif()
    target_link_options(frame_startup_audit PRIVATE -nostdlib -Wl,--no-undefined)
endif()

if(FRAME_MASK_BUILD_RECEIVER_OBSERVER)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The receiver observer supports Linux/glibc only")
    endif()
    find_package(Threads REQUIRED)
    add_library(frame_receiver_observer SHARED
        receiver/receiver_observer.cpp receiver/receiver_observer_core.cpp)
    target_include_directories(frame_receiver_observer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(frame_receiver_observer PRIVATE
        -Wall -Wextra -Wpedantic -fvisibility=hidden -ftls-model=initial-exec
        -fno-exceptions -fno-rtti)
    target_link_libraries(frame_receiver_observer PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(BUILD_TESTING)
        add_executable(test_receiver_observer
            tests/test_receiver_observer.cpp receiver/receiver_observer_core.cpp)
        target_include_directories(test_receiver_observer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(test_receiver_observer PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_receiver_observer PRIVATE Threads::Threads)
        if(NOT CMAKE_CROSSCOMPILING)
            add_test(NAME receiver_observer COMMAND test_receiver_observer)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
                add_executable(test_receiver_observer_plt tests/test_receiver_observer_plt.cpp)
                target_compile_options(test_receiver_observer_plt PRIVATE -Wall -Wextra -Wpedantic)
                target_link_options(test_receiver_observer_plt PRIVATE -rdynamic)
                target_link_libraries(test_receiver_observer_plt PRIVATE Threads::Threads)
                find_package(Python3 REQUIRED COMPONENTS Interpreter)
                add_test(NAME receiver_observer_preload COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_receiver_observer_preload.py
                    $<TARGET_FILE:frame_receiver_observer> $<TARGET_FILE:test_receiver_observer_plt>)
            endif()
        endif()
    endif()
endif()

if(UNIX AND (BUILD_TESTING OR FRAME_MASK_BUILD_TRACE OR FRAME_MASK_BUILD_IMPORT_ENTRY OR FRAME_MASK_BUILD_PIXEL_SNAPSHOT))
    if((CMAKE_CROSSCOMPILING OR FRAME_PIXEL_RELEASE_RECEIVER) AND NOT BUILD_TESTING)
        # A dispatch layer resolves the next native functions through the loader
        # chain; it needs headers, not a foreign target libvulkan to link against.
        find_path(FRAME_VULKAN_HEADERS vulkan/vulkan.h REQUIRED)
        set(Vulkan_INCLUDE_DIRS ${FRAME_VULKAN_HEADERS})
    else()
        find_package(Vulkan REQUIRED)
    endif()
    find_package(Threads REQUIRED)
    add_library(VkLayer_frame_mask_trace SHARED layer/passive_pipeline_trace.cpp)
    target_include_directories(VkLayer_frame_mask_trace PRIVATE ${Vulkan_INCLUDE_DIRS})
    target_compile_options(VkLayer_frame_mask_trace PRIVATE
        -Wall -Wextra -Wpedantic -fvisibility=hidden)
    target_link_options(VkLayer_frame_mask_trace PRIVATE -Wl,-Bsymbolic-functions)
    target_link_libraries(VkLayer_frame_mask_trace PRIVATE Threads::Threads)
    if(FRAME_MASK_BUILD_IMPORT_ENTRY)
        enable_language(ASM)
        target_sources(VkLayer_frame_mask_trace PRIVATE
            import/native_import_entry.S import/import_forwarding.cpp
            import/import_vulkan_forwarding.cpp)
        target_include_directories(VkLayer_frame_mask_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(VkLayer_frame_mask_trace PRIVATE FRAME_MASK_IMPORT_ENTRY=1)
        target_link_options(VkLayer_frame_mask_trace PRIVATE -Wl,--no-undefined)
    endif()
    if(FRAME_MASK_COMPOSITOR_RAW OR FRAME_MASK_LAYER_PRIVATE_EXPORTS)
        target_compile_definitions(VkLayer_frame_mask_trace PRIVATE FRAME_MASK_PRIVATE_VULKAN_EXPORTS=1)
        target_link_options(VkLayer_frame_mask_trace PRIVATE
            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/layer/compositor_raw_exports.map")
        set_property(TARGET VkLayer_frame_mask_trace APPEND PROPERTY LINK_DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/layer/compositor_raw_exports.map)
    endif()
    if(FRAME_MASK_COMPOSITOR_RAW)
        target_sources(VkLayer_frame_mask_trace PRIVATE
            receiver/compositor_raw_observer.cpp receiver/compositor_raw_runtime.cpp
            import/compositor_main_preparation.cpp import/import_main_preparation.cpp
            import/import_loader_reader.cpp import/import_module_identity.cpp
            import/compositor_import_thread.cpp import/import_startup_gate.cpp
            import/native_client_birth.cpp import/import_pipe_epoch.cpp import/compositor_resource_raw.cpp
            import/compositor_resource_sparse.cpp import/native_import_route.cpp
            import/compositor_journal_bridge.cpp)
        target_compile_definitions(VkLayer_frame_mask_trace PRIVATE FRAME_MASK_COMPOSITOR_RAW=1)
        target_compile_options(VkLayer_frame_mask_trace PRIVATE -ftls-model=initial-exec -fexceptions -fno-rtti)
        target_link_options(VkLayer_frame_mask_trace PRIVATE -Wl,-z,separate-code)
        target_link_libraries(VkLayer_frame_mask_trace PRIVATE ${CMAKE_DL_LIBS})
    endif()
    set(FRAME_MASK_TRACE_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}VkLayer_frame_mask_trace${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(FRAME_MASK_TRACE_FUNCTIONS "")
    if(FRAME_MASK_COMPOSITOR_RAW OR FRAME_MASK_LAYER_PRIVATE_EXPORTS)
        set(FRAME_MASK_TRACE_FUNCTIONS "\n    \"functions\": {\n      \"vkGetInstanceProcAddr\": \"frame_compositor_raw_vkGetInstanceProcAddr\",\n      \"vkGetDeviceProcAddr\": \"frame_compositor_raw_vkGetDeviceProcAddr\",\n      \"vkNegotiateLoaderLayerInterfaceVersion\": \"frame_compositor_raw_vkNegotiateLoaderLayerInterfaceVersion\"\n    },")
    endif()
    configure_file(layer/VkLayer_frame_mask_trace.json.in
        ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_frame_mask_trace.json @ONLY)
elseif(FRAME_MASK_BUILD_TRACE)
    message(FATAL_ERROR "The passive compositor trace targets Linux Vulkan")
endif()

if(FRAME_MASK_BUILD_PIXEL_SNAPSHOT)
    if(NOT UNIX)
        message(FATAL_ERROR "The receiver pixel snapshot targets Linux Vulkan")
    endif()
    add_library(VkLayer_frame_pixel_snapshot SHARED
        layer/pixel_snapshot_layer.cpp layer/pixel_readback.cpp layer/pixel_sampler_probe.cpp
        layer/pixel_native_driver_lease.cpp)
    if(FRAME_PIXEL_UNIFORM_COMPANION)
        target_sources(VkLayer_frame_pixel_snapshot PRIVATE layer/pixel_uniform_copy.cpp)
        target_compile_definitions(VkLayer_frame_pixel_snapshot PRIVATE FRAME_PIXEL_UNIFORM_COMPANION=1)
    endif()
    if(FRAME_PIXEL_SSBO_DEFINITION_JOURNAL)
        target_compile_definitions(VkLayer_frame_pixel_snapshot PRIVATE FRAME_PIXEL_SSBO_DEFINITION_JOURNAL=1)
    endif()
    if(FRAME_PIXEL_SYNTHETIC_PRESENTER)
        target_sources(VkLayer_frame_pixel_snapshot PRIVATE layer/native_presenter_receiver.cpp)
        target_compile_definitions(VkLayer_frame_pixel_snapshot PRIVATE FRAME_PIXEL_SYNTHETIC_PRESENTER=1)
    endif()
    if(FRAME_PIXEL_RELEASE_RECEIVER)
        target_compile_definitions(VkLayer_frame_pixel_snapshot PRIVATE FRAME_PIXEL_RELEASE_RECEIVER=1)
    endif()
    if(FRAME_PIXEL_SSBO_TRANSACTION)
        target_sources(VkLayer_frame_pixel_snapshot PRIVATE layer/pixel_ssbo_owned_clone.cpp
            layer/pixel_ssbo_draw_transaction.cpp layer/pixel_ssbo_receiver_bridge.cpp)
        target_compile_definitions(VkLayer_frame_pixel_snapshot PRIVATE FRAME_PIXEL_SSBO_TRANSACTION=1)
    endif()
    target_include_directories(VkLayer_frame_pixel_snapshot PRIVATE ${Vulkan_INCLUDE_DIRS})
    target_compile_options(VkLayer_frame_pixel_snapshot PRIVATE
        -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden)
    target_link_options(VkLayer_frame_pixel_snapshot PRIVATE -Wl,-Bsymbolic-functions
        -Wl,--no-undefined "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/layer/pixel_snapshot_exports.map")
    set_property(TARGET VkLayer_frame_pixel_snapshot APPEND PROPERTY LINK_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/layer/pixel_snapshot_exports.map)
    target_link_libraries(VkLayer_frame_pixel_snapshot PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    set(FRAME_PIXEL_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}VkLayer_frame_pixel_snapshot${CMAKE_SHARED_LIBRARY_SUFFIX}")
    configure_file(layer/VkLayer_frame_pixel_snapshot.json.in
        ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_frame_pixel_snapshot.json @ONLY)
endif()

if(BUILD_TESTING AND UNIX)
    add_library(pixel_native_driver_lease_fixture SHARED tests/native-driver-lease/fixture.cpp)
    target_compile_options(pixel_native_driver_lease_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_options(pixel_native_driver_lease_fixture PRIVATE -Wl,--build-id=sha1)
    add_custom_command(TARGET pixel_native_driver_lease_fixture POST_BUILD
        COMMAND chmod 0755 $<TARGET_FILE:pixel_native_driver_lease_fixture>)
    add_executable(test_pixel_native_driver_lease tests/test_pixel_native_driver_lease.cpp)
    target_include_directories(test_pixel_native_driver_lease PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_native_driver_lease PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_native_driver_lease PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_dependencies(test_pixel_native_driver_lease pixel_native_driver_lease_fixture)
    add_test(NAME pixel_native_driver_lease COMMAND test_pixel_native_driver_lease $<TARGET_FILE:pixel_native_driver_lease_fixture>)
    add_executable(test_pixel_definition_facts tests/test_pixel_definition_facts.cpp)
    target_include_directories(test_pixel_definition_facts PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_definition_facts PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME pixel_definition_facts COMMAND test_pixel_definition_facts)
    add_executable(test_pixel_definition_report tests/test_pixel_definition_report.cpp)
    target_include_directories(test_pixel_definition_report PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_definition_report PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME pixel_definition_report COMMAND test_pixel_definition_report)
    add_executable(test_pixel_graphics_admission tests/test_pixel_graphics_admission.cpp)
    target_include_directories(test_pixel_graphics_admission PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_graphics_admission PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME pixel_graphics_admission COMMAND test_pixel_graphics_admission)
    add_executable(test_pixel_renderpass_recording_contract tests/test_pixel_renderpass_recording_contract.cpp
        layer/pixel_native_driver_lease.cpp)
    target_include_directories(test_pixel_renderpass_recording_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_renderpass_recording_contract PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_definitions(test_pixel_renderpass_recording_contract PRIVATE FRAME_PIXEL_NATIVE_DRIVER_LEASE_TEST=1)
    target_link_libraries(test_pixel_renderpass_recording_contract PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME pixel_renderpass_recording_contract COMMAND test_pixel_renderpass_recording_contract)
    add_test(NAME pixel_valve_srgb_recording_contract COMMAND test_pixel_renderpass_recording_contract --valve-srgb-only)
    add_executable(test_pixel_observation_boundary tests/test_pixel_observation_boundary.cpp)
    target_compile_options(test_pixel_observation_boundary PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_observation_boundary PRIVATE Threads::Threads)
    add_test(NAME pixel_observation_boundary COMMAND test_pixel_observation_boundary)
    add_executable(test_pixel_ssbo_definition_journal tests/test_pixel_ssbo_definition_journal.cpp)
    target_include_directories(test_pixel_ssbo_definition_journal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_ssbo_definition_journal PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_ssbo_definition_journal PRIVATE Threads::Threads)
    add_test(NAME pixel_ssbo_definition_journal COMMAND test_pixel_ssbo_definition_journal)
    # Cold-only definitions and lifecycle owners; no receiver linkage or native
    # shader input is used by these ordinary CI controls.
    add_executable(test_pixel_ssbo_clone_contract tests/test_pixel_ssbo_clone_contract.cpp)
    target_include_directories(test_pixel_ssbo_clone_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_ssbo_clone_contract PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME pixel_ssbo_clone_contract COMMAND test_pixel_ssbo_clone_contract)
    add_executable(test_pixel_ssbo_owned_clone tests/test_pixel_ssbo_owned_clone.cpp layer/pixel_ssbo_owned_clone.cpp)
    target_include_directories(test_pixel_ssbo_owned_clone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_ssbo_owned_clone PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME pixel_ssbo_owned_clone COMMAND test_pixel_ssbo_owned_clone)
    add_executable(test_native_presenter_pipeline tests/test_native_presenter_pipeline.cpp)
    target_include_directories(test_native_presenter_pipeline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_native_presenter_pipeline PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_presenter_pipeline COMMAND test_native_presenter_pipeline)
    add_executable(test_native_presenter_receiver tests/test_native_presenter_receiver.cpp
        layer/native_presenter_receiver.cpp layer/pixel_readback.cpp layer/pixel_sampler_probe.cpp
        layer/pixel_native_driver_lease.cpp)
    target_include_directories(test_native_presenter_receiver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_definitions(test_native_presenter_receiver PRIVATE FRAME_PIXEL_SYNTHETIC_PRESENTER=1 FRAME_PIXEL_SSBO_DEFINITION_JOURNAL=1)
    target_compile_options(test_native_presenter_receiver PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_native_presenter_receiver PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME native_presenter_receiver COMMAND test_native_presenter_receiver)
    add_executable(test_pixel_ssbo_profile_transform tests/test_pixel_ssbo_profile_transform.cpp)
    target_include_directories(test_pixel_ssbo_profile_transform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_ssbo_profile_transform PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME pixel_ssbo_profile_transform COMMAND test_pixel_ssbo_profile_transform)
    add_executable(test_pixel_ssbo_draw_transaction tests/test_pixel_ssbo_draw_transaction.cpp
        layer/pixel_ssbo_draw_transaction.cpp layer/pixel_ssbo_owned_clone.cpp)
    target_include_directories(test_pixel_ssbo_draw_transaction PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_ssbo_draw_transaction PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_ssbo_draw_transaction PRIVATE Threads::Threads)
    add_test(NAME pixel_ssbo_draw_transaction COMMAND test_pixel_ssbo_draw_transaction)
    add_test(NAME pixel_ssbo_recording_contexts COMMAND test_pixel_ssbo_draw_transaction contexts)
    add_executable(test_pixel_ssbo_tracker_bridge tests/test_pixel_capture_tracker.cpp)
    target_include_directories(test_pixel_ssbo_tracker_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_definitions(test_pixel_ssbo_tracker_bridge PRIVATE FRAME_PIXEL_SSBO_TRANSACTION=1)
    target_compile_options(test_pixel_ssbo_tracker_bridge PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_ssbo_tracker_bridge PRIVATE Threads::Threads)
    add_test(NAME pixel_ssbo_tracker_bridge COMMAND test_pixel_ssbo_tracker_bridge ssbo)
    add_test(NAME pixel_ssbo_restore_snapshot COMMAND test_pixel_ssbo_tracker_bridge ssbo-restore)
    add_executable(test_pixel_ssbo_receiver_bridge tests/test_pixel_ssbo_receiver_bridge.cpp
        layer/pixel_ssbo_receiver_bridge.cpp layer/pixel_ssbo_draw_transaction.cpp layer/pixel_ssbo_owned_clone.cpp)
    target_include_directories(test_pixel_ssbo_receiver_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_definitions(test_pixel_ssbo_receiver_bridge PRIVATE FRAME_PIXEL_SSBO_TRANSACTION=1)
    target_compile_options(test_pixel_ssbo_receiver_bridge PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_ssbo_receiver_bridge PRIVATE Threads::Threads)
    add_test(NAME pixel_ssbo_receiver_bridge COMMAND test_pixel_ssbo_receiver_bridge)
    add_executable(test_pixel_snapshot_rp2_hooks tests/test_pixel_snapshot_rp2_hooks.cpp
        layer/pixel_native_driver_lease.cpp
        layer/pixel_ssbo_receiver_bridge.cpp layer/pixel_ssbo_draw_transaction.cpp layer/pixel_ssbo_owned_clone.cpp
        layer/pixel_readback.cpp layer/pixel_sampler_probe.cpp)
    target_include_directories(test_pixel_snapshot_rp2_hooks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_definitions(test_pixel_snapshot_rp2_hooks PRIVATE FRAME_PIXEL_SSBO_TRANSACTION=1 FRAME_PIXEL_SSBO_DEFINITION_JOURNAL=1 FRAME_PIXEL_NATIVE_DRIVER_LEASE_TEST=1)
    target_compile_options(test_pixel_snapshot_rp2_hooks PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_snapshot_rp2_hooks PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME pixel_snapshot_rp2_hooks COMMAND test_pixel_snapshot_rp2_hooks)
    add_test(NAME pixel_snapshot_recording_hooks COMMAND test_pixel_snapshot_rp2_hooks --recording-public)
    # The native companion is an explicit, uninstalled diagnostic build option.
    # Its contracts remain independently testable without a headset or capture.
    add_executable(test_pixel_uniform_resources tests/test_pixel_uniform_resources.cpp)
    target_include_directories(test_pixel_uniform_resources PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_uniform_resources PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_uniform_resources PRIVATE Threads::Threads)
    add_test(NAME pixel_uniform_resources COMMAND test_pixel_uniform_resources)
    add_executable(test_pixel_uniform_contract tests/test_pixel_uniform_contract.cpp)
    target_include_directories(test_pixel_uniform_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_uniform_contract PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_uniform_contract PRIVATE Threads::Threads)
    add_test(NAME pixel_uniform_contract COMMAND test_pixel_uniform_contract)
    add_executable(test_pixel_uniform_copy tests/test_pixel_uniform_copy.cpp layer/pixel_uniform_copy.cpp)
    target_include_directories(test_pixel_uniform_copy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_uniform_copy PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_uniform_copy PRIVATE Vulkan::Vulkan Threads::Threads)
    add_test(NAME pixel_uniform_copy_controls COMMAND test_pixel_uniform_copy controls)
    add_test(NAME pixel_uniform_copy_gpu COMMAND test_pixel_uniform_copy gpu)
    set_tests_properties(pixel_uniform_copy_gpu PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(test_pixel_uniform_metadata tests/test_pixel_uniform_metadata.cpp layer/pixel_uniform_copy.cpp)
    target_include_directories(test_pixel_uniform_metadata PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_uniform_metadata PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_uniform_metadata PRIVATE Threads::Threads)
    add_test(NAME pixel_uniform_metadata COMMAND test_pixel_uniform_metadata)
    add_executable(test_pixel_sampler_probe tests/test_pixel_sampler_probe.cpp layer/pixel_sampler_probe.cpp)
    target_include_directories(test_pixel_sampler_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_sampler_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME pixel_sampler_controls COMMAND test_pixel_sampler_probe)
    foreach(sampler_helper admission format)
        add_executable(test_pixel_sampler_${sampler_helper} tests/test_pixel_sampler_${sampler_helper}.cpp)
        target_include_directories(test_pixel_sampler_${sampler_helper} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        target_compile_options(test_pixel_sampler_${sampler_helper} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME pixel_sampler_${sampler_helper} COMMAND test_pixel_sampler_${sampler_helper})
    endforeach()
    add_executable(test_pixel_sampler_gpu tests/test_pixel_sampler_gpu.cpp layer/pixel_sampler_probe.cpp)
    target_include_directories(test_pixel_sampler_gpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_sampler_gpu PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_sampler_gpu PRIVATE Vulkan::Vulkan)
    add_test(NAME pixel_sampler_gpu COMMAND test_pixel_sampler_gpu all)
    add_test(NAME pixel_sampler_gpu_alternate_profile COMMAND test_pixel_sampler_gpu p010-supported)
    add_executable(test_pixel_readback tests/test_pixel_readback.cpp layer/pixel_readback.cpp)
    target_include_directories(test_pixel_readback PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_readback PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_readback PRIVATE Vulkan::Vulkan Threads::Threads)
    add_test(NAME pixel_readback_controls COMMAND test_pixel_readback controls)
    add_test(NAME pixel_readback_gpu COMMAND test_pixel_readback gpu)
    add_executable(test_pixel_capture_tracker tests/test_pixel_capture_tracker.cpp)
    target_include_directories(test_pixel_capture_tracker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_capture_tracker PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_capture_tracker PRIVATE Threads::Threads)
    add_test(NAME pixel_capture_tracker COMMAND test_pixel_capture_tracker)
    add_executable(test_pixel_snapshot_io tests/test_pixel_snapshot_io.cpp)
    target_include_directories(test_pixel_snapshot_io PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_snapshot_io PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_snapshot_io PRIVATE Threads::Threads)
    add_test(NAME pixel_snapshot_io COMMAND test_pixel_snapshot_io)
    add_executable(test_pixel_snapshot_selection tests/test_pixel_snapshot_selection.cpp)
    target_include_directories(test_pixel_snapshot_selection PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_options(test_pixel_snapshot_selection PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_pixel_snapshot_selection PRIVATE Threads::Threads)
    add_test(NAME pixel_snapshot_selection COMMAND test_pixel_snapshot_selection)
endif()

add_library(frame_mask STATIC mask/mask_protocol.cpp mask/frame_association.cpp)
target_include_directories(frame_mask PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(frame_mask PRIVATE -Wall -Wextra -Wpedantic)
add_executable(mask_endpoint tools/mask_endpoint.cpp)
target_link_libraries(mask_endpoint PRIVATE frame_mask)
target_compile_options(mask_endpoint PRIVATE -Wall -Wextra -Wpedantic)
if(WIN32)
    target_link_libraries(mask_endpoint PRIVATE ws2_32)
endif()
if(MINGW)
    target_link_options(mask_endpoint PRIVATE -static-libgcc -static-libstdc++)
endif()

if(BUILD_TESTING)
    add_executable(test_mask_protocol tests/test_mask_protocol.cpp)
    target_link_libraries(test_mask_protocol PRIVATE frame_mask)
    add_test(NAME mask_protocol COMMAND test_mask_protocol)
    add_executable(test_frame_association tests/test_frame_association.cpp)
    target_link_libraries(test_frame_association PRIVATE frame_mask)
    target_compile_options(test_frame_association PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME frame_association COMMAND test_frame_association)
    add_executable(test_mask_module_guard tests/test_mask_module_guard.cpp)
    target_include_directories(test_mask_module_guard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME mask_module_guard COMMAND test_mask_module_guard)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(UNIX)
        add_test(NAME prepare_native_release COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prepare_native_release.py)
        add_test(NAME pixel_uniform_recovery COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pixel_uniform_recovery.py)
        add_test(NAME pixel_uniform_mapping COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pixel_uniform_mapping.py)
        add_test(NAME registration_source_recovery COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_registration_source_recovery.py)
        add_test(NAME registration_header_discovery COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_registration_header_discovery.py)
        add_test(NAME registration_canvas_locator COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_registration_canvas_locator.py)
        add_test(NAME native_r1_registration COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_r1_registration.py)
        # Public parser controls run without redistributing any native module.
        # Private original-shader checks require an explicit local input.
        add_test(NAME native_fragment_ssa_probe COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fragment_ssa_probe.py)
        add_test(NAME native_fragment_atlas_probe COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fragment_atlas_probe.py)
        add_test(NAME native_fragment_ssbo_probe COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fragment_ssbo_probe.py)
        add_test(NAME native_fragment_ssbo_profile COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fragment_ssbo_profile.py)
        add_test(NAME native_fragment_packed_output COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fragment_packed_output.py)
        add_test(NAME native_fragment_r1_presenter COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fragment_r1_presenter.py)
        add_test(NAME native_fragment_guarded_presenter COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fragment_guarded_presenter.py)
        add_test(NAME native_fragment_subgroup_presenter COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fragment_subgroup_presenter.py)
        add_test(NAME native_fragment_scene_presenter COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fragment_scene_presenter.py)
        add_test(NAME native_fragment_scene_full_presenter COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fragment_scene_full_presenter.py)
        add_test(NAME native_fragment_scene_robust_presenter COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_fragment_scene_robust_presenter.py)
        add_test(NAME native_fragment_robust_scene_stream_contract COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/native-fragment-robust-scene-stream/test_contract.py)
        add_test(NAME synthetic_presenter_session COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_synthetic_presenter_session.py)
        add_test(NAME scene_presenter_session COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scene_presenter_session.py)
        add_test(NAME steamvr_camera_alpha COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_steamvr_camera_alpha.py)
        add_test(NAME pixel_uniform_metadata_bridge COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pixel_uniform_metadata.py
            --fixture $<TARGET_FILE:test_pixel_uniform_metadata>)
        add_test(NAME pixel_sampler_shader COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pixel_sampler_shader.py)
        add_test(NAME pixel_sampler_routes COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pixel_sampler_routes.py)
        add_test(NAME pixel_sampler_recovery COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pixel_sampler_recovery.py)
        add_test(NAME app_owned_payload_reference COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_app_payload_probe.py)
        add_test(NAME pixel_payload_recovery COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pixel_payload_recovery.py)
        add_test(NAME pixel_refusal_status COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pixel_refusal_status.py)
        add_test(NAME pixel_snapshot_session COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pixel_snapshot_session.py)
        add_test(NAME pixel_stream_preflight COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pixel_stream_preflight.py)
        add_test(NAME native_provider_preflight_integration COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_provider_preflight_integration.py $<TARGET_FILE:test_pixel_snapshot_rp2_hooks>)
        if(FRAME_MASK_BUILD_PIXEL_SNAPSHOT)
            set(pixel_definition_test_args)
            if(FRAME_PIXEL_SSBO_DEFINITION_JOURNAL)
                set(pixel_definition_test_args --ssbo-definitions)
            endif()
            if(FRAME_PIXEL_SSBO_TRANSACTION)
                list(APPEND pixel_definition_test_args --ssbo-transaction)
            endif()
            add_executable(test_pixel_snapshot_layer tests/test_pixel_snapshot_layer.cpp)
            target_include_directories(test_pixel_snapshot_layer PRIVATE ${Vulkan_INCLUDE_DIRS})
            target_compile_options(test_pixel_snapshot_layer PRIVATE -Wall -Wextra -Wpedantic -Werror)
            target_link_libraries(test_pixel_snapshot_layer PRIVATE Vulkan::Vulkan Threads::Threads ${CMAKE_DL_LIBS})
            add_test(NAME pixel_snapshot_real_loader COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pixel_snapshot_layer.py
                $<TARGET_FILE:test_pixel_snapshot_layer> $<TARGET_FILE:VkLayer_frame_pixel_snapshot>
                ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_frame_pixel_snapshot.json ${pixel_definition_test_args})
            set_tests_properties(pixel_snapshot_real_loader PROPERTIES TIMEOUT 90)
            add_test(NAME pixel_snapshot_query_loader COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pixel_snapshot_layer.py
                $<TARGET_FILE:test_pixel_snapshot_layer> $<TARGET_FILE:VkLayer_frame_pixel_snapshot>
                ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_frame_pixel_snapshot.json --statistics-only ${pixel_definition_test_args})
            set_tests_properties(pixel_snapshot_query_loader PROPERTIES TIMEOUT 90)
        endif()
        add_executable(test_compositor_bootstrap tests/test_compositor_bootstrap.cpp
            import/compositor_main_preparation.cpp import/import_main_preparation.cpp
            import/import_loader_reader.cpp import/import_module_identity.cpp)
        target_include_directories(test_compositor_bootstrap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(test_compositor_bootstrap PRIVATE ${CMAKE_DL_LIBS})
        add_test(NAME compositor_cold_bootstrap COMMAND test_compositor_bootstrap)
        add_executable(test_compositor_import_thread tests/test_compositor_import_thread.cpp
            import/compositor_import_thread.cpp import/native_import_route.cpp
            import/import_startup_gate.cpp import/native_client_birth.cpp import/import_pipe_epoch.cpp)
        target_include_directories(test_compositor_import_thread PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME compositor_first_import_thread COMMAND test_compositor_import_thread)
        add_executable(test_compositor_raw_receipt tests/test_compositor_raw_receipt.cpp
            import/compositor_journal_bridge.cpp)
        target_include_directories(test_compositor_raw_receipt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        target_link_libraries(test_compositor_raw_receipt PRIVATE Threads::Threads)
        add_test(NAME compositor_owning_journal_receipt COMMAND test_compositor_raw_receipt)
        add_test(NAME compositor_selective_startup_audit COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compositor_startup_audit.py)
        add_test(NAME compositor_raw_session_recovery COMMAND ${Python3_EXECUTABLE}
            -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_compositor_raw_session.py)
        add_test(NAME compositor_ui_startup_health COMMAND ${Python3_EXECUTABLE}
            -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_compositor_startup_health.py)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
            # Deployment additionally requires the reviewed cold-freeze package,
            # exact native identities and actual helper isolation/UI evidence.
            enable_language(C)
            add_executable(test_compositor_launch_selector tests/test_compositor_launch_selector.c
                receiver/compositor_launch_selector.c)
            target_include_directories(test_compositor_launch_selector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            target_compile_options(test_compositor_launch_selector PRIVATE -std=c11 -Wall -Wextra -Wpedantic)
            target_link_libraries(test_compositor_launch_selector PRIVATE ${CMAKE_DL_LIBS})
            add_test(NAME compositor_launch_selector_core COMMAND test_compositor_launch_selector)
            add_test(NAME compositor_launch_loader_fixture COMMAND ${Python3_EXECUTABLE}
                -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_compositor_launch_preload.py)
            set_tests_properties(compositor_launch_loader_fixture PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;FRAME_LAUNCH_TEST_RECEIPT=${CMAKE_CURRENT_BINARY_DIR}/launch-selector-host.json"
                TIMEOUT 120)
        endif()
        set_tests_properties(compositor_raw_session_recovery compositor_ui_startup_health PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 120)
        add_test(NAME compositor_raw_offline_artifacts COMMAND ${Python3_EXECUTABLE}
            -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_prepare_compositor_raw_observer.py)
        # These fixtures include the production implementation directly, with
        # explicit test transports. They cannot create sealed native Ready.
        add_executable(test_compositor_raw_runtime tests/test_compositor_raw_runtime.cpp
            import/compositor_import_thread.cpp import/compositor_resource_sparse.cpp
            import/compositor_resource_raw.cpp import/native_import_route.cpp
            import/import_startup_gate.cpp import/import_pipe_epoch.cpp import/native_client_birth.cpp
            import/compositor_main_preparation.cpp import/import_main_preparation.cpp
            import/import_loader_reader.cpp import/import_module_identity.cpp)
        target_include_directories(test_compositor_raw_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        target_link_libraries(test_compositor_raw_runtime PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
        target_compile_options(test_compositor_raw_runtime PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME compositor_raw_callback_integration COMMAND test_compositor_raw_runtime)
        add_executable(test_compositor_raw_writer tests/test_compositor_raw_observer.cpp
            import/compositor_main_preparation.cpp import/import_main_preparation.cpp
            import/import_loader_reader.cpp import/import_module_identity.cpp)
        target_include_directories(test_compositor_raw_writer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        target_link_libraries(test_compositor_raw_writer PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
        target_compile_options(test_compositor_raw_writer PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME compositor_raw_bounded_writer COMMAND test_compositor_raw_writer)
        add_executable(test_compositor_probe_schedule tests/test_compositor_probe_schedule.cpp)
        target_include_directories(test_compositor_probe_schedule PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(test_compositor_probe_schedule PRIVATE Threads::Threads)
        add_test(NAME compositor_bounded_probe_schedule COMMAND test_compositor_probe_schedule)
        add_executable(test_compositor_arm_trace tests/test_compositor_arm_trace.cpp import/compositor_journal_bridge.cpp)
        target_include_directories(test_compositor_arm_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        target_link_libraries(test_compositor_arm_trace PRIVATE Threads::Threads)
        target_compile_options(test_compositor_arm_trace PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME compositor_raw_reserved_trace COMMAND test_compositor_arm_trace)
        add_test(NAME compositor_raw_unassociated_draw_trace COMMAND test_compositor_arm_trace public-draw)
        add_test(NAME compositor_raw_unarmed_trace COMMAND test_compositor_arm_trace unarmed)
        add_test(NAME compositor_raw_byte_cap_lifecycle COMMAND test_compositor_arm_trace byte-cap)
        add_test(NAME compositor_raw_event_cap_lifecycle COMMAND test_compositor_arm_trace event-cap)
        add_test(NAME compositor_raw_failed_writer COMMAND test_compositor_arm_trace failed-writer)
        set_tests_properties(compositor_raw_reserved_trace compositor_raw_unarmed_trace
            compositor_raw_byte_cap_lifecycle compositor_raw_event_cap_lifecycle
            compositor_raw_failed_writer PROPERTIES TIMEOUT 10)
        add_executable(test_import_observer tests/test_import_observer.cpp
            import/import_observer_core.cpp)
        target_include_directories(test_import_observer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(test_import_observer PRIVATE -Wall -Wextra -Wpedantic)
        find_package(Threads REQUIRED)
        target_link_libraries(test_import_observer PRIVATE Threads::Threads)
        add_test(NAME compositor_import_core COMMAND test_import_observer)
        add_executable(test_native_import_route tests/test_native_import_route.cpp
            import/native_import_route.cpp)
        target_include_directories(test_native_import_route PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(test_native_import_route PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME compositor_import_native_route COMMAND test_native_import_route)
        add_executable(test_compositor_resource_sparse tests/test_compositor_resource_sparse.cpp
            import/compositor_resource_sparse.cpp import/native_import_route.cpp)
        target_include_directories(test_compositor_resource_sparse PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(test_compositor_resource_sparse PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME compositor_resource_sparse_copy COMMAND test_compositor_resource_sparse)
        add_executable(test_compositor_resource_raw tests/test_compositor_resource_raw.cpp
            import/compositor_resource_raw.cpp import/compositor_resource_sparse.cpp
            import/native_import_route.cpp)
        target_include_directories(test_compositor_resource_raw PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        target_compile_options(test_compositor_resource_raw PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME compositor_resource_raw_metadata COMMAND test_compositor_resource_raw)
        add_executable(test_compositor_resource_thread tests/test_compositor_resource_thread.cpp
            import/compositor_resource_thread.cpp import/import_startup_gate.cpp
            import/native_client_birth.cpp import/import_pipe_epoch.cpp)
        target_include_directories(test_compositor_resource_thread PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(test_compositor_resource_thread PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_compositor_resource_thread PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
        add_test(NAME compositor_resource_thread_preparation COMMAND test_compositor_resource_thread)
        set_tests_properties(compositor_resource_thread_preparation PROPERTIES
            ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=1")
        add_executable(test_import_pipe_epoch tests/test_import_pipe_epoch.cpp
            import/import_pipe_epoch.cpp)
        target_include_directories(test_import_pipe_epoch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(test_import_pipe_epoch PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_definitions(test_import_pipe_epoch PRIVATE FRAME_IMPORT_PIPE_EPOCH_TEST_FIXTURE)
        target_link_libraries(test_import_pipe_epoch PRIVATE Threads::Threads)
        add_test(NAME compositor_import_pipe_epochs COMMAND test_import_pipe_epoch)
        add_executable(test_compositor_attempt tests/test_compositor_attempt.cpp
            import/compositor_attempt.cpp import/native_import_route.cpp
            import/import_pipe_epoch.cpp import/import_observer_core.cpp)
        target_include_directories(test_compositor_attempt PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        target_compile_options(test_compositor_attempt PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_compositor_attempt PRIVATE Threads::Threads)
        add_test(NAME compositor_import_attempt COMMAND test_compositor_attempt)
        add_executable(test_client_carrier_core tests/test_client_carrier_core.cpp
            import/client_carrier_core.cpp)
        target_include_directories(test_client_carrier_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(test_client_carrier_core PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_client_carrier_core PRIVATE Threads::Threads)
        add_test(NAME client_import_carrier COMMAND test_client_carrier_core)
        add_executable(test_native_client_route tests/test_native_client_route.cpp
            import/native_client_route.cpp)
        target_include_directories(test_native_client_route PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(test_native_client_route PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME client_import_native_route COMMAND test_native_client_route)
        add_executable(test_client_resource_sparse tests/test_client_resource_sparse.cpp
            import/client_resource_sparse.cpp import/native_client_route.cpp)
        target_include_directories(test_client_resource_sparse PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(test_client_resource_sparse PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME client_resource_sparse_copy COMMAND test_client_resource_sparse)
        add_executable(test_client_resource_raw tests/test_client_resource_raw.cpp
            import/client_resource_raw.cpp import/native_client_route.cpp import/client_carrier_core.cpp)
        target_include_directories(test_client_resource_raw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(test_client_resource_raw PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME client_resource_raw_metadata COMMAND test_client_resource_raw)
        add_executable(test_import_forwarding tests/test_import_forwarding.cpp
            import/import_forwarding.cpp)
        target_include_directories(test_import_forwarding PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(test_import_forwarding PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_import_forwarding PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
        add_test(NAME compositor_import_libc_forwarding COMMAND test_import_forwarding)
        add_executable(test_import_vulkan_forwarding tests/test_import_vulkan_forwarding.cpp
            import/import_vulkan_forwarding.cpp)
        target_include_directories(test_import_vulkan_forwarding PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        target_compile_options(test_import_vulkan_forwarding PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_import_vulkan_forwarding PRIVATE Vulkan::Vulkan Threads::Threads)
        add_test(NAME compositor_import_vulkan_forwarding COMMAND test_import_vulkan_forwarding)
        add_test(NAME compositor_import_entry_abi COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_import_entry_abi.py
            --source ${CMAKE_CURRENT_SOURCE_DIR}/import/native_import_entry.S)
        set_tests_properties(compositor_import_entry_abi PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME import_startup_entry_abi COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_import_entry_abi.py
            --source ${CMAKE_CURRENT_SOURCE_DIR}/import/native_import_entry.S --startup)
        set_tests_properties(import_startup_entry_abi PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME import_startup_mutex_entry_abi COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_import_entry_abi.py
            --source ${CMAKE_CURRENT_SOURCE_DIR}/import/native_import_entry.S --mutex)
        set_tests_properties(import_startup_mutex_entry_abi PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME client_connect_entry_abi COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_import_entry_abi.py
            --source ${CMAKE_CURRENT_SOURCE_DIR}/import/native_import_entry.S --connection)
        set_tests_properties(client_connect_entry_abi PROPERTIES SKIP_RETURN_CODE 77)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_executable(test_import_module_identity tests/test_import_module_identity.cpp
                import/import_module_identity.cpp)
            target_include_directories(test_import_module_identity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            target_compile_options(test_import_module_identity PRIVATE -Wall -Wextra -Wpedantic)
            add_test(NAME import_module_hashing COMMAND test_import_module_identity --sha-self-test)
            add_executable(test_import_startup_forwarding tests/test_import_startup_forwarding.cpp
                import/import_startup_forwarding.cpp)
            target_include_directories(test_import_startup_forwarding PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            target_compile_options(test_import_startup_forwarding PRIVATE -Wall -Wextra -Wpedantic -fexceptions)
            target_link_libraries(test_import_startup_forwarding PRIVATE Threads::Threads)
            add_test(NAME import_startup_provider_forwarding COMMAND test_import_startup_forwarding)
            if(NOT CMAKE_CROSSCOMPILING)
                set(client_startup_core_sources receiver/client_startup_runtime.cpp
                    import/client_connect_observation.cpp import/client_resource_census.cpp import/client_resource_thread.cpp
                    import/import_main_preparation.cpp import/import_loader_reader.cpp
                    import/import_module_identity.cpp import/import_startup_gate.cpp
                    import/native_client_birth.cpp import/import_pipe_epoch.cpp
                    import/import_forwarding.cpp import/import_startup_forwarding.cpp)
                add_executable(test_client_startup_runtime tests/test_client_startup_runtime.cpp
                    ${client_startup_core_sources})
                target_include_directories(test_client_startup_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
                target_compile_options(test_client_startup_runtime PRIVATE -Wall -Wextra -Wpedantic -fexceptions)
                target_link_libraries(test_client_startup_runtime PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
                add_test(NAME client_startup_readiness COMMAND test_client_startup_runtime)
                add_executable(test_client_connect_observation tests/test_client_connect_observation.cpp
                    import/client_connect_observation.cpp)
                target_include_directories(test_client_connect_observation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
                target_compile_options(test_client_connect_observation PRIVATE -Wall -Wextra -Wpedantic)
                add_test(NAME client_connect_register_observation COMMAND test_client_connect_observation)
                add_executable(test_client_resource_census tests/test_client_resource_census.cpp
                    import/client_resource_census.cpp import/client_connect_observation.cpp
                    import/import_startup_gate.cpp import/native_client_birth.cpp import/import_pipe_epoch.cpp)
                target_include_directories(test_client_resource_census PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
                target_compile_options(test_client_resource_census PRIVATE -Wall -Wextra -Wpedantic)
                target_link_libraries(test_client_resource_census PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
                add_test(NAME client_resource_register_census COMMAND test_client_resource_census)
                add_executable(test_client_resource_thread tests/test_client_resource_thread.cpp
                    import/client_resource_thread.cpp import/import_startup_gate.cpp
                    import/native_client_birth.cpp import/import_pipe_epoch.cpp)
                target_include_directories(test_client_resource_thread PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
                target_compile_options(test_client_resource_thread PRIVATE -Wall -Wextra -Wpedantic)
                target_link_libraries(test_client_resource_thread PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
                add_test(NAME client_resource_thread_preparation COMMAND test_client_resource_thread)
                # Exercise actual pthread-owned stack bounds, not ASan's optional
                # artificial return stack outside that native stack allocation.
                set_tests_properties(client_resource_register_census client_resource_thread_preparation PROPERTIES
                    ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=1")
                # Host controls deliberately contain no native public aliases
                # or fabricated Ready. Actual ARM aliases are checked separately
                # with the explicitly supplied final ELF and pinned providers.
                add_library(client_startup_inert_fixture SHARED receiver/client_startup_observer.cpp
                    ${client_startup_core_sources})
                target_include_directories(client_startup_inert_fixture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
                target_compile_options(client_startup_inert_fixture PRIVATE
                    -Wall -Wextra -Wpedantic -fexceptions -fvisibility=hidden -ftls-model=initial-exec)
                target_link_libraries(client_startup_inert_fixture PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
                add_test(NAME client_startup_inert_preload COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_client_startup_observer.py
                    --observer $<TARGET_FILE:client_startup_inert_fixture>
                    --workdir ${CMAKE_CURRENT_BINARY_DIR}/client-startup-controls)
                add_test(NAME client_startup_offline_preparation COMMAND ${Python3_EXECUTABLE}
                    -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests
                    -p test_prepare_client_startup_observer.py)
            endif()
            if(NOT CMAKE_CROSSCOMPILING)
                add_test(NAME import_startup_audit_binding COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_import_startup_audit.py
                    --record ${CMAKE_CURRENT_BINARY_DIR}/import-startup-audit-validation.json)
                add_test(NAME import_startup_deepbind_binding COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_import_startup_audit.py --deepbind
                    --record ${CMAKE_CURRENT_BINARY_DIR}/import-startup-deepbind-validation.json)
                add_test(NAME client_connect_audit_binding COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_import_startup_audit.py --connection
                    --record ${CMAKE_CURRENT_BINARY_DIR}/client-connect-audit-validation.json)
                add_test(NAME client_resource_thread_audit_binding COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_import_startup_audit.py --resource-thread
                    --record ${CMAKE_CURRENT_BINARY_DIR}/client-resource-thread-audit-validation.json)
                add_test(NAME client_resource_raw_audit_binding COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_client_raw_audit.py
                    --record ${CMAKE_CURRENT_BINARY_DIR}/client-resource-raw-audit-validation.json)
            endif()
            if(NOT CMAKE_CROSSCOMPILING)
                add_library(import_loader_fixture SHARED tests/test_import_loader_reader.cpp)
                target_compile_definitions(import_loader_fixture PRIVATE IMPORT_LOADER_TEST_DSO)
                target_compile_options(import_loader_fixture PRIVATE -fcf-protection=none)
                target_link_options(import_loader_fixture PRIVATE
                    -nostdlib -nostartfiles -Wl,--no-eh-frame-hdr -Wl,-z,norelro)
                add_executable(test_import_loader_reader tests/test_import_loader_reader.cpp
                    import/import_loader_reader.cpp import/import_module_identity.cpp)
                target_include_directories(test_import_loader_reader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
                target_compile_options(test_import_loader_reader PRIVATE -Wall -Wextra -Wpedantic)
                target_link_libraries(test_import_loader_reader PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
                add_test(NAME import_loader_lease COMMAND test_import_loader_reader $<TARGET_FILE:import_loader_fixture>)
                add_test(NAME import_loader_cold_preflight COMMAND test_import_loader_reader --cold-only)
                add_library(import_main_fixture SHARED tests/test_import_main_preparation.cpp)
                target_compile_definitions(import_main_fixture PRIVATE IMPORT_MAIN_TEST_DSO)
                target_compile_options(import_main_fixture PRIVATE -fcf-protection=none)
                set(import_main_profile_dir ${CMAKE_CURRENT_BINARY_DIR}/import-main-fixture-profile)
                add_custom_command(OUTPUT ${import_main_profile_dir}/module-6.bin
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${import_main_profile_dir}
                    COMMAND ${CMAKE_COMMAND} -E remove -f ${import_main_profile_dir}/module-6.bin
                        ${import_main_profile_dir}/module-6.json
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_import_main_modules.py
                        --fixture $<TARGET_FILE:import_main_fixture> --output ${import_main_profile_dir}
                    DEPENDS import_main_fixture tools/prepare_import_main_modules.py
                    VERBATIM)
                add_custom_target(import_main_profile DEPENDS ${import_main_profile_dir}/module-6.bin)
                add_executable(test_import_main_preparation tests/test_import_main_preparation.cpp
                    import/import_main_preparation.cpp import/import_loader_reader.cpp
                    import/import_module_identity.cpp)
                target_include_directories(test_import_main_preparation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
                target_compile_options(test_import_main_preparation PRIVATE -Wall -Wextra -Wpedantic)
                target_link_libraries(test_import_main_preparation PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
                add_dependencies(test_import_main_preparation import_main_profile)
                add_test(NAME import_main_cold_preparation COMMAND test_import_main_preparation
                    $<TARGET_FILE:import_main_fixture> ${import_main_profile_dir}/module-6.bin)
            endif()
            if(FRAME_MASK_IMPORT_CLIENT_PROFILE OR FRAME_MASK_IMPORT_CLIENT_REFERENCE)
                if(NOT FRAME_MASK_IMPORT_CLIENT_PROFILE OR NOT FRAME_MASK_IMPORT_CLIENT_REFERENCE)
                    message(FATAL_ERROR "Private mapped-identity checks require both client profile and reference")
                endif()
                add_test(NAME import_module_mapped_identity COMMAND test_import_module_identity
                    ${FRAME_MASK_IMPORT_CLIENT_PROFILE} ${FRAME_MASK_IMPORT_CLIENT_REFERENCE})
            endif()
            add_executable(test_native_client_birth tests/test_native_client_birth.cpp
                import/native_client_birth.cpp)
            target_include_directories(test_native_client_birth PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            target_compile_options(test_native_client_birth PRIVATE -Wall -Wextra -Wpedantic)
            add_test(NAME import_client_birth_decoder COMMAND test_native_client_birth)
            if(NOT CMAKE_CROSSCOMPILING)
                add_library(import_startup_fixture SHARED tests/test_import_startup_gate.cpp)
                target_compile_definitions(import_startup_fixture PRIVATE FRAME_STARTUP_FIXTURE_LIBRARY)
                target_link_options(import_startup_fixture PRIVATE
                    -Wl,--section-start=.fixture_rx=0x90000 -Wl,--section-start=.fixture_hmd=0x647d78)
                add_executable(test_import_startup_gate tests/test_import_startup_gate.cpp
                    import/import_startup_gate.cpp import/native_client_birth.cpp import/import_pipe_epoch.cpp)
                target_include_directories(test_import_startup_gate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
                target_compile_options(test_import_startup_gate PRIVATE -Wall -Wextra -Wpedantic)
                target_link_libraries(test_import_startup_gate PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
                add_test(NAME import_startup_gate COMMAND test_import_startup_gate $<TARGET_FILE:import_startup_fixture>)
                # The fixture tests actual pthread-owned stack bounds. ASan's
                # optional artificial return stack is outside that ownership.
                set_tests_properties(import_startup_gate PROPERTIES
                    ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=1")
            endif()
            if(FRAME_MASK_IMPORT_QEMU OR FRAME_MASK_IMPORT_PROVIDERS)
                if(NOT FRAME_MASK_IMPORT_QEMU OR NOT FRAME_MASK_IMPORT_PROVIDERS)
                    message(FATAL_ERROR "Native ARM64 execution requires explicit QEMU and private providers")
                endif()
                add_test(NAME import_native_arm64_execution COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_import_native_forwarding.py
                    --qemu ${FRAME_MASK_IMPORT_QEMU} --providers ${FRAME_MASK_IMPORT_PROVIDERS}
                    --record ${CMAKE_CURRENT_BINARY_DIR}/import-native-arm64-validation.json)
                add_test(NAME import_startup_native_arm64_execution COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_import_startup_native.py
                    --qemu ${FRAME_MASK_IMPORT_QEMU} --providers ${FRAME_MASK_IMPORT_PROVIDERS}
                    --record ${CMAKE_CURRENT_BINARY_DIR}/import-startup-native-arm64-validation.json)
            endif()
        endif()
        add_test(NAME unix_connection_snapshot COMMAND ${Python3_EXECUTABLE}
            -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests
            -p test_inspect_unix_connection.py)
        add_test(NAME receiver_observer_preparation COMMAND ${Python3_EXECUTABLE}
            -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests
            -p test_prepare_receiver_observer.py)
        add_test(NAME receiver_observer_session_recovery COMMAND ${Python3_EXECUTABLE}
            -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests
            -p test_receiver_observer_session.py)
        add_test(NAME client_startup_session_recovery COMMAND ${Python3_EXECUTABLE}
            -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests
            -p test_client_startup_session.py)
        add_test(NAME frame_trace_session_recovery COMMAND ${Python3_EXECUTABLE}
            -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_frame_trace_session.py)
    endif()
    if(TARGET VkLayer_frame_mask_trace)
        add_executable(test_passive_pipeline_trace tests/test_passive_pipeline_trace.cpp)
        target_include_directories(test_passive_pipeline_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(test_passive_pipeline_trace PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_passive_pipeline_trace PRIVATE Threads::Threads)
        add_test(NAME passive_pipeline_trace COMMAND test_passive_pipeline_trace)
        add_executable(test_passive_binding_trace tests/test_passive_binding_trace.cpp)
        target_include_directories(test_passive_binding_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        target_compile_options(test_passive_binding_trace PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_passive_binding_trace PRIVATE Threads::Threads)
        add_test(NAME passive_binding_trace COMMAND test_passive_binding_trace)
        foreach(suite passive_binding_journal passive_binding_extensions passive_binding_lookup)
            add_executable(test_${suite} tests/test_${suite}.cpp)
            target_include_directories(test_${suite} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
            target_compile_options(test_${suite} PRIVATE -Wall -Wextra -Wpedantic)
            target_link_libraries(test_${suite} PRIVATE Threads::Threads)
            add_test(NAME ${suite} COMMAND test_${suite})
        endforeach()
        add_executable(test_import_resource_receipt tests/test_import_resource_receipt.cpp)
        target_include_directories(test_import_resource_receipt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        target_compile_options(test_import_resource_receipt PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(test_import_resource_receipt PRIVATE Threads::Threads)
        add_test(NAME compositor_import_resource_receipt COMMAND test_import_resource_receipt)
        add_executable(binding_throughput_fixture tests/binding_throughput_fixture.cpp)
        target_include_directories(binding_throughput_fixture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        target_compile_options(binding_throughput_fixture PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(binding_throughput_fixture PRIVATE Threads::Threads)
        add_test(NAME passive_binding_throughput COMMAND binding_throughput_fixture --check)
    endif()
    add_test(NAME mask_endpoint COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mask_endpoint.py $<TARGET_FILE:mask_endpoint>)
    add_test(NAME mask_shader COMMAND ${Python3_EXECUTABLE}
        -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_mask_shader.py)
    add_test(NAME mask_bridge_collector COMMAND ${Python3_EXECUTABLE}
        -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_collect_mask_bridge.py)
    add_test(NAME mask_pc_bridge_collector COMMAND ${Python3_EXECUTABLE}
        -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_collect_pc_bridge.py)
    add_test(NAME mask_live_bridge_inspection COMMAND ${Python3_EXECUTABLE}
        -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_inspect_live_bridge.py)
    find_package(Vulkan QUIET)
    find_program(GLSLC glslc)
    find_program(SPIRV_AS spirv-as)
    find_program(SPIRV_VAL spirv-val)
    if(Vulkan_FOUND AND GLSLC AND SPIRV_AS AND SPIRV_VAL)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mrchroma_shaders.h
            COMMAND ${GLSLC} -O --target-env=vulkan1.1 ${CMAKE_CURRENT_SOURCE_DIR}/layer/key.vert
                -o ${CMAKE_CURRENT_BINARY_DIR}/key.vert.spv
            COMMAND ${GLSLC} -O --target-env=vulkan1.1 ${CMAKE_CURRENT_SOURCE_DIR}/layer/key.frag
                -o ${CMAKE_CURRENT_BINARY_DIR}/key.frag.spv
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spv.py
                ${CMAKE_CURRENT_BINARY_DIR}/mrchroma_shaders.h
                ${CMAKE_CURRENT_BINARY_DIR}/key.vert.spv ${CMAKE_CURRENT_BINARY_DIR}/key.frag.spv
            DEPENDS layer/key.vert layer/key.frag tools/embed_spv.py)
        add_library(VkLayer_mrchroma SHARED layer/mrchroma_layer.cpp
            ${CMAKE_CURRENT_BINARY_DIR}/mrchroma_shaders.h)
        target_include_directories(VkLayer_mrchroma PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${Vulkan_INCLUDE_DIRS})
        target_compile_options(VkLayer_mrchroma PRIVATE -Wall -Wno-missing-field-initializers -fvisibility=hidden)
        target_link_libraries(VkLayer_mrchroma PRIVATE pthread)
        add_executable(test_mask_gpu tests/test_mask_gpu.cpp)
        target_link_libraries(test_mask_gpu PRIVATE Vulkan::Vulkan Threads::Threads)
        target_compile_options(test_mask_gpu PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME mask_gpu COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_mask_gpu.py $<TARGET_FILE:test_mask_gpu>
            $<TARGET_FILE:test_mask_module_guard>)
        if(UNIX AND NOT CMAKE_CROSSCOMPILING)
            # Inert host fixture only: no layer linkage, installation or native
            # shader input in CI. Actual private GPU runs remain explicit.
            add_executable(test_native_fragment_ssa_gpu tests/native-fragment-ssa-gpu/fixture.cpp)
            target_link_libraries(test_native_fragment_ssa_gpu PRIVATE Vulkan::Vulkan)
            target_compile_options(test_native_fragment_ssa_gpu PRIVATE -Wall -Wextra -Wpedantic -Werror)
            # Compiled host-only helpers; execution requires private inputs
            # and an explicitly available validation layer, never a receiver.
            add_executable(test_native_fragment_atlas_gpu tests/native-fragment-atlas-gpu/fixture.cpp)
            target_link_libraries(test_native_fragment_atlas_gpu PRIVATE Vulkan::Vulkan)
            target_compile_options(test_native_fragment_atlas_gpu PRIVATE -Wall -Wextra -Wpedantic -Werror)
            add_executable(test_native_fragment_aperture tests/native-fragment-atlas-gpu/aperture_diagnostic.cpp)
            target_link_libraries(test_native_fragment_aperture PRIVATE Vulkan::Vulkan)
            target_compile_options(test_native_fragment_aperture PRIVATE -Wall -Wextra -Wpedantic -Werror)
            add_executable(test_native_fragment_ssbo_gpu tests/native-fragment-ssbo-gpu/fixture.cpp)
            target_link_libraries(test_native_fragment_ssbo_gpu PRIVATE Vulkan::Vulkan)
            target_compile_options(test_native_fragment_ssbo_gpu PRIVATE -Wall -Wextra -Wpedantic -Werror)
            add_executable(test_native_fragment_ssbo_cohort_gpu tests/native-fragment-ssbo-cohort-gpu/fixture.cpp)
            target_link_libraries(test_native_fragment_ssbo_cohort_gpu PRIVATE Vulkan::Vulkan)
            target_compile_options(test_native_fragment_ssbo_cohort_gpu PRIVATE -Wall -Wextra -Wpedantic -Werror)
            add_executable(test_native_fragment_set0_mirror_gpu tests/native-fragment-set0-mirror-gpu/fixture.cpp)
            target_link_libraries(test_native_fragment_set0_mirror_gpu PRIVATE Vulkan::Vulkan)
            target_compile_options(test_native_fragment_set0_mirror_gpu PRIVATE -Wall -Wextra -Wpedantic -Werror)
            add_executable(test_native_fragment_prefix_restore_gpu tests/native-fragment-prefix-restore-gpu/fixture.cpp)
            target_link_libraries(test_native_fragment_prefix_restore_gpu PRIVATE Vulkan::Vulkan)
            target_compile_options(test_native_fragment_prefix_restore_gpu PRIVATE -Wall -Wextra -Wpedantic -Werror)
            add_executable(test_native_fragment_owned_clone_gpu tests/native-fragment-owned-clone-gpu/fixture.cpp layer/pixel_ssbo_owned_clone.cpp)
            target_include_directories(test_native_fragment_owned_clone_gpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            target_link_libraries(test_native_fragment_owned_clone_gpu PRIVATE Vulkan::Vulkan)
            target_compile_options(test_native_fragment_owned_clone_gpu PRIVATE -Wall -Wextra -Wpedantic -Werror)
            add_executable(test_native_fragment_packed_output_gpu tests/native-fragment-packed-output-gpu/fixture.cpp)
            target_link_libraries(test_native_fragment_packed_output_gpu PRIVATE Vulkan::Vulkan)
            target_compile_options(test_native_fragment_packed_output_gpu PRIVATE -Wall -Wextra -Wpedantic -Werror)
            add_executable(test_native_fragment_r1_presenter_gpu tests/native-fragment-r1-presenter-gpu/fixture.cpp)
            target_link_libraries(test_native_fragment_r1_presenter_gpu PRIVATE Vulkan::Vulkan)
            target_compile_options(test_native_fragment_r1_presenter_gpu PRIVATE -Wall -Wextra -Wpedantic -Werror)
            add_executable(test_native_fragment_guarded_presenter_gpu tests/native-fragment-guarded-presenter-gpu/fixture.cpp)
            target_link_libraries(test_native_fragment_guarded_presenter_gpu PRIVATE Vulkan::Vulkan)
            target_compile_options(test_native_fragment_guarded_presenter_gpu PRIVATE -Wall -Wextra -Wpedantic -Werror)
            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/native_fragment_ssa_fixture.vert.spv
                COMMAND ${GLSLC} --target-env=vulkan1.1
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/native-fragment-ssa-gpu/fixture.vert
                    -o ${CMAKE_CURRENT_BINARY_DIR}/native_fragment_ssa_fixture.vert.spv
                DEPENDS tests/native-fragment-ssa-gpu/fixture.vert)
            add_custom_target(native_fragment_ssa_fixture_vertex ALL
                DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/native_fragment_ssa_fixture.vert.spv)
            add_test(NAME native_fragment_ssa_vertex COMMAND ${SPIRV_VAL} --target-env vulkan1.1
                ${CMAKE_CURRENT_BINARY_DIR}/native_fragment_ssa_fixture.vert.spv)
            # A layer-only ABI fixture uses the real loader/Mesa while remaining
            # unable to seal native compositor preparation or import entry scope.
            add_library(VkLayer_frame_compositor_private_fixture SHARED layer/passive_pipeline_trace.cpp)
            target_include_directories(VkLayer_frame_compositor_private_fixture PRIVATE ${Vulkan_INCLUDE_DIRS})
            target_compile_definitions(VkLayer_frame_compositor_private_fixture PRIVATE FRAME_MASK_PRIVATE_VULKAN_EXPORTS=1)
            target_compile_options(VkLayer_frame_compositor_private_fixture PRIVATE -Wall -Wextra -Wpedantic -fvisibility=hidden)
            target_link_options(VkLayer_frame_compositor_private_fixture PRIVATE -Wl,-Bsymbolic-functions
                "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/layer/compositor_raw_exports.map")
            set_property(TARGET VkLayer_frame_compositor_private_fixture APPEND PROPERTY LINK_DEPENDS
                ${CMAKE_CURRENT_SOURCE_DIR}/layer/compositor_raw_exports.map)
            target_link_libraries(VkLayer_frame_compositor_private_fixture PRIVATE Threads::Threads)
            add_executable(test_compositor_preload_vulkan tests/test_compositor_preload_vulkan.cpp)
            target_link_libraries(test_compositor_preload_vulkan PRIVATE Vulkan::Vulkan ${CMAKE_DL_LIBS})
            target_compile_options(test_compositor_preload_vulkan PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
            set(FRAME_MASK_TRACE_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}VkLayer_frame_compositor_private_fixture${CMAKE_SHARED_LIBRARY_SUFFIX}")
            set(FRAME_MASK_TRACE_FUNCTIONS "\n    \"functions\": {\n      \"vkGetInstanceProcAddr\": \"frame_compositor_raw_vkGetInstanceProcAddr\",\n      \"vkGetDeviceProcAddr\": \"frame_compositor_raw_vkGetDeviceProcAddr\",\n      \"vkNegotiateLoaderLayerInterfaceVersion\": \"frame_compositor_raw_vkNegotiateLoaderLayerInterfaceVersion\"\n    },")
            configure_file(layer/VkLayer_frame_mask_trace.json.in
                ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_frame_compositor_private_fixture.json @ONLY)
            add_test(NAME compositor_preload_implicit_vulkan COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compositor_preload_vulkan.py
                $<TARGET_FILE:test_compositor_preload_vulkan>
                $<TARGET_FILE:VkLayer_frame_compositor_private_fixture>
                ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_frame_compositor_private_fixture.json)
            set_tests_properties(compositor_preload_implicit_vulkan PROPERTIES TIMEOUT 120)
        endif()
        if(TARGET VkLayer_frame_mask_trace)
            add_test(NAME passive_pipeline_trace_gpu COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_passive_pipeline_trace.py
                $<TARGET_FILE:test_mask_gpu> ${CMAKE_CURRENT_BINARY_DIR})
        endif()
    else()
        message(FATAL_ERROR "Mask validation needs Vulkan headers, glslc and spirv-tools. For transport-only builds use -DBUILD_TESTING=OFF.")
    endif()
endif()
