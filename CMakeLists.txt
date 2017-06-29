project(composition-tool)

cmake_minimum_required(VERSION 3.0)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")

find_package(LLVM 3.7)
find_package(Clang 3.7)

# I know, I know, those flags should not be added like this
add_definitions("-std=c++14 -Wall -Wextra")

add_executable(tooling_sample tooling_sample.cpp)

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})

target_link_libraries(tooling_sample ${LLVM_LIBS} ${CLANG_LIBS})
