/*
 * \brief  adb transport: how the engine's messages reach the host
 * \author Ivan Loskutov
 * \date   2026-10-24
 *
 * Implemented by the front ends (USB: usb_adb, TCP later). The transport
 * hands every complete host message to 'Adb::Engine::message' and sends
 * the engine's messages, header and payload, in order.
 */

/*
 * SPDX-License-Identifier: MIT
 */

#ifndef _ADB_ENGINE__TRANSPORT_H_
#define _ADB_ENGINE__TRANSPORT_H_

#include <util/interface.h>
#include <util/string.h>
#include <usb_gadget/adb.h>

namespace Adb { struct Transport; }


struct Adb::Transport : Genode::Interface
{
	/**
	 * Whether 'send' may be called now
	 */
	virtual bool ready() const = 0;

	/**
	 * Send one message, 'payload' holds 'data_length' bytes
	 */
	virtual void send(Usb_gadget::Adb::Header const &,
	                  Genode::Const_byte_range_ptr const &payload) = 0;

	/**
	 * Call the engine's 'wakeup' soon, from where the transport handles
	 * its own signals (under its guard, never nested in the engine)
	 */
	virtual void wakeup_soon() = 0;
};

#endif /* _ADB_ENGINE__TRANSPORT_H_ */
