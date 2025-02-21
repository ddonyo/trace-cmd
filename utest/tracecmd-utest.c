// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2020, VMware, Tzvetomir Stoyanov <tz.stoyanov@gmail.com>
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>

#include <trace-cmd.h>

#include "trace-utest.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

static char tracecmd_exec[PATH_MAX];

#define TRACECMD_SUITE		"trace-cmd"
#define TRACECMD_FILE		"__trace_test__.dat"
#define TRACECMD_FILE2		"__trace_test__2.dat"
#define TRACECMD_OUT		"-o", TRACECMD_FILE
#define TRACECMD_OUT2		"-o", TRACECMD_FILE2
#define TRACECMD_IN		"-i", TRACECMD_FILE
#define TRACECMD_IN2		"-i", TRACECMD_FILE2

#define TRACECMD_SQL_HIST	"SELECT irq FROM irq_handler_entry"
#define TRACECMD_SQL_READ_HIST	"show", "--hist", "irq_handler_entry"

#define SYNTH_EVENT		"wakeup"
#define TRACECMD_SQL_SYNTH	"-e", "-n", SYNTH_EVENT, "SELECT start.pid AS this_pid, (end.TIMESTAMP_USECS - start.TIMESTAMP_USECS) AS delta FROM sched_waking as start JOIN sched_switch AS end ON start.pid = end.next_pid"
#define TRACECMD_SQL_START_SYNTH "start", "-e", SYNTH_EVENT

static char **get_args(const char *cmd, va_list ap)
{
	const char *param;
	char **argv;
	char **tmp;

	argv = tracefs_list_add(NULL, tracecmd_exec);
	if (!argv)
		return NULL;

	tmp = tracefs_list_add(argv, cmd);
	if (!tmp)
		goto fail;
	argv = tmp;

	for (param = va_arg(ap, const char *);
	     param; param = va_arg(ap, const char *)) {
		tmp = tracefs_list_add(argv, param);
		if (!tmp)
			goto fail;
		argv = tmp;
	}

	return argv;
 fail:
	tracefs_list_free(argv);
	return NULL;
}

static void silent_output(void)
{
	close(STDOUT_FILENO);
	open("/dev/null", O_WRONLY);
	close(STDERR_FILENO);
	open("/dev/null", O_WRONLY);
}

static int wait_for_exec(int pid)
{
	int status;
	int ret;

	ret = waitpid(pid, &status, 0);
	if (ret != pid)
		return -1;

	return WEXITSTATUS(status) ? -1 : 0;
}

static int run_trace(const char *cmd, ...)
{
	char **argv;
	va_list ap;
	int ret = -1;
	pid_t pid;

	va_start(ap, cmd);
	argv = get_args(cmd, ap);
	va_end(ap);

	if (!argv)
		return -1;

	pid = fork();
	if (pid < 0)
		goto out;

	if (!pid) {
		if (!show_output)
			silent_output();
		ret = execvp(tracecmd_exec, argv);
		exit (ret);
	}

	ret = wait_for_exec(pid);
 out:
	tracefs_list_free(argv);
	return ret;
}

static int pipe_it(int *ofd, int *efd, int (*func)(void *),
		   void *data)
{
	int obrass[2];
	int ebrass[2];
	pid_t pid;
	int ret;

	if (pipe(obrass) < 0)
		return -1;

	if (pipe(ebrass) < 0)
		goto fail_out;

	pid = fork();
	if (pid < 0)
		goto fail;

	if (!pid) {
		char shret[32];

		close(obrass[0]);
		close(STDOUT_FILENO);
		if (dup2(obrass[1], STDOUT_FILENO) < 0)
			exit(-1);

		close(ebrass[0]);
		close(STDERR_FILENO);
		if (dup2(obrass[1], STDERR_FILENO) < 0)
			exit(-1);

		ret = func(data);

		/*
		 * valgrind triggers its reports when the application
		 * exits. If the application does a fork() and the child
		 * exits, it will still trigger the valgrind report for
		 * all the allocations that were not freed by the parent.
		 *
		 * To prevent valgrind from triggering, do an execl() on
		 * a basic shell that will simply exit with the return value.
		 * This will quiet valgrind from reporting memory that has
		 * been allocated by the parent up to here.
		 */
		snprintf(shret, 32, "exit %d", ret);
		execl("/usr/bin/sh", "/usr/bin/sh", "-c", shret, NULL);
		execl("/bin/sh", "/bin/sh", "-c", shret, NULL);

		/* If the above execl() fails, simply do an exit */
		exit(ret);
	}

	close(obrass[1]);
	close(ebrass[1]);

	*ofd = obrass[0];
	*efd = ebrass[0];

	return pid;

 fail:
	close(ebrass[0]);
	close(ebrass[1]);
 fail_out:
	close(obrass[0]);
	close(obrass[1]);
	return -1;
}

struct do_grep {
	const char		*cmd;
	va_list			*ap;
};

static int do_grep(void *data)
{
	struct do_grep *gdata = data;
	char **argv;
	int ret;

	argv = get_args(gdata->cmd, *gdata->ap);
	if (!argv)
		exit(-1);

	ret = execvp(tracecmd_exec, argv);
	tracefs_list_free(argv);
	return ret;
}

struct do_grep_it {
	const char		*match;
	const char		*cmd;
	va_list			*ap;
};

static int do_grep_it(void *data)
{
	struct do_grep_it *dgdata = data;
	struct do_grep gdata;
	FILE *fp;
	regex_t reg;
	char *buf = NULL;
	ssize_t n;
	size_t l = 0;
	int ofd;
	int efd;
	int pid;
	int ret;

	if (regcomp(&reg, dgdata->match, REG_ICASE|REG_NOSUB))
		return -1;

	gdata.cmd = dgdata->cmd;
	gdata.ap = dgdata->ap;
	pid = pipe_it(&ofd, &efd, do_grep, &gdata);

	if (pid < 0) {
		regfree(&reg);
		return -1;
	}

	fp = fdopen(ofd, "r");
	if (!fp)
		goto out;

	do {
		n = getline(&buf, &l, fp);
		if (n > 0 && regexec(&reg, buf, 0, NULL, 0) == 0)
			printf("%s", buf);
	} while (n >= 0);

	free(buf);
 out:
	ret = wait_for_exec(pid);
	if (fp)
		fclose(fp);
	else
		perror("fp");
	close(ofd);
	close(efd);
	regfree(&reg);

	return ret > 0 ? 0 : ret;
}

struct do_grep_match {
	const char		*match;
	const char		*cmd;
	va_list			*ap;
};

static int grep_match(const char *match, const char *cmd, ...)
{
	struct do_grep_it gdata;
	FILE *fp;
	va_list ap;
	char *buf = NULL;
	ssize_t n;
	size_t l = 0;
	bool found = false;
	int ofd;
	int efd;
	int pid;
	int ret;

	va_start(ap, cmd);
	gdata.match = match;
	gdata.cmd = cmd;
	gdata.ap = &ap;
	pid = pipe_it(&ofd, &efd, do_grep_it, &gdata);
	va_end(ap);

	if (pid < 0)
		return -1;

	fp = fdopen(ofd, "r");
	if (!fp)
		goto out;

	do {
		n = getline(&buf, &l, fp);
		if (n > 0) {
			if (show_output)
				printf("%s", buf);
			found = true;
		}
	} while (n >= 0);

	free(buf);
 out:
	ret = wait_for_exec(pid);
	if (ret)
		n = 1;
	if (fp)
		fclose(fp);
	else {
		perror("fp");
		close(ofd);
	}
	close(efd);

	return found ? 0 : 1;
}

static void test_trace_record_report(void)
{
	int ret;

	ret = run_trace("record", TRACECMD_OUT, "-e", "sched", "sleep", "1", NULL);
	CU_TEST(ret == 0);
	ret = run_trace("convert", "--file-version", "6", TRACECMD_IN, TRACECMD_OUT2, NULL);
	CU_TEST(ret == 0);
}

static void test_trace_sqlhist_hist(void)
{
	int ret;

	ret = run_trace("sqlhist", "-e", TRACECMD_SQL_HIST, NULL);
	CU_TEST(ret == 0);
	ret = grep_match(" *Hits: [0-9][0-9]*", TRACECMD_SQL_READ_HIST, NULL);
	CU_TEST(ret == 0);
	ret = run_trace("sqlhist", TRACECMD_SQL_SYNTH, NULL);
	CU_TEST(ret == 0);
	ret = run_trace(TRACECMD_SQL_START_SYNTH, NULL);
	CU_TEST(ret == 0);
	sleep(1);
	ret = grep_match(SYNTH_EVENT ":", "show", NULL);
	CU_TEST(ret == 0);
	/* Ensure synthetic events remain untouched after "trace-cmd reset -k synth". */
	ret = run_trace("reset", "-k", "synth", NULL);
	CU_TEST(ret == 0);
	ret = grep_match(SYNTH_EVENT, "stat", NULL);
	CU_TEST(ret == 0);

	tracefs_instance_reset(NULL);
}

static int read_stats(const char *out, const char *match, const char *cmd, ...)
{
	struct do_grep_it gdata;
	FILE *fp;
	va_list ap;
	bool found = false;
	char *buf = NULL;
	char *p;
	ssize_t n;
	size_t l = 0;
	int ofd;
	int efd;
	int pid;
	int ret;
	int val;

	va_start(ap, cmd);
	gdata.match = match;
	gdata.cmd = cmd;
	gdata.ap = &ap;
	pid = pipe_it(&ofd, &efd, do_grep_it, &gdata);
	va_end(ap);

	if (pid < 0)
		return -1;

	fp = fdopen(ofd, "r");
	if (!fp)
		goto out;

	do {
		n = getline(&buf, &l, fp);
		if (n > 0) {
			for (p = buf; isspace(*p); p++)
				;
			val = atoi(p);
			found = true;
			if (show_output)
				printf("%s", buf);
			CU_TEST(val < 10000000);
		}
	} while (n >= 0);

	free(buf);
 out:
	ret = wait_for_exec(pid);
	if (fp)
		fclose(fp);
	else {
		perror("fp");
	}
	if (!found)
		ret = -1;
	close(ofd);
	close(efd);
	return ret > 0 ? 0 : ret;
}

static void test_trace_record_max(void)
{
	int ret;

	ret = run_trace("record", TRACECMD_OUT, "-p", "function", "-m", "5000",
			"sleep", "10", NULL);
	CU_TEST(ret == 0);

	ret = read_stats(TRACECMD_FILE, ".*bytes in size.*", "report", TRACECMD_IN, "--stat", NULL);
	CU_TEST(ret == 0);
}

static void test_trace_convert6(void)
{
	struct stat st;
	int ret;

	/* If the trace data is already created, just use it, otherwise make it again */
	if (stat(TRACECMD_FILE, &st) < 0) {
		ret = run_trace("record", TRACECMD_OUT, "-e", "sched", "sleep", "1", NULL);
		CU_TEST(ret == 0);
	}
	ret = grep_match("[ \t]6[ \t]*\\[Version\\]", "dump", TRACECMD_IN2, NULL);
	CU_TEST(ret == 0);
}

struct callback_data {
	long			counter;
	struct trace_seq	seq;
};

static int read_events(struct tracecmd_input *handle, struct tep_record *record,
		       int cpu, void *data)
{
	struct tep_handle *tep = tracecmd_get_tep(handle);
	struct callback_data *cd = data;
	struct trace_seq *seq = &cd->seq;

	cd->counter++;

	trace_seq_reset(seq);
	tep_print_event(tep, seq, record, "%6.1000d", TEP_PRINT_TIME);
	trace_seq_printf(seq, " [%03d] ", cpu);
	tep_print_event(tep, seq, record, "%s-%d %s %s\n",
			TEP_PRINT_COMM, TEP_PRINT_PID,
			TEP_PRINT_NAME, TEP_PRINT_INFO);
	if (show_output)
		trace_seq_do_printf(seq);
	return 0;
}

static int read_events_10(struct tracecmd_input *handle, struct tep_record *record,
			  int cpu, void *data)
{
	struct callback_data *cd = data;

	read_events(handle, record, cpu, data);
	return  cd->counter < 10 ? 0 : 1;
}

static void test_trace_library_read(void)
{
	struct tracecmd_input *handle;
	struct callback_data data;
	struct stat st;
	int ret;

	data.counter = 0;
	trace_seq_init(&data.seq);

	/* If the trace data is already created, just use it, otherwise make it again */
	if (stat(TRACECMD_FILE, &st) < 0) {
		ret = run_trace("record", TRACECMD_OUT, "-e", "sched", "sleep", "1", NULL);
		CU_TEST(ret == 0);
	}

	handle = tracecmd_open(TRACECMD_FILE, 0);
	CU_TEST(handle != NULL);
	ret = tracecmd_iterate_events(handle, NULL, 0, read_events, &data);
	CU_TEST(ret == 0);

	tracecmd_close(handle);

	CU_TEST(data.counter > 0);
	trace_seq_destroy(&data.seq);
}

static void test_trace_library_read_inc(void)
{
	struct tracecmd_input *handle;
	struct callback_data data;
	struct stat st;
	long save_count;
	long total = 0;
	int ret;

	data.counter = 0;
	trace_seq_init(&data.seq);

	/* If the trace data is already created, just use it, otherwise make it again */
	if (stat(TRACECMD_FILE, &st) < 0) {
		ret = run_trace("record", TRACECMD_OUT, "-e", "sched", "sleep", "1", NULL);
		CU_TEST(ret == 0);
	}

	/* First read all again */
	handle = tracecmd_open(TRACECMD_FILE, 0);
	CU_TEST(handle != NULL);
	ret = tracecmd_iterate_events(handle, NULL, 0, read_events, &data);
	CU_TEST(ret == 0);
	CU_TEST(data.counter > 0);

	/* Save the counter */
	save_count = data.counter;

	tracecmd_iterate_reset(handle);

	/* Read 10 at a time */
	do {
		data.counter = 0;
		ret = tracecmd_iterate_events(handle, NULL, 0, read_events_10, &data);
		CU_TEST(ret >= 0);
		CU_TEST(data.counter <= 10);
		total += data.counter;
	} while (data.counter);
	CU_TEST(ret == 0);

	CU_TEST(total == save_count);

	trace_seq_destroy(&data.seq);
	tracecmd_close(handle);
}

static void test_trace_library_read_back(void)
{
	struct tracecmd_input *handle;
	struct callback_data data;
	struct stat st;
	long save_count;
	int ret;

	data.counter = 0;
	trace_seq_init(&data.seq);

	/* If the trace data is already created, just use it, otherwise make it again */
	if (stat(TRACECMD_FILE, &st) < 0) {
		ret = run_trace("record", TRACECMD_OUT, "-e", "sched", "sleep", "1", NULL);
		CU_TEST(ret == 0);
	}

	/* First read all again */
	handle = tracecmd_open(TRACECMD_FILE, 0);
	CU_TEST(handle != NULL);
	ret = tracecmd_iterate_events(handle, NULL, 0, read_events, &data);
	CU_TEST(ret == 0);
	CU_TEST(data.counter > 0);

	/* Save the counter */
	save_count = data.counter;

	tracecmd_iterate_reset(handle);

	/* Read backwards */
	data.counter = 0;
	ret = tracecmd_iterate_events_reverse(handle, NULL, 0, read_events, &data, false);
	CU_TEST(ret == 0);
	CU_TEST(data.counter == save_count);

	/* Read forward again */
	data.counter = 0;
	ret = tracecmd_iterate_events(handle, NULL, 0, read_events, &data);
	CU_TEST(ret == 0);
	CU_TEST(data.counter == save_count);

	/* Read backwards from where we left off */
	data.counter = 0;
	ret = tracecmd_iterate_events_reverse(handle, NULL, 0, read_events, &data, true);
	CU_TEST(ret == 0);
	CU_TEST(data.counter == save_count);

	trace_seq_destroy(&data.seq);
	tracecmd_close(handle);
}

static void test_trace_reset_kprobe(void)
{
	int ret;

	/* Create a simple kprobe for do_sys_open */
	ret = tracefs_instance_file_write(NULL, "kprobe_events", "p do_sys_open");
	CU_TEST(ret > 0);

	/* Ensure the kprobe is listed in "trace-cmd stat" output. */
	ret = grep_match("p:kprobes/p_do_sys_open_0 do_sys_open", "stat", NULL);
	CU_TEST(ret == 0);

	/* Issue "trace-cmd reset", but keep kprobes. */
	ret = run_trace("reset", "-k", "kprobe", NULL);
	CU_TEST(ret == 0);

	/* Verify the kprobe's existence after reset. */
	ret = grep_match("p:kprobes/p_do_sys_open_0 do_sys_open", "stat", NULL);
	CU_TEST(ret == 0);
}

static void test_trace_reset_kretprobe(void)
{
	int ret;

	/* Create a simple kretprobe for do_sys_open */
	ret = tracefs_instance_file_write(NULL, "kprobe_events", "r do_sys_open");
	CU_TEST(ret > 0);

	/* Ensure the kretprobe is listed in "trace-cmd stat" output. */
	ret = grep_match("r[0-9]*:kprobes/r_do_sys_open_0 do_sys_open", "stat", NULL);
	CU_TEST(ret == 0);

	/* Issue "trace-cmd reset", but keep kretprobes. */
	ret = run_trace("reset", "-k", "kretprobe", NULL);
	CU_TEST(ret == 0);

	/* Verify the kretprobe's existence after reset. */
	ret = grep_match("r[0-9]*:kprobes/r_do_sys_open_0 do_sys_open", "stat", NULL);
	CU_TEST(ret == 0);
}

static void test_trace_reset_uprobe(void)
{
	int ret;

	/* Create a simple uprobe for do_sys_open */
	ret = tracefs_instance_file_write(NULL, "uprobe_events", "p /bin/bash:0x4245c0");
	CU_TEST(ret > 0);

	/* Ensure the uprobe is listed in "trace-cmd stat" output. */
	ret = grep_match("p:uprobes/p_bash_0x4245c0 /bin/bash:0x00000000004245c0", "stat", NULL);
	CU_TEST(ret == 0);

	/* Issue "trace-cmd reset", but keep uprobes. */
	ret = run_trace("reset", "-k", "uprobe", NULL);
	CU_TEST(ret == 0);

	/* Verify the uprobe's existence after reset. */
	ret = grep_match("p:uprobes/p_bash_0x4245c0 /bin/bash:0x00000000004245c0", "stat", NULL);
	CU_TEST(ret == 0);
}

static void test_trace_reset_uretprobe(void)
{
	int ret;

	/* Create a simple uretprobe for do_sys_open */
	ret = tracefs_instance_file_write(NULL, "uprobe_events", "r /bin/bash:0x4245c0");
	CU_TEST(ret > 0);

	/* Ensure the uretprobe is listed in "trace-cmd stat" output. */
	ret = grep_match("r:uprobes/p_bash_0x4245c0 /bin/bash:0x00000000004245c0", "stat", NULL);
	CU_TEST(ret == 0);

	/* Issue "trace-cmd reset", but keep uretprobes. */
	ret = run_trace("reset", "-k", "uretprobe", NULL);
	CU_TEST(ret == 0);

	/* Verify the uretprobe's existence after reset. */
	ret = grep_match("r:uprobes/p_bash_0x4245c0 /bin/bash:0x00000000004245c0", "stat", NULL);
	CU_TEST(ret == 0);
}

static void test_trace_reset_eprobe(void)
{
	int fd;
	bool matched = false;
	size_t l = 0;
	ssize_t n;
	char *buf = NULL;
	struct tracefs_dynevent *deprobe;
	FILE *fp;

	deprobe = tracefs_eprobe_alloc(NULL, "sopen_in", "syscalls", "sys_enter_openat", NULL);
	CU_TEST(deprobe != NULL);

	CU_TEST(tracefs_dynevent_create(deprobe) == 0);

	/* Issue "trace-cmd reset", but keep eprobes. */
	CU_TEST(run_trace("reset", "-k", "eprobe", NULL) == 0);

	/* Verify the eprobe's existence after reset. */
	fd = tracefs_instance_file_open(NULL, "dynamic_events", O_RDONLY);
	CU_TEST(fd != -1);
	fp = fdopen(fd, "r");
	CU_TEST(fp != NULL);

	while ((n = getline(&buf, &l, fp)) != -1) {
		if (!strcmp(buf, "e:eprobes/sopen_in syscalls.sys_enter_openat\n")) {
			matched = true;
			break;
		}
	}
	free(buf);

	fclose(fp);

	CU_TEST(matched == true);

	CU_TEST(tracefs_dynevent_destroy(deprobe, false) == 0);

	tracefs_dynevent_free(deprobe);
}

static void test_trace_reset(void)
{
	char *str;

	test_trace_reset_kprobe();
	test_trace_reset_kretprobe();
	test_trace_reset_uprobe();
	test_trace_reset_uretprobe();
	test_trace_reset_eprobe();

	/* Destroy all dynamic events. */
	CU_TEST(run_trace("reset", NULL) == 0);

	/* Paranoia check since "trace-cmd reset" may tell porkies. */
	str = tracefs_instance_file_read(NULL, "dynamic_events", NULL);
	CU_TEST(str == NULL);
	if (str)
		free(str);
}

static int test_suite_destroy(void)
{
	unlink(TRACECMD_FILE);
	unlink(TRACECMD_FILE2);
	return 0;
}

static int test_suite_init(void)
{
	struct stat st;
	const char *p;

	/* The test must be in the utest directory */
	for (p = argv0 + strlen(argv0) - 1; p > argv0 && *p != '/'; p--)
		;

	if (*p == '/')
		snprintf(tracecmd_exec, PATH_MAX, "%.*s/../tracecmd/trace-cmd",
			 (int)(p - argv0), argv0);
	else
		strncpy(tracecmd_exec, "../tracecmd/trace-cmd", PATH_MAX);

	if (stat(tracecmd_exec, &st) < 0) {
		fprintf(stderr, "In tree trace-cmd executable not found\n");
		return 1;
	}

	if (!(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
		fprintf(stderr, "In tree trace-cmd executable not executable\n");
		return 1;
	}

	return 0;
}

void test_tracecmd_lib(void)
{
	CU_pSuite suite = NULL;

	suite = CU_add_suite(TRACECMD_SUITE, test_suite_init, test_suite_destroy);
	if (suite == NULL) {
		fprintf(stderr, "Suite \"%s\" cannot be created\n", TRACECMD_SUITE);
		return;
	}
	CU_add_test(suite, "Simple record and report",
		    test_trace_record_report);
	CU_add_test(suite, "Create a histogram",
		    test_trace_sqlhist_hist);
	CU_add_test(suite, "Test convert from v7 to v6",
		    test_trace_convert6);
	CU_add_test(suite, "Use libraries to read file",
		    test_trace_library_read);
	CU_add_test(suite, "Use libraries to read file incremental",
		    test_trace_library_read_inc);
	CU_add_test(suite, "Use libraries to read file backwards",
		    test_trace_library_read_back);
	CU_add_test(suite, "Test max length",
		    test_trace_record_max);
	CU_add_test(suite, "Simple reset", test_trace_reset);
}
