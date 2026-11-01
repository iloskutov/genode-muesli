/* Host test: Usb_gadget::Adb (adb message header, CNXN banner) */
#include <usb_gadget/adb.h>
#include <cstdio>
#include <cstring>

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

using namespace Usb_gadget::Adb;

static unsigned u32(unsigned char const *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

int main()
{
	/* command words are the ASCII names, little endian */
	CHECK(CNXN == 0x4e584e43);
	CHECK(OPEN == 0x4e45504f);
	CHECK(OKAY == 0x59414b4f);
	CHECK(CLSE == 0x45534c43);
	CHECK(WRTE == 0x45545257);
	CHECK(AUTH == 0x48545541);

	/* header_roundtrip */
	{
		unsigned char raw[HEADER_SIZE];
		Header const h { CNXN, VERSION, MAX_PAYLOAD, 5, checksum((unsigned char const *)"hello", 5) };
		write(h, raw);
		CHECK(u32(raw + 0)  == CNXN);
		CHECK(u32(raw + 4)  == VERSION);
		CHECK(u32(raw + 8)  == MAX_PAYLOAD);
		CHECK(u32(raw + 12) == 5);
		CHECK(u32(raw + 16) == 'h' + 'e' + 'l' + 'l' + 'o');
		CHECK(u32(raw + 20) == (CNXN ^ 0xffffffffu));

		Header p { };
		CHECK(parse(raw, sizeof(raw), p));
		CHECK(p.command == CNXN && p.arg0 == VERSION && p.arg1 == MAX_PAYLOAD);
		CHECK(p.data_length == 5);
	}

	/* reject_bad_header: wrong size, wrong magic, payload above the maximum */
	{
		unsigned char raw[HEADER_SIZE];
		Header p { };
		write(Header { OPEN, 1, 0, 10, 0 }, raw);
		CHECK(!parse(raw, sizeof(raw) - 1, p));
		raw[20] ^= 1;
		CHECK(!parse(raw, sizeof(raw), p));
		write(Header { WRTE, 1, 2, MAX_PAYLOAD + 1, 0 }, raw);
		CHECK(!parse(raw, sizeof(raw), p));
	}

	/* cnxn_reply: version, our maximum payload (or the host's if smaller), banner */
	{
		Device_info const info { "panther", "Pixel 7", "panther" };
		char banner[256];
		unsigned const n = banner_of(info, banner, sizeof(banner));
		char const *expect = "device::ro.product.name=panther;ro.product.model=Pixel 7;"
		                     "ro.product.device=panther;features=shell_v2";
		CHECK(n == strlen(expect));
		CHECK(memcmp(banner, expect, n) == 0);

		Header const host { CNXN, VERSION, 1024*1024, 0, 0 };
		Header const r = cnxn_reply(host, n, checksum((unsigned char const *)banner, n));
		CHECK(r.command == CNXN && r.arg0 == VERSION && r.arg1 == MAX_PAYLOAD);
		CHECK(r.data_length == n);

		Header const small { CNXN, VERSION, 4096, 0, 0 };
		CHECK(cnxn_reply(small, n, 0).arg1 == 4096);
	}

	/* refuse_open: CLSE with local 0 and the host's id */
	{
		Header const open { OPEN, 7, 0, 6, 0 };
		Header const r = refuse(open);
		CHECK(r.command == CLSE && r.arg0 == 0 && r.arg1 == 7 && r.data_length == 0);
	}

	/* logcat_service: what the host's 'adb logcat' asks for */
	{
		bool dump = true;
		CHECK(logcat_service("shell:export ANDROID_LOG_TAGS=\"\"; exec logcat", dump));
		CHECK(!dump);
		CHECK(logcat_service("shell:export ANDROID_LOG_TAGS=''; exec logcat -d", dump));
		CHECK(dump);
		CHECK(logcat_service("exec:logcat", dump) && !dump);
		CHECK(logcat_service("shell:logcat -d", dump) && dump);
		CHECK(logcat_service("shell:logcat -v threadtime -d", dump) && dump);
		CHECK(!logcat_service("shell:ls", dump));
		CHECK(!logcat_service("sync:", dump));
		CHECK(!logcat_service("shell:echo logcatx", dump));
		CHECK(!logcat_service("shell:logcat -dx", dump) || !dump);

		/* only a command that starts with logcat or holds "exec logcat" */
		CHECK(!logcat_service("shell:echo logcat", dump));
		CHECK(!logcat_service("shell:grep -r logcat /data", dump));
		CHECK(!logcat_service("shell:echo hi; logcat -d", dump));
		CHECK(!logcat_service("shell:myexec logcat", dump));
		CHECK(logcat_service("shell:  logcat -d", dump) && dump);
		CHECK(logcat_service("shell:exec  logcat", dump) && !dump);

		/* host adb quotes every argument (escape_arg) */
		CHECK(logcat_service("shell:export ANDROID_LOG_TAGS=\"\" ; exec logcat '-d'", dump));
		CHECK(dump);
		CHECK(logcat_service("shell:export ANDROID_LOG_TAGS=\"\" ; exec logcat '-v' 'brief' '-d'", dump));
		CHECK(dump);
		CHECK(logcat_service("shell:export ANDROID_LOG_TAGS=\"\" ; exec logcat '-v' 'brief'", dump));
		CHECK(!dump);
	}

	/* stream messages */
	{
		Header const o = okay(5, 9);
		CHECK(o.command == OKAY && o.arg0 == 5 && o.arg1 == 9 && o.data_length == 0);
		Header const w = wrte(5, 9, 100, 1234);
		CHECK(w.command == WRTE && w.arg0 == 5 && w.arg1 == 9 && w.data_length == 100 && w.data_check == 1234);
		Header const c = clse(5, 9);
		CHECK(c.command == CLSE && c.arg0 == 5 && c.arg1 == 9);
	}

	std::printf("test_adb: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
