#include "database/mongodb.hpp"
#include "database/mssql.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"

#include <chrono>
#include <cstdlib>
#include <gtest/gtest.h>
#include <string>
#include <thread>

namespace {
    std::string getCACertPath() {
        const char* path = std::getenv("SQLEDITOR_TEST_CA_CERT");
        return path ? path : "";
    }

    bool hasTLSCerts() {
        return !getCACertPath().empty();
    }
} // namespace

// Postgres

class PostgresSSLTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!hasTLSCerts())
            GTEST_SKIP() << "TLS certs not available (SQLEDITOR_TEST_CA_CERT not set)";

        const char* host = std::getenv("SQLEDITOR_TEST_PG_HOST");
        const char* port = std::getenv("SQLEDITOR_TEST_PG_PORT");
        const char* db = std::getenv("SQLEDITOR_TEST_PG_DB");
        const char* user = std::getenv("SQLEDITOR_TEST_PG_USER");
        const char* pass = std::getenv("SQLEDITOR_TEST_PG_PASSWORD");
        if (!host || !db || !user)
            GTEST_SKIP() << "PostgreSQL test env vars not set";

        connInfo.name = "PostgresSSLTest";
        connInfo.type = DatabaseType::POSTGRESQL;
        connInfo.host = host;
        connInfo.port = port ? std::stoi(port) : 5432;
        connInfo.database = db;
        connInfo.username = user;
        if (pass)
            connInfo.password = pass;
    }

    DatabaseConnectionInfo connInfo;
};

TEST_F(PostgresSSLTest, ConnectsWithSslModeRequire) {
    connInfo.sslmode = SslMode::Require;
    auto database = std::make_shared<PostgresDatabase>(connInfo);

    const auto [ok, err] = database->connect();
    ASSERT_TRUE(ok) << "PostgreSQL SSL (require) connection failed: " << err;
    EXPECT_TRUE(database->isConnected());

    auto result = database->executeQuery("SELECT 1 AS test_col");
    ASSERT_TRUE(result.success()) << result.errorMessage();
    ASSERT_FALSE(result.empty());
    EXPECT_EQ(result[0].tableData[0][0], "1");

    database->disconnect();
}

TEST_F(PostgresSSLTest, ConnectsWithSslModeVerifyCA) {
    connInfo.sslmode = SslMode::VerifyCA;
    connInfo.sslCACertPath = getCACertPath();
    auto database = std::make_shared<PostgresDatabase>(connInfo);

    const auto [ok, err] = database->connect();
    ASSERT_TRUE(ok) << "PostgreSQL SSL (verify-ca) connection failed: " << err;
    EXPECT_TRUE(database->isConnected());

    auto result = database->executeQuery("SELECT 1 AS test_col");
    ASSERT_TRUE(result.success()) << result.errorMessage();

    database->disconnect();
}

TEST_F(PostgresSSLTest, SslDisabledStillConnects) {
    connInfo.sslmode = SslMode::Disable;
    auto database = std::make_shared<PostgresDatabase>(connInfo);

    const auto [ok, err] = database->connect();
    ASSERT_TRUE(ok) << "PostgreSQL (ssl disabled) connection failed: " << err;
    EXPECT_TRUE(database->isConnected());

    database->disconnect();
}

// MySQL

class MySQLSSLTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* host = std::getenv("SQLEDITOR_TEST_MYSQL_HOST");
        const char* port = std::getenv("SQLEDITOR_TEST_MYSQL_PORT");
        const char* db = std::getenv("SQLEDITOR_TEST_MYSQL_DB");
        const char* user = std::getenv("SQLEDITOR_TEST_MYSQL_USER");
        const char* pass = std::getenv("SQLEDITOR_TEST_MYSQL_PASSWORD");
        if (!host || !db || !user)
            GTEST_SKIP() << "MySQL test env vars not set";

        connInfo.name = "MySQLSSLTest";
        connInfo.type = DatabaseType::MYSQL;
        connInfo.host = host;
        connInfo.port = port ? std::stoi(port) : 3306;
        connInfo.database = db;
        connInfo.username = user;
        if (pass)
            connInfo.password = pass;
    }

    DatabaseConnectionInfo connInfo;
};

TEST_F(MySQLSSLTest, ConnectsWithSslModeRequire) {
    // MySQL 8.0 auto-generates self-signed certs
    connInfo.sslmode = SslMode::Require;
    auto database = std::make_shared<MySQLDatabase>(connInfo);

    bool connected = false;
    std::string lastError;
    for (int i = 0; i < 10; ++i) {
        auto [ok, err] = database->connect();
        if (ok) {
            connected = true;
            break;
        }
        lastError = err;
        // auto-create test DB if needed
        if (lastError.find("Unknown database") != std::string::npos) {
            auto adminInfo = connInfo;
            adminInfo.database = "";
            auto admin = std::make_shared<MySQLDatabase>(adminInfo);
            if (auto [aok, _] = admin->connect(); aok) {
                admin->createDatabase(connInfo.database);
                admin->disconnect();
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    ASSERT_TRUE(connected) << "MySQL SSL (require) connection failed: " << lastError;

    auto result = database->executeQuery("SELECT 1 AS test_col");
    ASSERT_TRUE(result.success()) << result.errorMessage();
    ASSERT_FALSE(result.empty());
    EXPECT_EQ(result[0].tableData[0][0], "1");

    database->disconnect();
}

TEST_F(MySQLSSLTest, ConnectsWithSslModePrefer) {
    connInfo.sslmode = SslMode::Prefer;
    auto database = std::make_shared<MySQLDatabase>(connInfo);

    bool connected = false;
    std::string lastError;
    for (int i = 0; i < 10; ++i) {
        auto [ok, err] = database->connect();
        if (ok) {
            connected = true;
            break;
        }
        lastError = err;
        if (lastError.find("Unknown database") != std::string::npos) {
            auto adminInfo = connInfo;
            adminInfo.database = "";
            auto admin = std::make_shared<MySQLDatabase>(adminInfo);
            if (auto [aok, _] = admin->connect(); aok) {
                admin->createDatabase(connInfo.database);
                admin->disconnect();
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    ASSERT_TRUE(connected) << "MySQL SSL (prefer) connection failed: " << lastError;

    auto result = database->executeQuery("SELECT 1 AS test_col");
    ASSERT_TRUE(result.success()) << result.errorMessage();

    database->disconnect();
}

// Regression test: VerifyCA must actually verify the server cert. The MySQL
// container in scripts/run-tests serves an auto-generated self-signed cert
// that is not signed by the test CA, so a connect with VerifyCA + the test
// CA must fail. If MYSQL_OPT_SSL_VERIFY_SERVER_CERT is left at 0 for
// VerifyCA, this connect would erroneously succeed.
TEST_F(MySQLSSLTest, RejectsSslModeVerifyCAWithUnrelatedCA) {
    const char* caCert = std::getenv("SQLEDITOR_TEST_CA_CERT");
    if (!caCert)
        GTEST_SKIP() << "SQLEDITOR_TEST_CA_CERT not set";

    connInfo.sslmode = SslMode::VerifyCA;
    connInfo.sslCACertPath = caCert;
    auto database = std::make_shared<MySQLDatabase>(connInfo);

    auto [ok, err] = database->connect();
    EXPECT_FALSE(ok) << "VerifyCA must reject server cert not signed by the supplied CA";
    if (!ok) {
        // sanity check that the failure is TLS-related, not e.g. a bad host
        const bool tlsError =
            err.find("SSL") != std::string::npos || err.find("TLS") != std::string::npos ||
            err.find("certificate") != std::string::npos || err.find("verify") != std::string::npos;
        EXPECT_TRUE(tlsError) << "expected an SSL/TLS error, got: " << err;
    }
    database->disconnect();
}

TEST_F(MySQLSSLTest, ConnectsWithSslModeDisable) {
    connInfo.sslmode = SslMode::Disable;
    auto database = std::make_shared<MySQLDatabase>(connInfo);

    bool connected = false;
    std::string lastError;
    for (int i = 0; i < 10; ++i) {
        auto [ok, err] = database->connect();
        if (ok) {
            connected = true;
            break;
        }
        lastError = err;
        if (lastError.find("Unknown database") != std::string::npos) {
            auto adminInfo = connInfo;
            adminInfo.database = "";
            auto admin = std::make_shared<MySQLDatabase>(adminInfo);
            if (auto [aok, _] = admin->connect(); aok) {
                admin->createDatabase(connInfo.database);
                admin->disconnect();
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    ASSERT_TRUE(connected) << "MySQL (ssl disabled) connection failed: " << lastError;

    database->disconnect();
}

// Redis

class RedisTLSTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!hasTLSCerts())
            GTEST_SKIP() << "TLS certs not available (SQLEDITOR_TEST_CA_CERT not set)";

        const char* host = std::getenv("SQLEDITOR_TEST_REDIS_HOST");
        const char* tlsPort = std::getenv("SQLEDITOR_TEST_REDIS_TLS_PORT");
        if (!host || !tlsPort)
            GTEST_SKIP() << "Redis TLS test env vars not set";

        connInfo.name = "RedisTLSTest";
        connInfo.type = DatabaseType::REDIS;
        connInfo.host = host;
        connInfo.port = std::stoi(tlsPort);
    }

    DatabaseConnectionInfo connInfo;
};

TEST_F(RedisTLSTest, ConnectsWithSslModeRequire) {
    connInfo.sslmode = SslMode::Require;
    connInfo.sslCACertPath = getCACertPath();
    auto database = std::make_shared<RedisDatabase>(connInfo);

    bool connected = false;
    std::string lastError;
    for (int i = 0; i < 15; ++i) {
        auto [ok, err] = database->connect();
        if (ok) {
            connected = true;
            break;
        }
        lastError = err;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    ASSERT_TRUE(connected) << "Redis TLS (require) connection failed: " << lastError;
    EXPECT_TRUE(database->isConnected());

    auto result = database->executeQuery("PING");
    ASSERT_FALSE(result.empty());
    EXPECT_TRUE(result[0].success);

    database->disconnect();
}

TEST_F(RedisTLSTest, ConnectsWithSslModeVerifyCA) {
    connInfo.sslmode = SslMode::VerifyCA;
    connInfo.sslCACertPath = getCACertPath();
    auto database = std::make_shared<RedisDatabase>(connInfo);

    bool connected = false;
    std::string lastError;
    for (int i = 0; i < 15; ++i) {
        auto [ok, err] = database->connect();
        if (ok) {
            connected = true;
            break;
        }
        lastError = err;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    ASSERT_TRUE(connected) << "Redis TLS (verify-ca) connection failed: " << lastError;
    EXPECT_TRUE(database->isConnected());

    auto result = database->executeQuery("PING");
    ASSERT_FALSE(result.empty());
    EXPECT_TRUE(result[0].success);

    database->disconnect();
}

// MongoDB

class MongoDBTLSTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!hasTLSCerts())
            GTEST_SKIP() << "TLS certs not available (SQLEDITOR_TEST_CA_CERT not set)";

        const char* host = std::getenv("SQLEDITOR_TEST_MONGODB_HOST");
        const char* port = std::getenv("SQLEDITOR_TEST_MONGODB_PORT");
        if (!host)
            GTEST_SKIP() << "MongoDB test env vars not set";

        connInfo.name = "MongoDBTLSTest";
        connInfo.type = DatabaseType::MONGODB;
        connInfo.host = host;
        connInfo.port = port ? std::stoi(port) : 27017;
        connInfo.database = "test";
    }

    DatabaseConnectionInfo connInfo;
};

TEST_F(MongoDBTLSTest, ConnectsWithSslModeRequire) {
    connInfo.sslmode = SslMode::Require;
    auto database = std::make_shared<MongoDBDatabase>(connInfo);

    bool connected = false;
    std::string lastError;
    for (int i = 0; i < 15; ++i) {
        auto [ok, err] = database->connect();
        if (ok) {
            connected = true;
            break;
        }
        lastError = err;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    ASSERT_TRUE(connected) << "MongoDB TLS (require) connection failed: " << lastError;
    EXPECT_TRUE(database->isConnected());

    database->disconnect();
}

TEST_F(MongoDBTLSTest, ConnectsWithSslModeVerifyCA) {
    connInfo.sslmode = SslMode::VerifyCA;
    connInfo.sslCACertPath = getCACertPath();
    auto database = std::make_shared<MongoDBDatabase>(connInfo);

    bool connected = false;
    std::string lastError;
    for (int i = 0; i < 15; ++i) {
        auto [ok, err] = database->connect();
        if (ok) {
            connected = true;
            break;
        }
        lastError = err;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    ASSERT_TRUE(connected) << "MongoDB TLS (verify-ca) connection failed: " << lastError;
    EXPECT_TRUE(database->isConnected());

    database->disconnect();
}

// MSSQL

class MSSQLSSLTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* host = std::getenv("SQLEDITOR_TEST_MSSQL_HOST");
        const char* port = std::getenv("SQLEDITOR_TEST_MSSQL_PORT");
        const char* db = std::getenv("SQLEDITOR_TEST_MSSQL_DB");
        const char* user = std::getenv("SQLEDITOR_TEST_MSSQL_USER");
        const char* pass = std::getenv("SQLEDITOR_TEST_MSSQL_PASSWORD");
        if (!host || !db || !user)
            GTEST_SKIP() << "MSSQL test env vars not set";

        connInfo.name = "MSSQLSSLTest";
        connInfo.type = DatabaseType::MSSQL;
        connInfo.host = host;
        connInfo.port = port ? std::stoi(port) : 1433;
        connInfo.database = db;
        connInfo.username = user;
        if (pass)
            connInfo.password = pass;
    }

    DatabaseConnectionInfo connInfo;
};

TEST_F(MSSQLSSLTest, ConnectsWithSslModeRequire) {
    // MSSQL Docker auto-generates a self-signed TLS cert
    connInfo.sslmode = SslMode::Require;
    auto database = std::make_shared<MSSQLDatabase>(connInfo);

    bool connected = false;
    std::string lastError;
    for (int i = 0; i < 15; ++i) {
        try {
            auto [ok, err] = database->connect();
            if (ok) {
                connected = true;
                break;
            }
            lastError = err;
        } catch (const std::exception& e) {
            lastError = e.what();
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    ASSERT_TRUE(connected) << "MSSQL SSL (require) connection failed: " << lastError;

    auto result = database->executeQuery("SELECT 1 AS test_col");
    ASSERT_TRUE(result.success()) << result.errorMessage();
    ASSERT_FALSE(result.empty());
    EXPECT_EQ(result[0].tableData[0][0], "1");

    database->disconnect();
}
