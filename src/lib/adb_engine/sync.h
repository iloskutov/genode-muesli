/*
 * \brief  adb sync stream (push/pull) on the component's VFS
 * \author Ivan Loskutov
 * \date   2026-10-24
 *
 * Takes the host's stream bytes ('input'), answers into an output buffer
 * the adb stream drains with WRTE messages ('take'). RECV data is read
 * from the file only as the output drains, so a large pull needs no
 * buffer of its size. The host's bytes are decoded only while no pull
 * runs and the output has room for a reply (RESERVE); the rest waits in
 * '_in', and the adb stream acknowledges the host's WRTE only when all is
 * consumed ('consumed'), which holds the host back. A reply that would
 * not fit anyway (a pipelining host) breaks the stream: it is closed.
 * Paths are absolute in the protocol and taken
 * relative to the VFS root. SEND creates missing parent directories (the
 * host does not, the device offers no "fixed_push_mkdir"). Modification
 * times are not kept (0).
 */

/*
 * SPDX-License-Identifier: MIT
 */

#ifndef _ADB_ENGINE__SYNC_H_
#define _ADB_ENGINE__SYNC_H_

#include <base/log.h>
#include <os/vfs.h>
#include <usb_gadget/adb_sync.h>
#include <util/reconstructible.h>

namespace Adb { class Sync; }


class Adb::Sync : Genode::Noncopyable
{
	private:

		using Directory = Genode::Directory;
		using Path      = Directory::Path;
		using Decoder   = Usb_gadget::Adb_sync::Decoder;

		enum : unsigned {
			MODE_DIR  = 040755, MODE_FILE = 0100644,
			OUT_SIZE  = Usb_gadget::Adb_sync::MAX_DATA + 64*1024,
			IN_SIZE   = Usb_gadget::Adb::MAX_PAYLOAD,
			RESERVE   = 1024,   /* room for any reply but a listing */
			DONE_SIZE = 20 };

		Directory &_root;

		Decoder _decoder { };

		unsigned char _out[OUT_SIZE];
		unsigned      _out_len = 0;

		/* host bytes not decoded yet */
		unsigned char _in[IN_SIZE];
		unsigned      _in_len = 0;

		bool _broken = false;

		/* pull in progress */
		Genode::Constructible<Genode::Readonly_file> _recv { };
		Genode::Vfs::file_size _recv_size = 0, _recv_at = 0;

		/* push in progress */
		Genode::Constructible<Genode::New_file> _send { };
		bool _send_failed = false;
		Path _send_path { };

		bool _quit = false;

		static Path _rel(char const *path)
		{
			while (*path == '/') path++;
			return Path(path);
		}

		unsigned _space() const { return OUT_SIZE - _out_len; }

		/**
		 * Whether a reply of 'n' bytes fits, the stream breaks if not
		 */
		bool _room(unsigned n)
		{
			if (_space() >= n) return true;
			if (!_broken)
				Genode::error("sync: reply of ", n, " bytes does not fit, stream closed");
			_broken = true;
			return false;
		}

		void _append(unsigned n) { _out_len += n; }

		void _fail(char const *msg)
		{
			if (_room(8 + 64))
				_append(Usb_gadget::Adb_sync::fail(_out + _out_len, 8 + 64, msg));
		}

		void _stat(char const *path)
		{
			using namespace Usb_gadget::Adb_sync;
			Path const rel = _rel(path);

			unsigned mode = 0, size = 0;
			if (rel == "" || _root.directory_exists(rel))
				mode = MODE_DIR;
			else if (_root.file_exists(rel)) {
				mode = MODE_FILE;
				size = (unsigned)_root.file_size(rel);
			}
			if (_room(16))
				_append(stat_v1(_out + _out_len, mode, size, 0));
		}

		void _list(char const *path)
		{
			using namespace Usb_gadget::Adb_sync;
			Path const rel = _rel(path);

			if (rel == "" || _root.directory_exists(rel)) {
				Directory dir(_root, rel);
				bool cut = false;
				dir.for_each_entry([&] (Directory::Entry const &e) {
					if (cut) return;
					bool const is_dir = e.dir();
					unsigned const size = is_dir ? 0 : (unsigned)dir.file_size(e.name());

					/* the closing DONE always keeps its room */
					unsigned const max = _space() > DONE_SIZE ? _space() - DONE_SIZE : 0;
					unsigned const n = dent_v1(_out + _out_len, max,
					                           is_dir ? MODE_DIR : MODE_FILE,
					                           size, 0, e.name().string());
					if (!n) {
						Genode::warning("sync: listing of ", rel, " cut short");
						cut = true;
					}
					_append(n);
				});
			}
			if (_room(DONE_SIZE))
				_append(dent_done(_out + _out_len));
		}

		void _recv_open(char const *path)
		{
			Path const rel = _rel(path);
			if (!_root.file_exists(rel)) {
				_fail("No such file or directory");
				return;
			}
			try {
				_recv.construct(_root, rel);
				_recv_size = _root.file_size(rel);
				_recv_at   = 0;
				Genode::log("pull ", rel, " (", _recv_size, " bytes)");
			}
			catch (...) { _fail("cannot open the file"); }
		}

		/* data chunks of a pull, as much as fits */
		void _recv_produce()
		{
			using namespace Usb_gadget::Adb_sync;

			while (_recv.constructed() && _space() > 16) {
				Genode::Vfs::file_size const left = _recv_size - _recv_at;
				unsigned const n = (unsigned)Genode::min(left,
					(Genode::Vfs::file_size)Genode::min((unsigned)MAX_DATA, _space() - 16));

				if (n) {
					Genode::size_t const got = _recv->read(Genode::Vfs::At { _recv_at },
					                               Genode::Byte_range_ptr((char *)_out + _out_len + 8, n));
					if (!got) { _recv.destruct(); _fail("read error"); return; }
					_append(data_header(_out + _out_len, (unsigned)got) + (unsigned)got);
					_recv_at += got;
				}
				if (_recv_at == _recv_size) {
					_recv.destruct();
					if (_room(8))
						_append(done(_out + _out_len));
				}
				if (n) return;
			}
		}

		void _send_open(char const *target)
		{
			char path[Usb_gadget::Adb_sync::MAX_PATH + 1];
			unsigned mode = 0;
			_send_failed = true;

			if (!Usb_gadget::Adb_sync::send_target(target, path, sizeof(path), mode))
				return;

			Path const rel = _rel(path);

			/* parent directories */
			char const *s = rel.string();
			int slash = -1;
			for (int i = 0; s[i]; i++) if (s[i] == '/') slash = i;
			try {
				if (slash > 0) {
					Path const parent = Genode::Cstring(s, slash);
					if (!_root.directory_exists(parent))
						_root.create_sub_directory(parent, { });
				}
				_send.construct(_root, rel, Genode::Vfs::Timestamp { });
				_send_failed = false;
				_send_path   = rel;
			}
			catch (...) { Genode::error("push ", rel, ": cannot create the file"); }
		}

		void _send_data(unsigned char const *p, unsigned long n)
		{
			if (_send_failed || !_send.constructed()) return;
			if (_send->append((char const *)p, n) != Genode::New_file::Append_result::OK)
				_send_failed = true;
		}

		void _send_done()
		{
			using namespace Usb_gadget::Adb_sync;
			bool const ok = !_send_failed && _send.constructed();
			_send.destruct();
			if (ok) {
				Genode::log("push ", _send_path, " (", _root.file_size(_send_path), " bytes)");
				if (_room(8))
					_append(okay(_out + _out_len));
			}
			else
				_fail("cannot write the file");
		}

		void _request(unsigned id, char const *path)
		{
			using namespace Usb_gadget::Adb_sync;

			switch (id) {
			case STAT: _stat(path);       return;
			case LIST: _list(path);       return;
			case RECV: _recv_open(path);  return;
			case SEND: _send_open(path);  return;
			case QUIT: _quit = true;      return;
			default:
				Genode::warning("sync: request ", Genode::Hex(id), " not supported");
				_fail("request not supported");
			}
		}

		/**
		 * Decode pending host bytes: requests one at a time while a reply
		 * fits and no pull runs, the data of a push in one go
		 */
		void _process()
		{
			unsigned used = 0;
			while (!_broken && used < _in_len && !_recv.constructed() && _space() >= RESERVE) {
				unsigned const n = _decoder.sending() ? _in_len - used : 1;
				_decoder.feed(_in + used, n,
					[&] (unsigned id, char const *path) { _request(id, path); },
					[&] (unsigned char const *d, unsigned long len) { _send_data(d, len); },
					[&] (unsigned) { _send_done(); },
					[&] (char const *what) {
						Genode::warning("sync: ", what);
						_send.destruct();
						_fail(what); });
				used += n;
			}
			for (unsigned i = used; i < _in_len; i++) _in[i - used] = _in[i];
			_in_len -= used;
		}

	public:

		Sync(Directory &root) : _root(root) { }

		/**
		 * Take the payload of one host WRTE
		 *
		 * The host sends its next WRTE only after our OKAY, which follows
		 * 'consumed', so the pending bytes never exceed one payload.
		 */
		void input(unsigned char const *p, unsigned long len)
		{
			if (_in_len + len > IN_SIZE) {
				Genode::error("sync: host sent before its data was taken, stream closed");
				_broken = true;
				return;
			}
			for (unsigned long i = 0; i < len; i++) _in[_in_len + i] = p[i];
			_in_len += (unsigned)len;
			_process();
		}

		/* all host bytes decoded: its next WRTE may come */
		bool consumed() const { return _in_len == 0; }

		/**
		 * Up to 'max' bytes of output, consumed
		 */
		unsigned take(unsigned char *dst, unsigned max)
		{
			_recv_produce();
			_process();

			unsigned const n = Genode::min(max, _out_len);
			for (unsigned i = 0; i < n; i++) dst[i] = _out[i];
			for (unsigned i = n; i < _out_len; i++) _out[i - n] = _out[i];
			_out_len -= n;
			return n;
		}

		/* the host is done or the stream broke, and everything is sent */
		bool finished() const
		{
			return _broken || (_quit && !_out_len && !_recv.constructed() && !_in_len);
		}
};

#endif /* _ADB_ENGINE__SYNC_H_ */
