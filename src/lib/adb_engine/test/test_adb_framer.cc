/* Host test: Adb::Framer (message reassembly from a byte stream) */
#include <adb/framer.h>
#include <cstdio>
#include <cstring>

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

using namespace Usb_gadget::Adb;

static unsigned msg(unsigned cmd, char const *payload, unsigned len, unsigned char *out)
{
	write(Header { cmd, 1, 2, len, checksum((unsigned char const *)payload, len) }, out);
	std::memcpy(out + HEADER_SIZE, payload, len);
	return HEADER_SIZE + len;
}

int main()
{
	/* framer_split_header */
	{
		char const *banner = "host::features=shell_v2,cmd,x";  /* 29 + NUL = 30 */
		unsigned char buf[64];
		unsigned const n = msg(CNXN, banner, 30, buf);
		::Adb::Framer f;
		int calls = 0; bool ok = false;
		for (unsigned i = 0; i < n; i++)
			f.feed((char const *)buf + i, 1, [&] (Header const &h, char const *p, size_t l) {
				calls++;
				ok = h.command == CNXN && l == 30 && !std::memcmp(p, banner, 30);
			});
		CHECK(calls == 1);
		CHECK(ok);
		CHECK(!f.broken());
	}

	/* framer_two_in_one */
	{
		unsigned char buf[128];
		unsigned n = msg(WRTE, "abc", 3, buf);
		n += msg(OPEN, "shell:", 6, buf + n);
		::Adb::Framer f;
		unsigned cmds[2] { }; int calls = 0;
		f.feed((char const *)buf, n, [&] (Header const &h, char const *, size_t) {
			if (calls < 2) cmds[calls] = h.command;
			calls++; });
		CHECK(calls == 2);
		CHECK(cmds[0] == WRTE && cmds[1] == OPEN);
	}

	/* framer_zero_payload */
	{
		unsigned char buf[32];
		unsigned const n = msg(OKAY, "", 0, buf);
		::Adb::Framer f;
		int calls = 0; size_t len = 99;
		f.feed((char const *)buf, n, [&] (Header const &h, char const *, size_t l) {
			if (h.command == OKAY) len = l;
			calls++; });
		CHECK(calls == 1 && len == 0);
	}

	/* framer_bad_magic */
	{
		unsigned char buf[32];
		unsigned n = msg(OKAY, "", 0, buf);
		buf[20] ^= 0xff;
		::Adb::Framer f;
		int calls = 0;
		auto const count = [&] (Header const &, char const *, size_t) { calls++; };
		f.feed((char const *)buf, n, count);
		CHECK(calls == 0);
		CHECK(f.broken());
		f.feed((char const *)buf, n, count);  /* stays broken */
		CHECK(calls == 0 && f.broken());
		f.reset();
		CHECK(!f.broken());
		n = msg(OKAY, "", 0, buf);
		f.feed((char const *)buf, n, count);
		CHECK(calls == 1 && !f.broken());
	}

	/* framer_oversize */
	{
		unsigned char buf[HEADER_SIZE];
		write(Header { WRTE, 1, 2, MAX_PAYLOAD + 1, 0 }, buf);
		::Adb::Framer f;
		int calls = 0;
		f.feed((char const *)buf, sizeof(buf), [&] (Header const &, char const *, size_t) { calls++; });
		CHECK(calls == 0);
		CHECK(f.broken());
	}

	/* max payload fits */
	{
		static unsigned char buf[HEADER_SIZE + MAX_PAYLOAD];
		static char payload[MAX_PAYLOAD];
		std::memset(payload, 7, sizeof(payload));
		unsigned const n = msg(WRTE, payload, MAX_PAYLOAD, buf);
		::Adb::Framer f;
		size_t len = 0;
		f.feed((char const *)buf, n, [&] (Header const &, char const *, size_t l) { len = l; });
		CHECK(len == MAX_PAYLOAD && !f.broken());
	}

	std::printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
	return failures ? 1 : 0;
}
