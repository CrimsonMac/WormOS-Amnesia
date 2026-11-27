/* Small helpers for wh. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/openat2.h>

#include "wh.h"

void die(const char *fmt, ...)
{
	va_list ap;

	fputs("wh: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

size_t bufadd(char *buf, size_t size, size_t n, const char *fmt, ...)
{
	va_list ap;
	int r;

	if (!size || n >= size - 1)
		return n;
	va_start(ap, fmt);
	r = vsnprintf(buf + n, size - n, fmt, ap);
	va_end(ap);
	if (r < 0)
		return n;
	return (size_t)r >= size - n ? size - 1 : n + (size_t)r;
}

void warn(const char *fmt, ...)
{
	va_list ap;

	fputs("wh: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void info(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

void root_path(char *out, size_t len, const char *rel)
{
	const char *sep = root[strlen(root) - 1] == '/' ? "" : "/";

	if ((size_t)snprintf(out, len, "%s%s%s", root, sep, rel) >= len)
		die("path too long: %s", rel);
}

/* Open rel inside the root, resolving symlinks as if the root were "/". */
int open_in_root(const char *rel, int flags)
{
	struct open_how how = {
		.flags = flags | O_CLOEXEC,
		.resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS,
	};

	while (*rel == '/')
		rel++;
	if (!*rel)
		rel = ".";
	return syscall(SYS_openat2, root_fd, rel, &how, sizeof(how));
}

/* Create rel and its parents inside the root (like mkdir -p). */
int mkdirs_at(int dfd, const char *rel, int mode)
{
	char buf[PATH_MAX], *p;

	snprintf(buf, sizeof(buf), "%s", rel);
	for (p = buf; (p = strchr(p + 1, '/')); ) {
		*p = '\0';
		if (mkdirat(dfd, buf, mode) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdirat(dfd, buf, mode) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

/*
 * Run a program and wait for it. With out, its standard output is captured
 * (up to outlen - 1 bytes; the rest is read and dropped). Returns the exit
 * status, or -1.
 */
int run(char *const argv[], char *out, size_t outlen)
{
	int pipefd[2] = { -1, -1 }, status;
	size_t n = 0;
	pid_t pid;

	if (out && pipe2(pipefd, O_CLOEXEC) < 0)
		return -1;
	fflush(stdout);
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		if (out)
			dup2(pipefd[1], STDOUT_FILENO);
		execvp(argv[0], argv);
		fprintf(stderr, "wh: cannot run %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	if (out) {
		char rest[4096];
		ssize_t r;

		close(pipefd[1]);
		while (n < outlen - 1 && (r = read(pipefd[0], out + n, outlen - 1 - n)) > 0)
			n += r;
		out[n] = '\0';
		/* Read to the end: closing the pipe early would kill the program
		 * with SIGPIPE and make it look as if it had failed. */
		while ((r = read(pipefd[0], rest, sizeof(rest))) > 0 || (r < 0 && errno == EINTR))
			;
		close(pipefd[0]);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

char *trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
			   end[-1] == '\n' || end[-1] == '\r'))
		*--end = '\0';
	return s;
}

/* Is word one of the space-separated words in list? */
int word_in(const char *list, const char *word)
{
	size_t n = strlen(word);

	for (const char *p = list; (p = strstr(p, word)); p += n)
		if ((p == list || p[-1] == ' ') && (p[n] == '\0' || p[n] == ' '))
			return 1;
	return 0;
}
