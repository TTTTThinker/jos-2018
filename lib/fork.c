// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	uint32_t err = utf->utf_err;
	int r;

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at uvpt
	//   (see <inc/memlayout.h>).

	// LAB 4: Your code here.
	if (!(err & FEC_WR) || !(uvpt[PGNUM(addr)] & PTE_COW))
		panic("Can't handle page fault caused by other actions except writing a COW page. Page fault type: %x\n", err);	

	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.

	// LAB 4: Your code here.
	if ((r = sys_page_alloc(0, (void *)PFTEMP, PTE_U | PTE_W | PTE_P)))
		panic("Allocate page for [envid: %08x] at PFTEMP failed. Error: %e\n", thisenv->env_id, r);
	memmove((void *)PFTEMP, ROUNDDOWN(addr, PGSIZE), PGSIZE);

	if ((r = sys_page_map(0, (void *)PFTEMP, 0, ROUNDDOWN(addr, PGSIZE), PTE_U | PTE_W | PTE_P)))
		panic("Map page for [envid: %08x] at [va: %p] failed. Error: %e\n", thisenv->env_id, ROUNDDOWN(addr, PGSIZE), r);

	if ((r = sys_page_unmap(0, (void *)PFTEMP)))
		panic("Unmap page for [envid: %08x] at PFTEMP failed. Error: %e\n", thisenv->env_id, r);
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn)
{
	int r;

	// LAB 4: Your code here.
	uint16_t perm = PTE_U | PTE_P;
	pte_t pte = uvpt[pn];

	if (pte & PTE_W || pte & PTE_COW)
		perm |= PTE_COW;

	if ((r = sys_page_map(0, (void *)(pn * PGSIZE), envid, (void *)(pn * PGSIZE), perm)))
		panic("Map page for [envid: %08x] at [va: %08x] from [envid: %08x] failed. Error: %e\n", envid, pn * PGSIZE, thisenv->env_id, r);

	if (perm & PTE_COW)
		r = sys_page_map(0, (void *)(pn * PGSIZE), 0, (void *)(pn * PGSIZE), perm);

	if (r)
		panic("Re-Map page for [envid: %08x] at [va: %08x] from [envid: %08x] failed. Error: %e\n", thisenv->env_id, pn * PGSIZE, thisenv->env_id, r);
	
	return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void)
{
	// LAB 4: Your code here.
	envid_t envid;
	uintptr_t addr;
	int r;
	extern void _pgfault_upcall(void);
	extern void (*_pgfault_handler)(struct UTrapframe *utf);

	set_pgfault_handler(pgfault);
	envid = sys_exofork();
	
	if (envid < 0)
		panic("sys_exofork() failed. Error: %e\n", envid);
	if (envid == 0) {
		thisenv = &envs[ENVX(sys_getenvid())];
		return 0;
	}
	
	// Now we are in parent environment

	for (addr = 0; addr < USTACKTOP; addr += PGSIZE) {
		if ((uvpd[PDX(addr)] & PTE_P) && (uvpt[PGNUM(addr)] & PTE_P))
			duppage(envid, PGNUM(addr));
	}

	if ((r = sys_page_alloc(envid, (void *)(UXSTACKTOP - PGSIZE), PTE_U | PTE_W | PTE_P)))
		panic("Allocate page for exception stack of [envid: %08x] failed. Error: %e\n", envid, r);
	if ((r = sys_env_set_pgfault_upcall(envid, (void *)_pgfault_upcall)))
		panic("Set page fault upcall for [envid: %08x] failed. Error: %e\n", envid, r);

	if ((r = sys_env_set_status(envid, ENV_RUNNABLE)))
		panic("Set [envid: %08x] to be ENV_RUNNABLE failed. Error: %e\n", envid, r);

	return envid;
}

// Challenge!
static void
duppage2(envid_t dstenv, void *addr)
{
	int r;

	// This is NOT what you should do in your fork.
	if ((r = sys_page_alloc(dstenv, addr, PTE_P|PTE_U|PTE_W)) < 0)
		panic("sys_page_alloc: %e", r);
	if ((r = sys_page_map(dstenv, addr, 0, UTEMP, PTE_P|PTE_U|PTE_W)) < 0)
		panic("sys_page_map: %e", r);
	memmove(UTEMP, addr, PGSIZE);
	if ((r = sys_page_unmap(0, UTEMP)) < 0)
		panic("sys_page_unmap: %e", r);
}

envid_t
sfork(void)
{
	envid_t envid;
	uintptr_t addr;
	uint16_t perm = PTE_SHARE | PTE_U | PTE_P;
	int r;

	envid = sys_exofork();
	
	if (envid < 0)
		panic("sys_exofork() failed. Error: %e\n", envid);
	if (envid == 0) {
		thisenv = &envs[ENVX(sys_getenvid())];
		return 0;
	}
	
	// Now we are in parent environment

	for (addr = 0; addr < USTACKTOP - PGSIZE; addr += PGSIZE) {
		if ((uvpd[PDX(addr)] & PTE_P) && (uvpt[PGNUM(addr)] & PTE_P)) {
			perm = (uvpt[PGNUM(addr)] & PTE_W) ? (perm | PTE_W) : perm;

			r = sys_page_map(0, (void *)addr, envid, (void *)addr, perm);
			if(r)
				panic("Map page for [envid: %08x] at [va: %08x] from [envid: %08x] failed. Error: %e\n", envid, addr, thisenv->env_id, r);

			if (!(uvpt[PGNUM(addr)] & PTE_SHARE))
				r = sys_page_map(0, (void *)addr, 0, (void *)addr, perm);			
			if(r)
				panic("Re-Map page for [envid: %08x] at [va: %08x] failed. Error: %e\n", thisenv->env_id, addr, r);
		}
	}

	duppage2(envid, (void *)(USTACKTOP - PGSIZE));

	if ((r = sys_env_set_status(envid, ENV_RUNNABLE)))
		panic("Set [envid: %08x] to be ENV_RUNNABLE failed. Error: %e\n", envid, r);

	return envid;
}
