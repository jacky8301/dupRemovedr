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

# Source files
set(SOURCES
    src/main.cpp
    src/md5.cpp
    src/scanner.cpp
)

set(HEADERS
    src/md5.h
    src/scanner.h
    src/resource.h
)

# Resource file
set(RESOURCES
    src/resource.rc
)

add_executable(${PROJECT_NAME} WIN32
    ${SOURCES}
    ${HEADERS}
    ${RESOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

# Link Windows libraries
target_link_libraries(${PROJECT_NAME}
    advapi32
    comctl32
    shell32
    ole32
    user32
    gdi32
)

# Set output directory
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin"
)

# Enable common compiler flags for MSVC
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /utf-8)
    # Statically link runtime to avoid requiring VC++ redist
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
