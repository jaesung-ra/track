#include "site_info_manager.h"
#include "../source/manual/manual_source.h"
#include "../source/voltdb/voltdb_source.h"
#include "../../utils/config_manager.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

// IP 주소 가져오기 함수
std::string get_ip_address() {
    struct ifreq ifr;
    struct sockaddr_in* ipaddr;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return "0.0.0.0";

    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFADDR, &ifr) < 0) {
        close(s);
        return "0.0.0.0";
    }

    ipaddr = (struct sockaddr_in*)&ifr.ifr_addr;
    static char ipstr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ipaddr->sin_addr, ipstr, INET_ADDRSTRLEN);
    close(s);
    return std::string(ipstr);
}

SiteInfoManager::SiteInfoManager() {
    // 로거는 initialize에서 초기화
}

SiteInfoManager::~SiteInfoManager() {
    shutdown();
}

bool SiteInfoManager::initialize(const std::string& config_path) {
    logger = getLogger("DS_SiteInfoManager_log");
    logger->info("SiteInfoManager 초기화 시작");
    
    try {
        // 기본 CAM ID 설정
        default_cam_id = DEFAULT_CAM_ID;
        
        // IP 주소 가져오기
        site_info.ip_address = get_ip_address();
        logger->info("시스템 IP 주소: {}", site_info.ip_address);
        
        // DataProvider 초기화
        if (!initializeDataProvider(config_path)) {
            logger->error("DataProvider 초기화 실패");
            return false;
        }
        
        logger->info("SiteInfoManager 초기화 완료");
        return true;
        
    } catch (const std::exception& e) {
        logger->error("초기화 중 오류 발생: {}", e.what());
        return false;
    }
}

bool SiteInfoManager::initializeDataProvider(const std::string& config_path) {
    auto& config = ConfigManager::getInstance();
    std::string mode = config.getOperationMode();
    
    logger->info("Operation mode: {}", mode);
    
    if (mode == "manual") {
        // Manual 모드
        data_provider = std::make_unique<ManualSource>();
        site_info.mode = SiteInfo::Mode::MANUAL;
    } else {
        // VoltDB 모드
        auto voltdb_source = std::make_unique<VoltDBSource>();
        
        // 재연결 성공 콜백 설정
        voltdb_source->setRecoveryCallback([this](const SiteInfo& new_info) {
            onCamDbReconnected(new_info);
        });
        
        data_provider = std::move(voltdb_source);
        site_info.mode = SiteInfo::Mode::VOLTDB;
    }
    
    // DataProvider 초기화
    if (!data_provider->initialize(config_path)) {
        logger->error("DataProvider 초기화 실패");
        return false;
    }
    
    // IP 주소 설정
    data_provider->setIPAddress(site_info.ip_address);
    
    // 연결 시도
    if (data_provider->connect()) {
        // 연결 성공 - 사이트 정보 가져오기
        site_info = data_provider->getSiteInfo();
        cam_db_connected = true;
        logger->info("DataProvider 생성 성공, CAM ID: {}", site_info.spot_camr_id);
    } else {
        // 연결 실패
        if (mode == "manual") {
            // Manual 모드에서는 실패 처리
            logger->error("Manual 모드에서 설정 읽기 실패");
            return false;
        } else {
            // VoltDB 모드에서는 기본값 설정
            // VoltDBSource가 자체적으로 백그라운드 재연결을 수행
            logger->warn("VoltDB 초기 연결 실패, 백그라운드 재연결은 VoltDBSource에서 처리");
            site_info.spot_camr_id = default_cam_id;
            site_info.is_valid = false;
            cam_db_connected = false;
        }
    }
    
    // 신호 지원 여부 설정 - signal_db가 활성화되어 있는지 확인
    auto signal_db_config = config.getDBConfig("signal_db");
    site_info.supports_signal_calc = signal_db_config.enabled;
    
    return true;
}

void SiteInfoManager::setSiteInfo(const SiteInfo& info) {
    std::lock_guard<std::mutex> lock(info_mutex);
    site_info = info;
    
    // Mode enum을 문자열로 변환하여 로그
    std::string mode_str = (info.mode == SiteInfo::Mode::MANUAL) ? "MANUAL" : 
                          (info.mode == SiteInfo::Mode::VOLTDB) ? "VOLTDB" : "UNKNOWN";
    
    logger->info("사이트 정보 업데이트: 모드={}, Edge IP={}, CAM ID={}, 교차로ID={}", 
                mode_str, info.ip_address, info.spot_camr_id, info.spot_ints_id);
}

SiteInfo SiteInfoManager::getSiteInfo() const {
    std::lock_guard<std::mutex> lock(info_mutex);
    return site_info;
}

std::string SiteInfoManager::getCameraId() const {
    std::lock_guard<std::mutex> lock(info_mutex);
    
    // 단순히 CAM ID 반환 (로그 출력용, 메타데이터에는 사용 안함)
    return site_info.spot_camr_id;
}

std::string SiteInfoManager::getIpAddress() const {
    std::lock_guard<std::mutex> lock(info_mutex);
    return site_info.ip_address;
}

void SiteInfoManager::setCamDbConnected(bool connected) {
    cam_db_connected = connected;
    logger->info("CAM DB 연결 상태 변경: {}", connected ? "연결됨" : "연결 해제됨");
}

bool SiteInfoManager::isCamDbConnected() const {
    return cam_db_connected.load();
}

std::string SiteInfoManager::getCrossroadId() const {
    std::lock_guard<std::mutex> lock(info_mutex);
    
    // VoltDB 모드일 때만 교차로 ID 반환
    if (site_info.mode == SiteInfo::Mode::VOLTDB && site_info.is_valid) {
        return site_info.spot_ints_id;
    }
    
    return "";
}

bool SiteInfoManager::isSignalDbEnabled() const {
    std::lock_guard<std::mutex> lock(info_mutex);
    auto& config = ConfigManager::getInstance();
    
    // 신호역산이 지원되고 설정에서 활성화된 경우
    auto signal_db_config = config.getDBConfig("signal_db");
    return site_info.supports_signal_calc && signal_db_config.enabled;
}

void SiteInfoManager::onCamDbReconnected(const SiteInfo& new_site_info) {
    logger->info("CAM DB 재연결 성공 - 새 CAM ID: {}, 교차로: {}, 타겟신호: {}", 
                 new_site_info.spot_camr_id, 
                 new_site_info.spot_ints_id,
                 new_site_info.target_signal);
    
    // 사이트 정보 업데이트
    setSiteInfo(new_site_info);
    cam_db_connected = true;
}

void SiteInfoManager::shutdown() {
    logger->info("SiteInfoManager 종료 중...");
    
    // DataProvider 연결 해제
    if (data_provider) {
        data_provider->disconnect();
    }
    
    logger->info("SiteInfoManager 종료 완료");
}