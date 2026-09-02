package main

import (
	"crypto/tls"
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"log"
	"math/rand"
	"net"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"

	// 프로젝트의 Protobuf 패키지 경로
	pb "github.com/Kwonhong1/-TCP-CHAT-SERVER-KAFKAFOTOK/clt/proto" 
	"google.golang.org/protobuf/proto"
)

// ==========================================
// 1. C++ 서버 구조와 100% 일치시킨 패킷 헤더 (12 Byte)
// ==========================================

const HeaderSize = 12 // uint16(2) + uint16(2) + uint32(4) + uint32(4)

type MessageType uint16

const (
	LOGIN_PROMPT             MessageType = 1000
	LOGIN_REQUEST            MessageType = 1001
	LOGIN_RESPONSE           MessageType = 1002
	CHAT_MESSAGE             MessageType = 1005
	JOIN_ROOM                MessageType = 1006
	REGISTER_REQUEST         MessageType = 1015
	REGISTER_RESPONSE        MessageType = 1016
	JOIN_ROOM_RESPONSE       MessageType = 1017
	PING                     MessageType = 1029
	PONG                     MessageType = 1030
)

// C++ struct PacketHeader (Little Endian)
type PacketHeader struct {
	PacketSize     uint16
	MessageType    MessageType
	UserID         uint32
	SequenceNumber uint32
}

// 패킷 인코딩 (Header + Protobuf Payload)
func EncodePacket(msgType MessageType, userID uint32, seqNum uint32, pbMsg proto.Message) ([]byte, error) {
	var payload []byte
	var err error

	if pbMsg != nil {
		payload, err = proto.Marshal(pbMsg)
		if err != nil {
			return nil, fmt.Errorf("protobuf marshal error: %w", err)
		}
	}

	packetSize := uint16(HeaderSize + len(payload))
	buf := make([]byte, packetSize)

	// C++ x86/x64 시스템 메모리 레이아웃 (LittleEndian)
	binary.LittleEndian.PutUint16(buf[0:2], packetSize)
	binary.LittleEndian.PutUint16(buf[2:4], uint16(msgType))
	binary.LittleEndian.PutUint32(buf[4:8], userID)
	binary.LittleEndian.PutUint32(buf[8:12], seqNum)

	if len(payload) > 0 {
		copy(buf[HeaderSize:], payload)
	}

	return buf, nil
}

// ==========================================
// 2. TLS 클라이언트 구조체 (구 client.go)
// ==========================================

type Client struct {
	conn      net.Conn
	username  string
	userID    uint32
	seq       uint32
	mu        sync.Mutex
	closed    bool
	respChans map[MessageType]chan []byte
}

func NewClient(username string) *Client {
	return &Client{
		username:  username,
		respChans: make(map[MessageType]chan []byte),
	}
}

// TLS 1.2 보안 세션 접속
func (c *Client) Connect(addr string) error {
	tlsConfig := &tls.Config{
		InsecureSkipVerify: true, // Self-Signed 서버 인증서 무시
		MinVersion:         tls.VersionTLS12,
	}

	conn, err := tls.DialWithDialer(&net.Dialer{Timeout: 5 * time.Second}, "tcp", addr, tlsConfig)
	if err != nil {
		return err
	}
	c.conn = conn
	return nil
}

func (c *Client) Close() {
	c.mu.Lock()
	defer c.mu.Unlock()
	if !c.closed && c.conn != nil {
		c.conn.Close()
		c.closed = true
	}
}

func (c *Client) SendPacket(msgType MessageType, pbMsg proto.Message) error {
	c.mu.Lock()
	c.seq++
	currentSeq := c.seq
	c.mu.Unlock()

	data, err := EncodePacket(msgType, c.userID, currentSeq, pbMsg)
	if err != nil {
		return err
	}

	_, err = c.conn.Write(data)
	return err
}

// 1) 회원가입 요청
func (c *Client) Register(username, password string) error {
	req := &pb.RegisterRequest{
		Username: username,
		Password: password,
	}
	return c.SendPacket(REGISTER_REQUEST, req)
}

// 2) 로그인 요청
func (c *Client) Login(username, password string) error {
	req := &pb.LoginRequest{
		Username: username,
		Password: password,
	}
	return c.SendPacket(LOGIN_REQUEST, req)
}

// 3) 1번 방 입장 요청 (JOIN_ROOM = 1006)
func (c *Client) EnterRoom(roomID uint32) error {
	req := &pb.JoinRoomRequest{
		RoomId: roomID,
	}
	return c.SendPacket(JOIN_ROOM, req)
}

// 4) 채팅 메시지 전송 (CHAT_MESSAGE = 1005)
func (c *Client) SendChatMessage(roomID uint32, message string) error {
	req := &pb.ChatMessage{
		RoomId:    roomID,
		Message:   message,
		Timestamp: time.Now().Unix(),
	}
	return c.SendPacket(CHAT_MESSAGE, req)
}

// 5) 수신 및 핑퐁 응답 수신 루프
func (c *Client) ReadLoop() {
	headerBuf := make([]byte, HeaderSize)

	for {
		_, err := io.ReadFull(c.conn, headerBuf)
		if err != nil {
			return
		}

		packetSize := binary.LittleEndian.Uint16(headerBuf[0:2])
		msgType := MessageType(binary.LittleEndian.Uint16(headerBuf[2:4]))
		userID := binary.LittleEndian.Uint32(headerBuf[4:8])

		if userID != 0 {
			c.userID = userID
		}

		payloadLen := int(packetSize) - HeaderSize
		payload := make([]byte, payloadLen)

		if payloadLen > 0 {
			_, err = io.ReadFull(c.conn, payload)
			if err != nil {
				return
			}
		}

		// 로그인 응답 처리 (Assigned User ID 수득)
		if msgType == LOGIN_RESPONSE {
			var res pb.LoginResponse
			if err := proto.Unmarshal(payload, &res); err == nil && res.GetSuccess() {
				c.userID = res.GetAssignedUserId()
			}
		}

		// 서버 프롬프트 및 PING 무시/자동 응답
		if msgType == PING {
			_ = c.SendPacket(PONG, nil)
		}
	}
}

// ==========================================
// 3. 부하 테스트 시나리오
// ==========================================

func main() {
	serverAddr := flag.String("addr", "127.0.0.1:8080", "Target C++ TLS Chat Server Address")
	userCount := flag.Int("vu", 10, "Number of Virtual Users (Goroutines)")
	roomID := flag.Uint("room", 1, "Target Chat Room ID")
	interval := flag.Int("interval", 1000, "Message Interval (ms)")
	flag.Parse()

	log.Printf("==================================================")
	log.Printf("[TLS Scenario Load Test] Server: %s | VUs: %d | Room: %d", *serverAddr, *userCount, *roomID)
	log.Printf("==================================================")

	stopChan := make(chan os.Signal, 1)
	signal.Notify(stopChan, os.Interrupt, syscall.SIGTERM)

	var wg sync.WaitGroup
	runID := time.Now().Unix() % 100000

	for i := 1; i <= *userCount; i++ {
		wg.Add(1)
		userIndex := i

		go func(idx int) {
			defer wg.Done()

			username := fmt.Sprintf("vu_%d_%04d", runID, idx)
			password := "password123!"

			client := NewClient(username)
			if err := client.Connect(*serverAddr); err != nil {
				log.Printf("[%s] ❌ TLS Connection failed: %v", username, err)
				return
			}
			defer client.Close()

			// 백그라운드 수신 루프
			go client.ReadLoop()

			// 1. 회원가입 시도
			_ = client.Register(username, password)
			time.Sleep(50 * time.Millisecond)

			// 2. 로그인 시도
			if err := client.Login(username, password); err != nil {
				log.Printf("[%s] ❌ Login request failed: %v", username, err)
				return
			}
			time.Sleep(50 * time.Millisecond)
			log.Printf("[%s] ✅ Logged In (Assigned ID: %d)", username, client.userID)

			// 3. 1번 채팅방 입장
			if err := client.EnterRoom(uint32(*roomID)); err != nil {
				log.Printf("[%s] ❌ Enter Room %d failed: %v", username, *roomID, err)
				return
			}
			log.Printf("[%s] ✅ Entered Room #%d", username, *roomID)

			// 4. 메시지 연사
			ticker := time.NewTicker(time.Duration(*interval) * time.Millisecond)
			defer ticker.Stop()

			seq := 1
			for {
				select {
				case <-stopChan:
					return
				case <-ticker.C:
					msg := fmt.Sprintf("Load test message #%d [User: %s] [Rand: %d]", seq, username, rand.Intn(9999))
					if err := client.SendChatMessage(uint32(*roomID), msg); err != nil {
						log.Printf("[%s] ❌ Send Chat failed: %v", username, err)
						return
					}
					seq++
				}
			}
		}(userIndex)

		time.Sleep(20 * time.Millisecond)
	}

	wg.Wait()
	log.Println("[Scenario Load Test Completed]")
}
