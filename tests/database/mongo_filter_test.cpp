#include "database/mongodb/mongo_filter.hpp"

#include <bsoncxx/json.hpp>
#include <gtest/gtest.h>

TEST(MongoFilterTest, ParsesSqlStyleAndFilter) {
    const auto doc = parseMongoFilter(
        R"(("project" = 'b77271d9-a5c7-49b7-a497-869a2bd36102') AND ("created_by" = 'system'))");
    const std::string json = bsoncxx::to_json(doc.view());
    EXPECT_NE(json.find("project"), std::string::npos);
    EXPECT_NE(json.find("b77271d9-a5c7-49b7-a497-869a2bd36102"), std::string::npos);
    EXPECT_NE(json.find("created_by"), std::string::npos);
    EXPECT_NE(json.find("system"), std::string::npos);
    EXPECT_NE(json.find("$and"), std::string::npos);
}

TEST(MongoFilterTest, ParsesJsonFilter) {
    const auto doc = parseMongoFilter(R"({"status": "active"})");
    const std::string json = bsoncxx::to_json(doc.view());
    EXPECT_NE(json.find("status"), std::string::npos);
    EXPECT_NE(json.find("active"), std::string::npos);
}

TEST(MongoFilterTest, ParsesIsNull) {
    const auto doc = parseMongoFilter("deleted_at IS NULL");
    const std::string json = bsoncxx::to_json(doc.view());
    EXPECT_NE(json.find("deleted_at"), std::string::npos);
    EXPECT_NE(json.find("null"), std::string::npos);
}

TEST(MongoFilterTest, ParsesSqlStyleSort) {
    const auto doc = parseMongoSort(R"("created_at" DESC, "name" ASC)");
    const std::string json = bsoncxx::to_json(doc.view());
    EXPECT_NE(json.find("created_at"), std::string::npos);
    EXPECT_NE(json.find("-1"), std::string::npos);
    EXPECT_NE(json.find("name"), std::string::npos);
    EXPECT_NE(json.find("1"), std::string::npos);
}

TEST(MongoFilterTest, EmptyFilterReturnsEmptyDocument) {
    const auto doc = parseMongoFilter("");
    EXPECT_TRUE(doc.view().empty());
}
