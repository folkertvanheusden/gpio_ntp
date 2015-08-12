#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <signal.h>

void error_exit(const char *const format, ...)
{
	int e = errno;
	char *buffer = NULL;
	va_list ap;

	va_start(ap, format);
	vasprintf(&buffer, format, ap);
	va_end(ap);

	fprintf(stderr, "%s: %s (%d)\n", buffer, strerror(e), e);

	free(buffer);

	exit(EXIT_FAILURE);
}
