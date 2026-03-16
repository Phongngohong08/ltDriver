#ifndef _LTCRYPT_H
#define _LTCRYPT_H

#include <linux/ioctl.h>

#define DES_KEY_SIZE        8
#define DES_BLOCK_SIZE      8
#define LTCRYPT_MAX_DATA    4096

#define LTCRYPT_MAGIC       'L'

struct ltcrypt_data {
    unsigned char data[LTCRYPT_MAX_DATA];
    size_t        len;
};

#define LTCRYPT_IOC_SET_KEY  _IOW(LTCRYPT_MAGIC, 0, unsigned char[DES_KEY_SIZE])
#define LTCRYPT_IOC_ENCRYPT  _IOWR(LTCRYPT_MAGIC, 1, struct ltcrypt_data)
#define LTCRYPT_IOC_DECRYPT  _IOWR(LTCRYPT_MAGIC, 2, struct ltcrypt_data)

#define LTCRYPT_DEVICE       "/dev/ltcrypt"

#endif /* _LTCRYPT_H */
