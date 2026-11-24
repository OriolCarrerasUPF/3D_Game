#include "test_harness.h"

#include "game/world_transform.h"

namespace
{
	void checkPoint(const Vector3& actual, float x, float y, float z, const char* label)
	{
		if (std::fabs(actual.x - x) > 1e-4f || std::fabs(actual.y - y) > 1e-4f || std::fabs(actual.z - z) > 1e-4f)
		{
			std::ostringstream message;
			message << label << ": got (" << actual.x << ", " << actual.y << ", " << actual.z
				<< "), expected (" << x << ", " << y << ", " << z << ")";
			test::fail(__FILE__, __LINE__, message.str());
		}
	}

	Vector3 applyAssetThenInstance(const Vector3& asset_scale, const Vector3& position, float yaw_degrees, const Vector3& local)
	{
		const Matrix44 asset = world_transform::makeAssetTransform(asset_scale, Vector3(0.0f, 0.0f, 0.0f));
		const Matrix44 instance = world_transform::makeInstanceTransform(position, yaw_degrees * DEG2RAD, Vector3(1.0f, 1.0f, 1.0f));
		return world_transform::composeAssetThenInstance(asset, instance) * local;
	}
}

TEST_CASE("world_transform: asset scale does not scale the placement position")
{
	// Regression: the P02 composition (instance * asset) multiplied the scene position
	// by the asset scale, sending the 1.9x deposits to (-44.6, -37) outside the map.
	const Vector3 origin = applyAssetThenInstance(Vector3(1.9f, 1.9f, 1.9f), Vector3(-23.5f, 0.0f, -19.5f), 0.0f, Vector3(0.0f, 0.0f, 0.0f));
	checkPoint(origin, -23.5f, 0.0f, -19.5f, "deposit origin");
	const Vector3 tip = applyAssetThenInstance(Vector3(2.0f, 2.0f, 2.0f), Vector3(10.0f, 0.0f, 0.0f), 0.0f, Vector3(1.0f, 0.0f, 0.0f));
	checkPoint(tip, 12.0f, 0.0f, 0.0f, "scaled vertex");
}

TEST_CASE("world_transform: rotation happens around the instance origin")
{
	// Regression: the P02 order rotated the translated point, moving the east wall
	// (rotation 180 at x = 7.5) inside the base at x = -7.05.
	const Vector3 east_wall = applyAssetThenInstance(Vector3(1.0f, 1.0f, 1.0f), Vector3(7.5f, 0.0f, -7.5f), 180.0f, Vector3(-0.45f, 0.0f, 0.0f));
	checkPoint(east_wall, 7.95f, 0.0f, -7.5f, "east wall slab");
}

TEST_CASE("world_transform: boundary wall rotations put the slab on the requested edge")
{
	// corridor_wall's slab sits on local x in [-0.5, -0.4]; walls are placed on interior
	// cell centres and rotated so the slab lands on the outer edge of that cell.
	const Vector3 local(-0.45f, 0.0f, 0.0f);
	const Vector3 cell(-7.5f, 0.0f, -7.5f); // cell (24,24) centre
	checkPoint(applyAssetThenInstance(Vector3(1.0f, 1.0f, 1.0f), cell, 0.0f, local), -7.95f, 0.0f, -7.5f, "west");
	checkPoint(applyAssetThenInstance(Vector3(1.0f, 1.0f, 1.0f), cell, 90.0f, local), -7.5f, 0.0f, -7.95f, "south");
	const Vector3 north_cell(-7.5f, 0.0f, 7.5f); // cell (24,39)
	checkPoint(applyAssetThenInstance(Vector3(1.0f, 1.0f, 1.0f), north_cell, -90.0f, local), -7.5f, 0.0f, 7.95f, "north");
}

TEST_CASE("world_transform: gate rotated 90 degrees spans world Z")
{
	// gate_simple is 1 m wide on local X; the 2-cell west door uses scale 2 at the edge x = -8.
	const Matrix44 asset = world_transform::makeAssetTransform(Vector3(1.0f, 1.0f, 1.0f), Vector3(0.0f, 0.0f, 0.0f));
	const Matrix44 instance = world_transform::makeInstanceTransform(Vector3(-8.0f, 0.0f, 0.0f), 90.0f * DEG2RAD, Vector3(2.0f, 2.0f, 2.0f));
	const Matrix44 model = world_transform::composeAssetThenInstance(asset, instance);
	checkPoint(model * Vector3(0.5f, 0.0f, 0.0f), -8.0f, 0.0f, 1.0f, "gate +X end");
	checkPoint(model * Vector3(-0.5f, 0.0f, 0.0f), -8.0f, 0.0f, -1.0f, "gate -X end");
	checkPoint(model * Vector3(0.0f, 1.08f, 0.0f), -8.0f, 2.16f, 0.0f, "gate top");
}

TEST_CASE("world_transform: pivot offset is applied after the asset scale")
{
	const Matrix44 asset = world_transform::makeAssetTransform(Vector3(2.0f, 2.0f, 2.0f), Vector3(0.0f, 0.5f, 0.0f));
	checkPoint(asset * Vector3(1.0f, 1.0f, 0.0f), 2.0f, 2.5f, 0.0f, "scaled + pivot");
}

TEST_CASE("world_transform: yaw convention round trip")
{
	const float directions[4][2] = { { 0.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, -1.0f }, { -1.0f, 0.0f } };
	for (const auto& d : directions)
	{
		const float yaw = world_transform::yawFromDirection(d[0], d[1]);
		const Vector3 facing = world_transform::directionFromYaw(yaw);
		checkPoint(facing, d[0], 0.0f, d[1], "directionFromYaw(yawFromDirection)");
		// The model matrix must agree: a +Z-forward model rotated by yaw faces d.
		const Matrix44 rotation = world_transform::makeInstanceTransform(Vector3(0.0f, 0.0f, 0.0f), yaw, Vector3(1.0f, 1.0f, 1.0f));
		checkPoint(rotation.rotateVector(Vector3(0.0f, 0.0f, 1.0f)), d[0], 0.0f, d[1], "matrix forward");
	}
}

TEST_CASE("world_transform: forward axis offsets map the authored forward to +Z")
{
	const char* axes[4] = { "+Z", "+X", "-X", "-Z" };
	const Vector3 locals[4] = { Vector3(0, 0, 1), Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 0, -1) };
	for (int i = 0; i < 4; ++i)
	{
		float yaw = 0.0f;
		CHECK(world_transform::forwardAxisYaw(axes[i], yaw));
		const Matrix44 rotation = world_transform::makeInstanceTransform(Vector3(0.0f, 0.0f, 0.0f), yaw, Vector3(1.0f, 1.0f, 1.0f));
		checkPoint(rotation.rotateVector(locals[i]), 0.0f, 0.0f, 1.0f, axes[i]);
	}
	float yaw = 0.0f;
	CHECK(!world_transform::forwardAxisYaw("up", yaw));
}
