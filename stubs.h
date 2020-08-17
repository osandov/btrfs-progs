#ifndef _BTRFS_STUBS_H
#define _BTRFS_STUBS_H

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/types.h>

struct iovec;

ssize_t pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset,
		 int flags);

#ifndef O_ALLOW_ENCODED
#define O_ALLOW_ENCODED      040000000
#endif

#ifndef RWF_ENCODED
enum {
	ENCODED_IOV_COMPRESSION_NONE,
#define ENCODED_IOV_COMPRESSION_NONE ENCODED_IOV_COMPRESSION_NONE
	ENCODED_IOV_COMPRESSION_ZLIB,
#define ENCODED_IOV_COMPRESSION_ZLIB ENCODED_IOV_COMPRESSION_ZLIB
	ENCODED_IOV_COMPRESSION_LZO,
#define ENCODED_IOV_COMPRESSION_LZO ENCODED_IOV_COMPRESSION_LZO
	ENCODED_IOV_COMPRESSION_ZSTD,
#define ENCODED_IOV_COMPRESSION_ZSTD ENCODED_IOV_COMPRESSION_ZSTD
	ENCODED_IOV_COMPRESSION_TYPES = ENCODED_IOV_COMPRESSION_ZSTD,
};

enum {
	ENCODED_IOV_ENCRYPTION_NONE,
#define ENCODED_IOV_ENCRYPTION_NONE ENCODED_IOV_ENCRYPTION_NONE
	ENCODED_IOV_ENCRYPTION_TYPES = ENCODED_IOV_ENCRYPTION_NONE,
};

struct encoded_iov {
	__aligned_u64 len;
	__aligned_u64 unencoded_len;
	__aligned_u64 unencoded_offset;
	__u32 compression;
	__u32 encryption;
};

#define ENCODED_IOV_SIZE_VER0 32

/* encoded (e.g., compressed and/or encrypted) IO */
#define RWF_ENCODED    ((__kernel_rwf_t)0x00000020)
#endif /* RWF_ENCODED */

#endif /* _BTRFS_STUBS_H */
