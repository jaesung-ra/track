#include "manual_source.h"

ManualSource::ManualSource() {
    logger = getLogger("DS_ManualSource_log");
    logger->info("ManualSource 생성");
}

bool ManualSource::initialize(const std::string& config_path) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    
    // config_path는 인터페이스 준수를 위해 받지만 사용하지 않음
    logger->info("Manual 모드 초기화");
    
    // Manual 모드 설정 - 모든 필드 초기화
    site_info_.mode = SiteInfo::Mode::MANUAL;
    site_info_.spot_camr_id = "";               // Manual 모드에서는 cam_id 불필요
    site_info_.spot_ints_id = "";               // 교차로 ID도 불필요
    site_info_.target_signal = 0;               // 타겟 신호 없음
    
    // Manual 모드는 항상 유효
    site_info_.is_valid = true;
    site_info_.supports_signal_calc = false;    // Manual 모드는 신호역산 미지원
    
    logger->info("Manual 모드 초기화 완료");
    initialized_ = true;
    return true;
}

SiteInfo ManualSource::getSiteInfo() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return site_info_;
}

void ManualSource::setIPAddress(const std::string& ip) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    site_info_.ip_address = ip;
    logger->info("Manual 모드 IP 주소 설정: {}", ip);
}