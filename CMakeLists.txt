cmake_minimum_required(VERSION 3.22)
project(tcpserver)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 공통 파일 옵션
add_compile_options(-Wall -Wextra -Wpedantic)

# liburing 찾기
find_library(URING_LIB uring REQUIRED)

# 서버 라이브러리
add_library(server_lib STATIC
    server/src/BufferManager.cpp
    server/src/EventHandler.cpp
    server/src/IOUringManager.cpp
    server/src/SessionManager.cpp
    server/src/SocketManager.cpp
)

add_library(client_lib STATIC
    client/src/ChatClient.cpp
)

target_include_directories(server_lib PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)

target_include_directories(client_lib PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/client/include
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)

target_link_libraries(server_lib PUBLIC
    ${URING_LIB}  # liburing 링크
)

# 서버 실행 파일
add_executable(server server/main.cpp)
target_link_libraries(server PRIVATE server_lib)

# 클라이언트 실행 파일
add_executable(client client/main.cpp)
target_link_libraries(client PRIVATE client_lib)

