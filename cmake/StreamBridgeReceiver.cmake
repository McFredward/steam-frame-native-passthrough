# Explicit two-sided runtime; the setup manager installs it only after exact
# package/runtime admission and the user's selected Install action.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The stream bridge receiver requires Linux")
endif()
enable_language(C)
find_path(SFNP_VULKAN_HEADERS vulkan/vulkan.h REQUIRED)
find_package(Threads REQUIRED)
set_target_properties(sfnp_stream_bridge sfnp_stream_bridge_control PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(VkLayer_steam_link_bridge SHARED
    layer/stream_bridge_vulkan.cpp layer/stream_bridge_renderer.cpp
    layer/stream_bridge_process.cpp layer/stream_bridge_coverage_gpu.cpp
    receiver/stream_bridge_native_runtime.cpp receiver/stream_bridge_native_decode.cpp
    receiver/stream_bridge_native_loader.cpp receiver/stream_bridge_native_images.cpp
    receiver/stream_bridge_local.cpp receiver/stream_bridge_camera_client.cpp)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    enable_language(ASM)
    target_sources(VkLayer_steam_link_bridge PRIVATE receiver/stream_bridge_native_decode_entry.S)
endif()
target_include_directories(VkLayer_steam_link_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SFNP_VULKAN_HEADERS})
target_compile_options(VkLayer_steam_link_bridge PRIVATE
    -Wall -Wextra -Wpedantic -Werror -Wno-missing-field-initializers -fvisibility=hidden)
target_link_options(VkLayer_steam_link_bridge PRIVATE -Wl,--no-undefined -Wl,-z,nodelete
    -Wl,-Bsymbolic-functions "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/layer/stream_bridge_exports.map")
target_link_libraries(VkLayer_steam_link_bridge PRIVATE sfnp_stream_bridge_control Threads::Threads ${CMAKE_DL_LIBS})
# Keep the companion's C++ runtime private when it shares a loader process
# with Steam's own runtime. Only the three Vulkan entry points are exported.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(VkLayer_steam_link_bridge PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL)
endif()
add_library(sfnp_stream_launch SHARED receiver/compositor_launch_selector.c receiver/stream_bridge_launch_config.c)
target_compile_definitions(sfnp_stream_launch PRIVATE FRAME_LAUNCH_RELEASE_BRIDGE=1)
target_compile_options(sfnp_stream_launch PRIVATE -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden)
target_link_options(sfnp_stream_launch PRIVATE -Wl,--no-undefined -Wl,-z,nodelete)
target_link_libraries(sfnp_stream_launch PRIVATE ${CMAKE_DL_LIBS})
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(test_stream_bridge_camera_client
        tests/stream-bridge-camera-client/client_tests.cpp receiver/stream_bridge_camera_client.cpp)
    target_include_directories(test_stream_bridge_camera_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(test_stream_bridge_camera_client PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(test_stream_bridge_camera_client PRIVATE Threads::Threads)
    add_test(NAME stream_bridge_camera_client COMMAND test_stream_bridge_camera_client)
endif()
