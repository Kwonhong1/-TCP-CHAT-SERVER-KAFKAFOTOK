package main

import (
	"context"
	"crypto/rand"
	"database/sql"
	"encoding/hex"
	"fmt"
	"net"
	"strconv"
	"strings"
	"time"

	mysqlDriver "github.com/go-sql-driver/mysql"
	"github.com/redis/go-redis/v9"
	"google.golang.org/grpc"

	pb "chat_db_server/pb" // 프로토버퍼 컴파일 경로
)

const (
	RedisChatQueueKey = "chat_msg_queue"
	BatchSize         = 100             // 한 번에 DB에 모아서 처리할 최대 메시지 수
	BatchInterval     = 5 * time.Second // 백그라운드 DB 저장 주기 (5초)
)

type server struct {
	pb.UnimplementedChatDBServiceServer
	db  *sql.DB
	rdb *redis.Client
}

// ----------------------------------------------------
// [신규] 백그라운드 배치 일괄 저장 (Bulk Insert) 워커
// ----------------------------------------------------
func (s *server) StartChatBatchWorker(ctx context.Context) {
	ticker := time.NewTicker(BatchInterval)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			fmt.Println("[Batch Worker] 배치 워커 종료")
			return
		case <-ticker.C:
			s.flushChatQueueToDB(ctx)
		}
	}
}

func (s *server) flushChatQueueToDB(ctx context.Context) {
	// Redis Queue에 쌓인 메시지 개수 확인
	queueLen, err := s.rdb.LLen(ctx, RedisChatQueueKey).Result()
	if err != nil || queueLen == 0 {
		return
	}

	// 최대 BatchSize 만큼 Pop
	fetchCount := queueLen
	if fetchCount > BatchSize {
		fetchCount = BatchSize
	}

	var rawMsgs []string
	for i := int64(0); i < fetchCount; i++ {
		val, err := s.rdb.LPop(ctx, RedisChatQueueKey).Result()
		if err != nil {
			break
		}
		rawMsgs = append(rawMsgs, val)
	}

	if len(rawMsgs) == 0 {
		return
	}

	// 다중 INSERT (Bulk Insert) SQL 구성: INSERT INTO chat_messages VALUES (...), (...), (...)
	valueStrings := make([]string, 0, len(rawMsgs))
	valueArgs := make([]interface{}, 0, len(rawMsgs)*4)

	for _, raw := range rawMsgs {
		// 저장 포맷: "RoomId:UserId:Timestamp:Message"
		parts := strings.SplitN(raw, ":", 4)
		if len(parts) < 4 {
			continue
		}

		roomId, _ := strconv.ParseUint(parts[0], 10, 32)
		userId, _ := strconv.ParseUint(parts[1], 10, 32)
		timestamp, _ := strconv.ParseInt(parts[2], 10, 64)
		message := parts[3]

		valueStrings = append(valueStrings, "(?, ?, ?, ?)")
		valueArgs = append(valueArgs, roomId, userId, message, timestamp)
	}

	if len(valueStrings) == 0 {
		return
	}

	stmt := fmt.Sprintf("INSERT INTO chat_messages (room_id, sender_id, message, timestamp) VALUES %s",
		strings.Join(valueStrings, ","))

	_, err = s.db.ExecContext(ctx, stmt, valueArgs...)
	if err != nil {
		fmt.Printf("[Batch Worker Error] DB Bulk Insert 실패: %v\n", err)
		// 실패 시 필요에 따라 Redis Queue에 다시 넣거나 복구 로그 작성
	} else {
		fmt.Printf("[Batch Worker Success] %d개 채팅 메시지 DB 일괄 저장 완료\n", len(valueStrings))
	}
}

// ----------------------------------------------------
// gRPC 서비스 메서드 구현
// ----------------------------------------------------

// 1. 토큰 발급 메서드
func (s *server) IssueToken(ctx context.Context, req *pb.IssueTokenRequest) (*pb.IssueTokenResponse, error) {
	bytes := make([]byte, 32)
	if _, err := rand.Read(bytes); err != nil {
		return &pb.IssueTokenResponse{Success: false, ErrorMessage: "FAILED_TO_GENERATE_TOKEN"}, nil
	}
	token := hex.EncodeToString(bytes)

	key := fmt.Sprintf("token:%s", token)
	val := fmt.Sprintf("%d:%s", req.UserId, req.Username)

	ttlSeconds := req.TtlSeconds
	if ttlSeconds <= 0 {
		ttlSeconds = 86400 // 기본값 24시간
	}
	ttl := time.Duration(ttlSeconds) * time.Second

	err := s.rdb.Set(ctx, key, val, ttl).Err()
	if err != nil {
		return &pb.IssueTokenResponse{Success: false, ErrorMessage: "REDIS_WRITE_ERROR"}, nil
	}

	return &pb.IssueTokenResponse{
		Success: true,
		Token:   token,
	}, nil
}

// 2. 토큰 검증 메서드
func (s *server) VerifyToken(ctx context.Context, req *pb.VerifyTokenRequest) (*pb.VerifyTokenResponse, error) {
	key := fmt.Sprintf("token:%s", req.Token)

	val, err := s.rdb.Get(ctx, key).Result()
	if err == redis.Nil {
		return &pb.VerifyTokenResponse{Success: false, ErrorMessage: "TOKEN_EXPIRED_OR_INVALID"}, nil
	} else if err != nil {
		return &pb.VerifyTokenResponse{Success: false, ErrorMessage: "REDIS_READ_ERROR"}, nil
	}

	parts := strings.SplitN(val, ":", 2)
	if len(parts) < 2 {
		return &pb.VerifyTokenResponse{Success: false, ErrorMessage: "MALFORMED_TOKEN_DATA"}, nil
	}

	userId, err := strconv.ParseUint(parts[0], 10, 32)
	if err != nil {
		return &pb.VerifyTokenResponse{Success: false, ErrorMessage: "INVALID_USER_ID"}, nil
	}

	return &pb.VerifyTokenResponse{
		Success:  true,
		UserId:   uint32(userId),
		Username: parts[1],
	}, nil
}

// 3. 로그인 및 토큰 발급
func (s *server) AuthenticateUser(ctx context.Context, req *pb.AuthRequest) (*pb.AuthResponse, error) {
	var id uint32
	var dbPass string
	query := "SELECT id, password_hash FROM users WHERE username = ? LIMIT 1"
	err := s.db.QueryRowContext(ctx, query, req.Username).Scan(&id, &dbPass)

	if err == sql.ErrNoRows || dbPass != req.Password {
		return &pb.AuthResponse{Success: false, ErrorMessage: "INVALID_CREDENTIALS"}, nil
	} else if err != nil {
		return &pb.AuthResponse{Success: false, ErrorMessage: err.Error()}, nil
	}

	tokenRes, err := s.IssueToken(ctx, &pb.IssueTokenRequest{
		UserId:     id,
		Username:   req.Username,
		TtlSeconds: 86400,
	})

	if err != nil || !tokenRes.Success {
		return &pb.AuthResponse{Success: false, ErrorMessage: "TOKEN_ISSUE_FAILED"}, nil
	}

	return &pb.AuthResponse{
		Success:        true,
		UserId:         id,
		ReconnectToken: tokenRes.Token,
	}, nil
}

// 4. 회원가입
func (s *server) RegisterUser(ctx context.Context, req *pb.RegisterRequest) (*pb.RegisterResponse, error) {
	query := "INSERT INTO users (username, password_hash) VALUES (?, ?)"
	res, err := s.db.ExecContext(ctx, query, req.Username, req.Password)
	if err != nil {
		return &pb.RegisterResponse{Success: false, ErrorMessage: "REGISTER_FAILED_OR_DUPLICATE"}, nil
	}

	lastID, _ := res.LastInsertId()
	return &pb.RegisterResponse{Success: true, AssignedId: uint32(lastID)}, nil
}

// 5. 세션 상태 저장
func (s *server) SetSessionState(ctx context.Context, req *pb.SessionStateRequest) (*pb.SessionStateResponse, error) {
	key := fmt.Sprintf("user:session:%d", req.UserId)
	err := s.rdb.Set(ctx, key, req.State, time.Duration(req.TtlSeconds)*time.Second).Err()
	return &pb.SessionStateResponse{Success: err == nil}, nil
}

// 6. [성능 최적화] 채팅 수신 시 DB 대신 Redis 메모리 큐로 진입 + Pub/Sub
func (s *server) PublishChat(ctx context.Context, req *pb.ChatPublishRequest) (*pb.ChatPublishResponse, error) {
	ts := req.Timestamp
	if ts == 0 {
		ts = time.Now().Unix()
	}

	// Redis Queue 저장용 포맷 (배치 저장을 위함)
	queueVal := fmt.Sprintf("%d:%d:%d:%s", req.RoomId, req.UserId, ts, req.Message)

	// DB 직접 저장 없이 Redis Queue(List)에 초당 수천 개 단위 비동기 추가
	err := s.rdb.RPush(ctx, RedisChatQueueKey, queueVal).Err()
	if err != nil {
		return &pb.ChatPublishResponse{Success: false}, nil
	}

	// 실시간 브로드캐스팅용 Pub/Sub
	pubPayload := fmt.Sprintf("%d:%d:%s", req.RoomId, req.UserId, req.Message)
	s.rdb.Publish(ctx, "chat_broadcast", pubPayload)

	return &pb.ChatPublishResponse{Success: true}, nil
}

// 7. 과거 채팅 기록 조회 (MySQL 페이징)
func (s *server) GetChatHistory(ctx context.Context, req *pb.ChatHistoryRequest) (*pb.ChatHistoryResponse, error) {
	limit := req.Limit
	if limit == 0 || limit > 100 {
		limit = 20
	}

	var rows *sql.Rows
	var err error

	if req.LastMessageId == 0 {
		query := `
			SELECT m.id, m.room_id, m.sender_id, u.username, m.message, m.timestamp 
			FROM chat_messages m
			JOIN users u ON m.sender_id = u.id
			WHERE m.room_id = ? 
			ORDER BY m.id DESC LIMIT ?`
		rows, err = s.db.QueryContext(ctx, query, req.RoomId, limit)
	} else {
		query := `
			SELECT m.id, m.room_id, m.sender_id, u.username, m.message, m.timestamp 
			FROM chat_messages m
			JOIN users u ON m.sender_id = u.id
			WHERE m.room_id = ? AND m.id < ? 
			ORDER BY m.id DESC LIMIT ?`
		rows, err = s.db.QueryContext(ctx, query, req.RoomId, req.LastMessageId, limit)
	}

	if err != nil {
		return &pb.ChatHistoryResponse{Success: false, ErrorMessage: err.Error()}, nil
	}
	defer rows.Close()

	var messages []*pb.ChatMessageData
	for rows.Next() {
		var msg pb.ChatMessageData
		if err := rows.Scan(&msg.MessageId, &msg.RoomId, &msg.SenderId, &msg.SenderName, &msg.Message, &msg.Timestamp); err != nil {
			continue
		}
		messages = append(messages, &msg)
	}

	return &pb.ChatHistoryResponse{
		Success:  true,
		Messages: messages,
	}, nil
}

// ----------------------------------------------------
// 메인 진입점
// ----------------------------------------------------
func main() {
	cfg := mysqlDriver.NewConfig()
	cfg.User = "game_user"
	cfg.Passwd = "rnjsghd123@"
	cfg.Net = "tcp"
	cfg.Addr = "172.31.43.246:3306"
	cfg.DBName = "game_db"
	cfg.Timeout = 5 * time.Second
	cfg.ReadTimeout = 5 * time.Second
	cfg.WriteTimeout = 5 * time.Second

	db, err := sql.Open("mysql", cfg.FormatDSN())
	if err != nil {
		panic(err)
	}
	defer db.Close()

	pingCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := db.PingContext(pingCtx); err != nil {
		panic(fmt.Sprintf("MySQL 연결 실패: %v", err))
	}
	fmt.Println("[Go DB/Redis Server] MySQL 연결 확인됨")

	rdb := redis.NewClient(&redis.Options{
		Addr:        "172.31.43.246:6379",
		DialTimeout: 5 * time.Second,
	})

	redisPingCtx, cancel2 := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel2()
	if err := rdb.Ping(redisPingCtx).Err(); err != nil {
		panic(fmt.Sprintf("Redis 연결 실패: %v", err))
	}
	fmt.Println("[Go DB/Redis Server] Redis 연결 확인됨")

	srv := &server{db: db, rdb: rdb}

	// 백그라운드 배치 일괄 저장 워커 실행 (5초 간격)
	ctx, cancelWorker := context.WithCancel(context.Background())
	defer cancelWorker()
	go srv.StartChatBatchWorker(ctx)

	lis, err := net.Listen("tcp", ":50051")
	if err != nil {
		panic(err)
	}

	s := grpc.NewServer()
	pb.RegisterChatDBServiceServer(s, srv)

	fmt.Println("[Go DB/Redis Server] Running on port 50051 (Batch Insert Enabled)...")
	if err := s.Serve(lis); err != nil {
		panic(err)
	}
}

