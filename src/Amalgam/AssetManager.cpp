//project headers:
#include "AssetManager.h"

#include "Amalgam.h"
#include "BinaryPacking.h"
#include "EntityExternalInterface.h"
#include "EvaluableNode.h"
#include "FileSupportCSV.h"
#include "FileSupportJSON.h"
#include "FileSupportYAML.h"
#include "Interpreter.h"
#include "PlatformSpecific.h"

//system headers:
#include <algorithm>
#include <ctime>
#include <fstream>
#include <iostream>

AssetManager asset_manager;

EvaluableNodeReference AssetManager::LoadResourcePath(std::string &resource_path,
	std::string &resource_base_path, std::string &file_type, EvaluableNodeManager *enm, bool escape_filename, EntityExternalInterface::LoadEntityStatus &status)
{
	//get file path based on the file loaded
	std::string path, file_base, extension;
	Platform_SeparatePathFileExtension(resource_path, path, file_base, extension);
	resource_base_path = path + file_base;

	//escape the string if necessary, otherwise just use the regular one
	std::string processed_resource_path;
	if(escape_filename)
	{
		resource_base_path = path + FilenameEscapeProcessor::SafeEscapeFilename(file_base);
		processed_resource_path = resource_base_path + "." + extension;
	}
	else
	{
		resource_base_path = path + file_base;
		processed_resource_path = resource_path;
	}

	if(file_type.empty())
		file_type = extension;

	//load this entity based on file_type
	if(file_type == FILE_EXTENSION_AMALGAM || file_type == FILE_EXTENSION_AMLG_METADATA)
	{
		auto [code, code_success] = Platform_OpenFileAsString(processed_resource_path);
		if(!code_success)
		{
			status.SetStatus(false, code);
			if(file_type == FILE_EXTENSION_AMALGAM)
				std::cerr << code << std::endl;
			return EvaluableNodeReference::Null();
		}

		//check for byte order mark for UTF-8 that may optionally appear at the beginning of the file.
		// If it is present, remove it.  No other encoding standards besides ascii and UTF-8 are currently permitted.
		if(code.size() >= 3)
		{
			if(static_cast<uint8_t>(code[0]) == 0xEF && static_cast<uint8_t>(code[1]) == 0xBB && static_cast<uint8_t>(code[2]) == 0xBF)
				code.erase(0, 3);
		}

		return Parser::Parse(code, enm, &resource_path, debugSources);
	}
	else if(file_type == FILE_EXTENSION_JSON)
		return EvaluableNodeReference(EvaluableNodeJSONTranslation::Load(processed_resource_path, enm, status), true);
	else if(file_type == FILE_EXTENSION_YAML)
		return EvaluableNodeReference(EvaluableNodeYAMLTranslation::Load(processed_resource_path, enm, status), true);
	else if(file_type == FILE_EXTENSION_CSV)
		return EvaluableNodeReference(FileSupportCSV::Load(processed_resource_path, enm, status), true);
	else if(file_type == FILE_EXTENSION_COMPRESSED_AMALGAM_CODE)
	{
		BinaryData compressed_data;
		auto [error_mesg, version, success] = LoadFileToBuffer<BinaryData>(processed_resource_path, file_type, compressed_data);
		if(!success)
		{
			status.SetStatus(false, error_mesg, version);
			return EvaluableNodeReference::Null();
		}

		OffsetIndex cur_offset = 0;
		auto strings = DecompressStrings(compressed_data, cur_offset);
		if(strings.size() == 0)
			return EvaluableNodeReference::Null();

		return Parser::Parse(strings[0], enm, &resource_path, debugSources);
	}
	else //just load the file as a string
	{
		std::string s;
		auto [error_mesg, version, success] = LoadFileToBuffer<std::string>(processed_resource_path, file_type, s);
		if(success)
			return EvaluableNodeReference(enm->AllocNode(ENT_STRING, s), true);
		else
		{
			status.SetStatus(false, error_mesg, version);
			return EvaluableNodeReference::Null();
		}
	}
}

bool AssetManager::StoreResourcePathFromProcessedResourcePaths(EvaluableNode *code, std::string &complete_resource_path,
	std::string &file_type, EvaluableNodeManager *enm, bool escape_filename, bool sort_keys)
{
	//store the entity based on file_type
	if(file_type == FILE_EXTENSION_AMALGAM || file_type == FILE_EXTENSION_AMLG_METADATA)
	{
		std::ofstream outf(complete_resource_path, std::ios::out | std::ios::binary);
		if(!outf.good())
			return false;

		std::string code_string = Parser::Unparse(code, enm, true, true, sort_keys);
		outf.write(code_string.c_str(), code_string.size());
		outf.close();

		return true;
	}
	else if(file_type == FILE_EXTENSION_JSON)
		return EvaluableNodeJSONTranslation::Store(code, complete_resource_path, enm, sort_keys);
	else if(file_type == FILE_EXTENSION_YAML)
		return EvaluableNodeYAMLTranslation::Store(code, complete_resource_path, enm, sort_keys);
	else if(file_type == FILE_EXTENSION_CSV)
		return FileSupportCSV::Store(code, complete_resource_path, enm);
	else if(file_type == FILE_EXTENSION_COMPRESSED_AMALGAM_CODE)
	{
		std::string code_string = Parser::Unparse(code, enm, false, true, sort_keys);

		//transform into format needed for compression
		CompactHashMap<std::string, size_t> string_map;
		string_map[code_string] = 0;

		//compress and store
		BinaryData compressed_data = CompressStrings(string_map);
		return StoreFileFromBuffer<BinaryData>(complete_resource_path, file_type, compressed_data);
	}
	else //binary string
	{
		std::string s = EvaluableNode::ToStringPreservingOpcodeType(code);
		return StoreFileFromBuffer<std::string>(complete_resource_path, file_type, s);
	}

	return false;
}

Entity *AssetManager::LoadEntityFromResourcePath(std::string &resource_path, std::string &file_type,
	bool persistent, bool load_contained_entities, bool escape_filename, bool escape_contained_filenames,
	std::string default_random_seed, Interpreter *calling_interpreter, EntityExternalInterface::LoadEntityStatus &status)
{
	std::string resource_base_path;
	Entity *new_entity = new Entity();

	EvaluableNodeReference code = LoadResourcePath(resource_path, resource_base_path, file_type, &new_entity->evaluableNodeManager, escape_filename, status);
	if(!status.loaded)
	{
		delete new_entity;
		return nullptr;
	}

	new_entity->SetRandomState(default_random_seed, true);

	if(file_type == FILE_EXTENSION_COMPRESSED_AMALGAM_CODE)
	{
		new_entity->SetRoot(code, true);

		EvaluableNodeReference args = EvaluableNodeReference(new_entity->evaluableNodeManager.AllocNode(ENT_ASSOC), true);
		args->SetMappedChildNode(GetStringIdFromBuiltInStringId(ENBISI_create_new_entity), new_entity->evaluableNodeManager.AllocNode(ENT_FALSE));
		auto call_stack = Interpreter::ConvertArgsToCallStack(args, new_entity->evaluableNodeManager);

		new_entity->Execute(StringInternPool::NOT_A_STRING_ID, call_stack, false, calling_interpreter);
		return new_entity;
	}

	new_entity->SetRoot(code, true);

	//load any metadata like random seed
	std::string metadata_filename = resource_base_path + "." + FILE_EXTENSION_AMLG_METADATA;
	std::string metadata_base_path;
	std::string metadata_extension;
	EntityExternalInterface::LoadEntityStatus metadata_status;
	EvaluableNode *metadata = LoadResourcePath(metadata_filename, metadata_base_path, metadata_extension, &new_entity->evaluableNodeManager, escape_filename, metadata_status);
	if(metadata_status.loaded)
	{
		if(EvaluableNode::IsAssociativeArray(metadata))
		{
			EvaluableNode **seed = metadata->GetMappedChildNode(GetStringIdFromBuiltInStringId(ENBISI_rand_seed));
			if(seed != nullptr)
			{
				default_random_seed = EvaluableNode::ToStringPreservingOpcodeType(*seed);
				new_entity->SetRandomState(default_random_seed, true);
			}

			EvaluableNode **version = metadata->GetMappedChildNode(GetStringIdFromBuiltInStringId(ENBISI_version));
			if(version != nullptr)
			{
				auto [tostr_success, version_str] = EvaluableNode::ToString(*version);
				if(tostr_success)
				{
					auto [error_message, success] = AssetManager::ValidateVersionAgainstAmalgam(version_str);
					if(!success)
					{
						status.SetStatus(false, error_message, version_str);
						delete new_entity;
						return nullptr;
					}
				}
			}
		}

		new_entity->evaluableNodeManager.FreeNodeTree(metadata);
	}

	if(persistent)
		SetEntityPersistentPath(new_entity, resource_path);

	//load contained entities
	if(load_contained_entities)
	{
		//iterate over all files in directory
		resource_base_path.append("/");
		std::vector<std::string> file_names;
		Platform_GetFileNamesOfType(file_names, resource_base_path, file_type);
		for(auto &f : file_names)
		{
			std::string ce_path, ce_file_base, ce_extension;
			Platform_SeparatePathFileExtension(f, ce_path, ce_file_base, ce_extension);

			std::string entity_name;
			if(escape_contained_filenames)
				entity_name = FilenameEscapeProcessor::SafeUnescapeFilename(ce_file_base);
			else
				entity_name = ce_file_base;

			//don't escape filename again because it's already escaped in this loop
			std::string default_seed = new_entity->CreateRandomStreamFromStringAndRand(entity_name);
			std::string contained_resource_path = resource_base_path + ce_file_base + "." + ce_extension;
			Entity *contained_entity = LoadEntityFromResourcePath(contained_resource_path, file_type,
				false, true, false, escape_contained_filenames, default_seed, calling_interpreter, status);
			if(!status.loaded)
			{
				delete new_entity;
				return nullptr;
			}

			new_entity->AddContainedEntity(contained_entity, entity_name);
		}
	}

	return new_entity;
}

void AssetManager::CreateEntity(Entity *entity)
{
	if(entity == nullptr)
		return;

#ifdef MULTITHREAD_INTERFACE
	Concurrency::ReadLock lock(persistentEntitiesMutex);
#endif
	//early out if no persistent entities
	if(persistentEntities.size() == 0)
		return;

	Entity *cur = entity->GetContainer();
	std::string slice_path;
	std::string filename;
	std::string extension = defaultEntityExtension;
	std::string traversal_path = "";
	std::string escaped_entity_id = FilenameEscapeProcessor::SafeEscapeFilename(entity->GetId());
	std::string id_suffix = "/" + escaped_entity_id + "." + defaultEntityExtension;
	while(cur != nullptr)
	{
		const auto &pe = persistentEntities.find(cur);
		if(pe != end(persistentEntities))
		{
			Platform_SeparatePathFileExtension(pe->second, slice_path, filename, extension);
			//create contained entity directory in case it doesn't currently exist
			std::string new_path = slice_path + filename + traversal_path;
			std::error_code ec;
			std::filesystem::create_directory(new_path, ec);

			if(!ec)
			{
				new_path += id_suffix;
				StoreEntityToResourcePath(entity, new_path, extension, false, true, false, true, false);
			}
			else
			{
				std::cerr << "Could not create directory: " << new_path << std::endl;
			}

		}

		//don't need to continue and allocate extra traversal path if already at outermost entity
		Entity *cur_container = cur->GetContainer();
		if(cur_container == nullptr)
			break;

		escaped_entity_id = FilenameEscapeProcessor::SafeEscapeFilename(cur->GetId());
		traversal_path = "/" + escaped_entity_id + traversal_path;
		cur = cur_container;
	}
}

void AssetManager::SetRootPermission(Entity *entity, bool permission)
{
	if(entity == nullptr)
		return;

#ifdef MULTITHREAD_INTERFACE
	Concurrency::WriteLock lock(rootEntitiesMutex);
#endif

	if(permission)
		rootEntities.insert(entity);
	else
		rootEntities.erase(entity);
}

std::pair<std::string, bool> AssetManager::ValidateVersionAgainstAmalgam(std::string &version)
{
	auto semver = StringManipulation::Split(version, '-'); //split on postfix
	auto version_split = StringManipulation::Split(semver[0], '.'); //ignore postfix
	if(version_split.size() != 3)
		return std::make_pair("Invalid version number", false);

	uint32_t major = atoi(version_split[0].c_str());
	uint32_t minor = atoi(version_split[1].c_str());
	uint32_t patch = atoi(version_split[2].c_str());
	auto dev_build = std::string(AMALGAM_VERSION_SUFFIX);
	if(!dev_build.empty()
			|| (AMALGAM_VERSION_MAJOR == 0 && AMALGAM_VERSION_MINOR == 0 && AMALGAM_VERSION_PATCH == 0))
		; // dev builds don't check versions
	else if(
		(major > AMALGAM_VERSION_MAJOR) ||
		(major == AMALGAM_VERSION_MAJOR && minor > AMALGAM_VERSION_MINOR) ||
		(major == AMALGAM_VERSION_MAJOR && minor == AMALGAM_VERSION_MINOR && patch > AMALGAM_VERSION_PATCH))
	{
		std::string err_msg = "Parsing Amalgam that is more recent than the current version is not supported";
		std::cerr << err_msg << ", version=" << version << std::endl;
		return std::make_pair(err_msg, false);
	}
	else if(AMALGAM_VERSION_MAJOR > major)
	{
		std::string err_msg = "Parsing Amalgam that is older than the current major version is not supported";
		std::cerr << err_msg << ", version=" << version << std::endl;
		return std::make_pair(err_msg, false);
	}

	return std::make_pair("", true);
}

std::string AssetManager::GetEvaluableNodeSourceFromComments(EvaluableNode *en)
{
	std::string source;
	if(asset_manager.debugSources)
	{
		if(en->HasComments())
		{
			auto comment = en->GetCommentsString();
			auto first_line_end = comment.find('\n');
			if(first_line_end == std::string::npos)
				source = comment;
			else //copy up until newline
			{
				source = comment.substr(0, first_line_end);
				if(source.size() > 0 && source.back() == '\r')
					source.pop_back();
			}

			source += ": ";
		}
	}

	return source;
}

void AssetManager::DestroyPersistentEntity(Entity *entity)
{
	Entity *cur = entity;
	std::string slice_path;
	std::string filename;
	std::string extension;
	std::string traversal_path;
	std::error_code ec;

	//remove it as a persistent entity if it happened to be a direct one (erase won't do anything if it doesn't exist)
	persistentEntities.erase(entity);

	//delete any contained entities that are persistent
	for(auto contained_entity : entity->GetContainedEntities())
		DestroyPersistentEntity(contained_entity);

	//cover the case if any of this entity's containers were also persisted entities
	while(cur != nullptr)
	{
		const auto &pe = persistentEntities.find(cur);
		if(pe != end(persistentEntities))
		{
			//get metadata filename
			Platform_SeparatePathFileExtension(pe->second, slice_path, filename, extension);
			std::string total_filepath = slice_path + filename + traversal_path;

			//delete files
			std::filesystem::remove(total_filepath + "." + defaultEntityExtension, ec);
			if(ec)
				std::cerr << "Could not remove file: " << total_filepath + "." + defaultEntityExtension << std::endl;

			std::filesystem::remove(total_filepath + "." + FILE_EXTENSION_AMLG_METADATA, ec);
			if(ec)
				std::cerr << "Could not remove file: " << total_filepath + "." + FILE_EXTENSION_AMLG_METADATA << std::endl;

			//remove directory and all contents if it exists (command will fail if it doesn't exist)
			std::filesystem::remove_all(total_filepath, ec);
			if(ec)
				std::cerr << "Could not remove directory: " << total_filepath << std::endl;
		}

		std::string escaped_entity_id = FilenameEscapeProcessor::SafeEscapeFilename(cur->GetId());
		traversal_path = "/" + escaped_entity_id + traversal_path;

		cur = cur->GetContainer();
	}
}

void AssetManager::RemoveRootPermissions(Entity *entity)
{
	//remove permissions on any contained entities
	for(auto contained_entity : entity->GetContainedEntities())
		RemoveRootPermissions(contained_entity);

	SetRootPermission(entity, false);
}

void AssetManager::PreprocessFileNameAndType(std::string &resource_path,
	std::string &file_type, bool escape_resource_path,
	std::string &resource_base_path, std::string &complete_resource_path)
{
	//get file path based on the file being stored
	std::string path, file_base, extension;
	Platform_SeparatePathFileExtension(resource_path, path, file_base, extension);

	//escape the string if necessary, otherwise just use the regular one
	if(escape_resource_path)
	{
		resource_base_path = path + FilenameEscapeProcessor::SafeEscapeFilename(file_base);
		complete_resource_path = resource_base_path + "." + extension;
	}
	else
	{
		resource_base_path = path + file_base;
		complete_resource_path = resource_path;
	}

	if(file_type.empty())
		file_type = extension;
}
