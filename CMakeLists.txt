cmake_minimum_required(VERSION 3.10)
project(rocos_shm VERSION 2.0.0 LANGUAGES CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "rocos-shm ABI v2 requires Linux x86_64")
endif()
find_package(Threads REQUIRED)
add_library(rocos_shm INTERFACE)
add_library(rocos::shm ALIAS rocos_shm)
set_target_properties(rocos_shm PROPERTIES EXPORT_NAME shm)
target_compile_features(rocos_shm INTERFACE cxx_std_11)
target_include_directories(rocos_shm INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(rocos_shm INTERFACE Threads::Threads rt)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS rocos_shm EXPORT rocos_shm-targets)
install(EXPORT rocos_shm-targets NAMESPACE rocos:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rocos_shm)
configure_package_config_file(cmake/rocos_shm-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/rocos_shm-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rocos_shm)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/rocos_shm-config-version.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rocos_shm-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/rocos_shm-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rocos_shm)
option(ROCOS_SHM_BUILD_TESTS "Build offline v2 IPC tests" OFF)
if(ROCOS_SHM_BUILD_TESTS)
    enable_testing()
    add_executable(rocos_shm_test tests/shm_test.cpp)
    target_link_libraries(rocos_shm_test PRIVATE rocos::shm)
    target_compile_options(rocos_shm_test PRIVATE -Wall -Wextra -Werror -pedantic)
    add_test(NAME rocos_shm_v2 COMMAND rocos_shm_test)
    set_tests_properties(rocos_shm_v2 PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()

# Optional tools do not change the library's C++11 requirement.
option(ROCOS_SHM_BUILD_WEB "Build the shared-memory Web monitor (C++17)" ON)
if(ROCOS_SHM_BUILD_WEB)
    include(CTest)
    add_subdirectory(tools/shm_web)
endif()
