/* The Saturn "system ID" at the start of sector 0 of the data track. */
#ifndef SBPC_DISC_SATURN_HEADER_H
#define SBPC_DISC_SATURN_HEADER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char maker[17];
    char product[11];
    char version[7];
    char date[9];        /* YYYYMMDD */
    char device[9];      /* e.g. "CD-1/1" */
    char areas[11];      /* J, T, U, B, K, A, E, L */
    char peripherals[17];
    char title[113];
    uint32_t ip_size;
    uint32_t master_stack;
    uint32_t slave_stack;
    uint32_t first_read_addr;
    uint32_t first_read_size;
} saturn_header;

/* Parses the 256-byte system ID; false if it doesn't start with "SEGA SEGASATURN ". */
bool saturn_header_parse(const uint8_t sector[2048], saturn_header *out);

#endif
