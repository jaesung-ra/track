/*
 * config_manager.h
 * 
 * 싱글톤 패턴의 설정 관리자 헤더
 * config.json 파일을 읽어서 파싱하고 관리
 * 
 * 차량 4K 전용 모드 자동 감지:
 * - meta_2k=false && meta_4k=true인 경우 4K 전용 모드로 동작
 * - 차량 4K 전용 모드에서는 다음 기능들이 자동으로 비활성화:
 *   * signal_db
 *   * statistics
 *   * wait_queue
 *   * pedestrian.meta
 *   * pedestrian.presence_check
 *   * vehicle.presence_check
 *   * 모든 돌발 이벤트
 */

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../json/json.h"

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

// 싱글톤 매크로
#define CONFIG ConfigManager::getInstance()

/**
 * @brief 설정 관리자 싱글톤 클래스
 * 
 * config.json 파일을 읽고 파싱하여 전역적으로 설정에 접근
 */
class ConfigManager {
private:
    static std::unique_ptr<ConfigManager> instance;
    static std::mutex instance_mutex;
    
    Json::Value config_root;
    std::string config_path_;
    std::shared_ptr<spdlog::logger> logger = nullptr;
    
    // 경로 캐시
    mutable std::mutex cache_mutex;
    mutable std::map<std::string, std::string> path_cache;
    
    // 설정값 캐시 (초기화 시 한 번만 계산)
    struct CachedFlags {
        // 차량 관련
        bool vehicle_2k_enabled = false;
        bool vehicle_4k_enabled = false;
        bool vehicle_presence_enabled = false;
        int vehicle_presence_detect_frames = 1;
        int vehicle_presence_absence_frames = 3;
        bool vehicle_presence_anti_flicker = true;
        bool is_4k_only_mode = false;
        
        // 보행자 관련
        bool pedestrian_meta_enabled = false;
        bool pedestrian_presence_enabled = false;
        int pedestrian_presence_detect_frames = 1;
        int pedestrian_presence_absence_frames = 3;
        bool pedestrian_presence_anti_flicker = true;
        
        // 분석 관련
        bool statistics_enabled = false;
        bool wait_queue_enabled = false;
        int stats_interval_minutes = 5;
        
        // 돌발이벤트 관련
        bool reverse_driving_enabled = false;
        bool abnormal_stop_enabled = false;
        bool pedestrian_jaywalk_enabled = false;
        bool incident_event_enabled = false;
        
        // Special Site 관련
        bool special_site_enabled = false;
        bool special_site_straight_left = false;
        bool special_site_right = false;
        
        // System
        int camera_fps = 15;
        std::string log_level = "info";
        std::string operation_mode = "manual";
        
        // Redis
        std::string redis_host = "127.0.0.1";
        int redis_port = 6379;
        
        // Paths
        std::string base_path;
        std::string db_filename;
        std::string log_path;
    } cached_flags;
    
    // private 생성자 (싱글톤)
    ConfigManager() = default;
    
    bool loadConfig(const std::string& path);
    bool validate() const;
    void cacheAllFlags();           // 모든 플래그 캐싱
    void logAllSettings() const;    // 모든 설정값 로그 출력
    const Json::Value* getJsonValue(const std::string& key) const;

public:
    // DB 설정 구조체
    struct DBConfig {
        std::string host;
        int port;
        bool enabled;
        
        struct {
            int max_attempts;
            int delay_ms;
        } retry;
        
        struct {
            bool enabled;
            int initial_delay_ms;
            int max_delay_ms;
            double backoff_multiplier;
            int check_interval_sec;
            double jitter_factor;
        } background_reconnect;
    };
    
    // 싱글톤 인스턴스 접근
    static ConfigManager& getInstance();
    
    // 초기화 (단일 config 파일만 사용)
    bool initialize(const std::string& config_path = "config/config.json");
    
    // Path 관련
    std::string getBasePath() const;
    std::string getImagePath(const std::string& type = "") const;
    std::string getFullImagePath(const std::string& type = "") const;
    std::string getROIPath() const;
    std::string getDatabasePath() const;
    std::string getSQLitePath() const;
    std::string getDBFileName() const;
    std::string getLogPath() const;
    std::string getFullPath(const std::string& relative_path) const;
    
    // 모드 관련 메서드들 (캐시된 값 반환)
    std::string getOperationMode() const { return cached_flags.operation_mode; }
    
    // System 설정 (캐시된 값 반환)
    int getCameraFPS() const { return cached_flags.camera_fps; }
    std::string getLogLevel() const { return cached_flags.log_level; }
    
    // Processing modules 설정 (캐시된 값 반환)
    bool isVehicle2KEnabled() const { return cached_flags.vehicle_2k_enabled; }
    bool isVehicle4KEnabled() const { return cached_flags.vehicle_4k_enabled; }
    bool isVehiclePresenceEnabled() const { return cached_flags.vehicle_presence_enabled; }
    int getVehiclePresenceDetectFrames() const { return cached_flags.vehicle_presence_detect_frames; }
    int getVehiclePresenceAbsenceFrames() const { return cached_flags.vehicle_presence_absence_frames; }
    bool getVehiclePresenceAntiFlicker() const { return cached_flags.vehicle_presence_anti_flicker; }
    
    bool isPedestrianMetaEnabled() const { return cached_flags.pedestrian_meta_enabled; }
    bool isPedestrianPresenceEnabled() const { return cached_flags.pedestrian_presence_enabled; }
    int getPedestrianPresenceDetectFrames() const { return cached_flags.pedestrian_presence_detect_frames; }
    int getPedestrianPresenceAbsenceFrames() const { return cached_flags.pedestrian_presence_absence_frames; }
    bool getPedestrianPresenceAntiFlicker() const { return cached_flags.pedestrian_presence_anti_flicker; }
    
    bool isStatisticsEnabled() const { return cached_flags.statistics_enabled; }
    int getStatsIntervalMinutes() const { return cached_flags.stats_interval_minutes; }
    bool isWaitQueueEnabled() const { return cached_flags.wait_queue_enabled; }
    
    // 돌발이벤트 개별 설정 (캐시된 값 반환)
    bool isReverseDrivingEnabled() const { return cached_flags.reverse_driving_enabled; }
    bool isAbnormalStopEnabled() const { return cached_flags.abnormal_stop_enabled; }
    bool isPedestrianJaywalkEnabled() const { return cached_flags.pedestrian_jaywalk_enabled; }
    
    // 돌발이벤트 통합 체크 (캐시된 값 반환)
    bool isIncidentEventEnabled() const { return cached_flags.incident_event_enabled; }
    
    // Special Site 설정 (캐시된 값 반환)
    bool isSpecialSiteEnabled() const { return cached_flags.special_site_enabled; }
    bool isSpecialSiteStraightLeft() const { return cached_flags.special_site_straight_left; }
    bool isSpecialSiteRight() const { return cached_flags.special_site_right; }
    
    // 4K 전용 모드 체크 (캐시된 값 반환)
    bool is4KOnlyMode() const { return cached_flags.is_4k_only_mode; }
    
    // DB 설정
    DBConfig getDBConfig(const std::string& db_name) const;
    std::vector<std::string> getDBNames() const;
    
    // Redis 설정 (캐시된 값 반환)
    std::string getRedisHost() const { return cached_flags.redis_host; }
    int getRedisPort() const { return cached_flags.redis_port; }
    std::string getRedisChannel(const std::string& channel_key) const;
    
    // 기능 플래그
    bool isModuleEnabled(const std::string& module) const;
    
    // Helper methods
    std::string getString(const std::string& key, const std::string& default_value = "") const;
    int getInt(const std::string& key, int default_value = 0) const;
    double getDouble(const std::string& key, double default_value = 0.0) const;
    bool getBool(const std::string& key, bool default_value = false) const;
};

#endif // CONFIG_MANAGER_H