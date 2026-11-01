/* Host test: Adb::Shell_v2 (packets of the shell protocol v2) */
#include <adb/shell_v2.h>
#include <cstdio>
#include <cstring>

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

using namespace ::Adb::Shell_v2;

int main()
{
	/* v2_write_exit */
	{
		char out[16];
		size_t n = write(EXIT, "\x03", 1, out, sizeof(out));
		CHECK(n == 6);
		CHECK(!std::memcmp(out, "\x03\x01\x00\x00\x00\x03", 6));
		CHECK(write(STDOUT, "abc", 3, out, 7) == 0);  /* too small */
	}
	/* v2_reader_split */
	{
		char const pkt[] = "\x00\x05\x00\x00\x00hello";  /* STDIN, 5 bytes */
		Reader r;
		int calls = 0; bool ok = false;
		auto const fn = [&] (Id id, char const *d, size_t l) {
			calls++; ok = id == STDIN && l == 5 && !std::memcmp(d, "hello", 5); };
		r.feed(pkt, 2, fn);
		r.feed(pkt + 2, 5, fn);
		CHECK(calls == 0);
		r.feed(pkt + 7, 3, fn);
		CHECK(calls == 1 && ok);
	}
	/* v2_reader_two */
	{
		char const pkt[] = "\x00\x02\x00\x00\x00hi" "\x04\x00\x00\x00\x00";
		Reader r;
		Id ids[3]; int calls = 0; size_t l0 = 0, l1 = 9;
		r.feed(pkt, 7 + 5, [&] (Id id, char const *, size_t l) {
			if (calls < 2) { ids[calls] = id; (calls ? l1 : l0) = l; }
			calls++; });
		CHECK(calls == 2);
		CHECK(ids[0] == STDIN && ids[1] == CLOSE_STDIN && l0 == 2 && l1 == 0);
	}

	/* v2_reader_oversize_chunked: 70000 data bytes in 1000-byte chunks, then a good packet */
	{
		static char big[5 + 70000], tail[7];
		big[0] = 0; big[1] = 0x70; big[2] = 0x11; /* 70000 = 0x11170 */ big[3] = 1; big[4] = 0;
		std::memset(big + 5, 'z', 70000);
		std::memcpy(tail, "\x00\x02\x00\x00\x00ok", 7);
		Reader r;
		int calls = 0; size_t l0 = 0; bool ok = false;
		auto const fn = [&] (Id id, char const *d, size_t l) {
			if (calls++ == 0) l0 = l; else ok = id == STDIN && l == 2 && !std::memcmp(d, "ok", 2); };
		for (size_t o = 0; o < sizeof(big); o += 1000)
			r.feed(big + o, sizeof(big) - o < 1000 ? sizeof(big) - o : 1000, fn);
		r.feed(tail, 7, fn);
		CHECK(calls == 2 && l0 == 64*1024 && ok);
	}

	std::printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
	return failures ? 1 : 0;
}
