/*
 * ltcrypt.c — DES Crypto Char Device Driver
 *
 * Provides /dev/ltcrypt with ioctl interface for DES encryption/decryption.
 * Also supports write() → encrypt → read() workflow.
 *
 * Usage:
 *   sudo insmod ltcrypt.ko
 *   ls /dev/ltcrypt    (or: make mknod from top-level)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <crypto/skcipher.h>
#include <crypto/des.h>

#include "../../include/ltcrypt.h"

#define DRIVER_NAME     "ltcrypt"
#define DEVICE_NAME     "ltcrypt"
#define CLASS_NAME      "ltcrypt"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ltDriver");
MODULE_DESCRIPTION("DES Crypto Character Device Driver");
MODULE_VERSION("1.0");

struct ltcrypt_dev {
    struct cdev          cdev;
    struct class        *cls;
    dev_t                devno;
    struct crypto_skcipher *tfm;
    struct skcipher_request *req;
    unsigned char        key[DES_KEY_SIZE];
    int                  key_set;
    struct mutex         lock;
    /* write→encrypt→read buffer */
    unsigned char        wr_buf[LTCRYPT_MAX_DATA];
    unsigned char        rd_buf[LTCRYPT_MAX_DATA];
    size_t               rd_len;
    size_t               rd_pos;
};

static struct ltcrypt_dev *ltdev;

/* ------------------------------------------------------------------ */
/* file operations                                                      */
/* ------------------------------------------------------------------ */

static int ltcrypt_open(struct inode *inode, struct file *filp)
{
    filp->private_data = ltdev;
    return 0;
}

static int ltcrypt_release(struct inode *inode, struct file *filp)
{
    return 0;
}

/*
 * write() — receive plaintext, encrypt it, store in rd_buf for read()
 */
static ssize_t ltcrypt_write(struct file *filp, const char __user *buf,
                              size_t count, loff_t *ppos)
{
    struct ltcrypt_dev *dev = filp->private_data;
    struct scatterlist sg_in, sg_out;
    size_t padded, i;
    int ret;

    if (count == 0 || count > LTCRYPT_MAX_DATA)
        return -EINVAL;

    mutex_lock(&dev->lock);

    if (!dev->key_set) {
        mutex_unlock(&dev->lock);
        return -ENOKEY;
    }

    if (copy_from_user(dev->wr_buf, buf, count)) {
        mutex_unlock(&dev->lock);
        return -EFAULT;
    }

    /* zero-pad to multiple of DES_BLOCK_SIZE */
    padded = (count + DES_BLOCK_SIZE - 1) & ~(DES_BLOCK_SIZE - 1);
    memset(dev->wr_buf + count, 0, padded - count);

    /* encrypt buffer in-place block-wise using skcipher */
    for (i = 0; i < padded; i += DES_BLOCK_SIZE) {
        sg_init_one(&sg_in, dev->wr_buf + i, DES_BLOCK_SIZE);
        sg_init_one(&sg_out, dev->rd_buf + i, DES_BLOCK_SIZE);
        skcipher_request_set_crypt(dev->req, &sg_in, &sg_out,
                                   DES_BLOCK_SIZE, NULL);
        ret = crypto_skcipher_encrypt(dev->req);
        if (ret) {
            mutex_unlock(&dev->lock);
            return ret;
        }
    }

    dev->rd_len = padded;
    dev->rd_pos = 0;

    mutex_unlock(&dev->lock);
    return (ssize_t)count;
}

/*
 * read() — return encrypted data stored by write()
 */
static ssize_t ltcrypt_read(struct file *filp, char __user *buf,
                             size_t count, loff_t *ppos)
{
    struct ltcrypt_dev *dev = filp->private_data;
    size_t avail, to_copy;

    mutex_lock(&dev->lock);

    avail = dev->rd_len - dev->rd_pos;
    if (avail == 0) {
        mutex_unlock(&dev->lock);
        return 0;
    }

    to_copy = min(count, avail);
    if (copy_to_user(buf, dev->rd_buf + dev->rd_pos, to_copy)) {
        mutex_unlock(&dev->lock);
        return -EFAULT;
    }

    dev->rd_pos += to_copy;

    mutex_unlock(&dev->lock);
    return (ssize_t)to_copy;
}

/*
 * ioctl() — SET_KEY / ENCRYPT / DECRYPT
 */
static long ltcrypt_ioctl(struct file *filp, unsigned int cmd,
                           unsigned long arg)
{
    struct ltcrypt_dev *dev = filp->private_data;
    struct ltcrypt_data kdata;
    unsigned char key[DES_KEY_SIZE];
    size_t padded, i;
    int ret = 0;

    mutex_lock(&dev->lock);

    switch (cmd) {

    case LTCRYPT_IOC_SET_KEY:
        if (copy_from_user(key, (void __user *)arg, DES_KEY_SIZE)) {
            ret = -EFAULT;
            break;
        }
        ret = crypto_skcipher_setkey(dev->tfm, key, DES_KEY_SIZE);
        if (ret == 0) {
            memcpy(dev->key, key, DES_KEY_SIZE);
            dev->key_set = 1;
            pr_info("ltcrypt: key set\n");
        }
        break;

    case LTCRYPT_IOC_ENCRYPT:
        if (!dev->key_set) { ret = -ENOKEY; break; }
        if (copy_from_user(&kdata, (void __user *)arg, sizeof(kdata))) {
            ret = -EFAULT; break;
        }
        if (kdata.len == 0 || kdata.len > LTCRYPT_MAX_DATA) {
            ret = -EINVAL; break;
        }
        padded = (kdata.len + DES_BLOCK_SIZE - 1) & ~(DES_BLOCK_SIZE - 1);
        memset(kdata.data + kdata.len, 0, padded - kdata.len);
        for (i = 0; i < padded; i += DES_BLOCK_SIZE) {
            struct scatterlist sg;
            sg_init_one(&sg, kdata.data + i, DES_BLOCK_SIZE);
            skcipher_request_set_crypt(dev->req, &sg, &sg,
                                       DES_BLOCK_SIZE, NULL);
            ret = crypto_skcipher_encrypt(dev->req);
            if (ret) {
                mutex_unlock(&dev->lock);
                return ret;
            }
        }
        kdata.len = padded;
        if (copy_to_user((void __user *)arg, &kdata, sizeof(kdata)))
            ret = -EFAULT;
        break;

    case LTCRYPT_IOC_DECRYPT:
        if (!dev->key_set) { ret = -ENOKEY; break; }
        if (copy_from_user(&kdata, (void __user *)arg, sizeof(kdata))) {
            ret = -EFAULT; break;
        }
        if (kdata.len == 0 || kdata.len > LTCRYPT_MAX_DATA ||
            kdata.len % DES_BLOCK_SIZE != 0) {
            ret = -EINVAL; break;
        }
        for (i = 0; i < kdata.len; i += DES_BLOCK_SIZE) {
            struct scatterlist sg;
            sg_init_one(&sg, kdata.data + i, DES_BLOCK_SIZE);
            skcipher_request_set_crypt(dev->req, &sg, &sg,
                                       DES_BLOCK_SIZE, NULL);
            ret = crypto_skcipher_decrypt(dev->req);
            if (ret) {
                mutex_unlock(&dev->lock);
                return ret;
            }
        }
        if (copy_to_user((void __user *)arg, &kdata, sizeof(kdata)))
            ret = -EFAULT;
        break;

    default:
        ret = -ENOTTY;
    }

    mutex_unlock(&dev->lock);
    return ret;
}

static const struct file_operations ltcrypt_fops = {
    .owner          = THIS_MODULE,
    .open           = ltcrypt_open,
    .release        = ltcrypt_release,
    .read           = ltcrypt_read,
    .write          = ltcrypt_write,
    .unlocked_ioctl = ltcrypt_ioctl,
};

/* ------------------------------------------------------------------ */
/* module init / exit                                                   */
/* ------------------------------------------------------------------ */

static int __init ltcrypt_init(void)
{
    int ret;

    ltdev = kzalloc(sizeof(*ltdev), GFP_KERNEL);
    if (!ltdev)
        return -ENOMEM;

    mutex_init(&ltdev->lock);

    /* allocate DES ECB skcipher */
    ltdev->tfm = crypto_alloc_skcipher("ecb(des)", 0, 0);
    if (IS_ERR(ltdev->tfm)) {
        ret = PTR_ERR(ltdev->tfm);
        pr_err("ltcrypt: failed to allocate DES skcipher: %d\n", ret);
        goto err_free;
    }

    ltdev->req = skcipher_request_alloc(ltdev->tfm, GFP_KERNEL);
    if (!ltdev->req) {
        pr_err("ltcrypt: failed to alloc skcipher request\n");
        ret = -ENOMEM;
        goto err_cipher;
    }

    /* register char device */
    ret = alloc_chrdev_region(&ltdev->devno, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("ltcrypt: alloc_chrdev_region failed: %d\n", ret);
        goto err_cipher;
    }

    cdev_init(&ltdev->cdev, &ltcrypt_fops);
    ltdev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&ltdev->cdev, ltdev->devno, 1);
    if (ret < 0) {
        pr_err("ltcrypt: cdev_add failed: %d\n", ret);
        goto err_region;
    }

    /* create /dev/ltcrypt automatically */
    ltdev->cls = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(ltdev->cls)) {
        ret = PTR_ERR(ltdev->cls);
        pr_err("ltcrypt: class_create failed: %d\n", ret);
        goto err_cdev;
    }

    if (IS_ERR(device_create(ltdev->cls, NULL, ltdev->devno,
                              NULL, DEVICE_NAME))) {
        ret = -ENOMEM;
        pr_err("ltcrypt: device_create failed\n");
        goto err_class;
    }

    pr_info("ltcrypt: loaded, major=%d\n", MAJOR(ltdev->devno));
    return 0;

err_class:
    class_destroy(ltdev->cls);
err_cdev:
    cdev_del(&ltdev->cdev);
err_region:
    unregister_chrdev_region(ltdev->devno, 1);
err_cipher:
    if (ltdev->req)
        skcipher_request_free(ltdev->req);
    if (ltdev->tfm)
        crypto_free_skcipher(ltdev->tfm);
err_free:
    kfree(ltdev);
    return ret;
}

static void __exit ltcrypt_exit(void)
{
    device_destroy(ltdev->cls, ltdev->devno);
    class_destroy(ltdev->cls);
    cdev_del(&ltdev->cdev);
    unregister_chrdev_region(ltdev->devno, 1);
    if (ltdev->req)
        skcipher_request_free(ltdev->req);
    if (ltdev->tfm)
        crypto_free_skcipher(ltdev->tfm);
    kfree(ltdev);
    pr_info("ltcrypt: unloaded\n");
}

module_init(ltcrypt_init);
module_exit(ltcrypt_exit);
