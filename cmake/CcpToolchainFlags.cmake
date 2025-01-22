set(CCP_TOOLSET ${CMAKE_GENERATOR_TOOLSET})

## Take manual control over these flags: https://gitlab.kitware.com/cmake/cmake/-/issues/19084
set(CMAKE_CXX_FLAGS_DEBUG "")
set(CMAKE_CXX_FLAGS_TRINITYDEV "")
set(CMAKE_C_FLAGS_TRINITYDEV "")
set(CMAKE_SHARED_LINKER_FLAGS_TRINITYDEV "")
set(CMAKE_EXE_LINKER_FLAGS_TRINITYDEV "")

set(MATH_OPTIMIZE_FLAG "/fp:fast")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
set(WIN_SDK_VERSION "10.0.17763.0")
set(CMAKE_SYSTEM_VERSION ${WIN_SDK_VERSION} CACHE STRING INTERNAL FORCE)
set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION ${WIN_SDK_VERSION} CACHE STRING INTERNAL FORCE)

# Avoid using MultiThreadedDebugDLL, which we don't support, this is ignored on non-MSVC platforms
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING INTERNAL FORCE)

# we only want to disable this on windows, and there's no way to do it from the triplet file unfortunately
if(MSVC)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
    set(MATH_OPTIMIZE_FLAG "/fp:fast")
elseif(APPLE)
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "13")
        set(MATH_OPTIMIZE_FLAG -ffast-math -fhonor-infinities -fhonor-nans)
    else()
        set(MATH_OPTIMIZE_FLAG -ffast-math -ffp-model=fast -fhonor-infinities -fhonor-nans)
    endif()
endif()
# Require out-of-source builds
file(TO_CMAKE_PATH "${PROJECT_BINARY_DIR}/CMakeLists.txt" LOC_PATH)
if(EXISTS "${LOC_PATH}")
    message(FATAL_ERROR "You cannot build in a source directory (or any directory with a CMakeLists.txt file). Please make a build subdirectory. Feel free to remove CMakeCache.txt and CMakeFiles.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OBJCXX_VISIBILITY_PRESET hidden)
set(CMAKE_XCODE_GENERATE_SCHEME ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)