# These portable modules are deliberately not linked into the installed loader,
# receiver, native renderer or a Steam process. Their explicit trust ports are
# not proof of a live provider, authenticated peer or safe image composition.
add_library(sfnp_stream_bridge STATIC
    bridge/stream_bridge_protocol.cpp
    bridge/stream_bridge_auth.cpp
    bridge/stream_bridge_frame.cpp
    bridge/hevc_bridge_sei.cpp
    pc/stream-bridge-sender.cpp
    receiver/stream_bridge_receiver.cpp)
target_include_directories(sfnp_stream_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(sfnp_stream_bridge PRIVATE /W4 /WX)
else()
    target_compile_options(sfnp_stream_bridge PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Cold explicit preparation and bounded control I/O; not installed or autostarted.
find_package(Threads REQUIRED)
add_library(sfnp_stream_bridge_control STATIC
    bridge/stream_bridge_credentials.cpp bridge/stream_bridge_control.cpp)
target_link_libraries(sfnp_stream_bridge_control PUBLIC sfnp_stream_bridge Threads::Threads)
if(WIN32)
    target_link_libraries(sfnp_stream_bridge_control PRIVATE ws2_32 advapi32 bcrypt)
endif()
if(MSVC)
    target_compile_options(sfnp_stream_bridge_control PRIVATE /W4 /WX)
else()
    target_compile_options(sfnp_stream_bridge_control PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_stream_bridge_control tests/test_stream_bridge_control.cpp)
    target_link_libraries(test_stream_bridge_control PRIVATE sfnp_stream_bridge_control)
    target_compile_options(test_stream_bridge_control PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME stream_bridge_control COMMAND test_stream_bridge_control)
    endif()
endif()

add_executable(stream_bridge_au EXCLUDE_FROM_ALL tools/stream_bridge_au.cpp)
target_link_libraries(stream_bridge_au PRIVATE sfnp_stream_bridge)
if(NOT MSVC)
    target_compile_options(stream_bridge_au PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING)
    foreach(suite stream_bridge_protocol stream_bridge_auth stream_bridge_authenticated stream_bridge_frame hevc_bridge_sei stream_bridge_sender stream_bridge_receiver)
        add_executable(test_${suite} tests/test_${suite}.cpp)
        target_link_libraries(test_${suite} PRIVATE sfnp_stream_bridge)
        if(NOT MSVC)
            target_compile_options(test_${suite} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        if(NOT CMAKE_CROSSCOMPILING)
            add_test(NAME ${suite} COMMAND test_${suite})
        endif()
    endforeach()
    if(NOT CMAKE_CROSSCOMPILING)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME steam_link_setup COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_steam_link_setup.py)
        add_test(NAME setup_bootstrap COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_setup_bootstrap.py)
        add_test(NAME setup_ui COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_setup_ui.py)
        add_test(NAME setup_dependencies COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_setup_dependencies.py)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|aarch64|arm64)$")
        find_package(Threads REQUIRED)
        add_executable(test_stream_bridge_native_decode
            receiver/stream_bridge_native_decode.cpp tests/stream_bridge_native_decode_test.cpp)
        target_compile_options(test_stream_bridge_native_decode PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_link_libraries(test_stream_bridge_native_decode PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
            enable_language(ASM)
            target_sources(test_stream_bridge_native_decode PRIVATE receiver/stream_bridge_native_decode_entry.S)
            add_executable(test_stream_bridge_native_decode_entry
                tests/stream_bridge_native_decode_test.cpp receiver/stream_bridge_native_decode_entry.S)
            target_compile_definitions(test_stream_bridge_native_decode_entry PRIVATE SFNP_NATIVE_DECODE_ENTRY_TEST=1)
            target_compile_options(test_stream_bridge_native_decode_entry PRIVATE -Wall -Wextra -Wpedantic -Werror)
            if(NOT CMAKE_CROSSCOMPILING)
                add_test(NAME stream_bridge_native_decode_entry COMMAND test_stream_bridge_native_decode_entry)
            endif()
        endif()
        if(NOT CMAKE_CROSSCOMPILING)
            add_test(NAME stream_bridge_native_decode COMMAND test_stream_bridge_native_decode)
        endif()
    endif()
endif()

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        add_executable(test_native_stream_coverage_gpu EXCLUDE_FROM_ALL
            tests/native-fragment-stream-coverage-gpu/fixture.cpp)
        target_compile_options(test_native_stream_coverage_gpu PRIVATE
            -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers)
        target_link_libraries(test_native_stream_coverage_gpu PRIVATE Vulkan::Vulkan)
        # Requires private installed shader inputs and active validation.
        # No public fixture can establish an actual selected-video lease.
    endif()
endif()
