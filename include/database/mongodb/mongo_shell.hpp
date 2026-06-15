#pragma once

#include <optional>
#include <string>
#include <vector>

struct MongoShellCommand {
    std::string collection;
    std::string method;
    std::vector<std::string> args;
    int limit = -1;
    int skip = -1;
};

// Parse mongosh-style queries such as:
//   db.users.find({ status: "active" }).limit(100)
//   db.users.aggregate([{ $match: {} }])
// Returns nullopt when the input is not shell syntax (caller may try legacy JSON).
std::optional<MongoShellCommand> tryParseMongoShell(const std::string& query);

// Convert a shell/JS-like literal to strict extended JSON for bsoncxx::from_json().
std::string shellLiteralToExtendedJson(std::string_view literal);
