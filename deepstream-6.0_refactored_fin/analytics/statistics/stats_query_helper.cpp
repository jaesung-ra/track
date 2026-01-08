/*
 * stats_query_helper.cpp
 * 
 * 통계 쿼리 헬퍼 클래스 구현
 * SQLiteHandler의 DB 연결을 사용하여 통계 관련 쿼리 수행
 */

#include "stats_query_helper.h"
#include <sqlite3.h>
#include <sstream>

StatsQueryHelper::StatsQueryHelper(SQLiteHandler* handler) 
    : sqlite_handler_(handler) {
    logger = getLogger("DS_StatsQuery_log");
    logger->info("StatsQueryHelper 생성");
}

bool StatsQueryHelper::executeQuery(const std::string& query, 
                                  std::function<void(sqlite3_stmt*)> callback) const {
    if (!sqlite_handler_ || !sqlite_handler_->isHealthy()) {
        logger->error("SQLiteHandler가 유효하지 않음");
        return false;
    }
    
    sqlite3* db = sqlite_handler_->getDatabase();
    if (!db) {
        logger->error("데이터베이스 연결을 가져올 수 없음");
        return false;
    }
    
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
    
    if (rc != SQLITE_OK) {
        logger->error("쿼리 준비 실패: {} - SQL: {}", sqlite3_errmsg(db), query);
        return false;
    }
    
    bool success = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (callback) {
            callback(stmt);
        }
    }
    
    sqlite3_finalize(stmt);
    return success;
}

// 회전별 통계 조회
int StatsQueryHelper::getVehicleCountByTurn(int start_time, int end_time, int turn_type) const {
    int count = 0;
    
    std::stringstream query;
    query << "SELECT COUNT(*) FROM main_table WHERE turn_type_cd = " << turn_type
          << " AND stln_pasg_unix_tm >= " << start_time 
          << " AND stln_pasg_unix_tm < " << end_time;
    
    executeQuery(query.str(), [&count](sqlite3_stmt* stmt) {
        count = sqlite3_column_int(stmt, 0);
    });
    
    return count;
}

double StatsQueryHelper::getAverageStopLineSpeedByTurn(int start_time, int end_time, int turn_type) const {
    double avg_speed = 0.0;
    
    std::stringstream query;
    query << "SELECT AVG(stln_dttn_sped) FROM main_table WHERE turn_type_cd = " << turn_type
          << " AND stln_pasg_unix_tm >= " << start_time 
          << " AND stln_pasg_unix_tm < " << end_time;
    
    executeQuery(query.str(), [&avg_speed](sqlite3_stmt* stmt) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            avg_speed = sqlite3_column_double(stmt, 0);
        }
    });
    
    return avg_speed;
}

double StatsQueryHelper::getAverageIntervalSpeedByTurn(int start_time, int end_time, int turn_type) const {
    double avg_speed = 0.0;
    
    std::stringstream query;
    query << "SELECT AVG(vhcl_sect_sped) FROM main_table WHERE turn_type_cd = " << turn_type
          << " AND stln_pasg_unix_tm >= " << start_time 
          << " AND stln_pasg_unix_tm < " << end_time;
    
    executeQuery(query.str(), [&avg_speed](sqlite3_stmt* stmt) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            avg_speed = sqlite3_column_double(stmt, 0);
        }
    });
    
    return avg_speed;
}

// 회전별 + 차종별 통계 조회 (TurnTypeStats의 차종별 교통량용)
int StatsQueryHelper::getVehicleCountByTurnAndType(int start_time, int end_time, int turn_type, const std::string& vehicle_type) const {
    int count = 0;
    
    std::stringstream query;
    query << "SELECT COUNT(*) FROM main_table WHERE turn_type_cd = " << turn_type
          << " AND kncr_cd = '" << vehicle_type << "'"
          << " AND stln_pasg_unix_tm >= " << start_time 
          << " AND stln_pasg_unix_tm < " << end_time;
    
    executeQuery(query.str(), [&count](sqlite3_stmt* stmt) {
        count = sqlite3_column_int(stmt, 0);
    });
    
    return count;
}

// 차종별 통계 조회
int StatsQueryHelper::getVehicleCountByType(int start_time, int end_time, const std::string& vehicle_type) const {
    int count = 0;
    
    std::stringstream query;
    query << "SELECT COUNT(*) FROM main_table WHERE kncr_cd = '" << vehicle_type << "'"
          << " AND stln_pasg_unix_tm >= " << start_time 
          << " AND stln_pasg_unix_tm < " << end_time;
    
    executeQuery(query.str(), [&count](sqlite3_stmt* stmt) {
        count = sqlite3_column_int(stmt, 0);
    });
    
    return count;
}

double StatsQueryHelper::getAverageStopLineSpeedByType(int start_time, int end_time, const std::string& vehicle_type) const {
    double avg_speed = 0.0;
    
    std::stringstream query;
    query << "SELECT AVG(stln_dttn_sped) FROM main_table WHERE kncr_cd = '" << vehicle_type << "'"
          << " AND stln_pasg_unix_tm >= " << start_time 
          << " AND stln_pasg_unix_tm < " << end_time;
    
    executeQuery(query.str(), [&avg_speed](sqlite3_stmt* stmt) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            avg_speed = sqlite3_column_double(stmt, 0);
        }
    });
    
    return avg_speed;
}

double StatsQueryHelper::getAverageIntervalSpeedByType(int start_time, int end_time, const std::string& vehicle_type) const {
    double avg_speed = 0.0;
    
    std::stringstream query;
    query << "SELECT AVG(vhcl_sect_sped) FROM main_table WHERE kncr_cd = '" << vehicle_type << "'"
          << " AND stln_pasg_unix_tm >= " << start_time 
          << " AND stln_pasg_unix_tm < " << end_time;
    
    executeQuery(query.str(), [&avg_speed](sqlite3_stmt* stmt) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            avg_speed = sqlite3_column_double(stmt, 0);
        }
    });
    
    return avg_speed;
}

// 차로별 통계 조회
int StatsQueryHelper::getVehicleCountByLane(int start_time, int end_time, int lane) const {
    int count = 0;
    
    std::stringstream query;
    query << "SELECT COUNT(*) FROM main_table WHERE lane_no = " << lane
          << " AND stln_pasg_unix_tm >= " << start_time 
          << " AND stln_pasg_unix_tm < " << end_time;
    
    executeQuery(query.str(), [&count](sqlite3_stmt* stmt) {
        count = sqlite3_column_int(stmt, 0);
    });
    
    return count;
}

double StatsQueryHelper::getAverageStopLineSpeedByLane(int start_time, int end_time, int lane) const {
    double avg_speed = 0.0;
    
    std::stringstream query;
    query << "SELECT AVG(stln_dttn_sped) FROM main_table WHERE lane_no = " << lane
          << " AND stln_pasg_unix_tm >= " << start_time 
          << " AND stln_pasg_unix_tm < " << end_time;
    
    executeQuery(query.str(), [&avg_speed](sqlite3_stmt* stmt) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            avg_speed = sqlite3_column_double(stmt, 0);
        }
    });
    
    return avg_speed;
}

double StatsQueryHelper::getAverageIntervalSpeedByLane(int start_time, int end_time, int lane) const {
    double avg_speed = 0.0;
    
    std::stringstream query;
    query << "SELECT AVG(vhcl_sect_sped) FROM main_table WHERE lane_no = " << lane
          << " AND stln_pasg_unix_tm >= " << start_time 
          << " AND stln_pasg_unix_tm < " << end_time;
    
    executeQuery(query.str(), [&avg_speed](sqlite3_stmt* stmt) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            avg_speed = sqlite3_column_double(stmt, 0);
        }
    });
    
    return avg_speed;
}

// 접근로별 통계 조회
int StatsQueryHelper::getTotalVehicleCount(int start_time, int end_time) const {
    int count = 0;
    
    std::stringstream query;
    query << "SELECT COUNT(*) FROM main_table WHERE stln_pasg_unix_tm >= " << start_time 
          << " AND stln_pasg_unix_tm < " << end_time;
    
    executeQuery(query.str(), [&count](sqlite3_stmt* stmt) {
        count = sqlite3_column_int(stmt, 0);
    });
    
    return count;
}

double StatsQueryHelper::getTotalAverageStopLineSpeed(int start_time, int end_time) const {
    double avg_speed = 0.0;
    
    std::stringstream query;
    query << "SELECT AVG(stln_dttn_sped) FROM main_table WHERE stln_pasg_unix_tm >= " << start_time 
          << " AND stln_pasg_unix_tm < " << end_time;
    
    executeQuery(query.str(), [&avg_speed](sqlite3_stmt* stmt) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            avg_speed = sqlite3_column_double(stmt, 0);
        }
    });
    
    return avg_speed;
}

double StatsQueryHelper::getTotalAverageIntervalSpeed(int start_time, int end_time) const {
    double avg_speed = 0.0;
    
    std::stringstream query;
    query << "SELECT AVG(vhcl_sect_sped) FROM main_table WHERE stln_pasg_unix_tm >= " << start_time 
          << " AND stln_pasg_unix_tm < " << end_time;
    
    executeQuery(query.str(), [&avg_speed](sqlite3_stmt* stmt) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            avg_speed = sqlite3_column_double(stmt, 0);
        }
    });
    
    return avg_speed;
}