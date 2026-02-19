// SPDX-License-Identifier: GPL-2.0
/* Simple EDU PCI driver (educational) --- adapted for local tree
 * Minimal implementation: probe/remove, simple irq handler and mmio access
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("EDU-driver import");
MODULE_DESCRIPTION("Educational PCI EDU driver (minimal)");

/* Vendor/device id chosen by upstream EDU project */
#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e9

static int major;
static void __iomem *mmio_base;
static struct pci_dev *edu_pci_dev;
static struct class *edu_class;
static struct device *edu_device;

/* IRQ handler: acknowledge and print */
static irqreturn_t edu_irq_handler(int irq, void *dev_id)
{
    u32 status;
    if (!mmio_base)
        return IRQ_NONE;
    status = ioread32(mmio_base + 0x24);
    pr_info("edu: irq=%d status=0x%08x\n", irq, status);
    /* acknowledge (if device uses that register) */
    iowrite32(status, mmio_base + 0x64);
    return IRQ_HANDLED;
}

static ssize_t edu_read(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
    u32 val = 0;
    if (!mmio_base)
        return -EIO;
    /* simple 32-bit read from mmio at offset *off */
    if (*off + sizeof(u32) > pci_resource_len(edu_pci_dev, 0))
        return 0;
    val = ioread32(mmio_base + *off);
    if (copy_to_user(buf, &val, min(len, (size_t)sizeof(val))))
        return -EFAULT;
    *off += sizeof(u32);
    return sizeof(u32);
}

static ssize_t edu_write(struct file *filp, const char __user *buf, size_t len, loff_t *off)
{
    u32 val = 0;
    if (!mmio_base)
        return -EIO;
    if (*off + sizeof(u32) > pci_resource_len(edu_pci_dev, 0))
        return -EINVAL;
    if (copy_from_user(&val, buf, min(len, (size_t)sizeof(val))))
        return -EFAULT;
    iowrite32(val, mmio_base + *off);
    *off += sizeof(u32);
    return sizeof(u32);
}

static loff_t edu_llseek(struct file *filp, loff_t off, int whence)
{
    filp->f_pos = off;
    return off;
}

static const struct file_operations edu_fops = {
    .owner = THIS_MODULE,
    .read = edu_read,
    .write = edu_write,
    .llseek = edu_llseek,
};

static struct pci_device_id edu_pci_ids[] = {
    { PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, edu_pci_ids);

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    int ret = 0;

    pr_info("edu: probe called...\n");

    major = register_chrdev(0, "edu", &edu_fops);
    if (major < 0) {
        pr_err("edu: register_chrdev failed: %d\n", major);
        return major;
    }
    pr_info("edu: registered major=%d\n", major);

    /* create device class and device node /dev/edu */
    edu_class = class_create("edu");
    if (IS_ERR(edu_class)) {
        pr_err("edu: class_create failed\n");
        unregister_chrdev(major, "edu");
        return PTR_ERR(edu_class);
    }

    edu_device = device_create(edu_class, NULL, MKDEV(major, 0), NULL, "edu");
    if (IS_ERR(edu_device)) {
        pr_warn("edu: device_create failed (udev may not be available)\n");
        edu_device = NULL;
    }

    ret = pci_enable_device(pdev);
    if (ret) {
        pr_err("edu: pci_enable_device failed\n");
        return ret;
    }

    if (pci_request_region(pdev, 0, "edu_region")) {
        pr_warn("edu: region busy\n");
    }

    if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28)))
        dev_warn(&pdev->dev, "edu: no suitable DMA available\n");

    mmio_base = pci_iomap(pdev, 0, pci_resource_len(pdev, 0));
    if (!mmio_base) {
        pr_err("edu: pci_iomap failed\n");
        pci_release_region(pdev, 0);
        pci_disable_device(pdev);
        return -ENOMEM;
    }

    pci_set_master(pdev);
    edu_pci_dev = pdev;

    /* register irq if present */
    if (pdev->irq)
        ret = request_irq(pdev->irq, edu_irq_handler, IRQF_SHARED, "edu_irq", &major);

    pr_info("edu: probe done\n");
    return 0;
}

static void edu_remove(struct pci_dev *pdev)
{
    pr_info("edu: remove\n");
    if (pdev->irq)
        free_irq(pdev->irq, &major);
    if (mmio_base)
        pci_iounmap(pdev, mmio_base);
    pci_release_region(pdev, 0);
    /* destroy device and class if present */
    if (edu_device)
        device_destroy(edu_class, MKDEV(major, 0));
    if (edu_class)
        class_destroy(edu_class);
    unregister_chrdev(major, "edu");
    pci_disable_device(pdev);
}

static struct pci_driver edu_driver = {
    .name = "edu",
    .id_table = edu_pci_ids,
    .probe = edu_probe,
    .remove = edu_remove,
};

static int __init edu_init_module(void)
{
    return pci_register_driver(&edu_driver);
}

static void __exit edu_exit_module(void)
{
    pci_unregister_driver(&edu_driver);
}

module_init(edu_init_module);
module_exit(edu_exit_module);
