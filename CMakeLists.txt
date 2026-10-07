cmake_minimum_required(VERSION 3.10)
project(PersistKV CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(PKV_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(PKV_TSAN "Build with ThreadSanitizer" OFF)

if(MSVC)
  add_compile_options(/W4)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
  if(PKV_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
  endif()
  if(PKV_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
  endif()
endif()

add_library(persistkv STATIC
  src/btree.cpp
  src/crc32.cpp
  src/db.cpp
  src/failpoint.cpp
  src/file.cpp
  src/pager.cpp
  src/sync.cpp
  src/wal.cpp)
target_include_directories(persistkv PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(persistkv PUBLIC Threads::Threads)

foreach(tool kvcli bench)
  add_executable(${tool} tools/${tool}.cpp)
  target_link_libraries(${tool} PRIVATE persistkv)
endforeach()

foreach(test unit_tests crash_test)
  add_executable(${test} tests/${test}.cpp)
  target_link_libraries(${test} PRIVATE persistkv)
endforeach()

enable_testing()
add_test(NAME unit_tests COMMAND unit_tests WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME crash_test COMMAND crash_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
