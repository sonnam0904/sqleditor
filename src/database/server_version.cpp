#include "database/server_version.hpp"
#include "database/cassandra.hpp"
#include "database/db_interface.hpp"
#include "database/mongodb.hpp"
#include "database/mssql.hpp"
#include "database/mysql.hpp"
#include "database/oracle.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"
#include <cctype>
#include <spdlog/spdlog.h>

namespace db_version {

std::string scalarFromQuery(const QueryResult& result) {
    if (result.statements.empty()) {
        return {};
    }
    const auto& statement = result.statements.front();
    if (!statement.success || statement.tableData.empty() || statement.tableData.front().empty()) {
        return {};
    }
    return statement.tableData.front().front();
}

namespace {

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string compactVersionLabel(std::string raw) {
    raw = trim(std::move(raw));
    if (raw.empty()) {
        return raw;
    }

    constexpr std::string_view postgresPrefix = "PostgreSQL ";
    if (raw.starts_with(postgresPrefix)) {
        raw.erase(0, postgresPrefix.size());
    }

    if (const auto cut = raw.find_first_of(" (\t"); cut != std::string::npos) {
        raw.resize(cut);
    }

    if (const auto dash = raw.find('-'); dash != std::string::npos && dash > 0) {
        const std::string_view suffix(raw.data() + dash + 1, raw.size() - dash - 1);
        if (!suffix.empty() && !std::isdigit(static_cast<unsigned char>(suffix.front()))) {
            raw.resize(dash);
        }
    }

    return trim(std::move(raw));
}

void storeIfNonEmpty(DatabaseInterface& db, std::string version) {
    version = compactVersionLabel(std::move(version));
    if (!version.empty()) {
        db.setServerVersion(std::move(version));
    }
}

} // namespace

void fetchAndStoreServerVersion(DatabaseInterface& db) {
    db.clearServerVersion();
    if (!db.isConnected()) {
        return;
    }

    const auto type = db.getConnectionInfo().type;

    try {
        switch (type) {
        case DatabaseType::POSTGRESQL:
        case DatabaseType::REDSHIFT:
            if (auto* pg = dynamic_cast<PostgresDatabase*>(&db)) {
                storeIfNonEmpty(db, scalarFromQuery(pg->executeQuery("SHOW server_version", 1)));
            }
            break;
        case DatabaseType::MYSQL:
        case DatabaseType::MARIADB:
            if (auto* mysql = dynamic_cast<MySQLDatabase*>(&db)) {
                storeIfNonEmpty(db, scalarFromQuery(mysql->executeQuery("SELECT VERSION()", 1)));
            }
            break;
        case DatabaseType::MSSQL:
            if (auto* mssql = dynamic_cast<MSSQLDatabase*>(&db)) {
                storeIfNonEmpty(
                    db, scalarFromQuery(mssql->executeQuery(
                            "SELECT CAST(SERVERPROPERTY('ProductVersion') AS NVARCHAR(128))", 1)));
            }
            break;
        case DatabaseType::ORACLE:
            if (auto* oracle = dynamic_cast<OracleDatabase*>(&db)) {
                storeIfNonEmpty(db,
                                scalarFromQuery(oracle->executeQuery("SELECT version FROM v$instance",
                                                                    1)));
            }
            break;
        case DatabaseType::MONGODB:
            if (auto* mongo = dynamic_cast<MongoDBDatabase*>(&db)) {
                storeIfNonEmpty(db, mongo->readServerVersion());
            }
            break;
        case DatabaseType::REDIS:
            if (auto* redis = dynamic_cast<RedisDatabase*>(&db)) {
                storeIfNonEmpty(db, redis->readServerVersion());
            }
            break;
        case DatabaseType::CASSANDRA:
            if (auto* cass = dynamic_cast<CassandraDatabase*>(&db)) {
                storeIfNonEmpty(
                    db, scalarFromQuery(cass->executeQuery("SELECT release_version FROM system.local",
                                                          1)));
            }
            break;
        case DatabaseType::SQLITE:
            if (auto* sqlite = dynamic_cast<SQLiteDatabase*>(&db)) {
                storeIfNonEmpty(db, scalarFromQuery(sqlite->executeQuery("SELECT sqlite_version()",
                                                                        1)));
            }
            break;
        default:
            break;
        }
    } catch (const std::exception& e) {
        spdlog::warn("Failed to read server version: {}", e.what());
    }
}

} // namespace db_version
