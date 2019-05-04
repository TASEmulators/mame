// license:BSD-3-Clause
// copyright-holders:feos
/***************************************************************************

    exports.cpp

    API for using MAME as a shared library.

***************************************************************************/


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


//**************************************************************************
//  GLOBAL FUNCTIONS
//**************************************************************************

extern int main(int argc, char *argv[]);


//**************************************************************************
//  API
//**************************************************************************

MAME_EXPORT int mame_launch(int argc, char *argv[])
{
	return main(argc, argv);
}

MAME_EXPORT int mame_five()
{
	return 5;
}