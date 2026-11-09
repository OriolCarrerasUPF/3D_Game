#pragma once

#include <filesystem>
#include <string>

struct DataLoadError
{
	std::filesystem::path file;
	std::string json_pointer;
	std::string message;

	std::string describe() const
	{
		std::string result = file.string();
		if (!json_pointer.empty())
			result += json_pointer;
		if (!result.empty())
			result += ": ";
		result += message;
		return result;
	}
};
