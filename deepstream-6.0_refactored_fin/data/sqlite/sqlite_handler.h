/*
 * sqlite_handler.h
 * 
 * SQLite 데이터베이스 핸들러
 * 24시간 자동 삭제 기능을 가진 main_table만 사용
 * StatsQueryHelper를 friend class로 지정
 */

#ifndef SQLITE_HANDLER_H
#define SQLITE_HANDLER_H

#include <map>
#include <memory>
#include <mutex>
#include <sqlite3.h>
#include <string>
#include <vector>
#include "../../common/object_data.h"

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

// Forward declaration
class StatsQueryHelper;

/**
 * @brief SQLite 데이터베이스 핸들러
 * 
 * 실시간 차량 데이터 저장을 위한 SQLite 관리 클래스
 * - main_table: 차량 데이터 저장 (24시간 자동 삭제)
 * 
 * 단일 DB 파일에 모든 테이블을 관리
 * cam_id와 이미지 정보는 저장하지 않음
 * 
 * main_table 스키마:
 * - row_id: PRIMARY KEY AUTOINCREMENT
 * - kncr_cd: 차종코드
 * - lane_no: 차로번호
 * - turn_type_cd: 회전유형코드
 * - turn_dttn_unix_tm: 회전검지유닉스시각
 * - turn_dttn_sped: 회전검지속도
 * - stln_pasg_unix_tm: 정지선통과유닉스시각
 * - stln_dttn_sped: 정지선검지속도
 * - vhcl_sect_sped: 차량구간속도
 * - frst_obsrvn_unix_tm: 최초관측유닉스시각
 * - vhcl_obsrvn_hr: 차량관측시간
 * - vhcl_dttn_2k_id: 차량ID
 * - timestamp: DB 저장 시각 (자동)
 */
class SQLiteHandler {
    // StatsQueryHelper가 내부 DB에 접근할 수 있도록 friend 지정
    friend class StatsQueryHelper;
    
private:
    // 데이터베이스 연결
    sqlite3* main_db = nullptr;
    
    // 데이터베이스 경로 및 파일명
    std::string db_path;
    std::string main_db_name;
    
    // 뮤텍스
    mutable std::mutex db_mutex;
    
    // 로거
    std::shared_ptr<spdlog::logger> logger;
    
    /**
     * @brief 데이터베이스 열기
     * @param db_name 데이터베이스 파일명
     * @return 성공 시 데이터베이스 포인터, 실패 시 nullptr
     */
    sqlite3* openDatabase(const std::string& db_name);
    
    /**
     * @brief SQL 실행 (범용)
     * @param sql SQL 문
     * @return 성공 시 0, 실패 시 음수
     */
    int executeSQL(const std::string& sql);

protected:
    /**
     * @brief 데이터베이스 포인터 반환 (StatsQueryHelper용)
     * @return sqlite3 포인터
     */
    sqlite3* getDatabase() const { 
        std::lock_guard<std::mutex> lock(db_mutex);
        return main_db; 
    }

public:
    /**
     * @brief 생성자
     */
    SQLiteHandler();
    
    /**
     * @brief 소멸자
     */
    ~SQLiteHandler();
    
    /**
     * @brief 차량 데이터 삽입 (main_table)
     * cam_id와 이미지 정보 없이 저장
     * @param vehicle_id 차량 ID (vhcl_dttn_2k_id)
     * @param obj 차량 객체 데이터
     * @param vehicle_type 차종 코드 (kncr_cd)
     * @return 성공 시 0, 실패 시 음수
     */
    int insertVehicleData(int vehicle_id, const obj_data& obj, 
                         const std::string& vehicle_type);
    
    /**
     * @brief 오래된 데이터 정리 (트리거가 자동 처리)
     * @param retention_hours 보관 시간 (시간 단위)
     * @return 삭제된 총 행 수
     */
    int cleanupOldData(int retention_hours = 24);
    
    /**
     * @brief 데이터베이스 최적화 (VACUUM)
     * @return 성공 시 0, 실패 시 음수
     */
    int optimize();
    
    /**
     * @brief 데이터베이스 상태 확인
     * @return 정상이면 true
     */
    bool isHealthy() const;
    
    /**
     * @brief 테이블 존재 확인
     * @param table_name 테이블명
     * @return 존재하면 true
     */
    bool tableExists(const std::string& table_name) const;
};

#endif // SQLITE_HANDLER_H