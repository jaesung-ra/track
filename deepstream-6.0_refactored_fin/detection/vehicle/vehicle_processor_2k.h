#ifndef VEHICLE_PROCESSOR_2K_H
#define VEHICLE_PROCESSOR_2K_H

#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../../common/common_types.h"
#include "../../common/object_data.h"
#include "nvbufsurface.h"

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

// Forward declarations
class ROIHandler;
class RedisClient;
class SQLiteHandler;
class SiteInfoManager;
class ImageCropper;
class ImageStorage;
class SpecialSiteAdapter;

/**
 * @brief 차량 감지 처리 클래스 (2K 모드)
 * 
 * 차량 객체의 추적, 속도 계산, ROI 진입/진출 감지,
 * 메타데이터 생성 및 전송 담당
 * 
 * - process_meta에서 current_pos를 파라미터로 전달
 * - obj.last_pos는 이전 프레임 위치 (process_meta에서 관리)
 * - 정지선 체크: obj.last_pos(이전)와 current_pos(현재) 비교
 * - Special Site 모드 지원 (신호 기반 방향 결정)
 * 
 * === 데이터 관리 정책 ===
 * - det_obj 직접 수정하지 않음
 * - 수정된 obj_data 복사본 반환
 * - 스레드 안전성 보장
 */
class VehicleProcessor2K {
private:
    ROIHandler& roi_handler;
    RedisClient& redis_client;
    SQLiteHandler& sqlite_handler;
    ImageCropper& image_cropper;
    ImageStorage& image_storage;
    SiteInfoManager& site_manager;
    
    // Special Site 어댑터 (nullptr 가능)
    SpecialSiteAdapter* special_site_adapter;
    
    // 로거
    std::shared_ptr<spdlog::logger> logger;
    
    // ========== 내부 메서드 ==========
    void updateSpeed(obj_data& obj, const ObjPoint& current_pos, int current_time);
    void checkROITransition(obj_data& obj, const ObjPoint& current_pos, 
                           int current_time, const box& obj_box, NvBufSurface* surface);
    void sendVehicleData(const obj_data& obj, int current_time);
    void saveVehicleImage(obj_data& obj, const box& obj_box, 
                         NvBufSurface* surface, int current_time);
    std::string generateMetadata(const obj_data& obj);

public:
    /**
     * @brief 생성자
     * @param special_adapter Special Site 어댑터 (nullptr 가능)
     */
    VehicleProcessor2K(ROIHandler& roi, RedisClient& redis, SQLiteHandler& sqlite,
                      ImageCropper& cropper, ImageStorage& storage, SiteInfoManager& site,
                      SpecialSiteAdapter* special_adapter = nullptr);
    
    /**
     * @brief 소멸자
     */
    ~VehicleProcessor2K() = default;
    
    /**
     * @brief 차량 처리 메인 함수 - obj_data를 반환
     * @param input_obj 입력 차량 데이터 (const 참조)
     * @param obj_box 바운딩 박스
     * @param current_pos 현재 프레임의 bottom_center 위치 (process_meta에서 계산)
     * @param current_time 현재 시간
     * @param second_changed 초 변경 여부
     * @param surface 이미지 서페이스
     * @return 수정된 obj_data (복사본)
     * 
     * @note input_obj.last_pos는 이전 프레임 위치
     *       current_pos는 현재 프레임 위치
     */
    obj_data processVehicle(const obj_data& input_obj, const box& obj_box,
                           const ObjPoint& current_pos, int current_time, 
                           bool second_changed, NvBufSurface* surface);
};

#endif // VEHICLE_PROCESSOR_2K_H