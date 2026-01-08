/**
 * @file incident_types.h
 * @brief 돌발상황 감지 모듈 타입 정의
 * 
 * 돌발상황 감지에 필요한 열거형, 구조체, 상수 정의
 */

#ifndef INCIDENT_TYPES_H
#define INCIDENT_TYPES_H

#include <string>

// 돌발상황 타입
enum IncidentType {
    ILLEGAL_WAIT = 1,      // 차량정지
    TAILGATE = 2,          // 꼬리물기
    JAYWALK = 3,           // 무단진입 보행자
    REVERSE = 4,           // 역주행
    ACCIDENT = 5           // 사고
};

// 돌발상황 감지 임계값
namespace IncidentThresholds {
    // 차량정지 감지
    const double STOP_SPEED_THRESHOLD = 5.0;               // 정지 판단 속도 (5 m/s 미만)
    const int STOP_DURATION_THRESHOLD = 4;                 // 정지 판단 시간 (4초 이상)

    // 사고 감지
    const int ACCIDENT_DURATION_WITHOUT_SIGNAL = 300;      // 신호 정보 없을 때 사고 판단 시간 (5분)
    
    // 이벤트 타임아웃
    const int EVENT_END_TIMEOUT = 60;                      // 이벤트 종료 타임아웃 (60초)
    const int EVENT_CLEANUP_TIMEOUT = 30;                  // 상태 정리 타임아웃 (30초)
    
    // 역주행 감지 (정지선 근처 장시간 역방향 이동)
    const double REVERSE_NEAR_STOPLINE_DISTANCE = 100.0;   // 정지선 근처 판단 거리 (100픽셀)
    const double REVERSE_MIN_SPEED = 5.0;                  // 역주행 판단 최소 속도 (5 m/s)
    const double REVERSE_MIN_DISTANCE = 50.0;              // 역주행 판단 최소 이동 거리 (50픽셀)
    const int REVERSE_MIN_DURATION = 10;                   // 역주행 판단 최소 지속 시간 (10초)
    const double REVERSE_START_THRESHOLD = 10.0;           // 역방향 이동 시작 판단 거리 (10픽셀)
}

// 돌발 이벤트 JSON 키
namespace IncidentJsonKeys {
    // 공통 키 (발생/종료 메시지 모두 사용)
    const std::string TRACE_ID = "trce_id";                 // 추적 ID
    const std::string EVENT_TYPE = "evet_type_cd";          // 이벤트 타입 코드
    const std::string OCCUR_TIME = "ocrn_unix_tm";          // 발생 시각
    const std::string IMAGE_PATH = "img_path_nm";           // 이미지 경로
    const std::string IMAGE_FILE = "img_file_nm";           // 이미지 파일명
    
    // 루트 키
    const std::string START_KEY = "start";                  // 발생 메시지 루트 키
    const std::string END_KEY = "end";                      // 종료 메시지 루트 키
    
    // 종료 메시지 추가 키
    const std::string END_TIME = "end_unix_tm";             // 종료 시각
    const std::string PROCESS_TIME = "prcs_unix_tm";        // 처리 시간 (종료-발생)
}
#endif // INCIDENT_TYPES_H