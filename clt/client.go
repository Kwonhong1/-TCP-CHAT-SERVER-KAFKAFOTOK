package main

import (
	"crypto/tls"
	"fmt"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"google.golang.org/protobuf/proto"
)

type Client struct {
	conn       net.Conn
	userID     uint32
	seqNo      uint32
	roomID     uint32
	isLoggedIn bool
	mu         sync.Mutex
}

func NewClient() *Client {
	return &Client{}
}

// 1. C++ 서버(8080 포트, TLS 1.2)에 TLS 연결 및 초기 LOGIN_PROMPT 수신
func (c *Client) Connect(serverAddr string) error {
	// 자체 서명 Certificate(server.crt) 검증 스킵 옵션 적용
	conf := &tls.Config{
		InsecureSkipVerify: true,
	}

	conn, err := tls.Dial("tcp", serverAddr, conf)
	if err != nil {
		return fmt.Errorf("TLS Dial Error: %w", err)
	}
	c.conn = conn

	// 서버 접속 직후 보내주는 LOGIN_PROMPT(1000) 패킷 읽어서 비워주기
	header, _, err := ReadPacket(c.conn)
	if err != nil {
		c.conn.Close()
		return fmt.Errorf("Prompt Packet Read Error: %w", err)
	}

	if header.MessageType != 1000 { // LOGIN_PROMPT
		c.conn.Close()
		return fmt.Errorf("expected LOGIN_PROMPT(1000), but got %d", header.MessageType)
	}

	return nil
}

// 2. 패킷 전송 헬퍼 함수
func (c *Client) Send(msgType uint16, pbMsg proto.Message) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	seq := atomic.AddUint32(&c.seqNo, 1)
	packetBytes, err := EncodePacket(msgType, c.userID, seq, pbMsg)
	if err != nil {
		return err
	}

	_, err = c.conn.Write(packetBytes)
	return err
}

// 3. 특정 응답 패킷 대기 함수
func (c *Client) ReadResponse() (*PacketHeader, []byte, error) {
	// 읽기 타임아웃 5초 설정 (응답이 없으면 에러)
	c.conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	defer c.conn.SetReadDeadline(time.Time{})

	return ReadPacket(c.conn)
}

func (c *Client) Close() {
	if c.conn != nil {
		c.conn.Close()
	}
}
//4124124