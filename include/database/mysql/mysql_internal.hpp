#pragma once

#include "database/db.hpp"
#include "database/db_interface.hpp"
#include <functional>
#include <memory>
#include <mysql/mysql.h>

namespace mysql_internal {

    struct MysqlResDeleter {
        void operator()(MYSQL_RES* r) const {
            if (r)
                mysql_free_result(r);
        }
    };
    using MysqlResPtr = std::unique_ptr<MYSQL_RES, MysqlResDeleter>;

    bool shouldApplySslCA(const DatabaseConnectionInfo& info);

    std::function<MYSQL*()> makeMysqlFactory(const DatabaseConnectionInfo& info);

    StatementResult extractMysqlResult(MYSQL* conn, int rowLimit);

} // namespace mysql_internal
