#include "json_data.h"

#include <cmath>
#include <fstream>
#include <limits>

namespace json_data
{
	DataLoadError makeError(const std::filesystem::path& file, const std::string& pointer, const std::string& message)
	{
		return DataLoadError{ file, pointer, message };
	}

	std::string childPointer(const std::string& pointer, const std::string& key)
	{
		return pointer + "/" + key;
	}

	std::string childPointer(const std::string& pointer, std::size_t index)
	{
		return pointer + "/" + std::to_string(index);
	}

	bool parseJsonText(const std::string& text, const std::filesystem::path& file, Json& result, DataLoadError& error)
	{
		try
		{
			result = Json::parse(text);
		}
		catch (const Json::parse_error& exception)
		{
			error = makeError(file, "", std::string("invalid JSON at byte ") + std::to_string(exception.byte) + ": " + exception.what());
			return false;
		}
		return true;
	}

	bool loadJsonFile(const std::filesystem::path& path, Json& result, DataLoadError& error)
	{
		std::ifstream input(path, std::ios::binary);
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
			error = makeError(path, "", std::string("invalid JSON at byte ") + std::to_string(exception.byte) + ": " + exception.what());
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
				error = makeError(file, childPointer(pointer, key), "required field is missing");
				return false;
			}
		}
		for (auto it = value.begin(); it != value.end(); ++it)
		{
			if (!required.count(it.key()) && !optional.count(it.key()))
			{
				error = makeError(file, childPointer(pointer, it.key()), "unknown field");
				return false;
			}
		}
		return true;
	}

	bool checkSchemaVersion(const Json& document, int expected_version,
		const std::filesystem::path& file, DataLoadError& error)
	{
		if (!document.is_object() || !document.contains("schemaVersion"))
		{
			error = makeError(file, "/schemaVersion", "required field is missing");
			return false;
		}
		const Json& version = document["schemaVersion"];
		if (!version.is_number_integer() || version.get<long long>() != expected_version)
		{
			error = makeError(file, "/schemaVersion", "expected supported schema version " + std::to_string(expected_version));
			return false;
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

	bool readFloatInRange(const Json& value, float min_value, float max_value, float& output,
		const std::filesystem::path& file, const std::string& pointer, DataLoadError& error)
	{
		float number = 0.0f;
		if (!readFiniteFloat(value, number, file, pointer, error))
			return false;
		if (number < min_value || number > max_value)
		{
			error = makeError(file, pointer, "number must be in [" + std::to_string(min_value) + ", " + std::to_string(max_value) + "]");
			return false;
		}
		output = number;
		return true;
	}

	bool readInteger(const Json& value, int min_value, int max_value, int& output,
		const std::filesystem::path& file, const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_number_integer())
		{
			error = makeError(file, pointer, "expected an integer");
			return false;
		}
		// Unsigned values above INT64_MAX would wrap; reject them before converting.
		if (value.is_number_unsigned() && value.get<unsigned long long>() > static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
		{
			error = makeError(file, pointer, "integer is out of range");
			return false;
		}
		const long long number = value.get<long long>();
		if (number < min_value || number > max_value)
		{
			error = makeError(file, pointer, "integer must be in [" + std::to_string(min_value) + ", " + std::to_string(max_value) + "]");
			return false;
		}
		output = static_cast<int>(number);
		return true;
	}

	bool readIntPair(const Json& value, int min_value, int max_value, IntPair& output,
		const std::filesystem::path& file, const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_array() || value.size() != 2)
		{
			error = makeError(file, pointer, "expected an array of exactly two integers");
			return false;
		}
		IntPair pair;
		if (!readInteger(value[0], min_value, max_value, pair.x, file, childPointer(pointer, std::size_t(0)), error) ||
			!readInteger(value[1], min_value, max_value, pair.z, file, childPointer(pointer, std::size_t(1)), error))
			return false;
		output = pair;
		return true;
	}

	namespace
	{
		template <std::size_t N>
		bool readFloatArray(const Json& value, float (&components)[N], const std::filesystem::path& file,
			const std::string& pointer, DataLoadError& error, const char* expectation)
		{
			if (!value.is_array() || value.size() != N)
			{
				error = makeError(file, pointer, expectation);
				return false;
			}
			for (std::size_t i = 0; i < N; ++i)
			{
				if (!readFiniteFloat(value[i], components[i], file, childPointer(pointer, i), error))
					return false;
			}
			return true;
		}
	}

	bool readVector2(const Json& value, Vector2& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		float c[2]{};
		if (!readFloatArray(value, c, file, pointer, error, "expected an array of exactly two numbers"))
			return false;
		output.set(c[0], c[1]);
		return true;
	}

	bool readVector3(const Json& value, Vector3& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		float c[3]{};
		if (!readFloatArray(value, c, file, pointer, error, "expected an array of exactly three numbers"))
			return false;
		output.set(c[0], c[1], c[2]);
		return true;
	}

	bool readColor4(const Json& value, Vector4& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		float c[4]{};
		if (!readFloatArray(value, c, file, pointer, error, "expected an array of exactly four numbers"))
			return false;
		for (std::size_t i = 0; i < 4; ++i)
		{
			if (c[i] < 0.0f || c[i] > 1.0f)
			{
				error = makeError(file, childPointer(pointer, i), "color components must be in [0, 1]");
				return false;
			}
		}
		output.set(c[0], c[1], c[2], c[3]);
		return true;
	}

	bool readNonEmptyArray(const Json& value, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_array() || value.empty())
		{
			error = makeError(file, pointer, "expected a non-empty array");
			return false;
		}
		return true;
	}

	bool isWithin(const std::filesystem::path& child, const std::filesystem::path& root)
	{
		const std::filesystem::path relative = child.lexically_relative(root);
		return !relative.empty() && relative != "." && *relative.begin() != "..";
	}

	bool resolveDataPath(const std::filesystem::path& base, const std::filesystem::path& data_root,
		const Json& value, std::filesystem::path& resolved, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_string())
		{
			error = makeError(file, pointer, "expected a relative path string");
			return false;
		}
		const std::filesystem::path relative_path(value.get<std::string>());
		if (relative_path.empty() || relative_path.is_absolute() || relative_path.has_root_name())
		{
			error = makeError(file, pointer, "path must be relative to the data directory");
			return false;
		}

		std::error_code ec;
		const auto canonical_data_root = std::filesystem::weakly_canonical(data_root, ec);
		if (ec)
		{
			error = makeError(file, pointer, "could not resolve the data directory");
			return false;
		}
		const auto candidate = std::filesystem::weakly_canonical(base / relative_path, ec);
		if (ec || !isWithin(candidate, canonical_data_root))
		{
			error = makeError(file, pointer, "path escapes the data directory or cannot be resolved");
			return false;
		}
		if (!std::filesystem::is_regular_file(candidate, ec) || ec)
		{
			error = makeError(file, pointer, "referenced file does not exist");
			return false;
		}
		resolved = candidate;
		return true;
	}

	bool isPositiveUniformScale(const Vector3& scale, float tolerance)
	{
		return scale.x > 0.0f && scale.y > 0.0f && scale.z > 0.0f &&
			std::fabs(scale.x - scale.y) <= tolerance &&
			std::fabs(scale.x - scale.z) <= tolerance;
	}
}
