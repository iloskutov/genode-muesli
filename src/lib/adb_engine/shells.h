/*
 * \brief  adb shells: a child per shell stream
 * \author Ivan Loskutov
 * \date   2026-10-24
 *
 * Every shell stream gets a fresh child (bash), started by the Sandbox
 * library from the template node
 *
 *   <shell binary="bash" ram="64M" caps="1000">
 *     <config> ... </config>  copied into the child's config
 *     <route>  ... </route>   copied, after "Terminal -> local"
 *   </shell>
 *
 * The child is named "shell.<stream id>". Its config gets the arguments
 * ("bash", or "bash -c <command>" for a one-shot command) and the env
 * ADB_TRANSPORT and ADB_PTY ("yes" for a pty stream, else "no": the libc
 * takes every Terminal for a tty, so a program that wants to ask the user
 * looks at ADB_PTY). Its Terminal sessions are served here: the child's own
 * and those of the programs it forks (labels "shell.<id> -> ..."). The
 * output of all of them goes to the stream in the order it arrives and
 * waits for the host (as a pty, a newline becomes CR LF: the host's
 * terminal is in raw mode). The host's input goes to the newest session
 * still open (the program in the foreground), once that one closes to the
 * next newest (bash). The stream ends when the child exits, its exit value
 * comes from the sandbox state report. The Terminal has no end of file: a
 * child that reads its stdin to the end waits for the stream to close. A
 * command with a control byte (other than tab, LF) is refused: the config
 * (HID) cannot carry it.
 *
 * Every Terminal session has an entrypoint (thread) of its own: a write
 * blocks while the shell's output queue is full, until the host takes
 * output, without stopping the other shells. The queues are shared with
 * the component's entrypoint under '_mutex'. Session requests and closes
 * are answered right away (the session state of a local service belongs
 * to the client child, it must not outlive it); a close wakes a blocked
 * write and waits for it to return.
 */

/*
 * SPDX-License-Identifier: MIT
 */

#ifndef _ADB_ENGINE__SHELLS_H_
#define _ADB_ENGINE__SHELLS_H_

#include <adb/service.h>
#include <base/attached_ram_dataspace.h>
#include <base/blockade.h>
#include <base/mutex.h>
#include <base/node.h>
#include <base/registry.h>
#include <base/rpc_server.h>
#include <base/session_object.h>
#include <sandbox/sandbox.h>
#include <terminal_session/terminal_session.h>
#include <usb_gadget/adb.h>

namespace Adb {

	template <typename> struct Optional;

	class Shells;
}


template <typename T>
struct Adb::Optional
{
	bool valid = false;
	T    value { };
};


class Adb::Shells : Genode::Sandbox::State_handler,
                    Genode::Sandbox::Local_service_base::Wakeup
{
	private:

		using Name = Genode::String<32>;
		using Node = Genode::Node;

		enum : unsigned { IN_SIZE = 128*1024, OUT_SIZE = 64*1024,
		                  COMMAND_MAX = Usb_gadget::Adb::MAX_PAYLOAD };

		/* what the sandbox and a session thread need per child */
		enum : Genode::size_t { RAM_RESERVE = 1024*1024, CAPS_RESERVE = 50 };

		/* simple byte queue, the oldest bytes first */
		template <unsigned SIZE>
		struct Queue
		{
			char     data[SIZE];
			unsigned len = 0;

			unsigned space() const { return SIZE - len; }

			unsigned put(char const *src, unsigned n)
			{
				n = Genode::min(n, space());
				for (unsigned i = 0; i < n; i++) data[len + i] = src[i];
				len += n;
				return n;
			}

			/* as 'put', a newline becomes CR LF */
			unsigned put_onlcr(char const *src, unsigned n)
			{
				unsigned i = 0;
				for (; i < n; i++) {
					if (src[i] == '\n') {
						if (space() < 2) break;
						data[len++] = '\r';
					} else if (!space()) break;
					data[len++] = src[i];
				}
				return i;
			}

			unsigned take(char *dst, unsigned n)
			{
				n = Genode::min(n, len);
				for (unsigned i = 0; i < n; i++) dst[i] = data[i];
				drop(n);
				return n;
			}

			void drop(unsigned n)
			{
				n = Genode::min(n, len);
				for (unsigned i = n; i < len; i++) data[i - n] = data[i];
				len -= n;
			}
		};

		class Terminal_session;

		/* the thread of a Terminal session, constructed before the session object */
		struct Session_thread
		{
			Genode::Rpc_entrypoint ep;

			Session_thread(Genode::Env &env)
			:
				ep(env.runtime(), "shell_terminal", Genode::Thread::Stack_size { 16*1024 },
				   Genode::Affinity::Location())
			{ }
		};

		struct Shell
		{
			unsigned const id;
			Name     const name { "shell.", id };
			bool     const pty;

			/* the command of 'bash -c', none: interactive */
			char     command[COMMAND_MAX];
			unsigned command_len = 0;

			/* under '_mutex', shared with the session's thread */
			Queue<IN_SIZE>  in  { };
			Queue<OUT_SIZE> out { };
			bool            closed = false;   /* the stream is gone */

			/* the open sessions, the newest (it gets the input) first */
			Genode::List<Genode::List_element<Terminal_session>> sessions { };

			Optional<int> exit { };

			Shell(unsigned id, Service const &service)
			:
				id(id), pty(service.pty)
			{
				command_len = unsigned(Genode::min(service.command_len,
				                                   (Genode::size_t)COMMAND_MAX));
				for (unsigned i = 0; i < command_len; i++)
					command[i] = service.command[i];
			}

			/*
			 * Noncopyable
			 */
			Shell(Shell const &) = delete;
			Shell &operator = (Shell const &) = delete;

			/* input is queued: tell the session in the foreground */
			inline void notify_read();

			/* output was taken or the stream closed: writes go on */
			inline void room();

			unsigned newlines() const
			{
				unsigned n = 0;
				for (unsigned i = 0; i < command_len; i++)
					if (command[i] == '\n') n++;
				return n;
			}
		};

		using Registered_shell = Genode::Registered_no_delete<Shell>;

		Genode::Env       &_env;
		Genode::Allocator &_alloc;

		/* a copy, the caller's node may go away */
		Genode::Buffered_node const _template;

		using Binary = Genode::String<128>;

		Binary                  const _binary;
		Genode::Number_of_bytes const _ram;
		unsigned long           const _caps;

		Genode::String<32> const _transport;

		/* output, exit, room for input: the owner's streams go on */
		Genode::Signal_context_capability const _io_sigh;

		/* queues and session links, shared with the session threads */
		Genode::Mutex mutable _mutex { };

		Genode::Registry<Registered_shell> _shells { };

		Genode::Sandbox _sandbox { _env, *this };

		using Terminal_service = Genode::Sandbox::Local_service<Terminal_session>;

		Terminal_service _terminal_service { _sandbox, *this };

		void _io() { Genode::Signal_transmitter(_io_sigh).submit(); }

		Shell *_shell(unsigned id)
		{
			Shell *result = nullptr;
			_shells.for_each([&] (Shell &sh) {
				if (sh.id == id && !sh.closed) result = &sh; });
			return result;
		}

		Shell const *_shell(unsigned id) const
		{
			Shell const *result = nullptr;
			_shells.for_each([&] (Shell const &sh) {
				if (sh.id == id && !sh.closed) result = &sh; });
			return result;
		}

		/* the shell of a session label "shell.<id>" or "shell.<id> -> ..." */
		Shell *_shell(Genode::Session_label const &label)
		{
			Shell *result = nullptr;
			_shells.for_each([&] (Shell &sh) {
				Genode::String<40> const prefix(sh.name, " -> ");
				if (!sh.closed && (label == sh.name
				 || !Genode::strcmp(label.string(), prefix.string(), prefix.length() - 1)))
					result = &sh; });
			return result;
		}

		/*
		 * A command byte the child's config (HID) cannot carry: a control
		 * byte other than tab, LF, CR before LF
		 */
		static bool _control_byte(Service const &service)
		{
			char const *c = service.command;
			for (Genode::size_t i = 0; i < service.command_len; i++) {
				if ((unsigned char)c[i] >= 0x20 || c[i] == '\t' || c[i] == '\n')
					continue;
				if (c[i] == '\r' && i + 1 < service.command_len && c[i + 1] == '\n')
					continue;
				return true;
			}
			return false;
		}

		bool _fits() const
		{
			Genode::Pd_session::Stats const stats = _env.pd().stats();
			Genode::size_t const ram  = stats.ram.avail().value,
			                     caps = stats.caps.avail().value;

			if (ram >= _ram + RAM_RESERVE + sizeof(Registered_shell)
			 && caps >= _caps + CAPS_RESERVE)
				return true;

			Genode::warning("no room for another shell (",
			                Genode::Number_of_bytes(ram), " RAM, ", caps, " caps left)");
			return false;
		}

		void _gen_start(Genode::Generator &g, Shell const &sh) const
		{
			auto const arg = [&] (char const *s, Genode::size_t len) {
				g.node("arg", [&] { g.append_quoted(s, len); }); };

			g.node("start", [&] {
				g.attribute("name", sh.name);
				g.attribute("ram",  (unsigned long long)_ram);
				g.attribute("caps", _caps);
				g.node("binary", [&] { g.attribute("name", _binary); });

				g.node("config", [&] {
					_template.with_optional_sub_node("config", [&] (Node const &config) {
						g.node_attributes(config);
						g.append_node_content(config); });

					arg("bash", 4);
					if (sh.command_len) {
						arg("-c", 2);
						arg(sh.command, sh.command_len);
					}
					g.node("env", [&] {
						g.attribute("key",   "ADB_TRANSPORT");
						g.attribute("value", _transport); });
					g.node("env", [&] {
						g.attribute("key",   "ADB_PTY");
						g.attribute("value", sh.pty ? "yes" : "no"); });
				});

				g.node("route", [&] {
					g.node("service", [&] {
						g.attribute("name", "Terminal");
						g.node("local"); });
					_template.with_optional_sub_node("route", [&] (Node const &route) {
						g.append_node_content(route); });
				});
			});
		}

		void _gen_config(Genode::Generator &g) const
		{
			/* the state report carries the children's exit values */
			g.node("report");

			static char const * const services[] = {
				"ROM", "PD", "CPU", "RM", "LOG", "Timer", "Rtc", "File_system", "Report" };

			g.node("parent-provides", [&] {
				for (char const *s : services)
					g.node("service", [&] { g.attribute("name", s); });
			});

			_shells.for_each([&] (Shell const &sh) {
				if (!sh.closed) _gen_start(g, sh); });
		}

		/**
		 * Start and remove children as '_shells' says
		 *
		 * \return false if the config could not be generated or is
		 *         invalid (nothing changed)
		 */
		bool _apply()
		{
			/* a newline of a command becomes newline, indentation, ": " */
			Genode::size_t size = 4096;
			_shells.for_each([&] (Shell const &sh) {
				size += 4*_template.num_bytes() + 1024 + 2*sh.command_len
				      + 64*sh.newlines(); });

			bool ok = false;
			Genode::Generated_node { _alloc, size, "config",
				[&] (Genode::Generator &g) { _gen_config(g); }
			}.node.with_result(
				[&] (Node const &config) {
					/* an invalid config would remove all children */
					if (!config.has_type("config")) {
						Genode::error("shells: invalid sandbox config");
						return;
					}
					_sandbox.apply_config(config);
					ok = true; },
				[&] (Genode::Buffer_error) { Genode::error("shells: sandbox config too large"); });

			if (!ok)
				return false;

			/* the children of closed streams are gone (with their sessions) */
			Shell *gone = nullptr;
			do {
				gone = nullptr;
				_shells.for_each([&] (Shell &sh) { if (!gone && sh.closed) gone = &sh; });
				if (gone)
					_destroy(*gone);
			} while (gone);

			return true;
		}

		inline void _destroy(Shell &);

		inline void _handle_service();

		/**
		 * Sandbox::Local_service_base::Wakeup interface
		 */
		void wakeup_local_service() override { _handle_service(); }

		/**
		 * Sandbox::State_handler interface
		 */
		void handle_sandbox_state() override
		{
			unsigned count = 0;
			_shells.for_each([&] (Shell const &) { count++; });

			Genode::Generated_node { _alloc, 4096 + 512*count, "state",
				[&] (Genode::Generator &g) { _sandbox.generate_state_report(g); }
			}.node.with_result(
				[&] (Node const &state) {
					bool exited = false;
					state.for_each_sub_node("child", [&] (Node const &child) {
						if (!child.has_attribute("exited"))
							return;
						Name const name = child.attribute_value("name", Name());
						_shells.for_each([&] (Shell &sh) {
							if (sh.name != name || sh.exit.valid)
								return;
							sh.exit = { true, int(child.attribute_value("exited", 0L) & 0xff) };
							exited = true;
						});
					});
					if (exited) _io();
				},
				[&] (Genode::Buffer_error) { Genode::error("shells: state report too large"); });
		}

	public:

		/**
		 * Constructor
		 *
		 * \param shell_template  node <shell binary ram caps> with the sub
		 *                        nodes <config> and <route>
		 * \param transport_name  value of the env ADB_TRANSPORT
		 * \param io_sigh         signalled on output, exit, room for input
		 */
		Shells(Genode::Env &env, Genode::Allocator &alloc, Node const &shell_template,
		       char const *transport_name, Genode::Signal_context_capability io_sigh)
		:
			_env(env), _alloc(alloc), _template(alloc, shell_template),
			_binary(shell_template.attribute_value("binary", Binary("bash"))),
			_ram   (shell_template.attribute_value("ram", Genode::Number_of_bytes(64*1024*1024))),
			_caps  (shell_template.attribute_value("caps", 1000UL)),
			_transport(transport_name), _io_sigh(io_sigh)
		{ }

		/**
		 * Start the child of a shell stream
		 *
		 * \param service  interactive (no command) or a one-shot command,
		 *                 'pty': output newlines become CR LF
		 * \return false if no <shell> is configured, the command holds a
		 *         control byte, the template's RAM or caps do not fit, or
		 *         the child's config could not be made
		 */
		bool open(unsigned stream_id, Service const &service)
		{
			if (!_template.has_type("shell")) {
				Genode::warning("no <shell> configured");
				return false;
			}
			if (_control_byte(service)) {
				Genode::warning("command with a control byte refused");
				return false;
			}
			if (!_fits())
				return false;

			Registered_shell &sh = *new (_alloc) Registered_shell(_shells, stream_id, service);
			if (_apply())
				return true;

			Genode::destroy(_alloc, &sh);
			return false;
		}

		/**
		 * Remove the child of a stream, whether it exited or not
		 */
		inline void close(unsigned stream_id);

		/**
		 * Input of the host (the caller checks 'input_room' first)
		 */
		void input(unsigned stream_id, char const *data, Genode::size_t len)
		{
			Shell *sh = _shell(stream_id);
			if (!sh) return;

			Genode::Mutex::Guard guard(_mutex);
			if (sh->in.put(data, unsigned(len)) < len)
				Genode::warning(sh->name, ": input dropped, host sent too much");
			sh->notify_read();
		}

		/**
		 * Whether 'len' more bytes of input fit
		 */
		bool input_room(unsigned stream_id, Genode::size_t len) const
		{
			Shell const *sh = _shell(stream_id);
			if (!sh) return false;

			Genode::Mutex::Guard guard(_mutex);
			return sh->in.space() >= len;
		}

		/**
		 * Call 'fn(char const *, size_t)' with the queued output, if any
		 *
		 * 'fn' returns the number of bytes it consumed. It is called with
		 * the queues locked: it must only copy.
		 */
		inline void with_output(unsigned stream_id, auto const &fn);

		/**
		 * Exit value of the stream's child, valid once it exited
		 */
		Optional<int> exit_value(unsigned stream_id) const
		{
			Shell const *sh = _shell(stream_id);
			return sh ? sh->exit : Optional<int> { };
		}
};


class Adb::Shells::Terminal_session
:
	private Session_thread,
	public  Genode::Session_object<Terminal::Session, Terminal_session>
{
	private:

		Genode::Mutex &_mutex;

		Genode::Attached_ram_dataspace _io;

		/* under '_mutex' */
		Shell *_shell;                   /* nullptr: no I/O (closed stream) */
		bool   _writing        = false;  /* a write is in progress */
		bool   _waiting        = false;  /* it waits for '_room' */
		bool   _closer_waiting = false;  /* a close waits for '_done' */

		Genode::Signal_context_capability _read_avail { };

		Genode::List_element<Terminal_session> _element { this };

		Genode::Blockade _room { }, _done { };

		Genode::Signal_context_capability const _io_sigh;

		bool _open() const { return _shell && !_shell->closed; }

		/* the session gets the input */
		bool _foreground() const { return _open() && _shell->sessions.first() == &_element; }

		/*
		 * Noncopyable
		 */
		Terminal_session(Terminal_session const &);
		Terminal_session &operator = (Terminal_session const &);

	public:

		/**
		 * Constructor
		 *
		 * \param shell  the shell to attach to (the session becomes its
		 *               foreground), nullptr: no I/O
		 */
		Terminal_session(Genode::Env &env, Genode::Mutex &mutex, Resources const &resources,
		                 Label const &label, Shell *shell,
		                 Genode::Signal_context_capability io_sigh)
		:
			Session_thread(env),
			Session_object(Session_thread::ep, resources, label),
			_mutex(mutex), _io(env.ram(), env.rm(), 4096),
			_shell(shell), _io_sigh(io_sigh)
		{
			if (!_shell) return;

			Genode::Mutex::Guard guard(_mutex);
			_shell->sessions.insert(&_element);
		}

		/**
		 * Input is queued for the foreground session
		 *
		 * Called with '_mutex' held.
		 */
		void notify_read()
		{
			if (_read_avail.valid())
				Genode::Signal_transmitter(_read_avail).submit();
		}

		/**
		 * Output was taken or the stream closed: a waiting write goes on
		 *
		 * Called with '_mutex' held.
		 */
		void room()
		{
			if (!_waiting) return;
			_waiting = false;
			_room.wakeup();
		}

		/**
		 * The session goes away: end I/O, wait for a write in progress
		 */
		void detach_and_wait()
		{
			bool wait = false;
			{
				Genode::Mutex::Guard guard(_mutex);
				if (_shell) {
					/* the next newest session gets the input */
					_shell->sessions.remove(&_element);
					_shell->notify_read();
				}
				_shell = nullptr;
				if (_writing) _closer_waiting = wait = true;
				room();
			}
			if (wait) _done.block();
		}

		Size size() override { return Size(80, 24); }

		bool avail() override
		{
			Genode::Mutex::Guard guard(_mutex);
			return _foreground() && _shell->in.len > 0;
		}

		Genode::size_t _read(Genode::size_t n)
		{
			Genode::Mutex::Guard guard(_mutex);
			if (!_foreground()) return 0;

			unsigned const got = _shell->in.take(_io.local_addr<char>(),
			                                     unsigned(Genode::min(n, _io.size())));
			if (got) Genode::Signal_transmitter(_io_sigh).submit();
			return got;
		}

		Genode::size_t _write(Genode::size_t n)
		{
			n = Genode::min(n, _io.size());
			char const *src = _io.local_addr<char const>();

			_mutex.acquire();
			_writing = true;
			for (unsigned done = 0; done < n && _open(); ) {
				unsigned const left = unsigned(n - done);
				done += _shell->pty ? _shell->out.put_onlcr(src + done, left)
				                    : _shell->out.put(src + done, left);
				Genode::Signal_transmitter(_io_sigh).submit();

				/* the queue is full: wait until the host takes output */
				if (done < n && _open()) {
					_waiting = true;
					_mutex.release();
					_room.block();
					_mutex.acquire();
				}
			}
			_writing = false;
			bool const closer = _closer_waiting;
			_closer_waiting = false;
			_mutex.release();

			if (closer) _done.wakeup();
			return n;
		}

		Genode::Dataspace_capability _dataspace() { return _io.cap(); }

		void read_avail_sigh(Genode::Signal_context_capability sigh) override
		{
			Genode::Mutex::Guard guard(_mutex);
			if (!_open()) return;
			_read_avail = sigh;
			if (_foreground() && _shell->in.len) notify_read();
		}

		void size_changed_sigh(Genode::Signal_context_capability) override { }

		void connected_sigh(Genode::Signal_context_capability sigh) override {
			Genode::Signal_transmitter(sigh).submit(); }

		Genode::size_t read(void *, Genode::size_t) override { return 0; }
		Genode::size_t write(void const *, Genode::size_t) override { return 0; }
};


void Adb::Shells::Shell::notify_read()
{
	if (in.len && sessions.first())
		sessions.first()->object()->notify_read();
}


void Adb::Shells::Shell::room()
{
	for (auto *e = sessions.first(); e; e = e->next())
		e->object()->room();
}


void Adb::Shells::close(unsigned stream_id)
{
	Shell *sh = _shell(stream_id);
	if (!sh) return;

	{
		Genode::Mutex::Guard guard(_mutex);
		sh->closed = true;
		sh->in.len = sh->out.len = 0;
		sh->room();
	}
	_apply();
}


void Adb::Shells::with_output(unsigned stream_id, auto const &fn)
{
	Shell *sh = _shell(stream_id);
	if (!sh) return;

	Genode::Mutex::Guard guard(_mutex);
	if (!sh->out.len) return;

	Genode::size_t const consumed = fn((char const *)sh->out.data,
	                                   (Genode::size_t)sh->out.len);
	sh->out.drop(unsigned(consumed));
	if (consumed) sh->room();
}


void Adb::Shells::_destroy(Shell &sh)
{
	/* sessions the sandbox did not close (not expected) lose their shell */
	for (;;) {
		Terminal_session *session = nullptr;
		{
			Genode::Mutex::Guard guard(_mutex);
			if (sh.sessions.first())
				session = sh.sessions.first()->object();
		}
		if (!session) break;

		Genode::error(sh.name, ": removed with an open session");
		session->detach_and_wait();
	}

	Genode::destroy(_alloc, static_cast<Registered_shell *>(&sh));
}


void Adb::Shells::_handle_service()
{
	_terminal_service.for_each_requested_session([&] (Terminal_service::Request &request) {

		/*
		 * The child's own session and those of the programs it forks
		 * belong to its stream, a session of a closed stream does no I/O
		 */
		Shell *sh = _shell(request.label);

		Terminal_session &session = *new (_alloc)
			Terminal_session(_env, _mutex, request.resources, request.label, sh, _io_sigh);

		request.deliver_session(session);
	});

	_terminal_service.for_each_upgraded_session([&] (Terminal_session &session,
	                                                 Genode::Session::Resources const &amount) {
		session.upgrade(amount.ram_quota);
		session.upgrade(amount.cap_quota);
		return Terminal_service::Upgrade_response::CONFIRMED;
	});

	_terminal_service.for_each_session_to_close([&] (Terminal_session &session) {
		session.detach_and_wait();
		Genode::destroy(_alloc, &session);
		return Terminal_service::Close_response::CLOSED;
	});
}

#endif /* _ADB_ENGINE__SHELLS_H_ */
