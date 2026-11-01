/* Host test: Usb_gadget::Adb_sync (file sync protocol v1, byte-stream decoder) */
#include <usb_gadget/adb_sync.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

using namespace Usb_gadget::Adb_sync;

static unsigned u32(unsigned char const *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static void put32(std::string &s, unsigned v) { for (int i = 0; i < 4; i++) s += char(v >> (8*i)); }

static std::string request(unsigned id, std::string const &path)
{
	std::string s; put32(s, id); put32(s, (unsigned)path.size()); return s + path;
}

static std::string chunk(unsigned id, std::string const &data)
{
	std::string s; put32(s, id); put32(s, (unsigned)data.size()); return s + data;
}

struct Recorder
{
	std::vector<std::string> log;
	std::string data;

	void request(unsigned id, char const *path) { log.push_back(std::string(1, char(id)) + ":" + path); }
	void data_piece(unsigned char const *p, unsigned long n) { data.append((char const *)p, n); }
	void done(unsigned mtime) { log.push_back("done:" + std::to_string(mtime)); }
	void error(char const *what) { log.push_back(std::string("error:") + what); }
};

static void feed(Decoder &d, Recorder &r, std::string const &bytes, unsigned piece)
{
	for (unsigned o = 0; o < bytes.size(); o += piece) {
		unsigned const n = (unsigned)std::min<size_t>(piece, bytes.size() - o);
		d.feed((unsigned char const *)bytes.data() + o, n,
		       [&] (unsigned id, char const *path) { r.request(id, path); },
		       [&] (unsigned char const *p, unsigned long len) { r.data_piece(p, len); },
		       [&] (unsigned mtime) { r.done(mtime); },
		       [&] (char const *what) { r.error(what); });
	}
}

int main()
{
	CHECK(STAT == 0x54415453 && LIST == 0x5453494c && SEND == 0x444e4553);
	CHECK(RECV == 0x56434552 && DATA == 0x41544144 && DONE == 0x454e4f44);
	CHECK(QUIT == 0x54495551 && DENT == 0x544e4544);

	/* requests, whole and split at every byte */
	for (unsigned piece : { 100u, 1u, 3u }) {
		Decoder d; Recorder r;
		feed(d, r, request(STAT, "/data/a.txt") + request(RECV, "/data/a.txt") + request(QUIT, ""), piece);
		CHECK(r.log.size() == 3);
		CHECK(r.log[0] == std::string(1, char(STAT)) + ":/data/a.txt");
		CHECK(r.log[1] == std::string(1, char(RECV)) + ":/data/a.txt");
		CHECK(r.log[2] == std::string(1, char(QUIT)) + ":");
	}

	/* send: request, data chunks (split anyhow), done with mtime, next request */
	for (unsigned piece : { 1000u, 1u, 7u }) {
		Decoder d; Recorder r;
		std::string done; put32(done, DONE); put32(done, 1700000000);
		feed(d, r, request(SEND, "/tmp/f,33188") + chunk(DATA, "hello ") + chunk(DATA, "world")
		         + done + request(STAT, "/tmp/f"), piece);
		CHECK(r.log.size() == 3);
		CHECK(r.log[0] == std::string(1, char(SEND)) + ":/tmp/f,33188");
		CHECK(r.log[1] == "done:1700000000");
		CHECK(r.log[2] == std::string(1, char(STAT)) + ":/tmp/f");
		CHECK(r.data == "hello world");
	}

	/* broken: path too long, unknown chunk in a send */
	{
		Decoder d; Recorder r;
		std::string s; put32(s, STAT); put32(s, 5000);
		feed(d, r, s, 100);
		CHECK(r.log.size() == 1 && r.log[0].rfind("error:", 0) == 0);
	}
	{
		Decoder d; Recorder r;
		feed(d, r, request(SEND, "/x,420") + chunk(STAT, "zz"), 100);
		CHECK(r.log.size() == 2 && r.log[1].rfind("error:", 0) == 0);
	}

	/* encoders */
	{
		unsigned char b[64];
		CHECK(stat_v1(b, 0100644, 11, 99) == 16);
		CHECK(u32(b) == STAT && u32(b + 4) == 0100644 && u32(b + 8) == 11 && u32(b + 12) == 99);

		CHECK(dent_v1(b, sizeof(b), 040755, 0, 0, "dir") == 23);
		CHECK(u32(b) == DENT && u32(b + 4) == 040755 && u32(b + 16) == 3 && std::memcmp(b + 20, "dir", 3) == 0);
		CHECK(dent_v1(b, 20, 040755, 0, 0, "dir") == 0);   /* too small */

		CHECK(dent_done(b) == 20 && u32(b) == DONE && u32(b + 16) == 0);
		CHECK(data_header(b, 1234) == 8 && u32(b) == DATA && u32(b + 4) == 1234);
		CHECK(done(b) == 8 && u32(b) == DONE && u32(b + 4) == 0);
		CHECK(okay(b) == 8 && u32(b) == 0x59414b4f && u32(b + 4) == 0);
		CHECK(fail(b, sizeof(b), "no such file") == 20 && u32(b) == 0x4c494146 && u32(b + 4) == 12);
	}

	/* send_target: "path,mode" */
	{
		char path[64]; unsigned mode = 0;
		CHECK(send_target("/data/x.txt,33188", path, sizeof(path), mode));
		CHECK(std::strcmp(path, "/data/x.txt") == 0 && mode == 33188);
		CHECK(send_target("/a,b/c,420", path, sizeof(path), mode));
		CHECK(std::strcmp(path, "/a,b/c") == 0 && mode == 420);
		CHECK(!send_target("/nomode", path, sizeof(path), mode));
	}

	std::printf("test_adb_sync: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
