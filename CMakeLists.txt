cmake_minimum_required(VERSION 3.10.0)
project(polyvox VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS True)

get_filename_component(WORKSPACE_DIR "${CMAKE_SOURCE_DIR}" ABSOLUTE)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${WORKSPACE_DIR}/bin")
# set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${WORKSPACE_DIR}/lib")
# set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${WORKSPACE_DIR}/lib")

# 包含头文件目录
include_directories(include)

# 主文件
file(GLOB MAIN_FILE 
    src/main.cpp
)

# 本地库实现
file(GLOB SRC_FILES 
    include/local/*.cpp
    include/local/*.h # <--- 添加.h以确保IDE能看到
)

add_executable(polyvox ${MAIN_FILE} ${SRC_FILES})

if(WIN32)
    target_link_libraries(polyvox PRIVATE shell32)
endif()

# 第三方库实现
file(GLOB THIRD_PARTY_SRC 
    include/third_party/*.cpp
)

target_sources(polyvox PRIVATE ${THIRD_PARTY_SRC})
