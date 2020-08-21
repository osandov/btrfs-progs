#ifndef _BTRFS_STUBS_H
#define _BTRFS_STUBS_H

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/types.h>

struct iovec;

ssize_t pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset,
		 int flags);

#ifndef O_ALLOW_ENCODED
#if defined(__alpha__)
#define O_ALLOW_ENCODED      0200000000
#elif defined(__hppa__)
#define O_ALLOW_ENCODED      0100000000
#elif defined(__sparc__)
#define O_ALLOW_ENCODED      0x8000000
#else
#define O_ALLOW_ENCODED      040000000
#endif
#endif

#ifndef ENCODED_IOV_SIZE_VER0

#define ENCODED_IOV_COMPRESSION_NONE 0
#define ENCODED_IOV_COMPRESSION_BTRFS_ZLIB 1
#define ENCODED_IOV_COMPRESSION_BTRFS_ZSTD 2
#define ENCODED_IOV_COMPRESSION_BTRFS_LZO_4K 3
#define ENCODED_IOV_COMPRESSION_BTRFS_LZO_8K 4
#define ENCODED_IOV_COMPRESSION_BTRFS_LZO_16K 5
#define ENCODED_IOV_COMPRESSION_BTRFS_LZO_32K 6
#define ENCODED_IOV_COMPRESSION_BTRFS_LZO_64K 7
#define ENCODED_IOV_COMPRESSION_TYPES 8

#define ENCODED_IOV_ENCRYPTION_NONE 0
#define ENCODED_IOV_ENCRYPTION_TYPES 1

struct encoded_iov {
	__aligned_u64 len;
	__aligned_u64 unencoded_len;
	__aligned_u64 unencoded_offset;
	__u32 compression;
	__u32 encryption;
};

#define ENCODED_IOV_SIZE_VER0 32

#endif /* ENCODED_IOV_SIZE_VER0 */

#ifndef RWF_ENCODED
/* encoded (e.g., compressed and/or encrypted) IO */
#define RWF_ENCODED    ((__kernel_rwf_t)0x00000020)
#endif

#endif /* _BTRFS_STUBS_H */
