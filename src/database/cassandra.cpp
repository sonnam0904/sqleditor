#include "database/cassandra.hpp"
#include "database/ssl_config.hpp"
#include <chrono>
#include <format>
#include <fstream>
#include <ranges>
#include <spdlog/spdlog.h>
#include <sstream>

namespace {

    // RAII wrappers around CassFuture* / CassResult* / CassStatement*. The
    // cpp-driver returns raw C handles that must be freed; using a unique_ptr
    // with a custom deleter keeps the call sites linear.
    using FuturePtr = std::unique_ptr<CassFuture, decltype(&cass_future_free)>;
    using ResultPtr = std::unique_ptr<const CassResult, decltype(&cass_result_free)>;
    using StatementPtr = std::unique_ptr<CassStatement, decltype(&cass_statement_free)>;
    using IteratorPtr = std::unique_ptr<CassIterator, decltype(&cass_iterator_free)>;

    FuturePtr makeFuture(CassFuture* f) {
        return {f, cass_future_free};
    }
    ResultPtr makeResult(const CassResult* r) {
        return {r, cass_result_free};
    }
    StatementPtr makeStatement(CassStatement* s) {
        return {s, cass_statement_free};
    }
    IteratorPtr makeIterator(CassIterator* i) {
        return {i, cass_iterator_free};
    }

    std::string futureError(CassFuture* future) {
        const char* msg = nullptr;
        size_t len = 0;
        cass_future_error_message(future, &msg, &len);
        return std::string{msg, len};
    }

    std::string cassValueToString(const CassValue* value) {
        if (cass_value_is_null(value)) {
            return std::string{NULL_SENTINEL};
        }
        const auto type = cass_value_type(value);
        switch (type) {
        case CASS_VALUE_TYPE_VARCHAR:
        case CASS_VALUE_TYPE_ASCII:
        case CASS_VALUE_TYPE_TEXT: {
            const char* s = nullptr;
            size_t n = 0;
            cass_value_get_string(value, &s, &n);
            return std::string(s, n);
        }
        case CASS_VALUE_TYPE_INT: {
            cass_int32_t v = 0;
            cass_value_get_int32(value, &v);
            return std::to_string(v);
        }
        case CASS_VALUE_TYPE_BIGINT:
        case CASS_VALUE_TYPE_COUNTER: {
            cass_int64_t v = 0;
            cass_value_get_int64(value, &v);
            return std::to_string(v);
        }
        case CASS_VALUE_TYPE_SMALL_INT: {
            cass_int16_t v = 0;
            cass_value_get_int16(value, &v);
            return std::to_string(v);
        }
        case CASS_VALUE_TYPE_TINY_INT: {
            cass_int8_t v = 0;
            cass_value_get_int8(value, &v);
            return std::to_string(static_cast<int>(v));
        }
        case CASS_VALUE_TYPE_BOOLEAN: {
            cass_bool_t v = cass_false;
            cass_value_get_bool(value, &v);
            return v ? std::string{BOOL_TRUE_SENTINEL} : std::string{BOOL_FALSE_SENTINEL};
        }
        case CASS_VALUE_TYPE_FLOAT: {
            cass_float_t v = 0;
            cass_value_get_float(value, &v);
            return std::to_string(v);
        }
        case CASS_VALUE_TYPE_DOUBLE: {
            cass_double_t v = 0;
            cass_value_get_double(value, &v);
            return std::to_string(v);
        }
        case CASS_VALUE_TYPE_UUID:
        case CASS_VALUE_TYPE_TIMEUUID: {
            CassUuid uuid;
            cass_value_get_uuid(value, &uuid);
            char buf[CASS_UUID_STRING_LENGTH];
            cass_uuid_string(uuid, buf);
            return std::string(buf);
        }
        case CASS_VALUE_TYPE_INET: {
            CassInet inet;
            cass_value_get_inet(value, &inet);
            char buf[CASS_INET_STRING_LENGTH];
            cass_inet_string(inet, buf);
            return std::string(buf);
        }
        case CASS_VALUE_TYPE_TIMESTAMP: {
            cass_int64_t millis = 0;
            cass_value_get_int64(value, &millis);
            const auto seconds = millis / 1000;
            const auto t = static_cast<std::time_t>(seconds);
            std::tm tm{};
#ifdef _WIN32
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
            return std::string(buf);
        }
        default:
            return "<unsupported>";
        }
    }

    // Cassandra column names from system_schema are always ASCII; this helper
    // pulls a string column from a CassRow by index.
    std::string getStringCol(const CassRow* row, size_t idx) {
        const CassValue* v = cass_row_get_column(row, idx);
        if (!v || cass_value_is_null(v))
            return "";
        const char* s = nullptr;
        size_t n = 0;
        cass_value_get_string(v, &s, &n);
        return std::string(s, n);
    }

    // Run a CQL statement on the supplied session, blocking. Returns
    // (result, errorMessage) — result is null on error.
    std::pair<ResultPtr, std::string> runCql(CassSession* session, const std::string& cql) {
        auto stmt = makeStatement(cass_statement_new(cql.c_str(), 0));
        auto fut = makeFuture(cass_session_execute(session, stmt.get()));
        cass_future_wait(fut.get());
        if (cass_future_error_code(fut.get()) != CASS_OK) {
            return {ResultPtr{nullptr, cass_result_free}, futureError(fut.get())};
        }
        return {makeResult(cass_future_get_result(fut.get())), ""};
    }

} // namespace

CassandraDatabase::CassandraDatabase(const DatabaseConnectionInfo& connInfo) {
    this->connectionInfo = connInfo;
    if (connectionInfo.port == 0 || connectionInfo.port == 5432) {
        connectionInfo.port = 9042;
    }
    spdlog::debug("Creating CassandraDatabase host={} port={} showAll={}", connectionInfo.host,
                  connectionInfo.port, connInfo.showAllDatabases);
}

CassandraDatabase::~CassandraDatabase() {
    databasesLoader.cancel();
    refreshWorkflow.cancel();
    for (auto& node : databaseDataCache | std::views::values) {
        if (node) {
            node->tablesLoader.cancel();
            node->viewsLoader.cancel();
        }
    }
    disconnect();
}

void CassandraDatabase::freeDriverState() {
    if (session_) {
        auto fut = makeFuture(cass_session_close(session_));
        cass_future_wait(fut.get());
        cass_session_free(session_);
        session_ = nullptr;
    }
    if (cluster_) {
        cass_cluster_free(cluster_);
        cluster_ = nullptr;
    }
    if (ssl_) {
        cass_ssl_free(ssl_);
        ssl_ = nullptr;
    }
}

std::pair<bool, std::string> CassandraDatabase::applySslConfig() {
    if (connectionInfo.sslmode == SslMode::Disable) {
        return {true, ""};
    }
    ssl_ = cass_ssl_new();

    if (connectionInfo.sslmode == SslMode::VerifyCA && !connectionInfo.sslCACertPath.empty()) {
        std::ifstream f(connectionInfo.sslCACertPath, std::ios::binary);
        if (!f) {
            return {false, "Cannot read CA cert: " + connectionInfo.sslCACertPath};
        }
        std::ostringstream pem;
        pem << f.rdbuf();
        const std::string s = pem.str();
        if (cass_ssl_add_trusted_cert_n(ssl_, s.data(), s.size()) != CASS_OK) {
            return {false, "Failed to add CA cert to SSL context"};
        }
        cass_ssl_set_verify_flags(ssl_, CASS_SSL_VERIFY_PEER_CERT);
    } else {
        // require / no CA: encrypt only, do not verify
        cass_ssl_set_verify_flags(ssl_, CASS_SSL_VERIFY_NONE);
    }

    cass_cluster_set_ssl(cluster_, ssl_);
    return {true, ""};
}

std::pair<bool, std::string> CassandraDatabase::connect() {
    if (connected) {
        return {true, ""};
    }

    setAttemptedConnection(true);
    auto [prepOk, prepErr] = prepareConnectionForConnect();
    if (!prepOk) {
        connected = false;
        setLastConnectionError(prepErr);
        return {false, prepErr};
    }

    std::lock_guard lock(sessionMutex_);
    freeDriverState();

    cluster_ = cass_cluster_new();
    cass_cluster_set_contact_points(cluster_, connectionInfo.host.c_str());
    cass_cluster_set_port(cluster_, connectionInfo.port);
    if (!connectionInfo.username.empty()) {
        cass_cluster_set_credentials(cluster_, connectionInfo.username.c_str(),
                                     connectionInfo.password.c_str());
    }
    cass_cluster_set_connect_timeout(cluster_, 10000);
    cass_cluster_set_request_timeout(cluster_, 30000);

    if (auto [ok, err] = applySslConfig(); !ok) {
        freeDriverState();
        connected = false;
        setLastConnectionError(err);
        return {false, err};
    }

    session_ = cass_session_new();
    auto fut = makeFuture(cass_session_connect(session_, cluster_));
    cass_future_wait(fut.get());
    if (cass_future_error_code(fut.get()) != CASS_OK) {
        std::string err = "Cassandra connect failed: " + futureError(fut.get());
        freeDriverState();
        connected = false;
        setLastConnectionError(err);
        return {false, err};
    }

    connected = true;
    setLastConnectionError("");
    spdlog::debug("Connected to Cassandra {}:{}", connectionInfo.host, connectionInfo.port);

    if (connectionInfo.showAllDatabases && !databasesLoaded && !databasesLoader.isRunning()) {
        refreshDatabaseNames();
    }
    return {true, ""};
}

void CassandraDatabase::disconnect() {
    std::lock_guard lock(sessionMutex_);
    freeDriverState();
    stopSshTunnel();
    connected = false;
}

void CassandraDatabase::refreshConnection() {
    refreshWorkflow.start([this]() -> bool {
        disconnect();
        setAttemptedConnection(false);
        setLastConnectionError("");
        auto [ok, err] = connect();
        if (!ok) {
            setLastConnectionError(err);
            return false;
        }
        if (connectionInfo.showAllDatabases) {
            auto names = getDatabaseNamesAsync();
            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames = std::move(names);
        } else {
            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames.clear();
        }
        return true;
    });
}

QueryResult CassandraDatabase::executeQuery(const std::string& query, int rowLimit) {
    QueryResult out;
    StatementResult s;
    const auto t0 = std::chrono::high_resolution_clock::now();

    if (!connect().first) {
        s.success = false;
        s.errorMessage = "Not connected to database";
        out.statements.push_back(std::move(s));
        return out;
    }

    auto stmt = makeStatement(cass_statement_new(query.c_str(), 0));
    cass_statement_set_paging_size(stmt.get(), rowLimit > 0 ? rowLimit : 1000);

    auto fut = makeFuture(cass_session_execute(session_, stmt.get()));
    cass_future_wait(fut.get());
    if (cass_future_error_code(fut.get()) != CASS_OK) {
        s.success = false;
        s.errorMessage = futureError(fut.get());
        out.statements.push_back(std::move(s));
        const auto t1 = std::chrono::high_resolution_clock::now();
        out.executionTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        return out;
    }

    auto result = makeResult(cass_future_get_result(fut.get()));
    const size_t cols = cass_result_column_count(result.get());

    if (cols == 0) {
        // INSERT/UPDATE/DELETE/DDL — Cassandra reports no row count, just OK.
        s.message = "OK";
        out.statements.push_back(std::move(s));
        const auto t1 = std::chrono::high_resolution_clock::now();
        out.executionTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        return out;
    }

    s.columnNames.reserve(cols);
    for (size_t i = 0; i < cols; ++i) {
        const char* name = nullptr;
        size_t nlen = 0;
        cass_result_column_name(result.get(), i, &name, &nlen);
        s.columnNames.emplace_back(name, nlen);
    }

    auto it = makeIterator(cass_iterator_from_result(result.get()));
    int taken = 0;
    while (cass_iterator_next(it.get()) && (rowLimit <= 0 || taken < rowLimit)) {
        const CassRow* row = cass_iterator_get_row(it.get());
        std::vector<std::string> r;
        r.reserve(cols);
        for (size_t i = 0; i < cols; ++i) {
            r.push_back(cassValueToString(cass_row_get_column(row, i)));
        }
        s.tableData.push_back(std::move(r));
        ++taken;
    }
    s.message =
        std::format("Returned {} row{}", s.tableData.size(), s.tableData.size() == 1 ? "" : "s");
    out.statements.push_back(std::move(s));
    const auto t1 = std::chrono::high_resolution_clock::now();
    out.executionTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return out;
}

CassandraDatabaseNode* CassandraDatabase::getDatabaseData(const std::string& keyspace) {
    auto it = databaseDataCache.find(keyspace);
    if (it == databaseDataCache.end()) {
        auto node = std::make_unique<CassandraDatabaseNode>();
        node->name = keyspace;
        node->parentDb = this;
        auto* ptr = node.get();
        databaseDataCache[keyspace] = std::move(node);
        return ptr;
    }
    return it->second.get();
}

std::unordered_map<std::string, std::unique_ptr<CassandraDatabaseNode>>&
CassandraDatabase::getDatabaseDataMap() {
    if (!databasesLoaded && !databasesLoader.isRunning() && isConnected()) {
        refreshDatabaseNames();
    }
    return databaseDataCache;
}

void CassandraDatabase::refreshDatabaseNames() {
    if (databasesLoader.isRunning()) {
        return;
    }
    databasesLoaded = false;
    databasesLoader.start([this]() { return getDatabaseNamesAsync(); });
}

bool CassandraDatabase::isLoadingDatabases() const {
    return databasesLoader.isRunning();
}

bool CassandraDatabase::hasPendingAsyncWork() const {
    if (isConnecting() || isLoadingDatabases())
        return true;
    for (const auto& [_, node] : databaseDataCache) {
        if (node && (node->tablesLoader.isRunning() || node->viewsLoader.isRunning()))
            return true;
    }
    return false;
}

void CassandraDatabase::checkDatabasesStatusAsync() {
    databasesLoader.check([this](const std::vector<std::string>& names) {
        for (const auto& n : names) {
            getDatabaseData(n);
        }
        databasesLoaded = true;
    });
}

void CassandraDatabase::checkRefreshWorkflowAsync() {
    refreshWorkflow.check([this](const bool ok) {
        if (!ok) {
            spdlog::error("Cassandra refresh workflow failed");
            return;
        }
        std::vector<std::string> refreshed;
        {
            std::lock_guard lock(refreshStateMutex);
            refreshed = std::move(pendingRefreshDatabaseNames);
            pendingRefreshDatabaseNames.clear();
        }
        for (const auto& n : refreshed) {
            getDatabaseData(n);
        }
        databasesLoaded = true;
        for (auto& [_, node] : databaseDataCache) {
            if (node)
                node->startTablesLoadAsync(true);
        }
    });
}

std::vector<std::string> CassandraDatabase::getDatabaseNamesAsync() const {
    std::vector<std::string> result;
    if (!isConnected() || !session_) {
        return result;
    }
    if (!connectionInfo.showAllDatabases) {
        if (!connectionInfo.database.empty())
            result.push_back(connectionInfo.database);
        return result;
    }

    auto [res, err] = runCql(session_, "SELECT keyspace_name FROM system_schema.keyspaces");
    if (!res) {
        spdlog::error("List keyspaces failed: {}", err);
        return result;
    }
    auto it = makeIterator(cass_iterator_from_result(res.get()));
    while (cass_iterator_next(it.get())) {
        const CassRow* row = cass_iterator_get_row(it.get());
        std::string ks = getStringCol(row, 0);
        // Hide internal system keyspaces by default; user can still query them.
        if (ks.starts_with("system"))
            continue;
        result.push_back(std::move(ks));
    }
    return result;
}

std::pair<bool, std::string> CassandraDatabase::createDatabase(const std::string& dbName,
                                                               const std::string& /*comment*/) {
    if (!isConnected())
        return {false, "Not connected"};
    // SimpleStrategy / RF=1 is the safe default for a single-node cluster; users
    // can craft their own CREATE KEYSPACE via the editor for production setups.
    const std::string cql =
        "CREATE KEYSPACE IF NOT EXISTS \"" + dbName +
        "\" WITH replication = {'class':'SimpleStrategy','replication_factor':1}";
    auto [res, err] = runCql(session_, cql);
    if (!res)
        return {false, err};
    getDatabaseData(dbName);
    return {true, ""};
}

std::pair<bool, std::string> CassandraDatabase::dropDatabase(const std::string& dbName) {
    if (!isConnected())
        return {false, "Not connected"};
    auto [res, err] = runCql(session_, "DROP KEYSPACE IF EXISTS \"" + dbName + "\"");
    if (!res)
        return {false, err};
    databaseDataCache.erase(dbName);
    return {true, ""};
}
