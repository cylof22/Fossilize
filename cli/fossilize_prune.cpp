/* Copyright (c) 2019 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "fossilize_db.hpp"
#include <memory>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include "layer/utils.hpp"
#include "cli_parser.hpp"

using namespace Fossilize;
using namespace std;

static void print_help()
{
	LOGI("Usage: fossilize-prune [--input-db path] [--output-db path] [--filter-application hash]\n");
}

template <typename T>
static inline T fake_handle(uint64_t v)
{
	return (T)v;
}

struct PruneReplayer : StateCreatorInterface
{
	unordered_set<Hash> accessed_samplers;
	unordered_set<Hash> accessed_descriptor_sets;
	unordered_set<Hash> accessed_pipeline_layouts;
	unordered_set<Hash> accessed_shader_modules;
	unordered_set<Hash> accessed_render_passes;
	unordered_set<Hash> accessed_graphics_pipelines;
	unordered_set<Hash> accessed_compute_pipelines;

	unordered_map<Hash, const VkDescriptorSetLayoutCreateInfo *> descriptor_sets;
	unordered_map<Hash, const VkPipelineLayoutCreateInfo *> pipeline_layouts;

	Hash current_application = 0;
	Hash filter_application_hash = 0;
	bool should_filter_application_hash = false;
	bool allow_application_info = false;

	void set_application_info(const VkApplicationInfo *app,
	                          const VkPhysicalDeviceFeatures2 *features) override
	{
		if (allow_application_info)
		{
			Hash hash = Hashing::compute_combined_application_feature_hash(
					Hashing::compute_application_feature_hash(app, features));

			LOGI("Available application feature hash: %016llx\n",
			     static_cast<unsigned long long>(hash));

			if (app)
			{
				LOGI("  applicationInfo: engineName = %s, applicationName = %s, engineVersion = %u, appVersion = %u\n",
				     app->pEngineName ? app->pEngineName : "N/A", app->pApplicationName ? app->pApplicationName : "N/A",
				     app->engineVersion, app->applicationVersion);
			}
		}
	}

	void set_current_application_info(Hash hash) override
	{
		current_application = hash;
	}

	bool enqueue_create_sampler(Hash hash, const VkSamplerCreateInfo *, VkSampler *sampler) override
	{
		*sampler = fake_handle<VkSampler>(hash);
		return true;
	}

	void access_sampler(Hash hash)
	{
		accessed_samplers.insert(hash);
	}

	void access_descriptor_set(Hash hash)
	{
		if (accessed_descriptor_sets.count(hash))
			return;
		accessed_descriptor_sets.insert(hash);

		auto *create_info = descriptor_sets[hash];
		if (!create_info)
			return;

		for (uint32_t binding = 0; binding < create_info->bindingCount; binding++)
		{
			auto &bind = create_info->pBindings[binding];
			if (bind.pImmutableSamplers && bind.descriptorCount != 0)
			{
				for (uint32_t i = 0; i < bind.descriptorCount; i++)
					if (bind.pImmutableSamplers[i] != VK_NULL_HANDLE)
						access_sampler((Hash)bind.pImmutableSamplers[i]);
			}
		}
	}

	void access_pipeline_layout(Hash hash)
	{
		if (accessed_pipeline_layouts.count(hash))
			return;
		accessed_pipeline_layouts.insert(hash);

		auto *create_info = pipeline_layouts[hash];
		if (!create_info)
			return;

		for (uint32_t layout = 0; layout < create_info->setLayoutCount; layout++)
			access_descriptor_set((Hash)create_info->pSetLayouts[layout]);
	}

	bool enqueue_create_descriptor_set_layout(Hash hash, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		*layout = fake_handle<VkDescriptorSetLayout>(hash);
		descriptor_sets[hash] = create_info;
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
		*layout = fake_handle<VkPipelineLayout>(hash);
		pipeline_layouts[hash] = create_info;
		return true;
	}

	bool enqueue_create_shader_module(Hash hash, const VkShaderModuleCreateInfo *, VkShaderModule *module) override
	{
		*module = fake_handle<VkShaderModule>(hash);
		return true;
	}

	bool enqueue_create_render_pass(Hash hash, const VkRenderPassCreateInfo *, VkRenderPass *render_pass) override
	{
		*render_pass = fake_handle<VkRenderPass>(hash);
		return true;
	}

	bool enqueue_create_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);

		if (!should_filter_application_hash || current_application == filter_application_hash)
		{
			access_pipeline_layout((Hash) create_info->layout);
			accessed_shader_modules.insert((Hash) create_info->stage.module);
			accessed_compute_pipelines.insert(hash);
		}
		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);

		if (!should_filter_application_hash || current_application == filter_application_hash)
		{
			access_pipeline_layout((Hash) create_info->layout);
			accessed_render_passes.insert((Hash) create_info->renderPass);
			for (uint32_t stage = 0; stage < create_info->stageCount; stage++)
				accessed_shader_modules.insert((Hash) create_info->pStages[stage].module);
			accessed_graphics_pipelines.insert(hash);
		}
		return true;
	}
};

static bool copy_accessed_types(DatabaseInterface &input_db,
                                DatabaseInterface &output_db,
                                vector<uint8_t> &state_json,
                                const unordered_set<Hash> &accessed,
                                ResourceTag tag,
                                unsigned *per_tag_written)
{
	per_tag_written[tag] = accessed.size();
	for (auto hash : accessed)
	{
		size_t compressed_size = 0;
		if (!input_db.read_entry(tag, hash, &compressed_size, nullptr, PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
			return false;
		state_json.resize(compressed_size);
		if (!input_db.read_entry(tag, hash, &compressed_size, state_json.data(), PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
			return false;
		if (!output_db.write_entry(tag, hash, state_json.data(), state_json.size(), PAYLOAD_WRITE_RAW_FOSSILIZE_DB_BIT))
			return false;
	}
	return true;
}

int main(int argc, char *argv[])
{
	CLICallbacks cbs;
	string input_db_path;
	string output_db_path;
	Hash application_hash = 0;
	bool should_filter_application_hash = false;
	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--input-db", [&](CLIParser &parser) { input_db_path = parser.next_string(); });
	cbs.add("--output-db", [&](CLIParser &parser) { output_db_path = parser.next_string(); });
	cbs.add("--filter-application", [&](CLIParser &parser) {
		application_hash = strtoull(parser.next_string(), nullptr, 16);
		should_filter_application_hash = true;
	});

	cbs.error_handler = [] { print_help(); };

	CLIParser parser(move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
		return EXIT_FAILURE;
	if (parser.is_ended_state())
		return EXIT_SUCCESS;

	if (input_db_path.empty() || output_db_path.empty())
	{
		print_help();
		return EXIT_FAILURE;
	}

	auto input_db = std::unique_ptr<DatabaseInterface>(create_database(input_db_path.c_str(), DatabaseMode::ReadOnly));
	auto output_db = std::unique_ptr<DatabaseInterface>(create_database(output_db_path.c_str(), DatabaseMode::OverWrite));
	if (!input_db || !input_db->prepare())
	{
		LOGE("Failed to load database: %s\n", argv[1]);
		return EXIT_FAILURE;
	}

	if (!output_db || !output_db->prepare())
	{
		LOGE("Failed to open database for writing: %s\n", argv[2]);
		return EXIT_FAILURE;
	}

	StateReplayer replayer;
	PruneReplayer prune_replayer;

	replayer.set_resolve_shader_module_handles(false);

	if (should_filter_application_hash)
	{
		prune_replayer.should_filter_application_hash = true;
		prune_replayer.filter_application_hash = application_hash;
	}

	static const ResourceTag playback_order[] = {
		RESOURCE_APPLICATION_INFO,
		RESOURCE_SHADER_MODULE,
		RESOURCE_SAMPLER,
		RESOURCE_DESCRIPTOR_SET_LAYOUT,
		RESOURCE_PIPELINE_LAYOUT,
		RESOURCE_RENDER_PASS,
		RESOURCE_GRAPHICS_PIPELINE,
		RESOURCE_COMPUTE_PIPELINE,
	};

	unsigned per_tag_read[RESOURCE_COUNT] = {};
	unsigned per_tag_written[RESOURCE_COUNT] = {};

	static const char *tag_names[] = {
		"AppInfo",
		"Sampler",
		"Descriptor Set Layout",
		"Pipeline Layout",
		"Shader Module",
		"Render Pass",
		"Graphics Pipeline",
		"Compute Pipeline",
	};

	vector<uint8_t> state_json;

	for (auto &tag : playback_order)
	{
		// No need to resolve this type.
		if (tag == RESOURCE_SHADER_MODULE)
			continue;

		prune_replayer.allow_application_info = tag == RESOURCE_APPLICATION_INFO;

		size_t hash_count = 0;
		if (!input_db->get_hash_list_for_resource_tag(tag, &hash_count, nullptr))
		{
			LOGE("Failed to get hashes.\n");
			return EXIT_FAILURE;
		}

		per_tag_read[tag] = hash_count;

		vector<Hash> hashes(hash_count);

		if (!input_db->get_hash_list_for_resource_tag(tag, &hash_count, hashes.data()))
		{
			LOGE("Failed to get shader module hashes.\n");
			return EXIT_FAILURE;
		}

		for (auto hash : hashes)
		{
			size_t state_json_size;
			if (!input_db->read_entry(tag, hash, &state_json_size, nullptr, 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			state_json.resize(state_json_size);

			if (!input_db->read_entry(tag, hash, &state_json_size, state_json.data(), 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			try
			{
				replayer.parse(prune_replayer, input_db.get(), state_json.data(), state_json.size());
			}
			catch (const exception &e)
			{
				LOGE("StateReplayer threw exception parsing (tag: %d, hash: 0x%llx): %s\n", tag,
				     static_cast<unsigned long long>(hash), e.what());
			}

			if (tag == RESOURCE_APPLICATION_INFO)
			{
				if (!should_filter_application_hash || hash == application_hash)
				{
					size_t compressed_size = 0;
					if (!input_db->read_entry(tag, hash, &compressed_size, nullptr, PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
						return EXIT_FAILURE;
					state_json.resize(compressed_size);
					if (!input_db->read_entry(tag, hash, &compressed_size, state_json.data(),
					                          PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
						return EXIT_FAILURE;
					if (!output_db->write_entry(tag, hash, state_json.data(), state_json.size(),
					                            PAYLOAD_WRITE_RAW_FOSSILIZE_DB_BIT))
						return EXIT_FAILURE;
					per_tag_written[tag]++;
				}
			}
		}
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
	                         prune_replayer.accessed_samplers, RESOURCE_SAMPLER,
	                         per_tag_written))
	{
		LOGE("Failed to copy RESOURCE_SAMPLERs.\n");
		return EXIT_FAILURE;
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
	                         prune_replayer.accessed_descriptor_sets, RESOURCE_DESCRIPTOR_SET_LAYOUT,
	                         per_tag_written))
	{
		LOGE("Failed to copy DESCRIPTOR_SET_LAYOUTs.\n");
		return EXIT_FAILURE;
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
	                         prune_replayer.accessed_shader_modules, RESOURCE_SHADER_MODULE,
	                         per_tag_written))
	{
		LOGE("Failed to copy SHADER_MODULEs.\n");
		return EXIT_FAILURE;
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
	                         prune_replayer.accessed_render_passes, RESOURCE_RENDER_PASS,
	                         per_tag_written))
	{
		LOGE("Failed to copy RENDER_PASSes.\n");
		return EXIT_FAILURE;
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
	                         prune_replayer.accessed_pipeline_layouts, RESOURCE_PIPELINE_LAYOUT,
	                         per_tag_written))
	{
		LOGE("Failed to copy PIPELINE_LAYOUTs.\n");
		return EXIT_FAILURE;
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
	                         prune_replayer.accessed_graphics_pipelines, RESOURCE_GRAPHICS_PIPELINE,
	                         per_tag_written))
	{
		LOGE("Failed to copy GRAPHICS_PIPELINEs.\n");
		return EXIT_FAILURE;
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
	                         prune_replayer.accessed_compute_pipelines, RESOURCE_COMPUTE_PIPELINE,
	                         per_tag_written))
	{
		LOGE("Failed to copy COMPUTE_PIPELINEs.\n");
		return EXIT_FAILURE;
	}

	for (auto tag : playback_order)
		LOGI("Pruned %s entries: %u -> %u entries\n", tag_names[tag], per_tag_read[tag], per_tag_written[tag]);
}
