#include "tests/harness/test_harness.h"
#include "util/status.h"

using namespace ltm;

TEST(Status, OkIsOk) {
  Status s = Status::Ok();
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(static_cast<int>(s.code()), static_cast<int>(StatusCode::kOk));
  EXPECT_TRUE(s.message().empty());
}

TEST(Status, ErrorCarriesCodeAndMessage) {
  Status s = Status::Error(StatusCode::kPageFull, "page 3 is full");
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.code() == StatusCode::kPageFull);
  EXPECT_TRUE(s.message().find("page 3") != std::string::npos);
}

TEST(Status, ToStringIncludesCodeName) {
  Status s = NotFound("key 7");
  std::string t = s.ToString();
  EXPECT_TRUE(t.find("kNotFound") != std::string::npos);
  EXPECT_TRUE(t.find("key 7") != std::string::npos);
  EXPECT_EQ(Status::Ok().ToString(), std::string("kOk"));
}

TEST(Status, PropagateKeepsFirstError) {
  Status first = InvalidArgument("the first error");
  Status second = NotFound("the second error");
  // Ok.Propagate(x) == x
  EXPECT_TRUE(Status::Ok().Propagate(second).code() == StatusCode::kNotFound);
  // 已有错误时保持自身
  EXPECT_TRUE(first.Propagate(second).code() == StatusCode::kInvalidArgument);
}

TEST(Status, StatusOrHoldsValueOrError) {
  StatusOr<int> good(42);
  EXPECT_TRUE(good.ok());
  EXPECT_EQ(good.value(), 42);
  EXPECT_EQ(*good, 42);

  StatusOr<int> bad(Status::Error(StatusCode::kCorruption, "broken"));
  EXPECT_FALSE(bad.ok());
  EXPECT_TRUE(bad.status().code() == StatusCode::kCorruption);
  EXPECT_EQ(bad.value_or(-1), -1);
}

TEST(Status, ConvenienceConstructors) {
  EXPECT_TRUE(OkStatus().ok());
  EXPECT_TRUE(InvalidArgument("x").code() == StatusCode::kInvalidArgument);
  EXPECT_TRUE(NotFound("x").code() == StatusCode::kNotFound);
  EXPECT_TRUE(NotImplemented("foo").code() == StatusCode::kNotImplemented);
  // NotImplemented 的提示里必须带上未实现的东西，便于定位
  EXPECT_TRUE(NotImplemented("foo").message().find("foo") != std::string::npos);
}

TEST(Status, ReturnIfErrorMacroExits) {
  auto f = [](bool fail) -> Status {
    LTM_RETURN_IF_ERROR(fail ? Status::Error(StatusCode::kIoError, "read failed") : Status::Ok());
    return Status::Ok();
  };
  EXPECT_TRUE(f(false).ok());
  EXPECT_TRUE(f(true).code() == StatusCode::kIoError);
}

TEST(Status, AssignOrReturnMacro) {
  auto f = [](int v) -> StatusOr<int> {
    int out = 0;
    LTM_ASSIGN_OR_RETURN(out, StatusOr<int>(v * 2));
    return out + 1;
  };
  auto r = f(10);
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.value(), 21);

  auto g = []() -> StatusOr<int> {
    int out = 0;
    LTM_ASSIGN_OR_RETURN(out, StatusOr<int>(Status::Error(StatusCode::kOutOfSpace, "full")));
    return out;
  };
  auto r2 = g();
  EXPECT_FALSE(r2.ok());
  EXPECT_TRUE(r2.status().code() == StatusCode::kOutOfSpace);
}

int main(int argc, char** argv) { return ltm::test::RunAll(argc, argv); }
