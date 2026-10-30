cmake_minimum_required(VERSION 3.16)
project(nds_hackathon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(nds_framework
    src/json.cpp
    src/framework.cpp
)
target_include_directories(nds_framework PUBLIC include)
target_compile_options(nds_framework PRIVATE -Wall -Wextra -Wpedantic)

add_executable(nds_baseline src/baseline_main.cpp)
target_link_libraries(nds_baseline PRIVATE nds_framework)

add_executable(nds_validate src/validate_main.cpp)
target_link_libraries(nds_validate PRIVATE nds_framework)

add_executable(nds_generate src/generate_main.cpp)
target_link_libraries(nds_generate PRIVATE nds_framework)

add_executable(nds_player src/player_main.cpp player/solution.cpp)
target_link_libraries(nds_player PRIVATE nds_framework)

add_executable(nds_tests tests/framework_test.cpp)
target_link_libraries(nds_tests PRIVATE nds_framework)

enable_testing()
add_test(NAME framework_tests COMMAND nds_tests ${CMAKE_CURRENT_SOURCE_DIR}/data)
