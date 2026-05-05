#ifndef NEWOS_STDDEF_H
#define NEWOS_STDDEF_H

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef NEWOS_SIZE_T_DEFINED
#define NEWOS_SIZE_T_DEFINED
typedef unsigned long size_t;
#endif

#ifndef NEWOS_PTRDIFF_T_DEFINED
#define NEWOS_PTRDIFF_T_DEFINED
typedef long ptrdiff_t;
#endif

#ifndef NEWOS_WCHAR_T_DEFINED
#define NEWOS_WCHAR_T_DEFINED
typedef int wchar_t;
#endif

#define offsetof(type, member) ((size_t)&(((type *)0)->member))

#endif