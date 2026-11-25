/* Minimal freestanding pieces the task-14 test modules need (our own code). */
typedef unsigned long size_t;
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
int __cxa_atexit(void (*f)(void *), void *a, void *d) { (void)f; (void)a; (void)d; return 0; }
void *__dso_handle;
void _ZdlPvm(void *p, size_t n) { (void)p; (void)n; }   /* operator delete(void*, size_t): objects here are never heap allocated */
void _ZdlPv(void *p) { (void)p; }
