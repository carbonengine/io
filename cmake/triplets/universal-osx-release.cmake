set(VCPKG_TARGET_ARCHITECTURE "arm64;x86_64")
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE "release")

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES "arm64;x86_64")

set(CARBON_BUILD_TYPE "Release")

if (PORT MATCHES "openssl")
    set(CARBON_x86_64_TRIPLET "arm64-osx-debug")
    set(CARBON_arm64_TRIPLET "x64-osx-debug")
    set(VCPKG_LIBRARY_LINKAGE static)
endif()

if (PORT MATCHES "libuv")
    set(VCPKG_CMAKE_CONFIGURE_OPTIONS "-DBUILD_TESTING=OFF")
endif()
