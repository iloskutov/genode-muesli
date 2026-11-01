/*
 * \brief  Follow a growing log file (lf log -f, adb logcat of a log file)
 * \author Ivan Loskutov
 * \date   2026-10-25
 *
 * The file (MüesliOS: /report/log, the report child's vfs_ram_log) is read
 * at its end, at most a window of bytes (default WINDOW). Only whole lines are taken. 'Follow'
 * keeps the offset after the last whole line it took and that line (its
 * last LAST_MAX bytes). A poll reads from the start of that line: if the
 * line is still there, the bytes after it are new. If the file shrank, or
 * the line is gone (the file was replaced, or vfs_ram_log's ring dropped
 * it: evicted bytes read as zeros), the window is searched for the line
 * from its end: the lines after it follow; if it is not there, all whole
 * lines of the window follow a "--- log restarted ---" line. Zero bytes
 * and the partial line after them (or after the cut start of a window)
 * are never taken. No dependencies: lf is a libc program, the adb engine
 * a Genode component.
 */

/*
 * SPDX-License-Identifier: MIT
 */

#ifndef _INCLUDE__MUESLIOS__LOG_FOLLOW_H_
#define _INCLUDE__MUESLIOS__LOG_FOLLOW_H_

namespace Mueslios::Log_follow {

	using size_t   = decltype(sizeof(0));
	using offset_t = unsigned long long;

	/* default window: the report child's ram_log keeps 1 MiB (gems sculpt/report/default) */
	static constexpr size_t WINDOW = 1024*1024;

	static constexpr size_t LAST_MAX = 512;

	static constexpr char const *RESTARTED = "--- log restarted ---\n";

	/**
	 * What to print: RESTARTED if 'restarted', then the bytes [start, end)
	 * of the buffer
	 */
	struct Range { bool restarted; size_t start, end; };

	/**
	 * Start of the whole lines of b[0, n)
	 *
	 * \param cut  the buffer starts within the file (it starts with a
	 *             partial line)
	 *
	 * After the last zero byte, and after the partial line that follows
	 * it or a cut start.
	 */
	static inline size_t valid_start(char const *b, size_t n, bool cut)
	{
		size_t pos = 0;
		for (size_t i = n; i > 0; i--)
			if (b[i - 1] == 0) { pos = i; cut = true; break; }

		if (!cut)
			return 0;

		while (pos < n && b[pos] != '\n') pos++;
		return pos < n ? pos + 1 : n;
	}

	/**
	 * End of the whole lines of b[from, n), 'from' if there is none
	 */
	static inline size_t lines_end(char const *b, size_t from, size_t n)
	{
		for (size_t i = n; i > from; i--)
			if (b[i - 1] == '\n') return i;
		return from;
	}

	/**
	 * Start of the last 'lines' lines of b[from, end)
	 *
	 * A last line without newline counts as a line.
	 */
	static inline size_t tail(char const *b, size_t from, size_t end,
	                          unsigned long lines)
	{
		if (!lines || end == from)
			return end;

		size_t pos = end;
		if (b[pos - 1] == '\n')
			pos--;

		unsigned long seen = 0;
		while (pos > from) {
			if (b[pos - 1] == '\n' && ++seen == lines)
				break;
			pos--;
		}
		return pos;
	}

	static inline bool has_zero(char const *b, size_t n)
	{
		for (size_t i = 0; i < n; i++)
			if (!b[i]) return true;
		return false;
	}

	static inline bool equal(char const *a, char const *b, size_t n)
	{
		for (size_t i = 0; i < n; i++)
			if (a[i] != b[i]) return false;
		return true;
	}

	class Follow;
}


/**
 * State of a follower: where it is in the file
 *
 * Use: 'start' with the window at the file's end ('resync_from(size)' up
 * to the size); then, for each change of the size ('changed'), read from
 * 'poll_from(size)' to the size and call 'poll'; if it returns false, read
 * from 'resync_from(size)' (unless that is where the poll read from) and
 * call 'resync'. The buffer passed must stay until its range is printed.
 * 'start' and 'poll' also take a read that ends before the file's end (a
 * chunk): its partial last line is taken by the next read.
 */
class Mueslios::Log_follow::Follow
{
	private:

		size_t const _window;

		offset_t _offset = 0;   /* after the last whole line taken */

		char   _last[LAST_MAX] { };   /* the bytes before '_offset' */
		size_t _last_len = 0;
		bool   _last_cut = false;     /* only the end of a longer line */

		/*
		 * Take the whole lines of b[from, n) (read from 'base'), print
		 * from 'print_from'
		 */
		Range _take(char const *b, size_t n, offset_t base, size_t from,
		            size_t print_from, bool restarted)
		{
			size_t const end = lines_end(b, from, n);

			if (end > from) {
				size_t start = end - 1;
				while (start > from && b[start - 1] != '\n') start--;

				_last_cut = end - start > LAST_MAX;
				if (_last_cut) start = end - LAST_MAX;

				_last_len = end - start;
				for (size_t i = 0; i < _last_len; i++) _last[i] = b[start + i];

				_offset = base + end;
			}
			return { restarted, print_from < end ? print_from : end, end };
		}

	public:

		/**
		 * Constructor
		 *
		 * \param window  bytes read at most from the file's end (at least
		 *                LAST_MAX)
		 */
		Follow(size_t window = WINDOW)
		: _window(window > LAST_MAX ? window : LAST_MAX) { }

		offset_t offset() const { return _offset; }

		size_t window() const { return _window; }

		/**
		 * Where the window of a file of 'size' bytes starts
		 */
		offset_t resync_from(offset_t size) const
		{
			return size > _window ? size - _window : 0;
		}

		/**
		 * First read: the window at the file's end
		 *
		 * \param b, n   the bytes read from 'base' to the file's end
		 * \param lines  how many of the last whole lines to print
		 */
		Range start(char const *b, size_t n, offset_t base, unsigned long lines)
		{
			_last_len = 0;
			_last_cut = false;

			size_t const valid = valid_start(b, n, base > 0);
			_offset = base + valid;

			return _take(b, n, base, valid,
			             tail(b, valid, lines_end(b, valid, n), lines), false);
		}

		/**
		 * True if a file of 'size' bytes may hold something new
		 */
		bool changed(offset_t size) const { return size != _offset; }

		/**
		 * Where a poll of a file of 'size' bytes reads from
		 */
		offset_t poll_from(offset_t size) const
		{
			offset_t const from = _offset - _last_len;

			if (size < _offset || size - from > _window)
				return resync_from(size);

			return from;
		}

		/**
		 * The bytes b[0, n) read from 'base' ('poll_from') to the file's end
		 *
		 * \return  false if the last line is not where it was: 'resync'
		 */
		bool poll(char const *b, size_t n, offset_t base, Range &range)
		{
			if (base != _offset - _last_len || n < _last_len ||
			    has_zero(b, n) || !equal(b, _last, _last_len))
				return false;

			range = _take(b, n, base, _last_len, _last_len, false);
			return true;
		}

		/**
		 * The window b[0, n) read from 'base' ('resync_from') to the end
		 */
		Range resync(char const *b, size_t n, offset_t base)
		{
			size_t const valid = valid_start(b, n, base > 0);
			size_t const end   = lines_end(b, valid, n);
			size_t const len   = _last_len;

			/* the last occurrence of the last line */
			if (len && end - valid >= len)
				for (size_t p = end - len + 1; p-- > valid; ) {
					bool const line_start = _last_cut || p == valid || b[p - 1] == '\n';
					if (line_start && equal(b + p, _last, len)) {
						_offset = base + p + len;
						return _take(b, n, base, p + len, p + len, false);
					}
				}

			_last_len = 0;
			_last_cut = false;
			_offset   = base + valid;
			return _take(b, n, base, valid, valid, true);
		}
};

#endif /* _INCLUDE__MUESLIOS__LOG_FOLLOW_H_ */
