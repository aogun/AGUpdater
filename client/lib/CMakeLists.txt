# Configurable options for integrators
set(AG_UPDATER_NAME "ag-updater" CACHE STRING "Updater executable file name")
set(AG_SERVER_URL "https://localhost:8443" CACHE STRING "Server URL")
set(AG_SECRET "default_secret" CACHE STRING "HMAC shared secret")

add_library(ag-update-lib STATIC
    src/ag_updater.cpp
    src/http_client.cpp
    src/auth.cpp
    src/version_util.cpp
)

target_include_directories(ag-update-lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(ag-update-lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${THIRD_PARTY_DIR}/cpp-httplib
)

target_compile_definitions(ag-update-lib PRIVATE
    CPPHTTPLIB_OPENSSL_SUPPORT
    AG_UPDATER_NAME="${AG_UPDATER_NAME}"
    AG_SERVER_URL="${AG_SERVER_URL}"
    AG_SECRET="${AG_SECRET}"
)

find_package(Threads REQUIRED)

target_link_libraries(ag-update-lib PRIVATE
    aglog
    cjson
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

if(WIN32)
    target_link_libraries(ag-update-lib PRIVATE ws2_32)
endif()
