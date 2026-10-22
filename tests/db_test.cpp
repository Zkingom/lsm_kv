#include "kv/db.h"

#include <gtest/gtest.h>

namespace kv {
namespace {

TEST(DBTest, OverwriteDeleteAndMissing) {
  DB db;
  EXPECT_FALSE(db.Get("a").has_value());

  db.Put("a", "1");
  db.Put("a", "2");
  EXPECT_EQ(db.Get("a"), "2");

  db.Delete("a");
  EXPECT_FALSE(db.Get("a").has_value());

  db.Delete("not-exist");  // 删除不存在的键不报错
  EXPECT_FALSE(db.Get("not-exist").has_value());

  db.Put("empty", "");  // 空值是合法值，要和"不存在"区分
  ASSERT_TRUE(db.Get("empty").has_value());
  EXPECT_EQ(*db.Get("empty"), "");
}

}  // namespace
}  // namespace kv
