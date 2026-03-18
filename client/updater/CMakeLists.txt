add_executable(ag-updater
    src/main.cpp
)

target_include_directories(ag-updater PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(ag-updater PRIVATE
    minizip
    ZLIB::ZLIB
)
