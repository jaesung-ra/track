#ifndef SITE_INFO_MANAGER_H
#define SITE_INFO_MANAGER_H

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "../core/data_provider.h"
#include "../core/site_info.h"
#include "../../common/common_types.h"

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

/**
 * @brief 사이트 정보 관리 클래스
 * 
 * CAM ID 관리, 신호 DB 조회를 위한 정보 관리 담당
 * VoltDBSource의 재연결은 VoltDBSource 자체에서 처리
 * 이 클래스는 연결 상태와 사이트 정보만 관리
 */
class SiteInfoManager {
private:
    SiteInfo site_info;
    mutable std::mutex info_mutex;
    
    // DataProvider 인스턴스
    std::unique_ptr<DataProvider> data_provider;
    
    // CAM DB 연결 상태
    std::atomic<bool> cam_db_connected{false};
    
    // 기본 CAM ID (DB 미연결시 사용)
    std::string default_cam_id = DEFAULT_CAM_ID;
    
    // 로거
    std::shared_ptr<spdlog::logger> logger;
    
    // 내부 메서드
    bool initializeDataProvider(const std::string& config_path);

public:
    /**
     * @brief 생성자
     */
    SiteInfoManager();
    
    /**
     * @brief 소멸자
     */
    ~SiteInfoManager();
    
    /**
     * @brief 초기화
     * @param config_path 설정 파일 경로
     * @return 성공 시 true
     */
    bool initialize(const std::string& config_path = "config/config.json");
    
    /**
     * @brief 사이트 정보 설정
     * @param info 사이트 정보
     */
    void setSiteInfo(const SiteInfo& info);
    
    /**
     * @brief 사이트 정보 가져오기
     * @return 현재 사이트 정보
     */
    SiteInfo getSiteInfo() const;
    
    /**
     * @brief 카메라 ID 가져오기 (로그 출력용)
     * @return 카메라 ID
     * @note 실제 메타데이터에는 사용되지 않음
     */
    std::string getCameraId() const;
    
    /**
     * @brief IP 주소 가져오기
     * @return IP 주소
     */
    std::string getIpAddress() const;
    
    /**
     * @brief CAM DB 연결 상태 설정
     * @param connected 연결 상태
     */
    void setCamDbConnected(bool connected);
    
    /**
     * @brief CAM DB 연결 상태 확인
     * @return 연결되어 있으면 true
     */
    bool isCamDbConnected() const;
    
    /**
     * @brief 기본 카메라 ID 반환 (DB 미연결시 사용)
     * @return 기본 카메라 ID
     */
    std::string getDefaultCameraId() const { return default_cam_id; }
    
    /**
     * @brief 교차로 ID 가져오기
     * @return 교차로 ID
     */
    std::string getCrossroadId() const;
    
    /**
     * @brief 신호 DB 사용 여부
     * @return 사용하면 true
     */
    bool isSignalDbEnabled() const;  
    
    /**
     * @brief DataProvider 가져오기
     * @return DataProvider 포인터
     */
    DataProvider* getDataProvider() const { return data_provider.get(); }
    
    /**
     * @brief VoltDBSource로부터 재연결 성공 통지 받기
     * @param new_site_info 새로운 사이트 정보
     */
    void onCamDbReconnected(const SiteInfo& new_site_info);
    
    /**
     * @brief 정지 및 정리
     */
    void shutdown();
};

#endif // SITE_INFO_MANAGER_H