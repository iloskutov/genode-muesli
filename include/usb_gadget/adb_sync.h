/*
 * \brief  adb file sync protocol, version 1 (adb push/pull)
 * \author Ivan Loskutov
 * \date   2026-10-24
 *
 * After OPEN "sync:" the stream carries requests (u32 id, u32 length,
 * path) and, after a SEND, data chunks (DATA, u32 size, bytes) ending with
 * DONE (u32 mtime). The bytes come in WRTE messages of any split, so the
 * decoder takes them piece by piece. Replies: STAT (mode, size, mtime),
 * DENT entries and a closing DONE for LIST, DATA chunks and DONE for
 * RECV, OKAY or FAIL for SEND (AOSP file_sync_protocol.h). The device
 * offers no sync feature (stat_v2, ls_v2, sendrecv_v2), so the host uses
 * these. No Genode dependencies.
 */

/*
 * SPDX-License-Identifier: MIT
 */

#ifndef _INCLUDE__USB_GADGET__ADB_SYNC_H_
#define _INCLUDE__USB_GADGET__ADB_SYNC_H_

namespace Usb_gadget::Adb_sync {

	enum Id : unsigned {
		STAT = 0x54415453, LIST = 0x5453494c, DENT = 0x544e4544,
		SEND = 0x444e4553, RECV = 0x56434552, DATA = 0x41544144,
		DONE = 0x454e4f44, OKAY = 0x59414b4f, FAIL = 0x4c494146,
		QUIT = 0x54495551 };

	enum : unsigned { MAX_PATH = 1024, MAX_DATA = 64*1024 };

	static inline void put32(unsigned char *p, unsigned v) {
		for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8*i)); }

	static inline unsigned get32(unsigned char const *p) {
		return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

	/**
	 * Decoder of the host's byte stream
	 */
	class Decoder
	{
		private:

			enum class State { HEADER, PATH, CHUNK_DATA };

			State         _state   = State::HEADER;
			bool          _sending = false;   /* after SEND: chunks, not requests */
			unsigned char _header[8] { };
			unsigned      _have    = 0;
			unsigned      _id      = 0;
			unsigned      _left    = 0;       /* path or chunk bytes still to come */
			char          _path[MAX_PATH + 1] { };

			void _reset()
			{
				_state = State::HEADER; _sending = false; _have = 0;
			}

		public:

			bool sending() const { return _sending; }

			/**
			 * Take 'len' bytes of the stream
			 *
			 * \param request_fn  (unsigned id, char const *path)
			 * \param data_fn     (unsigned char const *, unsigned long) piece of a SEND
			 * \param done_fn     (unsigned mtime) end of a SEND
			 * \param error_fn    (char const *what), the decoder starts over
			 */
			void feed(unsigned char const *p, unsigned long len,
			          auto const &request_fn, auto const &data_fn,
			          auto const &done_fn, auto const &error_fn)
			{
				while (len) {
					switch (_state) {

					case State::HEADER:
					{
						while (len && _have < 8) { _header[_have++] = *p++; len--; }
						if (_have < 8) return;
						_have = 0;
						_id   = get32(_header);
						_left = get32(_header + 4);

						if (_sending) {
							if (_id == DONE) {
								_sending = false;
								done_fn(_left);
							} else if (_id == DATA && _left <= MAX_DATA) {
								if (_left) _state = State::CHUNK_DATA;
							} else {
								_reset();
								error_fn("unexpected chunk in a SEND");
							}
							break;
						}

						if (_left > MAX_PATH) {
							_reset();
							error_fn("path too long");
							break;
						}
						if (_left) { _state = State::PATH; break; }

						_path[0] = 0;
						request_fn(_id, _path);
						break;
					}

					case State::PATH:
					{
						unsigned const total = get32(_header + 4);
						while (len && _left) { _path[total - _left] = (char)*p++; _left--; len--; }
						if (_left) return;
						_path[total] = 0;
						_state = State::HEADER;
						if (_id == SEND) _sending = true;
						request_fn(_id, _path);
						break;
					}

					case State::CHUNK_DATA:
					{
						unsigned long const n = len < _left ? len : _left;
						data_fn(p, n);
						p += n; len -= n; _left -= (unsigned)n;
						if (!_left) _state = State::HEADER;
						break;
					}
					}
				}
			}
	};

	/* replies, each returns its length in bytes */

	static inline unsigned stat_v1(unsigned char *b, unsigned mode, unsigned size, unsigned mtime)
	{
		put32(b, STAT); put32(b + 4, mode); put32(b + 8, size); put32(b + 12, mtime);
		return 16;
	}

	/* \return 0 if 'max' is too small for the entry */
	static inline unsigned dent_v1(unsigned char *b, unsigned max, unsigned mode,
	                               unsigned size, unsigned mtime, char const *name)
	{
		unsigned n = 0;
		while (name[n]) n++;
		if (20 + n > max) return 0;
		put32(b, DENT); put32(b + 4, mode); put32(b + 8, size); put32(b + 12, mtime);
		put32(b + 16, n);
		for (unsigned i = 0; i < n; i++) b[20 + i] = (unsigned char)name[i];
		return 20 + n;
	}

	static inline unsigned dent_done(unsigned char *b)
	{
		put32(b, DONE); put32(b + 4, 0); put32(b + 8, 0); put32(b + 12, 0); put32(b + 16, 0);
		return 20;
	}

	static inline unsigned data_header(unsigned char *b, unsigned size) {
		put32(b, DATA); put32(b + 4, size); return 8; }

	static inline unsigned done(unsigned char *b) { put32(b, DONE); put32(b + 4, 0); return 8; }
	static inline unsigned okay(unsigned char *b) { put32(b, OKAY); put32(b + 4, 0); return 8; }

	static inline unsigned fail(unsigned char *b, unsigned max, char const *msg)
	{
		unsigned n = 0;
		while (msg[n] && 8 + n < max) n++;
		put32(b, FAIL); put32(b + 4, n);
		for (unsigned i = 0; i < n; i++) b[8 + i] = (unsigned char)msg[i];
		return 8 + n;
	}

	/**
	 * Split the SEND path "path,mode" (mode decimal)
	 */
	static inline bool send_target(char const *s, char *path, unsigned max, unsigned &mode)
	{
		int comma = -1;
		for (int i = 0; s[i]; i++) if (s[i] == ',') comma = i;
		if (comma < 0 || (unsigned)comma + 1 > max) return false;

		for (int i = 0; i < comma; i++) path[i] = s[i];
		path[comma] = 0;

		mode = 0;
		for (char const *m = s + comma + 1; *m; m++) {
			if (*m < '0' || *m > '9') return false;
			mode = mode*10 + unsigned(*m - '0');
		}
		return true;
	}
}

#endif /* _INCLUDE__USB_GADGET__ADB_SYNC_H_ */
