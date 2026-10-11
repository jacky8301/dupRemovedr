cmake_minimum_required(VERSION 3.16)
project(dupRemover LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 启用 Unicode。
add_definitions(-DUNICODE -D_UNICODE)

# 查找 Windows SDK。
if(NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION "10.0")
endif()

set(HEADERS
    src/md5.h
    src/scanner.h
    src/image_preview.h
    src/resource.h
)

add_library(dupRemoverCore STATIC
    src/md5.cpp
    src/scanner.cpp
    src/image_preview.cpp
    ${HEADERS}
)

target_include_directories(dupRemoverCore PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(dupRemoverCore PUBLIC windowscodecs ole32)

# 资源文件。
set(RESOURCES
    src/resource.rc
)

add_executable(${PROJECT_NAME} WIN32
    src/main.cpp
    ${RESOURCES}
)

# 对话框模块独立编译，应用和测试复用同一份实现。
add_library(dupRemoverApp STATIC
    src/dialog.cpp
    src/dialog_view.cpp
    src/dialog_preview.cpp
    src/dialog_results.cpp
    src/dialog_scan.cpp
    src/dialog_cleanup.cpp
    src/dialog_commands.cpp
    src/context_menu.cpp
    src/cleanup.cpp
    src/dialog.h
    src/context_menu.h
    src/cleanup.h
)
target_link_libraries(dupRemoverApp PUBLIC
    dupRemoverCore
    advapi32
    comctl32
    shell32
    ole32
    user32
    gdi32
    uxtheme
)
target_link_libraries(${PROJECT_NAME} PRIVATE dupRemoverApp)

# 设置输出目录。
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin"
)

# 设置 MSVC 编译选项。
if(MSVC)
    target_compile_options(dupRemoverCore PRIVATE /W4 /utf-8)
    target_compile_options(dupRemoverApp PRIVATE /W4 /utf-8)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /utf-8)
    # 静态链接运行库，避免额外安装 VC++ 运行时。
    set_target_properties(dupRemoverCore dupRemoverApp ${PROJECT_NAME} PROPERTIES
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

    # 注入目录枚举故障，验证部分读取失败时不会发布成功结果。
    add_executable(test_scan_errors test/test_scan_errors.cpp)
    target_link_libraries(test_scan_errors PRIVATE dupRemoverCore)
    add_test(NAME scan_errors COMMAND test_scan_errors)

    # 回收站替身验证后台清理及界面响应，不操作真实文件。
    add_executable(test_cleanup test/test_cleanup.cpp ${RESOURCES})
    target_link_libraries(test_cleanup PRIVATE dupRemoverApp)
    add_test(NAME cleanup COMMAND test_cleanup)

    # 独立样本上的真实回收测试，覆盖关联资源与原路径替换。
    add_executable(test_cleanup_safety test/test_cleanup_safety.cpp ${RESOURCES})
    target_link_libraries(test_cleanup_safety PRIVATE dupRemoverApp)
    add_test(NAME cleanup_safety COMMAND test_cleanup_safety)

    add_executable(test_preview test/test_preview.cpp)
    target_link_libraries(test_preview PRIVATE dupRemoverCore gdiplus)
    add_test(NAME preview COMMAND test_preview)

    # 性能基准单独运行，避免常规测试生成大体积数据。
    add_executable(benchmark_scanner EXCLUDE_FROM_ALL test/benchmark_scanner.cpp)
    target_link_libraries(benchmark_scanner PRIVATE dupRemoverCore)

    # 隐藏窗口视觉检查需要先准备性能基准数据集。
    add_executable(test_ui EXCLUDE_FROM_ALL test/test_ui.cpp ${RESOURCES})
    target_link_libraries(test_ui PRIVATE dupRemoverApp gdiplus)

    if(MSVC)
        target_compile_options(test_md5_vectors PRIVATE /W4 /utf-8)
        target_compile_options(test_scanner PRIVATE /W4 /utf-8)
        target_compile_options(test_scan_errors PRIVATE /W4 /utf-8)
        target_compile_options(benchmark_scanner PRIVATE /W4 /utf-8)
        target_compile_options(test_ui PRIVATE /W4 /utf-8)
        target_compile_options(test_cleanup PRIVATE /W4 /utf-8)
        target_compile_options(test_cleanup_safety PRIVATE /W4 /utf-8)
        target_compile_options(test_preview PRIVATE /W4 /utf-8)
        set_target_properties(test_md5_vectors test_scanner test_scan_errors benchmark_scanner test_ui test_cleanup test_cleanup_safety test_preview PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()
endif()
