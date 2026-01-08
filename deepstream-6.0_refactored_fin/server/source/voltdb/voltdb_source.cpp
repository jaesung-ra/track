#include "voltdb_source.h"
#include "../../../api/rest.h"
#include "../../../utils/config_manager.h"
#include <algorithm>
#include <fstream>
#include <iostream>

VoltDBSource::VoltDBSource() {
    logger = getLogger("DS_VoltDBSource_log");
    logger->info("VoltDBSource 생성");
}

VoltDBSource::~VoltDBSource() {
    disconnect();
}

bool VoltDBSource::initialize(const std::string& config_path) {
    try {
        // ConfigManager에서 설정 읽기
        auto& config = ConfigManager::getInstance();
        
        // CAM DB 설정 가져오기
        auto cam_db_config = config.getDBConfig("cam_db");
        cam_db_host_ = cam_db_config.host;
        cam_db_port_ = cam_db_config.port;
        
        // CAM DB 재시도 설정
        cam_db_retry_ = {
            cam_db_config.retry.max_attempts,
            cam_db_config.retry.delay_ms
        };
        
        // CAM DB 백그라운드 재연결 설정
        cam_db_bg_reconnect_ = {
            cam_db_config.background_reconnect.enabled,
            cam_db_config.background_reconnect.initial_delay_ms,
            cam_db_config.background_reconnect.max_delay_ms,
            cam_db_config.background_reconnect.backoff_multiplier,
            cam_db_config.background_reconnect.check_interval_sec,
            cam_db_config.background_reconnect.jitter_factor
        };
        
        // Signal DB 설정 가져오기
        auto signal_db_config = config.getDBConfig("signal_db");
        signal_db_enabled_ = signal_db_config.enabled;
        signal_db_config_enabled_ = signal_db_enabled_;
        
        // Signal DB가 활성화된 경우에만 host/port 설정
        if (signal_db_enabled_) {
            signal_db_host_ = signal_db_config.host;
            signal_db_port_ = signal_db_config.port;
            
            // Signal DB 재시도 설정
            signal_db_retry_ = {
                signal_db_config.retry.max_attempts,
                signal_db_config.retry.delay_ms
            };
            
            // Signal DB 백그라운드 재연결 설정  
            signal_db_bg_reconnect_ = {
                signal_db_config.background_reconnect.enabled,
                signal_db_config.background_reconnect.initial_delay_ms,
                signal_db_config.background_reconnect.max_delay_ms,
                signal_db_config.background_reconnect.backoff_multiplier,
                signal_db_config.background_reconnect.check_interval_sec,
                signal_db_config.background_reconnect.jitter_factor
            };
        }
        
        // 로그 출력
        logger->info("VoltDB 설정 로드 완료:");
        logger->info("  * CAM DB: {}:{}", cam_db_host_, cam_db_port_);
        logger->info("    * 재시도: {}회 시도, {}ms 간격", 
                     cam_db_retry_.max_attempts, 
                     cam_db_retry_.delay_ms);
        logger->info("    * 백그라운드 재연결: {} (백오프: {}x, 지터: ±{}%)", 
                     cam_db_bg_reconnect_.enabled ? "활성화" : "비활성화",
                     cam_db_bg_reconnect_.backoff_multiplier,
                     static_cast<int>(cam_db_bg_reconnect_.jitter_factor * 100));
        
        // Signal DB 로그 출력 수정
        if (signal_db_enabled_) {
            logger->info("  * Signal DB: 활성화 - {}:{}", signal_db_host_, signal_db_port_);
            logger->info("    * 재시도: {}회 시도, {}ms 간격", 
                         signal_db_retry_.max_attempts, 
                         signal_db_retry_.delay_ms);
            logger->info("    * 백그라운드 재연결: {} (백오프: {}x, 지터: ±{}%)", 
                         signal_db_bg_reconnect_.enabled ? "활성화" : "비활성화",
                         signal_db_bg_reconnect_.backoff_multiplier,
                         static_cast<int>(signal_db_bg_reconnect_.jitter_factor * 100));
        } else {
            logger->info("  * Signal DB: 비활성화");
        }
        
        return true;
        
    } catch (const std::exception& e) {
        logger->error("초기화 중 오류 발생: {}", e.what());
        return false;
    }
}

bool VoltDBSource::connect() {
    logger->info("VoltDB 연결 시작");
    
    // CAM DB 연결 시도
    if (!connectWithRetry("cam_db")) {
        logger->warn("CAM DB 초기 연결 실패 - 백그라운드 재연결 시작");
        
        // 백그라운드 재연결 스레드 시작
        if (cam_db_bg_reconnect_.enabled) {
            cam_db_down_since_ = std::chrono::steady_clock::now();
            cam_db_recovery_thread_ = std::thread(&VoltDBSource::camDBRecoveryThreadFunc, this);
        }
    } else {
        cam_db_connected_ = true;
        logger->info("CAM DB 연결 성공");
    }
    
    // Signal DB 연결 시도 (활성화된 경우)
    if (signal_db_enabled_) {
        if (!connectWithRetry("signal_db")) {
            logger->warn("Signal DB 초기 연결 실패 - 백그라운드 재연결 시작");
            
            // 백그라운드 재연결 스레드 시작
            if (signal_db_bg_reconnect_.enabled) {
                signal_db_reconnect_thread_ = std::thread(&VoltDBSource::signalDBReconnectThreadFunc, this);
            }
        } else {
            signal_db_connected_ = true;
            logger->info("Signal DB 연결 성공");
        }
    }
    
    connected_ = true;  // 부분 연결도 허용
    return connected_.load();
}

void VoltDBSource::disconnect() {
    logger->info("VoltDB 연결 해제");
    
    running_ = false;
    
    // 재연결 스레드 종료 대기
    if (cam_db_recovery_thread_.joinable()) {
        cam_db_recovery_thread_.join();
    }
    
    if (signal_db_reconnect_thread_.joinable()) {
        signal_db_reconnect_thread_.join();
    }
    
    connected_ = false;
    cam_db_connected_ = false;
    signal_db_connected_ = false;
}

bool VoltDBSource::isConnected() const {
    return connected_.load();
}

bool VoltDBSource::isCamIdAvailable() const {
    return cam_id_available_.load();
}

SiteInfo::Mode VoltDBSource::getMode() const {
    return SiteInfo::Mode::VOLTDB;
}

SiteInfo VoltDBSource::getSiteInfo() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    
    // CAM ID가 없으면 조회 시도
    if (!cam_id_available_.load()) {
        if (cam_db_connected_.load()) {
            try {
                site_info_.spot_camr_id = getCamIDFromDB(site_info_.ip_address);
                site_info_.mode = SiteInfo::Mode::VOLTDB;
                site_info_.parseVoltDBFormat();
                cam_id_available_ = true;
                
                logger->info("CAM ID 조회 성공: {}", site_info_.spot_camr_id);
                logger->info("  * 교차로 ID: {}", site_info_.spot_ints_id);
                logger->info("  * 타겟 신호: {}", site_info_.target_signal);
                logger->info("  * 신호역산 지원: {}", site_info_.supports_signal_calc ? "지원" : "미지원");
                
                // 초기 조회 성공 시 콜백 호출
                notifyRecovery();
                
            } catch (const std::exception& e) {
                logger->info("CAM ID 조회 실패: {} (나중에 재시도)", e.what());
                // CAM DB 연결 실패 시 임시 CAM ID 설정
                site_info_.spot_camr_id = SiteInfo::PENDING_CAM_ID;
                site_info_.mode = SiteInfo::Mode::VOLTDB;
                site_info_.parseVoltDBFormat();
                cam_id_available_ = false;
            }
        }
    }
    
    return site_info_;
}

void VoltDBSource::setIPAddress(const std::string& ip) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    site_info_.ip_address = ip;
    cam_id_available_ = false;  // IP 변경 시 재조회 필요
}

bool VoltDBSource::supportsSignalData() const {
    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    return signal_db_enabled_ && signal_db_connected_.load();
}

std::string VoltDBSource::getCamIDFromDB(const std::string& ip_address) {
    if (!cam_db_connected_.load()) {
        throw std::runtime_error("CAM DB가 연결되지 않음");
    }
    
    try {
        std::string query = "SELECT spot_camr_id FROM SOITGCAMRINFO WHERE edge_sys_2k_ip = '" 
                          + ip_address + "'";
        std::string result = executeQueryWithRetry(cam_db_host_, cam_db_port_, query, cam_db_retry_);
        
        Json::Reader reader;
        Json::Value res;
        if (reader.parse(result, res)) {
            if (res["status"].asInt() == 1 && 
                res["results"][0]["data"].size() > 0 &&
                res["results"][0]["data"][0].size() > 0) {
                return res["results"][0]["data"][0][0].asString();
            }
        }
        
        throw std::runtime_error("해당 IP에 대한 CAM ID를 찾을 수 없음");
        
    } catch (const std::exception& e) {
        throw;
    }
}

std::vector<int> VoltDBSource::getPhaseInfo(const std::string& spot_ints_id, int& LC_CNT) {
    std::vector<int> result;
    
    if (!signal_db_connected_.load()) {
        logger->warn("Signal DB가 연결되지 않음");
        return result;
    }
    
    try {
        // Phase Duration 정보 조회 쿼리
        std::string query = "SELECT LC_CNT";
        
        // A_RING과 B_RING의 phase duration 컬럼들 추가
        for (char ring : {'A', 'B'}) { 
            for (int i = 1; i <= 8; ++i) {
                query += ", " + std::string(1, ring) + "_RING_" + std::to_string(i) + "_PHAS_HR";
            }
        }
        query += " FROM SOITDSPOTINTSSTTS WHERE SPOT_INTS_ID = " + spot_ints_id;
        
        logger->debug("Phase 정보 쿼리: {}", query);
        std::string response = executeQueryWithRetry(signal_db_host_, signal_db_port_, query, signal_db_retry_);
        
        Json::Reader reader;
        Json::Value res;
        if (reader.parse(response, res)) {
            if (res["status"].asInt() == 1 && res["results"][0]["data"].size() > 0) {
                const Json::Value& row = res["results"][0]["data"][0];
                
                if (row.size() < 17) {
                    logger->error("Phase 정보 데이터 크기 부족: {}", row.size());
                    return result;
                }
                
                // LC_CNT는 첫 번째 컬럼
                LC_CNT = row[0].asInt();
                logger->debug("LC_CNT: {}", LC_CNT);
                
                // 나머지 16개 phase duration 데이터
                for (int i = 1; i <= 16; i++) {
                    result.push_back(row[i].asInt());
                }
                
                logger->info("Phase 정보 조회 성공 - LC_CNT: {}, 데이터 수: {}", LC_CNT, result.size());
            }
        }
    } catch (const std::exception& e) {
        logger->error("Phase 정보 조회 실패: {}", e.what());
    }
    
    return result;
}

std::vector<int> VoltDBSource::getMovementInfo(const std::string& spot_ints_id) {
    std::vector<int> result;
    
    if (!signal_db_connected_.load()) {
        logger->warn("Signal DB가 연결되지 않음");
        return result;
    }
    
    try {
        // Movement 정보 조회 쿼리
        std::string query = "SELECT ";
        
        bool first = true;
        // A_RING과 B_RING의 movement 컬럼들 추가
        for (char ring : {'A', 'B'}) { 
            for (int i = 1; i <= 8; ++i) {
                if (!first) query += ",";
                query += " " + std::string(1, ring) + "_RING_" + std::to_string(i) + "_PHAS_MVMT_NO";
                first = false;
            }
        }
        query += " FROM SOITDINTSPHASINFO WHERE SPOT_INTS_ID = " + spot_ints_id +
                 " AND OPER_SE_CD = '0' ORDER BY CLCT_DT DESC LIMIT 1";
        
        logger->debug("Movement 정보 쿼리: {}", query);
        std::string response = executeQueryWithRetry(signal_db_host_, signal_db_port_, query, signal_db_retry_);
        
        Json::Reader reader;
        Json::Value res;
        if (reader.parse(response, res)) {
            if (res["status"].asInt() == 1 && res["results"][0]["data"].size() > 0) {
                const Json::Value& row = res["results"][0]["data"][0];
                
                if (row.size() < 16) {
                    logger->error("Movement 정보 데이터 크기 부족: {}", row.size());
                    return result;
                }
                
                // 16개의 movement 데이터 추출
                for (int i = 0; i < 16; i++) {
                    result.push_back(row[i].asInt());
                }
                
                logger->info("Movement 정보 조회 성공 - 데이터 수: {}", result.size());
            }
        }
    } catch (const std::exception& e) {
        logger->error("Movement 정보 조회 실패: {}", e.what());
    }
    
    return result;
}

void VoltDBSource::setRecoveryCallback(CamDBRecoveryCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    recovery_callback_ = callback;
}

int VoltDBSource::getDowntimeMinutes() const {
    if (cam_db_connected_.load()) {
        return 0;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto downtime = std::chrono::duration_cast<std::chrono::minutes>(now - cam_db_down_since_);
    return static_cast<int>(downtime.count());
}

// Private 메서드들
std::string VoltDBSource::executeQuery(const std::string& host, int port, const std::string& query) {
    // rest.cpp의 executeQueryTimeOut 함수 사용
    return executeQueryTimeOut(host, port, query, 5L);
}

std::string VoltDBSource::executeQueryWithRetry(const std::string& host, int port, 
                                                const std::string& query, const RetryConfig& retry) {
    for (int attempt = 1; attempt <= retry.max_attempts; attempt++) {
        try {
            return executeQuery(host, port, query);
        } catch (const std::exception& e) {
            if (attempt == retry.max_attempts) {
                throw;
            }
            logger->warn("쿼리 실행 실패 (시도 {}/{}): {}", 
                        attempt, retry.max_attempts, e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(retry.delay_ms));
        }
    }
    throw std::runtime_error("모든 재시도 실패");
}

bool VoltDBSource::connectWithRetry(const std::string& db_type) {
    if (db_type == "cam_db") {
        for (int attempt = 1; attempt <= cam_db_retry_.max_attempts; attempt++) {
            logger->info("CAM DB 연결 시도 {}/{}", attempt, cam_db_retry_.max_attempts);
            
            if (connectToCamDB()) {
                return true;
            }
            
            if (attempt < cam_db_retry_.max_attempts) {
                std::this_thread::sleep_for(std::chrono::milliseconds(cam_db_retry_.delay_ms));
            }
        }
    } else if (db_type == "signal_db") {
        for (int attempt = 1; attempt <= signal_db_retry_.max_attempts; attempt++) {
            logger->info("Signal DB 연결 시도 {}/{}", attempt, signal_db_retry_.max_attempts);
            
            if (connectToSignalDB()) {
                return true;
            }
            
            if (attempt < signal_db_retry_.max_attempts) {
                std::this_thread::sleep_for(std::chrono::milliseconds(signal_db_retry_.delay_ms));
            }
        }
    }
    return false;
}

bool VoltDBSource::connectToCamDB() {
    logger->info("CAM DB 연결 시도 중...");
    try {
        // 테스트 쿼리로 연결 확인 (대문자 테이블명 사용)
        std::string test_query = "SELECT COUNT(*) FROM SOITGCAMRINFO WHERE 1=0";
        executeQueryWithRetry(cam_db_host_, cam_db_port_, test_query, cam_db_retry_);
        return true;
    } catch (const std::exception& e) {
        logger->info("CAM DB 연결 실패: {}", e.what());
        return false;
    }
}

bool VoltDBSource::connectToSignalDB() {
    logger->info("신호 DB 연결 시도 중...");
    try {
        // 테스트 쿼리로 연결 확인 (대문자 테이블명 사용)
        std::string test_query = "SELECT COUNT(*) FROM SOITDINTSPHASINFO WHERE 1=0";
        executeQueryWithRetry(signal_db_host_, signal_db_port_, test_query, signal_db_retry_);
        return true;
    } catch (const std::exception& e) {
        logger->info("Signal DB 연결 실패: {}", e.what());
        return false;
    }
}

void VoltDBSource::camDBRecoveryThreadFunc() {
    logger->info("CAM DB 백그라운드 재연결 스레드 시작");
    
    auto& config = cam_db_bg_reconnect_;
    int current_delay_ms = config.initial_delay_ms;
    std::random_device rd;
    std::mt19937 gen(rd());
    
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(config.check_interval_sec));
        
        if (!cam_db_connected_.load()) {
            // 지터 추가 (±jitter_factor)
            std::uniform_real_distribution<> dis(1.0 - config.jitter_factor, 
                                               1.0 + config.jitter_factor);
            int jittered_delay = static_cast<int>(current_delay_ms * dis(gen));
            
            logger->info("CAM DB 재연결 시도 ({}ms 후)", jittered_delay);
            std::this_thread::sleep_for(std::chrono::milliseconds(jittered_delay));
            
            if (connectToCamDB()) {
                cam_db_connected_ = true;
                logger->info("CAM DB 재연결 성공!");
                
                // CAM ID 재조회 및 콜백 호출
                try {
                    std::lock_guard<std::mutex> lock(data_mutex_);
                    if (!site_info_.ip_address.empty()) {
                        site_info_.spot_camr_id = getCamIDFromDB(site_info_.ip_address);
                        site_info_.parseVoltDBFormat();
                        cam_id_available_ = true;
                        
                        logger->info("CAM ID 재조회 성공: {}", site_info_.spot_camr_id);
                        
                        // 복구 콜백 호출
                        notifyRecovery();
                    }
                } catch (const std::exception& e) {
                    logger->error("CAM ID 재조회 실패: {}", e.what());
                }
                
                // 지연 시간 초기화
                current_delay_ms = config.initial_delay_ms;
            } else {
                // 백오프 적용
                current_delay_ms = static_cast<int>(current_delay_ms * config.backoff_multiplier);
                if (current_delay_ms > config.max_delay_ms) {
                    current_delay_ms = config.max_delay_ms;
                }
                logger->info("다음 재연결 시도는 약 {}초 후", current_delay_ms / 1000);
            }
        }
    }
    
    logger->info("CAM DB 백그라운드 재연결 스레드 종료");
}

void VoltDBSource::signalDBReconnectThreadFunc() {
    logger->info("Signal DB 백그라운드 재연결 스레드 시작");
    
    auto& config = signal_db_bg_reconnect_;
    int current_delay_ms = config.initial_delay_ms;
    std::random_device rd;
    std::mt19937 gen(rd());
    
    bool first_success = false;
    
    while (running_.load() && !first_success) {
        std::this_thread::sleep_for(std::chrono::seconds(config.check_interval_sec));
        
        if (!signal_db_connected_.load()) {
            // 지터 추가
            std::uniform_real_distribution<> dis(1.0 - config.jitter_factor, 
                                               1.0 + config.jitter_factor);
            int jittered_delay = static_cast<int>(current_delay_ms * dis(gen));
            
            logger->info("Signal DB 재연결 시도 ({}ms 후)", jittered_delay);
            std::this_thread::sleep_for(std::chrono::milliseconds(jittered_delay));
            
            if (connectToSignalDB()) {
                signal_db_connected_ = true;
                first_success = true;
                logger->info("Signal DB 재연결 성공! (최초 연결 성공 - 재연결 스레드 종료)");
            } else {
                // 백오프 적용
                current_delay_ms = static_cast<int>(current_delay_ms * config.backoff_multiplier);
                if (current_delay_ms > config.max_delay_ms) {
                    current_delay_ms = config.max_delay_ms;
                }
                logger->info("다음 재연결 시도는 약 {}초 후", current_delay_ms / 1000);
            }
        }
    }
    
    logger->info("Signal DB 백그라운드 재연결 스레드 종료");
}

void VoltDBSource::notifyRecovery() {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (recovery_callback_) {
        recovery_callback_(site_info_);
    }
}