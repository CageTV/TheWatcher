# Usage: cmake -DSRC=<file> -DDST=<file> -P copy_if_missing.cmake
if(NOT EXISTS "${DST}")
    file(COPY_FILE "${SRC}" "${DST}")
    message(STATUS "Copied default ${DST}")
else()
    message(STATUS "Kept existing ${DST}")
endif()
