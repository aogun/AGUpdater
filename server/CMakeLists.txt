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
