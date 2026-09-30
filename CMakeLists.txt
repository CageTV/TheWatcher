cmake_minimum_required(VERSION 3.21)

# DLL name: TheWatcher.dll
project(TheWatcher VERSION 1.0.4 LANGUAGES CXX)

# Build output stays in the build folder only (build\release-msvc or build\debug-msvc).
# Nothing is copied anywhere else; copy TheWatcher.dll, TheWatcher.pdb and TheWatcher.ini yourself.

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
include(cmake/headerlist.cmake)
include(cmake/sourcelist.cmake)
add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${headers} ${sources})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE src)

# Release builds make a PDB too (/Z7 keeps debug info in the .obj files, so parallel builds never fight over one PDB)
target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/Z7>)
target_link_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)

# Stack walking / minidumps (dbghelp) and process memory stats (psapi)
target_link_libraries(${PROJECT_NAME} PRIVATE dbghelp psapi)

# Helper program that writes the minidump from outside the frozen game. Ship it next to TheWatcher.dll.
add_executable(TheWatcherDump src/DumpHelper.cpp)
target_compile_features(TheWatcherDump PRIVATE cxx_std_20)
target_link_libraries(TheWatcherDump PRIVATE dbghelp)
