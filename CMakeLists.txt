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

# =============================================================================
# Windows MinGW: collect runtime DLLs to output directories
# =============================================================================
if(WIN32 AND MINGW)
    get_filename_component(MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)

    # DLLs commonly needed by MinGW-built programs
    set(MINGW_RUNTIME_DLLS
        libwinpthread-1.dll
        libgcc_s_seh-1.dll
        libstdc++-6.dll
        libssl-3-x64.dll
        libcrypto-3-x64.dll
        zlib1.dll
    )

    # Find each DLL
    set(FOUND_DLLS "")
    foreach(dll ${MINGW_RUNTIME_DLLS})
        find_file(DLL_${dll} ${dll} PATHS "${MINGW_BIN_DIR}" NO_DEFAULT_PATH)
        if(DLL_${dll})
            list(APPEND FOUND_DLLS "${DLL_${dll}}")
        else()
            message(STATUS "Optional DLL not found: ${dll}")
        endif()
    endforeach()

    if(FOUND_DLLS)
        # Copy DLLs next to ag-server
        add_custom_command(TARGET ag-server POST_BUILD
            COMMENT "Copying runtime DLLs to ag-server output directory"
        )
        foreach(dll_path ${FOUND_DLLS})
            add_custom_command(TARGET ag-server POST_BUILD APPEND
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${dll_path}" "$<TARGET_FILE_DIR:ag-server>"
            )
        endforeach()

        # Copy DLLs next to ag-manager
        add_custom_command(TARGET ag-manager POST_BUILD
            COMMENT "Copying runtime DLLs to ag-manager output directory"
        )
        foreach(dll_path ${FOUND_DLLS})
            add_custom_command(TARGET ag-manager POST_BUILD APPEND
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${dll_path}" "$<TARGET_FILE_DIR:ag-manager>"
            )
        endforeach()

        # Copy DLLs next to ag-updater (only zlib + runtime)
        add_custom_command(TARGET ag-updater POST_BUILD
            COMMENT "Copying runtime DLLs to ag-updater output directory"
        )
        foreach(dll_path ${FOUND_DLLS})
            add_custom_command(TARGET ag-updater POST_BUILD APPEND
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${dll_path}" "$<TARGET_FILE_DIR:ag-updater>"
            )
        endforeach()
    endif()
endif()
