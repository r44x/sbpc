/* Puts a disc's boot code into a memory image the way the Saturn BIOS does:
 * the Initial Program (IP, system area sectors 0-15) at 0x06002000 and the
 * "1st read file" (first file in the root directory) at the header's 1st read address. */
#ifndef SBPC_ANALYSIS_BOOT_H
#define SBPC_ANALYSIS_BOOT_H

#include "analysis/image.h"
#include "disc/iso9660.h"
#include "disc/saturn_header.h"

#define SATURN_IP_ADDR 0x06002000u

typedef struct {
    saturn_header header;
    char first_file[ISO_NAME_MAX];
    uint32_t first_addr, first_size;
    uint32_t ip_size;
    uint32_t aip_addr;   /* Application Initial Program, after the security code and area codes */
} boot_info;

/* Loads IP + 1st read file into `im`. fs must already be open on the data track. */
bool boot_load(iso_fs *fs, mem_image *im, boot_info *out, char *err, size_t errlen);

#endif
