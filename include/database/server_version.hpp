#pragma once

#include "db.hpp"

class DatabaseInterface;

namespace db_version {

std::string scalarFromQuery(const QueryResult& result);

void fetchAndStoreServerVersion(DatabaseInterface& db);

} // namespace db_version
