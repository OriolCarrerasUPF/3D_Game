#pragma once

// Shared, headless JSON helpers for every Arkfall data loader (assets, materials,
// scene and, from P03 on, scenario/buildings/colonists). Every reader reports the
// failing file, JSON pointer and a human readable message through DataLoadError and
// never applies implicit defaults: a missing or malformed field is always an error.

#include "data_load_error.h"
#include "framework/framework.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <set>
#include <string>

namespace json_data
{
	using Json = nlohmann::json;

	// Integer grid coordinate pair as authored in JSON: [x, z].
	struct IntPair
	{
		int x = 0;
		int z = 0;
	};

	DataLoadError makeError(const std::filesystem::path& file, const std::string& pointer, const std::string& message);

	// Joins a JSON pointer with an object key or array index: "/assets" + 3 -> "/assets/3".
	std::string childPointer(const std::string& pointer, const std::string& key);
	std::string childPointer(const std::string& pointer, std::size_t index);

	bool loadJsonFile(const std::filesystem::path& path, Json& result, DataLoadError& error);
	bool parseJsonText(const std::string& text, const std::filesystem::path& file, Json& result, DataLoadError& error);

	// The value must be an object with every required key and no key outside required/optional.
	bool validateKeys(const Json& value, const std::set<std::string>& required,
		const std::set<std::string>& optional, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);

	// Reads value["schemaVersion"] and requires it to equal expected_version.
	bool checkSchemaVersion(const Json& document, int expected_version,
		const std::filesystem::path& file, DataLoadError& error);

	bool readString(const Json& value, std::string& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);
	bool readBool(const Json& value, bool& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);
	bool readFiniteFloat(const Json& value, float& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);
	bool readFloatInRange(const Json& value, float min_value, float max_value, float& output,
		const std::filesystem::path& file, const std::string& pointer, DataLoadError& error);
	bool readInteger(const Json& value, int min_value, int max_value, int& output,
		const std::filesystem::path& file, const std::string& pointer, DataLoadError& error);
	bool readIntPair(const Json& value, int min_value, int max_value, IntPair& output,
		const std::filesystem::path& file, const std::string& pointer, DataLoadError& error);
	bool readVector2(const Json& value, Vector2& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);
	bool readVector3(const Json& value, Vector3& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);
	// Four finite components, each in [0, 1] (colors / tints).
	bool readColor4(const Json& value, Vector4& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);
	bool readNonEmptyArray(const Json& value, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);

	// True when child is strictly inside root (both lexically normalized/canonical paths).
	bool isWithin(const std::filesystem::path& child, const std::filesystem::path& root);

	// Resolves a relative path string against base, requires the result to stay inside
	// data_root and to name an existing regular file.
	bool resolveDataPath(const std::filesystem::path& base, const std::filesystem::path& data_root,
		const Json& value, std::filesystem::path& resolved, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);

	// Scale vectors must be positive and uniform because the basic shaders transform
	// normals with the model matrix.
	bool isPositiveUniformScale(const Vector3& scale, float tolerance = 0.0001f);
}
