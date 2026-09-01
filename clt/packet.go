package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"net"

	"google.golang.org/protobuf/proto"
)

// C++ 서버의 MessageType enum 값 정의
const (
	MessageTypeLoginRequest       uint16 = 1001
	MessageTypeLoginResponse      uint16 = 1002
	MessageTypeChatMessage        uint16 = 1005
	MessageTypeCreateRoomRequest  uint16 = 1008
	MessageTypeCreateRoomResponse uint16 = 1009
	MessageTypeJoinRoom           uint16 = 1006
	MessageTypeJoinRoomResponse   uint16 = 1017
	MessageTypePing               uint16 = 1029
	MessageTypePong               uint16 = 1030
)

const HeaderSize = 12 // 2 + 2 + 4 + 4 bytes

// C++ #pragma pack(push, 1) 패킷 헤더 대응 구조체
type PacketHeader struct {
	PacketSize     uint16
	MessageType    uint16
	UserID         uint32
	SequenceNumber uint32
}

// 1. Protobuf 메시지와 헤더를 C++ 서버 규격에 맞게 Little-Endian 바이트 스트림으로 패킹
func EncodePacket(msgType uint16, userID uint32, seqNum uint32, pbMsg proto.Message) ([]byte, error) {
	var payload []byte
	var err error

	if pbMsg != nil {
		payload, err = proto.Marshal(pbMsg)
		if err != nil {
			return nil, fmt.Errorf("protobuf marshal error: %w", err)
		}
	}

	totalSize := uint16(HeaderSize + len(payload))
	header := PacketHeader{
		PacketSize:     totalSize,
		MessageType:    msgType,
		UserID:         userID,
		SequenceNumber: seqNum,
	}

	buf := new(bytes.Buffer)

	// C++ x86/x64의 리틀 엔디안 바이너리 레이아웃 직렬화
	if err := binary.Write(buf, binary.LittleEndian, header); err != nil {
		return nil, fmt.Errorf("header write error: %w", err)
	}

	if len(payload) > 0 {
		buf.Write(payload)
	}

	return buf.Bytes(), nil
}

// 2. 소켓 스트림에서 C++ RingPacketBuffer 방식처럼 패킷을 완전하게 읽어오는 수신기
func ReadPacket(conn net.Conn) (*PacketHeader, []byte, error) {
	headerBuf := make([]byte, HeaderSize)
	_, err := io.ReadFull(conn, headerBuf)
	if err != nil {
		return nil, nil, err
	}

	var header PacketHeader
	reader := bytes.NewReader(headerBuf)
	if err := binary.Read(reader, binary.LittleEndian, &header); err != nil {
		return nil, nil, err
	}

	// 페이로드 크기 계산 및 읽기
	payloadSize := int(header.PacketSize) - HeaderSize
	payload := make([]byte, payloadSize)

	if payloadSize > 0 {
		_, err = io.ReadFull(conn, payload)
		if err != nil {
			return nil, nil, fmt.Errorf("payload read error: %w", err)
		}
	}

	return &header, payload, nil
}
