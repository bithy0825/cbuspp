cmake_minimum_required(VERSION 3.20)
project(cbuspp VERSION 0.1.0 LANGUAGES CXX)

# Setup Vcpkg toolchain if available
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED VCPKG_ROOT AND EXISTS "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "Vcpkg toolchain file")
  elseif(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "Vcpkg toolchain file")
  endif()
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_TESTS "Build tests" ON)

add_subdirectory(src)

if(BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(test)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cbusppConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(EXPORT cbusppTargets
    FILE cbusppConfig.cmake
    NAMESPACE cbuspp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cbuspp
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/cbusppConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cbuspp
)
