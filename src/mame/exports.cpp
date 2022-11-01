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
#define MAME_EXPORT extern "C" __attribute__((visibility("default"))) __attribute__((used))
#endif
#else
#define MAME_EXPORT
#endif


extern int main(int argc, char *argv[]);
lua_engine *lua() { return mame_machine_manager::instance()->lua(); }
address_space &space() { return mame_machine_manager::instance()->machine()->root_device().subdevice(":maincpu")->memory().space(AS_PROGRAM); }
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
//  COTHREAD MAGIC
//**************************************************************************

#include <libco.h>

static cothread_t co_control, co_emu;
static int main_ret;
static int main_argc;
static std::string* main_argv;

static void main_co()
{
	int argc = main_argc;
	char** argv = new char*[argc];
	for (int i = 0; i < argc; i++)
	{
		argv[i] = main_argv[i].data();
	}

	main_ret = main(argc, argv);
	delete[] argv;

	// switch back to the main cothread (a cothread does not return)
	co_switch(co_control);
}


//**************************************************************************
//  CALLBACKS
//**************************************************************************

void(*sound_callback)(void) = nullptr;
void(*log_callback)(int channel, int size, const char *buffer) = nullptr;

//-------------------------------------------------
//  export_periodic_callback - inform the client
//  that mame is ready for a new lua command
//-------------------------------------------------

void export_periodic_callback()
{
	co_switch(co_control);
}

//-------------------------------------------------
//  export_sound_callback - inform the client
//  that mame has already generated new sound samples
//-------------------------------------------------

void export_sound_callback()
{
	if (sound_callback)
		sound_callback();
}

//-------------------------------------------------
//  export_boot_callback - inform the client that
//  mame has started up and is ready to execute
//  lua code
//-------------------------------------------------

void export_boot_callback()
{
	co_switch(co_control);
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
	main_ret = 0;
	main_argc = argc;
	main_argv = new std::string[argc];
	for (int i = 0; i < argc; i++)
	{
		main_argv[i] = std::string(argv[i]);
	}
	
	co_control = co_active();
	co_emu = co_create(32768 * sizeof(void*), main_co);
	co_switch(co_emu);

	// if main returned, this will be non-zero
	return main_ret;
}

//-------------------------------------------------
//  mame_set_sound_callback - subscribe to
//  emulator_info::sound_hook()
//-------------------------------------------------

MAME_EXPORT void mame_set_sound_callback(void(*callback)(void))
{
	sound_callback = callback;
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

//-------------------------------------------------
//  mame_coswitch - switch back to the cothread
//  controlling main
//-------------------------------------------------

MAME_EXPORT void mame_coswitch()
{
	co_switch(co_emu);
}

//-------------------------------------------------
//  mame_read_byte - read byte from maincpu
//  program space
//-------------------------------------------------

MAME_EXPORT char mame_read_byte(unsigned int address)
{
	return space().read_byte(address);
}