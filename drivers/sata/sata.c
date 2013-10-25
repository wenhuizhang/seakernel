#include <kernel.h>
#include <module.h>
#include <pmap.h>
#include <mutex.h>
#include <memory.h>
#include <types.h>
#include <modules/sata.h>
#include <modules/pci.h>
#include <asm/system.h>
#include <isr.h>
#include <block.h>
#include <symbol.h>
struct pci_device *sata_pci;
int ahci_int = 0;
struct hba_memory *hba_mem;
struct pmap *sata_pmap;
struct ahci_device *ports[32];
int ahci_major=0;
struct pci_device *get_sata_pci()
{
	struct pci_device *sata = pci_locate_class(0x1, 0x6);
	if(!sata)
		return 0;
	sata->flags |= PCI_ENGAGED;
	sata->flags |= PCI_DRIVEN;
	hba_mem = (void *)(addr_t)sata->pcs->bar5;
	if(!(sata->pcs->command & 4))
		printk(0, "[sata]: setting PCI command to bus mastering mode\n");
	unsigned short cmd = sata->pcs->command | 4;
	sata->pcs->command = cmd;
	pci_write_dword(sata->bus, sata->dev, sata->func, 4, cmd);
	/* of course, we need to map a virtual address to physical address
	 * for paging to not hate on us... */
	hba_mem = (void *)pmap_get_mapping(sata_pmap, (addr_t)hba_mem);
	printk(0, "[sata]: mapping hba_mem to %x -> %x\n", hba_mem, sata->pcs->bar5);
	printk(0, "[sata]: using interrupt %d\n", sata->pcs->interrupt_line+32);
	ahci_int = sata->pcs->interrupt_line+32;
	return sata;
}

uint32_t ahci_flush_commands(struct hba_port *port)
{
	uint32_t c = port->command;
	c=c;
	return c;
}

void ahci_stop_port_command_engine(volatile struct hba_port *port)
{
	port->command &= ~HBA_PxCMD_ST;
	port->command &= ~HBA_PxCMD_FRE;
	while((port->command & HBA_PxCMD_CR) || (port->command & HBA_PxCMD_FR))
		asm("pause");
}

void ahci_start_port_command_engine(volatile struct hba_port *port)
{
	while(port->command & HBA_PxCMD_CR);
	port->command |= HBA_PxCMD_FRE;
	port->command |= HBA_PxCMD_ST; 
}

struct hba_command_header *ahci_initialize_command_header(struct hba_memory *abar, struct hba_port *port, struct ahci_device *dev, int slot, int write, int atapi, int prd_entries, int fis_len)
{
	struct hba_command_header *h = (struct hba_command_header *)dev->clb_virt;
	h += slot;
	h->write=write ? 1 : 0;
	h->prdb_count=0;
	h->atapi=atapi ? 1 : 0;
	h->fis_length = fis_len;
	h->prdt_len=prd_entries;
	h->prefetchable=0;
	h->bist=0;
	h->pmport=0;
	h->reset=0;
	return h;
}

struct fis_reg_host_to_device *ahci_initialize_fis_host_to_device(struct hba_memory *abar, struct hba_port *port, struct ahci_device *dev, int slot, int cmdctl, int ata_command)
{
	struct hba_command_table *tbl = (struct hba_command_table *)(dev->ch[slot]);
	struct fis_reg_host_to_device *fis = (struct fis_reg_host_to_device *)(tbl->command_fis);
	
	memset(fis, 0, sizeof(*fis));
	fis->fis_type = FIS_TYPE_REG_H2D;
	fis->command = ata_command;
	fis->c=cmdctl?1:0;
	return fis;
}

void ahci_send_command(struct hba_port *port, int slot)
{
	port->interrupt_status = ~0;
	port->command_issue = (1 << slot);
	ahci_flush_commands(port);
}

int ahci_write_prdt(struct hba_memory *abar, struct hba_port *port, struct ahci_device *dev, int slot, int offset, int length, addr_t virt_buffer)
{
	int num_entries = ((length-1) / PRDT_MAX_COUNT) + 1;
	struct hba_command_table *tbl = (struct hba_command_table *)(dev->ch[slot]);
	int i;
	struct hba_prdt_entry *prd;
	for(i=0;i<num_entries-1;i++)
	{
		addr_t phys_buffer = vm_do_getmap(virt_buffer, 0, 0);
		prd = &tbl->prdt_entries[i+offset];
		prd->byte_count = PRDT_MAX_COUNT-1;
		prd->data_base_l = phys_buffer & 0xFFFFFFFF;
		prd->data_base_h = UPPER32(phys_buffer);
		prd->interrupt_on_complete=0;
		
		length -= PRDT_MAX_COUNT;
		virt_buffer += PRDT_MAX_COUNT;
	}
	addr_t phys_buffer = vm_do_getmap(virt_buffer, 0, 0);
	prd = &tbl->prdt_entries[i+offset];
	prd->byte_count = length-1;
	prd->data_base_l = phys_buffer & 0xFFFFFFFF;
	prd->data_base_h = UPPER32(phys_buffer);
	prd->interrupt_on_complete=0;
	
	return num_entries;
}

int ahci_reset_device(struct hba_memory *abar, struct hba_port *port)
{
#warning "implement"
}

int ahci_port_dma_data_transfer(struct hba_memory *abar, struct hba_port *port, struct ahci_device *dev, int slot, int write, addr_t virt_buffer, int sectors, uint64_t lba)
{
	port->interrupt_status = ~0;
	int fis_len = sizeof(struct fis_reg_host_to_device) / 4;
	int ne = ahci_write_prdt(abar, port, dev, slot, 0, ATA_SECTOR_SIZE * sectors, virt_buffer);
	struct hba_command_header *h = ahci_initialize_command_header(abar, port, dev, slot, write, 0, ne, fis_len);
	struct fis_reg_host_to_device *fis = ahci_initialize_fis_host_to_device(abar, port, dev, slot, 1, write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX);
	fis->device = 1<<6;
	fis->count_l = sectors & 0xFF;
	fis->count_h = (sectors >> 8) & 0xFF;
	
	fis->lba0 = (unsigned char)( lba        & 0xFF);
	fis->lba1 = (unsigned char)((lba >> 8)  & 0xFF);
	fis->lba2 = (unsigned char)((lba >> 16) & 0xFF);
	fis->lba3 = (unsigned char)((lba >> 24) & 0xFF);
	fis->lba4 = (unsigned char)((lba >> 32) & 0xFF);
	fis->lba5 = (unsigned char)((lba >> 40) & 0xFF);
	
	while ((port->task_file_data & (ATA_DEV_BUSY | ATA_DEV_DRQ)))
	{
		asm("pause");
	}
	
	ahci_send_command(port, slot);
	
	while ((port->task_file_data & (ATA_DEV_BUSY | ATA_DEV_DRQ)))
	{
		asm("pause");
	}
	
	while(1)
	{
		if(!((port->sata_active | port->command_issue) & (1 << slot)))
			break;
	}
	if(port->sata_error)
	{
		printk(0, "[sata]: device %d: sata error: %x\n", dev->idx, port->sata_error);
		goto error;
	}
	if(port->task_file_data & ATA_DEV_ERR)
	{
		printk(0, "[sata]: device %d returned task file data error\n", dev->idx);
		goto error;
	}
	return 1;
	error:
	ahci_reset_device(abar, port);
	return 0;
}

uint32_t ahci_get_previous_byte_count(struct hba_memory *abar, struct hba_port *port, struct ahci_device *dev, int slot)
{
	struct hba_command_header *h = (struct hba_command_header *)dev->clb_virt;
	h += slot;
	return h->prdb_count;
}

void ahci_device_identify_sata(struct hba_memory *abar, struct hba_port *port, struct ahci_device *dev)
{
	int fis_len = sizeof(struct fis_reg_host_to_device) / 4;
	unsigned short *buf = kmalloc_a(0x1000);
	ahci_write_prdt(abar, port, dev, 0, 0, 512, (addr_t)buf);
	struct hba_command_header *h = ahci_initialize_command_header(abar, port, dev, 0, 0, 0, 1, fis_len);
	struct fis_reg_host_to_device *fis = ahci_initialize_fis_host_to_device(abar, port, dev, 0, 1, ATA_CMD_IDENTIFY);
	
	while ((port->task_file_data & (ATA_DEV_BUSY | ATA_DEV_DRQ)))
	{
		asm("pause");
	}
	ahci_send_command(port, 0);
	
	while(1)
	{
		if(!((port->sata_active | port->command_issue) & 1))
			break;
	}
	memcpy(&dev->identify, buf, sizeof(struct ata_identify));
	kfree(buf);
	printk(2, "[sata]: device %d: num sectors=%d: %x, %x\n", dev->idx, dev->identify.lba48_addressable_sectors, dev->identify.ss_2);
}

void ahci_initialize_device(struct hba_memory *abar, struct ahci_device *dev)
{
	printk(0, "[sata]: initializing device %d\n", dev->idx);
	struct hba_port *port = (struct hba_port *)&abar->ports[dev->idx];
	ahci_stop_port_command_engine(port);
	
	/* power on, spin up */
	port->command |= 6;
	/* initialize state */
	port->interrupt_status = ~0; /* clear pending interrupts */
	port->interrupt_enable = ~0; /* we want some interrupts */
	
	port->command |= (1 << 28); /* set interface to active */
	port->command &= ~((1 << 27) | (1 << 26)); /* clear some bits */
	/* TODO: Do we need a delay here? */
	/* map memory */
	addr_t clb_phys, fis_phys;
	void *clb_virt, *fis_virt;
	clb_virt = kmalloc_ap(0x2000, &clb_phys);
	fis_virt = kmalloc_ap(0x1000, &fis_phys);
	dev->clb_virt = clb_virt;
	dev->fis_virt = fis_virt;
	
	struct hba_command_header *h = (struct hba_command_header *)clb_virt;
	int i;
	for(i=0;i<HBA_COMMAND_HEADER_NUM;i++) {
		addr_t phys;
		dev->ch[i] = kmalloc_ap(0x1000, &phys);
		memset(h, 0, sizeof(*h));
		h->command_table_base_l = phys & 0xFFFFFFFF;
		h->command_table_base_h = UPPER32(phys);
		h++;
	}
	
	port->command_list_base_l = (clb_phys & 0xFFFFFFFF);
	port->command_list_base_h = UPPER32(clb_phys);
	
	port->fis_base_l = (fis_phys & 0xFFFFFFFF);
	port->fis_base_h = UPPER32(fis_phys);
	
 	ahci_start_port_command_engine(port);
	ahci_device_identify_sata(abar, port, dev);
}

uint32_t ahci_check_type(volatile struct hba_port *port)
{
	uint32_t s = port->sata_status;
	uint8_t ipm, det;
	ipm = (s >> 8) & 0x0F;
	det = s & 0x0F;
	/* TODO: Where do these numbers come from? */
	if(ipm != 1 || det != 3)
		return 0;
	return port->signature;
}

int read_partitions(struct ahci_device *dev, char *node, int port)
{
	addr_t p = find_kernel_function("enumerate_partitions");
	if(!p)
		return 0;
	int d = GETDEV(ahci_major, port);
	int (*e_p)(int, int, struct partition *);
	e_p = (int (*)(int, int, struct partition *))p;
	struct partition part;
	int i=0;
	while(i<64)
	{
		/* Returns the i'th partition of device 'd' into info struct part. */
		int r = e_p(i, d, &part);
		if(!r)
			break;
		if(part.sysid)
		{
			printk(0, "[sata]: %d: read partition start=%d, len=%d\n", port, part.start_lba, part.length);
			int a = port;
			char tmp[17];
			memset(tmp, 0, 17);
			sprintf(tmp, "%s%d", node, i+1);
			devfs_add(devfs_root, tmp, S_IFBLK, ahci_major, a+(i+1)*32);
		}
		memcpy(&(dev->part[i]), &part, sizeof(struct partition));
		i++;
	}
	
	return 0;
}

void ahci_create_device(struct ahci_device *dev)
{
	char node[16];
	char c = 'a';
	if(dev->idx > 25) c = 'A';
	sprintf(node, "sd%c", (dev->idx % 26) + c);
	dev->node = devfs_add(devfs_root, node, S_IFBLK, ahci_major, dev->idx);
	read_partitions(dev, node, dev->idx);
}

void ahci_probe_ports(struct hba_memory *abar)
{
	uint32_t pi = abar->port_implemented;
	int i=0;
	while(i < 32)
	{
		if(pi & 1)
		{
			uint32_t type = ahci_check_type(&abar->ports[i]);
			if(type) {
				ports[i] = kmalloc(sizeof(struct ahci_device));
				ports[i]->type = type;
				ports[i]->idx = i;
				mutex_create(&(ports[i]->lock), 0);
				ahci_initialize_device(abar, ports[i]);
				ahci_create_device(ports[i]);
			}
		}
		i++;
		pi >>= 1;
	}
}

void ahci_init_hba(struct hba_memory *abar)
{
	/* enable the AHCI and reset it */
	abar->global_host_control |= HBA_GHC_AHCI_ENABLE;
	abar->global_host_control |= HBA_GHC_RESET;
	/* wait for reset to complete */
	while(abar->global_host_control & HBA_GHC_RESET) asm("pause");
	/* enable the AHCI and interrupts */
	abar->global_host_control |= HBA_GHC_AHCI_ENABLE;
	abar->global_host_control |= HBA_GHC_INTERRUPT_ENABLE;
}

void ahci_interrupt_handler()
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

/* since a DMA transfer must write to contiguous physical RAM, we need to allocate
 * buffers that allow us to create PRDT entries that do not cross a page boundary.
 * That means that each PRDT entry can transfer a maximum of PAGE_SIZE bytes (for
 * 0x1000 page size, that's 8 sectors). Thus, we allocate a buffer that is page aligned, 
 * in a multiple of PAGE_SIZE, so that the PRDT will write to contiguous physical ram
 * (the key here is that the buffer need not be contiguous across multiple PRDT entries).
 */
int ahci_rw_multiple_do(int rw, int min, u64 blk, char *out_buffer, int count)
{
	uint32_t length = count * ATA_SECTOR_SIZE;
	
	int d = min % 32;
	int p = min / 32;
	struct ahci_device *dev = ports[d];
	uint32_t part_off=0, part_len=0;
	u64 end_blk = dev->identify.lba48_addressable_sectors;
	if(p > 0) {
		part_off = dev->part[p-1].start_lba;
		part_len = dev->part[p-1].length;
		end_blk = part_len + part_off;
	}
	blk += part_off;
	
	if(blk >= end_blk)
		return 0;
	
	if((blk+count) > end_blk)
		count = end_blk - blk;
	
	if(!count)
		return 0;
	
	int num_pages = ((ATA_SECTOR_SIZE * (count-1)) / PAGE_SIZE) + 1;
	assert(length <= (unsigned)num_pages * 0x1000);
	unsigned char *buf = kmalloc_a(0x1000 * num_pages);
	
	int num_read_blocks = count;
	
	if(rw == WRITE)
		memcpy(buf, out_buffer, length);
	
	mutex_acquire(&dev->lock);
	
	struct hba_port *port = (struct hba_port *)&hba_mem->ports[dev->idx];
	int slot=0;
	if(!ahci_port_dma_data_transfer(hba_mem, port, dev, slot, rw == WRITE ? 1 : 0, (addr_t)buf, count, blk))
		num_read_blocks = 0;
	
	mutex_release(&dev->lock);
	
	if(rw == READ && num_read_blocks)
		memcpy(out_buffer, buf, length);
	
	kfree(buf);
	return num_read_blocks * ATA_SECTOR_SIZE;
}

/* and then since there is a maximum transfer amount because of the page size
 * limit, wrap the transfer function to allow for bigger transfers than that even.
 */
int ahci_rw_multiple(int rw, int min, u64 blk, char *out_buffer, int count)
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

int ahci_rw_single(int rw, int dev, u64 blk, char *buf)
{
	return ahci_rw_multiple_do(rw, dev, blk, buf, 1);
}

int ioctl_ahci(int min, int cmd, long arg)
{
	return -EINVAL;
}

int irq1;
int module_install()
{
	printk(0, "[sata]: initializing sata driver...\n");
	sata_pmap = pmap_create(0, 0);
	if(!(sata_pci = get_sata_pci()))
	{
		printk(0, "[sata]: no AHCI controllers present!\n");
		pmap_destroy(sata_pmap);
		return -ENOENT;
	}
	irq1 = register_interrupt_handler(ahci_int, ahci_interrupt_handler, 0);
	ahci_major = set_availablebd(ahci_rw_single, ATA_SECTOR_SIZE, ioctl_ahci, ahci_rw_multiple, 0);
	ahci_init_hba(hba_mem);
	ahci_probe_ports(hba_mem);
	return 0;
}

int module_deps(char *b)
{
	write_deps(b, "pci,:");
	return KVERSION;
}

int module_exit()
{
	int i;
	unregister_block_device(ahci_major);
	unregister_interrupt_handler(ahci_int, irq1);
	for(i=0;i<32;i++)
	{
		if(ports[i]) {
			mutex_destroy(&(ports[i]->lock));
			kfree(ports[i]->clb_virt);
			kfree(ports[i]->fis_virt);
			for(int j=0;j<HBA_COMMAND_HEADER_NUM;j++)
				kfree(ports[i]->ch[j]);
			kfree(ports[i]);
		}
	}
	pmap_destroy(sata_pmap);
	sata_pci->flags = 0;
	return 0;
}