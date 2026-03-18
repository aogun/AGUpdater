cmake_minimum_required(VERSION 3.10)
project(AGUpdater VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Third-party paths
set(THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/third_party)

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Find zlib (needed by minizip)
find_package(ZLIB REQUIRED)

# SQLite3 as a library target
add_library(sqlite3 STATIC
    ${THIRD_PARTY_DIR}/sqlite3/sqlite3.c
)
target_include_directories(sqlite3 PUBLIC ${THIRD_PARTY_DIR}/sqlite3)

# cJSON as a library target
add_library(cjson STATIC
    ${THIRD_PARTY_DIR}/cJSON/cJSON.c
)
target_include_directories(cjson PUBLIC ${THIRD_PARTY_DIR}/cJSON)

# minizip as a library target
set(MINIZIP_SOURCES
    ${THIRD_PARTY_DIR}/minizip/unzip.c
    ${THIRD_PARTY_DIR}/minizip/ioapi.c
)
if(WIN32)
    list(APPEND MINIZIP_SOURCES ${THIRD_PARTY_DIR}/minizip/iowin32.c)
endif()
add_library(minizip STATIC ${MINIZIP_SOURCES})
target_include_directories(minizip PUBLIC ${THIRD_PARTY_DIR}/minizip)
target_link_libraries(minizip PRIVATE ZLIB::ZLIB)

# Shared log library
add_library(aglog STATIC ${CMAKE_SOURCE_DIR}/src/log.cpp)
target_include_directories(aglog PUBLIC ${CMAKE_SOURCE_DIR}/src)

# Sub-projects
add_subdirectory(server)
add_subdirectory(client)
