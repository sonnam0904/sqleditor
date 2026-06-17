#pragma once

#include <optional>
#include <string>
#include <vector>

inline constexpr int kDefaultMongoFindLimit = 100;

struct MongoShellCommand {
    std::string collection;
    std::string method;
    std::vector<std::string> args;
    std::string sort;
    int limit = -1;
    int skip = -1;
};

// Parse mongosh-style queries such as:
//   db.users.find({ status: "active" }).sort({ created_at: -1 }).limit(100)
//   db.users.aggregate([{ $match: {} }])
// Returns nullopt when the input is not shell syntax (caller may try legacy JSON).
std::optional<MongoShellCommand> tryParseMongoShell(const std::string& query);

// Split editor text into individual db.* shell commands (supports multi-line formatting).
std::vector<std::string> splitMongoShellCommands(const std::string& text);

// Convert a shell/JS-like literal to strict extended JSON for bsoncxx::from_json().
std::string shellLiteralToExtendedJson(std::string_view literal);
