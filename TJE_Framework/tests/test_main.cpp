#include "test_harness.h"

#include <cstdlib>
#include <cstring>

// Usage: arkfall_tests [name-filter]
// Runs every registered case (or those whose name contains the filter).
int main(int argc, char** argv)
{
	const char* filter = argc > 1 ? argv[1] : nullptr;
	int executed = 0;
	int failed_cases = 0;
	for (const test::Case& test_case : test::registry())
	{
		if (filter && !std::strstr(test_case.name, filter))
			continue;
		const int failures_before = test::failureCount();
		std::cout << "[ RUN  ] " << test_case.name << std::endl;
		try
		{
			test_case.body();
		}
		catch (const std::exception& exception)
		{
			test::fail(__FILE__, __LINE__, std::string("unexpected exception: ") + exception.what());
		}
		++executed;
		if (test::failureCount() != failures_before)
		{
			++failed_cases;
			std::cout << "[ FAIL ] " << test_case.name << std::endl;
		}
	}
	std::cout << executed - failed_cases << "/" << executed << " test cases passed" << std::endl;
	if (executed == 0)
	{
		std::cerr << "no test cases matched" << std::endl;
		return EXIT_FAILURE;
	}
	return failed_cases == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
