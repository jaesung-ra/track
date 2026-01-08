#ifndef REDIS_CLIENT_H
#define REDIS_CLIENT_H

#include <atomic>
#include <chrono>
#include <hiredis/hiredis.h>
#include <memory>
#include <mutex>
#include <string>

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

/**
 * @brief Redis 통신을 담당하는 클래스
 * 
 * 로컬 Redis 서버에 데이터를 전송
 * 자동 재연결 및 연결 상태 관리 기능 포함
 */
class RedisClient {
private:
    // Redis 연결
    redisContext* redis_cli = nullptr;
    std::string redis_server_ip = "127.0.0.1";
    int redis_server_port = 6379;
    
    // 연결 관리
    std::mutex connection_mutex;
    std::atomic<bool> connection_valid{false};
    std::chrono::steady_clock::time_point last_reconnect_attempt;
    const std::chrono::seconds reconnect_interval{5};  // 5초마다 재연결 시도
    
    // 로거
    std::shared_ptr<spdlog::logger> logger;
    
    /**
     * @brief Redis 서버 연결
     * @return 성공 시 0, 실패 시 음수
     */
    int connect();
    
    /**
     * @brief Redis 서버 연결 (host, port 지정)
     * @param host 호스트 주소
     * @param port 포트 번호
     * @return 성공 시 0, 실패 시 음수
     */
    int connect(const std::string& host, int port);
    
    /**
     * @brief Redis 연결 상태 확인 및 재연결
     * @return 연결이 유효하면 true
     */
    bool ensureConnection();
    
    /**
     * @brief 채널로 데이터 전송 (내부 함수)
     * @param channel 채널명
     * @param data 전송할 데이터
     * @return 성공 시 0, 실패 시 음수 값
     */
    int publishToChannel(const std::string& channel, const std::string& data);

public:
    /**
     * @brief 생성자 (ConfigManager에서 설정 로드)
     */
    RedisClient();
    
    /**
     * @brief 생성자 (수동 설정)
     * @param ip Redis 서버 IP
     * @param port Redis 서버 포트
     */
    RedisClient(const std::string& ip, int port);
    
    /**
     * @brief 소멸자
     */
    ~RedisClient();
    
    /**
     * @brief 데이터 전송 (통합 메서드)
     * @param channel_type 채널 타입 (channel_types.h의 ChannelType enum)
     * @param data 전송할 데이터 (CSV 또는 JSON 형식)
     * @return 성공 시 0, 실패 시 음수 값
     *         -1: 연결 실패
     *         -2: PUBLISH 실패
     *         -3: 잘못된 채널 타입
     *         -4: 빈 데이터
     */
    int sendData(int channel_type, const std::string& data);
    
    /**
     * @brief Redis 연결 해제
     * @return 성공 시 0, 실패 시 음수 값
     */
    int disconnect();
    
    /**
     * @brief 연결 상태 확인
     * @return 연결되어 있으면 true
     */
    bool isConnected() const { return connection_valid.load(); }
};

#endif // REDIS_CLIENT_H