#ifndef REDIS_CLIENT_H
#define REDIS_CLIENT_H

#include <hiredis/hiredis.h>  
#include <sqlite3.h>          
#include <map>
#include <memory>             
#include <string>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
#include <iostream>

#include "process_meta.h"
#include "server_receiver.h"
#include "server_manager.h"   
#include "server_interface.h" 
#include "config_parser.h"
#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

/**
 * @brief Redis 통신을 담당하는 클래스
 * 
 * 딥스트림 객체 탐지 결과를 Redis 서버와 SQLite DB에 저장하고
 * 통계 쿼리를 VoltDB로 전송하는 기능을 제공합니다.
 */
class RedisClient {
private:
    // 차량 타입 매핑 테이블 (객체 감지 라벨 -> DB 저장 형식)
    static const std::map<std::string, std::string> car_type_table;
    
    // Redis 연결 관련 변수
    redisContext* redis_cli = nullptr;
    std::string redis_server_ip;
    int redis_server_port;
    std::string channel_2k;         // 차량 데이터 전송 채널
    std::string channel_query;      // 쿼리 전송 채널

    // 데이터 참조
    const std::map<int, obj_data>& det_obj;

    // 쿼리 전송 채널 뮤텍스
    std::mutex query_lock;

    // 서버 통신 관련 객체
    std::unique_ptr<ServerInterface> server;
    ServerReceiver server_receiver;  
    
    // 재연결 관련 변수
    std::atomic<bool> connection_valid{false};
    std::chrono::steady_clock::time_point last_reconnect_attempt;
    const std::chrono::seconds reconnect_interval{5};
    
    // 로거 인스턴스
    std::shared_ptr<spdlog::logger> logger = NULL;

    /**
     * @brief Redis 서버에 연결
     * @return 성공 시 0, 실패 시 음수 값
     */
    int redisConnect();

    /**
     * @brief Redis 연결 상태 확인 및 필요시 재연결
     * @return 연결 상태가 유효하면 true, 아니면 false
     */
    bool ensureConnection();

    /**
     * @brief 쿼리 파라미터 이스케이프 처리
     * @param input 원본 문자열
     * @return 이스케이프 처리된 문자열
     */
    std::string escapeString(const std::string& input);

public:
    /**
     * @brief 생성자
     * @param det_obj_ref 감지된 객체 데이터 참조
     * @param config_path 설정 파일 경로 (기본값: "")
     */
    RedisClient(const std::map<int, obj_data>& det_obj_ref, const std::string& config_path = "");
    
    /**
     * @brief 소멸자
     */
    ~RedisClient();
    
    /**
     * @brief 객체 데이터 전송
     * @param id 객체 ID
     * @return 성공 시 0, 실패 시 음수 값
     */
    int redisSendData(int id);

    /**
     * @brief 쿼리 전송
     * @param query SQL 쿼리문
     * @return 성공 시 0, 실패 시 음수 값
     */
    int redisSendQuery(const std::string& query);

    /**
     * @brief Redis 연결 해제
     * @return 성공 시 0, 실패 시 음수 값
     */
    int redisDisconnect();

    /**
     * @brief 카메라 ID 반환
     * @return 카메라 ID 문자열
     */
    std::string camID() { return server_receiver.getCamrID(); }
};

#endif 
