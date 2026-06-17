#include "database/mongodb/mongo_filter.hpp"

#include <bsoncxx/types.hpp>
#include <gtest/gtest.h>
#include <string>

TEST(MongoFilterTest, AcceptsNumericAndDateAndCombined) {
    EXPECT_TRUE(isValidMongoFilter(R"(("time_response_over" = 15))"));
    EXPECT_TRUE(isValidMongoFilter(R"(("created_at" = ISODate("2024-04-15T05:11:03Z")))"));
    EXPECT_TRUE(isValidMongoFilter(
        R"(("time_response_over" = 15) AND ("created_at" = ISODate("2024-04-15T05:11:03Z")))"));
}

TEST(MongoFilterTest, ParsesDateFilterToBsonDate) {
    const auto doc = parseMongoFilter(R"(("created_at" = ISODate("2024-04-15T05:11:03Z")))");
    const auto elem = doc.view()["created_at"];
    ASSERT_TRUE(elem);
    ASSERT_EQ(elem.type(), bsoncxx::type::k_document);
    const auto range = elem.get_document().view();
    EXPECT_TRUE(range["$gte"]);
    EXPECT_TRUE(range["$lt"]);
    EXPECT_EQ(range["$gte"].type(), bsoncxx::type::k_date);
    EXPECT_EQ(range["$lt"].type(), bsoncxx::type::k_date);
}

TEST(MongoFilterTest, DateEqualityExactWithMilliseconds) {
    const auto doc = parseMongoFilter(R"(("created_at" = ISODate("2024-04-15T05:11:03.500Z")))");
    const auto elem = doc.view()["created_at"];
    ASSERT_TRUE(elem);
    EXPECT_EQ(elem.type(), bsoncxx::type::k_date);
    EXPECT_EQ(elem.get_date().value.count() % 1000, 500);
}

TEST(MongoFilterTest, ParsesCombinedFilter) {
    const auto doc = parseMongoFilter(
        R"(("time_response_over" = 15) AND ("created_at" = ISODate("2024-04-15T05:11:03Z")))");
    EXPECT_TRUE(doc.view()["$and"]);
}
