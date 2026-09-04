#include <iostream>
#include <hiredis/hiredis.h>

int main()
{
    // DB 인스턴스의 사설 IP와 Redis 포트(6379)로 연결
    // TODO: "172.xx.xx.xx" 부분을 실제 DB 인스턴스의 사설 IP로 수정하세요!
    const char* db_instance_ip = "172.31.43.246"; 
    int port = 6379;

    redisContext* c = redisConnect(db_instance_ip, port);
    if (c == NULL || c->err)
    {
        if (c)
        {
            std::cerr << "[Error] Redis Connection Failed: " << c->errstr << std::endl;
            redisFree(c);
        }
        else
        {
            std::cerr << "[Error] Cannot allocate redis context" << std::endl;
        }
        return 1;
    }

    std::cout << "[Success] Connected to Remote Redis Server! (" << db_instance_ip << ")" << std::endl;

    // 간단한 PING 테스트
    redisReply* reply = (redisReply*)redisCommand(c, "PING");
    if (reply)
    {
        std::cout << "[Response] PING -> " << reply->str << std::endl; // PONG 출력이 나오면 성공
        freeReplyObject(reply);
    }

    redisFree(c);
    return 0;
}

