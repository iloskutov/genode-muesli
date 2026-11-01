/*
 * \brief  adb: message framer
 * \author Ivan Loskutov
 * \date   2026-10-24
 *
 * Reassembles adb messages (24-byte header plus payload) from a byte
 * stream, e.g. TCP, where message boundaries are not kept. No Genode
 * dependencies.
 */

/*
 * SPDX-License-Identifier: MIT
 */

#ifndef _INCLUDE__ADB__FRAMER_H_
#define _INCLUDE__ADB__FRAMER_H_

#include <usb_gadget/adb.h>

namespace Adb {

	/* no <cstddef>: a Genode component without libc includes this too */
	using size_t = decltype(sizeof(0));

	class Framer;
}


class Adb::Framer
{
	private:

		using Header = Usb_gadget::Adb::Header;

		static constexpr size_t HEADER_SIZE = Usb_gadget::Adb::HEADER_SIZE;
		static constexpr size_t MAX_PAYLOAD = Usb_gadget::Adb::MAX_PAYLOAD;

		unsigned char _buf[HEADER_SIZE + MAX_PAYLOAD];
		size_t        _used   = 0;
		bool          _broken = false;

	public:

		/**
		 * Consume 'len' bytes of the stream
		 *
		 * Calls 'fn(Header const &, char const *payload, size_t payload_len)'
		 * once per complete message. After a bad header nothing is
		 * delivered until 'reset()'.
		 */
		template <typename FN>
		void feed(char const *data, size_t len, FN const &fn)
		{
			while (len && !_broken) {

				size_t want = HEADER_SIZE;
				Header h { };
				bool const have_header = _used >= HEADER_SIZE;

				if (have_header) {
					Usb_gadget::Adb::parse(_buf, HEADER_SIZE, h);
					want += h.data_length;
				}

				size_t const n = (want - _used < len) ? want - _used : len;
				for (size_t i = 0; i < n; i++) _buf[_used + i] = (unsigned char)data[i];
				_used += n; data += n; len -= n;

				if (_used < HEADER_SIZE)
					continue;

				if (!have_header) {
					if (!Usb_gadget::Adb::parse(_buf, HEADER_SIZE, h)) {
						_broken = true;
						return;
					}
					want = HEADER_SIZE + h.data_length;
				}

				if (_used == want) {
					_used = 0;
					fn(h, (char const *)_buf + HEADER_SIZE, (size_t)h.data_length);
				}
			}
		}

		bool broken() const { return _broken; }

		void reset() { _used = 0; _broken = false; }
};

#endif /* _INCLUDE__ADB__FRAMER_H_ */
