#include "database/cassandra/cassandra_database_node.hpp"
#include "database/cassandra.hpp"
#include "database/sql_builder.hpp"
#include <algorithm>
#include <cassandra.h>
#include <format>
#include <ranges>
#include <spdlog/spdlog.h>

namespace {

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
        return std::string(msg, len);
    }

    std::string getStringCol(const CassRow* row, const char* name) {
        const CassValue* v = cass_row_get_column_by_name(row, name);
        if (!v || cass_value_is_null(v))
            return "";
        const char* s = nullptr;
        size_t n = 0;
        cass_value_get_string(v, &s, &n);
        return std::string(s, n);
    }

    // CQL identifiers are case-sensitive when quoted; mixed-case keyspaces /
    // tables created from the connection dialog need quoting.
    std::string quoteId(const std::string& id) {
        std::string out;
        out.reserve(id.size() + 2);
        out += '"';
        for (char c : id) {
            if (c == '"')
                out += '"';
            out += c;
        }
        out += '"';
        return out;
    }

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

DatabaseInterface* CassandraDatabaseNode::ownerDatabase() const {
    return parentDb;
}

std::string CassandraDatabaseNode::getFullPath() const {
    return name;
}

QueryResult CassandraDatabaseNode::executeQuery(const std::string& sql, int limit) {
    if (!parentDb)
        return {};
    // Switch session to this keyspace via USE, then execute the user query.
    // Cassandra has no per-statement keyspace selector for arbitrary CQL.
    parentDb->executeQuery("USE " + quoteId(name), 1);
    return parentDb->executeQuery(sql, limit);
}

void CassandraDatabaseNode::startTablesLoadAsync(bool force) {
    if (!parentDb || tablesLoader.isRunning())
        return;
    if (force) {
        tables.clear();
        tablesLoaded = false;
        lastTablesError.clear();
    }
    if (!force && tablesLoaded)
        return;
    tablesLoader.start([this]() { return loadTablesSync(); });
}

void CassandraDatabaseNode::startViewsLoadAsync(bool force) {
    if (!parentDb || viewsLoader.isRunning())
        return;
    if (force) {
        views.clear();
        viewsLoaded = false;
        lastViewsError.clear();
    }
    if (!force && viewsLoaded)
        return;
    viewsLoader.start([this]() { return loadViewsSync(); });
}

void CassandraDatabaseNode::checkLoadingStatus() {
    tablesLoader.check([this](const std::vector<Table>& result) {
        tables = result;
        tablesLoaded = true;
    });
    viewsLoader.check([this](const std::vector<Table>& result) {
        views = result;
        viewsLoaded = true;
    });
}

void CassandraDatabaseNode::startTableRefreshAsync(const std::string& tableName) {
    auto& op = tableRefreshLoaders[tableName];
    if (op.isRunning())
        return;
    op.start([this, tableName]() { return refreshTableSync(tableName); });
}

bool CassandraDatabaseNode::isTableRefreshing(const std::string& tableName) const {
    auto it = tableRefreshLoaders.find(tableName);
    return it != tableRefreshLoaders.end() && it->second.isRunning();
}

void CassandraDatabaseNode::checkTableRefreshStatusAsync(const std::string& tableName) {
    auto it = tableRefreshLoaders.find(tableName);
    if (it == tableRefreshLoaders.end())
        return;
    it->second.check([this, &tableName](const Table& refreshed) {
        for (auto& t : tables) {
            if (t.name == tableName) {
                t = refreshed;
                break;
            }
        }
    });
}

std::vector<Table> CassandraDatabaseNode::loadTablesSync() {
    std::vector<Table> result;
    if (!parentDb || !parentDb->session())
        return result;

    const std::string cql =
        std::format("SELECT table_name FROM system_schema.tables WHERE keyspace_name = '{}'", name);
    auto [res, err] = runCql(parentDb->session(), cql);
    if (!res) {
        lastTablesError = err;
        spdlog::error("Cassandra loadTablesSync failed for {}: {}", name, err);
        return result;
    }

    auto it = makeIterator(cass_iterator_from_result(res.get()));
    while (cass_iterator_next(it.get())) {
        if (!tablesLoader.isRunning())
            break;
        const CassRow* row = cass_iterator_get_row(it.get());
        Table t;
        t.name = getStringCol(row, "table_name");
        t.schema = name;
        t.fullName = parentDb->getConnectionInfo().name + "." + name + "." + t.name;
        t.columns = loadColumns(t.name);
        result.push_back(std::move(t));
    }
    return result;
}

std::vector<Table> CassandraDatabaseNode::loadViewsSync() {
    std::vector<Table> result;
    if (!parentDb || !parentDb->session())
        return result;

    const std::string cql = std::format(
        "SELECT view_name, base_table_name FROM system_schema.views WHERE keyspace_name = '{}'",
        name);
    auto [res, err] = runCql(parentDb->session(), cql);
    if (!res) {
        lastViewsError = err;
        return result;
    }

    auto it = makeIterator(cass_iterator_from_result(res.get()));
    while (cass_iterator_next(it.get())) {
        if (!viewsLoader.isRunning())
            break;
        const CassRow* row = cass_iterator_get_row(it.get());
        Table v;
        v.name = getStringCol(row, "view_name");
        v.schema = name;
        v.fullName = parentDb->getConnectionInfo().name + "." + name + "." + v.name;
        v.definition = "Materialized view of " + getStringCol(row, "base_table_name");
        v.columns = loadColumns(v.name);
        result.push_back(std::move(v));
    }
    return result;
}

std::vector<Column> CassandraDatabaseNode::loadColumns(const std::string& tableName) {
    std::vector<Column> cols;
    if (!parentDb || !parentDb->session())
        return cols;

    const std::string cql =
        std::format("SELECT column_name, type, kind, position FROM system_schema.columns "
                    "WHERE keyspace_name = '{}' AND table_name = '{}'",
                    name, tableName);
    auto [res, err] = runCql(parentDb->session(), cql);
    if (!res)
        return cols;

    // system_schema.columns returns kind ∈ {partition_key, clustering, regular,
    // static}. partition_key + clustering form the primary key.
    struct Pending {
        Column col;
        std::string kind;
        int position = 0;
    };
    std::vector<Pending> pending;

    auto it = makeIterator(cass_iterator_from_result(res.get()));
    while (cass_iterator_next(it.get())) {
        const CassRow* row = cass_iterator_get_row(it.get());
        Pending p;
        p.col.name = getStringCol(row, "column_name");
        p.col.type = getStringCol(row, "type");
        p.kind = getStringCol(row, "kind");
        const CassValue* posVal = cass_row_get_column_by_name(row, "position");
        if (posVal && !cass_value_is_null(posVal)) {
            cass_int32_t v = 0;
            cass_value_get_int32(posVal, &v);
            p.position = v;
        }
        if (p.kind == "partition_key" || p.kind == "clustering") {
            p.col.isPrimaryKey = true;
            p.col.isNotNull = true;
        }
        pending.push_back(std::move(p));
    }

    // PK columns first (by position), then everything else alphabetically — the
    // sidebar reads these in display order.
    std::stable_sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
        const int aRank = (a.kind == "partition_key") ? 0 : (a.kind == "clustering") ? 1 : 2;
        const int bRank = (b.kind == "partition_key") ? 0 : (b.kind == "clustering") ? 1 : 2;
        if (aRank != bRank)
            return aRank < bRank;
        if (aRank < 2)
            return a.position < b.position;
        return a.col.name < b.col.name;
    });

    cols.reserve(pending.size());
    for (auto& p : pending)
        cols.push_back(std::move(p.col));
    return cols;
}

Table CassandraDatabaseNode::refreshTableSync(const std::string& tableName) {
    Table t;
    t.name = tableName;
    t.schema = name;
    if (parentDb)
        t.fullName = parentDb->getConnectionInfo().name + "." + name + "." + tableName;
    t.columns = loadColumns(tableName);
    return t;
}

std::vector<std::vector<std::string>>
CassandraDatabaseNode::getTableData(const Table& table, int limit, int offset,
                                    const std::string& whereClause,
                                    const std::string& orderByClause) {
    // Cassandra lacks OFFSET; for browsing we do a paged SELECT and ignore
    // offset (sidebar paging requests offset=0 first).
    (void)offset;
    std::vector<std::vector<std::string>> rows;
    if (!parentDb)
        return rows;

    std::string cql = "SELECT * FROM " + quoteId(name) + "." + quoteId(table.name);
    if (!whereClause.empty())
        cql += " WHERE " + whereClause + " ALLOW FILTERING";
    if (!orderByClause.empty())
        cql += " ORDER BY " + orderByClause;
    cql += " LIMIT " + std::to_string(limit > 0 ? limit : 1000);

    QueryResult r = parentDb->executeQuery(cql, limit);
    if (r.empty())
        return rows;
    return r[0].tableData;
}

std::vector<std::string> CassandraDatabaseNode::getColumnNames(const Table& table) {
    std::vector<std::string> names;
    names.reserve(table.columns.size());
    for (const auto& c : table.columns)
        names.push_back(c.name);
    return names;
}

int CassandraDatabaseNode::getRowCount(const Table& table, const std::string& whereClause) {
    if (!parentDb)
        return 0;
    std::string cql = "SELECT COUNT(*) FROM " + quoteId(name) + "." + quoteId(table.name);
    if (!whereClause.empty())
        cql += " WHERE " + whereClause + " ALLOW FILTERING";

    QueryResult r = parentDb->executeQuery(cql, 1);
    if (r.empty() || r[0].tableData.empty() || r[0].tableData[0].empty())
        return 0;
    try {
        return std::stoi(r[0].tableData[0][0]);
    } catch (...) {
        return 0;
    }
}

std::pair<bool, std::string> CassandraDatabaseNode::dropTable(const std::string& tableName) {
    if (!parentDb || !parentDb->session())
        return {false, "Not connected"};
    const auto builder = createSQLBuilder(getDatabaseType());
    auto [res, err] = runCql(parentDb->session(), builder->dropTable(name, tableName));
    if (!res)
        return {false, err};
    std::erase_if(tables, [&](const Table& t) { return t.name == tableName; });
    return {true, ""};
}

std::pair<bool, std::string> CassandraDatabaseNode::createTable(const Table& table) {
    if (!parentDb || !parentDb->session())
        return {false, "Not connected"};
    try {
        const auto builder = createSQLBuilder(DatabaseType::CASSANDRA);
        const std::string cql = builder->createTable(table, name);
        auto [res, err] = runCql(parentDb->session(), cql);
        if (!res)
            return {false, err};
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, std::string(e.what())};
    }
}
