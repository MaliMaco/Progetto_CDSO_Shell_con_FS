cmake_minimum_required(VERSION 3.10)
project(simplefs_shell C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -g)
add_compile_definitions(_POSIX_C_SOURCE=200809L)

#fs/: bitmap, disk driver (mmap) e filesystem
add_library(simplefs STATIC fs/bitmap.c fs/disk_driver.c fs/simplefs.c)
target_include_directories(simplefs PUBLIC fs)

#utils/: comandi della shell sul filesystem, gestione path, parsing
add_library(shell_utils STATIC shell/commands.c shell/path.c shell/parser.c utils/jobs.c)
target_include_directories(shell_utils PUBLIC utils)
target_link_libraries(shell_utils PUBLIC simplefs)

#shell/: ciclo interattivo che emula una shell dedicata al filesystem
add_executable(fs_shell shell/shell.c)
target_link_libraries(fs_shell PRIVATE shell_utils)
