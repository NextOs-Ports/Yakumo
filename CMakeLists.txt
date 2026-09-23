cmake_minimum_required(VERSION 3.20)
project(Yakumo VERSION 0.1.0 LANGUAGES CXX)

# PortableKit brings the recompiler, the runtime and the system layer, and
# defines portablekit_add_game(). It is a submodule, so a change in the
# framework can be tried here before it is tagged.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/portablekit/CMakeLists.txt")
    message(FATAL_ERROR "portablekit/ is empty. Fetch it with: git submodule update --init")
endif()
# Before the framework, so ctest in the build directory runs its tests too.
enable_testing()
add_subdirectory(portablekit)

# The game: Monster Hunter Portable 3rd HD Ver.
add_subdirectory(profiles/mhp3rd)
