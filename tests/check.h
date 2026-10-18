#pragma once

// Minimal self-contained test harness: TEST(name) { CHECK(...); CHECK_NEAR(a, b, tol); }

#include <cmath>
#include <cstdio>
#include <vector>

namespace check
{
	struct Case
	{
		const char* name;
		void (*fn)();
	};

	inline std::vector<Case>& Registry()
	{
		static std::vector<Case> r;
		return r;
	}

	inline int& Failures()
	{
		static int f = 0;
		return f;
	}

	struct Registrar
	{
		Registrar(const char* name, void (*fn)()) { Registry().push_back({ name, fn }); }
	};

	inline void Fail(const char* file, int line, const char* expr)
	{
		++Failures();
		std::printf("    FAIL %s:%d  %s\n", file, line, expr);
	}

	inline void FailNear(const char* file, int line, const char* a, const char* b, double va, double vb, double tol)
	{
		++Failures();
		std::printf("    FAIL %s:%d  %s ~= %s  (%.6f vs %.6f, tol %.6f)\n", file, line, a, b, va, vb, tol);
	}

	inline int RunAll()
	{
		int failedCases = 0;
		for (const auto& c : Registry()) {
			const int before = Failures();
			c.fn();
			const bool ok = Failures() == before;
			std::printf("[%s] %s\n", ok ? " ok " : "FAIL", c.name);
			failedCases += ok ? 0 : 1;
		}
		std::printf("\n%zu cases, %d failed, %d failed checks\n", Registry().size(), failedCases, Failures());
		return failedCases == 0 ? 0 : 1;
	}
}

#define TEST(name)                                                      \
	static void name();                                                 \
	static check::Registrar registrar_##name(#name, &name);             \
	static void name()

#define CHECK(expr) \
	do { if (!(expr)) check::Fail(__FILE__, __LINE__, #expr); } while (false)

#define CHECK_NEAR(a, b, tol)                                                                                       \
	do {                                                                                                            \
		const double va_ = static_cast<double>(a), vb_ = static_cast<double>(b), t_ = static_cast<double>(tol);     \
		if (!(std::fabs(va_ - vb_) <= t_)) check::FailNear(__FILE__, __LINE__, #a, #b, va_, vb_, t_);              \
	} while (false)
