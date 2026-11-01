/*
 * \brief  adb function: the phone as an adb device for unmodified host adb
 * \author Ivan Loskutov
 * \date   2026-10-24
 *
 * Client of the gadget driver's Usb_gadget session, the USB transport of
 * the adb engine (src/lib/adb_engine/engine.h: logcat, sync, a shell
 * child per shell stream). Every message is a header transfer and a
 * payload transfer of its own, the device never sends a zero-length packet.
 * Config: the engine's attributes 'name', 'model',
 * 'device'; the <vfs> node is what adb push/pull see, the <shell> node the
 * template of the shells (shells.h).
 */

/*
 * SPDX-License-Identifier: MIT
 */

#include <base/allocator_avl.h>
#include <base/attached_rom_dataspace.h>
#include <base/component.h>
#include <base/heap.h>
#include <usb_gadget/adb.h>
#include <usb_gadget/descriptor.h>
#include <util/reconstructible.h>

#include <engine.h>
#include <usb_gadget_session/connection.h>

namespace Usb_adb {

	using namespace Genode;

	struct Usb_transport;
	struct Main;

	using Packet = Usb_gadget::Packet_descriptor;
	using Header = Usb_gadget::Adb::Header;

	using Usb_gadget::Interface_descriptor;
	using Usb_gadget::Endpoint_descriptor;
	using Usb_gadget::Ss_companion_descriptor;

	/* interface ff/42/01 (adb) with bulk IN and bulk OUT, string 1 */
	static constexpr Interface_descriptor adb_interface {
		.number = 0, .num_endpoints = 2,
		.cls = 0xff, .subclass = 0x42, .protocol = 0x01, .string = 1 };

	/* function-local endpoints: 1 bulk IN, 2 bulk OUT */
	enum : unsigned char { ADB_BULK_IN = Usb_gadget::EP_IN | 1, ADB_BULK_OUT = 2 };

	static constexpr Endpoint_descriptor adb_endpoint(unsigned char  address,
	                                                  unsigned short max_packet)
	{
		return { .address = address, .attributes = Usb_gadget::EP_BULK,
		         .max_packet = max_packet, .interval = 0 };
	}

	/* descriptors at full and high speed */
	struct [[gnu::packed]] Adb_descriptors
	{
		Interface_descriptor interface = adb_interface;
		Endpoint_descriptor  bulk_in, bulk_out;
	};

	static constexpr Adb_descriptors adb_descriptors(unsigned short max_packet)
	{
		return { .bulk_in  = adb_endpoint(ADB_BULK_IN,  max_packet),
		         .bulk_out = adb_endpoint(ADB_BULK_OUT, max_packet) };
	}

	static constexpr Adb_descriptors adb_fs = adb_descriptors(64);
	static constexpr Adb_descriptors adb_hs = adb_descriptors(512);

	/* SuperSpeed: 1024-byte bulk packets, each endpoint with its companion */
	struct [[gnu::packed]] Adb_ss_descriptors
	{
		Interface_descriptor    interface = adb_interface;
		Endpoint_descriptor     bulk_in   = adb_endpoint(ADB_BULK_IN, 1024);
		Ss_companion_descriptor bulk_in_companion { };
		Endpoint_descriptor     bulk_out  = adb_endpoint(ADB_BULK_OUT, 1024);
		Ss_companion_descriptor bulk_out_companion { };
	};

	static constexpr Adb_ss_descriptors adb_ss { };

	static_assert(sizeof(Adb_descriptors)    == 23);
	static_assert(sizeof(Adb_ss_descriptors) == 35);

	static constexpr char adb_strings[] = "ADB Interface";
}


/*
 * The engine's messages on bulk IN
 */
struct Usb_adb::Usb_transport : Adb::Transport
{
	enum { EP_IN = 1 };

	Usb_gadget::Connection &_gadget;

	bool enabled = false;

	/* Main's I/O handler, which calls the engine's 'wakeup' under its guard */
	Signal_context_capability wakeup_sigh { };

	Usb_transport(Usb_gadget::Connection &gadget) : _gadget(gadget) { }

	void wakeup_soon() override { Signal_transmitter(wakeup_sigh).submit(); }

	bool _send_in(unsigned char const *data, unsigned len)
	{
		if (!_gadget.tx()->ready_to_submit(2)) {
			error("IN queue full, message dropped");
			return false;
		}
		Packet p = _gadget.alloc_packet(len, EP_IN);
		if (p.size() != len) {
			error("out of packet buffer, message dropped");
			return false;
		}
		unsigned char *d = (unsigned char *)_gadget.tx()->packet_content(p);
		for (unsigned i = 0; i < len; i++) d[i] = data[i];
		p.zlp = false;
		_gadget.tx()->submit_packet(p);
		return true;
	}

	/* Adb::Transport */
	bool ready() const override { return enabled; }

	void send(Header const &h, Const_byte_range_ptr const &payload) override
	{
		unsigned char raw[Usb_gadget::Adb::HEADER_SIZE];
		Usb_gadget::Adb::write(h, raw);
		if (_send_in(raw, sizeof(raw)) && h.data_length)
			_send_in((unsigned char const *)payload.start, h.data_length);
	}
};


struct Usb_adb::Main
{
	enum { EP_OUT = 2, OUT_PACKETS = 4,
	       OUT_SIZE = Usb_gadget::Adb::MAX_PAYLOAD,
	       EP0_AREA = 128, TX_BUF = 1024*1024 };

	Env &_env;

	Attached_rom_dataspace _config { _env, "config" };

	Heap          _heap     { _env.ram(), _env.rm() };
	Allocator_avl _tx_alloc { &_heap };

	Usb_gadget::Connection _gadget { _env, _tx_alloc, "adb", TX_BUF };

	Io_signal_handler<Main> _io_handler { _env.ep(), *this, &Main::_handle_io };

	unsigned _out_pending = 0;

	Usb_transport _transport { _gadget };

	/* constructed once the function is set up */
	Constructible<Adb::Engine> _engine { };

	/* the next OUT transfer is a header, or the payload of '_message' */
	bool   _expect_header = true;
	Header _message { };

	Packet _ep0_area { };

	void _fill_out()
	{
		while (_transport.enabled && _out_pending < OUT_PACKETS
		    && _gadget.tx()->ready_to_submit(2)) {
			Packet const p = _gadget.alloc_packet(OUT_SIZE, EP_OUT);
			if (p.size() != OUT_SIZE) return;
			_gadget.tx()->submit_packet(p);
			_out_pending++;
		}
	}

	void _handle_out(unsigned char const *data, unsigned len)
	{
		/* the host's ZLP after a payload that filled a whole OUT buffer */
		if (!len) return;

		if (_expect_header) {
			if (!Usb_gadget::Adb::parse(data, len, _message)) {
				warning("bad adb header (", len, " bytes), dropped");
				return;
			}
			if (_message.data_length) {
				_expect_header = false;
				return;
			}
			_engine->message(_message, Const_byte_range_ptr(nullptr, 0));
			return;
		}

		_expect_header = true;
		if (len != _message.data_length) {
			warning("payload of ", len, " bytes, header said ", _message.data_length);
			return;
		}

		_engine->message(_message, Const_byte_range_ptr((char const *)data, len));
	}

	void _ep0_stall()
	{
		Packet p(_ep0_area.offset(), 0);
		p.endpoint = 0;
		p.stall    = true;
		_gadget.tx()->submit_packet(p);
	}

	void _handle_acks()
	{
		auto &source = *_gadget.tx();

		while (source.ack_avail()) {
			Packet const p = source.get_acked_packet();

			if (p.endpoint == EP_OUT) {
				if (_out_pending) _out_pending--;
				if (p.status == Packet::OK)
					_handle_out((unsigned char const *)source.packet_content(p), p.actual);
			}

			/* the ep0 area stays allocated */
			if (p.endpoint != 0)
				source.release_packet(p);
		}
		_fill_out();
	}

	void _handle_events()
	{
		for (;;) {
			Usb_gadget::Event const e = _gadget.event();

			switch (e.type) {
			case Usb_gadget::Event::NONE:
				return;
			case Usb_gadget::Event::ENABLED:
				log("enabled by the host");
				_transport.enabled = true;
				_expect_header     = true;
				_engine->connected();
				_fill_out();
				break;
			case Usb_gadget::Event::DISABLED:
				log("disabled");
				_transport.enabled = false;
				_engine->disconnected();
				break;
			case Usb_gadget::Event::SETUP:
				/* adb has no class or vendor requests */
				_ep0_stall();
				break;
			default:
				break;
			}
		}
	}

	/*
	 * The engine may block in the VFS and dispatch I/O signals: OUT
	 * transfers that complete in the meantime are handled after it
	 */
	bool _in_io = false, _io_again = false;

	void _handle_io()
	{
		if (_in_io) { _io_again = true; return; }
		_in_io = true;

		do {
			_io_again = false;

			/* acknowledgements queued before an event are seen first */
			_handle_acks();
			_handle_events();

			/* IN progress, streams may go on */
			_engine->wakeup();
		} while (_io_again);

		_in_io = false;
	}

	Main(Env &env) : _env(env)
	{
		using Blob = Usb_gadget::Session::Blob;
		auto const blob = [] (auto const &a) { return Blob((char const *)&a, sizeof(a)); };

		_ep0_area = _gadget.alloc_packet(EP0_AREA, 0);

		_transport.wakeup_sigh = _io_handler;
		_gadget.event_sigh(_io_handler);
		_gadget.tx_channel()->sigh_ack_avail(_io_handler);
		_gadget.tx_channel()->sigh_ready_to_submit(_io_handler);

		if (!_gadget.function(blob(adb_fs), blob(adb_hs), blob(adb_ss),
		                      Blob(adb_strings, sizeof(adb_strings))))
			error("gadget driver rejected the adb descriptors");

		_config.node().with_sub_node("vfs",
			[&] (Node const &vfs) {
				_engine.construct(env, _heap, _transport, _config.node(), vfs, "usb"); },
			[&] {
				_engine.construct(env, _heap, _transport, _config.node(), Node(), "usb"); });
	}
};


void Component::construct(Genode::Env &env) { static Usb_adb::Main main(env); }
