/*
 * redis_client.cpp
 * 
 * Redis 통신 클라이언트 구현
 * 범용 sendData 메서드로 모든 채널에 데이터 전송
 */

#include "channel_types.h"
#include "redis_client.h"
#include "../../utils/config_manager.h"
#include <sstream>
#include <thread>

RedisClient::RedisClient() {
    logger = getLogger("DS_RedisClient_log");
    
    // ConfigManager에서 설정 읽기
    auto& config = ConfigManager::getInstance();
    redis_server_ip = config.getRedisHost();
    redis_server_port = config.getRedisPort();
    
    logger->info("RedisClient 초기화 - {}:{}", redis_server_ip, redis_server_port);
    
    // 초기 연결 시도
    connect();
}

RedisClient::RedisClient(const std::string& ip, int port) 
    : redis_server_ip(ip), redis_server_port(port) {
    
    logger = getLogger("DS_RedisClient_log");
    logger->info("RedisClient 초기화 - {}:{}", redis_server_ip, redis_server_port);
    
    // 초기 연결 시도
    connect();
}

RedisClient::~RedisClient() {
    disconnect();
}

int RedisClient::connect() {
    return connect(redis_server_ip, redis_server_port);
}

int RedisClient::connect(const std::string& host, int port) {
    std::lock_guard<std::mutex> lock(connection_mutex);
    
    // 기존 연결 해제
    if (redis_cli) {
        redisFree(redis_cli);
        redis_cli = nullptr;
    }
    
    // 새 연결 시도
    struct timeval timeout = {5, 0}; // 5초 타임아웃
    redis_cli = redisConnectWithTimeout(host.c_str(), port, timeout);
    
    if (!redis_cli || redis_cli->err) {
        if (redis_cli) {
            logger->error("Redis 연결 실패: {}", redis_cli->errstr);
            redisFree(redis_cli);
            redis_cli = nullptr;
        } else {
            logger->error("Redis 연결 할당 실패");
        }
        connection_valid = false;
        return -1;
    }
    
    // 연결 테스트 (PING)
    redisReply* reply = (redisReply*)redisCommand(redis_cli, "PING");
    if (!reply) {
        logger->error("Redis PING 실패");
        redisFree(redis_cli);
        redis_cli = nullptr;
        connection_valid = false;
        return -1;
    }
    
    freeReplyObject(reply);
    connection_valid = true;
    logger->info("Redis 연결 성공: {}:{}", host, port);
    
    return 0;
}

bool RedisClient::ensureConnection() {
    if (connection_valid && redis_cli && redis_cli->err == 0) {
        return true;
    }
    
    // 재연결 간격 확인
    auto now = std::chrono::steady_clock::now();
    if (now - last_reconnect_attempt < reconnect_interval) {
        return false;
    }
    
    last_reconnect_attempt = now;
    logger->info("Redis 재연결 시도...");
    
    return connect() == 0;
}

int RedisClient::publishToChannel(const std::string& channel, const std::string& data) {
    if (!ensureConnection()) {
        logger->error("Redis 연결 없음 - 채널: {}", channel);
        return -1;
    }
    
    std::lock_guard<std::mutex> lock(connection_mutex);
    
    // PUBLISH 명령 실행 (바이너리 안전)
    redisReply* reply = (redisReply*)redisCommand(redis_cli, 
        "PUBLISH %b %b",
        channel.c_str(), channel.length(),
        data.c_str(), data.length());
    
    if (!reply) {
        logger->error("Redis PUBLISH 실패 - 채널: {}, 에러: {}", 
                     channel, redis_cli->errstr);
        connection_valid = false;
        return -2;
    }
    
    freeReplyObject(reply);
    
    return 0;
}

int RedisClient::sendData(int channel_type, const std::string& data) {
    // 채널 타입을 채널명으로 변환
    std::string channel_name = getChannelName(channel_type);
    
    if (channel_name == "unknown_channel") {
        logger->error("알 수 없는 채널 타입: {}", channel_type);
        return -3;
    }
    
    // 데이터 유효성 검사
    if (data.empty()) {
        logger->warn("빈 데이터 - 채널: {}", channel_name);
        return -4;
    }
    
    // 채널별 로깅
    switch (channel_type) {
        case CHANNEL_VEHICLE_2K:
        case CHANNEL_VEHICLE_4K:
            logger->debug("차량 데이터 전송 - 채널: {}, 크기: {} bytes", 
                         channel_name, data.length());
            break;
        case CHANNEL_PEDESTRIAN:
            logger->debug("보행자 데이터 전송 - 채널: {}, 크기: {} bytes", 
                         channel_name, data.length());
            break;
        case CHANNEL_STATS:
            logger->info("통계 데이터 전송 - 채널: {}, 크기: {} bytes", 
                        channel_name, data.length());
            break;
        case CHANNEL_QUEUE:
            logger->info("대기행렬 데이터 전송 - 채널: {}, 크기: {} bytes", 
                        channel_name, data.length());
            break;
        case CHANNEL_INCIDENT:
            logger->info("돌발이벤트 데이터 전송 - 채널: {}, 크기: {} bytes", 
                        channel_name, data.length());
            break;
        case CHANNEL_VEHICLE_PRESENCE:
        case CHANNEL_PED_WAITING:
        case CHANNEL_PED_CROSSING:
            logger->debug("Presence 데이터 전송 - 채널: {}, 크기: {} bytes", 
                        channel_name, data.length());
            break;
    }
    
    // 실제 전송
    return publishToChannel(channel_name, data);
}

int RedisClient::disconnect() {
    std::lock_guard<std::mutex> lock(connection_mutex);
    
    if (redis_cli) {
        redisFree(redis_cli);
        redis_cli = nullptr;
    }
    
    connection_valid = false;
    logger->info("Redis 연결 해제");
    
    return 0;
}