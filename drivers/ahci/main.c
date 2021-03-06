#include <sea/kernel.h>
#include <sea/loader/module.h>
#include <sea/mutex.h>
#include <sea/mm/vmm.h>
#include <sea/types.h>
#include <modules/ahci.h>
#include <modules/pci.h>
#include <sea/asm/system.h>
#include <sea/cpu/interrupt.h>
#include <sea/dm/block.h>
#include <sea/loader/symbol.h>
#include <modules/psm.h>
#include <sea/tm/timing.h>
#include <sea/cpu/interrupt.h>
#include <sea/vsprintf.h>
#include <sea/errno.h>
#include <sea/dm/blockdev.h>
#include <sea/mm/kmalloc.h>

struct pci_device *ahci_pci;
int ahci_int = 0;
struct hba_memory *hba_mem;
struct ahci_device *ports[32];

struct pci_device *get_ahci_pci (void)
{
	struct pci_device *ahci = pci_locate_class(0x1, 0x6);
	if(!ahci) ahci = pci_locate_device(0x8086, 0x8c03);
	if(!ahci) ahci = pci_locate_device(0x8086, 0x2922);
	if(!ahci)
		return 0;
	ahci->flags |= PCI_ENGAGED;
	ahci->flags |= PCI_DRIVEN;
	hba_mem = (void *)(addr_t)ahci->pcs->bar5;
	if(!(ahci->pcs->command & 4))
		printk(KERN_DEBUG, "[ahci]: setting PCI command to bus mastering mode\n");
	unsigned short cmd = ahci->pcs->command | 4;
	ahci->pcs->command = cmd;
	pci_write_dword(ahci->bus, ahci->dev, ahci->func, 4, cmd);
	/* of course, we need to map a virtual address to physical address
	 * for paging to not hate on us... */
	hba_mem = (void *)((addr_t)hba_mem + PHYS_PAGE_MAP);
	printk(KERN_DEBUG, "[ahci]: mapping hba_mem to %x -> %x\n", hba_mem, ahci->pcs->bar5);
	printk(KERN_DEBUG, "[ahci]: using interrupt %d\n", ahci->pcs->interrupt_line+32);
	ahci_int = ahci->pcs->interrupt_line+32;
	return ahci;
}

void ahci_interrupt_handler(struct registers *regs, int int_no, int flags)
{
	int i;
	for(i=0;i<32;i++) {
		if(hba_mem->interrupt_status & (1 << i)) {
			hba_mem->ports[i].interrupt_status = ~0;
			hba_mem->interrupt_status = (1 << i);
			ahci_flush_commands((struct hba_port *)&hba_mem->ports[i]);
		}
	}
}

int ahci_port_acquire_slot(struct ahci_device *dev)
{
	while(1) {
		int i;
		mutex_acquire(&dev->lock);
		for(i=0;i<32;i++)
		{
			if(!(dev->slots & (1 << i))) {
				dev->slots |= (1 << i);
				mutex_release(&dev->lock);
				return i;
			}
		}
		mutex_release(&dev->lock);
		tm_schedule();
	}
}

void ahci_port_release_slot(struct ahci_device *dev, int slot)
{
	mutex_acquire(&dev->lock);
	dev->slots &= ~(1 << slot);
	mutex_release(&dev->lock);
}

/* since a DMA transfer must write to contiguous physical RAM, we need to allocate
 * buffers that allow us to create PRDT entries that do not cross a page boundary.
 * That means that each PRDT entry can transfer a maximum of PAGE_SIZE bytes (for
 * 0x1000 page size, that's 8 sectors). Thus, we allocate a buffer that is page aligned, 
 * in a multiple of PAGE_SIZE, so that the PRDT will write to contiguous physical ram
 * (the key here is that the buffer need not be contiguous across multiple PRDT entries).
 */
int ahci_rw_multiple_do(int rw, int min, uint64_t blk, unsigned char *out_buffer, int count)
{
	uint32_t length = count * ATA_SECTOR_SIZE;
	int d = min;
	struct ahci_device *dev = ports[d];
	uint64_t end_blk = dev->identify.lba48_addressable_sectors;
	if(blk >= end_blk)
		return 0;
	if((blk+count) > end_blk)
		count = end_blk - blk;
	if(!count)
		return 0;
	int num_pages = ((ATA_SECTOR_SIZE * (count-1)) / PAGE_SIZE) + 1;
	assert(length <= (unsigned)num_pages * 0x1000);
	struct dma_region dma;
	dma.p.size = 0x1000 * num_pages;
	dma.p.alignment = 0x1000;
	mm_allocate_dma_buffer(&dma);
	int num_read_blocks = count;
	struct hba_port *port = (struct hba_port *)&hba_mem->ports[dev->idx];
	if(rw == WRITE)
		memcpy((void *)dma.v, out_buffer, length);
	
	int slot=ahci_port_acquire_slot(dev);
	if(!ahci_port_dma_data_transfer(hba_mem, port, dev, slot, rw == WRITE ? 1 : 0, (addr_t)dma.v, count, blk))
		num_read_blocks = 0;
	
	ahci_port_release_slot(dev, slot);
	
	if(rw == READ && num_read_blocks)
		memcpy(out_buffer, (void *)dma.v, length);
	
	mm_free_dma_buffer(&dma);
	return num_read_blocks * ATA_SECTOR_SIZE;
}

/* and then since there is a maximum transfer amount because of the page size
 * limit, wrap the transfer function to allow for bigger transfers than that even.
 */
int ahci_rw_multiple(int rw, int min, uint64_t blk, unsigned char *out_buffer, int count)
{
	int i=0;
	int ret=0;
	int c = count;
	for(i=0;i<count;i+=(PRDT_MAX_ENTRIES * PRDT_MAX_COUNT) / ATA_SECTOR_SIZE)
	{
		int n = (PRDT_MAX_ENTRIES * PRDT_MAX_COUNT) / ATA_SECTOR_SIZE;
		if(n > c)
			n=c;
		ret += ahci_rw_multiple_do(rw, min, blk+i, out_buffer + ret, n);
		c -= n;
	}
	return ret;
}

struct hash portmap;
static ssize_t __rw(int rw, struct inode *node, uint64_t block, uint8_t *buffer, size_t len)
{
	int min = MINOR(node->phys_dev);
	int port = (long)hash_lookup(&portmap, &min, sizeof(min));
	return ahci_rw_multiple(rw, port, block, buffer, len);
}

void ahci_create_device(struct ahci_device *dev)
{
	dev->created=1;
	char name[32];
	snprintf(name, 32, "/dev/ad%c", dev->idx+'a'); /* TODO: better naming scheme */
	sys_mknod(name, S_IFBLK | 0600, 0);
	int err;
	struct inode *node = fs_path_resolve_inode(name, 0, &err);
	node->flags |= INODE_PERSIST;
	dev->bctl.rw = __rw;
	dev->bctl.ioctl = NULL;
	dev->bctl.select = NULL;
	dev->bctl.blocksize = 512;
	int min = blockdev_register(node, &dev->bctl);
	vfs_icache_put(node);
	dev->minor = min;
	hash_insert(&portmap, &dev->minor, sizeof(dev->minor), &dev->mapelem, (void *)(long)dev->idx);
}

int irq;
int module_install(void)
{
	hash_create(&portmap, 0, 32);
	printk(KERN_DEBUG, "[ahci]: initializing ahci driver...\n");
	if(!(ahci_pci = get_ahci_pci()))
	{
		printk(KERN_DEBUG, "[ahci]: no AHCI controllers present!\n");
		return -ENOENT;
	}
	irq = cpu_interrupt_register_handler(ahci_int, ahci_interrupt_handler);
	ahci_init_hba(hba_mem);
	ahci_probe_ports(hba_mem);
	return 0;
}

int module_exit(void)
{
	int i;
	cpu_interrupt_unregister_handler(ahci_int, irq);
	for(i=0;i<32;i++)
	{
		if(ports[i]) {
			mutex_destroy(&(ports[i]->lock));
			mm_free_dma_buffer(&(ports[i]->dma_clb));
			mm_free_dma_buffer(&(ports[i]->dma_fis));
			for(int j=0;j<HBA_COMMAND_HEADER_NUM;j++)
				mm_free_dma_buffer(&(ports[i]->ch_dmas[j]));
			kfree(ports[i]);
		}
	}
	ahci_pci->flags = 0;
	return 0;
}

