cmake_minimum_required(VERSION 3.16)
project(dupRemover LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable Unicode
add_definitions(-DUNICODE -D_UNICODE)

# Find Windows SDK
if(NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION "10.0")
endif()

set(HEADERS
    src/md5.h
    src/scanner.h
    src/resource.h
)

add_library(dupRemoverCore STATIC
    src/md5.cpp
    src/scanner.cpp
    ${HEADERS}
)

target_include_directories(dupRemoverCore PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

# Resource file
set(RESOURCES
    src/resource.rc
)

add_executable(${PROJECT_NAME} WIN32
    src/main.cpp
    ${RESOURCES}
)

target_link_libraries(${PROJECT_NAME} PRIVATE dupRemoverCore)

# Link Windows libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    advapi32
    comctl32
    shell32
    ole32
    user32
    gdi32
    uxtheme
)

# Set output directory
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin"
)

# Enable common compiler flags for MSVC
if(MSVC)
    target_compile_options(dupRemoverCore PRIVATE /W4 /utf-8)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /utf-8)
    # Statically link runtime to avoid requiring VC++ redist
    set_target_properties(dupRemoverCore ${PROJECT_NAME} PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

include(CTest)

if(BUILD_TESTING)
    add_executable(test_md5_vectors test/test_vectors.cpp)
    target_link_libraries(test_md5_vectors PRIVATE dupRemoverCore)
    add_test(NAME md5_vectors COMMAND test_md5_vectors)

    add_executable(test_scanner test/test_scanner.cpp)
    target_link_libraries(test_scanner PRIVATE dupRemoverCore)
    add_test(NAME scanner COMMAND test_scanner)

    # 性能基准单独运行，避免常规测试生成大体积数据。
    add_executable(benchmark_scanner EXCLUDE_FROM_ALL test/benchmark_scanner.cpp)
    target_link_libraries(benchmark_scanner PRIVATE dupRemoverCore)

    # 隐藏窗口视觉检查需要先准备性能基准数据集。
    add_executable(test_ui EXCLUDE_FROM_ALL test/test_ui.cpp ${RESOURCES})
    target_link_libraries(test_ui PRIVATE dupRemoverCore
        advapi32 comctl32 shell32 ole32 user32 gdi32 uxtheme gdiplus)

    if(MSVC)
        target_compile_options(test_md5_vectors PRIVATE /W4 /utf-8)
        target_compile_options(test_scanner PRIVATE /W4 /utf-8)
        target_compile_options(benchmark_scanner PRIVATE /W4 /utf-8)
        target_compile_options(test_ui PRIVATE /W4 /utf-8)
        set_target_properties(test_md5_vectors test_scanner benchmark_scanner test_ui PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()
endif()
