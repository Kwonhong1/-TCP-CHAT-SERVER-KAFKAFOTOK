cmake_minimum_required(VERSION 3.16)

# Boost가 내장 CMake Config를 타고 boost_system을 요구하는 것을 완전히 강제 차단
set(Boost_NO_BOOST_CMAKE ON)
set(BOOST_NO_BOOST_CMAKE ON)

project(ChatServerProject CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# C++20 환경에서 Boost Asio 사용 시 플레이스홀더 충돌 방지
add_definitions(-DBOOST_BIND_GLOBAL_PLACEHOLDERS)

# 1. 필수 패키지 및 프로그램 탐색
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(OpenSSL REQUIRED) # 서버 TLS/SSL 사용을 위한 OpenSSL
find_program(gRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
find_program(GO_EXECUTABLE go)
find_program(PROTOC_EXECUTABLE protoc) # Go용 protoc 호출을 위해 탐색

set(BOOST_INCLUDE_DIR "/usr/include")

# =========================================================================
# 2. Proto 파일 1: chat_protocol.proto (C++ 클라이언트-C++ 서버)
# =========================================================================
set(NET_PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/chat_protocol.proto")
set(NET_PROTO_SRC "${CMAKE_CURRENT_BINARY_DIR}/chat_protocol.pb.cc")
set(NET_PROTO_HDR "${CMAKE_CURRENT_BINARY_DIR}/chat_protocol.pb.h")

add_custom_command(
    OUTPUT "${NET_PROTO_SRC}" "${NET_PROTO_HDR}"
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    ARGS --cpp_out="${CMAKE_CURRENT_BINARY_DIR}" -I"${CMAKE_CURRENT_SOURCE_DIR}" "${NET_PROTO_FILE}"
    DEPENDS "${NET_PROTO_FILE}"
    COMMENT "Generating C++ code for chat_protocol.proto..."
)

# =========================================================================
# 3. Proto 파일 2: chat_db.proto (C++ 서버용 C++ / gRPC 파일 생성)
# =========================================================================
set(DB_PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/chat_db.proto")
set(DB_PROTO_SRC "${CMAKE_CURRENT_BINARY_DIR}/chat_db.pb.cc")
set(DB_PROTO_HDR "${CMAKE_CURRENT_BINARY_DIR}/chat_db.pb.h")
set(DB_GRPC_SRC "${CMAKE_CURRENT_BINARY_DIR}/chat_db.grpc.pb.cc")
set(DB_GRPC_HDR "${CMAKE_CURRENT_BINARY_DIR}/chat_db.grpc.pb.h")

add_custom_command(
    OUTPUT "${DB_PROTO_SRC}" "${DB_PROTO_HDR}" "${DB_GRPC_SRC}" "${DB_GRPC_HDR}"
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    ARGS --cpp_out="${CMAKE_CURRENT_BINARY_DIR}"
         --grpc_out="${CMAKE_CURRENT_BINARY_DIR}"
         --plugin=protoc-gen-grpc=${gRPC_CPP_PLUGIN_EXECUTABLE}
         -I"${CMAKE_CURRENT_SOURCE_DIR}" "${DB_PROTO_FILE}"
    DEPENDS "${DB_PROTO_FILE}"
    COMMENT "Generating gRPC C++ code for chat_db.proto..."
)

# =========================================================================
# 4. C++ 메인 서버 (cpp_server) 빌드
# =========================================================================
add_executable(cpp_server
    server.cpp
    ${NET_PROTO_SRC} ${NET_PROTO_HDR}
    ${DB_PROTO_SRC} ${DB_PROTO_HDR}
    ${DB_GRPC_SRC} ${DB_GRPC_HDR}
)

# AddressSanitizer (ASan) 및 디버그 기호 추가
target_compile_options(cpp_server PRIVATE -fsanitize=address -g -O1)
target_link_options(cpp_server PRIVATE -fsanitize=address)

target_include_directories(cpp_server PRIVATE
    ${BOOST_INCLUDE_DIR}
    ${PROTOBUF_INCLUDE_DIRS}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# OpenSSL::SSL 및 OpenSSL::Crypto 추가 링크
target_link_libraries(cpp_server PRIVATE
    gRPC::grpc++
    ${PROTOBUF_LIBRARIES}
    OpenSSL::SSL           # 서버 SSL 라이브러리 링크
    OpenSSL::Crypto        # 서버 Crypto 라이브러리 링크
    pthread
)

# =========================================================================
# 5. Go DB 서버 빌드 (Go용 chat_db.proto 컴파일 과정 포함)
# =========================================================================
if(GO_EXECUTABLE)
    # (1) Go 폴더 내부에서 protoc로 chat_db.proto를 Go 파일들(.pb.go)로 변환
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/chat_db_server/chat_db.pb.go"
               "${CMAKE_CURRENT_SOURCE_DIR}/chat_db_server/chat_db_grpc.pb.go"
        COMMAND ${PROTOC_EXECUTABLE}
        ARGS -I="${CMAKE_CURRENT_SOURCE_DIR}"
             --go_out="${CMAKE_CURRENT_SOURCE_DIR}/chat_db_server"
             --go-grpc_out="${CMAKE_CURRENT_SOURCE_DIR}/chat_db_server"
             "${DB_PROTO_FILE}"
        DEPENDS "${DB_PROTO_FILE}"
        COMMENT "Generating Go protobuf/gRPC code for chat_db.proto..."
    )
    # (2) 생성된 Go proto 파일들과 Go 소스 코드를 가지고 go build 실행
    add_custom_target(build_go_server ALL
        COMMAND ${GO_EXECUTABLE} build -o ${CMAKE_CURRENT_BINARY_DIR}/go_db_server .
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/chat_db_server
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/chat_db_server/chat_db.pb.go"
        COMMENT "Building Go DB Server executable..."
    )
    
else()
    message(WARNING "Go compiler not found!")
endif()

