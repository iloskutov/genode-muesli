/*
 * \brief  adb logcat of a log file of the engine's VFS
 * \author Ivan Loskutov
 * \date   2026-10-25
 *
 * The source of logcat when the config names a 'logcat_file' and no
 * Terminal "logcat" (log_ring) is routed (MüesliOS: /report/log, the
 * report child's vfs_ram_log). The file's end (at most the window, whole
 * lines; with "-d" the partial last line too) and later what was appended
 * are read in chunks with the rule of mueslios/log_follow.h; a resync
 * reads the whole window. The stream drains the bytes with 'take'; the
 * next chunk is read only once they are taken. "-d" ends at the size the
 * file had at its first read.
 *
 * 'update' reads the VFS, which may block and dispatch I/O signals: the
 * engine calls it only from 'wakeup', under the transport's guard.
 */

/*
 * SPDX-License-Identifier: MIT
 */

#ifndef _ADB_ENGINE__LOG_FILE_H_
#define _ADB_ENGINE__LOG_FILE_H_

#include <base/allocator.h>
#include <base/log.h>
#include <mueslios/log_follow.h>
#include <os/vfs.h>

namespace Adb { class Log_file; }


class Adb::Log_file
{
	public:

		using Path = Genode::Directory::Path;

	private:

		using Follow   = Mueslios::Log_follow::Follow;
		using Range    = Mueslios::Log_follow::Range;
		using offset_t = Mueslios::Log_follow::offset_t;
		using size_t   = Genode::size_t;

		enum : size_t { CHUNK = 64*1024 };

		Genode::Allocator &_alloc;
		Genode::Directory &_root;

		Path const _path;
		bool const _dump;

		Follow _follow;

		bool     _started   = false;
		bool     _more      = true;    /* read again without waiting */
		bool     _dump_done = false;
		offset_t _dump_size = 0;       /* "-d" ends there */
		size_t   _chunk     = CHUNK;   /* grows while a chunk has no whole line */
		bool     _warned    = false;

		/* the bytes read last */
		char  *_buf      = nullptr;
		size_t _capacity = 0;
		size_t _len      = 0;

		/* not taken yet: the rest of the marker, then _buf[_pos, _end) */
		char const *_marker = "";
		size_t      _pos = 0, _end = 0;

		/*
		 * Noncopyable
		 */
		Log_file(Log_file const &);
		Log_file &operator = (Log_file const &);

		bool _size(offset_t &size)
		{
			try { size = _root.file_size(_path); return true; }
			catch (Genode::Directory::Nonexistent_file) { return false; }
		}

		/* the bytes [from, to) to '_buf', fewer if the file is shorter now */
		bool _read(offset_t from, offset_t to)
		{
			_len = 0;
			size_t const want = size_t(to - from);   /* at most the window */

			if (want > _capacity) {
				if (_buf) _alloc.free(_buf, _capacity);
				_buf = nullptr; _capacity = 0;

				size_t capacity = 4096;
				while (capacity < want) capacity *= 2;

				_alloc.try_alloc(capacity).with_result(
					[&] (Genode::Allocator::Allocation &a) {
						a.deallocate = false;
						_buf = (char *)a.ptr;
						_capacity = capacity; },
					[&] (Genode::Alloc_error) { });
				if (!_buf) return false;
			}

			try {
				Genode::Readonly_file const file(_root, _path);
				_len = file.read(Genode::Vfs::At { from },
				                 Genode::Byte_range_ptr(_buf, want));
			}
			catch (Genode::File::Open_failed) { return false; }
			return true;
		}

		void _set(Range const &range)
		{
			_marker = range.restarted ? Mueslios::Log_follow::RESTARTED : "";
			_pos    = range.start;
			_end    = range.end;
		}

		bool _update()
		{
			offset_t size = 0;
			if (!_size(size))
				return false;

			if (!_started)
				_dump_size = size;

			/* "-d" reads up to the size of the first read */
			offset_t const end = _dump && size > _dump_size ? _dump_size : size;

			if (_started && !_follow.changed(end)) {
				_more = false;
				_dump_done = _dump;
				return true;
			}

			offset_t from = _started ? _follow.poll_from(end) : _follow.resync_from(end);
			offset_t to   = end - from > _chunk ? from + _chunk : end;
			if (!_read(from, to))
				return false;

			Range range { };
			if (!_started) {
				range = _follow.start(_buf, _len, from, ~0UL);
				_started = true;
			}
			else if (!_follow.poll(_buf, _len, from, range)) {
				/* rare: the whole window */
				if (from != _follow.resync_from(end) || to != end) {
					from = _follow.resync_from(end);
					to   = end;
					if (!_read(from, to))
						return false;
				}
				range = _follow.resync(_buf, _len, from);
			}

			bool const limited = to < end;
			_chunk = (limited && range.end == range.start)
			       ? Genode::min(2*_chunk, _follow.window()) : size_t(CHUNK);
			_more  = limited;
			_set(range);

			/* the end of a dump: its partial last line too */
			if (_dump && !limited) {
				_end       = _len;
				_dump_done = true;
			}
			return true;
		}

	public:

		/**
		 * Constructor
		 *
		 * \param window  bytes read at most from the file's end
		 */
		Log_file(Genode::Allocator &alloc, Genode::Directory &root,
		         Path const &path, bool dump, size_t window)
		:
			_alloc(alloc), _root(root), _path(path), _dump(dump),
			_follow(window)
		{ }

		~Log_file() { if (_buf) _alloc.free(_buf, _capacity); }

		/**
		 * True if the file is there (a stat, it may block)
		 */
		bool exists()
		{
			offset_t size = 0;
			return _size(size);
		}

		bool pending() const { return *_marker || _pos < _end; }

		/**
		 * True if the next 'update' should come right away, not at a poll
		 */
		bool more() const { return !_dump_done && _more; }

		/**
		 * True if a "-d" stream has sent all
		 */
		bool finished() const { return _dump_done && !pending(); }

		/**
		 * Read what is new, if all read before is taken
		 *
		 * \return  false if the file cannot be read (warned once until a
		 *          read succeeds again)
		 */
		bool update()
		{
			if (pending() || _dump_done)
				return true;

			bool const ok = _update();
			if (!ok)
				_more = false;
			if (!ok && !_warned)
				Genode::warning("logcat: cannot read ", _path);
			_warned = !ok;
			return ok;
		}

		/**
		 * Copy up to 'max' bytes not taken yet to 'dst'
		 *
		 * \return  number of bytes
		 */
		size_t take(char *dst, size_t max)
		{
			size_t n = 0;
			for (; n < max && *_marker; n++) dst[n] = *_marker++;
			for (; n < max && _pos < _end; n++) dst[n] = _buf[_pos++];
			return n;
		}
};

#endif /* _ADB_ENGINE__LOG_FILE_H_ */
