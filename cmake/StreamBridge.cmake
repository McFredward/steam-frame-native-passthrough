# These portable modules are deliberately not linked into the installed loader,
# receiver, native renderer or a Steam process. Their explicit trust ports are
# not proof of a live provider, authenticated peer or safe image composition.
add_library(sfnp_stream_bridge STATIC
    bridge/stream_bridge_protocol.cpp
    bridge/hevc_bridge_sei.cpp
    pc/stream-bridge-sender.cpp
    receiver/stream_bridge_receiver.cpp)
target_include_directories(sfnp_stream_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(sfnp_stream_bridge PRIVATE /W4 /WX)
else()
    target_compile_options(sfnp_stream_bridge PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(stream_bridge_au EXCLUDE_FROM_ALL tools/stream_bridge_au.cpp)
target_link_libraries(stream_bridge_au PRIVATE sfnp_stream_bridge)
if(NOT MSVC)
    target_compile_options(stream_bridge_au PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING)
    foreach(suite stream_bridge_protocol hevc_bridge_sei stream_bridge_sender stream_bridge_receiver)
        add_executable(test_${suite} tests/test_${suite}.cpp)
        target_link_libraries(test_${suite} PRIVATE sfnp_stream_bridge)
        if(NOT MSVC)
            target_compile_options(test_${suite} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        if(NOT CMAKE_CROSSCOMPILING)
            add_test(NAME ${suite} COMMAND test_${suite})
        endif()
    endforeach()
endif()
