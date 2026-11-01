/*
 * \brief  adb protocol: message header and connection handshake
 * \author Ivan Loskutov
 * \date   2026-10-24
 *
 * A message is a 24-byte header (six little-endian u32: command, arg0,
 * arg1, data_length, data_check, magic = command ^ 0xffffffff) followed by
 * data_length bytes of payload, each sent as a USB transfer of its own
 * (AOSP adb protocol.txt). The device trusts any host: it answers CNXN
 * with CNXN, never AUTH. No Genode dependencies.
 */

/*
 * SPDX-License-Identifier: MIT
 */

#ifndef _INCLUDE__USB_GADGET__ADB_H_
#define _INCLUDE__USB_GADGET__ADB_H_

namespace Usb_gadget::Adb {

	enum Command : unsigned {
		CNXN = 0x4e584e43, OPEN = 0x4e45504f, OKAY = 0x59414b4f,
		CLSE = 0x45534c43, WRTE = 0x45545257, AUTH = 0x48545541,
		STLS = 0x534c5453 };

	enum : unsigned {
		HEADER_SIZE = 24,
		VERSION     = 0x01000001,  /* no data checksum needed */
		MAX_PAYLOAD = 64*1024,
	};

	struct Header
	{
		unsigned command, arg0, arg1, data_length, data_check;
	};

	struct Device_info
	{
		char const *name, *model, *device;  /* ro.product.* */
	};

	static inline unsigned checksum(unsigned char const *p, unsigned len)
	{
		unsigned sum = 0;
		for (unsigned i = 0; i < len; i++) sum += p[i];
		return sum;
	}

	static inline void write(Header const &h, unsigned char *raw)
	{
		unsigned const v[6] = { h.command, h.arg0, h.arg1, h.data_length,
		                        h.data_check, h.command ^ 0xffffffffu };
		for (unsigned i = 0; i < 6; i++)
			for (unsigned b = 0; b < 4; b++)
				raw[i*4 + b] = (unsigned char)(v[i] >> (8*b));
	}

	/**
	 * \return false if 'len' is no header size, the magic is wrong, or the
	 *         payload exceeds MAX_PAYLOAD
	 */
	static inline bool parse(unsigned char const *raw, unsigned long len, Header &h)
	{
		if (len != HEADER_SIZE) return false;

		unsigned v[6];
		for (unsigned i = 0; i < 6; i++)
			v[i] = raw[i*4] | raw[i*4 + 1] << 8 | raw[i*4 + 2] << 16
			     | (unsigned)raw[i*4 + 3] << 24;

		if (v[5] != (v[0] ^ 0xffffffffu) || v[3] > MAX_PAYLOAD)
			return false;

		h = Header { v[0], v[1], v[2], v[3], v[4] };
		return true;
	}

	/**
	 * Banner of our CNXN: "device::ro.product.name=..;...;features=shell_v2"
	 *
	 * \return length (no terminating NUL), 0 if 'max' is too small
	 */
	static inline unsigned banner_of(Device_info const &info, char *out, unsigned max)
	{
		char const *parts[] = { "device::ro.product.name=", info.name,
		                        ";ro.product.model=", info.model,
		                        ";ro.product.device=", info.device,
		                        ";features=shell_v2" };
		unsigned n = 0;
		for (char const *s : parts)
			for (; *s; s++) {
				if (n == max) return 0;
				out[n++] = *s;
			}
		return n;
	}

	/* CNXN answer: our version, the smaller maximum payload, the banner */
	static inline Header cnxn_reply(Header const &host, unsigned banner_len,
	                                unsigned banner_check)
	{
		unsigned const max = host.arg1 < MAX_PAYLOAD ? host.arg1 : MAX_PAYLOAD;
		return Header { CNXN, VERSION, max, banner_len, banner_check };
	}

	/* the stream the host asked for does not exist (yet) */
	static inline Header refuse(Header const &open)
	{
		return Header { CLSE, 0, open.arg0, 0, 0 };
	}

	static inline Header okay(unsigned local, unsigned remote) {
		return Header { OKAY, local, remote, 0, 0 }; }

	static inline Header wrte(unsigned local, unsigned remote, unsigned len, unsigned check) {
		return Header { WRTE, local, remote, len, check }; }

	static inline Header clse(unsigned local, unsigned remote) {
		return Header { CLSE, local, remote, 0, 0 }; }

	/**
	 * Whether 'service' (payload of the host's OPEN) runs logcat
	 *
	 * Host adb asks for "shell:export ANDROID_LOG_TAGS=\"\" ; exec logcat
	 * '-d' ..." (every argument single-quoted); also accepted: "shell:logcat
	 * [args]", "exec:logcat [args]", arguments quoted or not. The command
	 * must start with the word logcat or hold "exec logcat": other commands
	 * that mention logcat ("echo logcat") are no log stream.
	 *
	 * \param dump  set if the arguments hold "-d" (print and exit)
	 */
	static inline bool logcat_service(char const *service, bool &dump)
	{
		auto const prefix = [] (char const *s, char const *p) {
			for (; *p; s++, p++) if (*s != *p) return false;
			return true; };
		auto const space = [] (char c) { return c == ' ' || c == '\t' || c == ';'; };
		auto const blank = [] (char c) { return c == ' ' || c == '\t'; };
		auto const logcat_word = [&] (char const *s) {
			return prefix(s, "logcat") && (!s[6] || space(s[6])); };

		if      (prefix(service, "shell:")) service += 6;
		else if (prefix(service, "exec:"))  service += 5;
		else return false;

		/* "logcat" as the first word or after the word "exec" */
		char const *s = service;
		while (blank(*s)) s++;
		if (!logcat_word(s))
			for (s = service; *s; s++) {
				if ((s != service && !space(s[-1])) || !prefix(s, "exec")
				 || !blank(s[4]))
					continue;
				char const *w = s + 4;
				while (blank(*w)) w++;
				if (logcat_word(w)) { s = w; break; }
			}
		if (!*s) return false;

		/* its arguments, as they are or single-quoted (host adb's escape_arg) */
		dump = false;
		for (s += 6; *s; s++) {
			if (!space(s[-1])) continue;
			char const *a = (*s == '\'') ? s + 1 : s;
			char const end = a[2];
			if (prefix(a, "-d") && (a == s ? (!end || space(end)) : end == '\''))
				dump = true;
		}
		return true;
	}
}

#endif /* _INCLUDE__USB_GADGET__ADB_H_ */
