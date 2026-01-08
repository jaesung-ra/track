#ifndef QUEUE_TYPES_H
#define QUEUE_TYPES_H

#include <ctime>
#include <map>
#include <string>
#include <vector>

/**
 * @brief 차로별 대기행렬 데이터
 */
struct LaneQueue {
    int lane_no;                    // 차로 번호
    int stats_bgng_unix_tm;         // 통계 시작 시간 (이전 녹색 신호 시작)
    int stats_end_unix_tm;          // 통계 종료 시간 (현재 녹색 신호 시작)
    double rmnn_queu_lngt;          // 잔여 대기행렬 길이 (대수)
    double max_queu_lngt;           // 최대 대기행렬 길이 (대수)
    std::string img_path_nm;        // 이미지 저장 경로
    std::string img_file_nm;        // 이미지 파일명
    
    bool is_valid = false;
    
    LaneQueue() {}
};

/**
 * @brief 접근로별 대기행렬 데이터
 */
struct ApproachQueue {
    int stats_bgng_unix_tm;         // 통계 시작 시간 (이전 녹색 신호 시작)
    int stats_end_unix_tm;          // 통계 종료 시간 (현재 녹색 신호 시작)
    double rmnn_queu_lngt;          // 잔여 대기행렬 길이 (전체 차로 합계)
    double max_queu_lngt;           // 최대 대기행렬 길이 (전체 차로 합계)
    std::string img_path_nm;        // 이미지 저장 경로
    std::string img_file_nm;        // 이미지 파일명
    
    bool is_valid = false;
    
    ApproachQueue() {}
};

/**
 * @brief 대기행렬 데이터 패킷
 */
struct QueueDataPacket {
    int timestamp;                  // 생성 시간
    int signal_cycle;               // 신호 주기 번호
    
    // 접근로별 대기행렬
    ApproachQueue approach;
    
    // 차로별 대기행렬
    std::vector<LaneQueue> lanes;
    
    // 이미지 정보
    bool has_image = false;
    std::string image_timestamp;    // 이미지 캡처 시간
    
    bool is_valid = false;
};

/**
 * @brief 대기행렬 분석 설정
 */
struct QueueConfig {
    bool capture_image = true;          // 이미지 캡처 여부
    std::string image_save_path;        // 실제 이미지 저장 경로 (로컬)
};

#endif // QUEUE_TYPES_H