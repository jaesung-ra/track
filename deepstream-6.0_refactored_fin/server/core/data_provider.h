#ifndef DATA_PROVIDER_H
#define DATA_PROVIDER_H

#include <memory>
#include <string>
#include <vector>
#include "site_info.h"

/**
 * @brief 데이터 제공자 인터페이스
 */
class DataProvider {
public:
    virtual ~DataProvider() = default;

    // 초기화 및 연결
    virtual bool initialize(const std::string& config_path) = 0;
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    // 사이트 정보 조회
    virtual SiteInfo getSiteInfo() = 0;
    virtual void setIPAddress(const std::string& ip) = 0;

    // CAM ID 상태
    virtual bool isCamIdAvailable() const = 0;

    // 신호 정보 조회 (VoltDB 모드만 지원)
    virtual bool supportsSignalData() const = 0;
    virtual std::vector<int> getPhaseInfo(const std::string& spot_ints_id, int& LC_CNT) {
        return {};
    }
    virtual std::vector<int> getMovementInfo(const std::string& spot_ints_id) {
        return {};
    }

    // 모드 정보
    virtual SiteInfo::Mode getMode() const = 0;
};

#endif