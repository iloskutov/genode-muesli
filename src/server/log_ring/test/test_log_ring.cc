/* Host test: Log_ring::Ring (bounded log history with reader cursors) */
#include <log_ring/ring.h>
#include <cstdio>
#include <cstring>

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main()
{
	using Ring = Log_ring::Ring;

	char data[16];

	/* read_from_start: a new reader gets everything there is */
	{
		Ring r { data, sizeof(data) };
		r.put("hello ", 6);
		r.put("world", 5);
		Ring::Cursor c = r.oldest();
		char buf[32] { };
		CHECK(r.avail(c) == 11);
		CHECK(r.read(c, buf, sizeof(buf)) == 11);
		CHECK(std::memcmp(buf, "hello world", 11) == 0);
		CHECK(r.avail(c) == 0);
		CHECK(r.read(c, buf, sizeof(buf)) == 0);
	}

	/* overwrite: only the last 16 bytes are kept, a slow reader skips ahead */
	{
		Ring r { data, sizeof(data) };
		Ring::Cursor c = r.oldest();
		r.put("0123456789", 10);
		r.put("abcdefghij", 10);   /* 20 bytes, "0123" lost */
		char buf[32] { };
		CHECK(r.avail(c) == 16);
		CHECK(r.read(c, buf, sizeof(buf)) == 16);
		CHECK(std::memcmp(buf, "456789abcdefghij", 16) == 0);
	}

	/* partial_reads: small buffers, the cursor advances */
	{
		Ring r { data, sizeof(data) };
		r.put("abcdef", 6);
		Ring::Cursor c = r.oldest();
		char buf[4] { };
		CHECK(r.read(c, buf, 4) == 4 && std::memcmp(buf, "abcd", 4) == 0);
		CHECK(r.read(c, buf, 4) == 2 && std::memcmp(buf, "ef", 2) == 0);
		r.put("gh", 2);
		CHECK(r.read(c, buf, 4) == 2 && std::memcmp(buf, "gh", 2) == 0);
	}

	/* newest: a follower that wants only new data */
	{
		Ring r { data, sizeof(data) };
		r.put("old", 3);
		Ring::Cursor c = r.newest();
		CHECK(r.avail(c) == 0);
		r.put("new", 3);
		char buf[8] { };
		CHECK(r.read(c, buf, 8) == 3 && std::memcmp(buf, "new", 3) == 0);
	}

	/* put larger than the ring keeps its tail */
	{
		Ring r { data, sizeof(data) };
		r.put("0123456789abcdefXYZ", 19);
		Ring::Cursor c = r.oldest();
		char buf[32] { };
		CHECK(r.read(c, buf, 32) == 16);
		CHECK(std::memcmp(buf, "3456789abcdefXYZ", 16) == 0);
	}

	/* the size is the one of the constructor */
	{
		Ring r { data, 4 };
		r.put("0123456789", 10);
		Ring::Cursor c = r.oldest();
		char buf[8] { };
		CHECK(r.avail(c) == 4);
		CHECK(r.read(c, buf, 8) == 4 && std::memcmp(buf, "6789", 4) == 0);
	}

	std::printf("test_log_ring: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
