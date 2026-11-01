/* Host test: Mueslios::Log_follow (lf log -f, adb logcat over /report/log) */
#include <mueslios/log_follow.h>
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

using namespace Mueslios::Log_follow;

/*
 * A file of 'size' bytes whose first 'evicted' bytes read as zeros (the
 * ring of vfs_ram_log), the others from 'content' at the same offsets
 */
struct File
{
	std::string content;
	offset_t    evicted = 0;

	offset_t size() const { return content.size(); }

	std::string read(offset_t from) const
	{
		std::string s = content.substr(size_t(from));
		for (offset_t i = from; i < evicted && i < size(); i++)
			s[size_t(i - from)] = 0;
		return s;
	}
};

/* what a follower prints: the marker, then the range */
static std::string text(std::string const &buf, Range const &r)
{
	return (r.restarted ? std::string(RESTARTED) : std::string())
	     + buf.substr(r.start, r.end - r.start);
}

/* one poll as lf and the engine do it */
static std::string poll(Follow &f, File const &file)
{
	if (!f.changed(file.size())) return "";

	offset_t from = f.poll_from(file.size());
	std::string buf = file.read(from);
	Range r { };
	if (!f.poll(buf.data(), buf.size(), from, r)) {
		if (from != f.resync_from(file.size())) {
			from = f.resync_from(file.size());
			buf  = file.read(from);
		}
		r = f.resync(buf.data(), buf.size(), from);
	}
	return text(buf, r);
}

static std::string start(Follow &f, File const &file, unsigned long lines)
{
	offset_t const from = f.resync_from(file.size());
	std::string const buf = file.read(from);
	return text(buf, f.start(buf.data(), buf.size(), from, lines));
}

int main()
{
	/* helpers */
	{
		char const b[] = "\0\0rt\nab\ncd";
		CHECK(valid_start(b, 10, false) == 5);    /* zeros and the partial line */
		CHECK(valid_start("ab\ncd", 5, false) == 0);
		CHECK(valid_start("ab\ncd", 5, true) == 3); /* a cut window */
		CHECK(valid_start("abc", 3, true) == 3);
		CHECK(lines_end("ab\ncd", 0, 5) == 3);
		CHECK(lines_end("abcd", 0, 4) == 0);
		CHECK(tail("a\nb\nc\n", 0, 6, 2) == 2);
		CHECK(tail("a\nb\nc", 0, 5, 1) == 4);     /* a last line without newline */
		CHECK(tail("a\nb\n", 0, 4, 9) == 0);
		CHECK(tail("a\nb\n", 0, 4, 0) == 4);
	}

	/* start_lines: the last N whole lines, a partial one waits */
	{
		File file { "a\nb\nc\npar", 0 };
		Follow f;
		CHECK(start(f, file, 2) == "b\nc\n");
		CHECK(f.offset() == 6);
		CHECK(poll(f, file) == "");               /* the partial line is no line */
		file.content += "t\n";
		CHECK(poll(f, file) == "part\n");
		CHECK(!f.changed(file.size()));
	}

	/* start_zero_lines: -n 0 prints nothing, then follows */
	{
		File file { "a\nb\n", 0 };
		Follow f;
		CHECK(start(f, file, 0) == "");
		file.content += "c\n";
		CHECK(poll(f, file) == "c\n");
	}

	/* append: the bytes after the last line */
	{
		File file { "a\nb\n", 0 };
		Follow f;
		CHECK(start(f, file, 1) == "b\n");
		file.content += "c\nd\n";
		CHECK(poll(f, file) == "c\nd\n");
		file.content += "e\n";
		CHECK(poll(f, file) == "e\n");
	}

	/* wrap_keeping_up: the ring drops old bytes, the last line is still there */
	{
		File file { "a\nb\n", 0 };
		Follow f;
		CHECK(start(f, file, 9) == "a\nb\n");
		file.content += "c\n";
		file.evicted = 2;                         /* "a\n" gone */
		CHECK(poll(f, file) == "c\n");
	}

	/* wrap_behind: the last line was dropped too, the rest after the marker */
	{
		File file { "a\nb\n", 0 };
		Follow f;
		CHECK(start(f, file, 9) == "a\nb\n");
		file.content += "ccc\nd\ne\n";
		file.evicted = 6;                         /* up to the middle of "ccc" */
		CHECK(poll(f, file) == std::string(RESTARTED) + "d\ne\n");
		file.content += "f\n";
		CHECK(poll(f, file) == "f\n");
	}

	/* replaced_found: the new content holds the last line, what follows it */
	{
		File file { "a\nb\nc\n", 0 };
		Follow f;
		CHECK(start(f, file, 1) == "c\n");
		file.content = "q\nc\nr\ns\nt\n";         /* bigger, other bytes before 6 */
		CHECK(poll(f, file) == "r\ns\nt\n");
	}

	/* shrunk_found: smaller, the last line in it */
	{
		File file { "a\nb\nc\nd\n", 0 };
		Follow f;
		CHECK(start(f, file, 1) == "d\n");
		file.content = "d\nr\n";
		CHECK(poll(f, file) == "r\n");
	}

	/* shrunk_new: a new log, from its start after the marker */
	{
		File file { "a\nb\nc\n", 0 };
		Follow f;
		CHECK(start(f, file, 1) == "c\n");
		file.content = "x\ny\n";
		CHECK(poll(f, file) == std::string(RESTARTED) + "x\ny\n");
		file.content += "z\n";
		CHECK(poll(f, file) == "z\n");
	}

	/* truncated_then_written: an empty file in between */
	{
		File file { "a\nb\n", 0 };
		Follow f;
		CHECK(start(f, file, 1) == "b\n");
		file.content = "";
		CHECK(poll(f, file) == RESTARTED);
		file.content = "n\n";
		CHECK(poll(f, file) == "n\n");
	}

	/* same_line_twice: the last occurrence counts */
	{
		File file { "x\nb\n", 0 };
		Follow f;
		CHECK(start(f, file, 1) == "b\n");
		file.content = "b\nm\nb\nn\n";
		CHECK(poll(f, file) == "n\n");
	}

	/* zeros_in_new_bytes: never printed */
	{
		File file { "a\nb\n", 0 };
		Follow f;
		CHECK(start(f, file, 1) == "b\n");
		file.content = std::string(12, 'z') + "part\nk\n";
		file.evicted = 12;
		std::string const out = poll(f, file);
		CHECK(out == std::string(RESTARTED) + "k\n");
		CHECK(out.find('\0') == std::string::npos);
	}

	/* zeros_after_last_line: the last line in place, zeros behind it */
	{
		File file { "a\nb\n", 0 };
		Follow f;
		CHECK(start(f, file, 1) == "b\n");
		file.content += std::string("\0\0x\ny\n", 6);
		CHECK(poll(f, file) == std::string(RESTARTED) + "y\n");
	}

	/* window: a big file is read from WINDOW before its end */
	{
		File file { std::string(WINDOW + 10, 'x'), 0 };
		file.content[5] = '\n';
		file.content += "\nlast\n";
		Follow f;
		std::string const out = start(f, file, 1);
		CHECK(out == "last\n");
		CHECK(f.resync_from(file.size()) == file.size() - WINDOW);
		CHECK(f.offset() == file.size());
		file.content += std::string(WINDOW + 1, 'y') + "\nnew\n";
		CHECK(poll(f, file) == std::string(RESTARTED) + "new\n");
	}

	/* long_line: longer than LAST_MAX, its end is the last line */
	{
		std::string const lng = std::string(LAST_MAX + 100, 'l') + "\n";
		File file { "a\n" + lng, 0 };
		Follow f;
		CHECK(start(f, file, 1) == lng);
		file.content += "b\n";
		CHECK(poll(f, file) == "b\n");
		file.content = "q\n" + lng + "b\nr\n";     /* same offsets, bigger */
		CHECK(poll(f, file) == "r\n");

		/* only the end of the long line is kept: found in the middle of a line */
		File other { "q\n" + lng, 0 };
		Follow g;
		CHECK(start(g, other, 1) == lng);
		other.content = lng.substr(50) + "t\n";    /* shrunk */
		CHECK(poll(g, other) == "t\n");
	}

	/* small_window: a window of its own */
	{
		File file { "aaaa\nbbbb\ncccc\n", 0 };
		Follow f(LAST_MAX);
		CHECK(f.window() == LAST_MAX);
		CHECK(start(f, file, 9) == "aaaa\nbbbb\ncccc\n");
		file.content += std::string(LAST_MAX + 1, 'q') + "\nnew\n";
		CHECK(poll(f, file) == std::string(RESTARTED) + "new\n");
		CHECK(Follow(1).window() == LAST_MAX);
	}

	/* chunks: reads that end before the file's end */
	{
		File file { "one\ntwo\nthree\nfour\n", 0 };
		Follow f;
		std::string out, buf = file.content.substr(0, 6);   /* "one\ntw" */
		out += text(buf, f.start(buf.data(), buf.size(), 0, ~0UL));
		CHECK(out == "one\n" && f.offset() == 4);
		/* a chunk without a new whole line grows, as Adb::Log_file does */
		size_t chunk = 8;
		for (int i = 0; i < 10 && f.changed(file.size()); i++) {
			offset_t const from = f.poll_from(file.size());
			buf = file.content.substr(size_t(from), chunk);
			Range r { };
			CHECK(f.poll(buf.data(), buf.size(), from, r));
			chunk = r.end > r.start ? 8 : chunk*2;
			out += text(buf, r);
		}
		CHECK(out == file.content);
	}

	std::printf("log_follow: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
