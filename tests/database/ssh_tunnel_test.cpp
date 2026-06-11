#include "database/postgresql.hpp"
#include "database/ssh_tunnel.hpp"

#include <chrono>
#include <cstdlib>
#include <gtest/gtest.h>
#include <string>
#include <thread>

namespace {
    struct SSHTunnelTestConfig {
        std::string sshHost;
        int sshPort = 22;
        std::string sshUser;
        std::string sshPassword;
        std::string sshKeyPath;
        std::string pgRemoteHost; // container name reachable from SSH server
        int pgRemotePort = 5432;
        std::string pgDatabase;
        std::string pgUser;
        std::string pgPassword;
    };

    std::optional<SSHTunnelTestConfig> loadSSHTunnelConfig() {
        SSHTunnelTestConfig cfg;

        const char* sshHost = std::getenv("SQLEDITOR_TEST_SSH_HOST");
        const char* sshPort = std::getenv("SQLEDITOR_TEST_SSH_PORT");
        const char* sshUser = std::getenv("SQLEDITOR_TEST_SSH_USER");
        const char* sshPassword = std::getenv("SQLEDITOR_TEST_SSH_PASSWORD");
        const char* sshKey = std::getenv("SQLEDITOR_TEST_SSH_KEY");
        const char* pgRemoteHost = std::getenv("SQLEDITOR_TEST_SSH_PG_REMOTE_HOST");

        if (!sshHost || !sshUser)
            return std::nullopt;

        cfg.sshHost = sshHost;
        if (sshPort && *sshPort != '\0')
            cfg.sshPort = std::stoi(sshPort);
        cfg.sshUser = sshUser;
        if (sshPassword)
            cfg.sshPassword = sshPassword;
        if (sshKey && *sshKey != '\0')
            cfg.sshKeyPath = sshKey;
        if (pgRemoteHost && *pgRemoteHost != '\0')
            cfg.pgRemoteHost = pgRemoteHost;
        else
            cfg.pgRemoteHost = "sqleditor-postgres-test";

        // PostgreSQL target (via Docker network)
        const char* pgDb = std::getenv("SQLEDITOR_TEST_PG_DB");
        const char* pgUser = std::getenv("SQLEDITOR_TEST_PG_USER");
        const char* pgPass = std::getenv("SQLEDITOR_TEST_PG_PASSWORD");

        cfg.pgDatabase = pgDb ? pgDb : "sqleditor_test";
        cfg.pgUser = pgUser ? pgUser : "postgres";
        if (pgPass)
            cfg.pgPassword = pgPass;
        cfg.pgRemotePort = 5432; // internal container port

        return cfg;
    }
} // namespace

class SSHTunnelTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto cfgOpt = loadSSHTunnelConfig();
        if (!cfgOpt.has_value())
            GTEST_SKIP() << "SSH tunnel test env vars not set "
                         << "(SQLEDITOR_TEST_SSH_HOST, SQLEDITOR_TEST_SSH_USER)";

        config = *cfgOpt;
    }

    SSHTunnelTestConfig config;
};

TEST_F(SSHTunnelTest, TunnelWithPrivateKeyConnectsToPostgres) {
    if (config.sshKeyPath.empty())
        GTEST_SKIP() << "SSH private key not available (SQLEDITOR_TEST_SSH_KEY not set)";

    DatabaseConnectionInfo connInfo;
    connInfo.name = "SSHTunnelKeyTest";
    connInfo.type = DatabaseType::POSTGRESQL;
    connInfo.host = config.pgRemoteHost;
    connInfo.port = config.pgRemotePort;
    connInfo.database = config.pgDatabase;
    connInfo.username = config.pgUser;
    connInfo.password = config.pgPassword;
    connInfo.sslmode = SslMode::Disable;

    connInfo.ssh.enabled = true;
    connInfo.ssh.host = config.sshHost;
    connInfo.ssh.port = config.sshPort;
    connInfo.ssh.username = config.sshUser;
    connInfo.ssh.authMethod = SSHAuthMethod::PrivateKey;
    connInfo.ssh.privateKeyPath = config.sshKeyPath;

    auto database = std::make_shared<PostgresDatabase>(connInfo);

    bool connected = false;
    std::string lastError;
    for (int attempt = 0; attempt < 15; ++attempt) {
        auto [ok, err] = database->connect();
        if (ok) {
            connected = true;
            break;
        }
        lastError = err;
        // auto-create DB if needed
        if (lastError.find("does not exist") != std::string::npos) {
            auto adminInfo = connInfo;
            adminInfo.database = "postgres";
            auto admin = std::make_shared<PostgresDatabase>(adminInfo);
            if (auto [aok, _] = admin->connect(); aok) {
                admin->createDatabase(config.pgDatabase);
                admin->disconnect();
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    ASSERT_TRUE(connected) << "SSH tunnel (key) + PostgreSQL connection failed: " << lastError;
    EXPECT_TRUE(database->isConnected());

    auto result = database->executeQuery("SELECT 1 AS tunnel_test");
    ASSERT_TRUE(result.success()) << result.errorMessage();
    ASSERT_FALSE(result.empty());
    EXPECT_EQ(result[0].tableData[0][0], "1");

    database->disconnect();
}

TEST_F(SSHTunnelTest, TunnelWithPasswordConnectsToPostgres) {
    if (config.sshPassword.empty())
        GTEST_SKIP() << "SSH password not available (SQLEDITOR_TEST_SSH_PASSWORD not set)";

    DatabaseConnectionInfo connInfo;
    connInfo.name = "SSHTunnelPassTest";
    connInfo.type = DatabaseType::POSTGRESQL;
    connInfo.host = config.pgRemoteHost;
    connInfo.port = config.pgRemotePort;
    connInfo.database = config.pgDatabase;
    connInfo.username = config.pgUser;
    connInfo.password = config.pgPassword;
    connInfo.sslmode = SslMode::Disable;

    connInfo.ssh.enabled = true;
    connInfo.ssh.host = config.sshHost;
    connInfo.ssh.port = config.sshPort;
    connInfo.ssh.username = config.sshUser;
    connInfo.ssh.authMethod = SSHAuthMethod::Password;
    connInfo.ssh.password = config.sshPassword;

    auto database = std::make_shared<PostgresDatabase>(connInfo);

    bool connected = false;
    std::string lastError;
    for (int attempt = 0; attempt < 15; ++attempt) {
        auto [ok, err] = database->connect();
        if (ok) {
            connected = true;
            break;
        }
        lastError = err;
        if (lastError.find("does not exist") != std::string::npos) {
            auto adminInfo = connInfo;
            adminInfo.database = "postgres";
            auto admin = std::make_shared<PostgresDatabase>(adminInfo);
            if (auto [aok, _] = admin->connect(); aok) {
                admin->createDatabase(config.pgDatabase);
                admin->disconnect();
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    ASSERT_TRUE(connected) << "SSH tunnel (password) + PostgreSQL connection failed: " << lastError;
    EXPECT_TRUE(database->isConnected());

    auto result = database->executeQuery("SELECT 1 AS tunnel_test");
    ASSERT_TRUE(result.success()) << result.errorMessage();
    ASSERT_FALSE(result.empty());
    EXPECT_EQ(result[0].tableData[0][0], "1");

    database->disconnect();
}

TEST_F(SSHTunnelTest, SSHTunnelClassStartsAndStops) {
    if (config.sshKeyPath.empty())
        GTEST_SKIP() << "SSH private key not available (SQLEDITOR_TEST_SSH_KEY not set)";

    SSHTunnel tunnel;

    SSHConfig ssh;
    ssh.enabled = true;
    ssh.host = config.sshHost;
    ssh.port = config.sshPort;
    ssh.username = config.sshUser;
    ssh.authMethod = SSHAuthMethod::PrivateKey;
    ssh.privateKeyPath = config.sshKeyPath;

    auto [ok, err] = tunnel.start(ssh, config.pgRemoteHost, config.pgRemotePort);
    ASSERT_TRUE(ok) << "SSH tunnel start failed: " << err;
    EXPECT_TRUE(tunnel.isRunning());
    EXPECT_GT(tunnel.localPort(), 0);

    tunnel.stop();
    EXPECT_FALSE(tunnel.isRunning());
}
