cmake_minimum_required(VERSION 3.15)

if(NOT UNIX)
  message(FATAL_ERROR "unix only")
endif()

find_program(CLANG_COMPILER clang)
if(CLANG_COMPILER)
  set(CMAKE_C_COMPILER ${CLANG_COMPILER})
endif()

project(wall C)

set(CMAKE_C_STANDARD 11)

option(NATIVE_BUILD "build with -march=native -mtune=native" OFF)
option(USE_MIMALLOC "use mimalloc allocator" OFF)

add_executable(wall wall.c)

target_compile_options(wall PRIVATE -O3 -ffast-math -fmath-errno -flto -fPIC -fPIE -fstack-protector-all)

if(NATIVE_BUILD)
  target_compile_options(wall PRIVATE -march=native -mtune=native)
endif()

include(CheckCCompilerFlag)
check_c_compiler_flag("-fsanitize=safe-stack" HAVE_SAFE_STACK)
if(HAVE_SAFE_STACK)
  target_compile_options(wall PRIVATE -fsanitize=safe-stack)
  target_link_options(wall PRIVATE -fsanitize=safe-stack)
endif()

set_property(TARGET wall PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_custom_command(TARGET wall POST_BUILD
    COMMAND strip --strip-unneeded $<TARGET_FILE:wall>
    COMMENT "stripping executable"
  )
endif()

find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(IMLIB2 REQUIRED imlib2)
pkg_check_modules(AVIF REQUIRED libavif)
pkg_check_modules(DAV1D REQUIRED dav1d)

if(USE_MIMALLOC)
  pkg_check_modules(MIMALLOC REQUIRED mimalloc)
endif()

target_include_directories(wall PRIVATE
  ${IMLIB2_INCLUDE_DIRS}
  ${AVIF_INCLUDE_DIRS}
  ${DAV1D_INCLUDE_DIRS}
)

target_link_directories(wall PRIVATE
  ${IMLIB2_LIBRARY_DIRS}
  ${AVIF_LIBRARY_DIRS}
  ${DAV1D_LIBRARY_DIRS}
)

target_link_libraries(wall PRIVATE
  X11::X11
  ${IMLIB2_LIBRARIES}
  ${AVIF_LIBRARIES}
  ${DAV1D_LIBRARIES}
)

if(USE_MIMALLOC)
  target_include_directories(wall PRIVATE ${MIMALLOC_INCLUDE_DIRS})
  target_link_directories(wall PRIVATE ${MIMALLOC_LIBRARY_DIRS})
  target_link_libraries(wall PRIVATE ${MIMALLOC_LIBRARIES})
endif()

install(TARGETS wall DESTINATION bin)
