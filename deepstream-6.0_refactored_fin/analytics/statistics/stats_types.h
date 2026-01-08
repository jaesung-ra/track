#ifndef STATS_TYPES_H
#define STATS_TYPES_H

#include <ctime>
#include <map>
#include <string>
#include <vector>
#include "../../json/json.h"

/**
 * @brief 통계 타입 정의
 * 
 * 서버 DB의 hr_type_cd 값과 매핑
 */
enum class StatsType {
    STATS_SIGNAL_PHASE = 1,       // 신호현시 통계 (hr_type_cd = 1)
    STATS_INTERVAL = 3            // 5분 단위 통계 (hr_type_cd = 3)
};

/**
 * @brief 밀도 정보
 * 
 * 차로별 교통 밀도 정보를 저장하는 구조체
 * 밀도 단위: 대/km (표준 교통공학 단위)
 * 서버 DB 스키마와 일치
 */
struct DensityInfo {
    int avg_density = 0;          // 평균 밀도 (대/km)
    int min_density = 0;          // 최소 밀도 (대/km)
    int max_density = 0;          // 최대 밀도 (대/km)
    int vehicle_count = 0;        // 현재 차량 수 (참조용)
    double occupancy_rate = 0.0;  // 차로별 교통량 점유율 (%)
};

/**
 * @brief soitgaprdstats - 접근로별 통계
 * 
 * 로컬 엣지 Redis로 전송 (cam_id 없음)
 * 밀도 단위: 대/km/차선 (서버 DB 스키마와 일치)
 */
struct ApproachStats {
    int hr_type_cd;                  // StatsType을 int로
    int stats_bgng_unix_tm;
    int stats_end_unix_tm;
    int totl_trvl;
    double avg_stln_dttn_sped;
    double avg_sect_sped;
    int avg_trfc_dnst;               // 평균 교통밀도 (대/km/차선)
    int min_trfc_dnst;               // 최소 교통밀도 (대/km/차선)
    int max_trfc_dnst;               // 최대 교통밀도 (대/km/차선)
    double avg_lane_ocpn_rt;         // 평균 차로 점유율 (%)
    
    bool is_valid = false;
    
    ApproachStats() {}
};

/**
 * @brief soitgturntypestats - 회전별 통계
 * 
 * 로컬 엣지 Redis로 전송 (cam_id 없음)
 * 차종별 교통량 포함
 */
struct TurnTypeStats {
    int turn_type_cd;                    // 회전 방향 (11, 21, 31, 41 등)
    int hr_type_cd;
    int stats_bgng_unix_tm;
    int stats_end_unix_tm;
    
    // 차종별 교통량 (서버 DB 순서대로)
    int kncr1_trvl = 0;                  // MBUS
    int kncr2_trvl = 0;                  // LBUS
    int kncr3_trvl = 0;                  // PCAR
    int kncr4_trvl = 0;                  // MOTOR
    int kncr5_trvl = 0;                  // MTRUCK
    int kncr6_trvl = 0;                  // LTRUCK
    
    double avg_stln_dttn_sped;
    double avg_sect_sped;
    
    // 편의를 위한 필드 (Redis 전송시 제외)
    int totl_trvl = 0;                   // 전체 교통량 (kncr1~6 합계)
    bool is_valid = false;
    
    TurnTypeStats() {}
};

/**
 * @brief soitgkncrstats - 차종별 통계
 * 
 * 로컬 엣지 Redis로 전송 (cam_id 없음)
 */
struct VehicleTypeStats {
    std::string kncr_cd;             // 차종 코드 (PCAR, MBUS 등)
    int hr_type_cd;
    int stats_bgng_unix_tm;
    int stats_end_unix_tm;
    int totl_trvl;
    double avg_stln_dttn_sped;
    double avg_sect_sped;
    
    bool is_valid = false;
    
    VehicleTypeStats() {}
};

/**
 * @brief soitglanestats - 차로별 통계
 * 
 * 로컬 엣지 Redis로 전송 (cam_id 없음)
 * 밀도 단위: 대/km (서버 DB 스키마와 일치)
 */
struct LaneStats {
    int lane_no;
    int hr_type_cd;
    int stats_bgng_unix_tm;
    int stats_end_unix_tm;
    int totl_trvl;
    double avg_stln_dttn_sped;
    double avg_sect_sped;
    int avg_trfc_dnst;                // 평균 교통밀도 (대/km)
    int min_trfc_dnst;                // 최소 교통밀도 (대/km)
    int max_trfc_dnst;                // 최대 교통밀도 (대/km)
    double ocpn_rt;                   // 점유율 (%)
    
    bool is_valid = false;
    
    LaneStats() {}
};

/**
 * @brief 통계 데이터 패킷
 * 
 * 한 번에 생성되는 모든 통계를 담는 구조체
 */
struct StatsDataPacket {
    StatsType type;
    ApproachStats approach;
    std::vector<TurnTypeStats> turn_types;
    std::vector<VehicleTypeStats> vehicle_types;
    std::vector<LaneStats> lanes;
    
    bool is_valid = false;
};

#endif // STATS_TYPES_H