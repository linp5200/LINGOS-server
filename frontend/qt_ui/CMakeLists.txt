cmake_minimum_required(VERSION 3.16)
# LING OS Qt6 桌面前端（0.4.3——先生裁决：取代 GTK3 lingos_gui；双形态之一）
# 依赖: Qt6 Widgets/Network（构建机/目标机需 qt6-base-dev）
# 用法: mkdir build && cd build && cmake .. && make
project(lingos_ui VERSION 0.4.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

set(SOURCES
    src/main.cpp
    src/api_client.cpp
    src/main_window.cpp
    src/terrain_painter.cpp
)
set(HEADERS
    src/api_client.h
    src/main_window.h
    src/terrain_painter.h
)

add_executable(lingos_ui ${SOURCES} ${HEADERS})
target_link_libraries(lingos_ui PRIVATE Qt6::Widgets Qt6::Network)
target_include_directories(lingos_ui PRIVATE src)

install(TARGETS lingos_ui RUNTIME DESTINATION bin)
