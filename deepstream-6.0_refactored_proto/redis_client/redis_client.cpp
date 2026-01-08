#include "redis_client.h"

const std::map<std::string, std::string> RedisClient::car_type_table = {
    {"car", "PCAR"}, {"bus", "MBUS"}, {"bus-45", "LBUS"},
    {"truck", "MTRUCK"}, {"truck-45T", "LTRUCK"}, {"motorbike", "MOTOR"}
};

RedisClient::RedisClient(const std::map<int, obj_data>& det_obj_ref, const std::string& config_path) 
    : det_obj(det_obj_ref),
    server(ServerManager::createServer("VoltDB")),
    server_receiver(server.get()),
    last_reconnect_attempt(std::chrono::steady_clock::now() - reconnect_interval) {
    
    try {
        // 로거 초기화
        logger = getLogger("DS_RedisClient_log");
        logger->info("RedisClient 모듈 초기화 시작");

        // 설정 파일에서 Redis 정보 로드 (ConfigParser 사용 또는 기본값 설정)
        // ConfigParser redis_config(config_path);
        // redis_server_ip = redis_config.getString("redis_server_ip", "127.0.0.1");
        // redis_server_port = redis_config.getInt("port", 6379);
        // channel_2k = redis_config.getString("channel_2k", "detection_info_2k");
        // channel_query = redis_config.getString("channel_query", "QUERYFROM_2K_DS");
        redis_server_ip = "127.0.0.1";
        redis_server_port = 6379;
        channel_2k = "detection_info_2k";
        channel_query = "QUERYFROM_2K_DS";

        logger->info("Redis 설정: IP={}, 포트={}, 데이터채널={}, 쿼리채널={}", 
                    redis_server_ip, redis_server_port, channel_2k, channel_query);

        // Redis 서버 연결
        if (redisConnect() != 0) {
            logger->warn("초기 Redis 연결 실패... 추후 재연결 시도 예정");
        }

        // ServerReceiver 콜백 설정
        server_receiver.setCallback([this](const std::string& query) {
            return this->redisSendQuery(query);
        });

        logger->info("RedisClient 모듈 초기화 완료");
    } catch (const std::exception& e) {
        if (logger) {
            logger->critical("RedisClient 초기화 중 치명적 오류 발생: {}", e.what());
        } else {
            std::cerr << "RedisClient 초기화 중 오류 발생 (로거 초기화 전): " << e.what() << std::endl;
        }
    }
}

RedisClient::~RedisClient() {
    try {
        if (logger) {
            logger->info("RedisClient 종료 시작");
        }
        
        // Redis 연결 해제
        redisDisconnect();
        
        logger->info("RedisClient 종료 완료");
    } catch (const std::exception& e) {
        if (logger) {
            logger->error("RedisClient 소멸자 오류: {}", e.what());
        }
    }
}

int RedisClient::redisConnect() {
    try {
        // 기존 연결이 있으면 해제
        if (redis_cli) {
            redisFree(redis_cli);
            redis_cli = nullptr;
        }
        
        logger->debug("Redis 서버 연결 시도 중: {}:{}", redis_server_ip, redis_server_port);
        
        // 타임아웃 설정 (1.5초)
        struct timeval timeout = {1, 500000};
        redis_cli = redisConnectWithTimeout(redis_server_ip.c_str(), redis_server_port, timeout);
        
        if (redis_cli == nullptr || redis_cli->err) {
            logger->error("Redis 연결 오류: {}", 
                        (redis_cli ? redis_cli->errstr : "Redis 컨텍스트 할당 실패"));
            
            if (redis_cli) {
                redisFree(redis_cli);
                redis_cli = nullptr;
            }
            
            connection_valid = false;
            return -1;
        }
        
        // 연결 테스트
        redisReply* reply = (redisReply*)redisCommand(redis_cli, "PING");
        if (!reply) {
            logger->error("Redis PING 실패: {}", redis_cli->errstr);
            redisFree(redis_cli);
            redis_cli = nullptr;
            connection_valid = false;
            return -2;
        }
        
        freeReplyObject(reply);
        connection_valid = true;
        logger->info("Redis 서버 연결 성공: {}:{}", redis_server_ip, redis_server_port);
        return 0;
    } catch (const std::exception& e) {
        logger->error("Redis 연결 중 예외 발생: {}", e.what());
        connection_valid = false;
        return -3;
    }
}

bool RedisClient::ensureConnection() {
    if (connection_valid && redis_cli && redis_cli->err == 0) {
        return true;
    }
    
    // 재연결 시도 간격 제한 (5초마다)
    auto now = std::chrono::steady_clock::now();
    if (now - last_reconnect_attempt < reconnect_interval) {
        return false;
    }
    
    last_reconnect_attempt = now;
    logger->info("Redis 서버 재연결 시도...");
    return redisConnect() == 0;
}

std::string RedisClient::escapeString(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    
    for (char c : input) {
        if (c == '\'' || c == '\\' || c == '"') {
            result += '\\';
        }
        result += c;
    }
    
    return result;
}

// int RedisClient::redisSendString(const std::string& data, const std::string& channel) {
//     if (!ensureConnection()) {
//         logger->warn("Redis 연결 실패로 데이터 전송 건너뜀: ID={}", id);
//         return -1;
//     }

//     std::string channel_to_send = (channel == "2k") ? channel_2k : channel_query;

//     redisReply* reply = (redisReply*)redisCommand(
//         redis_cli, "PUBLISH %s %s", channel_to_send.c_str(), data.c_str());
        
//     if (!reply) {
//         logger->error("Redis 명령 실패: {}", redis_cli->errstr);
//         connection_valid = false;
//         return -2;
//     }
    
//     logger->info("{} 데이터가 Redis 채널 {}로 전송됨", data, channel_to_send);
//     freeReplyObject(reply);
//     return 0;
// }

int RedisClient::redisSendData(int id) {
    try {
        // 객체 데이터 가져오기
        if (det_obj.find(id) == det_obj.end()) {
            logger->warn("ID {}에 해당하는 객체 데이터를 찾을 수 없습니다.", id);
            return -1;
        }
        
        const obj_data& detected_object = det_obj.at(id);
        
        // 차종 확인
        std::string vehicle_type = "PCAR";  // 기본값
        auto type_it = car_type_table.find(detected_object.label);
        if (type_it != car_type_table.end()) {
            vehicle_type = type_it->second;
        }
        
        logger->debug("객체 ID {} 처리 중: 차종={}, 차로={}, 방향={}", 
                     id, vehicle_type, detected_object.lane, detected_object.dir_out);
        
        {
            std::lock_guard<std::mutex> db_lock(sqlite_lock);
            sqlite3* db_insert = nullptr;
            int rc = sqlite3_open_v2("test.db", &db_insert, SQLITE_OPEN_READWRITE, nullptr);
            if (rc) {
                logger->error("SQLite DB Open 실패: {}", sqlite3_errmsg(db_insert));
            }else{
                sqlite3_busy_timeout(db_insert, 50);
            }

            // 준비된 명령문 사용
            sqlite3_stmt* stmt = nullptr;
            const char* sql = 
                "INSERT INTO test_table ("
                "id, turn_sensing_date, stop_sensing_date, first_detected_time, "
                "label, lane, dir_out, turn_point_speed, stop_point_speed, interval_speed, "
                "sensing_time, image_name) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
                
            rc = sqlite3_prepare_v2(db_insert, sql, -1, &stmt, nullptr);
            if (rc != SQLITE_OK) {
                logger->error("SQL 준비 실패: {}", sqlite3_errmsg(db_insert));
                return -3;
            }

            // 파라미터 바인딩
            sqlite3_bind_int(stmt, 1, id);
            sqlite3_bind_int(stmt, 2, detected_object.turn_time);
            sqlite3_bind_int(stmt, 3, detected_object.stop_pass_time);
            sqlite3_bind_int(stmt, 4, detected_object.first_detected_time);
            sqlite3_bind_text(stmt, 5, vehicle_type.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 6, detected_object.lane);
            sqlite3_bind_int(stmt, 7, detected_object.dir_out);
            sqlite3_bind_double(stmt, 8, detected_object.turn_pass_speed);
            sqlite3_bind_double(stmt, 9, detected_object.stop_pass_speed);
            sqlite3_bind_double(stmt, 10, detected_object.avg_speed);
            sqlite3_bind_int(stmt, 11, detected_object.turn_time - detected_object.first_detected_time);
            sqlite3_bind_text(stmt, 12, detected_object.image_name.c_str(), -1, SQLITE_STATIC);

            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            
            if (rc != SQLITE_DONE) {
                logger->error("데이터 삽입 실패: {}", sqlite3_errmsg(db_insert));
                return -4;
            }
            
            logger->info("객체 ID {} 데이터가 SQLite DB에 저장됨", id);
            sqlite3_close(db_insert);
        }

        // Redis로 전송
        if (!ensureConnection()) {
            logger->warn("Redis 연결 실패로 데이터 전송 건너뜀: ID={}", id);
            return -5;
        }

        // CSV 형식으로 데이터 포맷팅
        std::ostringstream oss;
        oss << id << ","//0
            << detected_object.stop_pass_time << ","//1
            << vehicle_type << ","//2
            << detected_object.lane << ","//3
            << detected_object.dir_out << ","//4
            << detected_object.turn_time << ","//5
            << detected_object.turn_pass_speed << ","//6
            << detected_object.stop_pass_time << ","//7
            << detected_object.stop_pass_speed << ","//8
            << detected_object.avg_speed << ","//9
            << detected_object.first_detected_time << ","//10
            << (detected_object.turn_time - detected_object.first_detected_time) << ","//11
            << detected_object.image_name;//12
        
        std::string data = oss.str();
        
        // Redis PUBLISH 명령어 실행
        redisReply* reply = (redisReply*)redisCommand(
            redis_cli, "PUBLISH %s %s", channel_2k.c_str(), data.c_str());
            
        if (!reply) {
            logger->error("Redis 명령 실패: {}", redis_cli->errstr);
            connection_valid = false;
            return -6;
        }
        
        logger->info("객체 ID {} 데이터가 Redis 채널 {}로 전송됨", id, channel_2k);
        freeReplyObject(reply);
        
        return 0;
    } catch (const std::exception& e) {
        logger->error("redisSendData 중 예외 발생 (ID={}): {}", id, e.what());
        return -7;
    }
}

int RedisClient::redisSendQuery(const std::string& query) {
    try {
        if (!ensureConnection()) {
            logger->warn("Redis 연결 실패로 쿼리 전송 건너뜀");
            return -1;
        }
        
        logger->debug("Redis 쿼리 전송: {}", query.substr(0, 100) + (query.length() > 100 ? "..." : ""));
        
        // Redis PUBLISH 명령어 실행
        std::lock_guard<std::mutex> lock(query_lock);
        redisReply* reply = (redisReply*)redisCommand(
            redis_cli, "PUBLISH %s %s", channel_query.c_str(), query.c_str());
            
        if (!reply) {
            logger->error("Redis 쿼리 전송 실패: {}", redis_cli->errstr);
            connection_valid = false;
            return -2;
        }
        
        logger->info("쿼리가 Redis 채널 {}로 전송됨", channel_query);
        freeReplyObject(reply);
        
        return 0;
    } catch (const std::exception& e) {
        logger->error("redisSendQuery 중 예외 발생: {}", e.what());
        return -3;
    }
}

int RedisClient::redisDisconnect() {
    try {
        if (redis_cli) {
            redisFree(redis_cli);
            redis_cli = nullptr;
            connection_valid = false;
            logger->info("Redis 연결이 해제되었습니다.");
        }
        return 0;
    } catch (const std::exception& e) {
        logger->error("redisDisconnect 중 예외 발생: {}", e.what());
        return -1;
    }
}
