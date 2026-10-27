/* Diagnostics for sti.dll. Lines go to the file named by $SFL_STI_LOG if set,
 * and to OutputDebugString (visible with WINEDEBUG=+debugstr) otherwise. */
#ifndef SFL_LOG_H
#define SFL_LOG_H

void sfl_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
