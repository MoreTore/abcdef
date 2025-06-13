#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include "../include/linux/ion.h"

/* ---------- ION helper ---------- */
int ion_alloc_contig_fd(size_t len)
{
    int fd_ion = open("/dev/ion", O_RDONLY);
    if (fd_ion < 0) { perror("open /dev/ion"); return -1; }

    struct ion_allocation_data alloc = {
        .len          = len,
        .align        = 4096,
        .heap_id_mask = (1u << ION_HEAP_TYPE_SYSTEM_CONTIG),
        .flags        = 0,
    };
    if (ioctl(fd_ion, ION_IOC_ALLOC, &alloc) == -1) {
        perror("ION_IOC_ALLOC"); close(fd_ion); return -1;
    }

    struct ion_fd_data share = { .handle = alloc.handle };
    if (ioctl(fd_ion, ION_IOC_SHARE, &share) == -1) {
        perror("ION_IOC_SHARE"); close(fd_ion); return -1;
    }

    struct ion_handle_data free_data = { .handle = alloc.handle };
    ioctl(fd_ion, ION_IOC_FREE, &free_data);
    close(fd_ion);
    return share.fd;                    /* this is a DMABUF fd */
}