cmake_minimum_required(VERSION 3.16)

# Boost가 내장 CMake Config를 타고 boost_system을 요구하는 것을 완전히 강제 차단
set(Boost_NO_BOOST_CMAKE ON)
set(BOOST_NO_BOOST_CMAKE ON)

project(ChatProject CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# [수정/추가] C++20 환경에서 Boost Asio 사용 시 플레이스홀더 충돌 방지
add_definitions(-DBOOST_BIND_GLOBAL_PLACEHOLDERS)

# 1. Protobuf 및 OpenSSL 패키지 탐색 (find_package(Boost)는 절대 사용 금지)
find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED)

# 2. Boost 헤더 경로는 /usr/include 수동 지정
set(BOOST_INCLUDE_DIR "/usr/include")

# 3. .proto 파일 컴파일 설정
set(PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/chat_protocol.proto")
set(PROTO_SRC "${CMAKE_CURRENT_BINARY_DIR}/chat_protocol.pb.cc")
set(PROTO_HDR "${CMAKE_CURRENT_BINARY_DIR}/chat_protocol.pb.h")

add_custom_command(
    OUTPUT "${PROTO_SRC}" "${PROTO_HDR}"
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    ARGS --cpp_out="${CMAKE_CURRENT_BINARY_DIR}" -I"${CMAKE_CURRENT_SOURCE_DIR}" "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Running C++ protocol buffer compiler on ${PROTO_FILE}"
)

# 4. Executable 생성
add_executable(client client.cpp ${PROTO_SRC} ${PROTO_HDR})

# Include 헤더 경로 지정
target_include_directories(client PRIVATE
    ${BOOST_INCLUDE_DIR}
    ${PROTOBUF_INCLUDE_DIRS}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# 5. 링크 라이브러리 설정 (OpenSSL::SSL 링크 시 헤더 경로 자동 전입)
target_link_libraries(client PRIVATE
    ${PROTOBUF_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)

