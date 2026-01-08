#ifndef VOLTDB_SOURCE_H
#define VOLTDB_SOURCE_H

#include "../../core/data_provider.h"
#include "../../../json/json.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

/**
 * @brief VoltDB 데이터 소스
 * 
 * 외부 VoltDB 서버에서 CAM ID와 신호 정보 조회
 * 신호역산 지원 (signal_db가 활성화된 경우)
 */
class VoltDBSource : public DataProvider {
public:
    // CAM DB 연결 복구 시 호출될 콜백 - SiteInfo를 전달
    using CamDBRecoveryCallback = std::function<void(const SiteInfo&)>;

private:
    // 재시도 설정 구조체
    struct RetryConfig {
        int max_attempts = 3;
        int delay_ms = 500;
    };
    
    // 백그라운드 재연결 설정 구조체
    struct BackgroundReconnectConfig {
        bool enabled = true;
        int initial_delay_ms = 1000;
        int max_delay_ms = 60000;
        double backoff_multiplier = 2.0;
        int check_interval_sec = 30;
        double jitter_factor = 0.1;
    };

    // CAM DB 설정
    std::string cam_db_host_ = "192.168.11.5";  // L4 스위치 IP
    int cam_db_port_ = 8080;
    RetryConfig cam_db_retry_;
    BackgroundReconnectConfig cam_db_bg_reconnect_;
    
    // Signal DB 설정
    bool signal_db_enabled_ = false;
    bool signal_db_config_enabled_ = false;
    std::string signal_db_host_ = "192.168.6.150";
    int signal_db_port_ = 8080;
    RetryConfig signal_db_retry_;
    BackgroundReconnectConfig signal_db_bg_reconnect_;
    
    // 연결 상태
    std::atomic<bool> connected_{false};
    std::atomic<bool> cam_db_connected_{false};
    std::atomic<bool> signal_db_connected_{false};
    std::atomic<bool> running_{true};
    
    // CAM ID 상태
    std::atomic<bool> cam_id_available_{false};
    std::chrono::steady_clock::time_point cam_db_down_since_;
    
    // 재연결 스레드
    std::thread cam_db_recovery_thread_;
    std::thread signal_db_reconnect_thread_;
    mutable std::mutex reconnect_mutex_;
    
    // 복구 콜백
    CamDBRecoveryCallback recovery_callback_;
    mutable std::mutex callback_mutex_;
    
    // 사이트 정보
    SiteInfo site_info_;
    mutable std::mutex data_mutex_;
    
    // 로거
    std::shared_ptr<spdlog::logger> logger = nullptr;
    
    // 내부 메서드
    std::string executeQuery(const std::string& host, int port, const std::string& query);
    std::string executeQueryWithRetry(const std::string& host, int port,
                                    const std::string& query, const RetryConfig& retry_config);
    std::string getCamIDFromDB(const std::string& ip_address);
    bool connectToCamDB();
    bool connectToSignalDB();
    bool connectWithRetry(const std::string& db_type);
    void camDBRecoveryThreadFunc();
    void signalDBReconnectThreadFunc();
    void notifyRecovery();

public:
    VoltDBSource();
    ~VoltDBSource() override;

    // DataProvider 구현
    bool initialize(const std::string& config_path) override;
    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;

    SiteInfo getSiteInfo() override;
    void setIPAddress(const std::string& ip) override;
    bool isCamIdAvailable() const override;
    bool supportsSignalData() const override;
    
    std::vector<int> getPhaseInfo(const std::string& spot_ints_id, int& LC_CNT) override;
    std::vector<int> getMovementInfo(const std::string& spot_ints_id) override;
    
    SiteInfo::Mode getMode() const override;
    
    void setRecoveryCallback(CamDBRecoveryCallback callback);
    int getDowntimeMinutes() const;
};

#endif