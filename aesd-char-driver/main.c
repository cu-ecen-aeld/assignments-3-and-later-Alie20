/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"
#include <linux/errno.h>
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Alie-Eldeen.Omar"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;
ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
int aesd_init_module(void);
void aesd_cleanup_module(void);
int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;

    PDEBUG("open");
    /**
     * TODO: handle open
     */
	dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
     filp->private_data = NULL;

    return 0;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                   loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = -ENOMEM;
    char *tmp = NULL;

    PDEBUG("aesd_write: writing %zu bytes", count);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* Allocate or expand write_buffer */
    tmp = krealloc(dev->write_buffer, dev->size + count, GFP_KERNEL);
    if (!tmp) {
        retval = -ENOMEM;
        goto out_unlock;
    }
    dev->write_buffer = tmp;

    /* Copy data from user */
    if (copy_from_user(dev->write_buffer + dev->size, buf, count)) {
        retval = -EFAULT;
        goto out_unlock;
    }
    dev->size += count;
    retval = count;

    /* Check for newline to finalize entry */
    if (memchr(dev->write_buffer, '\n', dev->size)) {
        struct aesd_buffer_entry new_entry;

        new_entry.buffptr = dev->write_buffer;
        new_entry.size = dev->size;

        /* Free overwritten entry if buffer is full */
        if (dev->buffer.full) {
            struct aesd_buffer_entry *replaced = &dev->buffer.entry[dev->buffer.in_offs];
            if (replaced->buffptr)
                kfree(replaced->buffptr);
        }

        /* Add new entry to circular buffer */
        aesd_circular_buffer_add_entry(&dev->buffer, &new_entry);

        /* Reset write_buffer for next command */
        dev->write_buffer = NULL;
        dev->size = 0;
    }

out_unlock:
    mutex_unlock(&dev->lock);
    return retval;
}
ssize_t aesd_read(struct file *filp, char __user *buf,
                  size_t count, loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = 0;
    size_t copied = 0;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    size_t entry_offset;
    struct aesd_buffer_entry *entry;
    uint8_t index;

    // find the first entry and offset for the current f_pos
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(
                &dev->buffer, *f_pos, &entry_offset);
    if (!entry)
        goto out; // nothing to read

    // determine index of that entry
    index = aesd_circular_buffer_find_entry_index_for_fpos(
                &dev->buffer, *f_pos);
    
    while (count > 0 && entry && entry->buffptr) {
        size_t bytes_available = entry->size - entry_offset;
        size_t bytes_to_copy = (bytes_available > count) ? count : bytes_available;

        if (copy_to_user(buf + copied,
                         entry->buffptr + entry_offset,
                         bytes_to_copy))
        {
            retval = -EFAULT;
            goto out;
        }

        copied       += bytes_to_copy;
        *f_pos       += bytes_to_copy;
        count        -= bytes_to_copy;

        // move to next entry in the circular buffer
        index = (index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        entry = &dev->buffer.entry[index];
        entry_offset = 0;
    }

    retval = copied;

out:
    mutex_unlock(&dev->lock);
    return retval;
}
/* ----------------------- LLSEEK ------------------------ */

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t new_pos = 0;
    loff_t total_size = 0;
    uint8_t index;
    struct aesd_buffer_entry *entry;

    if (!dev)
        return -EINVAL;

    /* Compute total stored bytes */
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, index) {
        if (entry && entry->buffptr)
            total_size += entry->size;
    }

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;

    case SEEK_CUR:
        new_pos = filp->f_pos + offset;
        break;

    case SEEK_END:
        new_pos = total_size + offset;
        break;

    default:
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    if (new_pos < 0 || new_pos > total_size) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    filp->f_pos = new_pos;

    mutex_unlock(&dev->lock);
    return new_pos;
}

/* ----------------------- IOCTL ------------------------- */

long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    struct aesd_buffer_entry *entry;
    uint8_t index = 0;
    loff_t new_pos = 0;

    if (cmd != AESDCHAR_IOCSEEKTO)
        return -ENOTTY;

    if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)))
        return -EFAULT;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* Validate command index */
    entry = aesd_circular_buffer_find_entry(&dev->buffer, seekto.write_cmd);
    if (!entry || !entry->buffptr) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    /* Validate offset */
    if (seekto.write_cmd_offset >= entry->size) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    /* Compute absolute position */
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, index) {
        if (index == seekto.write_cmd)
            break;
        new_pos += entry->size;
    }

    new_pos += seekto.write_cmd_offset;

    filp->f_pos = new_pos;

    mutex_unlock(&dev->lock);
    return new_pos;
}

/* ---------------- FILE OPERATIONS ---------------------- */

struct file_operations aesd_fops = {
    .owner = THIS_MODULE,
    .read = aesd_read,
    .write = aesd_write,
    .open = aesd_open,
    .release = aesd_release,
    .llseek = aesd_llseek,
    .unlocked_ioctl = aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.buffer);
    aesd_device.write_buffer = NULL;
    aesd_device.size = 0;
    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    struct aesd_buffer_entry *entry;


    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */
     AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr) {
            kfree(entry->buffptr);
            entry->buffptr = NULL;
        }
    }


    if (aesd_device.write_buffer) {
        kfree(aesd_device.write_buffer);
        aesd_device.write_buffer = NULL;
    }


    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
