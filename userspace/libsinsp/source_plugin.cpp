/*
Copyright (C) 2013-2018 Draios Inc dba Sysdig.

This file is part of sysdig.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

#ifndef _WIN32
#include <dlfcn.h>
#include <inttypes.h>
#endif
#include "sinsp.h"
#include "sinsp_int.h"
#include "filter.h"
#include "chisel.h"
#include "filterchecks.h"
#include "source_plugin_info.h"
#include "source_plugin.h"

#include <third-party/tinydir.h>

extern sinsp_filter_check_list g_filterlist;
extern vector<chiseldir_info>* g_plugin_dirs;

#define ENSURE_PLUGIN_EXPORT(_fn) if(m_source_info._fn == NULL) throw sinsp_exception("invalid source plugin: '" #_fn "' method missing");

///////////////////////////////////////////////////////////////////////////////
// source_plugin filter check implementation
// This class implements a dynamic filter check that acts as a bridge to the
// plugin simplified field extraction implementations
///////////////////////////////////////////////////////////////////////////////
class sinsp_filter_check_plugin : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_CNT = 0,
	};

	sinsp_filter_check_plugin()
	{
		m_info.m_name = "plugin";
		m_info.m_fields = NULL;
		m_info.m_nfields = 0;
		m_info.m_flags = filter_check_info::FL_NONE;
		m_cnt = 0;
	}

	int32_t parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering)
	{
		int32_t res = sinsp_filter_check::parse_field_name(str, alloc_state, needed_for_filtering);

		if(res != -1)
		{
			string val(str);
			size_t pos1 = val.find_first_of('[', 0);
			if(pos1 != string::npos)
			{
				size_t argstart = pos1 + 1;
				if(argstart < val.size())
				{
					m_argstr = val.substr(argstart);
					size_t pos2 = m_argstr.find_first_of(']', 0);
					m_argstr = m_argstr.substr(0, pos2);
					m_arg = (char*)m_argstr.c_str();
					return pos1 + pos2 + 2;
				}
			}
		}

		return res;
	}

	sinsp_filter_check* allocate_new()
	{
		sinsp_filter_check_plugin* np = new sinsp_filter_check_plugin();
		np->set_fields((filtercheck_field_info*)m_info.m_fields, m_info.m_nfields);
		np->set_name(m_info.m_name);
		np->m_id = m_id;
		np->m_type = m_type;
		np->m_source_info = m_source_info;

		return (sinsp_filter_check*)np;
	}

	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
	{
		//
		// Reject any event that is not generated by a plugin
		//
		if(evt->get_type() != PPME_PLUGINEVENT_E)
		{
			return NULL;
		}

		//
		// If this is a source plugin, reject events that have not generated by this
		// plugin specifically
		//
		sinsp_evt_param *parinfo;
		if(m_type == TYPE_SOURCE_PLUGIN)
		{
			parinfo = evt->get_param(0);
			ASSERT(parinfo->m_len == sizeof(int32_t));
			uint32_t pgid = *(int32_t *)parinfo->m_val;
			if(pgid != m_id)
			{
				return NULL;
			}
		}

		parinfo = evt->get_param(1);
		*len = 0;

		ppm_param_type type = m_info.m_fields[m_field_id].m_type;
		switch(type)
		{
		case PT_CHARBUF:
		{
			char* pret = m_source_info->extract_str(evt->get_num(), 
				m_field_id, m_arg, 
				(uint8_t*)parinfo->m_val, 
				parinfo->m_len);
			//if(pret == NULL)
			//{
			//	throw sinsp_exception("plugin's extract_as_string returned a NULL result");
			//}
			if(pret != NULL)
			{
				*len = strlen(pret);
			}
			else
			{
				*len = 0;
			}
			return (uint8_t*)pret;
		}
		default:
			ASSERT(false);
			throw sinsp_exception("plugin extract error unsupported field type " + to_string(type));
			break;
		}

		return NULL;
	}

	void set_name(string name)
	{
		m_info.m_name = name;
	}

	void set_fields(filtercheck_field_info* fields, uint32_t nfields)
	{
		m_info.m_fields = fields;
		m_info.m_nfields = nfields;
	}

	uint64_t m_cnt;
	uint32_t m_id;
	string m_argstr;
	char* m_arg = NULL;
	ss_plugin_info* m_source_info;
	ss_plugin_type m_type;
};

///////////////////////////////////////////////////////////////////////////////
// sinsp_source_plugin implementation
///////////////////////////////////////////////////////////////////////////////
sinsp_source_plugin::sinsp_source_plugin(sinsp* inspector)
{
	m_inspector = inspector;
}

sinsp_source_plugin::~sinsp_source_plugin()
{
	if(m_source_info.destroy != NULL)
	{
		m_source_info.destroy(m_source_info.state);
	}
}

void sinsp_source_plugin::configure(ss_plugin_info* plugin_info, char* config)
{
	int init_res;

	ASSERT(m_inspector != NULL);
	ASSERT(plugin_info != NULL);

	m_source_info = *plugin_info;

	ENSURE_PLUGIN_EXPORT(get_type);
	ENSURE_PLUGIN_EXPORT(get_last_error);

	m_type = (ss_plugin_type)m_source_info.get_type();

	if(m_type == TYPE_SOURCE_PLUGIN)
	{
		ENSURE_PLUGIN_EXPORT(get_id);
		ENSURE_PLUGIN_EXPORT(get_name);
		ENSURE_PLUGIN_EXPORT(get_description);
		ENSURE_PLUGIN_EXPORT(open);
		ENSURE_PLUGIN_EXPORT(close);
		ENSURE_PLUGIN_EXPORT(next);
		ENSURE_PLUGIN_EXPORT(event_to_string);
	}
	else if(m_type == TYPE_EXTRACTOR_PLUGIN)
	{
		ENSURE_PLUGIN_EXPORT(get_name);
		ENSURE_PLUGIN_EXPORT(get_description);
		ENSURE_PLUGIN_EXPORT(get_fields);
		ENSURE_PLUGIN_EXPORT(extract_str);
	}
	else
	{
		throw sinsp_exception("unknown plugin type " + to_string(m_type));
	}

	//
	// Initialize the plugin
	//
	if(m_source_info.init != NULL)
	{
		m_source_info.state = m_source_info.init(config, &init_res);
		if(init_res != SCAP_SUCCESS)
		{
			throw sinsp_exception(m_source_info.get_last_error());
		}
	}

	if(m_source_info.get_id)
	{
		m_id = m_source_info.get_id();
		m_source_info.id = m_id;
	}
	else
	{
		m_id = 0;
		m_source_info.id = 0;
	}

	//
	// If filter fields are exported by the plugin, the json from get_fields(), 
	// parse it, created our list of fields and feed them to a new sinsp_filter_check_plugin
	// extractor.
	//
	if(m_source_info.get_fields)
	{
		char* sfields = m_source_info.get_fields();
		if(sfields == NULL)
		{
			throw sinsp_exception(string("error in plugin ") + m_source_info.get_name() + ": get_fields returned a null string");
		}
		string json(sfields);
		SINSP_DEBUG("Parsing Container JSON=%s", json.c_str());
		Json::Value root;
		if(Json::Reader().parse(json, root) == false)
		{
			throw sinsp_exception(string("error in plugin ") + m_source_info.get_name() + ": get_fields returned an invalid JSON");
		}

		for(Json::Value::ArrayIndex j = 0; j < root.size(); j++)
		{
			filtercheck_field_info tf;
			tf.m_flags = EPF_NONE;

			const Json::Value &jvtype = root[j]["type"];
			string ftype = jvtype.asString();
			if(ftype == "")
			{
				throw sinsp_exception(string("error in plugin ") + m_source_info.get_name() + ": field JSON entry has no type");
			}
			const Json::Value &jvname = root[j]["name"];
			string fname = jvname.asString();
			if(fname == "")
			{
				throw sinsp_exception(string("error in plugin ") + m_source_info.get_name() + ": field JSON entry has no name");
			}
			const Json::Value &jvdesc = root[j]["desc"];
			string fdesc = jvdesc.asString();
			if(fdesc == "")
			{
				throw sinsp_exception(string("error in plugin ") + m_source_info.get_name() + ": field JSON entry has no desc");
			}

			strncpy(tf.m_name, fname.c_str(), sizeof(tf.m_name));
			strncpy(tf.m_description, fdesc.c_str(), sizeof(tf.m_description));
			tf.m_print_format = PF_DEC;
			if(ftype == "string")
			{
				tf.m_type = PT_CHARBUF;
			}
			else if(ftype == "integer")
			{
				tf.m_type = PT_INT64;
			}
			else if(ftype == "float")
			{
				tf.m_type = PT_DOUBLE;
			}
			else
			{
				throw sinsp_exception(string("error in plugin ") + m_source_info.get_name() + ": invalid field type " + ftype);
			}

			m_fields.push_back(tf);
		}

		m_filtercheck = new sinsp_filter_check_plugin();
		m_filtercheck->set_name(m_source_info.get_name() + string(" (plugin)"));
		m_filtercheck->set_fields((filtercheck_field_info*)&m_fields[0], 
			m_fields.size());
		m_filtercheck->m_id = m_id;
		m_filtercheck->m_type = m_type;
		m_filtercheck->m_source_info = &m_source_info;

		g_filterlist.add_filter_check(m_filtercheck);
	}
}

uint32_t sinsp_source_plugin::get_id()
{
	return m_id;
}

ss_plugin_type sinsp_source_plugin::get_type()
{
	return m_type;
}

void sinsp_source_plugin::add_plugin_dirs(sinsp* inspector, string sysdig_installation_dir)
{
	//
	// Add the default chisel directory statically configured by the build system
	//
	inspector->add_plugin_dir(sysdig_installation_dir + PLUGINS_INSTALLATION_DIR, false);

	//
	// Add the directories configured in the SYSDIG_PLUGIN_DIR environment variable
	//
	char* s_user_cdirs = getenv("SYSDIG_PLUGIN_DIR");

	if(s_user_cdirs != NULL)
	{
		vector<string> user_cdirs = sinsp_split(s_user_cdirs, ';');

		for(uint32_t j = 0; j < user_cdirs.size(); j++)
		{
			inspector->add_plugin_dir(user_cdirs[j], true);
		}
	}
}

void sinsp_source_plugin::list_plugins(sinsp* inspector)
{
	vector<sinsp_source_plugin*>* plist = inspector->get_plugins();

	//
	// Print the list to the screen
	//
	for(uint32_t j = 0; j < plist->size(); j++)
	{
		auto p = plist->at(j);

		if(p->get_type() == TYPE_SOURCE_PLUGIN)
		{
			printf("name: %s\n", p->m_source_info.get_name());
			printf("description: %s\n", p->m_source_info.get_description());
			printf("type: source plugin\n");
			printf("id: %" PRIu32 "\n\n", p->get_id());
		}
		else
		{
			printf("name: %s\n", p->m_source_info.get_name());
			printf("description: %s\n", p->m_source_info.get_description());
			printf("type: extractor plugin\n\n");
		}
	}
}

void* sinsp_source_plugin::getsym(void* handle, const char* name)
{
#ifdef _WIN32
	return GetProcAddress((HINSTANCE)handle, name);
#else
	return dlsym(handle, name);
#endif
}

//
// Polulate a source_plugin_info struct with the symbols coming from a dynamic library
//
bool sinsp_source_plugin::create_dynlib_source(string libname, OUT ss_plugin_info* info, OUT string* error)
{
#ifdef _WIN32
	HINSTANCE handle = LoadLibrary(libname.c_str());
#else
	void* handle = dlopen(libname.c_str(), RTLD_LAZY);
#endif
	if(handle == NULL)
	{
		*error = "error loading plugin " + libname + ": " + strerror(errno);
		return false;
	}

	*(void**)(&(info->init)) = getsym(handle, "plugin_init");
	*(void**)(&(info->destroy)) = getsym(handle, "plugin_destroy");
	*(void**)(&(info->get_last_error)) = getsym(handle, "plugin_get_last_error");
	*(void**)(&(info->get_type)) = getsym(handle, "plugin_get_type");
	*(void**)(&(info->get_id)) = getsym(handle, "plugin_get_id");
	*(void**)(&(info->get_name)) = getsym(handle, "plugin_get_name");
	*(void**)(&(info->get_description)) = getsym(handle, "plugin_get_description");
	*(void**)(&(info->get_fields)) = getsym(handle, "plugin_get_fields");
	*(void**)(&(info->open)) = getsym(handle, "plugin_open");
	*(void**)(&(info->close)) = getsym(handle, "plugin_close");
	*(void**)(&(info->next)) = getsym(handle, "plugin_next");
	*(void**)(&(info->event_to_string)) = getsym(handle, "plugin_event_to_string");
	*(void**)(&(info->extract_str)) = getsym(handle, "plugin_extract_str");

	return true;
}

//
// 1. Iterates through the plugin files on disk
// 2. Opens them and add them to the inspector
//
void sinsp_source_plugin::load_dynlib_plugins(sinsp* inspector)
{
	for(vector<chiseldir_info>::const_iterator it = g_plugin_dirs->begin();
		it != g_plugin_dirs->end(); ++it)
	{
		if(string(it->m_dir).empty())
		{
			continue;
		}

		tinydir_dir dir = {};

		tinydir_open(&dir, it->m_dir.c_str());

		while(dir.has_next)
		{
			tinydir_file file;
			tinydir_readfile(&dir, &file);

			string fname(file.name);
			string fpath(file.path);
			bool add_to_vector = false;
			ss_plugin_info si;
			string error;

			if(fname == "." || fname == "..")
			{
				goto nextfile;
			}

			if(create_dynlib_source(file.path, &si, &error) == false)
			{
				fprintf(stderr, "warning: cannot load plugin %s: %s\n", file.path, error.c_str());
				goto nextfile;
			}

			inspector->add_plugin(&si, NULL);

nextfile:
			tinydir_next(&dir);
		}

		tinydir_close(&dir);
	}
}

void sinsp_source_plugin::register_source_plugins(sinsp* inspector, string sysdig_installation_dir)
{
	add_plugin_dirs(inspector, sysdig_installation_dir);
	load_dynlib_plugins(inspector);

	//
	// ADD INTERNAL SOURCE PLUGINS HERE.
	// We don't have any yet.
	//
}
