cmake_minimum_required(VERSION 3.21)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake CACHE STRING "Vcpkg toolchain file")

project(Client C CXX)

find_package(FFMPEG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_executable(RockchipPlayer
main.cpp)

set_property(TARGET RockchipPlayer PROPERTY CXX_STANDARD 17)

target_include_directories(RockchipPlayer PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(RockchipPlayer PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(RockchipPlayer ${FFMPEG_LIBRARIES})
target_link_libraries(RockchipPlayer EGL)
target_link_libraries(RockchipPlayer GLESv2)
target_link_libraries(RockchipPlayer glfw)
target_link_libraries(RockchipPlayer imgui::imgui)
target_link_libraries(RockchipPlayer pthread)
target_link_libraries(RockchipPlayer rga)
target_link_libraries(RockchipPlayer /usr/lib/aarch64-linux-gnu/librockchip_mpp.so)
