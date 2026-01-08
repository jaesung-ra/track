#ifndef INCIDENT_DETECTOR_H
#define INCIDENT_DETECTOR_H

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <queue>
#include <vector>
#include "incident_types.h"
#include "../../common/object_data.h"
#include "../../common/common_types.h"
#include "../../server/core/signal_types.h"
#include "../../json/json.h"
#include "nvbufsurface.h"
#include "opencv2/opencv.hpp"

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

// Forward declarations
class ROIHandler;
class RedisClient;
class ConfigManager;
class ImageCropper;
class ImageStorage;

/**
 * @brief 돌발상황 감지 클래스
 * 
 * 차량정지-꼬리물기-사고 연쇄 이벤트, 역주행, 무단횡단
 * 다양한 돌발상황을 감지하고 JSON 메타데이터 생성
 * 돌발이벤트 감지 시 즉시 bbox가 그려진 이미지 저장
 */
class IncidentDetector {
private:
    // 돌발 이벤트 추적 구조체
    struct ActiveIncident {
        IncidentType type;
        int object_id;
        int start_time;                 // 발생 시각
        int end_time;                   // 종료 시각
        std::string image_file;         // 이미지 파일명
        bool end_sent;                  // 종료 메시지 전송 여부
        
        // 연쇄 이벤트용 추가 정보
        int stop_start_phase;           // 차량정지 시작시 신호 phase
        int tail_gate_start_cycle;      // 꼬리물기 시작시 주기
    };
    
    // 차량별 추적 상태
    struct VehicleTrackingState {
        ObjPoint last_position;
        double last_speed;
        int stop_start_time;            // 정지 시작 시간
        int stop_duration;              // 정지 지속 시간
        int last_update_time;
        int lane_id;
        int direction;
        bool in_intersection;           // 교차로 내부 여부
        
        // 역주행 감지용 추가 필드
        bool near_stop_line;            // 정지선 근처 여부
        int reverse_start_time;         // 역방향 이동 시작 시간
        int reverse_duration;           // 역방향 이동 지속 시간
        double initial_y;               // 역방향 이동 시작시 Y좌표
        bool reverse_detected;          // 역주행 감지 여부
        
        // 연쇄 이벤트 추적
        bool is_stopped;                // 차량정지 상태
        bool is_tail_gating;            // 꼬리물기 상태
        bool is_accident;               // 사고 상태
        
        int stop_event_id;              // 차량정지 이벤트 ID
        int tail_gate_event_id;         // 꼬리물기 이벤트 ID
        int accident_event_id;          // 사고 이벤트 ID
    };
    
    // 보행자별 추적 상태
    struct PedestrianTrackingState {
        ObjPoint last_position;
        int last_update_time;
        int jaywalk_event_id;           // 무단횡단 이벤트 ID
    };

    // 의존성
    ROIHandler* roi_handler_;
    RedisClient* redis_client_;
    ImageCropper* image_cropper_;
    ImageStorage* image_storage_;
    
    // 추적 상태
    std::map<int, VehicleTrackingState> vehicle_states_;
    std::map<int, PedestrianTrackingState> pedestrian_states_;
    
    // 활성 돌발 이벤트 (이벤트ID -> 이벤트 정보)
    std::map<int, ActiveIncident> active_incidents_;
    int next_event_id_;  // 다음 이벤트 ID
    
    // 신호 정보
    int current_phase_;                             // 현재 신호 phase (0: 적색, 1: 녹색)
    int current_cycle_;                             // 현재 신호 주기
    bool has_signal_info_;                          // 신호 정보 사용 가능 여부
    
    // 설정
    std::string incident_image_path_;               // 돌발상황 이미지 저장 경로
    
    // 활성화 플래그
    bool enabled_;
    bool abnormal_stop_sequence_enabled_;           // 차량정지-꼬리물기-사고 연쇄
    bool reverse_driving_enabled_;                  // 역주행
    bool pedestrian_jaywalk_enabled_;               // 무단횡단
    
    // 로거
    std::shared_ptr<spdlog::logger> logger;
    
    // 뮤텍스 (thread-safe를 위해)
    mutable std::mutex incident_mutex_;
    
    // 내부 메서드 - 연쇄 이벤트 (NvBufSurface와 box 파라미터 추가)
    void checkVehicleStop(int id, VehicleTrackingState& state, const box& bbox, 
                         NvBufSurface* surface, int current_time);
    void checkTailGating(int id, VehicleTrackingState& state, const box& bbox, 
                        NvBufSurface* surface, int current_time);
    void checkAccident(int id, VehicleTrackingState& state, const box& bbox, 
                      NvBufSurface* surface, int current_time);
    
    // 내부 메서드 - 개별 이벤트 (NvBufSurface와 box 파라미터 추가)
    void checkReverseDriving(int id, const VehicleTrackingState& state, const box& bbox, 
                            NvBufSurface* surface, int current_time);
    void checkPedestrianJaywalk(int id, PedestrianTrackingState& state, const ObjPoint& position, 
                                const box& bbox, NvBufSurface* surface, int current_time);
    
    // 이벤트 관리
    int createIncident(IncidentType type, int object_id, int start_time);
    void endIncident(int event_id, int end_time);
    void sendIncidentStart(const ActiveIncident& incident);
    void sendIncidentEnd(const ActiveIncident& incident);
    std::string createStartJson(const ActiveIncident& incident);
    std::string createEndJson(const ActiveIncident& incident);
    
    // 이미지 저장 관련 (box 파라미터 추가)
    void saveIncidentImage(NvBufSurface* surface, int object_id, const box& bbox,
                          int timestamp, IncidentType type);
    void drawBbox(cv::Mat& image, const box& bbox);
    std::string generateIncidentFilename(int object_id, int timestamp, IncidentType type);
    
    // 상태 관리
    void cleanupOldStates(int current_time);
    void checkIncidentTimeouts(int current_time);

public:
    IncidentDetector();
    ~IncidentDetector();
    
    /**
     * @brief 초기화
     * @param roi_handler ROI 핸들러
     * @param redis_client Redis 클라이언트
     * @param image_cropper 이미지 Cropper
     * @param image_storage 이미지 Storage
     * @return 성공 시 true
     */
    bool initialize(ROIHandler* roi_handler, RedisClient* redis_client,
                   ImageCropper* image_cropper, ImageStorage* image_storage);
    
    /**
     * @brief 차량 객체 처리
     * @param id 차량 ID
     * @param obj 객체 데이터
     * @param bbox 바운딩 박스
     * @param surface NvBufSurface (이미지 저장용)
     * @param current_time 현재 시간
     */
    void processVehicle(int id, const obj_data& obj, const box& bbox, 
                       NvBufSurface* surface, int current_time);
    
    /**
     * @brief 보행자 객체 처리
     * @param id 보행자 ID
     * @param obj 객체 데이터
     * @param bbox 바운딩 박스
     * @param surface NvBufSurface (이미지 저장용)
     * @param current_time 현재 시간
     */
    void processPedestrian(int id, const obj_data& obj, const box& bbox,
                          NvBufSurface* surface, int current_time);
    
    /**
     * @brief 신호 변경 이벤트 처리
     * @param event 신호 변경 이벤트
     */
    void onSignalChange(const SignalChangeEvent& event);
    
    /**
     * @brief 객체의 돌발상황 여부 확인
     * @param object_id 객체 ID
     * @return 돌발상황이 발생 중이면 true
     */
    bool hasIncident(int object_id) const;
    
    /**
     * @brief 활성화 상태 확인
     * @return 활성화 여부
     */
    bool isEnabled() const { return enabled_; }
    
    /**
     * @brief 정기적인 상태 업데이트 (매 초 호출)
     * @param current_time 현재 시간
     */
    void updatePerSecond(int current_time);
};

#endif // INCIDENT_DETECTOR_H