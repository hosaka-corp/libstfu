/* starlet.c - libstfu emulator core
 */

#include <unicorn/unicorn.h>
#include <string.h>
#include <endian.h>

#include "core_types.h"
#include "starlet.h"
#include "util.h"

#define DEBUG true
#define LOGGING true

// __init_mmu()
// Initialize various memory mappings
// UC_PROT_ALL=7
static int __init_mmu(starlet *e)
{
	// Main memory
	e->mram = malloc(sizeof(mram));
	uc_mem_map_ptr(e->uc, 0x00000000, 0x01800000, 7, e->mram->mem1);
	uc_mem_map_ptr(e->uc, 0x10000000, 0x04000000, 7, e->mram->mem2);

	// MMIOs
	uc_mem_map_ptr(e->uc, 0x0d010000, 0x00000400, 7, e->iomem.nand);
	uc_mem_map_ptr(e->uc, 0x0d020000, 0x00000400, 7, e->iomem.aes);
	uc_mem_map_ptr(e->uc, 0x0d030000, 0x00000400, 7, e->iomem.sha);

	uc_mem_map_ptr(e->uc, 0x0d806000, 0x00000400, 7, e->iomem.exi);
	uc_mem_map_ptr(e->uc, 0x0d800000, 0x00000400, 7, e->iomem.hlwd);
	uc_mem_map_ptr(e->uc, 0x0d8b0000, 0x00000400, 7, e->iomem.mem_unk);
	uc_mem_map_ptr(e->uc, 0x0d8b4000, 0x00000400, 7, e->iomem.ddr);

	// [Initial] mappings for SRAM and BROM
	uc_mem_map_ptr(e->uc, 0xffff0000, 0x00010000, 7, e->sram.brom);
	uc_mem_map_ptr(e->uc, 0xfffe0000, 0x00010000, 7, e->sram.bank_a);
	uc_mem_map_ptr(e->uc, 0xfff00000, 0x00010000, 7, e->sram.bank_a);
	uc_mem_map_ptr(e->uc, 0x0d400000, 0x00010000, 7, e->sram.bank_a);
	uc_mem_map_ptr(e->uc, 0x0d410000, 0x00010000, 7, e->sram.bank_b);
	uc_mem_map_ptr(e->uc, 0xfff10000, 0x00010000, 7, e->sram.bank_b);
	return 0;
}

// __destroy_mmu()
// Free any backing memory we allocated.
static int __destroy_mmu(starlet *emu) 
{ 
	free(emu->mram);
	return 0; 
}

// __hook_unmapped()
// Fired on UC_HOOK_MEM_UNMAPPED events.
static bool __hook_unmapped(uc_engine *uc, uc_mem_type type,
	u64 address, int size, s64 value, void *user_data)
{
	switch(type){
	case UC_MEM_WRITE_UNMAPPED:
		printf("Unmapped write on %08x\n", address);
		return false;
	case UC_MEM_READ_UNMAPPED:
		printf("Unmapped read on %08x\n", address);
		return false;
	case UC_MEM_FETCH_UNMAPPED:
		printf("Unmapped fetch on %08x\n", address);
		return false;
	}
	return false;
}

// __hook_timer()
// Fired after successful read on HW_TIMER (does this actually work?)
static bool __hook_timer(uc_engine *uc, uc_mem_type type,
	u64 address, int size, s64 value, starlet *emu)
{
	*(u32*)&emu->iomem.hlwd[0x10] += 10;
	log("HW_TIMER=%08x\n", *(u32*)&emu->iomem.hlwd[0x10]);
}

// __register_hooks()
// Register all default hooks necessary for emulation.
// This includes: Unicorn exception handlers, MMIO emulation, etc.
static int __register_hooks(starlet *e)
{
	uc_hook x, y, z;
	uc_hook_add(e->uc, &x,UC_HOOK_MEM_UNMAPPED,__hook_unmapped, NULL,1,0);
	uc_hook_add(e->uc, &y,UC_HOOK_MEM_READ_AFTER,__hook_timer, e, 
			0x0d800010, 0x0d800010);
	register_mmio_hooks(e);
}



// ----------------------------------------------------------------------------
// These are the functions exposed to users linking against libstfu


// starlet_destroy()
// Destroy a Starlet instance.
void starlet_destroy(starlet *emu)
{ 
	dbg("%s\n", "destroying instance ...");
	uc_close(emu->uc); 
	__destroy_mmu(emu);
	if (emu->nand.data)
		free(emu->nand.data);
}

// starlet_init()
// Initialize a new starlet instance.
int starlet_init(starlet *emu)
{
	uc_err err;
	err = uc_open(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_BIG_ENDIAN, &emu->uc);
	if (err)
	{
		printf("Couldn't create Unicorn instance\n");
		return -1;
	}

	// Configure initial memory mappings
	__init_mmu(emu);
	__register_hooks(emu);
	dbg("%s\n", "initialized instance");
}

// starlet_halt()
// Halt a Starlet instance with the provided reason.
int starlet_halt(starlet *emu, u32 why)
{
	emu->halt_code = why;
	uc_emu_stop(emu->uc);
}

// starlet_run()
// Start running a Starlet instance. The main loop is implemented here.
#define LOOP_INSTRS 0x100
int starlet_run(starlet *emu)
{
	uc_err err;
	u32 pc, cpsr;
	u32 temp;

	// Set the initial entrypoint
	uc_reg_write(emu->uc, UC_ARM_REG_PC, &emu->entrypoint);

	// Do the main emulation loop; break out on errors
	while (true)
	{
		// The PC we read from Unicorn does not encode THUMB state.
		// If the processor is in THUMB mode, fix the program counter.

		uc_reg_read(emu->uc, UC_ARM_REG_PC, &pc);
		uc_reg_read(emu->uc, UC_ARM_REG_CPSR, &cpsr);
		if (cpsr & 0x20) pc |= 1;

		log("pc=%08x\n", pc);
		// Let Unicorn emulate for some number of instructions.
		// I don't know how efficient this is ...

		err = uc_emu_start(emu->uc, pc, 0, 100, 0);

		// Handle any Unicorn-specific exceptions here.
		// If there are none, move onto the halt-code check.

		switch (err) {
		case UC_ERR_OK: break;
		default:
			emu->halt_code = err;
			break;
		}

		// Both Unicorn exceptions and hooks are expected to set the
		// halt-code when emulation has stopped for some reason.
		// When a halt-code is non-zero, we terminate the main loop.

		if (emu->halt_code)
		{
			dbg("Halt code %08x\n", emu->halt_code);
			return emu->halt_code;
		}

		// Since we expect that MMIO hooks immediately perform some
		// actions on control register writes, for now this is the
		// earliest point where we can unset busy bits on registers.

		temp = htobe32(*(u32*)&emu->iomem.nand[0x00]);
		if (temp & 0x80000000)
		{
			dbg("%s\n", "mainloop cleared NAND busy");
			*(u32*)&emu->iomem.nand[0x00] = temp & 0x7fffffff;
		}

		
	}
}

// starlet_load_code()
// Read a file with some code into memory, then write it into the emulator
// at the requested memory address. 
// This sets the Starlet entrypoint to the provided address.
int starlet_load_code(starlet *emu, char *filename, u64 addr)
{
	FILE *fp;
	size_t bytes_read;
	uc_err err;

	// Die if we can't get the filesize
	size_t filesize = get_filesize(filename); 
	if (filesize == -1)
	{
		printf("Couldn't open %s\n", filename);
		return -1;
	}

	// Temporarily load onto the heap
	u8 *data = malloc(filesize);
	fp = fopen(filename, "rb");
	bytes_read = fread(data, 1, filesize, fp);
	fclose(fp);

	// Die if we can't read the whole file
	if (bytes_read != filesize)
	{
		printf("Couldn't read all bytes in %s\n", filename);
		free(data);
		return -1;
	}

	// Write code to the destination address in memory
	uc_mem_write(emu->uc, addr, data, bytes_read);

	// Only set the entrypoint here if boot0 isn't loaded
	if (emu->entrypoint != 0xffff0000)
		emu->entrypoint = addr;

	free(data);
	return 0;
}

// starlet_load_boot0()
// Load the boot ROM from a file..
int starlet_load_boot0(starlet *emu, char *filename)
{
	FILE *fp;
	size_t bytes_read;
	uc_err err;

	// Die if we can't get the filesize
	size_t filesize = get_filesize(filename); 
	if (filesize == -1)
	{
		printf("Couldn't open %s\n", filename);
		return -1;
	}

	// Temporarily load onto the heap
	u8 *data = malloc(filesize);
	fp = fopen(filename, "rb");
	bytes_read = fread(data, 1, filesize, fp);
	fclose(fp);

	// Die if we can't read the whole file
	if (bytes_read != filesize)
	{
		printf("Couldn't read all bytes in %s\n", filename);
		free(data);
		return -1;
	}

	uc_mem_write(emu->uc, 0xffff0000, data, bytes_read);
	emu->entrypoint = 0xffff0000;

	free(data);
	return 0;
}

// starlet_load_nand_buffer()
// Prepare the NAND flash interface with a buffer.
int starlet_load_nand_buffer(starlet *emu, void *buffer, u64 len)
{
	// Don't support NAND data larger than 512MB
	if (len > 0x21000400) return -1;

	u8 *buf = malloc(len);
	if (!buf)
	{
		log("Couldn't allocate buffer %08x for NAND\n", len);
		return -1;
	}
	emu->nand.data = buf;
	emu->nand.data_len = len;
	memcpy(buf, buffer, len);
	
	return 0;
}