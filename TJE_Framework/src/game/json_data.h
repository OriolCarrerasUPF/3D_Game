#pragma once

// Shared JSON reading/validation helpers for every ARKFALL data loader (assets,
// materials, scene and, from P03 on, scenario/buildings/colonists).
// Every function reports failure through DataLoadError (file + JSON pointer + message)
// and returns false; nothing throws. Gameplay fields have no silent defaults (plan
// C9.1). No OpenGL dependency, so the headless tests can use it.

#include "arkfall_types.h"
#include "data_load_error.h"
#include "framework/framework.h"   // Vector2/3/4 (header-only math)

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace json_data
{
	using Json = nlohmann::json;

	// ---- Errors, pointers and files ---------------------------------------

	DataLoadError makeError(const std::filesystem::path& file, const std::string& pointer,
		const std::string& message);

	// Joins a JSON pointer with an object key or array index: "/assets" + 3 -> "/assets/3".
	std::string childPointer(const std::string& pointer, const std::string& key);
	std::string childPointer(const std::string& pointer, std::size_t index);

	// Opens and parses a JSON file. Fails on a missing file or invalid JSON.
	bool loadJson(const std::filesystem::path& path, Json& result, DataLoadError& error);

	// Parses JSON text (used by tests and in-memory data); `file` labels errors.
	bool parseJsonText(const std::string& text, const std::filesystem::path& file, Json& result,
		DataLoadError& error);

	// Requires document["schemaVersion"] to be the integer `supported_version`.
	bool checkSchemaVersion(const Json& document, int supported_version,
		const std::filesystem::path& file, DataLoadError& error);

	// ---- Objects ----------------------------------------------------------

	// Value must be an object containing every `required` key and no key outside
	// `required` + `optional`.
	bool validateKeys(const Json& value, const std::set<std::string>& required,
		const std::set<std::string>& optional, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);

	// ---- Scalars ----------------------------------------------------------

	bool readString(const Json& value, std::string& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);

	bool readBool(const Json& value, bool& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);

	bool readFiniteFloat(const Json& value, float& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);

	// Finite float in [min_value, max_value] inclusive (needs, rates, ...).
	bool readFloatInRange(const Json& value, float min_value, float max_value, float& output,
		const std::filesystem::path& file, const std::string& pointer, DataLoadError& error);

	// Integer in [min_value, max_value] inclusive.
	bool readInt(const Json& value, int& output, int min_value, int max_value,
		const std::filesystem::path& file, const std::string& pointer, DataLoadError& error);

	// ---- Arrays and compound values --------------------------------------

	// Value must be an array with at least `min_size` elements.
	bool requireArray(const Json& value, std::size_t min_size, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);

	bool readVector2(const Json& value, Vector2& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);

	bool readVector3(const Json& value, Vector3& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);

	// Four finite components, each in [0, 1] (colors / tints).
	bool readColor4(const Json& value, Vector4& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error);

	// [x, z] grid indices, each inside [0, width) / [0, height).
	bool readCell(const Json& value, Cell& output, int grid_width, int grid_height,
		const std::filesystem::path& file, const std::string& pointer, DataLoadError& error);

	// ---- Enums ------------------------------------------------------------

	// Maps a string such as "iron" to an enum value; unknown names are errors.
	// Template, so it lives in the header.
	template <typename EnumType>
	bool readEnum(const Json& value, const std::map<std::string, EnumType>& names, EnumType& output,
		const std::filesystem::path& file, const std::string& pointer, DataLoadError& error)
	{
		std::string text;
		if (!readString(value, text, file, pointer, error))
			return false;
		const auto it = names.find(text);
		if (it == names.end())
		{
			std::string allowed;
			for (const auto& [name, enum_value] : names)
			{
				(void)enum_value;
				allowed += (allowed.empty() ? "" : ", ") + name;
			}
			error = makeError(file, pointer, "unknown value '" + text + "' (expected one of: " + allowed + ")");
			return false;
		}
		output = it->second;
		return true;
	}

	// ---- Paths and transforms ---------------------------------------------

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
