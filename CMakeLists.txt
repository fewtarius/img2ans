cmake_minimum_required(VERSION 3.15)
project(img2ans LANGUAGES C)

# Derive version from git tag (v20260314.1 or v1.2.3); fall back to 0.0.0-dev
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --match "v*" --abbrev=0
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(GIT_TAG MATCHES "^v(.+)$")
    set(PROJECT_VERSION "${CMAKE_MATCH_1}")
else()
    set(PROJECT_VERSION "0.0.0-dev")
endif()
message(STATUS "img2ans version: ${PROJECT_VERSION}")

# Require C99
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Compiler warnings
if(MSVC)
    add_compile_options(/W3)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# Target
add_executable(img2ans img2ans.c)

# Pass version string to the compiler
target_compile_definitions(img2ans PRIVATE IMG2ANS_VERSION="${PROJECT_VERSION}")

# Math library (not needed on Windows/MSVC)
if(NOT MSVC)
    target_link_libraries(img2ans PRIVATE m)
endif()

# stb_image.h lives next to img2ans.c
target_include_directories(img2ans PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Install
include(GNUInstallDirs)
install(TARGETS img2ans
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES img2ans.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
