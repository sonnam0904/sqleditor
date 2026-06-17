// Subprocess bridge: links mongo-c-driver 1.x only. Communicates via JSON lines on stdin/stdout.
#include <bson/bson.h>
#include <mongoc/mongoc.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

mongoc_client_t* g_client = nullptr;

void writeResponse(const nlohmann::json& response) {
    std::cout << response.dump() << '\n';
    std::cout.flush();
}

std::string bsonTypeLabel(bson_type_t type) {
    switch (type) {
    case BSON_TYPE_DOUBLE:
        return "double";
    case BSON_TYPE_UTF8:
        return "string";
    case BSON_TYPE_DOCUMENT:
        return "object";
    case BSON_TYPE_ARRAY:
        return "array";
    case BSON_TYPE_BINARY:
        return "binary";
    case BSON_TYPE_OID:
        return "objectId";
    case BSON_TYPE_BOOL:
        return "bool";
    case BSON_TYPE_DATE_TIME:
        return "date";
    case BSON_TYPE_NULL:
        return "null";
    case BSON_TYPE_INT32:
        return "int32";
    case BSON_TYPE_INT64:
        return "int64";
    case BSON_TYPE_DECIMAL128:
        return "decimal128";
    default:
        return "unknown";
    }
}

bool parseBsonFromJson(const std::string& json, bson_t* out, std::string& error) {
    bson_error_t bsonError{};
    if (!bson_init_from_json(out, json.c_str(), json.size(), &bsonError)) {
        error = bsonError.message;
        return false;
    }
    return true;
}

mongoc_database_t* openDatabase(const std::string& dbName, std::string& error) {
    if (!g_client) {
        error = "Not connected";
        return nullptr;
    }
    return mongoc_client_get_database(g_client, dbName.c_str());
}

mongoc_collection_t* openCollection(const std::string& dbName, const std::string& collName,
                                    std::string& error) {
    if (!g_client) {
        error = "Not connected";
        return nullptr;
    }
    return mongoc_client_get_collection(g_client, dbName.c_str(), collName.c_str());
}

void handleConnect(const nlohmann::json& req, nlohmann::json& resp) {
    if (!req.contains("uri") || !req["uri"].is_string()) {
        resp["ok"] = false;
        resp["error"] = "connect requires uri";
        return;
    }

    if (g_client) {
        mongoc_client_destroy(g_client);
        g_client = nullptr;
    }

    bson_error_t error{};
    const std::string uri = req["uri"].get<std::string>();
    g_client = mongoc_client_new(uri.c_str());
    if (!g_client) {
        resp["ok"] = false;
        resp["error"] = "mongoc_client_new failed";
        return;
    }

    bson_t reply{};
    bson_t cmd = BSON_INITIALIZER;
    BSON_APPEND_INT32(&cmd, "ping", 1);
    const bool ok =
        mongoc_client_command_simple(g_client, "admin", &cmd, nullptr, &reply, &error);
    bson_destroy(&cmd);
    bson_destroy(&reply);

    if (!ok) {
        mongoc_client_destroy(g_client);
        g_client = nullptr;
        resp["ok"] = false;
        resp["error"] = error.message;
        return;
    }

    resp["ok"] = true;
}

void handleDisconnect(nlohmann::json& resp) {
    if (g_client) {
        mongoc_client_destroy(g_client);
        g_client = nullptr;
    }
    resp["ok"] = true;
}

void handleBuildInfo(const nlohmann::json& req, nlohmann::json& resp) {
    std::string dbName = "admin";
    if (req.contains("db") && req["db"].is_string()) {
        dbName = req["db"].get<std::string>();
    }

    if (!g_client) {
        resp["ok"] = false;
        resp["error"] = "Not connected";
        return;
    }

    bson_t cmd = BSON_INITIALIZER;
    BSON_APPEND_INT32(&cmd, "buildInfo", 1);
    bson_t reply{};
    bson_error_t error{};
    const bool ok = mongoc_client_command_simple(g_client, dbName.c_str(), &cmd, nullptr, &reply,
                                                 &error);
    bson_destroy(&cmd);

    if (!ok) {
        resp["ok"] = false;
        resp["error"] = error.message;
        bson_destroy(&reply);
        return;
    }

    bson_iter_t iter{};
    if (bson_iter_init_find(&iter, &reply, "version") && BSON_ITER_HOLDS_UTF8(&iter)) {
        resp["ok"] = true;
        resp["version"] = std::string(bson_iter_utf8(&iter, nullptr));
    } else {
        resp["ok"] = false;
        resp["error"] = "buildInfo missing version";
    }
    bson_destroy(&reply);
}

void handleListDatabases(nlohmann::json& resp) {
    if (!g_client) {
        resp["ok"] = false;
        resp["error"] = "Not connected";
        return;
    }

    bson_t cmd = BSON_INITIALIZER;
    BSON_APPEND_INT32(&cmd, "listDatabases", 1);
    bson_t reply{};
    bson_error_t error{};
    const bool ok =
        mongoc_client_command_simple(g_client, "admin", &cmd, nullptr, &reply, &error);
    bson_destroy(&cmd);

    if (!ok) {
        resp["ok"] = false;
        resp["error"] = error.message;
        return;
    }

    nlohmann::json names = nlohmann::json::array();
    bson_iter_t outer{};
    if (bson_iter_init_find(&outer, &reply, "databases") && BSON_ITER_HOLDS_ARRAY(&outer)) {
        const uint8_t* data = nullptr;
        uint32_t len = 0;
        bson_iter_array(&outer, &len, &data);
        bson_t arr{};
        bson_init_static(&arr, data, len);
        bson_iter_t dbIter{};
        if (bson_iter_init(&dbIter, &arr)) {
            while (bson_iter_next(&dbIter)) {
                if (!BSON_ITER_HOLDS_DOCUMENT(&dbIter)) {
                    continue;
                }
                uint32_t docLen = 0;
                const uint8_t* docData = nullptr;
                bson_iter_document(&dbIter, &docLen, &docData);
                bson_t dbDoc{};
                bson_init_static(&dbDoc, docData, docLen);
                bson_iter_t nameIter{};
                if (bson_iter_init_find(&nameIter, &dbDoc, "name") &&
                    BSON_ITER_HOLDS_UTF8(&nameIter)) {
                    names.push_back(std::string(bson_iter_utf8(&nameIter, nullptr)));
                }
                bson_destroy(&dbDoc);
            }
        }
        bson_destroy(&arr);
    }
    bson_destroy(&reply);

    resp["ok"] = true;
    resp["databases"] = std::move(names);
}

void handleListCollections(const nlohmann::json& req, nlohmann::json& resp) {
    if (!req.contains("db") || !req["db"].is_string()) {
        resp["ok"] = false;
        resp["error"] = "list_collections requires db";
        return;
    }

    std::string error;
    mongoc_database_t* db = openDatabase(req["db"].get<std::string>(), error);
    if (!db) {
        resp["ok"] = false;
        resp["error"] = error;
        return;
    }

    bson_error_t bsonError{};
    char** names = mongoc_database_get_collection_names_with_opts(db, nullptr, &bsonError);
    mongoc_database_destroy(db);

    if (!names) {
        resp["ok"] = false;
        resp["error"] = bsonError.message;
        return;
    }

    nlohmann::json collNames = nlohmann::json::array();
    for (char** it = names; *it; ++it) {
        collNames.push_back(std::string(*it));
    }
    bson_strfreev(names);

    resp["ok"] = true;
    resp["collections"] = std::move(collNames);
}

void collectSchemaFromDocument(const bson_t* doc, std::unordered_map<std::string, std::string>& fields) {
    bson_iter_t iter{};
    if (!bson_iter_init(&iter, doc)) {
        return;
    }
    while (bson_iter_next(&iter)) {
        const char* key = bson_iter_key(&iter);
        const auto label = bsonTypeLabel(bson_iter_type(&iter));
        auto [itField, inserted] = fields.emplace(key, label);
        if (!inserted && itField->second != label) {
            itField->second = "mixed";
        }
    }
}

void handleSampleSchema(const nlohmann::json& req, nlohmann::json& resp) {
    if (!req.contains("db") || !req.contains("collection")) {
        resp["ok"] = false;
        resp["error"] = "sample_schema requires db and collection";
        return;
    }

    const std::string dbName = req["db"].get<std::string>();
    const std::string collName = req["collection"].get<std::string>();
    const int limit = req.value("limit", 100);

    std::string error;
    mongoc_collection_t* coll = openCollection(dbName, collName, error);
    if (!coll) {
        resp["ok"] = false;
        resp["error"] = error;
        return;
    }

    bson_t query = BSON_INITIALIZER;
    bson_t opts = BSON_INITIALIZER;
    BSON_APPEND_INT32(&opts, "limit", limit);

    mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(coll, &query, &opts, nullptr);
    bson_destroy(&query);
    bson_destroy(&opts);

    std::unordered_map<std::string, std::string> fields;
    const bson_t* doc = nullptr;
    while (mongoc_cursor_next(cursor, &doc)) {
        collectSchemaFromDocument(doc, fields);
    }

    bson_error_t cursorError{};
    if (mongoc_cursor_error(cursor, &cursorError)) {
        resp["ok"] = false;
        resp["error"] = cursorError.message;
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(coll);
        return;
    }

    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(coll);

    nlohmann::json fieldArr = nlohmann::json::array();
    for (const auto& [name, type] : fields) {
        nlohmann::json field;
        field["name"] = name;
        field["type"] = type;
        fieldArr.push_back(std::move(field));
    }

    resp["ok"] = true;
    resp["fields"] = std::move(fieldArr);
}

void handleFind(const nlohmann::json& req, nlohmann::json& resp) {
    if (!req.contains("db") || !req.contains("collection")) {
        resp["ok"] = false;
        resp["error"] = "find requires db and collection";
        return;
    }

    const std::string dbName = req["db"].get<std::string>();
    const std::string collName = req["collection"].get<std::string>();
    const std::string filterJson = req.value("filter", "{}");
    const int limit = req.value("limit", 1000);
    const int skip = req.value("skip", 0);
    const std::string sortJson = req.value("sort", "");

    std::string error;
    mongoc_collection_t* coll = openCollection(dbName, collName, error);
    if (!coll) {
        resp["ok"] = false;
        resp["error"] = error;
        return;
    }

    bson_t query{};
    if (!parseBsonFromJson(filterJson, &query, error)) {
        mongoc_collection_destroy(coll);
        resp["ok"] = false;
        resp["error"] = error;
        return;
    }

    bson_t opts = BSON_INITIALIZER;
    BSON_APPEND_INT32(&opts, "limit", limit);
    if (skip > 0) {
        BSON_APPEND_INT32(&opts, "skip", skip);
    }
    if (!sortJson.empty()) {
        bson_t sort{};
        if (!parseBsonFromJson(sortJson, &sort, error)) {
            bson_destroy(&query);
            bson_destroy(&opts);
            mongoc_collection_destroy(coll);
            resp["ok"] = false;
            resp["error"] = error;
            return;
        }
        BSON_APPEND_DOCUMENT(&opts, "sort", &sort);
        bson_destroy(&sort);
    }

    mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(coll, &query, &opts, nullptr);
    bson_destroy(&query);
    bson_destroy(&opts);

    nlohmann::json docs = nlohmann::json::array();
    const bson_t* doc = nullptr;
    while (mongoc_cursor_next(cursor, &doc)) {
        char* json = bson_as_relaxed_extended_json(doc, nullptr);
        if (json) {
            docs.push_back(std::string(json));
            bson_free(json);
        }
    }

    bson_error_t cursorError{};
    if (mongoc_cursor_error(cursor, &cursorError)) {
        resp["ok"] = false;
        resp["error"] = cursorError.message;
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(coll);
        return;
    }

    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(coll);

    resp["ok"] = true;
    resp["documents"] = std::move(docs);
}

void handleCount(const nlohmann::json& req, nlohmann::json& resp) {
    if (!req.contains("db") || !req.contains("collection")) {
        resp["ok"] = false;
        resp["error"] = "count requires db and collection";
        return;
    }

    const std::string dbName = req["db"].get<std::string>();
    const std::string collName = req["collection"].get<std::string>();
    const std::string filterJson = req.value("filter", "{}");

    std::string error;
    mongoc_collection_t* coll = openCollection(dbName, collName, error);
    if (!coll) {
        resp["ok"] = false;
        resp["error"] = error;
        return;
    }

    bson_t query{};
    if (!parseBsonFromJson(filterJson, &query, error)) {
        mongoc_collection_destroy(coll);
        resp["ok"] = false;
        resp["error"] = error;
        return;
    }

    bson_error_t bsonError{};
    const int64_t count = mongoc_collection_count_documents(coll, &query, nullptr, nullptr, nullptr,
                                                            &bsonError);
    bson_destroy(&query);
    mongoc_collection_destroy(coll);

    if (count < 0) {
        resp["ok"] = false;
        resp["error"] = bsonError.message;
        return;
    }

    resp["ok"] = true;
    resp["count"] = count;
}

void handleEstimatedCount(const nlohmann::json& req, nlohmann::json& resp) {
    if (!req.contains("db") || !req.contains("collection")) {
        resp["ok"] = false;
        resp["error"] = "estimated_count requires db and collection";
        return;
    }

    const std::string dbName = req["db"].get<std::string>();
    const std::string collName = req["collection"].get<std::string>();

    std::string error;
    mongoc_collection_t* coll = openCollection(dbName, collName, error);
    if (!coll) {
        resp["ok"] = false;
        resp["error"] = error;
        return;
    }

    bson_error_t bsonError{};
    const int64_t count = mongoc_collection_estimated_document_count(coll, nullptr, nullptr,
                                                                     nullptr, &bsonError);
    mongoc_collection_destroy(coll);

    if (count < 0) {
        resp["ok"] = false;
        resp["error"] = bsonError.message;
        return;
    }

    resp["ok"] = true;
    resp["count"] = count;
}

void handleAggregate(const nlohmann::json& req, nlohmann::json& resp) {
    if (!req.contains("db") || !req.contains("collection")) {
        resp["ok"] = false;
        resp["error"] = "aggregate requires db and collection";
        return;
    }

    const std::string dbName = req["db"].get<std::string>();
    const std::string collName = req["collection"].get<std::string>();
    const std::string pipelineJson = req.value("pipeline", "[]");
    const int limit = req.value("limit", 1000);

    std::string error;
    mongoc_collection_t* coll = openCollection(dbName, collName, error);
    if (!coll) {
        resp["ok"] = false;
        resp["error"] = error;
        return;
    }

    bson_t pipeline{};
    if (!parseBsonFromJson(pipelineJson, &pipeline, error)) {
        mongoc_collection_destroy(coll);
        resp["ok"] = false;
        resp["error"] = error;
        return;
    }

    mongoc_cursor_t* cursor =
        mongoc_collection_aggregate(coll, MONGOC_QUERY_NONE, &pipeline, nullptr, nullptr);
    bson_destroy(&pipeline);
    mongoc_collection_destroy(coll);

    if (!cursor) {
        resp["ok"] = false;
        resp["error"] = "aggregate failed to create cursor";
        return;
    }

    nlohmann::json docs = nlohmann::json::array();
    const bson_t* doc = nullptr;
    int count = 0;
    while (mongoc_cursor_next(cursor, &doc) && count < limit) {
        char* json = bson_as_relaxed_extended_json(doc, nullptr);
        if (json) {
            docs.push_back(std::string(json));
            bson_free(json);
            ++count;
        }
    }

    bson_error_t cursorError{};
    if (mongoc_cursor_error(cursor, &cursorError)) {
        resp["ok"] = false;
        resp["error"] = cursorError.message;
        mongoc_cursor_destroy(cursor);
        return;
    }

    mongoc_cursor_destroy(cursor);
    resp["ok"] = true;
    resp["documents"] = std::move(docs);
}

void handleRunCommand(const nlohmann::json& req, nlohmann::json& resp) {
    if (!req.contains("db") || !req.contains("command")) {
        resp["ok"] = false;
        resp["error"] = "run_command requires db and command";
        return;
    }

    const std::string dbName = req["db"].get<std::string>();
    const std::string commandJson = req["command"].get<std::string>();

    if (!g_client) {
        resp["ok"] = false;
        resp["error"] = "Not connected";
        return;
    }

    bson_t cmd{};
    std::string error;
    if (!parseBsonFromJson(commandJson, &cmd, error)) {
        resp["ok"] = false;
        resp["error"] = error;
        return;
    }

    bson_t reply{};
    bson_error_t bsonError{};
    const bool ok =
        mongoc_client_command_simple(g_client, dbName.c_str(), &cmd, nullptr, &reply, &bsonError);
    bson_destroy(&cmd);

    if (!ok) {
        resp["ok"] = false;
        resp["error"] = bsonError.message;
        bson_destroy(&reply);
        return;
    }

    char* json = bson_as_relaxed_extended_json(&reply, nullptr);
    bson_destroy(&reply);
    resp["ok"] = true;
    resp["result"] = json ? std::string(json) : "{}";
    if (json) {
        bson_free(json);
    }
}

void handlePing(nlohmann::json& resp) {
    resp["ok"] = true;
    resp["pong"] = true;
}

void dispatch(const nlohmann::json& req) {
    nlohmann::json resp;
    if (req.contains("id")) {
        resp["id"] = req["id"];
    }

    const std::string op = req.value("op", "");
    if (op == "ping") {
        handlePing(resp);
    } else if (op == "connect") {
        handleConnect(req, resp);
    } else if (op == "disconnect") {
        handleDisconnect(resp);
    } else if (op == "build_info") {
        handleBuildInfo(req, resp);
    } else if (op == "list_databases") {
        handleListDatabases(resp);
    } else if (op == "list_collections") {
        handleListCollections(req, resp);
    } else if (op == "sample_schema") {
        handleSampleSchema(req, resp);
    } else if (op == "find") {
        handleFind(req, resp);
    } else if (op == "count") {
        handleCount(req, resp);
    } else if (op == "estimated_count") {
        handleEstimatedCount(req, resp);
    } else if (op == "aggregate") {
        handleAggregate(req, resp);
    } else if (op == "run_command") {
        handleRunCommand(req, resp);
    } else {
        resp["ok"] = false;
        resp["error"] = "unknown op: " + op;
    }

    writeResponse(resp);
}

} // namespace

int main() {
    mongoc_init();

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        try {
            const auto req = nlohmann::json::parse(line);
            dispatch(req);
        } catch (const std::exception& e) {
            nlohmann::json resp;
            resp["ok"] = false;
            resp["error"] = std::string("parse error: ") + e.what();
            writeResponse(resp);
        }
    }

    if (g_client) {
        mongoc_client_destroy(g_client);
        g_client = nullptr;
    }
    mongoc_cleanup();
    return 0;
}
