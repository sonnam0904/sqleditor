#include "database/mongodb/mongo_shell.hpp"

#include <gtest/gtest.h>

TEST(MongoShellTest, ParsesFindWithSortAndLimit) {
    const auto cmd = tryParseMongoShell(
        R"(db.skio_channel.find({}).sort({ created_at : -1 }).limit(10))");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->collection, "skio_channel");
    EXPECT_EQ(cmd->method, "find");
    ASSERT_EQ(cmd->args.size(), 1u);
    EXPECT_EQ(cmd->args[0], "{}");
    EXPECT_EQ(cmd->sort, "{ created_at : -1 }");
    EXPECT_EQ(cmd->limit, 10);
}

TEST(MongoShellTest, ParsesFindWithSortOnly) {
    const auto cmd = tryParseMongoShell(R"(db.users.find({ active: true }).sort({ name: 1 }))");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->sort, "{ name: 1 }");
    EXPECT_EQ(cmd->limit, -1);
}
