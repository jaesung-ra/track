#include "logger.hpp"
#include <sys/stat.h>
#include <sys/types.h>
#include <fstream>
#include <iostream>
#include "../json/json.h"

// Static 변수로 로그 경로와 레벨 캐싱
static std::string g_log_path;
static std::string g_log_level;
static bool g_config_initialized = false;

// config.json에서 설정 읽기 (한 번만 실행)
static void loadConfig() {
    if (!g_config_initialized) {
        // 기본값 설정
        g_log_path = "/home/nvidia/Desktop/deepstream_gb/logs";
        g_log_level = "info";  // 기본 로그 레벨
        
        // config.json 읽기 시도
        std::ifstream config_file("/opt/nvidia/deepstream/deepstream-6.0/sources/apps/sample_apps/deepstream-6.0-calibration/config/config.json");
        if (config_file.is_open()) {
            std::cout << "[DEBUG] Config file opened successfully" << std::endl;
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(config_file, root)) {
                // paths.logs 값 읽기
                if (root.isMember("paths") && root["paths"].isMember("logs")) {
                    std::string log_path = root["paths"]["logs"].asString();
                    
                    // 절대 경로인지 확인
                    if (!log_path.empty()) {
                        if (log_path[0] == '/') {
                            // 절대 경로
                            g_log_path = log_path;
                        } else {
                            // 상대 경로인 경우 base_path와 결합
                            std::string base_path = root["paths"]["base_path"].asString();
                            if (!base_path.empty()) {
                                g_log_path = base_path + "/" + log_path;
                            }
                        }
                    }
                }
                
                // system.log_level 값 읽기
                if (root.isMember("system") && root["system"].isMember("log_level")) {
                    g_log_level = root["system"]["log_level"].asString();
                }
            }
            config_file.close();
        }

        g_config_initialized = true;
        std::cout << "[Logger] Configuration loaded - Path: " << g_log_path 
                  << ", Level: " << g_log_level << std::endl;
    }
}

// 문자열을 spdlog 레벨로 변환하는 함수
static spdlog::level::level_enum getLogLevelEnum(const std::string& level_str) {
    if (level_str == "trace") return spdlog::level::trace;
    if (level_str == "debug") return spdlog::level::debug;
    if (level_str == "info") return spdlog::level::info;
    if (level_str == "warn" || level_str == "warning") return spdlog::level::warn;
    if (level_str == "error") return spdlog::level::err;
    if (level_str == "critical") return spdlog::level::critical;
    if (level_str == "off") return spdlog::level::off;
    
    // 기본값은 info
    return spdlog::level::info;
}

std::shared_ptr<spdlog::logger> getLogger(const char* logger_name){
    // config.json에서 설정 가져오기 (항상 실행)
    loadConfig();
    
    // 이미 생성된 로거가 있는지 확인
    auto existing_logger = spdlog::get(logger_name);
    if(existing_logger != nullptr){
        // 기존 로거의 로그 레벨 업데이트
        spdlog::level::level_enum log_level = getLogLevelEnum(g_log_level);

        existing_logger->set_level(log_level);
        existing_logger->flush_on(log_level);
        return existing_logger;
    }
    
    // 로그 디렉토리 확인 및 생성
    struct stat st = {0};
    if (stat(g_log_path.c_str(), &st) == -1) {
        if (mkdir(g_log_path.c_str(), 0755) == -1) {
            // 디렉토리 생성 실패 시 기본 경로 사용
            g_log_path = "/tmp";
        }
    }
    
    // 각 로거별로 별도의 날짜별 파일 생성
    std::string log_file = g_log_path + "/" + std::string(logger_name) + ".txt";
    std::shared_ptr<spdlog::logger> file_logger = spdlog::daily_logger_mt(
        logger_name,  // 로거 이름
        log_file,     // 파일 경로 (날짜는 자동으로 추가됨)
        23, 59        // 매일 23:59에 새 파일 생성
    );
    
    // 로그 레벨 설정
    spdlog::level::level_enum log_level = getLogLevelEnum(g_log_level);
    file_logger->set_level(log_level);
    // 성능 최적화: info 이상에서만 즉시 flush
    file_logger->flush_on(spdlog::level::info);
    
    // 첫 번째 로거를 기본 로거로 설정
    static bool first_logger = true;
    if (first_logger) {
        spdlog::set_default_logger(file_logger);
        first_logger = false;
    }
    
    return file_logger;
}