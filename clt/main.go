package main

import (
	"flag"
	"fmt"
	"log"
	"math/rand"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"
)

func main() {
	// 실행 플래그 설정 (서버 주소, 가상 유저 수, 방 번호, 메시지 발송 주기)
	serverAddr := flag.String("addr", "172.31.36.2:8080", "Target C++ Chat Server Address")
	userCount := flag.Int("vu", 1, "Number of Virtual Users (Goroutines)")
	roomID := flag.Int64("room", 1, "Target Chat Room ID (Default: 1)")
	interval := flag.Int("interval", 1000, "Message interval in milliseconds")
	flag.Parse()

	log.Printf("==================================================")
	log.Printf("[Load Test Start] Target: %s | VUs: %d | Room: %d", *serverAddr, *userCount, *roomID)
	log.Printf("==================================================")

	// 프로세스 종료 시그널 처리 (Ctrl+C 누를 때 안전하게 고루틴 정리)
	stopChan := make(chan os.Signal, 1)
	signal.Notify(stopChan, os.Interrupt, syscall.SIGTERM)

	var wg sync.WaitGroup
	// 실행 시점의 Unix 타임스탬프를 조합하여 실행할 때마다 절대 중복되지 않는 ID 접두사 생성
	runID := time.Now().Unix() % 100000

	for i := 1; i <= *userCount; i++ {
		wg.Add(1)
		userIndex := i

		go func(idx int) {
			defer wg.Done()

			// 1. 중복 방지 Unique ID 및 Password/Token 생성
			userID := fmt.Sprintf("dummy_%d_%04d", runID, idx)
			userPw := "password123!"

			// 2. TCP 커넥션 연결 (client.go의 Client 활용)
			client := NewClient(*serverAddr, userID)
			if err := client.Connect(); err != nil {
				log.Printf("[%s] ❌ Connection failed: %v", userID, err)
				return
			}
			defer client.Close()

			// 3. 수신 백그라운드 고루틴 실행 (서버에서 올 응답/채팅 수신)
			go client.ReadLoop()

			// 4. 회원가입 및 로그인 절차 뚫기
			// (서버가 회원가입을 요구할 수도 있으므로 회원가입 시도 후 로그인 진행)
			_ = client.Register(userID, userPw) // 이미 존재하는 경우 에러가 나더라도 로그인으로 통과
			time.Sleep(50 * time.Millisecond)

			if err := client.Login(userID, userPw); err != nil {
				log.Printf("[%s] ❌ Login failed: %v", userID, err)
				return
			}
			log.Printf("[%s] ✅ Login Success", userID)

			// 5. 1번 채팅방 입장
			time.Sleep(50 * time.Millisecond)
			if err := client.EnterRoom(*roomID); err != nil {
				log.Printf("[%s] ❌ Enter Room %d failed: %v", userID, *roomID, err)
				return
			}
			log.Printf("[%s] ✅ Entered Room #%d", userID, *roomID)

			// 6. 무한 메시지 연사 루프 (1번 방으로 지속 송신)
			ticker := time.NewTicker(time.Duration(*interval) * time.Millisecond)
			defer ticker.Stop()

			msgSeq := 1
			for {
				select {
				case <-stopChan:
					// Ctrl+C 시그널 감지 시 퇴장
					log.Printf("[%s] Stopping load generation...", userID)
					return
				case <-ticker.C:
					// 가상 유저별 무작위 텍스트 생성
					msgText := fmt.Sprintf("Hello C++ Server! [User: %s] [Seq: %d] [Rand: %d]", userID, msgSeq, rand.Intn(9999))
					
					if err := client.SendChatMessage(*roomID, msgText); err != nil {
						log.Printf("[%s] ❌ Send Chat failed: %v", userID, err)
						return
					}
					msgSeq++
				}
			}
		}(userIndex)

		// 서버 Connection Throttling (접속 순간 폭주 방지)
		time.Sleep(25 * time.Millisecond)
	}

	wg.Wait()
	log.Println("[Load Test Finished]")
}
