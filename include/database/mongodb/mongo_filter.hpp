#pragma once

#include <bsoncxx/document/value.hpp>
#include <string>

// Parse a table-viewer filter for MongoDB find().
// Accepts either:
//   - MongoDB extended JSON: {"field": "value"}
//   - SQL-style WHERE fragments used elsewhere in SQLEditor:
//       project = 'abc' AND created_by = 'system'
//       ("project" = 'abc') AND ("created_by" = 'system')
//       status IS NULL
//       name LIKE 'john%'
bsoncxx::document::value parseMongoFilter(const std::string& filter);

// Parse sort for MongoDB find(). Accepts JSON {"field": 1} or SQL-style:
//   project ASC, created_at DESC
bsoncxx::document::value parseMongoSort(const std::string& sort);
