/*
 * \brief  Test of 'log_ring': a reader of its Terminal session
 * \author Ivan Loskutov
 * \date   2026-11-01
 *
 * Reads the Terminal session until the last line of 'test-log' passes and logs
 * "test-log_ring: PASS".
 */

/*
 * SPDX-License-Identifier: MIT
 */

#include <base/component.h>
#include <base/log.h>
#include <terminal_session/connection.h>

namespace Test {

	using namespace Genode;

	struct Main;
}


struct Test::Main
{
	Terminal::Connection _terminal;

	Signal_handler<Main> _read_avail;

	char _buf[4096];

	size_t _matched = 0;
	bool   _passed  = false;

	void _check(char const c)
	{
		static char const expected[] = "[test-log] Test done.";

		if (c != expected[_matched])
			_matched = 0;

		if (c == expected[_matched])
			_matched++;

		if (_matched == sizeof(expected) - 1 && !_passed) {
			_passed = true;
			log("test-log_ring: PASS");
		}
	}

	void _handle_read_avail()
	{
		while (_terminal.avail()) {
			size_t const n = _terminal.read(_buf, sizeof(_buf));
			for (size_t i = 0; i < n; i++)
				_check(_buf[i]);
		}
	}

	Main(Env &env)
	:
		_terminal(env), _read_avail(env.ep(), *this, &Main::_handle_read_avail)
	{
		_terminal.read_avail_sigh(_read_avail);
		_handle_read_avail();
	}
};


void Component::construct(Genode::Env &env) { static Test::Main main(env); }
