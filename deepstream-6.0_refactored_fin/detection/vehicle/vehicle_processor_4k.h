#ifndef VEHICLE_PROCESSOR_4K_H
#define VEHICLE_PROCESSOR_4K_H

#include <deque>
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
class ImageCropper;
class ImageStorage;
class ConfigManager;

/**
 * @brief 차량 감지 처리 클래스 (4K 모드)
 * 
 * 4K 카메라용 차량 처리
 * - 정지선 통과 감지
 * - 차로 판단
 * - 속도 계산 및 저장
 * - 다수 이미지 저장 (오토바이: 1장, 기타 차종: 다수)
 * - Redis 전송 (SQLite 제외)
 * 
 * === 이미지 저장 정책 ===
 * - 오토바이: 정지선 통과시 1장만
 * - 기타 차종:
 *   * 정지선 통과 전: 속도 5km/h 이상이면서 calibration ROI 내부일 때 초당 1장, 최대 10장
 *   * 정지선 통과 시: 1장
 *   * 정지선 통과 후 1초 경과: 1장
 * - 파일명: ID_imageCount_정지선통과시각.jpg
 */
class VehicleProcessor4K {
private:
    // 의존성
    ROIHandler& roi_handler;
    RedisClient& redis_client;
    ImageCropper& image_cropper;
    ImageStorage& image_storage;
    
    // 로거
    std::shared_ptr<spdlog::logger> logger;
    
    // 이미지 캡처 추적용 구조체
    struct ImageCaptureState {
        int image_count = 0;                    // 저장된 이미지 수
        int last_capture_time = 0;              // 마지막 캡처 시간 (초)
        int stop_pass_time = 0;                 // 정지선 통과 시각 (초)
        bool stop_line_image_saved = false;     // 정지선 이미지 저장 여부
        bool after_stop_image_saved = false;    // 정지선 후 1초 이미지 저장 여부
        std::deque<std::string> saved_images;   // 저장된 이미지 파일명 목록
        std::string image_path;                 // 이미지 저장 경로 (파일명 제외)
    };
    
    // 차량별 이미지 캡처 상태 관리
    std::map<int, ImageCaptureState> capture_states_;
    
    // FPS 정보 (ConfigManager에서 가져옴)
    int camera_fps_ = 30;
    
    // ========== 내부 이미지 저장 클래스 ==========
    class ImageSaver {
    private:
        ImageCropper& cropper_;
        ImageStorage& storage_;
        std::shared_ptr<spdlog::logger> logger_;
        
    public:
        ImageSaver(ImageCropper& cropper, ImageStorage& storage);
        
        /**
         * @brief 차량 이미지 저장
         * @param surface NvBufSurface 포인터
         * @param bbox 바운딩 박스
         * @param object_id 객체 ID
         * @param image_count 이미지 번호
         * @param timestamp 타임스탬프
         * @param save_path 저장 경로
         * @return 성공 시 파일명, 실패 시 빈 문자열
         */
        std::string saveVehicleImage(NvBufSurface* surface, const box& bbox,
                                   int object_id, int image_count, 
                                   int timestamp, const std::string& save_path);
        
        /**
         * @brief 이미지 파일명 생성
         * @param object_id 객체 ID
         * @param image_count 이미지 번호
         * @param timestamp 타임스탬프
         * @return 파일명
         */
        std::string generateFilename(int object_id, int image_count, int timestamp);
    };
    
    // ImageSaver 인스턴스
    std::unique_ptr<ImageSaver> image_saver_;
    
    // ========== 내부 메서드 ==========
    void updateSpeed(obj_data& obj, const ObjPoint& current_pos, int current_time);
    void checkStopLine(obj_data& obj, const ObjPoint& current_pos, 
                      int current_time, const box& obj_box, NvBufSurface* surface);
    void processImageCapture(obj_data& obj, const ObjPoint& current_pos,
                            int current_time, const box& obj_box, NvBufSurface* surface);
    void sendVehicleData(const obj_data& obj, int current_time, const std::string& image_path);
    std::string generateMetadata(const obj_data& obj, const std::string& image_path);
    void cleanupOldStates(int current_time);

public:
    /**
     * @brief 생성자
     * @param roi ROI 핸들러 참조
     * @param redis Redis 클라이언트 참조
     * @param cropper 이미지 cropper 참조
     * @param storage 이미지 storage 참조
     */
    VehicleProcessor4K(ROIHandler& roi, RedisClient& redis,
                      ImageCropper& cropper, ImageStorage& storage);
    
    /**
     * @brief 소멸자
     */
    ~VehicleProcessor4K() = default;
    
    /**
     * @brief 차량 처리 메인 함수 - obj_data를 반환
     * @param input_obj 입력 차량 데이터 (const 참조)
     * @param obj_box 바운딩 박스
     * @param current_pos 현재 프레임의 bottom_center 위치 (process_meta에서 계산)
     * @param current_time 현재 시간 (초 단위 Unix timestamp)
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

#endif // VEHICLE_PROCESSOR_4K_H