#include "test_harness.h"

#include "game/json_data.h"

#include <fstream>

namespace
{
	using json_data::Json;

	Json parse(const char* text)
	{
		Json value;
		DataLoadError error;
		if (!json_data::parseJsonText(text, "test.json", value, error))
			test::fail(__FILE__, __LINE__, "fixture JSON did not parse: " + error.describe());
		return value;
	}
}

TEST_CASE("json_data: malformed JSON reports file and byte")
{
	Json value;
	DataLoadError error;
	CHECK(!json_data::parseJsonText("{ \"a\": [1, 2 }", "broken.json", value, error));
	CHECK(error.file == "broken.json");
	CHECK(error.message.find("invalid JSON at byte") != std::string::npos);
}

TEST_CASE("json_data: missing file is an error")
{
	Json value;
	DataLoadError error;
	CHECK(!json_data::loadJsonFile("data/arkfall/does_not_exist.json", value, error));
	CHECK(error.message == "could not open JSON file");
}

TEST_CASE("json_data: validateKeys rejects missing and unknown fields")
{
	DataLoadError error;
	const Json ok = parse(R"({"id": "a", "extra": 1})");
	CHECK(json_data::validateKeys(ok, {"id"}, {"extra"}, "f.json", "/item", error));

	CHECK(!json_data::validateKeys(parse(R"({"extra": 1})"), {"id"}, {"extra"}, "f.json", "/item", error));
	CHECK_EQ(error.json_pointer, std::string("/item/id"));
	CHECK_EQ(error.message, std::string("required field is missing"));

	CHECK(!json_data::validateKeys(parse(R"({"id": "a", "typo": 1})"), {"id"}, {}, "f.json", "/item", error));
	CHECK_EQ(error.json_pointer, std::string("/item/typo"));

	CHECK(!json_data::validateKeys(parse("[1]"), {"id"}, {}, "f.json", "/item", error));
	CHECK_EQ(error.message, std::string("expected an object"));
}

TEST_CASE("json_data: schema version must match exactly")
{
	DataLoadError error;
	CHECK(json_data::checkSchemaVersion(parse(R"({"schemaVersion": 1})"), 1, "f.json", error));
	CHECK(!json_data::checkSchemaVersion(parse(R"({"schemaVersion": 2})"), 1, "f.json", error));
	CHECK(!json_data::checkSchemaVersion(parse(R"({"schemaVersion": 1.0})"), 1, "f.json", error));
	CHECK(!json_data::checkSchemaVersion(parse(R"({})"), 1, "f.json", error));
	CHECK_EQ(error.json_pointer, std::string("/schemaVersion"));
}

TEST_CASE("json_data: scalar readers validate type and range")
{
	DataLoadError error;
	std::string text;
	CHECK(json_data::readString(Json("abc"), text, "f", "/s", error));
	CHECK_EQ(text, std::string("abc"));
	CHECK(!json_data::readString(Json(""), text, "f", "/s", error));
	CHECK(!json_data::readString(Json(3), text, "f", "/s", error));

	bool flag = false;
	CHECK(json_data::readBool(Json(true), flag, "f", "/b", error));
	CHECK(flag);
	CHECK(!json_data::readBool(Json(1), flag, "f", "/b", error));

	float number = 0.0f;
	CHECK(json_data::readFiniteFloat(Json(1.5), number, "f", "/n", error));
	CHECK_NEAR(number, 1.5, 1e-6);
	CHECK(!json_data::readFiniteFloat(Json("1.5"), number, "f", "/n", error));
	CHECK(!json_data::readFiniteFloat(Json(1e300), number, "f", "/n", error)); // not representable as float
	CHECK(json_data::readFloatInRange(Json(100), 0.0f, 100.0f, number, "f", "/n", error));
	CHECK(!json_data::readFloatInRange(Json(100.5), 0.0f, 100.0f, number, "f", "/n", error));

	int integer = 0;
	CHECK(json_data::readInteger(Json(63), 0, 63, integer, "f", "/i", error));
	CHECK_EQ(integer, 63);
	CHECK(!json_data::readInteger(Json(64), 0, 63, integer, "f", "/i", error));
	CHECK(!json_data::readInteger(Json(-1), 0, 63, integer, "f", "/i", error));
	CHECK(!json_data::readInteger(Json(1.0), 0, 63, integer, "f", "/i", error));
	CHECK(!json_data::readInteger(Json(18446744073709551615ull), 0, 63, integer, "f", "/i", error));
}

TEST_CASE("json_data: vector readers require exact lengths")
{
	DataLoadError error;
	Vector3 v;
	CHECK(json_data::readVector3(parse("[1, 2, 3]"), v, "f", "/v", error));
	CHECK_NEAR(v.x, 1.0, 1e-6); CHECK_NEAR(v.y, 2.0, 1e-6); CHECK_NEAR(v.z, 3.0, 1e-6);
	CHECK(!json_data::readVector3(parse("[1, 2]"), v, "f", "/v", error));
	CHECK(!json_data::readVector3(parse("[1, 2, \"x\"]"), v, "f", "/v", error));
	CHECK_EQ(error.json_pointer, std::string("/v/2"));

	Vector2 v2;
	CHECK(json_data::readVector2(parse("[-32, -32]"), v2, "f", "/o", error));
	CHECK_NEAR(v2.x, -32.0, 1e-6);

	Vector4 color;
	CHECK(json_data::readColor4(parse("[0, 0.5, 1, 1]"), color, "f", "/c", error));
	CHECK(!json_data::readColor4(parse("[0, 0.5, 1.2, 1]"), color, "f", "/c", error));
	CHECK_EQ(error.json_pointer, std::string("/c/2"));

	json_data::IntPair cell;
	CHECK(json_data::readIntPair(parse("[23, 31]"), 0, 63, cell, "f", "/cell", error));
	CHECK_EQ(cell.x, 23); CHECK_EQ(cell.z, 31);
	CHECK(!json_data::readIntPair(parse("[23, 64]"), 0, 63, cell, "f", "/cell", error));
	CHECK_EQ(error.json_pointer, std::string("/cell/1"));
	CHECK(!json_data::readIntPair(parse("[23]"), 0, 63, cell, "f", "/cell", error));
}

TEST_CASE("json_data: data paths cannot escape the data directory")
{
	DataLoadError error;
	std::filesystem::path resolved;
	const std::filesystem::path data_root = std::filesystem::absolute("data");
	const std::filesystem::path base = data_root / "arkfall";
	CHECK(json_data::resolveDataPath(base, data_root, Json("models/terrain.obj"), resolved, "assets.json", "/p", error));
	CHECK(resolved.filename() == "terrain.obj");
	CHECK(json_data::resolveDataPath(base, data_root, Json("../shaders/basic.vs"), resolved, "assets.json", "/p", error));
	CHECK(!json_data::resolveDataPath(base, data_root, Json("../../CMakeLists.txt"), resolved, "assets.json", "/p", error));
	CHECK(!json_data::resolveDataPath(base, data_root, Json("models/missing.obj"), resolved, "assets.json", "/p", error));
	CHECK(!json_data::resolveDataPath(base, data_root, Json(""), resolved, "assets.json", "/p", error));
	CHECK(!json_data::resolveDataPath(base, data_root, Json((data_root / "arkfall" / "assets.json").string()), resolved, "assets.json", "/p", error));
}

TEST_CASE("json_data: uniform scale check")
{
	CHECK(json_data::isPositiveUniformScale(Vector3(2.0f, 2.0f, 2.0f)));
	CHECK(!json_data::isPositiveUniformScale(Vector3(2.0f, 1.0f, 2.0f)));
	CHECK(!json_data::isPositiveUniformScale(Vector3(0.0f, 0.0f, 0.0f)));
	CHECK(!json_data::isPositiveUniformScale(Vector3(-1.0f, -1.0f, -1.0f)));
}
