cmake_minimum_required(VERSION 3.16)
# Also support configuring this tool directly, without a consumer checkout.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(shm_web LANGUAGES CXX)
    # Prevent the library from adding this directory again in standalone builds.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(ROCOS_SHM_BUILD_WEB OFF)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." "${CMAKE_CURRENT_BINARY_DIR}/rocos-shm")
    include(CTest)
endif()

find_package(Threads REQUIRED)

add_executable(shm_web_server shm_web_server.cpp)

target_compile_features(shm_web_server PRIVATE cxx_std_17)
set_target_properties(shm_web_server PROPERTIES CXX_EXTENSIONS OFF)

target_compile_options(shm_web_server PRIVATE -Wall -Wextra)

target_link_libraries(shm_web_server PRIVATE
    rocos::shm
    Threads::Threads
    rt
)

# Ship offline UI, graph library, font weights, and their license notices together.
set(SHM_WEB_ASSETS
    "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/value_decoder.mjs"
    "${CMAKE_CURRENT_SOURCE_DIR}/topology.mjs"
    "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics.mjs"
    "${CMAKE_CURRENT_SOURCE_DIR}/controls.mjs"
    "${CMAKE_CURRENT_SOURCE_DIR}/AlibabaPuHuiTi-3-55-Regular.woff2"
    "${CMAKE_CURRENT_SOURCE_DIR}/AlibabaPuHuiTi-3-65-Medium.woff2"
    "${CMAKE_CURRENT_SOURCE_DIR}/AlibabaPuHuiTi-3-75-SemiBold.woff2"
    "${CMAKE_CURRENT_SOURCE_DIR}/AlibabaPuHuiTi-3-85-Bold.woff2"
    "${CMAKE_CURRENT_SOURCE_DIR}/AlibabaPuHuiTi-3.0-LICENSE.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/font-sources.json"
)
add_custom_target(shm_web_assets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:shm_web_server>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHM_WEB_ASSETS} "$<TARGET_FILE_DIR:shm_web_server>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/vendor" "$<TARGET_FILE_DIR:shm_web_server>/vendor"
    COMMENT "Copying frontend assets next to shm_web_server"
)
add_custom_command(TARGET shm_web_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHM_WEB_ASSETS} "$<TARGET_FILE_DIR:shm_web_server>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/vendor" "$<TARGET_FILE_DIR:shm_web_server>/vendor"
)

if(BUILD_TESTING)
    add_executable(shm_web_output_test output_write_test.cpp)
    target_compile_features(shm_web_output_test PRIVATE cxx_std_17)
    set_target_properties(shm_web_output_test PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_options(shm_web_output_test PRIVATE -Wall -Wextra)
    target_link_libraries(shm_web_output_test PRIVATE rocos::shm Threads::Threads rt)
    add_test(NAME shm_web_output COMMAND shm_web_output_test)
    add_executable(shm_web_control_test control_test.cpp)
    target_compile_features(shm_web_control_test PRIVATE cxx_std_17)
    target_compile_options(shm_web_control_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(shm_web_control_test PRIVATE rocos::shm Threads::Threads rt)
    add_test(NAME shm_web_control COMMAND shm_web_control_test)
    set_tests_properties(shm_web_control PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()
