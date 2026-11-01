/*
 * \brief  LOG server that keeps the recent log for readers (adb logcat)
 * \author Ivan Loskutov
 * \date   2026-10-24
 *
 * Serves LOG sessions: every line goes, prefixed with the client's label, into
 * a ring of the recent log. Serves read-only Terminal sessions: a reader
 * gets the ring from its oldest byte on, then new lines as they come
 * (read_avail signals). Has no dependencies but core's services, so every
 * component can route its LOG here.
 *
 *   config | uart: no | size: 256K
 *
 *   uart  also write every line to the component's own LOG. The client then
 *         waits for the UART, so it is for a system that may hang or reset.
 *   size  size of the ring, at least 4K. It comes from the component's RAM quota.
 */

/*
 * SPDX-License-Identifier: MIT
 */

#include <base/attached_ram_dataspace.h>
#include <base/attached_rom_dataspace.h>
#include <base/component.h>
#include <base/heap.h>
#include <base/session_object.h>
#include <log_ring/ring.h>
#include <log_session/log_session.h>
#include <root/component.h>
#include <terminal_session/terminal_session.h>

namespace Log_ring {

	using namespace Genode;

	struct Terminal_component;
	struct Main;

	using History = Ring;
}


struct Log_ring::Terminal_component
:
	Rpc_object<Terminal::Session, Terminal_component>
{
	History const &_history;
	History::Cursor _cursor;

	Attached_ram_dataspace _io_buffer;

	Signal_context_capability _read_avail { };

	Terminal_component(Env &env, History const &history)
	:
		_history(history), _cursor(history.oldest()),
		_io_buffer(env.ram(), env.rm(), 4096)
	{ }

	void new_data()
	{
		if (_read_avail.valid())
			Signal_transmitter(_read_avail).submit();
	}

	Size size() override { return Size(0, 0); }

	bool avail() override { return _history.avail(_cursor) > 0; }

	size_t _read(size_t n)
	{
		return _history.read(_cursor, _io_buffer.local_addr<char>(),
		                     min(n, _io_buffer.size()));
	}

	size_t _write(size_t n) { return n; }  /* read-only */

	Dataspace_capability _dataspace() { return _io_buffer.cap(); }

	void read_avail_sigh(Signal_context_capability sigh) override
	{
		_read_avail = sigh;
		if (avail()) new_data();
	}

	void size_changed_sigh(Signal_context_capability) override { }

	void connected_sigh(Signal_context_capability sigh) override {
		Signal_transmitter(sigh).submit(); }

	size_t read(void *, size_t) override { return 0; }
	size_t write(void const *, size_t) override { return 0; }
};


struct Log_ring::Main
{
	Env &_env;

	Main(Main const &) = delete;
	Main &operator = (Main const &) = delete;

	Attached_rom_dataspace _config { _env, "config" };

	bool const _uart = _config.node().attribute_value("uart", false);

	size_t const _size = max(size_t(_config.node().attribute_value("size",
	                                Number_of_bytes(256*1024))), size_t(4096));

	Sliced_heap _sliced_heap { _env.ram(), _env.rm() };

	Attached_ram_dataspace _ring { _env.ram(), _env.rm(), _size };

	History _history { _ring.local_addr<char>(), _size };

	enum { MAX_READERS = 8 };

	Terminal_component *_readers[MAX_READERS] { };

	void _put(Session_label const &label, char const *text)
	{
		size_t len = strlen(text);
		if (!len) return;

		/* as core prints it, one line per write */
		bool const newline = text[len - 1] == '\n';
		if (newline) len--;

		String<160> const prefix("[", label, "] ");
		_history.put(prefix.string(), prefix.length() - 1);
		_history.put(text, len);
		_history.put("\n", 1);

		if (_uart)
			log(prefix, Cstring(text, len));

		for (Terminal_component *r : _readers)
			if (r) r->new_data();
	}

	struct Log_session_component : Session_object<Log_session>
	{
		Main &_main;

		Log_session_component(Entrypoint &ep, Resources const &resources,
		                      Label const &label, Main &main)
		:
			Session_object(ep, resources, label), _main(main)
		{ }

		void write(String const &string) override
		{
			if (string.valid_string())
				_main._put(label(), string.string());
		}
	};

	struct Log_root : Root_component<Log_session_component>
	{
		Env  &_env;
		Main &_main;

		Create_result _create_session(char const *args) override
		{
			return *new (md_alloc())
				Log_session_component(_env.ep(), session_resources_from_args(args),
				                      session_label_from_args(args), _main);
		}

		Log_root(Env &env, Allocator &md_alloc, Main &main)
		:
			Root_component<Log_session_component>(env.ep(), md_alloc),
			_env(env), _main(main)
		{ }
	};

	struct Terminal_root : Root_component<Terminal_component>
	{
		Env  &_env;
		Main &_main;

		Create_result _create_session(char const *) override
		{
			for (Terminal_component *&slot : _main._readers) {
				if (slot) continue;
				slot = new (md_alloc()) Terminal_component(_env, _main._history);
				return *slot;
			}
			error("too many readers");
			throw Service_denied();
		}

		void _destroy_session(Terminal_component &t) override
		{
			for (Terminal_component *&slot : _main._readers)
				if (slot == &t) slot = nullptr;
			Genode::destroy(md_alloc(), &t);
		}

		Terminal_root(Env &env, Allocator &md_alloc, Main &main)
		:
			Root_component<Terminal_component>(env.ep(), md_alloc),
			_env(env), _main(main)
		{ }
	};

	Log_root      _log_root      { _env, _sliced_heap, *this };
	Terminal_root _terminal_root { _env, _sliced_heap, *this };

	Main(Env &env) : _env(env)
	{
		env.parent().announce(env.ep().manage(_log_root));
		env.parent().announce(env.ep().manage(_terminal_root));
	}
};


void Component::construct(Genode::Env &env) { static Log_ring::Main main(env); }
