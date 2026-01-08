/**
 * @file object_data.h
 * @brief 객체 추적 데이터 구조체
 * 
 * ITS 시스템에서 감지된 객체와 추적 정보를 저장하는 데이터 구조체 정의
 */

#ifndef OBJECT_DATA_H
#define OBJECT_DATA_H

#include <cmath>
#include <deque>
#include <string>

/**
 * @brief 2D 좌표 구조체
 */
struct ObjPoint {
    double x;
    double y;
};

/**
 * @brief 바운딩 박스 구조체
 * 매 프레임마다 생성되고 사라지는 임시 데이터
 */
struct box {
    double top = -1;
    double height = -1;
    double left = -1;
    double width = -1;
};

/**
 * @brief 객체 추적 메인 데이터 구조체
 * 
 * 추적 중인 객체의 모든 정보를 저장
 * 여러 모듈이 공유하는 데이터만 포함
 * 
 * === 초기값 정책 ===
 * - 타임스탬프: -1 (미설정 상태)
 * - 차로/방향: -1 (미설정 상태)
 * - 위치: {-1, -1} (무효 좌표)
 * - 속도: -1.0 (미계산 상태)
 * - 카운터: 0 (유효한 초기값)
 * - 플래그: false (초기 상태)
 * 
 * === 데이터 관리 정책 ===
 * - process_meta에서만 det_obj 쓰기
 * - 프로세서들은 복사본 반환
 * - 돌발이벤트 관련은 IncidentDetector가 자체 관리
 * 
 * === 읽기/쓰기 규칙 ===
 * [W:PM] = process_meta에서만 쓰기
 * [W:VP] = VehicleProcessor에서만 쓰기  
 * [W:PP] = PedestrianProcessor에서만 쓰기
 * [R:*]  = 모든 모듈에서 읽기 가능
 * 
 * 중요: 플래그 필드는 false→true 한 방향으로만 변경
 *      절대 true→false로 리셋하지 말 것
 */
struct obj_data {
    // ========== 객체 식별 정보 =============
    int object_id = 0;              // [W:PM] 트래커 ID (0부터 시작 가능)
    int class_id = 0;               // [W:PM] 클래스 ID (0부터 시작)
    std::string label;              // [W:PM] 객체 라벨 (PCAR, MBUS 등)
    
    // ========== 타임스탬프 (-1: 미설정) =================
    int first_detected_time = -1;   // [W:PM] 최초 검지 시간 (-1: 아직 설정 안됨)
    int stop_pass_time = -1;        // [W:VP] 정지선 통과 시간 (-1: 미통과)
    int turn_time = -1;             // [W:VP] 회전 ROI 진입 시간 (-1: 미진입)
    
    // ========== 위치 및 이동 ===============
    ObjPoint last_pos = {-1, -1};   // [W:VP] 이전 프레임에서의 위치 (매 프레임 업데이트, -1: 무효)
    ObjPoint prev_pos = {-1, -1};   // [W:VP] 1초 전 위치 (속도 계산용, -1: 무효)
    int prev_pos_time = -1;         // [W:VP] prev_pos 시점의 시간 (-1: 미설정)
    
    // ========== 차로 및 방향 (-1: 미설정) =============
    int lane = 0;                   // [W:VP] 차로 번호 (0: 차로 밖 또는 미확인, 1~N: 차로 번호)
    int dir_out = -1;               // [W:VP] 회전 방향 (-1: 미확정, 11:직진, 21:좌회전, 31:우회전, 41:유턴)
    
    // ========== 속도 데이터 (차량 전용, -1.0: 미계산) ==============
    double speed = -1.0;            // [W:VP] 현재 속도 (-1.0: 아직 계산 안됨)
    double avg_speed = -1.0;        // [W:VP] 평균 속도 (-1.0: 아직 계산 안됨)
    double stop_pass_speed = -1.0;  // [W:VP] 정지선 통과 속도 (-1.0: 미기록)
    double turn_pass_speed = -1.0;  // [W:VP] 회전 통과 속도 (-1.0: 미기록)
    double interval_speed = -1.0;   // [W:VP] 구간 속도 (-1.0: 미계산)
    int num_speed = 0;              // [W:VP] 속도 계산 횟수 (0부터 시작)
    
    // ========== 상태 플래그 ==========
    bool stop_line_pass = false;    // [W:VP] 정지선 통과 여부 (한번만 true로)
    bool turn_pass = false;         // [W:VP] 회전 ROI 진입 여부 (한번만 true로)
    bool data_sent_2k = false;      // [W:PM] 2K 데이터 전송 완료 (중복 방지)
    bool data_sent_4k = false;      // [W:PM] 4K 데이터 전송 완료 (중복 방지)
    bool data_processed = false;    // [W:VP] 프로세서 처리 완료 (새 객체 판단용)
    bool image_saved = false;       // [W:VP] 이미지 저장 여부 (중복 저장 방지)
    
    // ========== 보행자 관련 ==========
    std::deque<ObjPoint> prev_ped;  // [W:PP] 보행자 궤적 (FPS 개수만큼)
    bool cross_out = false;         // [W:PP] 횡단보도 밖 플래그
    bool ped_pass = false;          // [W:PP] 보행자 처리 완료 (한번만 true로)
    int ped_dir = 0;                // [W:PP] 보행자 방향 (0: 미정, -1: 왼쪽, 1: 오른쪽)
    
    // ========== 이미지 파일명 ==========
    std::string image_name;         // [W:VP] 저장된 이미지 파일명
};

/**
 * @brief 박스의 하단 중심점 계산
 * @param b 바운딩 박스
 * @return 하단 중심점 좌표
 */
inline ObjPoint getBottomCenter(const box& b) {
    return {b.left + b.width/2.0, b.top + b.height};
}

/**
 * @brief 두 점 사이의 거리 계산
 * @param p1 첫 번째 점
 * @param p2 두 번째 점
 * @return 유클리드 거리
 */
inline double calculateDistance(const ObjPoint& p1, const ObjPoint& p2) {
    double dx = p2.x - p1.x;
    double dy = p2.y - p1.y;
    return sqrt(dx * dx + dy * dy);
}

/**
 * @brief 위치가 유효한지 확인
 * @param pos 확인할 좌표
 * @return 유효하면 true, 무효(-1, -1)이면 false
 */
inline bool isValidPosition(const ObjPoint& pos) {
    return pos.x != -1 && pos.y != -1;
}

/**
 * @brief 타임스탬프가 유효한지 확인
 * @param timestamp 확인할 시간값
 * @return 유효하면 true, 미설정(-1)이면 false
 */
inline bool isValidTimestamp(int timestamp) {
    return timestamp != -1;
}

/**
 * @brief 속도값이 유효한지 확인
 * @param speed 확인할 속도값
 * @return 계산된 유효한 속도면 true, 미계산(-1.0)이면 false
 */
inline bool isValidSpeed(double speed) {
    return speed >= 0.0;  // -1.0은 미계산 상태
}

#endif // OBJECT_DATA_H