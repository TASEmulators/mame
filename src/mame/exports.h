// license:BSD-3-Clause
// copyright-holders:feos, CasualPokePlayer
/***************************************************************************

    exports.h

    API for using MAME as a shared library.

***************************************************************************/

#ifndef __EXPORTS_H__
#define __EXPORTS_H__


#include "osdcore.h"


//**************************************************************************
//  CALLBACKS
//**************************************************************************

void export_periodic_callback();
void export_boot_callback();
time_t export_base_time_callback();

class export_output : public osd_output
{
public:
	virtual void output_callback(osd_output_channel channel, util::format_argument_pack<std::ostream> const &args) override;
};

#endif // __EXPORTS_H__