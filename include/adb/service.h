/*
 * \brief  adb: service string of an OPEN message
 * \author Ivan Loskutov
 * \date   2026-10-24
 *
 * Classifies the service the host asks for: shell (v1, v2, exec), sync,
 * logcat, reboot. No Genode dependencies.
 */

/*
 * SPDX-License-Identifier: MIT
 */

#ifndef _INCLUDE__ADB__SERVICE_H_
#define _INCLUDE__ADB__SERVICE_H_

#include <usb_gadget/adb.h>

namespace Adb {

	/* no <cstddef>: a Genode component without libc includes this too */
	using size_t = decltype(sizeof(0));

	struct Service
	{
		enum Kind { SHELL, SYNC, LOGCAT, REBOOT, OTHER };

		Kind        kind;
		bool        v2;
		bool        pty;
		char const *command;      /* points into the parsed string */
		size_t      command_len;  /* 0: interactive */
	};

	/**
	 * Parse the payload of an OPEN message ('s' need not be terminated)
	 */
	static inline Service parse_service(char const *s, size_t len)
	{
		auto const starts = [&] (char const *p) {
			size_t i = 0;
			for (; p[i]; i++) if (i >= len || s[i] != p[i]) return false;
			return true; };

		Service r { Service::OTHER, false, false, s + len, 0 };

		/* the part up to the first ':' names the service and its options */
		size_t colon = 0;
		while (colon < len && s[colon] != ':') colon++;
		if (colon == len) return r;

		auto const set_command = [&] {
			r.command     = s + colon + 1;
			r.command_len = len - colon - 1; };

		if (starts("shell:") || starts("exec:") || starts("shell,")) {

			bool const exec = starts("exec:");
			r.kind = Service::SHELL;
			set_command();

			bool pty_opt = false, raw_opt = false;
			if (starts("shell,")) {
				/* options, comma separated, up to the colon */
				for (size_t i = 6; i <= colon; ) {
					size_t e = i;
					while (e < colon && s[e] != ',') e++;
					size_t const l = e - i;
					if (l == 2 && !__builtin_memcmp(s + i, "v2", 2))  r.v2 = true;
					if (l == 3 && !__builtin_memcmp(s + i, "raw", 3)) raw_opt = true;
					if (l == 3 && !__builtin_memcmp(s + i, "pty", 3)) pty_opt = true;
					i = e + 1;
				}
			}
			r.pty = !exec && (pty_opt || (!raw_opt && r.command_len == 0));

			/* logcat is served by its own stream, whatever the shell flavor */
			if (r.command_len) {
				char tmp[256];
				size_t const prefix = 6;  /* logcat_service() wants "shell:<cmd>" */
				size_t take = r.command_len;

				/* a long command is cut at a word boundary (the words
				 * before it decide) */
				if (take > sizeof(tmp) - prefix - 1) {
					take = sizeof(tmp) - prefix - 1;
					while (take && r.command[take] != ' ' && r.command[take] != '\t'
					            && r.command[take] != ';')
						take--;
				}

				size_t n = 0;
				for (char const *p = "shell:"; *p; p++) tmp[n++] = *p;
				for (size_t i = 0; i < take; i++) tmp[n++] = r.command[i];
				tmp[n] = 0;

				bool dump = false;
				if (Usb_gadget::Adb::logcat_service(tmp, dump))
					r.kind = Service::LOGCAT;
			}
		}
		else if (starts("sync:"))   { r.kind = Service::SYNC; }
		else if (starts("reboot:")) { r.kind = Service::REBOOT; set_command(); }

		return r;
	}
}

#endif /* _INCLUDE__ADB__SERVICE_H_ */
