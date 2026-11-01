/* Host test: Adb::parse_service (OPEN service string) */
#include <adb/service.h>
#include <cstdio>
#include <cstring>

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

using ::Adb::parse_service;
using ::Adb::Service;

static Service p(char const *s) { return parse_service(s, std::strlen(s)); }
static bool cmd_is(Service const &s, char const *c) {
	return s.command_len == std::strlen(c) && !std::memcmp(s.command, c, s.command_len); }

int main()
{
	/* service_shell_interactive */
	{
		Service s = p("shell:");
		CHECK(s.kind == Service::SHELL && !s.v2 && s.pty && s.command_len == 0);
	}
	/* service_shell_cmd_v1 */
	{
		Service s = p("shell:ls -l");
		CHECK(s.kind == Service::SHELL && !s.v2 && !s.pty && cmd_is(s, "ls -l"));
	}
	/* service_shell_v2_raw */
	{
		Service s = p("shell,v2,TERM=xterm-256color,raw:echo hi");
		CHECK(s.kind == Service::SHELL && s.v2 && !s.pty && cmd_is(s, "echo hi"));
	}
	/* service_shell_v2_pty */
	{
		Service s = p("shell,v2,pty:");
		CHECK(s.kind == Service::SHELL && s.v2 && s.pty && s.command_len == 0);
	}
	/* service_v2_logcat */
	{
		Service s = p("shell,v2,raw:export ANDROID_LOG_TAGS=''; exec logcat -d");
		CHECK(s.kind == Service::LOGCAT && s.v2);
	}
	/* service_exec */
	{
		Service s = p("exec:cat /x");
		CHECK(s.kind == Service::SHELL && !s.v2 && !s.pty && cmd_is(s, "cat /x"));
	}
	/* service_sync_reboot_other */
	{
		CHECK(p("sync:").kind == Service::SYNC);
		Service r = p("reboot:bootloader");
		CHECK(r.kind == Service::REBOOT && cmd_is(r, "bootloader"));
		CHECK(p("tcpip:5555").kind == Service::OTHER);
		CHECK(p("").kind == Service::OTHER);
	}
	/* v1 logcat */
	{
		CHECK(p("shell:logcat -d").kind == Service::LOGCAT);
	}
	/* the host's 'adb logcat', v1 and v2 */
	{
		CHECK(p("shell:export ANDROID_LOG_TAGS=\"\" ; exec logcat '-d'").kind == Service::LOGCAT);
		CHECK(p("shell,v2,raw:export ANDROID_LOG_TAGS=''; exec logcat").kind == Service::LOGCAT);
		CHECK(p("shell,v2,raw:export ANDROID_LOG_TAGS='' ; exec logcat '-v' 'brief'").kind == Service::LOGCAT);
	}
	/* echo_logcat: a command that mentions logcat runs in the shell */
	{
		Service s = p("shell,v2,raw:echo logcat");
		CHECK(s.kind == Service::SHELL && s.v2 && cmd_is(s, "echo logcat"));
		CHECK(p("shell:echo logcat").kind == Service::SHELL);
		CHECK(p("shell:grep -r logcat /data").kind == Service::SHELL);
	}

	/* long commands: no overflow, decided by the words before the cut */
	{
		char buf[600];
		std::memset(buf, 'a', sizeof(buf));
		std::memcpy(buf, "shell:", 6);
		buf[sizeof(buf) - 1] = 0;
		CHECK(parse_service(buf, std::strlen(buf)).kind == Service::SHELL);

		std::strcpy(buf, "shell:logcat -d ");
		std::memset(buf + 16, 'b', 400); buf[416] = 0;
		CHECK(parse_service(buf, std::strlen(buf)).kind == Service::LOGCAT);

		/* "logcat" straddling the cut is no word of its own */
		std::memset(buf, ' ', 6 + 245);
		std::memcpy(buf, "shell:", 6);
		std::memcpy(buf + 6 + 245, "logcatx", 7);
		std::memset(buf + 6 + 252, ' ', 100); buf[400] = 0;
		CHECK(parse_service(buf, std::strlen(buf)).kind == Service::SHELL);
	}

	std::printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
	return failures ? 1 : 0;
}
