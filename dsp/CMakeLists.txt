cmake_minimum_required(VERSION 3.20)
project(tab_leveler_dsp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(leveler STATIC src/leveler.cpp)
target_include_directories(leveler PUBLIC include)
if(MSVC)
  target_compile_options(leveler PRIVATE /W4 /WX)
else()
  target_compile_options(leveler PRIVATE -Wall -Wextra -Werror)
endif()

add_executable(test_leveler tests/test_leveler.cpp)
target_link_libraries(test_leveler PRIVATE leveler)

enable_testing()
add_test(NAME dsp COMMAND test_leveler)
