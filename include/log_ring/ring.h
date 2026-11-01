/*
 * \brief  Bounded log history with independent reader cursors
 * \author Ivan Loskutov
 * \date   2026-10-24
 *
 * Keeps the last bytes written, as many as the buffer of the constructor holds.
 * A reader holds a cursor (absolute byte count); one that fell behind the history
 * continues at its oldest byte.
 * No Genode dependencies.
 */

/*
 * SPDX-License-Identifier: MIT
 */

#ifndef _INCLUDE__LOG_RING__RING_H_
#define _INCLUDE__LOG_RING__RING_H_

namespace Log_ring { struct Ring; }


struct Log_ring::Ring
{
	struct Cursor { unsigned long long pos = 0; };

	char * const        _data;
	unsigned long const _size;
	unsigned long long  _total = 0;   /* bytes ever written */

	unsigned long long _start() const { return _total > _size ? _total - _size : 0; }

	Ring(Ring const &) = delete;
	Ring &operator = (Ring const &) = delete;

	/**
	 * \param size  size of 'data', must not be 0
	 */
	Ring(char *data, unsigned long size) : _data(data), _size(size) { }

	void put(char const *src, unsigned long len)
	{
		for (unsigned long i = 0; i < len; i++)
			_data[(_total + i) % _size] = src[i];
		_total += len;
	}

	Cursor oldest() const { return Cursor { _start() }; }
	Cursor newest() const { return Cursor { _total }; }

	unsigned long avail(Cursor const &c) const
	{
		unsigned long long const from = c.pos < _start() ? _start() : c.pos;
		return (unsigned long)(_total - from);
	}

	/**
	 * Copy up to 'max' bytes from the cursor on, advance it
	 *
	 * \return bytes copied
	 */
	unsigned long read(Cursor &c, char *dst, unsigned long max) const
	{
		if (c.pos < _start()) c.pos = _start();

		unsigned long n = 0;
		for (; n < max && c.pos < _total; n++, c.pos++)
			dst[n] = _data[c.pos % _size];
		return n;
	}
};

#endif /* _INCLUDE__LOG_RING__RING_H_ */
