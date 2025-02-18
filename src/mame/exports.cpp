// license:BSD-3-Clause
// copyright-holders:feos, CasualPokePlayer
/***************************************************************************

    exports.cpp

    API for using MAME as a shared library.

***************************************************************************/

#include "exports.h"
#include "emu.h"
#include "fileio.h"
#include "mame.h"
#include "clifront.h"
#include "luaengine.h"
#include "ui/ui.h"
#include "ui/info.h"
#include "corestr.h"


//**************************************************************************
//  MACROS
//**************************************************************************

#if !defined(MAME_WATERBOX)
#error MAME_WATERBOX not defined
#endif

#define MAME_EXPORT extern "C" __attribute__((visibility("default"))) __attribute__((used))

extern int main(int argc, char *argv[]);
static inline lua_engine *lua() { return mame_machine_manager::instance()->lua(); }
static inline device_t &root_device() { return mame_machine_manager::instance()->machine()->root_device(); }
static inline address_space &space() { return mame_machine_manager::instance()->machine()->root_device().subdevice(":maincpu")->memory().space(AS_PROGRAM); }
static inline const ioport_list &ports() { return mame_machine_manager::instance()->machine()->ioport().ports(); }
static inline sound_manager &sound() { return mame_machine_manager::instance()->machine()->sound(); }
static inline video_manager &video() { return mame_machine_manager::instance()->machine()->video(); }
static inline ui::machine_info &machine_info() { return mame_machine_manager::instance()->ui().machine_info(); }


//-------------------------------------------------
//  lua_run - execute lua code and return
//  the resulting value as an object
//-------------------------------------------------

static sol::object lua_run(const char *code)
{
	auto &l = *lua();
	auto lr = l.load_string(code);

	if (lr.valid())
	{
		auto pfr = (lr.get<sol::protected_function>())();
		if (pfr.valid())
			return pfr;

		sol::error err = pfr;
		osd_printf_error("[LUA ERROR] in run: %s\n", err.what());
	}
	else
	{
		osd_printf_error("[LUA ERROR] %s loading Lua script\n", sol::to_string(lr.status()));
	}

	return sol::make_object(l.sol(), sol::lua_nil);
}

//-------------------------------------------------
//  get_lua_value - execute lua code and return
//  the resulting value as the expected type,
//  or std::nullopt if an error occurs
//-------------------------------------------------

template <typename T>
static inline std::optional<T> get_lua_value(const char *code)
{
	auto obj = lua_run(code);

	if (obj.is<T>())
		return obj.as<T>();

	osd_printf_error("[LUA ERROR] return type mismatch: %s expected, got Lua %s\n",
		typeid(T).name(),
		(sol::type_name(lua()->sol(), obj.get_type())).c_str());

	return std::nullopt;
}

//**************************************************************************
//  COTHREAD MAGIC
//**************************************************************************

#include <libco.h>

static cothread_t co_control, co_emu;
static int main_ret;
static int main_argc;
static std::string *main_argv;

static void main_co()
{
	auto argc = main_argc;
	auto argv = new char*[argc];
	for (int i = 0; i < argc; i++)
	{
		argv[i] = main_argv[i].data();
	}

	main_ret = main(argc, argv);
	delete []argv;
	delete []main_argv;

	// main has returned, this is probably a crash.
	// a cothread does not return, so we need to
	// switch back to the host cothread. if this
	// happened after bootup, we need this to ensure
	// the host cothread will be immediately
	// switched back every frame advance call,
	// hence the while true loop
	while (true)
	{
		co_switch(co_control);
	}
}


//**************************************************************************
//  CALLBACKS
//**************************************************************************

static void(*log_callback)(int channel, int size, const char *buffer) = nullptr;
static time_t(*base_time_callback)() = nullptr;
static void(*input_poll_callback)() = nullptr;

static bool lag_flag = false;

//-------------------------------------------------
//  export_periodic_callback - inform the client
//  that mame is ready for a new lua command
//-------------------------------------------------

void export_periodic_callback()
{
	co_switch(co_control);
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
//  export_base_time_callback - request the base
//  emulation time from the client
//-------------------------------------------------

time_t export_base_time_callback()
{
	if (base_time_callback)
		return base_time_callback();

	return 0;
}

//-------------------------------------------------
//  export_input_poll_callback - inform the client
//  that mame has polled controller input ("unlag")
//-------------------------------------------------

void export_input_poll_callback()
{
	if (input_poll_callback)
		input_poll_callback();

	lag_flag = false;
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
//  mame_launch - create a new cothread which will
//  call the available main(). the host cothread will
//  be switched back to on export_boot_callback, or
//  in case main returns. If the latter occurs,
//  non-zero will be returned
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
	co_emu = co_create(65536 * sizeof(void*), main_co);
	co_switch(co_emu);

	return main_ret;
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
//  mame_set_base_time_callback - set a callback
//  which returns the base time emulation uses
//-------------------------------------------------

MAME_EXPORT void mame_set_base_time_callback(time_t(*callback)())
{
	base_time_callback = callback;
}

//-------------------------------------------------
//  mame_set_input_poll_callback - set a callback
//  which is called every input poll
//-------------------------------------------------

MAME_EXPORT void mame_set_input_poll_callback(void(*callback)())
{
	input_poll_callback = callback;
}

//-------------------------------------------------
//  mame_lua_execute - execute provided lua code
//-------------------------------------------------

MAME_EXPORT void mame_lua_execute(const char *code)
{
	lua_run(code);
}

//-------------------------------------------------
//  mame_lua_get_bool - execute provided lua code
//  and return the result as bool
//-------------------------------------------------

MAME_EXPORT bool mame_lua_get_bool(const char *code)
{
	return get_lua_value<bool>(code).value_or(false);
}

//-------------------------------------------------
//  mame_lua_get_int - execute provided lua code
//  and return the result as int
//-------------------------------------------------

MAME_EXPORT int mame_lua_get_int(const char *code)
{
	return get_lua_value<int>(code).value_or(0);
}

//-------------------------------------------------
//  mame_lua_get_long - execute provided lua code
//  and return the result as long
//-------------------------------------------------

MAME_EXPORT long mame_lua_get_long(const char *code)
{
	return get_lua_value<long>(code).value_or(0L);
}

//-------------------------------------------------
//  mame_lua_get_double - execute provided lua code
//  and return the result as double
//-------------------------------------------------

MAME_EXPORT double mame_lua_get_double(const char *code)
{
	return get_lua_value<double>(code).value_or(.0);
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
	auto string = get_lua_value<std::string>(code);

	if (!string.has_value())
	{
		return nullptr;
	}

	int length = string->length();
	auto *ret = new char[length + 1];
	std::memcpy(ret, string->c_str(), length + 1);
	*out_length = length;
	return ret;
}

//-------------------------------------------------
//  mame_lua_free_string - delete char array
//-------------------------------------------------

MAME_EXPORT void mame_lua_free_string(const char *pointer)
{
	delete []pointer;
}

//-------------------------------------------------
//  mame_coswitch - switch back to the emu
//  cothread. this will also set the lag flag
//  and return it. if the emu cothread polls
//  input, then the returned lag flag will be false
//-------------------------------------------------

MAME_EXPORT bool mame_coswitch()
{
	lag_flag = true;
	co_switch(co_emu);
	return lag_flag;
}

//-------------------------------------------------
//  mame_read_byte - read byte from maincpu
//  program space
//-------------------------------------------------

MAME_EXPORT char mame_read_byte(unsigned int address)
{
	return space().read_byte(address);
}

//-------------------------------------------------
//  mame_input_get_field_ptr - get field pointer
//  by tag and field name
//-------------------------------------------------

MAME_EXPORT ioport_field *mame_input_get_field_ptr(const char *tag, const char *field_name)
{
	const auto port(ports().find(std::string(tag)));
	if (port == ports().end())
		return nullptr;

	auto fn = std::string(field_name);

	for (auto &f : port->second->fields())
	{
		if (f.name() == fn)
			return &f;
	}

	return nullptr;
}

//-------------------------------------------------
//  mame_set_inputs - set inputs using a client
//  provided ioport_field pointer array
//-------------------------------------------------

MAME_EXPORT void mame_input_set_fields(ioport_field **fields, unsigned int *inputs, int length)
{
	for (int i = 0; i < length; i++)
	{
		fields[i]->set_value(inputs[i]);
	}
}

//-------------------------------------------------
//  mame_sound_get_samples - get sound samples
//  and return sample count. sample buffer should
//  be able to hold at least 1 second of samples
//-------------------------------------------------

MAME_EXPORT int mame_sound_get_samples(short *buffer)
{
	auto &s = sound();
	s.manual_update();
	s.samples(buffer);
	return s.sample_count();
}

//-------------------------------------------------
//  mame_video_get_dimensions - get video dimensions
//-------------------------------------------------

MAME_EXPORT void mame_video_get_dimensions(int *width, int *height)
{
	video().compute_snapshot_size(*width, *height);
}

//-------------------------------------------------
//  mame_video_get_pixels - get video pixels
//-------------------------------------------------

MAME_EXPORT void mame_video_get_pixels(unsigned int *buffer)
{
	video().pixels(buffer);
}

static std::string nvram_filename(device_t &device)
{
	std::ostringstream result;

	if (device.owner() != nullptr)
	{
		const char *software = nullptr;
		for (device_t *dev = &device; dev->owner() != nullptr; dev = dev->owner())
		{
			device_image_interface *intf;
			if (dev->interface(intf))
			{
				software = intf->basename_noext();
				break;
			}
		}
		if (software != nullptr && *software != '\0')
			result << ";" << software;

		std::string tag(device.tag());
		tag.erase(0, 1);
		strreplacechr(tag,':', '_');
		result << ";" << tag;
	}

	return result.str();
}

//-------------------------------------------------
//  mame_nvram_get_filenames - get nvram filenames
//-------------------------------------------------

MAME_EXPORT void mame_nvram_get_filenames(void(*filename_callback)(const char *filename))
{
	for (device_nvram_interface &nvram : nvram_interface_enumerator(root_device()))
	{
		if (nvram.nvram_can_save())
		{
			filename_callback(nvram_filename(nvram.device()).c_str());
		}
	}
}

//-------------------------------------------------
//  mame_nvram_save - save nvram
//-------------------------------------------------

MAME_EXPORT void mame_nvram_save()
{
	for (device_nvram_interface &nvram : nvram_interface_enumerator(root_device()))
	{
		if (nvram.nvram_can_save())
		{
			emu_file file("", OPEN_FLAG_WRITE);
			if (!file.open(nvram_filename(nvram.device())))
			{
				if (!nvram.nvram_save(file))
					osd_printf_error("Error writing NVRAM file %s\n", file.filename());
				file.close();
			}
		}
	}
}

//-------------------------------------------------
//  mame_nvram_load - load nvram
//-------------------------------------------------

MAME_EXPORT void mame_nvram_load()
{
	for (device_nvram_interface &nvram : nvram_interface_enumerator(root_device()))
	{
		emu_file file("", OPEN_FLAG_READ);
		if (nvram.nvram_backup_enabled() && !file.open(nvram_filename(nvram.device())))
		{
			if (!nvram.nvram_load(file))
				osd_printf_error("Error reading NVRAM file %s\n", file.filename());
			file.close();
		}
		else
			nvram.nvram_reset();
	}
}

//-------------------------------------------------
//  mame_info_get_warnings_string - get warnings string
//-------------------------------------------------

MAME_EXPORT void mame_info_get_warnings_string(void(*info_callback)(const char *info))
{
	info_callback(machine_info().warnings_string().c_str());
}
