// license:BSD-3-Clause
// copyright-holders:feos
/***************************************************************************

    exports.cpp

    API for using MAME as a shared library.

***************************************************************************/

#include "exports.h"
#include "../emu/emu.h"
#include "../frontend/mame/mame.h"
#include "../frontend/mame/clifront.h"
#include "../frontend/mame/luaengine.h"


//**************************************************************************
//  MACROS
//**************************************************************************

#ifdef MAME_SHARED_LIB
#ifdef WIN32
#define MAME_EXPORT extern "C" __declspec(dllexport)
#else
#define MAME_EXPORT extern "C" __attribute__((visibility("default"))) 
#endif
#else
#define MAME_EXPORT
#endif


extern int main(int argc, char *argv[]);
lua_engine *lua() { return mame_machine_manager::instance()->lua(); }
std::vector<std::unique_ptr<util::ovectorstream>> lua_strings_list;


//-------------------------------------------------
//  get_lua_value - execute lua code and return
//  the resulting value
//-------------------------------------------------

template <typename T>
T get_lua_value(const char *code)
{
	sol::object obj = lua()->load_string(code);

	if (obj.is<T>())
		return obj.as<T>();

	osd_printf_error("[LUA ERROR] return type mismatch: %s expected, got Lua %s\n",
		typeid(T).name(),
		(sol::type_name(lua()->sol(), obj.get_type())).c_str());

	throw false;
}


//**************************************************************************
//  CALLBACKS
//**************************************************************************

void(*frame_callback)(void) = nullptr;
void(*periodic_callback)(void) = nullptr;
void(*boot_callback)(void) = nullptr;
void(*log_callback)(int channel, int size, const char *buffer) = nullptr;

//-------------------------------------------------
//  export_frame_callback - inform the client that
//  a frame has ended
//-------------------------------------------------

void export_frame_callback()
{
	if (frame_callback)
		frame_callback();
}

//-------------------------------------------------
//  export_periodic_callback - inform the client
//  that mame is ready for a new lua command
//-------------------------------------------------

void export_periodic_callback()
{
	if (periodic_callback)
		periodic_callback();
}

//-------------------------------------------------
//  export_boot_callback - inform the client that
//  mame has started up and is ready to execute
//  lua code
//-------------------------------------------------

void export_boot_callback()
{
	if (boot_callback)
		boot_callback();
}

//-------------------------------------------------
//  output_callback - forward any textual output
//  to the client
//-------------------------------------------------

void export_output::output_callback(osd_output_channel channel, util::format_argument_pack<std::ostream> const &args)
{
	// fallback to the previous osd_output on the stack if no callback is attached
	if (!log_callback)
	{
		chain_output(channel, args);
		return;
	}

	std::ostringstream buffer;
	util::stream_format(buffer, args);
	log_callback((int)channel, buffer.str().length(), buffer.str().c_str());
};


//**************************************************************************
//  API
//**************************************************************************

//-------------------------------------------------
//  mame_launch - direct call to available main()
//-------------------------------------------------

MAME_EXPORT int mame_launch(int argc, char *argv[])
{
	return main(argc, argv);
}

//-------------------------------------------------
//  mame_set_boot_callback - subscribe to
//  emulator_info::frame_hook
//-------------------------------------------------

MAME_EXPORT void mame_set_frame_callback(void(*callback)(void))
{
	frame_callback = callback;
}

//-------------------------------------------------
//  mame_set_periodic_callback - subscribe to
//  emulator_info::periodic_check
//-------------------------------------------------

MAME_EXPORT void mame_set_periodic_callback(void(*callback)(void))
{
	periodic_callback = callback;
}

//-------------------------------------------------
//  mame_set_boot_callback - subscribe to
//  mame_machine_manager::autoboot_callback
//-------------------------------------------------

MAME_EXPORT void mame_set_boot_callback(void(*callback)(void))
{
	boot_callback = callback;
}

//-------------------------------------------------
//  mame_set_log_callback - subscribe to
//  osd_common_t::output_callback
//-------------------------------------------------

MAME_EXPORT void mame_set_log_callback(void(*callback)(int channel, int size, const char *buffer))
{
	log_callback = callback;
}

//-------------------------------------------------
//  mame_lua_execute - execute provided lua code
//-------------------------------------------------

MAME_EXPORT void mame_lua_execute(const char *code)
{
	lua()->load_string(code);
}

//-------------------------------------------------
//  mame_lua_get_int - execute provided lua code
//  and return the result as int
//-------------------------------------------------

MAME_EXPORT int mame_lua_get_int(const char *code)
{
	try
	{
		return get_lua_value<int>(code);
	}
	catch (...)
	{
		return 0;
	}
}

//-------------------------------------------------
//  mame_lua_get_double - execute provided lua code
//  and return the result as double
//-------------------------------------------------

MAME_EXPORT double mame_lua_get_double(const char *code)
{
	try
	{
		return get_lua_value<double>(code);
	}
	catch (...)
	{
		return .0;
	}
}

//-------------------------------------------------
//  mame_lua_get_bool - execute provided lua code
//  and return the result as bool
//-------------------------------------------------

MAME_EXPORT bool mame_lua_get_bool(const char *code)
{
	try
	{
		return get_lua_value<bool>(code);
	}
	catch (...)
	{
		return false;
	}
}

//-------------------------------------------------
//  mame_lua_get_string - execute provided lua code
//  and return the result as string buffer. must be
//  freed by the caller via mame_lua_free_string().
//  note that luaengine packs binary buffers as
//  strings too.
//-------------------------------------------------

MAME_EXPORT const char *mame_lua_get_string(const char *code, int *out_length)
{
	std::string string;

	try
	{
		string = get_lua_value<std::string>(code);
	}
	catch (...)
	{
		return nullptr;
	}

	auto buffer = std::make_unique<util::ovectorstream>();
	s32 length = string.length();

	buffer->reserve(length);
	buffer->clear();
	buffer->rdbuf()->clear();
	buffer->seekp(0);
	buffer->write(string.c_str(), length);

	*out_length = length;
	lua_strings_list.push_back(std::move(buffer));
	auto ret = lua_strings_list.back().get();
	return &ret->vec()[0];
}

//-------------------------------------------------
//  mame_lua_free_string - destruct ovectorstream
//  by dropping it from the list 
//-------------------------------------------------

MAME_EXPORT bool mame_lua_free_string(const char *pointer)
{
	for (auto it = lua_strings_list.begin(); it < lua_strings_list.end(); ++it)
	{
		auto buf = it->get();
		if (&buf->vec()[0] == pointer)
		{
			lua_strings_list.erase(it);
			return true;
		}
	}
	osd_printf_error("can't free buffer: no matching pointer found");
	return false;
}