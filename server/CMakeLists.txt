add_executable(ag-server
    src/main.cpp
    src/http_server.cpp
    src/admin_api.cpp
    src/client_api.cpp
    src/auth.cpp
    src/database.cpp
    src/config.cpp
    src/version_util.cpp
)

target_include_directories(ag-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src
    ${THIRD_PARTY_DIR}/cpp-httplib
)

target_compile_definitions(ag-server PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_link_libraries(ag-server PRIVATE
    aglog
    sqlite3
    cjson
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(WIN32)
    target_link_libraries(ag-server PRIVATE ws2_32 crypt32)
endif()

# Copy web resources to build directory on every build
add_custom_target(copy-web ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/web
        ${CMAKE_CURRENT_BINARY_DIR}/web
    COMMENT "Copying web resources"
)
add_dependencies(ag-server copy-web)

# Copy MinGW runtime DLLs next to ag-server
if(WIN32 AND MINGW AND MINGW_RUNTIME_DLL_PATHS)
    foreach(dll_path ${MINGW_RUNTIME_DLL_PATHS})
        add_custom_command(TARGET ag-server POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${dll_path}" "$<TARGET_FILE_DIR:ag-server>"
        )
    endforeach()
endif()

# =============================================================================
# ag-server-pack: package ag-server with all dependencies into a ZIP
# =============================================================================

# Parse version from version.h
file(STRINGS "${CMAKE_SOURCE_DIR}/src/version.h" _ver_major REGEX "APP_VERSION_MAJOR")
file(STRINGS "${CMAKE_SOURCE_DIR}/src/version.h" _ver_minor REGEX "APP_VERSION_MINOR")
file(STRINGS "${CMAKE_SOURCE_DIR}/src/version.h" _ver_patch REGEX "APP_VERSION_PATCH")
string(REGEX MATCH "[0-9]+" AG_VER_MAJOR "${_ver_major}")
string(REGEX MATCH "[0-9]+" AG_VER_MINOR "${_ver_minor}")
string(REGEX MATCH "[0-9]+" AG_VER_PATCH "${_ver_patch}")
set(AG_SERVER_VERSION "${AG_VER_MAJOR}.${AG_VER_MINOR}.${AG_VER_PATCH}")

set(PACK_NAME "ag-server-${AG_SERVER_VERSION}")
set(PACK_DIR "${CMAKE_BINARY_DIR}/${PACK_NAME}")

add_custom_target(ag-server-pack
    DEPENDS ag-server copy-web
    COMMENT "Packaging ag-server ${AG_SERVER_VERSION}..."

    # Clean and create pack directory
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${PACK_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PACK_DIR}"

    # Copy ag-server executable
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ag-server>" "${PACK_DIR}/"

    # Copy web resources
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_BINARY_DIR}/web" "${PACK_DIR}/web"

    # Copy config.json from data/
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/data/config.json" "${PACK_DIR}/"

    # Create zip
    COMMAND ${CMAKE_COMMAND} -E tar "cf" "${CMAKE_BINARY_DIR}/${PACK_NAME}.zip"
        --format=zip "${PACK_NAME}"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# Copy MinGW runtime DLLs into pack directory
if(WIN32 AND MINGW AND MINGW_RUNTIME_DLL_PATHS)
    foreach(dll_path ${MINGW_RUNTIME_DLL_PATHS})
        add_custom_command(TARGET ag-server-pack POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${dll_path}" "${PACK_DIR}/"
        )
    endforeach()
    # Re-create zip after DLLs are copied
    add_custom_command(TARGET ag-server-pack POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove -f "${CMAKE_BINARY_DIR}/${PACK_NAME}.zip"
        COMMAND ${CMAKE_COMMAND} -E tar "cf" "${CMAKE_BINARY_DIR}/${PACK_NAME}.zip"
            --format=zip "${PACK_NAME}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()
