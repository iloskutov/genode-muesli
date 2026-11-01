/*
 * \brief  adb: shell protocol v2 packets
 * \author Ivan Loskutov
 * \date   2026-10-24
 *
 * A shell,v2 stream carries packets: id byte, little-endian u32 length,
 * data. No Genode dependencies.
 */

/*
 * SPDX-License-Identifier: MIT
 */

#ifndef _INCLUDE__ADB__SHELL_V2_H_
#define _INCLUDE__ADB__SHELL_V2_H_

namespace Adb {

	/* no <cstddef>: a Genode component without libc includes this too */
	using size_t = decltype(sizeof(0));
}


namespace Adb::Shell_v2 {

	enum Id : unsigned char { STDIN = 0, STDOUT = 1, STDERR = 2, EXIT = 3,
	                    CLOSE_STDIN = 4, WINDOW_SIZE_CHANGE = 5 };

	static constexpr size_t PACKET_HEADER = 5;

	/**
	 * Write one packet to 'out'
	 *
	 * \return packet size, 0 if 'out_size' is too small
	 */
	static inline size_t write(Id id, char const *data, size_t len,
	                           char *out, size_t out_size)
	{
		if (out_size < PACKET_HEADER || len > out_size - PACKET_HEADER)
			return 0;

		out[0] = (char)id;
		for (unsigned b = 0; b < 4; b++)
			out[1 + b] = (char)(len >> (8*b));
		for (size_t i = 0; i < len; i++)
			out[PACKET_HEADER + i] = data[i];
		return PACKET_HEADER + len;
	}

	/**
	 * Reassembles packets from the payloads of consecutive WRTE messages
	 */
	class Reader
	{
		private:

			static constexpr size_t MAX_DATA = 64*1024;

			unsigned char _buf[PACKET_HEADER + MAX_DATA];
			size_t        _used = 0;

			size_t _data_len() const
			{
				return _buf[1] | _buf[2] << 8 | _buf[3] << 16
				     | (size_t)_buf[4] << 24;
			}

		public:

			/**
			 * Calls 'fn(Id, char const *data, size_t len)' per complete
			 * packet. Packets with more than 64 KiB of data are cut off.
			 */
			template <typename FN>
			void feed(char const *data, size_t len, FN const &fn)
			{
				while (len) {
					size_t const total = _used < PACKET_HEADER
					                   ? PACKET_HEADER
					                   : PACKET_HEADER + _data_len();
					size_t const room = _used < sizeof(_buf) ? sizeof(_buf) - _used : 0;

					/* take what belongs to this packet, drop what does not fit */
					size_t n = total - _used;
					if (n > len) n = len;
					size_t const keep = n < room ? n : room;
					for (size_t i = 0; i < keep; i++) _buf[_used + i] = (unsigned char)data[i];
					_used += n; data += n; len -= n;

					if (_used < PACKET_HEADER)
						continue;

					size_t const dl = _data_len();
					if (_used == PACKET_HEADER + dl) {
						size_t const have = dl < MAX_DATA ? dl : MAX_DATA;
						_used = 0;
						fn((Id)_buf[0], (char const *)_buf + PACKET_HEADER, have);
					}
				}
			}
	};
}

#endif /* _INCLUDE__ADB__SHELL_V2_H_ */
