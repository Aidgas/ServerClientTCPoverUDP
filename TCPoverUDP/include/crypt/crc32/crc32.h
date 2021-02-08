/*
 * crc32.h
 */
#ifndef _CRC32_H
#define _CRC32_H

#include <inttypes.h>
#include <stdlib.h>

unsigned int xcrc32 (const unsigned char *buf, int len, unsigned int init);

#endif /* _CRC32_H */
