#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
#include <linux/cdev.h>
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 18)
#include <linux/uaccess.h>
#else
#include <asm/uaccess.h>
#endif
#include <asm/io.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/pci.h>

#ifndef __user
#define __user
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
#define pccard_ioremap(addr, size) ioremap((addr), (size))
#else
#define pccard_ioremap(addr, size) ioremap_nocache((addr), (size))
#endif

// linux/kdev_t.h
// we might need this.
#include "defs.h"

static struct cardbus_info
{
  unsigned int bus;
  unsigned int dev_fn;
  unsigned int status_cmd;
  unsigned int exca_base_address;
  unsigned int mem_base_0;
  unsigned int mem_base_1;
  unsigned int mem_limit_0;
  unsigned int mem_limit_1;
  unsigned int io_base_0;
  unsigned int io_base_1;
  unsigned int io_limit_0;
  unsigned int io_limit_1;
  unsigned short bridge_ctrl;
  unsigned int bridge_interrupt_pin;
  unsigned int bridge_line;
  unsigned int pccard_base_address;
  unsigned int sys_ctrl;
  unsigned short card_ctrl;
  unsigned char dev_ctrl;
  unsigned char diag_reg;
  unsigned int v_exca_base_address;
  unsigned int v_exca_mem_base;
} CBI;

static unsigned long ulMemBase = 0;
static unsigned long m_ulReadMissTimes = 0;
static unsigned long m_ulLinuxReadCnt = 0;
static int pccard_major = 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
static dev_t pccard_devno;
static struct cdev pccard_cdev;
static int pccard_cdev_registered = 0;
#endif
static int pccard_irq = -1;
static int pccard_irq_registered = 0;
static int pccard_use_irq = 0;
static int pccard_8bit_windows = 0;
static int pccard_startup_probe =
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
    1;
#else
    0;
#endif
static int pccard_reset_on_open = 0;
static int pccard_accept_valid_f1 = 1;
static int pccard_debug = 0;
static int pccard_trace = 0;
static int pccard_start_retries = 2;
static int pccard_start_retry_delay_ms = 5000;
static int pccard_parent_window_fallback = 1;
static int pccard_prefer_parent_window =
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
    1;
#else
    0;
#endif
static int pccard_using_parent_window = 0;
static unsigned char bINT = 0;
static unsigned char m_bCmdPortValDiff = 0;
static unsigned char m_bInterruptError = 0;
static unsigned long m_ulIntWhileTimes = 0;
static char *pcCmdPort = NULL;
static char *pcIntPort = NULL;
static unsigned long m_ulIntAnother = 0;
static unsigned char m_bIntComeIn = 0;
static unsigned char m_bReadComeIn = 0;
static unsigned char m_bWriteComeIn = 0;
static unsigned long m_ulA27IntCnt = 0;
static unsigned char m_bCmdPortValRead = 0;
static unsigned char m_bCmdPortVal = 0;
static char *pcClrIntPort = NULL;
static char *pcClrExCAIntPort = NULL;
static unsigned char ucCmdPortVal = 0;

static int pccard_open(struct inode *inode, struct file *file);
static irqreturn_t pccard_interrupt(int irq, void *dev_id);
static void pccard_probe_poke(void);
static int pccard_response_checksum_ok(void);
static int pccard_write_checksum_ok(void);
static unsigned int pccard_read32(unsigned long addr);
static void pccard_trace_read(unsigned char status, unsigned int copy_len);
static void pccard_trace_write(loff_t off, size_t count, unsigned char expect,
                               unsigned char first, unsigned char second);
static void pccard_sleep_ms(unsigned int ms);
static int pccard_allocate_parent_window(struct pci_dev *slot);
void ResetExCA(void);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
#define PCCARD_MINOR(inode) iminor(inode)
#else
#define PCCARD_MINOR(inode) MINOR((inode)->i_rdev)
#endif

#define PCCARD_APERTURE_SIZE 0x03100000UL
#define PCCARD_MEM_OFFSET 0x02000000UL
#define PCCARD_MEM_SIZE 0x01000000UL
#define PCCARD_ALIGN 0x01000000UL
#define PCCARD_LEGACY_APERTURE_BASE 0xCB000000UL
#define PCCARD_USE_LEGACY_APERTURE 1
#define PCCARD_READ8(addr) readb((void __iomem *)(addr))
#define PCCARD_WRITE8(addr, v) writeb((v), (void __iomem *)(addr))
#define PCCARD_WRITE32(addr, v) writel((v), (void __iomem *)(addr))

static struct resource pccard_mmio_resource = {.name = "igs-27a-pccard",
                                               .start = 0,
                                               .end = 0,
                                               .flags = IORESOURCE_MEM | IORESOURCE_BUSY};
static int pccard_mmio_allocated = 0;

static irqreturn_t
pccard_interrupt(int irq, void *dev_id)
{
  unsigned long result = ulMemBase + 0x1FFFF;
  unsigned char v1;

  if (PCCARD_READ8(ulMemBase + 0x1FFFF) != 'R' || PCCARD_READ8(ulMemBase + 0x1FFFE) != 'F' || PCCARD_READ8(ulMemBase + 0x1FFFD) != '4')
    return IRQ_NONE;

  PCCARD_WRITE8(ulMemBase + 0x1FFFF, 0);
  PCCARD_WRITE8(result - 1, 0);
  PCCARD_WRITE8(result - 2, 0);
  ucCmdPortVal = PCCARD_READ8(pcCmdPort);
  m_bCmdPortValDiff = 0;
  m_ulReadMissTimes = 0;
  m_bInterruptError = 0;
  m_ulIntWhileTimes = 0;

  if ((unsigned char)++m_bCmdPortVal > 0xEFu)
    m_bCmdPortVal = 1;

  v1 = PCCARD_READ8(pcCmdPort);
  if (PCCARD_READ8(pcCmdPort) == 0xF1)
  {
    printk("igs-pccard: <int>debug code=%d,%d,%d\n",
           PCCARD_READ8(ulMemBase + 0x1F800),
           PCCARD_READ8(ulMemBase + 0x1F801),
           PCCARD_READ8(ulMemBase + 0x1F802));
  }

  if (m_bCmdPortVal != v1)
  {
    printk(
        "igs-pccard: <int>command port value error!! expected=%d got=%d\n",
        (unsigned char)m_bCmdPortVal, v1);
    m_bCmdPortValDiff = 1;
  }

  m_bCmdPortValRead = v1;
  PCCARD_WRITE8(pcCmdPort, m_bCmdPortVal);
  bINT = 0xFF;
  return IRQ_HANDLED;
}

static void
pccard_probe_poke(void)
{
  int probe_attempt;

  for (probe_attempt = 0; probe_attempt <= 4; ++probe_attempt)
  {
    unsigned char value = (unsigned char)(probe_attempt - 0x79);

    PCCARD_WRITE8(pcCmdPort, value);
    PCCARD_WRITE8(pcIntPort, value);
    udelay(1000);
    (void)PCCARD_READ8(pcClrExCAIntPort);
  }
}

static int
pccard_response_checksum_ok(void)
{
  unsigned char len, has_msg, ir_password, light_reset;
  unsigned char err, coin, errcnt, mode, checksum_1, checksum_2;
  unsigned char checksum;

  len = PCCARD_READ8(ulMemBase + 0x00);
  has_msg = PCCARD_READ8(ulMemBase + 0x04);
  ir_password = PCCARD_READ8(ulMemBase + 0x08);
  light_reset = PCCARD_READ8(ulMemBase + 0x09);
  err = PCCARD_READ8(ulMemBase + 0x0A);
  mode = PCCARD_READ8(ulMemBase + 0x3C);
  coin = PCCARD_READ8(ulMemBase + 0x3E);
  errcnt = PCCARD_READ8(ulMemBase + 0x40);
  checksum_1 = PCCARD_READ8(ulMemBase + 0x42);
  checksum_2 = PCCARD_READ8(ulMemBase + 0x43);

  checksum = has_msg + ir_password + light_reset + errcnt + err + coin + len + mode;

  if (err == 0 && checksum_1 == checksum_2 && checksum_1 == checksum)
    return 1;

  if (pccard_debug)
    printk("igs-pccard: reject err=%u chk=%02x/%02x calc=%02x len=%u mode=%u "
           "msg=%u\n",
           err, checksum_1, checksum_2, checksum, len, mode, has_msg);
  return 0;
}

static int
pccard_write_checksum_ok(void)
{
  unsigned char checksum;

  checksum = PCCARD_READ8(ulMemBase + 0x04);
  checksum += PCCARD_READ8(ulMemBase + 0x00);
  checksum += PCCARD_READ8(ulMemBase + 0x16);
  checksum += PCCARD_READ8(ulMemBase + 0x17);

  return PCCARD_READ8(ulMemBase + 0x14) == PCCARD_READ8(ulMemBase + 0x15) && PCCARD_READ8(ulMemBase + 0x14) == checksum;
}

static unsigned int
pccard_read32(unsigned long addr)
{
  return PCCARD_READ8(addr) | (PCCARD_READ8(addr + 1) << 8) | (PCCARD_READ8(addr + 2) << 16) | (PCCARD_READ8(addr + 3) << 24);
}

static void
pccard_trace_read(unsigned char status, unsigned int copy_len)
{
  unsigned char checksum;

  if (!pccard_trace)
    return;

  checksum = PCCARD_READ8(ulMemBase + 0x04);
  checksum += PCCARD_READ8(ulMemBase + 0x08);
  checksum += PCCARD_READ8(ulMemBase + 0x09);
  checksum += PCCARD_READ8(ulMemBase + 0x40);
  checksum += PCCARD_READ8(ulMemBase + 0x0A);
  checksum += PCCARD_READ8(ulMemBase + 0x3E);
  checksum += PCCARD_READ8(ulMemBase + 0x00);
  checksum += PCCARD_READ8(ulMemBase + 0x3C);

  printk("igs-pccard: R status=%02x cmd=%02x raw=%02x len=%u mode=%u err=%u "
         "errcnt=%u coin=%u chk=%02x/%02x calc=%02x copy=%u msg=%u ir=%u "
         "light=%u\n",
         status, (unsigned char)m_bCmdPortVal, m_bCmdPortValRead,
         pccard_read32(ulMemBase + 0x00), PCCARD_READ8(ulMemBase + 0x3C),
         PCCARD_READ8(ulMemBase + 0x0A), PCCARD_READ8(ulMemBase + 0x40),
         PCCARD_READ8(ulMemBase + 0x3E), PCCARD_READ8(ulMemBase + 0x42),
         PCCARD_READ8(ulMemBase + 0x43), checksum, copy_len,
         PCCARD_READ8(ulMemBase + 0x04), PCCARD_READ8(ulMemBase + 0x08),
         PCCARD_READ8(ulMemBase + 0x09));
}

static void
pccard_trace_write(loff_t off, size_t count, unsigned char expect,
                   unsigned char first, unsigned char second)
{
  unsigned char checksum;

  if (!pccard_trace)
    return;

  checksum = PCCARD_READ8(ulMemBase + 0x04);
  checksum += PCCARD_READ8(ulMemBase + 0x00);
  checksum += PCCARD_READ8(ulMemBase + 0x16);
  checksum += PCCARD_READ8(ulMemBase + 0x17);

  printk("igs-pccard: W off=%u count=%u expect=%02x first=%02x second=%02x "
         "len=%u mode=%u chk=%02x/%02x calc=%02x light_disable=%u field17=%u "
         "b08=%02x b09=%02x b0a=%02x b0b=%02x\n",
         (unsigned int)off, (unsigned int)count, expect, first, second,
         pccard_read32(ulMemBase + 0x00), PCCARD_READ8(ulMemBase + 0x04),
         PCCARD_READ8(ulMemBase + 0x14), PCCARD_READ8(ulMemBase + 0x15),
         checksum, PCCARD_READ8(ulMemBase + 0x16),
         PCCARD_READ8(ulMemBase + 0x17), PCCARD_READ8(ulMemBase + 0x08),
         PCCARD_READ8(ulMemBase + 0x09), PCCARD_READ8(ulMemBase + 0x0A),
         PCCARD_READ8(ulMemBase + 0x0B));
}

static void
pccard_sleep_ms(unsigned int ms)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
  msleep(ms);
#else
  mdelay(ms);
#endif
}

int pccard_bDataSend(void)
{
  int v0; // edx
  int i;  // edx
  int j;  // eax
  unsigned long started;

  v0 = ulMemBase + 0x1FFFF;
  PCCARD_WRITE8(ulMemBase + 0x1FFFF, 0);
  PCCARD_WRITE8(v0 - 1, 0);
  PCCARD_WRITE8(v0 - 2, 0);
  m_ulReadMissTimes = 0;
  m_bCmdPortValDiff = 0;
  m_bInterruptError = 0;
  m_ulIntWhileTimes = 0;
  started = jiffies;
  while (PCCARD_READ8(pcCmdPort) <= 0xEFu)
  {
    for (i = 0; i <= 99; ++i)
    {
      for (j = 4; j >= 0; --j)
        ;
    }
    ++m_ulIntWhileTimes;
    if ((unsigned long)(jiffies - started) > 1000)
    {
      printk("igs-pccard: time out waiting for card data(cmdport=%u)\n",
             PCCARD_READ8(pcCmdPort));
      m_bInterruptError = 1;
      return 0;
    }
  }
  m_bCmdPortValRead = PCCARD_READ8(pcCmdPort);
  PCCARD_WRITE8(pcCmdPort, m_bCmdPortVal);
  return 1;
}

static ssize_t
pccard_i_write(struct file *file, const char __user *ubuff, size_t count,
               loff_t *off)
{

  int v4;            // ecx
  char v5;           // al
  int v6;            // ebx
  int v7;            // ecx
  bool v8;           // zf
  int v9;            // edx
  int k;             // eax
  int j;             // eax
  int i;             // edx
  unsigned char v14; // [esp+Eh] [ebp-Ah]
  unsigned char v15; // [esp+Fh] [ebp-9h]
  int start_retried = 0;
  unsigned char trace_expect;
  *off = file->f_pos;
  v4 = ulMemBase + 0x1FFFF;
  if (*off == 254)
  {
    v5 = (unsigned char)*off;
    m_ulIntAnother = 0;
    m_ulLinuxReadCnt = 0;
    m_bIntComeIn = 0;
    m_bReadComeIn = 0;
    m_bWriteComeIn = 0;
    bINT = 0;
    m_bCmdPortVal = 1;
    PCCARD_WRITE8(pcCmdPort, v5);
    m_ulA27IntCnt = 0;
    PCCARD_WRITE32(v4 - 6, 0);
    if (pccard_trace)
      printk("igs-pccard: W start off=%u count=%u cmd=%02x\n",
             *(unsigned int *)off, (unsigned int)count, v5);
    printk("<write>==============================\n");
    printk("<write>==============================\n");
    printk("<write>==============================\n");
    printk("<write>==============================\n");
    printk("<write>==============================\n");
    printk("<write>start(%d)\n", *(unsigned int *)off);
    printk("<write>==============================\n");
    printk("<write>==============================\n");
    printk("<write>==============================\n");
    printk("<write>==============================\n");
  }
  else
  {
    if ((unsigned char)++m_bCmdPortVal > 0xEFu)
      m_bCmdPortVal = 1;
    PCCARD_WRITE8(pcCmdPort, m_bCmdPortVal);
    copy_from_user((void *)ulMemBase, ubuff, count);
    trace_expect = m_bCmdPortVal + 1;
    if (trace_expect > 0xEFu)
      trace_expect = 1;
    pccard_trace_write(*off, count, trace_expect, PCCARD_READ8(pcCmdPort),
                       0);
  }
  v7 = jiffies;
  wmb();
  PCCARD_WRITE8(pcIntPort, 1);
  if ((unsigned char)++m_bCmdPortVal > 0xEFu)
    m_bCmdPortVal = 1;
  mb();
  v15 = PCCARD_READ8(pcCmdPort);
  v14 = PCCARD_READ8(pcCmdPort);
  v8 = 1;
  v6 = 1;
  while (1)
  {
    if (!v8)
    {
      for (i = 499; i >= 0; --i)
        ;
      goto LABEL_21;
    }
    if (v15 == m_bCmdPortVal)
      return v6;
    v9 = 0;
    if (v15 == 0xFD)
      break;
    do
    {
      for (j = 9; j >= 0; --j)
        ;
      ++v9;
    } while (v9 <= 99);
  LABEL_21:
    if ((unsigned int)(jiffies - v7) > 1000)
    {
      if (*off != 254 && pccard_accept_valid_f1 && v15 == 0xF1 && v14 == 0xF1 && pccard_write_checksum_ok())
      {
        if (pccard_debug)
          printk("igs-pccard: <write>f_pos=%u,accepting valid 0xf1 response as "
                 "write ack(%d,%d,%d)\n",
                 *(unsigned int *)off, (unsigned char)m_bCmdPortVal,
                 v15, v14);
        return 1;
      }
      if (*off == 254 && start_retried < pccard_start_retries && ((v15 == 0xF1 && v14 == 0xF1) || (v15 == 0 && v14 == 0)))
      {
        printk("igs-pccard: <write>f_pos=%u,start retry %d after priming "
               "timeout(%d,%d,%d)\n",
               *(unsigned int *)off, start_retried + 1,
               (unsigned char)m_bCmdPortVal, v15, v14);
        ++start_retried;
        if (pccard_start_retry_delay_ms > 0)
          pccard_sleep_ms(pccard_start_retry_delay_ms);
        m_bCmdPortVal = 1;
        PCCARD_WRITE8(pcCmdPort, (unsigned char)*off);
        wmb();
        PCCARD_WRITE8(pcIntPort, 1);
        if ((unsigned char)++m_bCmdPortVal > 0xEFu)
          m_bCmdPortVal = 1;
        mb();
        v7 = jiffies;
        v15 = PCCARD_READ8(pcCmdPort);
        v14 = PCCARD_READ8(pcCmdPort);
        v8 = 1;
        continue;
      }
      printk("igs-pccard: <write>f_pos=%u,", *(unsigned int *)off);
      printk("igs-pccard: time out(%d,%d,%d)\n", (unsigned char)m_bCmdPortVal, v15,
             v14);
      return 0;
    }
    v15 = v14;
    v14 = PCCARD_READ8(pcCmdPort);
    v8 = v15 == PCCARD_READ8(pcCmdPort);
  }
  do
  {
    for (k = 9; k >= 0; --k)
      ;
    ++v9;
  } while (v9 <= 9999);
  printk("igs-pccard: <write>f_pos=%u", *(unsigned int *)off);
  printk("igs-pccard: re-send int to asic27\n");
  wmb();
  PCCARD_WRITE8(pcIntPort, 1);
  return 0;
}

static ssize_t
pccard_i_read(struct file *file, char __user *output_buffer, size_t count,
              loff_t *ppos)
{
  unsigned char return_val = 0;
  unsigned int copy_len;
  if (pccard_bDataSend())
  {
    ++m_ulLinuxReadCnt;
    return_val = 0xF3;
    if (!m_bCmdPortValDiff)
    {
      return_val = 0xF4;
      if (!m_bInterruptError)
        return_val = m_bCmdPortValRead;
    }
    if (return_val == 0xF1 && pccard_accept_valid_f1)
    {
      udelay(1000);
      if (pccard_response_checksum_ok())
      {
        if (pccard_debug)
          printk(
              "igs-pccard: accepting valid 0xf1 response as cmd %u\n",
              (unsigned char)m_bCmdPortVal);
        return_val = m_bCmdPortVal;
      }
    }
    *(unsigned char *)(ulMemBase + 0x41) = 0x64;
    copy_len = pccard_read32(ulMemBase);
    if (copy_len > 0x3FFF)
    {
      udelay(1000);
      copy_len = pccard_read32(ulMemBase);
    }
    if (copy_len > 0x3FFF)
    {
      printk("igs-pccard: invalid read length %u; limiting copy\n",
             copy_len);
      copy_len = 0;
      if (return_val <= 0xEF && !pccard_response_checksum_ok())
        return_val = 0xF1;
    }
    pccard_trace_read(return_val, copy_len);
    copy_to_user(output_buffer, (void *)ulMemBase, copy_len + 0x84);
    *(unsigned int *)ulMemBase = 0;
    bINT = 0;
  }
  return return_val;
}

static int
pccard_release(struct inode *inode, struct file *file)
{
  return 0;
}

static loff_t
pccard_seek(struct file *file, loff_t offset, int whence)
{
  file->f_pos = offset;
  return file->f_pos;
}

static struct file_operations pccard_i_fops = {.owner = THIS_MODULE,
                                               .llseek = pccard_seek,
                                               .read = pccard_i_read,
                                               .write = pccard_i_write,
                                               .open = pccard_open,
                                               .release = pccard_release};

static int
pccard_open(struct inode *inode, struct file *file)
{
  printk("igs-pccard: pccard_open\n");

  if (PCCARD_MINOR(inode))
  {
    file->f_op = &pccard_i_fops;
    if (pccard_reset_on_open)
    {
      ResetExCA();
      pccard_probe_poke();
    }
  }

  return 0;
}

#ifndef PCI_ANY_ID
#define PCI_ANY_ID (~0)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0) && !defined(PCI_BUS_NUM_RESOURCES)
#define PCI_BUS_NUM_RESOURCES PCI_BRIDGE_RESOURCE_NUM
#endif

static struct pci_dev *
pccard_get_slot(unsigned int bus, unsigned int devfn)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
  struct pci_bus *pci_bus = pci_find_bus(0, bus);

  if (!pci_bus)
    return NULL;
  return pci_get_slot(pci_bus, devfn);
#else
  return pci_find_slot(bus, devfn);
#endif
}

static void
pccard_put_dev(struct pci_dev *dev)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
  if (dev)
    pci_dev_put(dev);
#endif
}

static int
pccard_allocate_parent_window(struct pci_dev *slot)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
  int i;
  unsigned long bar0;

  bar0 = pci_resource_start(slot, 0);
  if (!bar0 || !(pci_resource_flags(slot, 0) & IORESOURCE_MEM))
  {
    printk("igs-pccard: Error - CardBus BAR0 is not a memory resource\n");
    return 0;
  }

  for (i = 0; i < PCI_BUS_NUM_RESOURCES; ++i)
  {
    struct resource *res = slot->bus->resource[i];

    if (!res || !(res->flags & IORESOURCE_MEM) || !res->start || res->end <= res->start)
      continue;
    if ((unsigned long)(res->end - res->start + 1) < PCCARD_MEM_SIZE)
      continue;
    if (allocate_resource(res, &pccard_mmio_resource, PCCARD_MEM_SIZE,
                          res->start, res->end, PCCARD_ALIGN, NULL, NULL))
      continue;

    pccard_mmio_allocated = 1;
    pccard_using_parent_window = 1;
    CBI.exca_base_address = bar0 & ~0xFUL;
    CBI.mem_base_0 = pccard_mmio_resource.start;
    CBI.mem_limit_0 = CBI.mem_base_0 + PCCARD_MEM_SIZE;
    printk("igs-pccard: parent window fallback: ExCA %08x MEM %08x-%08x\n",
           CBI.exca_base_address, CBI.mem_base_0, CBI.mem_limit_0 - 1);
    return 1;
  }
#endif
  return 0;
}

static struct pci_dev *
_pci_find_slot(unsigned int bus, unsigned int devfn)
{
  struct pci_dev *dev = NULL;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
  while ((dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev)) != NULL)
  {
#else
  while ((dev = pci_find_device(PCI_ANY_ID, PCI_ANY_ID, dev)) != NULL)
  {
#endif
    if (dev->bus->number == bus && dev->devfn == devfn)
    {
      return dev;
    }
  }
  return NULL;
}

static int
pccard_is_supported_bridge(struct pci_dev *slot, unsigned int id)
{
  unsigned int vendor = id & 0xffff;
  unsigned int device = (id >> 16) & 0xffff;
  unsigned int class = slot->class >> 8;

  if (class != PCI_CLASS_BRIDGE_CARDBUS)
    return 0;

  if (vendor == PCI_VENDOR_ID_TI)
  {
    printk("igs-pccard: Accepting TI CardBus bridge device %04x\n", device);
    return 1;
  }

  printk("igs-pccard: Ignoring unsupported CardBus bridge %04x:%04x\n",
         vendor, device);
  return 0;
}

int FindPCIDevice(void)
{
  struct pci_dev *slot;
  int dwRead;
  int tmpval;
  int result = -1;
  int current_bus;
  int current_dev_fn;
  memset(&CBI, 0, sizeof(CBI));
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0)
  if (pcibios_present())
  {
#endif
    for (current_bus = 0; current_bus <= 64; ++current_bus)
    {
      for (current_dev_fn = 0; current_dev_fn <= 255; ++current_dev_fn)
      {
        slot = (struct pci_dev *)_pci_find_slot(current_bus,
                                                current_dev_fn);
        if (slot)
        {
          pci_read_config_dword(slot, 0, &dwRead);
          tmpval = dwRead;
          if (dwRead != -1)
          {
            printk("igs-pccard: BUS %d,DEV %2d DID VID Reg contain "
                   ":%08X\n",
                   current_bus, current_dev_fn, dwRead);
            pci_read_config_dword(slot, 8, &dwRead);
            if ((dwRead & 0xFFFF0000) == 0x6070000)
            {
              printk("igs-pccard: Found CardBUS CLASS on %d\n",
                     current_dev_fn);
              if (pccard_is_supported_bridge(slot, tmpval))
              {
                printk("igs-pccard: Found CardBUS device on:%d "
                       "Class %08X\n",
                       current_dev_fn, dwRead);
                result = current_dev_fn;
                CBI.dev_fn = current_dev_fn;
                CBI.bus = current_bus;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
                pccard_irq = slot->irq;
                printk("igs-pccard: Linux IRQ routed to %d\n",
                       pccard_irq);
#endif
                pccard_put_dev(slot);
                return (result == -1) - 1;
              }
            }
          }
          pccard_put_dev(slot);
        }
      }
    }
    return (result == -1) - 1;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0)
  }
  else
  {
    printk("igs-pccard: No PCI bios present\n");
    return 0;
  }
#endif
}

void GetCardBusInfo(void)
{
  struct pci_dev *slot = pccard_get_slot(CBI.bus, CBI.dev_fn);
  unsigned short tmpval_2;
  int pci_dword;
  int tmpval;
  if (!slot)
  {
    printk("igs-pccard: Error: PCI slot disappeared\n");
    return;
  }
  pci_read_config_dword(slot, 8, &pci_dword);
  // Check to ensure this is a PCCARD
  if ((pci_dword & 0xFFFF0000) != 0x6070000)
  {
    printk("igs-pccard: Error: not PCCARD\n");
    pccard_put_dev(slot);
    return;
  }

  pci_read_config_dword(slot, 4, &CBI.status_cmd);
  pci_read_config_dword(slot, 16, &pci_dword);
  CBI.exca_base_address = pci_dword;
  pci_read_config_dword(slot, 28, &CBI.mem_base_0);
  pci_read_config_dword(slot, 36, &CBI.mem_base_1);
  pci_read_config_dword(slot, 32, &CBI.mem_limit_0);
  pci_read_config_dword(slot, 40, &CBI.mem_limit_1);
  pci_read_config_dword(slot, 44, &CBI.io_base_0);
  pci_read_config_dword(slot, 52, &CBI.io_base_1);
  pci_read_config_dword(slot, 48, &CBI.io_limit_0);
  pci_read_config_dword(slot, 56, &CBI.io_limit_1);
  pci_read_config_dword(slot, 60, &pci_dword);
  tmpval = pci_dword;
  CBI.bridge_line = pci_dword & 0xFF;
  CBI.bridge_ctrl = HIWORD(tmpval);
  CBI.bridge_interrupt_pin = BYTE1(tmpval);
  pci_read_config_dword(slot, 68, &CBI.pccard_base_address);
  pci_read_config_dword(slot, 128, &CBI.sys_ctrl);
  pci_read_config_dword(slot, 144, &pci_dword);
  tmpval_2 = BYTE1(pci_dword);
  *(unsigned short *)&CBI.dev_ctrl = HIWORD(pci_dword);
  CBI.card_ctrl = tmpval_2;
  pccard_put_dev(slot);
}

int SetCardBusInfo(void)
{
  int pci_read_dword = 0;
  struct pci_dev *slot = pccard_get_slot(CBI.bus, CBI.dev_fn);
  if (!slot)
  {
    printk("igs-pccard: Error - PCI slot disappeared\n");
    return 0;
  }
  pci_read_config_dword(slot, 8, &pci_read_dword);
  if ((pci_read_dword & 0xFFFF0000) == 0x6070000)
  {
    if (!pccard_mmio_allocated)
    {
      if (pccard_prefer_parent_window && pccard_allocate_parent_window(slot))
      {
        /* Use the window Linux already forwards to this CardBus bus. */
      }
      else if (allocate_resource(&iomem_resource, &pccard_mmio_resource,
                                 PCCARD_APERTURE_SIZE,
#if PCCARD_USE_LEGACY_APERTURE
                                 PCCARD_LEGACY_APERTURE_BASE,
                                 PCCARD_LEGACY_APERTURE_BASE + PCCARD_APERTURE_SIZE - 1,
#else
                                 0x10000000UL, 0xEFFFFFFFUL,
#endif
                                 PCCARD_ALIGN, NULL, NULL))
      {
        printk("igs-pccard: no free legacy MMIO aperture\n");
        if (!pccard_parent_window_fallback || !pccard_allocate_parent_window(slot))
        {
          printk("igs-pccard: Error - no usable MMIO window\n");
          pccard_put_dev(slot);
          return 0;
        }
      }
      else
      {
        pccard_mmio_allocated = 1;
        pccard_using_parent_window = 0;
        CBI.exca_base_address = pccard_mmio_resource.start;
        CBI.mem_base_0 = CBI.exca_base_address + PCCARD_MEM_OFFSET;
        CBI.mem_limit_0 = CBI.mem_base_0 + PCCARD_MEM_SIZE;
      }
    }

    pci_write_config_word(slot, 4, 7);
    pci_write_config_dword(slot, 0x2C, 0x10000);
    pci_write_config_dword(slot, 0x10, CBI.exca_base_address | 0x8);
    CBI.v_exca_base_address = (unsigned int)pccard_ioremap(CBI.exca_base_address, 0x1000);
    pci_write_config_dword(slot, 0x1C, CBI.mem_base_0 + 0x8);
    pci_write_config_dword(slot, 0x20, CBI.mem_limit_0 + 0x8);
    CBI.v_exca_mem_base = (unsigned int)pccard_ioremap(CBI.mem_base_0, PCCARD_MEM_SIZE);
    if (!CBI.v_exca_base_address || !CBI.v_exca_mem_base)
    {
      printk("igs-pccard: Error - ioremap failed for %08x\n",
             CBI.exca_base_address);
      if (CBI.v_exca_base_address)
        iounmap((void *)CBI.v_exca_base_address);
      if (CBI.v_exca_mem_base)
        iounmap((void *)CBI.v_exca_mem_base);
      release_resource(&pccard_mmio_resource);
      pccard_mmio_allocated = 0;
      pccard_put_dev(slot);
      return 0;
    }
    if (pccard_using_parent_window)
      printk("igs-pccard: using parent forwarded window %08lx-%08lx\n",
             (unsigned long)pccard_mmio_resource.start,
             (unsigned long)pccard_mmio_resource.end);
    else
      printk("igs-pccard: MMIO aperture %08lx-%08lx\n",
             (unsigned long)pccard_mmio_resource.start,
             (unsigned long)pccard_mmio_resource.end);
    pci_write_config_dword(slot, 0x8C, 0x110112);
    pci_write_config_dword(slot, 0xAC, 0);
    pci_read_config_dword(slot, 0x90, &pci_read_dword);
    pci_write_config_dword(slot, 0x90, pci_read_dword & 0xFFF9FFFF);
    pccard_put_dev(slot);
    return 1;
  }
  else
  {
    printk("igs-pccard: Error - Not PCCard\n");
  }
  pccard_put_dev(slot);
  return 0;
}

void SetExCAInfo(void)
{
  unsigned char *exca_buffer = (unsigned char *)CBI.v_exca_base_address;
  exca_buffer[0x806] &= 0xFCu;
  exca_buffer[0x805] |= 4;
  exca_buffer[0x810] = 0;
  exca_buffer[0x811] = pccard_8bit_windows ? 0x00 : 0x80;
  exca_buffer[0x812] = 0xFF;
  exca_buffer[0x813] = 15;
  exca_buffer[0x814] = 0;
  exca_buffer[0x815] = 0;
  exca_buffer[0x840] = HIBYTE(CBI.mem_base_0);
  exca_buffer[0x818] = 0;
  exca_buffer[0x819] = pccard_8bit_windows ? 0x08 : 0x88;
  exca_buffer[0x81A] = 0xFF;
  exca_buffer[0x81B] = 0xF;
  exca_buffer[0x81C] = 0;
  exca_buffer[0x81D] = 8;
  exca_buffer[0x841] = HIBYTE(CBI.mem_base_0);
  exca_buffer[0x806] = 3;
  exca_buffer[0x802] = 0x90;
  printk("igs-pccard: memory windows are %s-bit\n",
         pccard_8bit_windows ? "8" : "16");
}

void DisplayCardInfo(void)
{
  printk("BUS %d,DevNUM %d,Status Command 0x%08x\n", CBI.bus, CBI.dev_fn,
         CBI.status_cmd);
  printk("ExCA Base Address %08X\n", CBI.exca_base_address);
  printk("Memory Base0  %08X, Base1  %08X\n", CBI.mem_base_0, CBI.mem_base_1);
  printk("Memory Limit0 0x%08x, Limit1 0x%08x\n", CBI.mem_limit_0,
         CBI.mem_limit_1);
  printk("IO Base0  %08X, Base1  %08X\n", CBI.io_base_0, CBI.io_base_1);
  printk("IO Limit0 0x%08x, Limit1 0x%08x\n", CBI.io_limit_0, CBI.io_limit_1);
  printk("Bridge Ctrl %4x,Interrupt Pin %2x,Line %2x\n", CBI.bridge_ctrl,
         CBI.bridge_interrupt_pin, CBI.bridge_line);
  printk(
      "PCCARD Base Address %08x,Sys Ctrl %08X,Card Ctrl %02X,Dev Ctrl %02X\n",
      CBI.pccard_base_address, CBI.sys_ctrl, CBI.card_ctrl, CBI.dev_ctrl);
  printk("Diag reg 0x%02x\n", CBI.diag_reg);
  printk("Virtual ExCA Base Address %08X\n", CBI.v_exca_base_address);
}

int TestPCICardBusInit(void)
{
  printk("igs-pccard: TestPCICardBusInit\n");
  GetCardBusInfo();
  if (!SetCardBusInfo())
    return 0;
  GetCardBusInfo();
  DisplayCardInfo();
  printk("ExCA %x,MEM %x\n", CBI.exca_base_address, CBI.mem_base_0);
  printk("ExCA %x,MEM %x\n", CBI.v_exca_base_address, CBI.v_exca_mem_base);
  SetExCAInfo();

  pcCmdPort = (char *)CBI.v_exca_mem_base + 0x800000;
  pcIntPort = (char *)CBI.v_exca_mem_base + 0xA00000;
  pcClrIntPort = (char *)CBI.v_exca_mem_base + 0xC00000;
  ulMemBase = CBI.v_exca_mem_base;
  pcClrExCAIntPort = (char *)(CBI.v_exca_base_address + 0x804);
  ucCmdPortVal = *(unsigned char *)(CBI.v_exca_mem_base + 0x800000);
  bINT = 0;
  return 1;
}

void DisplayExCARegisters(void)
{
  unsigned int v_exca_base_Address; // esi
  int i;                            // ebx
  int j;                            // ebx
  int v5;                           // [esp+0h] [ebp-24h]
  int v6;                           // [esp+0h] [ebp-24h]
  int v7;                           // [esp+4h] [ebp-20h]
  int v8;                           // [esp+4h] [ebp-20h]

  v_exca_base_Address = CBI.v_exca_base_address;
  printk("igs-pccard: CardBus Socket Registers\n");
  for (i = 0; i <= 32; ++i)
  {
    if (i > 0 && !(i % 10))
      printk("\n");
    v7 = *(unsigned char *)(i + v_exca_base_Address);
    v5 = i;
    printk("igs-pccard: %03X:%02X ", v5, v7);
  }
  printk("\nigs-pccard: Display ExCA Registers\n");
  for (j = 0; j <= 68; ++j)
  {
    if (j > 0 && !(j % 10))
      printk("\n");
    v8 = *(unsigned char *)(j + v_exca_base_Address + 0x800);
    v6 = j + 0x800;
    printk("igs-pccard: %03X:%02X ", v6, v8);
  }
}

void ResetExCA(void)
{
  char exca_value = PCCARD_READ8(CBI.v_exca_base_address + 0x803);
  printk("igs-pccard: ResetExCA\n");
  PCCARD_WRITE8(CBI.v_exca_base_address + 0x803, exca_value | 0x40);
  mdelay(20);
  PCCARD_WRITE8(CBI.v_exca_base_address + 0x803, exca_value & 0xBF);
}

int pccard_init(void)
{
  int err;

  m_ulReadMissTimes = 0;
  // pccard_i_fops = 0;

  if (FindPCIDevice())
  {
    if (!TestPCICardBusInit())
      return -1;
    DisplayExCARegisters();
    ResetExCA();
    if (pccard_startup_probe)
      pccard_probe_poke();
  }
  else
    return -1;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
  err = alloc_chrdev_region(&pccard_devno, 0, 1, "pccard");
  if (err < 0)
  {
    printk("igs-pccard: <6>pccard: can't allocate char device number (%d)\n", err);
    return err;
  }
  pccard_major = MAJOR(pccard_devno);
  cdev_init(&pccard_cdev, &pccard_i_fops);
  pccard_cdev.owner = THIS_MODULE;
  err = cdev_add(&pccard_cdev, pccard_devno, 1);
  if (err < 0)
  {
    printk("igs-pccard: <6>pccard: can't add char device (%d)\n", err);
    unregister_chrdev_region(pccard_devno, 1);
    pccard_major = 0;
    return err;
  }
  pccard_cdev_registered = 1;
#else
  pccard_major = register_chrdev(0, "pccard", &pccard_i_fops);
#endif
  if (pccard_major >= 0)
  {
    printk("\nigs-pccard: major %x\n", pccard_major);
    if (CBI.bridge_line && pccard_irq < 0)
      pccard_irq = CBI.bridge_line;
    if (pccard_use_irq && pccard_irq >= 0)
    {
      if (request_irq(pccard_irq, pccard_interrupt, IRQF_SHARED, "pccard",
                      pccard_interrupt))
      {
        printk("igs-pccard: can't get assigned irq %i\n", pccard_irq);
        pccard_irq = -1;
      }
      else
      {
        pccard_irq_registered = 1;
        printk("igs-pccard: IRQ set to %d\n", pccard_irq);
      }
    }
    else if (!pccard_use_irq)
    {
      printk("igs-pccard: IRQ handler disabled; using 2.4-style polling\n");
    }
    else
    {
      printk("igs-pccard: no CardBus IRQ available\n");
    }
    return 0;
  }
  else
  {
    printk("igs-pccard: <6>can't get major number\n");
    return pccard_major;
  }
}

void pccard_cleanup(void)
{

  if (pccard_irq_registered)
  {
    free_irq(pccard_irq, pccard_interrupt);
    pccard_irq_registered = 0;
  }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
  if (pccard_cdev_registered)
  {
    cdev_del(&pccard_cdev);
    unregister_chrdev_region(pccard_devno, 1);
    pccard_cdev_registered = 0;
  }
#else
  unregister_chrdev(pccard_major, "pccard");
#endif
  printk("igs-pccard: unmap ExCA:%x,MEM:%x\n", CBI.v_exca_base_address,
         CBI.v_exca_mem_base);
  if (CBI.v_exca_base_address)
  {
    iounmap((void *)CBI.v_exca_base_address);
  }

  if (CBI.v_exca_mem_base)
  {
    iounmap((void *)CBI.v_exca_mem_base);
  }

  if (pccard_mmio_allocated)
  {
    release_resource(&pccard_mmio_resource);
    pccard_mmio_allocated = 0;
  }
}

module_init(pccard_init);
module_exit(pccard_cleanup);
MODULE_LICENSE("GPL");
module_param(pccard_use_irq, int, 0644);
MODULE_PARM_DESC(
    pccard_use_irq,
    "Enable runtime IRQ handler; default off to match the 2.4 driver");
module_param(pccard_8bit_windows, int, 0644);
MODULE_PARM_DESC(
    pccard_8bit_windows,
    "Use 8-bit ExCA memory windows; default off to match the 2.4 driver");
module_param(pccard_startup_probe, int, 0644);
MODULE_PARM_DESC(pccard_startup_probe,
                 "Send the legacy startup probe sequence after reset; "
                 "default on for Linux 2.6+");
module_param(pccard_reset_on_open, int, 0644);
MODULE_PARM_DESC(pccard_reset_on_open,
                 "Reset and probe the card on each device open; default off "
                 "to match the game");
module_param(pccard_accept_valid_f1, int, 0644);
MODULE_PARM_DESC(pccard_accept_valid_f1,
                 "Translate status 0xf1 to the current command value when "
                 "the RF4 response checksum is valid");
module_param(pccard_debug, int, 0644);
MODULE_PARM_DESC(pccard_debug, "Enable verbose protocol debug logging");
module_param(pccard_trace, int, 0644);
MODULE_PARM_DESC(pccard_trace,
                 "Enable compact read/write protocol trace logging");
module_param(pccard_start_retries, int, 0644);
MODULE_PARM_DESC(
    pccard_start_retries,
    "Number of delayed retries for the offset-254 start command");
module_param(pccard_start_retry_delay_ms, int, 0644);
MODULE_PARM_DESC(pccard_start_retry_delay_ms,
                 "Delay before retrying the offset-254 start command");
module_param(pccard_parent_window_fallback, int, 0644);
MODULE_PARM_DESC(pccard_parent_window_fallback,
                 "Use a parent PCI bridge memory window when the legacy "
                 "cb000000 aperture is unavailable");
module_param(pccard_prefer_parent_window, int, 0644);
MODULE_PARM_DESC(pccard_prefer_parent_window,
                 "Prefer Linux's parent bridge memory window over the legacy "
                 "cb000000 aperture on newer kernels");
