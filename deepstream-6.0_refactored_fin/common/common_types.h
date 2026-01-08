/**
 * @file common_types.h
 * @brief 전역 상수, 열거형, 타입 정의
 * 
 * ITS 애플리케이션 전체에서 사용되는 시스템 전역 상수,
 * 열거형, 타입 매핑을 포함
 */

#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <ctime>
#include <map>
#include <string>
#include <vector>

// 콘솔 출력용 ANSI 컬러 코드
#define RED     "\x1b[31m"
#define GRN     "\x1b[32m"
#define YEL     "\x1b[33m"
#define BLU     "\x1b[34m"
#define MAG     "\x1b[35m"
#define CYN     "\x1b[36m"
#define WHT     "\x1b[37m"
#define RESET   "\x1b[0m"

// 시스템 기본값
const std::string DEFAULT_CONFIG_PATH = "config/config.json";
const std::string DEFAULT_CAM_ID = "0000_00_00";

// 차량 클래스 매핑 (DeepStream 라벨 -> 서버 DB 코드)
const std::map<std::string, std::string> VEHICLE_TYPE_MAP = {
    {"bus", "MBUS"},
    {"bus-45", "LBUS"},
    {"car", "PCAR"},
    {"motorbike", "MOTOR"},    
    {"truck", "MTRUCK"},
    {"truck-45T", "LTRUCK"}
};

// 통계 상수 (StatsGenerator용)
const std::vector<std::string> STATS_VEHICLE_TYPES = {"MBUS", "LBUS", "PCAR", "MOTOR", "MTRUCK", "LTRUCK"};
const std::vector<int> STATS_TURN_TYPES = {
    11, 21, 22, 31, 32, 41,           // 정방향
    -11, -21, -22, -31, -32, -41      // 역방향
};

// 서버 DB KNCR 필드 매핑 (kncr1_trvl ~ kncr6_trvl)
// 서버 DB soitgturntypestats 테이블의 고정된 순서
// 이 순서는 서버 DB 스키마에 따른 것으로 변경하면 안됨
const std::vector<std::string> KNCR_MAPPING = {
    "MBUS",    // kncr1_trvl - 중형버스
    "LBUS",    // kncr2_trvl - 대형버스  
    "PCAR",    // kncr3_trvl - 승용차
    "MOTOR",   // kncr4_trvl - 오토바이
    "MTRUCK",  // kncr5_trvl - 중형트럭
    "LTRUCK"   // kncr6_trvl - 대형트럭
};

// YOLO 모델용 객체 클래스 정의
enum ObjectClass {
    BUS = 0,        // bus → MBUS (중형버스)
    BUS_45 = 1,     // bus-45 → LBUS (대형버스)
    CAR = 2,        // car → PCAR (승용차)
    MOTORBIKE = 3,  // motorbike → MOTOR (오토바이)
    PERSON = 4,     // person → PERSON (보행자)
    TRUCK = 5,      // truck → MTRUCK (중형트럭)
    TRUCK_45T = 6   // truck-45T → LTRUCK (대형트럭)
};

// 방향 타입 (ROI 및 통계용)
enum DirectionType {
    DIR_STRAIGHT = 11,          // 직진
    DIR_LEFT_TURN = 21,         // 좌회전
    DIR_LEFT_TURN_2 = 22,       // 좌회전2
    DIR_RIGHT_TURN = 31,        // 우회전
    DIR_RIGHT_TURN_2 = 32,      // 우회전2
    DIR_U_TURN = 41,            // 유턴
    DIR_REVERSE_STRAIGHT = -11,  // 역방향 직진
    DIR_REVERSE_LEFT = -21,      // 역방향 좌회전
    DIR_REVERSE_LEFT_2 = -22,    // 역방향 좌회전2
    DIR_REVERSE_RIGHT = -31,     // 역방향 우회전
    DIR_REVERSE_RIGHT_2 = -32,   // 역방향 우회전2
    DIR_REVERSE_U_TURN = -41     // 역방향 유턴
};

// 시간 상수
const int STATS_INTERVAL_SEC = 300;      // 통계 생성 주기: 5분 (StatsGenerator)

// 이미지 설정
const int JPEG_QUALITY = 95;             // JPEG 압축 품질 (ImageStorage)
const int IMAGE_PADDING = 15;            // 차량 크롭 이미지 패딩 (ImageCropper)

// 4K 이미지 캡처 설정
const double MIN_SPEED_FOR_IMAGE_CAPTURE = 5.0;  // 최소 속도 (km/h)
const int MAX_IMAGES_BEFORE_STOPLINE = 10;       // 정지선 전 최대 이미지 수
const int FRAMES_PER_SECOND_FOR_CAPTURE = 30;    // 초당 캡처용 FPS (30FPS 기준)

// 헬퍼 함수
/**
 * @brief 차량 클래스인지 확인
 */
inline bool isVehicleClass(int class_id) {
    return (class_id >= 0 && class_id <= 6 && class_id != PERSON);
}

/**
 * @brief 보행자 클래스인지 확인
 */
inline bool isPedestrianClass(int class_id) {
    return (class_id == PERSON);
}

/**
 * @brief 오토바이 클래스인지 확인
 * @param label DeepStream 라벨 문자열
 * @return 오토바이면 true, 아니면 false
 */
inline bool isMotorbike(const std::string& label) {
    return (label == "motorbike" || label == "MOTOR");
}

/**
 * @brief DeepStream 라벨을 서버 DB 코드로 변환
 */
inline std::string getVehicleTypeCode(const std::string& label) {
    auto it = VEHICLE_TYPE_MAP.find(label);
    return (it != VEHICLE_TYPE_MAP.end()) ? it->second : "UNKNOWN";
}

/**
 * @brief 현재 Unix 타임스탬프 반환
 */
inline int getCurTime() {
    return static_cast<int>(std::time(nullptr));
}

#endif // COMMON_TYPES_H