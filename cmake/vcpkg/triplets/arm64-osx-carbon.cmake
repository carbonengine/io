set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)

set(VCPKG_ENV_PASSTHROUGH CCP_EVE_PERFORCE_BRANCH_PATH)

# Explicitly set the minimum macOS version we target; otherwise it defaults to whatever version
# we are building on, but we want to stick to our policy of supporting the last three releases.
set(VCPKG_OSX_DEPLOYMENT_TARGET 10.14)
message(STATUS "Building for minimum macOS version: ${CMAKE_OSX_DEPLOYMENT_TARGET}")

# Explicit architecture required to support x64 builds on arm64 (Apple silicon)
set(VCPKG_OSX_ARCHITECTURES "arm64;x86_64")
message(STATUS "Building for ${CMAKE_OSX_ARCHITECTURES} architecture")

set(COMPILE_FLAGS_STRING "${VCPKG_CXX_FLAGS}")
# adjust warning settings for all our projects, but do not treat them as errors just yet.
string(APPEND COMPILE_FLAGS_STRING " -Wall")

# we want to use the two ones below once we're good with -Wall
#    string(APPEND COMPILE_FLAGS_STRING "-Wpedantic")
#    string(APPEND COMPILE_FLAGS_STRING "-Wextra")

# We're using a lot of MSVC specific pragmas in our codebase, so we silence those warnings until we got around to
# cleaning them up
string(APPEND COMPILE_FLAGS_STRING " -Wno-unknown-pragmas")
# There's a surprising amount of unused functions, we need to investigate this deeper at one point
string(APPEND COMPILE_FLAGS_STRING " -Wno-unused-function")
# Ditto, much like the functions there are also a lot of unused variables it appears
string(APPEND COMPILE_FLAGS_STRING " -Wno-unused-variable")
# We've not been very good at keeping order
string(APPEND COMPILE_FLAGS_STRING " -Wno-reorder")
# -Wmissing-braces should only be used by C / ObjectiveC, but for some reason it shows up for our C++ code, too.
string(APPEND COMPILE_FLAGS_STRING " -Wno-missing-braces")
# Manually add debug symbols to builds
string(APPEND COMPILE_FLAGS_STRING " -g")

set(VCPKG_CXX_FLAGS ${COMPILE_FLAGS_STRING})
set(VCPKG_C_FLAGS "${VCPKG_CXX_FLAGS}")

get_filename_component(toolchain_settings_file "${CMAKE_CURRENT_LIST_DIR}/../CcpToolchainFlags.cmake" ABSOLUTE)
set(VCPKG_CMAKE_CONFIGURE_OPTIONS "-DCMAKE_PROJECT_INCLUDE=\"${toolchain_settings_file}\"")
