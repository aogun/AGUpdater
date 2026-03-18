add_executable(ag-manager
    src/main.cpp
)

target_include_directories(ag-manager PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(ag-manager PRIVATE
    ag-update-lib
    cjson
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(WIN32)
    target_link_libraries(ag-manager PRIVATE comctl32 gdi32 user32 crypt32 ws2_32)
endif()
