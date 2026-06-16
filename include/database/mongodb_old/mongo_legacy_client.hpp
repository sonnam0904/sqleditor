#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// IPC client for sqleditor-mongo3-bridge (mongo-c-driver 1.x subprocess).
class MongoLegacyClient {
public:
    MongoLegacyClient() = default;
    ~MongoLegacyClient();

    MongoLegacyClient(const MongoLegacyClient&) = delete;
    MongoLegacyClient& operator=(const MongoLegacyClient&) = delete;

    bool ensureStarted(std::string& error);
    void shutdown();

    bool connect(const std::string& uri, std::string& error);
    void disconnect();

    [[nodiscard]] bool isBridgeRunning() const {
        return bridgeRunning_;
    }

    std::string buildInfo(const std::string& db, std::string& error);
    std::vector<std::string> listDatabases(std::string& error);
    std::vector<std::string> listCollections(const std::string& db, std::string& error);

    struct SchemaField {
        std::string name;
        std::string type;
    };
    std::vector<SchemaField> sampleSchema(const std::string& db, const std::string& collection,
                                          int limit, std::string& error);

    struct FindResult {
        std::vector<std::string> documentsJson;
    };
    FindResult find(const std::string& db, const std::string& collection,
                    const std::string& filterJson, int limit, int skip,
                    const std::string& sortJson, std::string& error);

    int64_t count(const std::string& db, const std::string& collection,
                  const std::string& filterJson, std::string& error);

    std::string runCommandJson(const std::string& db, const std::string& commandJson,
                               std::string& error);

    FindResult aggregate(const std::string& db, const std::string& collection,
                         const std::string& pipelineJson, int limit, std::string& error);

private:
    mutable std::mutex mutex_;
    bool bridgeRunning_ = false;
    int nextId_ = 1;

#if defined(_WIN32)
    void* childProcess_ = nullptr;
    void* stdinWrite_ = nullptr;
    void* stdoutRead_ = nullptr;
#else
    int childPid_ = -1;
    int stdinWrite_ = -1;
    int stdoutRead_ = -1;
#endif

    static std::filesystem::path bridgeExecutablePath();

    bool startBridgeProcess(std::string& error);
    void stopBridgeProcess();

    bool writeLine(const std::string& line, std::string& error);
    bool readLine(std::string& line, std::string& error);

    struct BridgeResponse {
        bool ok = false;
        std::string error;
        std::string rawJson;
    };
    BridgeResponse sendRequest(const std::string& requestJson, std::string& error);
};
