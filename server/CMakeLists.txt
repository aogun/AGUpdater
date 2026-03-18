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
    sqlite3
    cjson
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(WIN32)
    target_link_libraries(ag-server PRIVATE ws2_32 crypt32)
endif()

# Install web resources alongside the executable
file(GLOB WEB_FILES ${CMAKE_CURRENT_SOURCE_DIR}/web/*)
if(WEB_FILES)
    file(COPY ${WEB_FILES} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/web)
endif()
