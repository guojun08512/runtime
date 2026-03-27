
#include "libipc/platform/detail.h"
#if defined(LIBIPC_OS_WIN)
#include "win/shm_win.cpp"
#elif defined(LIBIPC_OS_LINUX) || defined(LIBIPC_OS_QNX) || defined(LIBIPC_OS_FREEBSD)
#include "posix/shm_posix.cpp"
#else/*IPC_OS*/
#   error "Unsupported platform."
#endif
