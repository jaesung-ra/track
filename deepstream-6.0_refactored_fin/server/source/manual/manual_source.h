#ifndef MANUAL_SOURCE_H
#define MANUAL_SOURCE_H

#include "../../core/data_provider.h"
#include <mutex>

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

/**
 * @brief 수동 설정 기반 데이터 소스
 * 
 * Manual 모드용 - 외부 DB 연결 없음
 * CAM ID 불필요, 신호역산 미지원
 */
class ManualSource : public DataProvider {
private:
    SiteInfo site_info_;
    mutable std::mutex data_mutex_;
    bool initialized_ = false;

    std::shared_ptr<spdlog::logger> logger = nullptr;

public:
    ManualSource();
    ~ManualSource() override = default;

    // DataProvider 구현
    bool initialize(const std::string& config_path) override;
    bool connect() override { return initialized_; }
    void disconnect() override { initialized_ = false; }
    bool isConnected() const override { return initialized_; }

    SiteInfo getSiteInfo() override;
    void setIPAddress(const std::string& ip) override;
    bool isCamIdAvailable() const override { return true; }     // Manual 모드에서는 cam_id 불필요
    bool supportsSignalData() const override { return false; }
    SiteInfo::Mode getMode() const override { return SiteInfo::Mode::MANUAL; }
};

#endif