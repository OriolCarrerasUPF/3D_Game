#include "json_data.h"

#include <cmath>
#include <fstream>
#include <limits>

namespace json_data
{
	// ---- Moved from asset_catalog.cpp / scene_parser.cpp (unchanged) ------

	DataLoadError makeError(const std::filesystem::path& file, const std::string& pointer,
		const std::string& message)
	{
		return DataLoadError{ file, pointer, message };
	}

	bool loadJson(const std::filesystem::path& path, Json& result, DataLoadError& error)
	{
		std::ifstream input(path);
		if (!input)
		{
			error = makeError(path, "", "could not open JSON file");
			return false;
		}
		try
		{
			input >> result;
		}
		catch (const Json::parse_error& exception)
		{
			error = makeError(path, "", std::string("invalid JSON at byte ") +
				std::to_string(exception.byte) + ": " + exception.what());
			return false;
		}
		return true;
	}

	bool validateKeys(const Json& value, const std::set<std::string>& required,
		const std::set<std::string>& optional, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_object())
		{
			error = makeError(file, pointer, "expected an object");
			return false;
		}
		for (const std::string& key : required)
		{
			if (!value.contains(key))
			{
				error = makeError(file, pointer + "/" + key, "required field is missing");
				return false;
			}
		}
		for (auto it = value.begin(); it != value.end(); ++it)
		{
			if (!required.count(it.key()) && !optional.count(it.key()))
			{
				error = makeError(file, pointer + "/" + it.key(), "unknown field");
				return false;
			}
		}
		return true;
	}

	bool readString(const Json& value, std::string& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_string() || value.get_ref<const std::string&>().empty())
		{
			error = makeError(file, pointer, "expected a non-empty string");
			return false;
		}
		output = value.get<std::string>();
		return true;
	}

	bool readFiniteFloat(const Json& value, float& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_number())
		{
			error = makeError(file, pointer, "expected a finite number");
			return false;
		}
		const double number = value.get<double>();
		if (!std::isfinite(number) || !std::isfinite(static_cast<float>(number)))
		{
			error = makeError(file, pointer, "number must be finite and representable as a float");
			return false;
		}
		output = static_cast<float>(number);
		return true;
	}

	bool readVector3(const Json& value, Vector3& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_array() || value.size() != 3)
		{
			error = makeError(file, pointer, "expected an array of exactly three numbers");
			return false;
		}
		float components[3]{};
		for (size_t i = 0; i < 3; ++i)
		{
			if (!readFiniteFloat(value[i], components[i], file, pointer + "/" + std::to_string(i), error))
				return false;
		}
		output.set(components[0], components[1], components[2]);
		return true;
	}

	// ---- New: replaces the repeated schemaVersion checks ------------------

	bool checkSchemaVersion(const Json& document, int supported_version,
		const std::filesystem::path& file, DataLoadError& error)
	{
		if (!document.is_object() || !document.contains("schemaVersion"))
		{
			error = makeError(file, "/schemaVersion", "required field is missing");
			return false;
		}
		const Json& version = document["schemaVersion"];
		if (!version.is_number_integer() || version.get<int>() != supported_version)
		{
			error = makeError(file, "/schemaVersion",
				"expected supported schema version " + std::to_string(supported_version));
			return false;
		}
		return true;
	}

	// ---- New: needed by scenario_loader (P03) and later loaders ------------

	bool readInt(const Json& value, int& output, int min_value, int max_value,
		const std::filesystem::path& file, const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_number_integer())
		{
			error = makeError(file, pointer, "expected an integer");
			return false;
		}
		const long long number = value.get<long long>();
		if (number < min_value || number > max_value)
		{
			error = makeError(file, pointer, "integer " + std::to_string(number) + " is outside [" +
				std::to_string(min_value) + ", " + std::to_string(max_value) + "]");
			return false;
		}
		output = static_cast<int>(number);
		return true;
	}

	bool readBool(const Json& value, bool& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_boolean())
		{
			error = makeError(file, pointer, "expected a boolean");
			return false;
		}
		output = value.get<bool>();
		return true;
	}

	bool requireArray(const Json& value, std::size_t min_size, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_array())
		{
			error = makeError(file, pointer, "expected an array");
			return false;
		}
		if (value.size() < min_size)
		{
			error = makeError(file, pointer, "expected at least " + std::to_string(min_size) + " element(s)");
			return false;
		}
		return true;
	}

	bool readCell(const Json& value, Cell& output, int grid_width, int grid_height,
		const std::filesystem::path& file, const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_array() || value.size() != 2)
		{
			error = makeError(file, pointer, "expected a cell [x, z]");
			return false;
		}
		Cell cell;
		if (!readInt(value[0], cell.x, 0, grid_width - 1, file, pointer + "/0", error) ||
			!readInt(value[1], cell.z, 0, grid_height - 1, file, pointer + "/1", error))
			return false;
		output = cell;
		return true;
	}
}