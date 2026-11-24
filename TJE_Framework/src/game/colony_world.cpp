#include "colony_world.h"

#include "world_transform.h"

#include "framework/camera.h"
#include "graphics/mesh.h"
#include "graphics/shader.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
	Matrix44 makeSceneTransform(const StaticSceneInstance& instance)
	{
		return world_transform::makeInstanceTransform(instance.position,
			instance.rotation_y_degrees * DEG2RAD, instance.scale);
	}

	Matrix44 makeAssetTransform(const AssetDefinition& asset)
	{
		return world_transform::makeAssetTransform(asset.default_scale, asset.pivot_offset);
	}

	// World XZ rectangle and top height of an instance's picking bounds.
	void registerWalkableSurface(SurfaceHeightMap& heights, const AssetDefinition& asset, const Matrix44& instance_transform)
	{
		const Vector3& lo = asset.picking_bounds_min;
		const Vector3& hi = asset.picking_bounds_max;
		float min_x = std::numeric_limits<float>::max(), min_z = std::numeric_limits<float>::max();
		float max_x = std::numeric_limits<float>::lowest(), max_z = std::numeric_limits<float>::lowest();
		float top_y = std::numeric_limits<float>::lowest();
		for (int corner = 0; corner < 8; ++corner)
		{
			const Vector3 local((corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z);
			const Vector3 world = instance_transform * local;
			min_x = std::min(min_x, world.x); max_x = std::max(max_x, world.x);
			min_z = std::min(min_z, world.z); max_z = std::max(max_z, world.z);
			top_y = std::max(top_y, world.y);
		}
		heights.addSurface(min_x, min_z, max_x, max_z, top_y);
	}
}

EntityMesh::EntityMesh(Mesh* mesh, const Material& material, Shader* instanced_shader,
	const Matrix44& asset_transform, const std::string& asset_id)
	: mesh(mesh)
	, material(material)
	, instanced_shader(instanced_shader)
	, asset_transform(asset_transform)
	, asset_id(asset_id)
{
}

void EntityMesh::refreshEntityModel()
{
	// A single instance is drawn through Entity::model so parenting keeps working.
	if (instance_models.size() == 1)
		model = instance_models.front();
	else
		model.setIdentity();
}

void EntityMesh::setInstances(const std::vector<Matrix44>& instance_transforms)
{
	instance_models.clear();
	instance_models.reserve(instance_transforms.size());
	for (const Matrix44& instance_transform : instance_transforms)
		instance_models.push_back(world_transform::composeAssetThenInstance(asset_transform, instance_transform));
	refreshEntityModel();
}

std::size_t EntityMesh::addInstance(const Matrix44& instance_transform)
{
	instance_models.push_back(world_transform::composeAssetThenInstance(asset_transform, instance_transform));
	refreshEntityModel();
	return instance_models.size() - 1;
}

void EntityMesh::setInstance(std::size_t index, const Matrix44& instance_transform)
{
	if (index >= instance_models.size())
		return;
	instance_models[index] = world_transform::composeAssetThenInstance(asset_transform, instance_transform);
	if (instance_models.size() == 1)
		model = instance_models.front();
}

void EntityMesh::removeInstance(std::size_t index)
{
	if (index >= instance_models.size())
		return;
	instance_models[index] = instance_models.back();
	instance_models.pop_back();
	refreshEntityModel();
}

void EntityMesh::clearInstances()
{
	instance_models.clear();
	refreshEntityModel();
}

void EntityMesh::render(Camera* camera)
{
	if (mesh && camera && !instance_models.empty())
	{
		const bool instanced = isInstanced();
		Shader* active_shader = instanced ? instanced_shader : material.shader;
		if (active_shader && active_shader->compiled)
		{
			active_shader->enable();
			active_shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
			active_shader->setUniform("u_color", material.color);

			if (instanced)
			{
				mesh->renderInstanced(GL_TRIANGLES, instance_models.data(), static_cast<int>(instance_models.size()));
			}
			else
			{
				active_shader->setUniform("u_model", getGlobalMatrix());
				mesh->render(GL_TRIANGLES);
			}
			active_shader->disable();
		}
	}
	Entity::render(camera);
}

ColonyWorld::ColonyWorld(const AssetCatalog& assets)
	: assets(&assets)
{
}

ColonyWorld::~ColonyWorld() = default;

std::unique_ptr<EntityMesh> ColonyWorld::makeView(const AssetDefinition& asset, const SemanticMaterial& material) const
{
	Material render_material;
	render_material.shader = assets->getStandardShader();
	render_material.color = material.tint;
	auto view = std::make_unique<EntityMesh>(asset.mesh, render_material, assets->getInstancedShader(),
		makeAssetTransform(asset), asset.id);
	view->name = asset.id;
	return view;
}

bool ColonyWorld::buildStaticViews(const SceneLoadResult& scene, DataLoadError& error)
{
	if (!scene)
	{
		error = *scene.error;
		return false;
	}

	struct InstanceGroup
	{
		const AssetDefinition* asset = nullptr;
		const SemanticMaterial* material = nullptr;
		std::vector<Matrix44> transforms;
	};
	std::map<std::pair<std::string, std::string>, InstanceGroup> groups;
	SurfaceHeightMap pending_heights;

	for (const StaticSceneInstance& instance : scene.instances)
	{
		const AssetDefinition* asset = assets->findAsset(instance.asset_id);
		const SemanticMaterial* material = assets->findMaterial(instance.material_id);
		if (!asset || !material)
		{
			error = DataLoadError{ "data/arkfall/scene.json", "/instances/" + instance.id,
				"scene instance references an undefined asset or material" };
			return false;
		}

		const Matrix44 transform = makeSceneTransform(instance);
		if (asset->walkable_surface)
			registerWalkableSurface(pending_heights, *asset, transform);

		if (!asset->mesh)
		{
			if (asset->load_failure_is_fatal)
			{
				error = DataLoadError{ asset->model_path, "/instances/" + instance.id,
					"required scene asset was not loaded" };
				return false;
			}
			continue;
		}

		InstanceGroup& group = groups[{ instance.asset_id, instance.material_id }];
		group.asset = asset;
		group.material = material;
		group.transforms.push_back(transform);
	}

	std::vector<std::unique_ptr<EntityMesh>> pending_views;
	std::size_t pending_instance_count = 0;
	for (const auto& [key, group] : groups)
	{
		(void)key;
		if (!group.asset || !group.material || !group.asset->mesh)
			continue;
		std::unique_ptr<EntityMesh> view = makeView(*group.asset, *group.material);
		view->setInstances(group.transforms);
		pending_instance_count += group.transforms.size();
		pending_views.push_back(std::move(view));
	}

	static_views.swap(pending_views);
	instance_count = pending_instance_count;
	surface_heights = pending_heights;
	return true;
}

DynamicViewId ColonyWorld::createDynamicView(const std::string& asset_id, const std::string& material_id, DataLoadError& error)
{
	const AssetDefinition* asset = assets->findAsset(asset_id);
	const SemanticMaterial* material = assets->findMaterial(material_id);
	if (!asset || !material)
	{
		error = DataLoadError{ "", "", "dynamic view references unknown asset '" + asset_id + "' or material '" + material_id + "'" };
		return invalid_dynamic_view;
	}
	if (!asset->mesh)
	{
		error = DataLoadError{ asset->model_path, "", "dynamic view asset '" + asset_id + "' has not been loaded" };
		return invalid_dynamic_view;
	}

	DynamicGroup& group = dynamic_groups[{ asset_id, material_id }];
	if (!group.view)
	{
		group.view = makeView(*asset, *material);
		if (!world_transform::forwardAxisYaw(asset->forward_axis, group.forward_yaw))
			group.forward_yaw = 0.0f;
	}

	const DynamicViewId id = next_dynamic_id++;
	const std::size_t slot = group.view->addInstance(Matrix44());
	group.slot_owners.push_back(id);
	dynamic_records[id] = DynamicRecord{ &group, slot };
	return id;
}

bool ColonyWorld::setDynamicViewPose(DynamicViewId id, const Vector3& gameplay_position, float yaw_radians)
{
	const auto it = dynamic_records.find(id);
	if (it == dynamic_records.end())
		return false;
	DynamicGroup& group = *it->second.group;
	const Vector3 visual_position(gameplay_position.x,
		gameplay_position.y + surface_heights.heightAt(gameplay_position.x, gameplay_position.z),
		gameplay_position.z);
	group.view->setInstance(it->second.slot, world_transform::makeInstanceTransform(
		visual_position, yaw_radians + group.forward_yaw, Vector3(1.0f, 1.0f, 1.0f)));
	return true;
}

bool ColonyWorld::destroyDynamicView(DynamicViewId id)
{
	const auto it = dynamic_records.find(id);
	if (it == dynamic_records.end())
		return false;
	DynamicGroup& group = *it->second.group;
	const std::size_t slot = it->second.slot;
	const std::size_t last = group.slot_owners.size() - 1;

	group.view->removeInstance(slot); // moves the last instance into `slot`
	if (slot != last)
	{
		const DynamicViewId moved = group.slot_owners[last];
		group.slot_owners[slot] = moved;
		dynamic_records[moved].slot = slot;
	}
	group.slot_owners.pop_back();
	dynamic_records.erase(it);
	return true;
}

void ColonyWorld::clearDynamicViews()
{
	dynamic_records.clear();
	dynamic_groups.clear();
	next_dynamic_id = 1;
}

void ColonyWorld::render(Camera* camera)
{
	for (const std::unique_ptr<EntityMesh>& view : static_views)
		view->render(camera);
	for (auto& [key, group] : dynamic_groups)
	{
		(void)key;
		if (group.view)
			group.view->render(camera);
	}
}

std::size_t ColonyWorld::getInstanceCountForAsset(const std::string& asset_id) const
{
	std::size_t count = 0;
	for (const std::unique_ptr<EntityMesh>& view : static_views)
	{
		if (view->getAssetId() == asset_id)
			count += view->getInstanceCount();
	}
	return count;
}

std::size_t ColonyWorld::getInstancedViewCount() const
{
	std::size_t count = 0;
	for (const std::unique_ptr<EntityMesh>& view : static_views)
		count += view->isInstanced() ? 1 : 0;
	return count;
}
