#include "database/cassandra.hpp"
#include "database/db_interface.hpp"
#include "database/mongodb.hpp"
#include "database/mongodb_old.hpp"
#include "database/mssql.hpp"
#include "database/mysql.hpp"
#include "database/oracle.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"

// Helper functions to convert between DatabaseType enum and strings
std::string databaseTypeToString(const DatabaseType type) {
    switch (type) {
    case DatabaseType::SQLITE:
        return "sqlite";
    case DatabaseType::POSTGRESQL:
        return "postgresql";
    case DatabaseType::MYSQL:
        return "mysql";
    case DatabaseType::REDIS:
        return "redis";
    case DatabaseType::MONGODB:
        return "mongodb";
    case DatabaseType::MONGODB_LEGACY:
        return "mongodb_legacy";
    case DatabaseType::MONGODB_OLD:
        return "mongodb_old";
    case DatabaseType::MARIADB:
        return "mariadb";
    case DatabaseType::MSSQL:
        return "mssql";
    case DatabaseType::ORACLE:
        return "oracle";
    case DatabaseType::REDSHIFT:
        return "redshift";
    case DatabaseType::CASSANDRA:
        return "cassandra";
    }
    return "unknown";
}

DatabaseType stringToDatabaseType(const std::string& typeStr) {
    if (typeStr == "sqlite")
        return DatabaseType::SQLITE;
    if (typeStr == "postgresql")
        return DatabaseType::POSTGRESQL;
    if (typeStr == "mysql")
        return DatabaseType::MYSQL;
    if (typeStr == "redis")
        return DatabaseType::REDIS;
    if (typeStr == "mongodb")
        return DatabaseType::MONGODB;
    if (typeStr == "mongodb_legacy")
        return DatabaseType::MONGODB_LEGACY;
    if (typeStr == "mongodb_old")
        return DatabaseType::MONGODB_OLD;
    if (typeStr == "mariadb")
        return DatabaseType::MARIADB;
    if (typeStr == "mssql")
        return DatabaseType::MSSQL;
    if (typeStr == "oracle")
        return DatabaseType::ORACLE;
    if (typeStr == "redshift")
        return DatabaseType::REDSHIFT;
    if (typeStr == "cassandra")
        return DatabaseType::CASSANDRA;
    return DatabaseType::SQLITE; // default
}

std::string sslModeToString(const SslMode mode) {
    switch (mode) {
    case SslMode::Disable:
        return "disable";
    case SslMode::Allow:
        return "allow";
    case SslMode::Prefer:
        return "prefer";
    case SslMode::Require:
        return "require";
    case SslMode::VerifyCA:
        return "verify-ca";
    case SslMode::VerifyFull:
        return "verify-full";
    case SslMode::VerifyIdentity:
        return "verify-identity";
    }
    return "prefer";
}

SslMode stringToSslMode(const std::string& str) {
    if (str == "disable")
        return SslMode::Disable;
    if (str == "allow")
        return SslMode::Allow;
    if (str == "prefer")
        return SslMode::Prefer;
    if (str == "require")
        return SslMode::Require;
    if (str == "verify-ca")
        return SslMode::VerifyCA;
    if (str == "verify-full")
        return SslMode::VerifyFull;
    if (str == "verify-identity")
        return SslMode::VerifyIdentity;
    return SslMode::Prefer;
}

std::shared_ptr<DatabaseInterface>
DatabaseFactory::createDatabase(const DatabaseConnectionInfo& info) {
    switch (info.type) {
    case DatabaseType::SQLITE:
        return std::make_shared<SQLiteDatabase>(info);

    case DatabaseType::POSTGRESQL:
        return std::make_shared<PostgresDatabase>(info);

    case DatabaseType::MYSQL:
        return std::make_shared<MySQLDatabase>(info);

    case DatabaseType::REDIS:
        return std::make_shared<RedisDatabase>(info);

    case DatabaseType::MONGODB:
    case DatabaseType::MONGODB_LEGACY:
        return std::make_shared<MongoDBDatabase>(info);

    case DatabaseType::MONGODB_OLD:
        return std::make_shared<MongoDBOldDatabase>(info);

    case DatabaseType::MARIADB:
        return std::make_shared<MySQLDatabase>(info);

    case DatabaseType::MSSQL:
        return std::make_shared<MSSQLDatabase>(info);

    case DatabaseType::ORACLE:
        return std::make_shared<OracleDatabase>(info);

    case DatabaseType::REDSHIFT:
        return std::make_shared<PostgresDatabase>(info);

    case DatabaseType::CASSANDRA:
        return std::make_shared<CassandraDatabase>(info);

    default:
        return nullptr;
    }
}
