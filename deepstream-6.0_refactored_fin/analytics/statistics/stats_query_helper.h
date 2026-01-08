#ifndef STATS_QUERY_HELPER_H
#define STATS_QUERY_HELPER_H

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "stats_types.h"
#include "../../data/sqlite/sqlite_handler.h"

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

/**
 * @brief 통계 쿼리 헬퍼 클래스
 * 
 * SQLiteHandler의 friend 클래스로 동작하며,
 * 통계 관련 복잡한 쿼리를 처리
 * SQLiteHandler의 비대화를 방지하고 책임을 분리
 */
class StatsQueryHelper {
private:
    SQLiteHandler* sqlite_handler_;
    std::shared_ptr<spdlog::logger> logger;
    
    /**
     * @brief 쿼리 실행 헬퍼 메서드
     * @param query SQL 쿼리
     * @param callback 결과 처리 콜백
     * @return 성공 시 true
     */
    bool executeQuery(const std::string& query, 
                     std::function<void(sqlite3_stmt*)> callback) const;

public:
    /**
     * @brief 생성자
     * @param handler SQLiteHandler 포인터
     */
    explicit StatsQueryHelper(SQLiteHandler* handler);
    
    /**
     * @brief 소멸자
     */
    ~StatsQueryHelper() = default;
    
    // 회전별 통계 조회
    int getVehicleCountByTurn(int start_time, int end_time, int turn_type) const;
    double getAverageStopLineSpeedByTurn(int start_time, int end_time, int turn_type) const;
    double getAverageIntervalSpeedByTurn(int start_time, int end_time, int turn_type) const;
    
    // 회전별 + 차종별 통계 조회 (TurnTypeStats의 차종별 교통량용)
    int getVehicleCountByTurnAndType(int start_time, int end_time, int turn_type, const std::string& vehicle_type) const;
    
    // 차종별 통계 조회
    int getVehicleCountByType(int start_time, int end_time, const std::string& vehicle_type) const;
    double getAverageStopLineSpeedByType(int start_time, int end_time, const std::string& vehicle_type) const;
    double getAverageIntervalSpeedByType(int start_time, int end_time, const std::string& vehicle_type) const;
    
    // 차로별 통계 조회
    int getVehicleCountByLane(int start_time, int end_time, int lane) const;
    double getAverageStopLineSpeedByLane(int start_time, int end_time, int lane) const;
    double getAverageIntervalSpeedByLane(int start_time, int end_time, int lane) const;
    
    // 접근로별 통계 조회
    int getTotalVehicleCount(int start_time, int end_time) const;
    double getTotalAverageStopLineSpeed(int start_time, int end_time) const;
    double getTotalAverageIntervalSpeed(int start_time, int end_time) const;
};

#endif // STATS_QUERY_HELPER_H