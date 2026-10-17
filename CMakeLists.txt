cmake_minimum_required(VERSION 3.16)
project(tg_notes_bot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

add_executable(tg_notes_bot
    src/main.cpp
    src/notes_store.cpp
    src/telegram_client.cpp
)

target_include_directories(tg_notes_bot PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(tg_notes_bot PRIVATE
    CURL::libcurl
    SQLite::SQLite3
    Threads::Threads
)

install(TARGETS tg_notes_bot RUNTIME DESTINATION bin)
