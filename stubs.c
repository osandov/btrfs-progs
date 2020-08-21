#if HAVE_PWRITEV2 != 1

#include "stubs.h"

#include "kerncompat.h"

#include <unistd.h>
#include <sys/syscall.h>
#include <sys/uio.h>

ssize_t pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset,
		 int flags)
{
/* these conditions indicate an x32 system, which has a different pwritev2 */
#if defined(__x86_64__) && defined(__ILP32__)
	return syscall(SYS_pwritev2, fd, iov, iovcnt, offset, flags);
#else
	unsigned long hi = offset >> (BITS_PER_LONG / 2) >> (BITS_PER_LONG / 2);
	unsigned long lo = offset;

	return syscall(SYS_pwritev2, fd, iov, iovcnt, lo, hi, flags);
#endif // X32
}
#endif /* HAVE_PWRIVEV2 */
