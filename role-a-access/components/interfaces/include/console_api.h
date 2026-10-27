#pragma once

/* One lock around serial output so a log line is not split by another task.
   Role E replaces the body when the management console takes over the port. */

void console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
