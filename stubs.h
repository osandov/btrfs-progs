#ifndef _BTRFS_STUBS_H
#define _BTRFS_STUBS_H

#include <sys/types.h>

struct iovec;

ssize_t pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset,
		 int flags);

#endif
