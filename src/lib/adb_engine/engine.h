/*
 * \brief  adb protocol engine, independent of the transport
 * \author Ivan Loskutov
 * \date   2026-10-24
 *
 * Answers the host's messages, which the transport hands over complete
 * ('message'), and sends through the transport (transport.h). Streams:
 * logcat reads the log history and new lines from a Terminal session
 * ("logcat", log_ring); with "-d" it ends after the history. Without that
 * session the config's 'logcat_file' (a path of the 'files_vfs', polled
 * every 200 ms, log_file.h; 'logcat_window' bytes of its end, default
 * 1M) is read the same way; without both logcat prints "logcat: no log
 * source" and ends. "sync:"
 * (adb push/pull) works on the VFS of the 'files_vfs' node (sync.h).
 * Every shell stream ("shell:", "shell:ls", "shell,v2,pty:", "exec:ls")
 * gets a child of its own (shells.h, the config's <shell> template). A
 * shell,v2 stream carries packets: input as STDIN, output as STDOUT, the
 * exit value as EXIT before CLSE (logcat on a v2 stream too); a v1 stream
 * carries raw bytes and ends with CLSE. Other streams are refused (CLSE).
 * One WRTE per stream waits for the host's OKAY. Config attributes 'name',
 * 'model', 'device': the ro.product.* values of the banner.
 */

/*
 * SPDX-License-Identifier: MIT
 */

#ifndef _ADB_ENGINE__ENGINE_H_
#define _ADB_ENGINE__ENGINE_H_

#include <adb/service.h>
#include <adb/shell_v2.h>
#include <base/heap.h>
#include <base/node.h>
#include <os/vfs.h>
#include <terminal_session/connection.h>
#include <timer_session/connection.h>
#include <usb_gadget/adb.h>
#include <util/reconstructible.h>

#include <log_file.h>
#include <shells.h>
#include <sync.h>
#include <transport.h>

namespace Adb { class Engine; }


class Adb::Engine
{
	private:

		using Header = Usb_gadget::Adb::Header;
		using Text   = Genode::String<64>;
		using Node   = Genode::Node;

		enum { MAX_STREAMS = 4, CHUNK = 4096 };

		/*
		 * Terminal session "logcat" (the log source), if there is one
		 *
		 * Requested with 'try_session': a denied session refuses the stream.
		 * A Terminal::Connection would stop the component (the env's
		 * 'session' sleeps forever on a denial).
		 */
		class Log_terminal
		{
			private:

				Genode::Env &_env;

				Genode::Parent::Client _parent_client { };

				Genode::Id_space<Genode::Parent::Client>::Element const
					_id { _parent_client, _env.id_space() };

				Genode::Constructible<Terminal::Session_client> _terminal { };

				/*
				 * Noncopyable
				 */
				Log_terminal(Log_terminal const &);
				Log_terminal &operator = (Log_terminal const &);

			public:

				Log_terminal(Genode::Env &env) : _env(env)
				{
					using Genode::Session_error;

					Genode::Ram_quota ram  { 10*1024 };
					Genode::Cap_quota caps { Terminal::Session::CAP_QUOTA };

					for (;;) {
						Genode::String<128> const args("label=\"logcat\", ram_quota=", ram,
						                               ", cap_quota=", caps);
						Genode::Env::Session_result const result =
							_env.try_session(Terminal::Session::service_name(), _id.id(),
							                 Genode::Parent::Session_args(args.string()),
							                 Genode::Affinity());

						if (result == Session_error::INSUFFICIENT_RAM) { ram.value  += 4096; continue; }
						if (result == Session_error::INSUFFICIENT_CAPS) { caps.value += 4;   continue; }

						result.with_result(
							[&] (Genode::Session_capability cap) {
								Genode::Capability<Terminal::Session> const terminal =
									Genode::static_cap_cast<Terminal::Session>(cap);
								_terminal.construct(_env.rm(), terminal);
								Terminal::Connection::wait_for_connection(terminal); },
							[&] (Session_error) { });
						return;
					}
				}

				~Log_terminal()
				{
					if (!_terminal.constructed()) return;
					_terminal.destruct();
					_env.close(_id.id());
				}

				bool connected() const { return _terminal.constructed(); }

				/**
				 * The session, only if 'connected'
				 */
				Terminal::Session_client &terminal() { return *_terminal; }
		};

		Genode::Env       &_env;
		Genode::Allocator &_alloc;
		Transport         &_transport;

		Text const _name, _model, _device;

		/* a copy, the caller's node may go away */
		Genode::Buffered_node const _files_vfs;

		/* the log source without a Terminal "logcat", empty: none */
		Log_file::Path const _logcat_file;

		Genode::size_t const _logcat_window;

		/* paths of the config are taken relative to the VFS root, as sync's */
		static Log_file::Path _rel(Log_file::Path const &path)
		{
			char const *s = path.string();
			while (*s == '/') s++;
			return Log_file::Path(s);
		}

		/* what adb push/pull see, built at the first sync stream */
		Genode::Constructible<Genode::Root_directory> _root { };

		Genode::Root_directory &_vfs()
		{
			if (!_root.constructed()) {
				if (_files_vfs.has_type("empty"))
					Genode::warning("no <vfs> configured, push/pull see nothing");
				_root.construct(_env, _alloc, _files_vfs);
			}
			return *_root;
		}

		Genode::Io_signal_handler<Engine> _io_handler {
			_env.ep(), *this, &Engine::_handle_io };

		/*
		 * The 'logcat_file' is read only in 'wakeup', which the transport
		 * calls under its own guard (no host message, no disconnect while
		 * the read blocks in the VFS and I/O signals come in). The timer
		 * (built at the first following logcat) and a drained chunk only
		 * mark the read due and ask the transport for a 'wakeup'.
		 */
		enum { LOG_POLL_US = 200*1000 };

		Genode::Constructible<Timer::Connection> _timer { };

		Genode::Signal_handler<Engine> _log_poll_handler {
			_env.ep(), *this, &Engine::_handle_log_poll };

		bool _log_poll_armed = false;
		bool _log_read_due   = false;

		bool _connected = false;

		/* constructed with the config's <shell> template */
		Genode::Constructible<Shells> _shells { };

		unsigned _max_payload = Usb_gadget::Adb::MAX_PAYLOAD;

		struct Stream
		{
			unsigned local  = 0;   /* our id, 0: unused */
			unsigned remote = 0;   /* the host's id */
			bool     dump   = false;
			bool     wait_okay = false;
			bool     no_log = false;   /* logcat without a log source */

			Genode::Constructible<Log_terminal> log  { };
			Genode::Constructible<Log_file>     log_file { };
			Genode::Constructible<Sync>         sync { };

			/* the host's last WRTE waits for our OKAY until it is consumed */
			bool okay_pending = false;

			bool shell     = false;   /* has a child in '_shells' */
			bool v2        = false;   /* shell protocol v2 packets */
			bool exit_sent = false;   /* v2: the EXIT packet is out */

			Genode::Constructible<Shell_v2::Reader> v2_in { };
		};

		Stream   _streams[MAX_STREAMS] { };
		unsigned _next_id = 1;

		/* output of one WRTE, per kind of stream (v2: a packet header more) */
		char          _shell_out[CHUNK + Shell_v2::PACKET_HEADER];
		unsigned char _sync_out[Usb_gadget::Adb::MAX_PAYLOAD];
		char          _log_out[CHUNK];
		char          _log_packet[CHUNK + Shell_v2::PACKET_HEADER];

		Stream *_stream(unsigned local)
		{
			for (Stream &s : _streams)
				if (local && s.local == local) return &s;
			return nullptr;
		}

		void _send(Header const &h, void const *payload)
		{
			_transport.send(h, Genode::Const_byte_range_ptr((char const *)payload,
			                                                h.data_length));
		}

		void _close(Stream &s)
		{
			s.log.destruct();
			s.log_file.destruct();
			s.sync.destruct();
			s.v2_in.destruct();
			if (s.shell) _shells->close(s.local);
			s.shell = s.v2 = s.exit_sent = false;
			s.local = s.remote = 0;
			s.dump = s.wait_okay = s.okay_pending = s.no_log = false;
		}

		void _send_wrte(Stream &s, char const *data, unsigned n)
		{
			using namespace Usb_gadget::Adb;
			_send(wrte(s.local, s.remote, n, checksum((unsigned char const *)data, n)), data);
			s.wait_okay = true;
		}

		/* the stream's end: v2 sends the exit value first, CLSE after its OKAY */
		void _finish(Stream &s, int exit_value)
		{
			if (s.v2 && !s.exit_sent) {
				char const value = char(exit_value);
				char packet[Shell_v2::PACKET_HEADER + 1];
				unsigned const n = unsigned(Shell_v2::write(Shell_v2::EXIT, &value, 1,
				                                            packet, sizeof(packet)));
				s.exit_sent = true;
				_send_wrte(s, packet, n);
				return;
			}
			_send(Usb_gadget::Adb::clse(s.local, s.remote), nullptr);
			_close(s);
		}

		void _close_all()
		{
			for (Stream &s : _streams)
				if (s.local) _close(s);
		}

		/**
		 * Send what 's' has, one WRTE at a time
		 */
		void _pump(Stream &s)
		{
			using namespace Usb_gadget::Adb;
			using Genode::min;

			if (!s.local || !_transport.ready())
				return;

			/* a shell's input goes on while the host has not taken its output */
			if (s.shell && s.okay_pending && _shells->input_room(s.local, _max_payload)) {
				_send(okay(s.local, s.remote), nullptr);
				s.okay_pending = false;
			}

			if (s.wait_okay)
				return;

			if (s.shell) {
				Genode::size_t const header = s.v2 ? Shell_v2::PACKET_HEADER : 0;
				Genode::size_t const max    = min((Genode::size_t)CHUNK,
				                                  (Genode::size_t)_max_payload - header);
				unsigned n = 0;
				_shells->with_output(s.local, [&] (char const *data, Genode::size_t len) {
					len = min(len, max);
					if (s.v2)
						n = unsigned(Shell_v2::write(Shell_v2::STDOUT, data, len,
						                             _shell_out, sizeof(_shell_out)));
					else {
						Genode::memcpy(_shell_out, data, len);
						n = unsigned(len);
					}
					return len; });
				if (n) {
					_send_wrte(s, _shell_out, n);
					return;
				}
				Optional<int> const exit = _shells->exit_value(s.local);
				if (exit.valid)
					_finish(s, exit.value);
				return;
			}

			if (s.sync.constructed()) {
				if (s.okay_pending && s.sync->consumed()) {
					_send(okay(s.local, s.remote), nullptr);
					s.okay_pending = false;
				}
				unsigned const n = s.sync->take(_sync_out,
				                                min((unsigned)sizeof(_sync_out), _max_payload));
				if (n) {
					_send(wrte(s.local, s.remote, n, checksum(_sync_out, n)), _sync_out);
					s.wait_okay = true;
					return;
				}
				if (s.sync->finished()) {
					_send(clse(s.local, s.remote), nullptr);
					_close(s);
				}
				return;
			}

			if (!s.log.constructed() && !s.log_file.constructed()) {
				/* logcat without a log source: its message is out */
				if (s.no_log) _finish(s, 1);
				return;
			}

			Genode::size_t const header = s.v2 ? Shell_v2::PACKET_HEADER : 0;
			Genode::size_t const max = min((Genode::size_t)CHUNK,
			                               (Genode::size_t)_max_payload - header);
			Genode::size_t n = 0;
			if (s.log_file.constructed()) {
				n = s.log_file->take(_log_out, max);
				if (!n && s.log_file->more()) _log_read_soon();
			} else {
				Terminal::Session_client &log = s.log->terminal();
				n = log.avail() ? log.read(_log_out, max) : 0;
			}

			if (n && s.v2) {
				_send_wrte(s, _log_packet,
				           unsigned(Shell_v2::write(Shell_v2::STDOUT, _log_out, n,
				                                    _log_packet, sizeof(_log_packet))));
				return;
			}
			if (n) {
				_send_wrte(s, _log_out, unsigned(n));
				return;
			}

			if (s.dump && (!s.log_file.constructed() || s.log_file->finished()))
				_finish(s, 0);
		}

		void _pump_all()
		{
			for (Stream &s : _streams) _pump(s);
		}

		Stream *_new_stream(Header const &open)
		{
			for (Stream &s : _streams)
				if (!s.local) {
					s.local  = _next_id++;
					s.remote = open.arg0;
					return &s;
				}

			Genode::warning("too many streams, refused");
			_send(Usb_gadget::Adb::refuse(open), nullptr);
			return nullptr;
		}

		void _open_sync(Header const &open)
		{
			Stream *s = _new_stream(open);
			if (!s) return;

			s->sync.construct(_vfs());
			_send(Usb_gadget::Adb::okay(s->local, s->remote), nullptr);
		}

		/* a child of its own, interactive or a one-shot command */
		void _open_shell(Header const &open, Service const &service)
		{
			Stream *s = _new_stream(open);
			if (!s) return;

			if (!_shells->open(s->local, service)) {
				_send(Usb_gadget::Adb::refuse(open), nullptr);
				_close(*s);
				return;
			}

			s->shell = true;
			s->v2    = service.v2;
			if (s->v2) s->v2_in.construct();
			_send(Usb_gadget::Adb::okay(s->local, s->remote), nullptr);
		}

		void _open_logcat(Header const &open, bool dump, bool v2)
		{
			using namespace Usb_gadget::Adb;

			Stream *free = _new_stream(open);
			if (!free) return;

			free->dump   = dump;
			free->v2     = v2;

			/*
			 * Without a "logcat" Terminal route (MüesliOS) the stream says
			 * so and ends with exit value 1. A CLSE instead of the OKAY
			 * would not do: host adb retries a refused logcat for ever
			 * ("- waiting for device -").
			 */
			free->log.construct(_env);
			if (!free->log->connected()) {
				free->log.destruct();

				if (_open_log_file(*free))
					return;

				Genode::warning("no log source (Terminal \"logcat\" or logcat_file), logcat ends");
				free->no_log = true;
				_send(okay(free->local, free->remote), nullptr);

				static char const message[] = "logcat: no log source\n";
				unsigned const len = sizeof(message) - 1;
				if (v2)
					_send_wrte(*free, _log_packet,
					           unsigned(Shell_v2::write(Shell_v2::STDERR, message, len,
					                                    _log_packet, sizeof(_log_packet))));
				else
					_send_wrte(*free, message, len);
				return;
			}
			free->log->terminal().read_avail_sigh(_io_handler);

			_send(okay(free->local, free->remote), nullptr);
			_pump(*free);
		}

		/*
		 * logcat of the 'logcat_file': false if there is none or it is
		 * not there. Its first read comes with the next 'wakeup'.
		 */
		bool _open_log_file(Stream &s)
		{
			using namespace Usb_gadget::Adb;

			if (_logcat_file == "")
				return false;

			s.log_file.construct(_alloc, _vfs(), _logcat_file, s.dump, _logcat_window);
			if (!s.log_file->exists()) {
				Genode::warning("logcat_file ", _logcat_file, " is not there");
				s.log_file.destruct();
				return false;
			}

			_send(okay(s.local, s.remote), nullptr);
			_log_read_soon();
			if (!s.dump) _arm_log_poll();
			return true;
		}

		void _log_read_soon()
		{
			if (_log_read_due) return;
			_log_read_due = true;
			_transport.wakeup_soon();
		}

		void _arm_log_poll()
		{
			if (_log_poll_armed) return;

			if (!_timer.constructed()) {
				_timer.construct(_env);
				_timer->sigh(_log_poll_handler);
			}
			_timer->trigger_once(LOG_POLL_US);
			_log_poll_armed = true;
		}

		static bool _following(Stream const &s)
		{
			return s.local && s.log_file.constructed() && !s.dump;
		}

		/* the poll interval is over */
		void _handle_log_poll()
		{
			_log_poll_armed = false;

			bool any = false;
			for (Stream const &s : _streams) any |= _following(s);

			if (any) {
				_log_read_soon();
				_arm_log_poll();
			}
		}

		/* from 'wakeup' only: the reads may block in the VFS */
		void _read_log_files()
		{
			if (!_log_read_due) return;
			_log_read_due = false;

			for (Stream &s : _streams) {
				if (!s.local || !s.log_file.constructed()) continue;

				/* a "-d" whose file cannot be read ends with exit value 1 */
				if (!s.log_file->update() && s.dump && !s.wait_okay) {
					_finish(s, 1);
					continue;
				}
				_pump(s);
			}
		}

		/* "-d" among the arguments, 'logcat_service' wants "shell:<command>" */
		static bool _logcat_dump(Service const &service)
		{
			Genode::String<256> const command("shell:",
				Genode::Cstring(service.command, service.command_len));
			bool dump = false;
			Usb_gadget::Adb::logcat_service(command.string(), dump);
			return dump;
		}

		void _open(Header const &h, Genode::Const_byte_range_ptr const &payload)
		{
			using namespace Usb_gadget::Adb;

			/* the service is a string, up to its terminating zero */
			char const *s = payload.start;
			Genode::size_t len = 0;
			while (len < payload.num_bytes && s[len]) len++;

			Text    const name(Genode::Cstring(s, len));
			Service const service = parse_service(s, len);

			switch (service.kind) {
			case Service::LOGCAT:
			{
				bool const dump = _logcat_dump(service);
				Genode::log("logcat", dump ? " -d" : "", " for the host (\"", name, "\")");
				_open_logcat(h, dump, service.v2);
				return;
			}
			case Service::SYNC:
				Genode::log("sync for the host");
				_open_sync(h);
				return;
			case Service::SHELL:
				if (service.command_len)
					Genode::log("command for the host (\"", name, "\")");
				else
					Genode::log("shell for the host (\"", name, "\")");
				_open_shell(h, service);
				return;
			case Service::REBOOT:
			case Service::OTHER:
				break;
			}
			Genode::log("stream \"", name, "\" refused (not yet supported)");
			_send(refuse(h), nullptr);
		}

		/*
		 * v2: STDIN packets go to the terminal; WINDOW_SIZE_CHANGE is
		 * ignored, CLOSE_STDIN too: known limit, the Terminal has no end of
		 * file (deferred)
		 */
		void _shell_input(Stream &s, Genode::Const_byte_range_ptr const &payload)
		{
			if (!s.v2) {
				_shells->input(s.local, payload.start, payload.num_bytes);
				return;
			}
			s.v2_in->feed(payload.start, payload.num_bytes,
				[&] (Shell_v2::Id id, char const *data, Genode::size_t len) {
					if (id == Shell_v2::STDIN)
						_shells->input(s.local, data, len); });
		}

		void _handle_message(Header const &h, Genode::Const_byte_range_ptr const &payload)
		{
			using namespace Usb_gadget::Adb;

			switch (h.command) {
			case CNXN:
			{
				_close_all();
				_max_payload = cnxn_reply(h, 0, 0).arg1;

				char banner[256];
				Device_info const info { _name.string(), _model.string(), _device.string() };
				unsigned const n = banner_of(info, banner, sizeof(banner));
				Genode::log("host connected (version ", Genode::Hex(h.arg0),
				            ", max payload ", h.arg1, ")");
				_send(cnxn_reply(h, n, checksum((unsigned char const *)banner, n)), banner);
				return;
			}
			case OPEN:
				_open(h, payload);
				return;
			case OKAY:
				if (Stream *s = _stream(h.arg1)) {
					s->wait_okay = false;
					_pump(*s);
				}
				return;
			case CLSE:
				if (Stream *s = _stream(h.arg1))
					_close(*s);
				return;
			case WRTE:
				if (Stream *s = _stream(h.arg1)) {

					/* the host waits for our OKAY before its next WRTE */
					if (s->sync.constructed()) {
						s->sync->input((unsigned char const *)payload.start, h.data_length);
						s->okay_pending = true;
					} else if (s->shell) {
						_shell_input(*s, payload);
						s->okay_pending = true;
					} else
						_send(okay(s->local, s->remote), nullptr);

					_pump(*s);
				}
				return;
			default:
				return;
			}
		}

		/*
		 * A VFS on a File_system server blocks and dispatches I/O signals:
		 * a signal in the middle of a sync request is handled after it, as
		 * a pump of all streams
		 */
		bool _in_io = false, _io_again = false;

		void _io(auto const &fn)
		{
			if (_in_io) { _io_again = true; return; }
			_in_io = true;

			fn();
			while (_io_again) {
				_io_again = false;
				if (_connected) _pump_all();
			}

			_in_io = false;
		}

		/*
		 * New log lines, shell output, exit or input taken. Sync streams
		 * are left to 'message' and 'wakeup':
		 * one blocking in the VFS here would let the transport's signals
		 * in while the engine is busy.
		 */
		void _handle_io()
		{
			_io([&] {
				if (_connected)
					for (Stream &s : _streams)
						if (!s.sync.constructed()) _pump(s); });
		}

	public:

		/**
		 * Constructor
		 *
		 * \param config          node with the attributes 'name', 'model',
		 *                        'device', 'logcat_file' (a path of
		 *                        'files_vfs'), 'logcat_window' and the
		 *                        <shell> template (shells.h)
		 * \param files_vfs       VFS of adb push/pull
		 * \param transport_name  ADB_TRANSPORT of the shells ("usb")
		 */
		Engine(Genode::Env &env, Genode::Allocator &alloc, Transport &transport,
		       Node const &config, Node const &files_vfs, char const *transport_name)
		:
			_env(env), _alloc(alloc), _transport(transport),
			_name  (config.attribute_value("name",   Text("panther"))),
			_model (config.attribute_value("model",  Text("Pixel 7"))),
			_device(config.attribute_value("device", Text("panther"))),
			_files_vfs(alloc, files_vfs),
			_logcat_file(_rel(config.attribute_value("logcat_file", Log_file::Path()))),
			_logcat_window(config.attribute_value("logcat_window",
			               Genode::Num_bytes { Mueslios::Log_follow::WINDOW }))
		{
			config.with_sub_node("shell",
				[&] (Node const &shell) {
					_shells.construct(env, alloc, shell, transport_name, _io_handler); },
				[&] {
					_shells.construct(env, alloc, Node(), transport_name, _io_handler); });
		}

		/**
		 * The transport is up, messages may come
		 */
		void connected() { _connected = true; }

		/**
		 * The transport is gone, all streams are closed
		 */
		void disconnected() { _connected = false; _close_all(); }

		/**
		 * Handle one complete message of the host
		 *
		 * The transport must not call it again before it returns (it may
		 * block in the VFS and dispatch I/O signals).
		 */
		void message(Header const &h, Genode::Const_byte_range_ptr const &payload)
		{
			if (_in_io) {
				Genode::error("adb message while the engine is busy, dropped");
				return;
			}
			_io([&] { _handle_message(h, payload); });
		}

		/**
		 * The transport is ready again or made progress, or it answers
		 * 'Transport::wakeup_soon': read the log file if due, pump the
		 * streams
		 *
		 * The transport must call it under its guard, as 'message': the
		 * log file read may block in the VFS and dispatch I/O signals.
		 */
		void wakeup()
		{
			_io([&] {
				if (!_connected) return;
				_read_log_files();
				_pump_all(); });
		}
};

#endif /* _ADB_ENGINE__ENGINE_H_ */
