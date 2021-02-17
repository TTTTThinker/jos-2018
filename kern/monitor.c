// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/trap.h>
#include <kern/pmap.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line


struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "backtrace", "Display stack backtrace for function calls", mon_backtrace },
	{ "showmapping", "Show page mapping of virtual address (Usage: showmapping [va1] ...)", mon_showmapping },
};

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(commands); i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
		ROUNDUP(end - entry, 1024) / 1024);
	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	// Your code here.
	uint32_t ebp, eip, args[5];
	struct Eipdebuginfo info;
	cprintf("Stack backtrace:\n");

	ebp = read_ebp();
	while(ebp != 0x0){
		eip = *((uint32_t *)ebp + 1);
		
		for(uint32_t i = 0; i < 5; ++i)
			args[i] = *((uint32_t *)ebp + i +2);

		debuginfo_eip((uintptr_t)eip, &info);

		cprintf("  ebp %08x  eip %08x  args %08x %08x %08x %08x %08x\n", ebp, eip, args[0], args[1], args[2], args[3], args[4]);
		cprintf("         %s:%d: %.*s+%d\n", info.eip_file, info.eip_line, info.eip_fn_namelen, info.eip_fn_name, (uintptr_t)eip - info.eip_fn_addr);
		ebp = *((uint32_t *)ebp);
	}
	return 0;
}

int
mon_showmapping(int argc, char **argv, struct Trapframe *tf)
{
	cprintf("[Virtual Address]\t[Virtual Page]\t[Physical Page]\t[Page Flags]\n\n");
	for (int cnt = 1; cnt < argc; ++cnt){
		uintptr_t va = 0;
		int bits = 0;
		
		for (int i = 0; ; ++i){
			if (argv[cnt][i] == '\0')
				break;
			bits++;
		}

		for (int i = 0; i < bits - 2; ++i){
			int coefficient = 0;
			uint32_t temp_sum = 1;

			for (int j = 0; j < i; ++j)
				temp_sum *= 16;
			if (argv[cnt][bits - i - 1] >= '0' && argv[cnt][bits - i - 1] <= '9')
				coefficient = argv[cnt][bits - i - 1] - '0';
			else if (argv[cnt][bits - i - 1] >= 'a' && argv[cnt][bits - i - 1] <= 'f')
				coefficient = argv[cnt][bits - i - 1] - 'a' + 10;
			else if(argv[cnt][bits - i - 1] >= 'A' && argv[cnt][bits - i - 1] <= 'F')
				coefficient = argv[cnt][bits - i - 1] - 'A' + 10;
			temp_sum *= coefficient;
			va += temp_sum;
		}
		uintptr_t page_va = ROUNDDOWN(va, PGSIZE);
		pte_t *pte = pgdir_walk((pde_t *) kern_pgdir, (char *) va, false);
		if (pte && (*pte & PTE_P)){
			physaddr_t page_pa = PTE_ADDR(*pte);
			cprintf("       0x%08x\t    0x%08x\t     0x%08x\t", va, page_va, page_pa);
			int perm = *pte & 0xfff;
			int perms_index[10] = { PTE_P, PTE_W, PTE_U, PTE_PWT, PTE_PCD, PTE_A, PTE_D, PTE_PS, PTE_G, PTE_AVAIL };
			char *perms[10] = { "PTE_P", "PTE_W", "PTE_U", "PTE_PWT", "PTE_PCD", "PTE_A", "PTE_D", "PTE_PS", "PTE_G", "PTE_AVAIL" };
			cprintf("%s", perms[0]);
			for (int i = 1; i < 10; ++i){
				if (perm & perms_index[i])
					cprintf(" | %s", perms[i]);
			}
			cprintf("\n");
		} else{
			char *placeholder = "**********";
			cprintf("       0x%08x\t    0x%08x\t     %s\t%s\n", va, page_va, placeholder, placeholder);
		}
	}
	return 0;
}


/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");

	if (tf != NULL)
		print_trapframe(tf);

	while (1) {
		buf = readline("K> ");
		if (buf != NULL) {
			if (buf[0] == 'q' && (buf[1] == ' ' || buf[1] == '\0'))
				break;
			if (runcmd(buf, tf) < 0)
				break;
		}
	}
}
