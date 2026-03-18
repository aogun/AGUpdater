# --- ag-update-lib (static library) ---

# Configurable options for integrators
set(AG_UPDATER_NAME "ag-updater" CACHE STRING "Updater executable file name")
set(AG_SERVER_URL "https://localhost:8443" CACHE STRING "Server URL")
set(AG_SECRET "default_secret" CACHE STRING "HMAC shared secret")

add_library(ag-update-lib STATIC
    lib/src/ag_updater.cpp
    lib/src/http_client.cpp
    lib/src/auth.cpp
    lib/src/version_util.cpp
)

target_include_directories(ag-update-lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/include
)

target_include_directories(ag-update-lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/src
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

# --- ag-manager (GUI version management tool) ---

add_executable(ag-manager
    manager_main.cpp
)

target_include_directories(ag-manager PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(ag-manager PRIVATE
    aglog
    ag-update-lib
    cjson
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(WIN32)
    target_link_libraries(ag-manager PRIVATE comctl32 gdi32 user32 crypt32 ws2_32)
endif()

# --- ag-updater (update extractor) ---

add_executable(ag-updater
    updater_main.cpp
)

target_include_directories(ag-updater PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(ag-updater PRIVATE
    aglog
    minizip
    ZLIB::ZLIB
)
