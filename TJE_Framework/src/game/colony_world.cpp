#include "colony_world.h"

#include "framework/camera.h"
#include "graphics/mesh.h"
#include "graphics/shader.h"

#include <cmath>
#include <map>

namespace
{
	Matrix44 makeSceneTransform(const StaticSceneInstance& instance)
	{
		Matrix44 transform;
		transform.scale(instance.scale);
		transform.rotate(instance.rotation_y_degrees * DEG2RAD, Vector3(0.0f, 1.0f, 0.0f));
		transform.translate(instance.position);
		return transform;
	}

	Matrix44 makeAssetTransform(const AssetDefinition& asset)
	{
		Matrix44 transform;
		transform.scale(asset.default_scale);
		transform.translate(asset.pivot_offset);
		return transform;
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

void EntityMesh::setInstances(const std::vector<Matrix44>& models, const std::vector<bool>& blocks_navigation)
{
	instance_models = models;
	instance_blocks_navigation = blocks_navigation;
	for (Matrix44& instance_model : instance_models)
		instance_model = instance_model * asset_transform;
	if (instance_models.size() == 1)
		model = instance_models.front();
	else
		model.setIdentity();
}

void EntityMesh::render(Camera* camera)
{
	if (mesh && camera)
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

ColonyWorld::~ColonyWorld()
{
	for (EntityMesh* view : static_views)
		delete view;
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
		std::vector<Matrix44> models;
		std::vector<bool> blocks_navigation;
	};
	std::map<std::pair<std::string, std::string>, InstanceGroup> groups;

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
		group.models.push_back(makeSceneTransform(instance));
		group.blocks_navigation.push_back(instance.blocks_navigation);
	}

	std::vector<EntityMesh*> pending_views;
	std::size_t pending_instance_count = 0;
	for (const auto& [key, group] : groups)
	{
		(void)key;
		if (!group.asset || !group.material || !group.asset->mesh)
			continue;
		Material render_material;
		render_material.shader = assets->getStandardShader();
		render_material.color = group.material->tint;
		EntityMesh* view = new EntityMesh(group.asset->mesh, render_material, assets->getInstancedShader(),
			makeAssetTransform(*group.asset), group.asset->id);
		view->name = group.asset->id;
		view->setInstances(group.models, group.blocks_navigation);
		pending_views.push_back(view);
		pending_instance_count += group.models.size();
	}

	for (EntityMesh* view : static_views)
		delete view;
	static_views.swap(pending_views);
	instance_count = pending_instance_count;
	return true;
}

void ColonyWorld::render(Camera* camera)
{
	for (EntityMesh* view : static_views)
		view->render(camera);
}

std::size_t ColonyWorld::getInstanceCountForAsset(const std::string& asset_id) const
{
	std::size_t count = 0;
	for (const EntityMesh* view : static_views)
	{
		if (view->getAssetId() == asset_id)
			count += view->getInstanceCount();
	}
	return count;
}

std::size_t ColonyWorld::getInstancedViewCount() const
{
	std::size_t count = 0;
	for (const EntityMesh* view : static_views)
		count += view->isInstanced() ? 1 : 0;
	return count;
}
