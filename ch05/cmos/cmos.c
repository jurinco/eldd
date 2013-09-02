#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/pci.h>

MODULE_LICENSE("GPL");


#define NUM_CMOS_BANKS 2

#define CMOS_BANK_SIZE              (0xFF*8)
#define DEVICE_NAME                 "cmos"
#define CMOS_BANK0_INDEX_PORT       0x70
#define CMOS_BANK0_DATA_PORT        0x71
#define CMOS_BANK1_INDEX_PORT       0x72
#define CMOS_BANK1_DATA_PORT        0x73


unsigned char addrports[NUM_CMOS_BANKS] = {CMOS_BANK0_INDEX_PORT,
                                           CMOS_BANK1_INDEX_PORT,};
unsigned char dataports[NUM_CMOS_BANKS] = {CMOS_BANK0_DATA_PORT,
                                           CMOS_BANK1_DATA_PORT,};


/* Per-device (per-bank) structure */
struct cmos_dev {
    unsigned short current_pointer; /* Current pointer within the
                                       bank */
    unsigned int size;              /* Size of the bank */
    int bank_number;                /* CMOS bank number */
    struct cdev cdev;               /* The cdev structure */
    char name[10];                  /* Name of I/O region */
    /* ... */                       /* Mutexes, spinlocks, wait
                                       queues, .. */
} *cmos_devp[NUM_CMOS_BANKS];

/*
 * Open CMOS bank
 */
int
cmos_open(struct inode *inode, struct file *file)
{
    struct cmos_dev *cmos_devp;

    /* Get the per-device structure that contains this cdev */
    cmos_devp = container_of(inode->i_cdev, struct cmos_dev, cdev);

    /* Easy access to cmos_devp from rest of the entry points */
    file->private_data = cmos_devp;

    /* Initialize some fields */
    cmos_devp->size = CMOS_BANK_SIZE;
    cmos_devp->current_pointer = 0;

    return 0;
}

/*
 * Release CMOS bank
 */
int
cmos_release(struct inode *inode, struct file *file)
{
    struct cmos_dev *cmos_devp = file->private_data;

    /* Reset file pointer */
    cmos_devp->current_pointer = 0;

    return 0;
}

/*
 * Read data from specified CMOS bank
 */
unsigned char
port_data_in(unsigned char offset, int bank)
{
    unsigned char data;

    if (unlikely(bank >= NUM_CMOS_BANKS)) {
        printk("Unknown CMOS Bank\n");
        return 0;
    } else {
        outb(offset, addrports[bank]); /* Read a byte */
        data = inb(dataports[bank]);
    }

    return data;
}

/*
 * Write data to specified CMOS bank
 */
void
port_data_out(unsigned char offset, unsigned char data,
              int bank)
{
    if (unlikely(bank >= NUM_CMOS_BANKS)) {
        printk("Unknown CMOS Bank\n");
        return;
    } else {
        outb(offset, addrports[bank]); /* Output a byte */
        outb(data, dataports[bank]);
    }

    return;
}

/*
 * Read from a CMOS Bank at bit-level granularity
 */
ssize_t
cmos_read(struct file *file, char *buf,
          size_t count, loff_t *ppos)
{
    struct cmos_dev *cmos_devp = file->private_data;
    char data[CMOS_BANK_SIZE];
    unsigned char mask;
    int xferred = 0, i = 0, l, zero_out;
    int start_byte = cmos_devp->current_pointer/8;
    int start_bit = cmos_devp->current_pointer%8;

    if (cmos_devp->current_pointer >= cmos_devp->size) {
        return 0; /*EOF*/
    }

    /* Adjust count if it edges past the end of the CMOS bank */
    if (cmos_devp->current_pointer + count > cmos_devp->size) {
        count = cmos_devp->size - cmos_devp->current_pointer;
    }

    /* Get the specified number of bits from the CMOS */
    while (xferred < count) {
        data[i] = port_data_in(start_byte, cmos_devp->bank_number)
            >> start_bit;
        xferred += (8 - start_bit);
        if ((start_bit) && (count + start_bit > 8)) {
            data[i] |= (port_data_in (start_byte + 1,
                        cmos_devp->bank_number) << (8 - start_bit));
            xferred += start_bit;
        }
        start_byte++;
        i++;
    }

    if (xferred > count) {
        /* Zero out (xferred-count) bits from the MSB
           of the last data byte */
        zero_out = xferred - count;
        mask = 1 << (8 - zero_out);
        for (l=0; l < zero_out; l++) {
            data[i-1] &= ~mask; mask <<= 1;
        }
        xferred = count;
    }

    if (!xferred) return -EIO;

    /* Copy the read bits to the user buffer */
    //if (copy_to_user(buf, (void *)data, ((xferred/8)+1)) != 0) {
    if (copy_to_user(buf, (void *)data, (xferred+7)/8) != 0) { 
        return -EIO;
    }

    /* Increment the file pointer by the number of xferred bits */
    cmos_devp->current_pointer += xferred;

    return xferred; /* Number of bits read */
}

/*
 * Write to a CMOS bank at bit-level granularity. 'count' holds the
 * number of bits to be written.
 */
ssize_t
cmos_write(struct file *file, const char *buf,
           size_t count, loff_t *ppos)
{
    struct cmos_dev *cmos_devp = file->private_data;
    int xferred = 0, i = 0, l, end_l, start_l;
    char *kbuf, tmp_kbuf;
    unsigned char tmp_data = 0, mask;
    int start_byte = cmos_devp->current_pointer/8;
    int start_bit = cmos_devp->current_pointer%8;

    if (cmos_devp->current_pointer >= cmos_devp->size) {
        return 0; /* EOF */
    }
    
    /* Adjust count if it edges past the end of the CMOS bank */
    if (cmos_devp->current_pointer + count > cmos_devp->size) {
        count = cmos_devp->size - cmos_devp->current_pointer;
    }

    kbuf = kmalloc((count/8)+1,GFP_KERNEL);
    if (kbuf==NULL)
        return -ENOMEM;

    /* Get the bits from the user buffer */
    if (copy_from_user(kbuf,buf,(count/8)+1)) {
        kfree(kbuf);
        return -EFAULT;
    }

    /* Write the specified number of bits to the CMOS bank */
    while (xferred < count) {
        tmp_data = port_data_in(start_byte, cmos_devp->bank_number);
        mask = 1 << start_bit;
        end_l = 8;

        if ((count-xferred) < (8 - start_bit)) {
            end_l = (count - xferred) + start_bit;
        }

        for (l = start_bit; l < end_l; l++) {
            tmp_data &= ~mask; mask <<= 1;
        }

        tmp_kbuf = kbuf[i];
        mask = 1 << end_l;
        for (l = end_l; l < 8; l++) {
            tmp_kbuf &= ~mask;
            mask <<= 1;
        }

        port_data_out(start_byte,
                      tmp_data |(tmp_kbuf << start_bit),
                      cmos_devp->bank_number);
        xferred += (end_l - start_bit);

        if ((xferred < count) && (start_bit) &&
            (count + start_bit > 8)) {
            tmp_data = port_data_in(start_byte+1,
                                    cmos_devp->bank_number);
            
            start_l = ((start_bit + count) % 8);
            mask = 1 << start_l;
            for (l=0; l < start_l; l++) {
                mask >>= 1;
                tmp_data &= ~mask;
            }
            port_data_out((start_byte+1),
                          tmp_data |(kbuf[i] >> (8 - start_bit)),
                          cmos_devp->bank_number);
            xferred += start_l;
        }
        start_byte++;
        i++;
    }

    if (!xferred) return -EIO;
    /* Push the offset pointer forward */
    cmos_devp->current_pointer += xferred;

    return xferred; /* Return the number of written bits */
}

/*
 * Seek to a bit offset within a CMOS bank
 */
static loff_t
cmos_llseek(struct file *file, loff_t offset,
            int orig)
{
    struct cmos_dev *cmos_devp = file->private_data;

    switch (orig) {
    case 0: /* SEEK_SET */
        if (offset >= cmos_devp->size) {
            return -EINVAL;
        }
        cmos_devp->current_pointer = offset; /* Bit Offset */
        break;
    case 1: /* SEEK_CURR */
        if ((cmos_devp->current_pointer + offset) >=
            cmos_devp->size) {
            return -EINVAL;
        }
        cmos_devp->current_pointer = offset; /* Bit Offset */
        break;
    case 2: /* SEEK_END - Not supported */
        return -EINVAL;
    default:
        return -EINVAL;
    }

    return(cmos_devp->current_pointer);
}

#define CMOS_ADJUST_CHECKSUM 1
#define CMOS_VERIFY_CHECKSUM 2
#define CMOS_BANK1_CRC_OFFSET 0x1E

unsigned short adjust_cmos_crc(unsigned short bank, unsigned short seed) {
    return 0;
}

/*
 * Ioctls to adjust and verify CRC16s.
 */
static int
cmos_ioctl(struct inode *inode, struct file *file,
           unsigned int cmd, unsigned long arg)
{
    unsigned short crc = 0;
    unsigned char buf;

    switch (cmd) {
    case CMOS_ADJUST_CHECKSUM:
        /* Calculate the CRC of bank0 using a seed of 0 */
        crc = adjust_cmos_crc(0, 0);
        /* Seed bank1 with CRC of bank0 */
        crc = adjust_cmos_crc(1, crc);
        /* Store calculated CRC */
        port_data_out(CMOS_BANK1_CRC_OFFSET,
                      (unsigned char)(crc & 0xFF), 1);
        port_data_out((CMOS_BANK1_CRC_OFFSET + 1),
                      (unsigned char) (crc >> 8), 1);
        break;
    case CMOS_VERIFY_CHECKSUM:
        /* Calculate the CRC of bank0 using a seed of 0 */
        crc = adjust_cmos_crc(0, 0);
        /* Seed bank1 with CRC of bank0 */
        crc = adjust_cmos_crc(1, crc);
        /* Compare the calculated CRC with the stored CRC */
        buf = port_data_in(CMOS_BANK1_CRC_OFFSET, 1);
        if (buf != (unsigned char) (crc & 0xFF)) return -EINVAL;
        buf = port_data_in((CMOS_BANK1_CRC_OFFSET+1), 1);
        if (buf != (unsigned char)(crc >> 8)) return -EINVAL;
        break;
        
    default:
        return -EIO;
    }

    return 0;
}



/* File operations structure. Defined in linux/fs.h */
static struct file_operations cmos_fops = {
    .owner = THIS_MODULE,           /* Owner */
    .open = cmos_open,              /* Open method */
    .release = cmos_release,        /* Release method */
    .read = cmos_read,              /* Read method */
    .write = cmos_write,            /* Write method */
    .llseek = cmos_llseek,          /* Seek method */
    .ioctl = cmos_ioctl,            /* Ioctl method */
};

static dev_t cmos_dev_number;       /* Allotted device number */
struct class *cmos_class;           /* Tie with the device model */

/*
 * Driver Initialization
 */
int __init
cmos_init(void)
{
    int i, ret;

    /* Request dynamic allocation of a device major number */
    if (alloc_chrdev_region(&cmos_dev_number, 0,
                            NUM_CMOS_BANKS, DEVICE_NAME) < 0) {
        printk("Can't register device\n"); return -1;
    }

    /* Populate sysfs entries */
    cmos_class = class_create(THIS_MODULE, DEVICE_NAME);

    for (i=0; i<NUM_CMOS_BANKS; i++) {
        /* Allocate memory for the per-device structure */
        cmos_devp[i] = kmalloc(sizeof(struct cmos_dev), GFP_KERNEL);
        if (!cmos_devp[i]) {
            printk("Bad Kmalloc\n"); return -ENOMEM;
        }

        /* Request I/O region */
        sprintf(cmos_devp[i]->name, "cmos%d", i);
        if (!(request_region(addrports[i], 2, cmos_devp[i]->name))) {
            printk("cmos: I/O port 0x%x is not free.\n", addrports[i]);
            return -EIO;
        }         

        /* Fill in the bank number to correlate this device
           with the corresponding CMOS bank */
        cmos_devp[i]->bank_number = i;

        /* Connect the file operations with the cdev */
        cdev_init(&cmos_devp[i]->cdev, &cmos_fops);
        cmos_devp[i]->cdev.owner = THIS_MODULE;

        /* Connect the major/minor number to the cdev */
        ret = cdev_add(&cmos_devp[i]->cdev, (cmos_dev_number + i), 1);
        if (ret) {
            printk("Bad cdev\n");
            return ret;
        }

        /* Send uevents to udev, so it'll create /dev nodes */
        device_create(cmos_class, NULL, MKDEV(MAJOR(cmos_dev_number), i), NULL,
                      "cmos%d", i);
    }

    printk("CMOS Driver Initialized.\n");
    return 0;
}

/* Driver Exit */
void __exit
cmos_cleanup(void)
{
    int i;

    /* Release the major number */
    unregister_chrdev_region((cmos_dev_number), NUM_CMOS_BANKS);

    /* Release I/O region */
    for (i=0; i<NUM_CMOS_BANKS; i++) {
        device_destroy (cmos_class, MKDEV(MAJOR(cmos_dev_number), i));
        release_region(addrports[i], 2);
        cdev_del(&cmos_devp[i]->cdev);
        kfree(cmos_devp[i]);
    }

    /* Destroy cmos_class */
    class_destroy(cmos_class);

    return;
}

module_init(cmos_init);
module_exit(cmos_cleanup); 
