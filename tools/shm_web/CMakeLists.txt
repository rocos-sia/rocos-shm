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

# 把前端资源复制到可执行文件旁，方便直接运行。
add_custom_target(shm_web_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
        "$<TARGET_FILE_DIR:shm_web_server>/index.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/value_decoder.mjs"
        "$<TARGET_FILE_DIR:shm_web_server>/value_decoder.mjs"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/AlibabaPuHuiTi-3-55-Regular.woff2"
        "$<TARGET_FILE_DIR:shm_web_server>/AlibabaPuHuiTi-3-55-Regular.woff2"
    COMMENT "Copying frontend assets next to shm_web_server"
)

add_custom_command(TARGET shm_web_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
        "$<TARGET_FILE_DIR:shm_web_server>/index.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/value_decoder.mjs"
        "$<TARGET_FILE_DIR:shm_web_server>/value_decoder.mjs"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/AlibabaPuHuiTi-3-55-Regular.woff2"
        "$<TARGET_FILE_DIR:shm_web_server>/AlibabaPuHuiTi-3-55-Regular.woff2"
)

if(BUILD_TESTING)
    add_executable(shm_web_output_test output_write_test.cpp)
    target_compile_features(shm_web_output_test PRIVATE cxx_std_17)
    set_target_properties(shm_web_output_test PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_options(shm_web_output_test PRIVATE -Wall -Wextra)
    target_link_libraries(shm_web_output_test PRIVATE rocos::shm Threads::Threads rt)
    add_test(NAME shm_web_output COMMAND shm_web_output_test)
endif()
