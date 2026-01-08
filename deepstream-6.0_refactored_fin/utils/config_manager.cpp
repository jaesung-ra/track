/*
 * config_manager.cpp
 * 
 * 싱글톤 패턴의 설정 관리자 구현
 * config.json 파일을 읽어서 파싱하고 관리
 * 
 * 차량 4K 전용 모드 자동 감지 및 처리:
 * - meta_2k=false && meta_4k=true 조합 감지
 * - 차량 4K 전용 모드에서 특정 기능들 자동 비활성화
 */

#include "config_manager.h"
#include "../json/jsoncpp.cpp" 
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>

// 싱글톤 인스턴스 정의
std::unique_ptr<ConfigManager> ConfigManager::instance = nullptr;
std::mutex ConfigManager::instance_mutex;

ConfigManager& ConfigManager::getInstance() {
    std::lock_guard<std::mutex> lock(instance_mutex);
    if (!instance) {
        instance = std::unique_ptr<ConfigManager>(new ConfigManager());
    }
    return *instance;
}

bool ConfigManager::initialize(const std::string& config_path) {
    logger = getLogger("DS_ConfigManager_log");
    logger->info("ConfigManager 초기화 시작: {}", config_path);
    
    config_path_ = config_path;
    
    if (!loadConfig(config_path)) {
        logger->error("설정 파일 로드 실패");
        return false;
    }
    
    // 모든 플래그를 캐싱
    cacheAllFlags();
    
    // 모든 설정 로깅
    logAllSettings();
    
    if (!validate()) {
        logger->error("설정 검증 실패");
        return false;
    }
    
    logger->info("ConfigManager 초기화 완료");
    return true;
}

void ConfigManager::logAllSettings() const {
    logger->info("========== CONFIG.JSON 설정값 전체 출력 시작 ==========");
    
    // System 설정
    logger->info("[System 설정]");
    logger->info("  - operation_mode: {}", cached_flags.operation_mode);
    logger->info("  - camera_fps: {}", cached_flags.camera_fps);
    logger->info("  - log_level: {}", cached_flags.log_level);
    
    // Processing Modules - Vehicle
    logger->info("[Vehicle 처리 모듈]");
    logger->info("  - vehicle.meta_2k: {}", cached_flags.vehicle_2k_enabled);
    logger->info("  - vehicle.meta_4k: {}", cached_flags.vehicle_4k_enabled);
    logger->info("  - vehicle.presence_check.enabled: {}", cached_flags.vehicle_presence_enabled);
    if (cached_flags.vehicle_presence_enabled) {
        logger->debug("    * detect_frames: {}", cached_flags.vehicle_presence_detect_frames);
        logger->debug("    * absence_frames: {}", cached_flags.vehicle_presence_absence_frames);
        logger->debug("    * anti_flicker: {}", cached_flags.vehicle_presence_anti_flicker);
    }
    
    // Processing Modules - Pedestrian
    logger->info("[Pedestrian 처리 모듈]");
    logger->info("  - pedestrian.meta: {}", cached_flags.pedestrian_meta_enabled);
    logger->info("  - pedestrian.presence_check.enabled: {}", cached_flags.pedestrian_presence_enabled);
    if (cached_flags.pedestrian_presence_enabled) {
        logger->debug("    * detect_frames: {}", cached_flags.pedestrian_presence_detect_frames);
        logger->debug("    * absence_frames: {}", cached_flags.pedestrian_presence_absence_frames);
        logger->debug("    * anti_flicker: {}", cached_flags.pedestrian_presence_anti_flicker);
    }
    
    // Processing Modules - Analytics
    logger->info("[Analytics 모듈]");
    logger->info("  - statistics: {}", cached_flags.statistics_enabled);
    logger->info("  - stats_interval_minutes: {}", cached_flags.stats_interval_minutes);
    logger->info("  - wait_queue: {}", cached_flags.wait_queue_enabled);
    if (cached_flags.statistics_enabled) {
        logger->info("    * 다음 정각 기준으로 {}분 간격 통계 생성", cached_flags.stats_interval_minutes);
    }
    
    // Processing Modules - Incident Event
    logger->info("[돌발이벤트 모듈]");
    logger->info("  - reverse_driving: {}", cached_flags.reverse_driving_enabled);
    logger->info("  - abnormal_stop_sequence: {}", cached_flags.abnormal_stop_enabled);
    logger->info("  - pedestrian_jaywalk: {}", cached_flags.pedestrian_jaywalk_enabled);
    logger->info("  - incident_event_enabled (종합): {}", cached_flags.incident_event_enabled);
    
    // Special Site
    logger->info("[특별 개소 설정]");
    logger->info("  - special_site: {}", cached_flags.special_site_enabled);
    if (cached_flags.special_site_enabled) {
        logger->info("    * straight_left: {}", cached_flags.special_site_straight_left);
        logger->info("    * right: {}", cached_flags.special_site_right);
        logger->info("    * 모드: {}", 
                    cached_flags.special_site_straight_left ? "직진/좌회전" : "우회전");
    }
    
    // 4K Only Mode
    if (cached_flags.is_4k_only_mode) {
        logger->warn("[4K 전용 모드]: 활성화됨 (meta_2k=false, meta_4k=true)");
    } else {
        logger->info("[4K 전용 모드]: 비활성화");
    }
    
    // Paths
    logger->info("[경로 설정]");
    logger->info("  - base_path: {}", cached_flags.base_path);
    logger->info("  - db_filename: {}", cached_flags.db_filename);
    logger->info("  - log_path: {}", cached_flags.log_path);
    logger->info("  - images_path: {}", getImagePath(""));
    logger->info("  - rois_path: {}", getROIPath());
    
    // Image Types
    logger->info("[이미지 타입별 경로]");
    logger->info("  - vehicle_2k: {}", getImagePath("vehicle_2k"));
    logger->info("  - vehicle_4k: {}", getImagePath("vehicle_4k"));
    logger->info("  - wait_queue: {}", getImagePath("wait_queue"));
    logger->info("  - incident_event: {}", getImagePath("incident_event"));
    
    // Redis
    logger->info("[Redis 설정]");
    logger->info("  - host: {}", cached_flags.redis_host);
    logger->info("  - port: {}", cached_flags.redis_port);
    
    // Redis Channels
    logger->info("[Redis 채널]");
    logger->info("  - vehicle_2k: {}", getRedisChannel("vehicle_2k"));
    logger->info("  - vehicle_4k: {}", getRedisChannel("vehicle_4k"));
    logger->info("  - pedestrian: {}", getRedisChannel("pedestrian"));
    logger->info("  - stats: {}", getRedisChannel("stats"));
    logger->info("  - queue: {}", getRedisChannel("queue"));
    logger->info("  - incident: {}", getRedisChannel("incident"));
    logger->info("  - vehicle_presence: {}", getRedisChannel("vehicle_presence"));
    logger->info("  - ped_crossing: {}", getRedisChannel("ped_crossing"));
    logger->info("  - ped_waiting: {}", getRedisChannel("ped_waiting"));
    
    // VoltDB - CAM DB
    if (cached_flags.operation_mode == "voltdb") {
        logger->info("[VoltDB - CAM DB 설정]");
        auto cam_config = getDBConfig("cam_db");
        logger->info("  - host: {}", cam_config.host);
        logger->info("  - port: {}", cam_config.port);
        logger->debug("  - retry.max_attempts: {}", cam_config.retry.max_attempts);
        logger->debug("  - retry.delay_ms: {}", cam_config.retry.delay_ms);
        logger->debug("  - background_reconnect.enabled: {}", cam_config.background_reconnect.enabled);
        if (cam_config.background_reconnect.enabled) {
            logger->debug("    * initial_delay_ms: {}", cam_config.background_reconnect.initial_delay_ms);
            logger->debug("    * max_delay_ms: {}", cam_config.background_reconnect.max_delay_ms);
            logger->debug("    * backoff_multiplier: {}", cam_config.background_reconnect.backoff_multiplier);
            logger->debug("    * check_interval_sec: {}", cam_config.background_reconnect.check_interval_sec);
            logger->debug("    * jitter_factor: {}", cam_config.background_reconnect.jitter_factor);
        }
        
        // VoltDB - Signal DB
        logger->info("[VoltDB - Signal DB 설정]");
        auto signal_config = getDBConfig("signal_db");
        logger->info("  - enabled: {}", signal_config.enabled);
        if (signal_config.enabled) {
            logger->info("  - host: {}", signal_config.host);
            logger->info("  - port: {}", signal_config.port);
            logger->debug("  - retry.max_attempts: {}", signal_config.retry.max_attempts);
            logger->debug("  - retry.delay_ms: {}", signal_config.retry.delay_ms);
            logger->debug("  - background_reconnect.enabled: {}", signal_config.background_reconnect.enabled);
            if (signal_config.background_reconnect.enabled) {
                logger->debug("    * initial_delay_ms: {}", signal_config.background_reconnect.initial_delay_ms);
                logger->debug("    * max_delay_ms: {}", signal_config.background_reconnect.max_delay_ms);
                logger->debug("    * backoff_multiplier: {}", signal_config.background_reconnect.backoff_multiplier);
                logger->debug("    * check_interval_sec: {}", signal_config.background_reconnect.check_interval_sec);
                logger->debug("    * jitter_factor: {}", signal_config.background_reconnect.jitter_factor);
            }
        }
    }
    
    // 최종 활성화 상태 요약
    logger->info("[최종 활성화 상태 요약]");
    logger->info("  - 차량 2K 메타데이터: {}", cached_flags.vehicle_2k_enabled ? "ON" : "OFF");
    logger->info("  - 차량 4K 메타데이터: {}", cached_flags.vehicle_4k_enabled ? "ON" : "OFF");
    logger->info("  - 차량 Presence: {}", cached_flags.vehicle_presence_enabled ? "ON" : "OFF");
    logger->info("  - 보행자 메타데이터: {}", cached_flags.pedestrian_meta_enabled ? "ON" : "OFF");
    logger->info("  - 보행자 Presence: {}", cached_flags.pedestrian_presence_enabled ? "ON" : "OFF");
    logger->info("  - 통계 생성: {}", cached_flags.statistics_enabled ? "ON" : "OFF");
    logger->info("  - 대기행렬 분석: {}", cached_flags.wait_queue_enabled ? "ON" : "OFF");
    logger->info("  - 돌발이벤트: {}", cached_flags.incident_event_enabled ? "ON" : "OFF");
    if (cached_flags.special_site_enabled) {
        logger->info("  - Special Site: ON ({})", 
                    cached_flags.special_site_straight_left ? "직진/좌회전" : "우회전");
    }
    
    logger->info("========== CONFIG.JSON 설정값 전체 출력 완료 ==========");
}

bool ConfigManager::loadConfig(const std::string& path) {
    std::ifstream config_file(path);
    if (!config_file.is_open()) {
        logger->error("설정 파일을 열 수 없음: {}", path);
        return false;
    }
    
    Json::Reader reader;
    if (!reader.parse(config_file, config_root)) {
        logger->error("JSON 파싱 실패: {}", reader.getFormattedErrorMessages());
        config_file.close();
        return false;
    }
    
    config_file.close();
    
    logger->info("설정 파일 로드 성공");
    return true;
}

void ConfigManager::cacheAllFlags() {
    // 기본 설정값 읽기
    bool raw_vehicle_2k = getBool("processing_modules.vehicle.meta_2k", false);
    bool raw_vehicle_4k = getBool("processing_modules.vehicle.meta_4k", false);
    bool raw_vehicle_presence = getBool("processing_modules.vehicle.presence_check.enabled", false);
    bool raw_pedestrian_meta = getBool("processing_modules.pedestrian.meta", false);
    bool raw_pedestrian_presence = getBool("processing_modules.pedestrian.presence_check.enabled", false);
    bool raw_statistics = getBool("processing_modules.vehicle_analytics.statistics", false);
    bool raw_wait_queue = getBool("processing_modules.vehicle_analytics.wait_queue", false);
    bool raw_reverse_driving = getBool("processing_modules.incident_event.reverse_driving", false);
    bool raw_abnormal_stop = getBool("processing_modules.incident_event.abnormal_stop_sequence", false);
    bool raw_pedestrian_jaywalk = getBool("processing_modules.incident_event.pedestrian_jaywalk", false);
    
    // 4K 전용 모드 체크
    cached_flags.is_4k_only_mode = (!raw_vehicle_2k && raw_vehicle_4k);
    
    // 차량 설정
    cached_flags.vehicle_2k_enabled = raw_vehicle_2k;
    cached_flags.vehicle_4k_enabled = raw_vehicle_4k;

    // 2K와 4K 동시 활성화 방지 (2K 우선)
    if (cached_flags.vehicle_2k_enabled && cached_flags.vehicle_4k_enabled) {
        logger->warn("차량 2K와 4K가 동시 활성화됨 - 4K를 자동 비활성화");
        cached_flags.vehicle_4k_enabled = false;
    }

    // 차량 Presence 설정 (4K 전용 모드에서는 강제 비활성화)
    cached_flags.vehicle_presence_enabled = cached_flags.is_4k_only_mode ? false : raw_vehicle_presence;
    cached_flags.vehicle_presence_detect_frames = getInt("processing_modules.vehicle.presence_check.detect_frames", 1);
    cached_flags.vehicle_presence_absence_frames = getInt("processing_modules.vehicle.presence_check.absence_frames", 3);
    cached_flags.vehicle_presence_anti_flicker = getBool("processing_modules.vehicle.presence_check.anti_flicker", true);
    
    // 보행자 설정 (4K 전용 모드에서는 강제 비활성화)
    cached_flags.pedestrian_meta_enabled = cached_flags.is_4k_only_mode ? false : raw_pedestrian_meta;
    cached_flags.pedestrian_presence_enabled = cached_flags.is_4k_only_mode ? false : raw_pedestrian_presence;
    cached_flags.pedestrian_presence_detect_frames = getInt("processing_modules.pedestrian.presence_check.detect_frames", 1);
    cached_flags.pedestrian_presence_absence_frames = getInt("processing_modules.pedestrian.presence_check.absence_frames", 3);
    cached_flags.pedestrian_presence_anti_flicker = getBool("processing_modules.pedestrian.presence_check.anti_flicker", true);
    
    // 분석 설정 (차량 2K 비활성 또는 4K 전용 모드에서는 강제 비활성화)
    cached_flags.statistics_enabled = (!cached_flags.vehicle_2k_enabled || cached_flags.is_4k_only_mode) 
                                     ? false : raw_statistics;
    cached_flags.wait_queue_enabled = (!cached_flags.vehicle_2k_enabled || cached_flags.is_4k_only_mode) 
                                     ? false : raw_wait_queue;
    cached_flags.stats_interval_minutes = getInt("processing_modules.vehicle_analytics.stats_interval_minutes", 5);

    // stats_interval_minutes 검증 (60의 약수만 허용)
    int raw_interval = getInt("processing_modules.vehicle_analytics.stats_interval_minutes", 5);
    
    // 60의 약수인지 검증 (1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 30, 60)
    if (raw_interval <= 0 || raw_interval > 60 || (60 % raw_interval != 0)) {
        logger->warn("잘못된 stats_interval_minutes 값: {}분 (60의 약수가 아님)", raw_interval);
        logger->warn("기본값 5분으로 설정. 허용값: 1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 30, 60");
        cached_flags.stats_interval_minutes = 5;  // 기본값
    } else {
        cached_flags.stats_interval_minutes = raw_interval;
        logger->info("인터벌 통계 주기 설정: {}분", cached_flags.stats_interval_minutes);
    }
    
    // 돌발이벤트 설정 (차량 2K 비활성 또는 4K 전용 모드에서는 강제 비활성화)
    bool incident_allowed = cached_flags.vehicle_2k_enabled && !cached_flags.is_4k_only_mode;
    cached_flags.reverse_driving_enabled = incident_allowed ? raw_reverse_driving : false;
    cached_flags.abnormal_stop_enabled = incident_allowed ? raw_abnormal_stop : false;
    cached_flags.pedestrian_jaywalk_enabled = incident_allowed ? raw_pedestrian_jaywalk : false;
    cached_flags.incident_event_enabled = cached_flags.reverse_driving_enabled || 
                                         cached_flags.abnormal_stop_enabled || 
                                         cached_flags.pedestrian_jaywalk_enabled;
    
    // Special Site 설정
    cached_flags.special_site_enabled = getBool("processing_modules.special_site.enabled", false);
    cached_flags.special_site_straight_left = getBool("processing_modules.special_site.straight_left", false);
    cached_flags.special_site_right = getBool("processing_modules.special_site.right", false);

    // Special Site 모드 검증 및 자동 조정
    if (cached_flags.special_site_enabled) {
        // 2K 전용 모드에서만 동작
        if (!cached_flags.vehicle_2k_enabled || cached_flags.vehicle_4k_enabled) {
            logger->warn("Special Site는 2K 전용 모드에서만 동작 (2K=true, 4K=false 필요)");
            cached_flags.special_site_enabled = false;
            cached_flags.special_site_straight_left = false;
            cached_flags.special_site_right = false;
        } else {
            // 둘 다 false면 straight_left를 true로 자동 보정
            if (!cached_flags.special_site_straight_left && !cached_flags.special_site_right) {
                logger->warn("Special Site 설정 자동 보정: straight_left와 right가 모두 false");
                logger->warn("기본값으로 straight_left=true, right=false로 설정");
                cached_flags.special_site_straight_left = true;
                cached_flags.special_site_right = false;
            }
            // 둘 다 true면 straight_left만 활성화
            else if (cached_flags.special_site_straight_left && cached_flags.special_site_right) {
                logger->warn("Special Site 설정 자동 보정: straight_left와 right가 모두 true");
                logger->warn("straight_left=true, right=false로 설정");
                cached_flags.special_site_straight_left = true;
                cached_flags.special_site_right = false;
            }
            
            // Special Site 모드에서는 통계와 대기행렬 자동 비활성화
            if (cached_flags.statistics_enabled || cached_flags.wait_queue_enabled) {
                logger->warn("Special Site 모드 활성화 - 통계와 대기행렬 자동 비활성화");
                cached_flags.statistics_enabled = false;
                cached_flags.wait_queue_enabled = false;
            }
        }
    }
    
    // System 설정
    cached_flags.camera_fps = getInt("system.camera_fps", 15);
    cached_flags.log_level = getString("system.log_level", "info");
    cached_flags.operation_mode = getString("system.operation_mode", "manual");
    
    // Redis 설정
    cached_flags.redis_host = getString("redis.host", "127.0.0.1");
    cached_flags.redis_port = getInt("redis.port", 6379);
    
    // Path 설정
    cached_flags.base_path = getString("paths.base_path", 
        "/opt/nvidia/deepstream/deepstream-6.0/sources/objectDetector_GB/");
    cached_flags.db_filename = getString("paths.sqlite_db.filename", "test.db");
    cached_flags.log_path = getString("paths.logs", "logs");
    
    // 초기화 시 한 번만 모드 정보 로깅
    if (cached_flags.is_4k_only_mode) {
        logger->warn("========================================================");
        logger->warn("차량 4K 전용 모드 활성화됨 (meta_2k=false, meta_4k=true)");
        logger->warn("다음 기능들이 자동으로 비활성화:");
        logger->warn("  - pedestrian (4K 전용 모드에서는 보행자 미검출)");
        logger->warn("  - signal_db  (4K 전용 모드에서는 신호 데이터 불필요)");
        logger->warn("  - statistics (4K 전용 모드에서는 통계 생성 불가)");
        logger->warn("  - wait_queue (4K 전용 모드에서는 대기행렬 분석 불가)");
        logger->warn("  - 모든 돌발 이벤트 (4K 전용 모드에서는 돌발이벤트 생성 불가)");
        logger->warn("  - 차량/보행자 presence (4K 전용 모드에서는 presence 생성 불필요)");
        logger->warn("========================================================");
    } else if (!cached_flags.vehicle_2k_enabled) {
        logger->info("차량 2K 비활성 감지 (4K도 비활성) - 통계, 대기행렬, 신호DB, 돌발이벤트 자동 비활성화");
    }
}

// 기본 경로 관련 메서드들
std::string ConfigManager::getBasePath() const {
    return cached_flags.base_path;
}

std::string ConfigManager::getImagePath(const std::string& type) const {
    std::string base_path = getBasePath();
    std::string image_dir = getString("paths.sub_paths.images", "images");
    
    if (type.empty()) {
        // type이 비어있으면 기본 이미지 경로 반환
        return base_path + image_dir;
    }
    
    // type이 지정된 경우
    std::string type_dir = getString("paths.image_types." + type, "");
    if (!type_dir.empty()) {
        return base_path + image_dir + "/" + type_dir;
    }
    return base_path + image_dir + "/" + type;  // fallback
}

std::string ConfigManager::getFullImagePath(const std::string& type) const {
    // getImagePath가 이미 전체 경로를 반환하므로 그대로 반환
    return getImagePath(type);
}

std::string ConfigManager::getROIPath() const {
    // 통합된 ROI 경로 사용
    return getString("paths.sub_paths.rois", "settings/rois");
}

std::string ConfigManager::getSQLitePath() const {
    // getDatabasePath()와 동일하게 수정
    return getDatabasePath();
}

std::string ConfigManager::getDatabasePath() const {
    std::string base_path = getBasePath();
    std::string db_dir = getString("paths.sub_paths.db", "");
    
    if (db_dir.empty()) {
        return base_path;  // db 하위 경로가 없으면 base_path 그대로 사용
    }
    return base_path + db_dir;
}

std::string ConfigManager::getDBFileName() const {
    return cached_flags.db_filename;
}

std::string ConfigManager::getLogPath() const {
    return cached_flags.log_path;
}

std::string ConfigManager::getFullPath(const std::string& relative_path) const {
    if (relative_path.empty() || relative_path[0] == '/') {
        return relative_path;  // 이미 절대 경로
    }
    
    // 캐시 확인
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = path_cache.find(relative_path);
        if (it != path_cache.end()) {
            return it->second;
        }
    }
    
    std::string full_path = getBasePath() + "/" + relative_path;
    
    // 캐시에 저장
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        path_cache[relative_path] = full_path;
    }
    
    return full_path;
}

// DB 설정 접근 메서드
ConfigManager::DBConfig ConfigManager::getDBConfig(const std::string& db_name) const {
    DBConfig config;

    // 차량 2K가 비활성이면 signal_db는 항상 disabled
    if (!cached_flags.vehicle_2k_enabled && db_name == "signal_db") {
        config.enabled = false;
        return config;
    }
    
    // 차량 4K 전용 모드에서 signal_db는 항상 disabled
    if (cached_flags.is_4k_only_mode && db_name == "signal_db") {
        config.enabled = false;
        return config;
    }
    
    std::string base_key = "voltdb." + db_name;
    if (!config_root.isMember("voltdb") || !config_root["voltdb"].isMember(db_name)) {
        // 로그 제거 - 필요시 호출자가 처리
        return config;
    }
    
    // 기본 설정
    config.host = getString(base_key + ".host", "localhost");
    config.port = getInt(base_key + ".port", 8080);
    config.enabled = getBool(base_key + ".enabled", false);
    
    // Retry 설정
    config.retry.max_attempts = getInt(base_key + ".retry.max_attempts", 3);
    config.retry.delay_ms = getInt(base_key + ".retry.delay_ms", 500);
    
    // Background reconnect 설정
    config.background_reconnect.enabled = getBool(base_key + ".background_reconnect.enabled", true);
    config.background_reconnect.initial_delay_ms = getInt(base_key + ".background_reconnect.initial_delay_ms", 1000);
    config.background_reconnect.max_delay_ms = getInt(base_key + ".background_reconnect.max_delay_ms", 60000);
    config.background_reconnect.backoff_multiplier = getDouble(base_key + ".background_reconnect.backoff_multiplier", 2.0);
    config.background_reconnect.check_interval_sec = getInt(base_key + ".background_reconnect.check_interval_sec", 30);
    config.background_reconnect.jitter_factor = getDouble(base_key + ".background_reconnect.jitter_factor", 0.1);
    
    return config;
}

std::vector<std::string> ConfigManager::getDBNames() const {
    std::vector<std::string> names;
    
    if (config_root.isMember("voltdb")) {
        const Json::Value& voltdb = config_root["voltdb"];
        for (const auto& name : voltdb.getMemberNames()) {
            names.push_back(name);
        }
    }
    
    return names;
}

// 기능 플래그
bool ConfigManager::isModuleEnabled(const std::string& module) const {
    return getBool("processing_modules." + module, false);
}

// Redis 채널 설정
std::string ConfigManager::getRedisChannel(const std::string& channel_key) const {
    return getString("redis.channels." + channel_key, "");
}

// Helper 메서드들
std::string ConfigManager::getString(const std::string& key, const std::string& default_value) const {
    const Json::Value* value = getJsonValue(key);
    if (value && value->isString()) {
        return value->asString();
    }
    return default_value;
}

int ConfigManager::getInt(const std::string& key, int default_value) const {
    const Json::Value* value = getJsonValue(key);
    if (value && value->isInt()) {
        return value->asInt();
    }
    return default_value;
}

double ConfigManager::getDouble(const std::string& key, double default_value) const {
    const Json::Value* value = getJsonValue(key);
    if (value && value->isNumeric()) {
        return value->asDouble();
    }
    return default_value;
}

bool ConfigManager::getBool(const std::string& key, bool default_value) const {
    const Json::Value* value = getJsonValue(key);
    if (value && value->isBool()) {
        return value->asBool();
    }
    return default_value;
}

const Json::Value* ConfigManager::getJsonValue(const std::string& key) const {
    std::vector<std::string> parts;
    std::stringstream ss(key);
    std::string part;
    
    while (std::getline(ss, part, '.')) {
        parts.push_back(part);
    }
    
    const Json::Value* current = &config_root;
    
    for (const auto& p : parts) {
        if (!current->isMember(p)) {
            return nullptr;
        }
        current = &(*current)[p];
    }
    
    return current;
}

bool ConfigManager::validate() const {
    // 필수 설정 확인
    if (!config_root.isMember("paths")) {
        logger->error("paths 섹션이 없음");
        return false;
    }
    
    if (!config_root.isMember("system") || !config_root["system"].isMember("operation_mode")) {
        logger->error("system.operation_mode가 없음");
        return false;
    }
    
    std::string mode = cached_flags.operation_mode;
    if (mode != "voltdb" && mode != "manual") {
        logger->error("잘못된 operation_mode: {}", mode);
        return false;
    }
    
    // 경로 유효성 확인
    std::string base_path = cached_flags.base_path;
    struct stat st;
    if (stat(base_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        logger->error("base_path가 유효하지 않음: {}", base_path);
        return false;
    }

    // 설정 충돌 경고 (초기화 시 한 번만)
    if (!cached_flags.vehicle_2k_enabled) {
        if (getBool("processing_modules.vehicle_analytics.statistics", false)) {
            logger->warn("config.json에 statistics=true이지만 차량 2K 비활성으로 무시됨");
        }
        if (getBool("processing_modules.vehicle_analytics.wait_queue", false)) {
            logger->warn("config.json에 wait_queue=true이지만 차량 2K 비활성으로 무시됨");  
        }
        if (getBool("voltdb.signal_db.enabled", false)) {
            logger->warn("config.json에 signal_db.enabled=true이지만 차량 2K 비활성으로 무시됨");
        }
    }

    if (cached_flags.is_4k_only_mode) {
        // 4K 전용 모드에서 활성화되어 있으면 안 되는 설정들 경고
        if (getBool("processing_modules.vehicle.presence_check.enabled", false)) {
            logger->warn("config.json에 vehicle.presence_check.enabled=true이지만 4K 전용 모드에서는 무시됨");
        }
        if (getBool("processing_modules.pedestrian.meta", false)) {
            logger->warn("config.json에 pedestrian.meta=true이지만 4K 전용 모드에서는 무시됨");
        }
        if (getBool("processing_modules.pedestrian.presence_check.enabled", false)) {
            logger->warn("config.json에 pedestrian.presence_check.enabled=true이지만 4K 전용 모드에서는 무시됨");
        }
        
        // 돌발이벤트 설정 경고
        if (getBool("processing_modules.incident_event.reverse_driving", false) ||
            getBool("processing_modules.incident_event.abnormal_stop_sequence", false) ||
            getBool("processing_modules.incident_event.pedestrian_jaywalk", false)) {
            logger->warn("config.json에 돌발이벤트가 활성화되어 있지만 4K 전용 모드에서는 무시됨");
        }
    }
    
    return true;
}