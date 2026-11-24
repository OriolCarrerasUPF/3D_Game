#pragma once

// Minimal self-registering test harness. CHECK never relies on assert(), so the
// checks also run in Release builds (plan E2).

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace test
{
	struct Case
	{
		const char* name;
		std::function<void()> body;
	};

	inline std::vector<Case>& registry()
	{
		static std::vector<Case> cases;
		return cases;
	}

	inline int& failureCount()
	{
		static int failures = 0;
		return failures;
	}

	struct Registrar
	{
		Registrar(const char* name, std::function<void()> body) { registry().push_back({ name, std::move(body) }); }
	};

	inline void fail(const char* file, int line, const std::string& message)
	{
		++failureCount();
		std::cerr << "  FAILED " << file << ":" << line << ": " << message << std::endl;
	}
}

#define TEST_CONCAT_INNER(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                                 \
	static void TEST_CONCAT(test_fn_, __LINE__)();                                     \
	static test::Registrar TEST_CONCAT(test_reg_, __LINE__)(name, &TEST_CONCAT(test_fn_, __LINE__)); \
	static void TEST_CONCAT(test_fn_, __LINE__)()

#define CHECK(condition)                                                                \
	do { if (!(condition)) test::fail(__FILE__, __LINE__, "CHECK(" #condition ")"); } while (0)

#define CHECK_EQ(actual, expected)                                                      \
	do {                                                                                \
		const auto& test_a_ = (actual); const auto& test_e_ = (expected);               \
		if (!(test_a_ == test_e_)) {                                                    \
			std::ostringstream test_s_;                                                 \
			test_s_ << #actual " == " #expected " (got " << test_a_ << ", expected " << test_e_ << ")"; \
			test::fail(__FILE__, __LINE__, test_s_.str());                              \
		}                                                                               \
	} while (0)

#define CHECK_NEAR(actual, expected, tolerance)                                         \
	do {                                                                                \
		const double test_a_ = static_cast<double>(actual);                             \
		const double test_e_ = static_cast<double>(expected);                           \
		if (!(std::fabs(test_a_ - test_e_) <= (tolerance))) {                           \
			std::ostringstream test_s_;                                                 \
			test_s_ << #actual " ~= " #expected " (got " << test_a_ << ", expected " << test_e_ << ")"; \
			test::fail(__FILE__, __LINE__, test_s_.str());                              \
		}                                                                               \
	} while (0)
