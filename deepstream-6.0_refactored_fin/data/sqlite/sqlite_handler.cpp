/*
 * sqlite_handler.cpp
 * 
 * SQLite 데이터베이스 핸들러 구현
 * main_table만 사용 (24시간 자동 삭제)
 */

#include "sqlite_handler.h"
#include "../../utils/config_manager.h"
#include <chrono>
#include <iostream>
#include <sstream>
#include <sys/stat.h>

SQLiteHandler::SQLiteHandler() {
    logger = getLogger("DS_SQLite_log");
    logger->info("SQLiteHandler 초기화 시작");
    
    // SQLite 버전 로그
    logger->info("SQLite runtime version: {}", sqlite3_libversion());
    
    // ConfigManager에서 경로 및 파일명 가져오기
    auto& config = ConfigManager::getInstance();
    db_path = config.getSQLitePath();  // 디렉토리 경로
    main_db_name = config.getString("paths.sqlite_db.filename", "test.db");
    
    logger->info("Database configuration - Path: {}, DB: {}", db_path, main_db_name);
    
    // 디렉토리 생성 확인
    struct stat st = {0};
    if (stat(db_path.c_str(), &st) == -1) {
        if (mkdir(db_path.c_str(), 0777) == 0) {
            logger->info("Database directory created: {}", db_path);
        } else {
            logger->error("Failed to create database directory: {}", db_path);
        }
    }
    
    // 단일 DB 초기화
    main_db = openDatabase(main_db_name);
    if (main_db) {
        // main_table 생성
        const char* main_table_sql = R"SQL(
            CREATE TABLE IF NOT EXISTS main_table(
                row_id INTEGER PRIMARY KEY AUTOINCREMENT,
                kncr_cd TEXT,
                lane_no INTEGER,
                turn_type_cd INTEGER,
                turn_dttn_unix_tm INTEGER,
                turn_dttn_sped REAL,
                stln_pasg_unix_tm INTEGER,
                stln_dttn_sped REAL,
                vhcl_sect_sped REAL,
                frst_obsrvn_unix_tm INTEGER,
                vhcl_obsrvn_hr INTEGER,
                vhcl_dttn_2k_id INTEGER,
                timestamp INTEGER DEFAULT (strftime('%s', 'now'))
            );
            CREATE INDEX IF NOT EXISTS idx_timestamp ON main_table(timestamp);
            CREATE INDEX IF NOT EXISTS idx_vhcl_dttn_2k_id ON main_table(vhcl_dttn_2k_id);
            CREATE INDEX IF NOT EXISTS idx_turn_type_cd ON main_table(turn_type_cd);
            CREATE INDEX IF NOT EXISTS idx_lane_no ON main_table(lane_no);
            CREATE INDEX IF NOT EXISTS idx_kncr_cd ON main_table(kncr_cd);
        )SQL";
        
        // Create main_table with indexes
        char* error_msg = nullptr;
        if (sqlite3_exec(main_db, main_table_sql, nullptr, nullptr, &error_msg) != SQLITE_OK) {
            logger->error("Failed to create main_table: {}", error_msg);
            sqlite3_free(error_msg);
        }
        
        // main_table 자동 삭제 트리거 생성 (24시간)
        const char* main_trigger_sql = R"SQL(
            CREATE TRIGGER IF NOT EXISTS cleanup_main_table AFTER INSERT ON main_table
            BEGIN 
                DELETE FROM main_table WHERE timestamp < (strftime('%s', 'now') - 86400);
            END;
        )SQL";
        
        if (sqlite3_exec(main_db, main_trigger_sql, nullptr, nullptr, &error_msg) != SQLITE_OK) {
            logger->error("Failed to create main trigger: {}", error_msg);
            sqlite3_free(error_msg);
        }
        
        logger->info("SQLite database initialized successfully");
    } else {
        logger->error("Failed to initialize database");
    }
}

SQLiteHandler::~SQLiteHandler() {
    logger->info("SQLiteHandler 종료");
    
    if (main_db) {
        sqlite3_close(main_db);
        main_db = nullptr;
    }
}

sqlite3* SQLiteHandler::openDatabase(const std::string& db_name) {
    std::string full_path = db_path + "/" + db_name;
    sqlite3* db = nullptr;
    
    int rc = sqlite3_open(full_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        logger->error("Cannot open database {}: {}", full_path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return nullptr;
    }
    
    // 성능 최적화를 위한 PRAGMA 설정
    char* error_msg = nullptr;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, &error_msg);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", nullptr, nullptr, &error_msg);
    sqlite3_exec(db, "PRAGMA cache_size=10000", nullptr, nullptr, &error_msg);
    sqlite3_exec(db, "PRAGMA temp_store=MEMORY", nullptr, nullptr, &error_msg);
    
    if (error_msg) {
        logger->warn("PRAGMA warning: {}", error_msg);
        sqlite3_free(error_msg);
    }
    
    return db;
}

int SQLiteHandler::executeSQL(const std::string& sql) {
    if (!main_db) return -1;
    
    char* error_msg = nullptr;
    int rc = sqlite3_exec(main_db, sql.c_str(), nullptr, nullptr, &error_msg);
    
    if (rc != SQLITE_OK) {
        logger->error("SQL error: {}", error_msg ? error_msg : "Unknown error");
        sqlite3_free(error_msg);
        return -1;
    }
    
    return 0;
}

int SQLiteHandler::insertVehicleData(int vehicle_id, const obj_data& obj, 
                                   const std::string& vehicle_type) {
    std::lock_guard<std::mutex> lock(db_mutex);
    
    if (!main_db) return -1;
    
    // main_table에 차량 데이터 삽입
    const char* sql = R"SQL(
        INSERT INTO main_table (kncr_cd, lane_no, turn_type_cd, 
                              turn_dttn_unix_tm, turn_dttn_sped, 
                              stln_pasg_unix_tm, stln_dttn_sped, 
                              vhcl_sect_sped, frst_obsrvn_unix_tm, 
                              vhcl_obsrvn_hr, vhcl_dttn_2k_id)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )SQL";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(main_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        logger->error("Failed to prepare insert: {}", sqlite3_errmsg(main_db));
        return -1;
    }
    
    // 파라미터 바인딩 - SQLITE_TRANSIENT 사용 (메모리 안전성)
    sqlite3_bind_text(stmt, 1, vehicle_type.c_str(), -1, SQLITE_TRANSIENT);  // kncr_cd
    sqlite3_bind_int(stmt, 2, obj.lane);                                     // lane_no
    sqlite3_bind_int(stmt, 3, obj.dir_out);                                  // turn_type_cd
    sqlite3_bind_int(stmt, 4, obj.turn_time);                                // turn_dttn_unix_tm
    sqlite3_bind_double(stmt, 5, obj.turn_pass_speed);                       // turn_dttn_sped
    sqlite3_bind_int(stmt, 6, obj.stop_pass_time);                           // stln_pasg_unix_tm
    sqlite3_bind_double(stmt, 7, obj.stop_pass_speed);                       // stln_dttn_sped
    sqlite3_bind_double(stmt, 8, obj.interval_speed);                        // vhcl_sect_sped
    sqlite3_bind_int(stmt, 9, obj.first_detected_time);                      // frst_obsrvn_unix_tm
    
    // 차량관측시간 계산 (vhcl_obsrvn_hr)
    int sensing_time = (obj.turn_time > 0) ?
                    (obj.turn_time - obj.first_detected_time) : 0;
    sqlite3_bind_int(stmt, 10, sensing_time);                                // vhcl_obsrvn_hr
    sqlite3_bind_int(stmt, 11, vehicle_id);                                  // vhcl_dttn_2k_id
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        logger->error("Failed to insert vehicle data: {}", sqlite3_errmsg(main_db));
        return -1;
    }
    
    logger->debug("Vehicle data inserted successfully: ID={}", vehicle_id);
    return 0;
}

int SQLiteHandler::cleanupOldData(int retention_hours) {
    // 트리거가 자동으로 처리하므로 수동 정리는 필요시에만
    logger->debug("Manual cleanup called - triggers handle automatic cleanup");
    return 0;
}

int SQLiteHandler::optimize() {
    if (!main_db) return -1;
    
    std::lock_guard<std::mutex> lock(db_mutex);
    return executeSQL("VACUUM");
}

bool SQLiteHandler::isHealthy() const {
    return (main_db != nullptr);
}

bool SQLiteHandler::tableExists(const std::string& table_name) const {
    if (!main_db) return false;
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    std::string query = "SELECT name FROM sqlite_master WHERE type='table' AND name='" + 
                       table_name + "'";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(main_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    
    return exists;
}