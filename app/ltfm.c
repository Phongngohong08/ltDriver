/*
 * ltfm.c — File Manager using /dev/ltcrypt for DES encryption
 *
 * Menu:
 *   1. List directory
 *   2. Encrypt file
 *   3. Decrypt file
 *   4. View encrypted file (hex dump)
 *   5. Set encryption key
 *   0. Quit
 *
 * Encrypted file format:
 *   [ciphertext padded to multiple of 8] + [4-byte LE original size]
 *   File is saved with ".enc" appended to original name.
 */

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>

/* Use user-space compatible ioctl header */
#include "../include/ltcrypt.h"

/* ------------------------------------------------------------------ */
/* globals                                                              */
/* ------------------------------------------------------------------ */

static int      g_crypt_fd = -1;        /* fd to /dev/ltcrypt */
static int      g_key_set  = 0;
static unsigned char g_key[DES_KEY_SIZE];

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static int open_device(void)
{
    if (g_crypt_fd >= 0)
        return 0;
    g_crypt_fd = open(LTCRYPT_DEVICE, O_RDWR);
    if (g_crypt_fd < 0) {
        perror("open " LTCRYPT_DEVICE);
        fprintf(stderr, "Hint: sudo insmod driver/ltcrypt/ltcrypt.ko\n");
        return -1;
    }
    return 0;
}

static void read_password(const char *prompt, char *buf, size_t size)
{
    struct termios old_term, new_term;
    tcgetattr(STDIN_FILENO, &old_term);
    new_term = old_term;
    new_term.c_lflag &= ~(tcflag_t)ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);

    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, (int)size, stdin) == NULL)
        buf[0] = '\0';
    /* strip newline */
    buf[strcspn(buf, "\n")] = '\0';
    printf("\n");

    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
}

/* ------------------------------------------------------------------ */
/* menu actions                                                         */
/* ------------------------------------------------------------------ */

/*
 * 5. Set encryption key
 */
static void action_set_key(void)
{
    char pass[64];
    unsigned char key[DES_KEY_SIZE];

    if (open_device() != 0)
        return;

    read_password("Enter 8-char key (padded/truncated): ", pass, sizeof(pass));

    memset(key, 0, DES_KEY_SIZE);
    memcpy(key, pass, strnlen(pass, DES_KEY_SIZE));

    if (ioctl(g_crypt_fd, LTCRYPT_IOC_SET_KEY, key) < 0) {
        perror("ioctl SET_KEY");
        return;
    }

    memcpy(g_key, key, DES_KEY_SIZE);
    g_key_set = 1;
    printf("Key set successfully.\n");
}

/*
 * 1. List directory
 */
static void action_list_dir(void)
{
    char path[512];
    DIR *dir;
    struct dirent *ent;
    struct stat st;
    char fullpath[600];
    const char *type;

    printf("Directory path [.]: ");
    if (fgets(path, sizeof(path), stdin) == NULL)
        return;
    path[strcspn(path, "\n")] = '\0';
    if (path[0] == '\0')
        strcpy(path, ".");

    dir = opendir(path);
    if (!dir) {
        perror("opendir");
        return;
    }

    printf("\n%-40s %10s  %s\n", "Name", "Size", "Type");
    printf("%-40s %10s  %s\n",
           "----------------------------------------",
           "----------", "----");

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, ent->d_name);
        if (lstat(fullpath, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode))
            type = "DIR";
        else if (strlen(ent->d_name) > 4 &&
                 strcmp(ent->d_name + strlen(ent->d_name) - 4, ".enc") == 0)
            type = "ENC";
        else
            type = "file";

        printf("%-40s %10lld  %s\n",
               ent->d_name, (long long)st.st_size, type);
    }
    printf("\n");
    closedir(dir);
}

/*
 * 2. Encrypt file
 */
static void action_encrypt_file(void)
{
    char src[512], dst[520];
    FILE *fin, *fout;
    struct ltcrypt_data buf;
    size_t n;
    uint32_t orig_size = 0;
    long file_size;

    if (!g_key_set) {
        printf("Key not set. Please set key first (option 5).\n");
        return;
    }
    if (open_device() != 0)
        return;

    printf("Source file: ");
    if (fgets(src, sizeof(src), stdin) == NULL) return;
    src[strcspn(src, "\n")] = '\0';

    snprintf(dst, sizeof(dst), "%s.enc", src);

    fin = fopen(src, "rb");
    if (!fin) { perror("fopen src"); return; }

    fout = fopen(dst, "wb");
    if (!fout) { perror("fopen dst"); fclose(fin); return; }

    /* get original file size for footer */
    fseek(fin, 0, SEEK_END);
    file_size = ftell(fin);
    rewind(fin);
    orig_size = (uint32_t)file_size;

    printf("Encrypting %s → %s (%ld bytes)...\n", src, dst, file_size);

    while ((n = fread(buf.data, 1, LTCRYPT_MAX_DATA, fin)) > 0) {
        buf.len = n;
        if (ioctl(g_crypt_fd, LTCRYPT_IOC_ENCRYPT, &buf) < 0) {
            perror("ioctl ENCRYPT");
            goto done;
        }
        fwrite(buf.data, 1, buf.len, fout);
    }

    /* write 4-byte LE footer: original size */
    fwrite(&orig_size, sizeof(orig_size), 1, fout);

    printf("Done. Encrypted file: %s\n", dst);

done:
    fclose(fin);
    fclose(fout);
}

/*
 * 3. Decrypt file
 */
static void action_decrypt_file(void)
{
    char src[512], dst[520];
    FILE *fin, *fout;
    struct ltcrypt_data buf;
    size_t n;
    uint32_t orig_size;
    long cipher_size;
    long bytes_remaining;

    if (!g_key_set) {
        printf("Key not set. Please set key first (option 5).\n");
        return;
    }
    if (open_device() != 0)
        return;

    printf("Encrypted file (.enc): ");
    if (fgets(src, sizeof(src), stdin) == NULL) return;
    src[strcspn(src, "\n")] = '\0';

    /* strip .enc to get output name */
    strncpy(dst, src, sizeof(dst) - 1);
    dst[sizeof(dst) - 1] = '\0';
    size_t slen = strlen(dst);
    if (slen > 4 && strcmp(dst + slen - 4, ".enc") == 0)
        dst[slen - 4] = '\0';
    else
        strncat(dst, ".dec", sizeof(dst) - strlen(dst) - 1);

    fin = fopen(src, "rb");
    if (!fin) { perror("fopen src"); return; }

    /* read footer (last 4 bytes) */
    fseek(fin, -4L, SEEK_END);
    if (fread(&orig_size, sizeof(orig_size), 1, fin) != 1) {
        fprintf(stderr, "Cannot read footer\n");
        fclose(fin);
        return;
    }

    fseek(fin, 0, SEEK_END);
    cipher_size = ftell(fin) - 4;  /* exclude footer */
    rewind(fin);

    if (cipher_size <= 0 || cipher_size % DES_BLOCK_SIZE != 0) {
        fprintf(stderr, "Invalid cipher file size %ld\n", cipher_size);
        fclose(fin);
        return;
    }

    fout = fopen(dst, "wb");
    if (!fout) { perror("fopen dst"); fclose(fin); return; }

    printf("Decrypting %s → %s (orig %u bytes)...\n", src, dst, orig_size);

    bytes_remaining = cipher_size;
    while (bytes_remaining > 0) {
        size_t to_read = (bytes_remaining > LTCRYPT_MAX_DATA)
                         ? LTCRYPT_MAX_DATA
                         : (size_t)bytes_remaining;

        n = fread(buf.data, 1, to_read, fin);
        if (n == 0) break;

        buf.len = n;
        if (ioctl(g_crypt_fd, LTCRYPT_IOC_DECRYPT, &buf) < 0) {
            perror("ioctl DECRYPT");
            goto done;
        }

        /* last chunk: trim to original size */
        bytes_remaining -= (long)n;
        if (bytes_remaining == 0) {
            /* this is the last chunk — write only orig_size mod chunk bytes */
            size_t last_plain = orig_size % LTCRYPT_MAX_DATA;
            if (last_plain == 0 && orig_size > 0)
                last_plain = LTCRYPT_MAX_DATA;
            /* clamp to actual decrypted length */
            if (last_plain > buf.len)
                last_plain = buf.len;
            fwrite(buf.data, 1, last_plain, fout);
        } else {
            fwrite(buf.data, 1, buf.len, fout);
        }
    }

    printf("Done. Decrypted file: %s\n", dst);

done:
    fclose(fin);
    fclose(fout);
}

/*
 * 4. View encrypted file (hex dump, first 256 bytes)
 */
static void action_view_enc(void)
{
    char path[512];
    FILE *f;
    unsigned char byte;
    int i = 0;

    printf("Encrypted file: ");
    if (fgets(path, sizeof(path), stdin) == NULL) return;
    path[strcspn(path, "\n")] = '\0';

    f = fopen(path, "rb");
    if (!f) { perror("fopen"); return; }

    printf("\nHex dump of %s (first 256 bytes):\n", path);
    printf("Offset   00 01 02 03 04 05 06 07  08 09 0a 0b 0c 0d 0e 0f\n");
    printf("-------- -----------------------------------------------\n");

    while (i < 256 && fread(&byte, 1, 1, f) == 1) {
        if (i % 16 == 0)
            printf("%08x ", i);
        printf("%02x ", byte);
        if (i % 16 == 7)
            printf(" ");
        if (i % 16 == 15)
            printf("\n");
        i++;
    }
    if (i % 16 != 0)
        printf("\n");

    printf("\n");
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* main menu                                                            */
/* ------------------------------------------------------------------ */

int main(void)
{
    char line[16];
    int choice;

    printf("=== ltfm — File Manager with DES Encryption ===\n");
    printf("Device: " LTCRYPT_DEVICE "\n\n");

    for (;;) {
        printf("Menu:\n");
        printf("  1. List directory\n");
        printf("  2. Encrypt file\n");
        printf("  3. Decrypt file\n");
        printf("  4. View encrypted file (hex)\n");
        printf("  5. Set encryption key\n");
        printf("  0. Quit\n");
        printf("Choice: ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL)
            break;
        choice = atoi(line);

        switch (choice) {
        case 0:
            printf("Bye.\n");
            if (g_crypt_fd >= 0)
                close(g_crypt_fd);
            return 0;
        case 1: action_list_dir();     break;
        case 2: action_encrypt_file(); break;
        case 3: action_decrypt_file(); break;
        case 4: action_view_enc();     break;
        case 5: action_set_key();      break;
        default:
            printf("Unknown option.\n");
        }
        printf("\n");
    }

    if (g_crypt_fd >= 0)
        close(g_crypt_fd);
    return 0;
}
