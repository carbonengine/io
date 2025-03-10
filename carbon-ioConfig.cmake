include(CMakeFindDependencyMacro)

set(CarbonIO_INCLUDE_DIR "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
set(CarbonIO_Libraries _socket _ssl _select)

if(APPLE)
    set(_SHARED_LIBRARY_SUFFIX ".so")
else()
    set(_SHARED_LIBRARY_SUFFIX ${CMAKE_SHARED_LIBRARY_SUFFIX})
endif()

if(APPLE)
    set_target_properties(CarbonIO PROPERTIES
            IMPORTED_LOCATION "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/_carbonsocket.so"
    )
elseif(WIN32)
    set_target_properties(CarbonIO PROPERTIES
            IMPORTED_LOCATION "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/_carbonsocket.pyd"
    )
else()
    message(FATAL_ERROR "carbon-io not supported on platform.")
endif()

set_target_properties(CarbonIO PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CarbonIO_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES ${CarbonIO_Libraries}
)

# Please specify all of this projects transitive dependencies here with calls
# In order for a consuming cmake project system to locate any transitive dependencies of this project, they must be
# specified here in a call to find_dependency(...)
#
# Example:
#
# My project CMakeLists.txt file looks like this:
# ------------------------
# MyProject/CMakeLists.txt
# ------------------------
#
# find_package(a CONFIG NO_CMAKE_PATH REQUIRED)
# find_package(b CONFIG NO_CMAKE_PATH REQUIRED)
# find_package(c CONFIG NO_CMAKE_PATH REQUIRED)
# target_link_libraries(MyProjectTarget PRIVATE package_a PUBLIC package_b INTERFACE package_c)
# . . .
#

# Then the myprojectConfig file (this file) looks like this:
#--------------------------------
# MyProject/myprojectConfig.cmake
# -------------------------------
#
# include(CMakeFindDependencyMacro)
# include(${CMAKE_CURRENT_LIST_DIR}/myproject.cmake)
#
# find_dependency(b CONFIG NO_CMAKE_PATH REQUIRED)
# find_dependency(c CONFIG NO_CMAKE_PATH REQUIRED)
#
