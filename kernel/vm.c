#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "proc.h"
#include "stat.h"
#include "fcntl.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // map kernel stacks
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if(size == 0)
    panic("mappages: size");
  
  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

void vmprint(pagetable_t pagetable);

#define max(a, b) (a > b ? a : b)
#define min(a, b) (a < b ? a : b)

    int pagefault()
{
  uint64 addr = r_stval();
  uint64 scause = r_scause();
  switch (scause)
  {
  case 12:
    printf("Instruction page fault\n");
    break;
  case 13:
    printf("Load page fault\n");
    break;
  case 15:
    printf("Store page fault\n");
    break;
  default:
    break;
  }
  printf("addr: %p\n", addr);

  struct proc *p = myproc();

  addr = PGROUNDDOWN(addr);

  // Check if addr falls into any vma
  struct vma *vma = 0;
  for (int i = 0; i < VMA_AREA_CNT; i++)
  {
    if (p->vma[i].length == 0)
      continue;
    if (p->vma[i].addr <= addr && addr < p->vma[i].addr + p->vma[i].length)
    {
      vma = &p->vma[i];
      break;
    }
  }

  // Not a mapped page
  if (!vma)
  {
    printf("pagefault: not a mapped page\n");
    return -1;
  }

  // Is a mapped page
  pte_t *pte = walk(p->pagetable, addr, 0);

  // Has been loaded but still cause pagefault
  if (pte && PTE_FLAGS(*pte) & PTE_V)
  {
    printf("pagefault: has been loaded\n");
    return -1;
  }

  uint64 pa = (uint64)kalloc();
  if (!pa)
    panic("page fault: no free page");
  memset((void *)pa, 0, PGSIZE);

  int perm = PTE_U;
  perm |= vma->prot & PROT_READ ? PTE_R : 0;
  perm |= vma->prot & PROT_WRITE ? PTE_W : 0;
  perm |= vma->prot & PROT_EXEC ? PTE_X : 0;
  if (mappages(p->pagetable, addr, PGSIZE, pa, perm))
    panic("page fault: mappages");

  vmprint(p->pagetable);

  uint64 offset = addr - vma->addr;
  int length = min(PGSIZE, vma->addr + vma->length - addr);

  struct inode *ip = vma->file->ip;
  ilock(ip);
  readi(ip, 0, pa, offset, length);
  iunlock(ip);

  return 0;
}

uint64
mmap(int length, int prot, int flags, struct file *file)
{
  struct proc *p = myproc();

  // Find a free vma struct
  struct vma *v = 0;
  for (int i = 0; i < VMA_AREA_CNT; i++)
  {
    if (p->vma[i].length == 0)
    {
      v = &p->vma[i];
      break;
    }
  }

  if (!v)
  {
    panic("mmap: no free vma area");
  }


  // min(length, file_stat.size)
  // struct stat file_stat;
  // ilock(file->ip);
  // stati(file->ip, &file_stat);
  // iunlock(file->ip);
  // length = length > file_stat.size ? file_stat.size : length;

  // Check permission
  if (flags == MAP_SHARED && !file->writable && (prot & PROT_WRITE))
    return 0xffffffffffffffff;

  // Add refference to fd
  filedup(file);

  // Set vma info
  v->file = file;
  v->length = length;
  v->prot = prot;
  v->flags = flags;
  v->fileoff = 0;

  // Arrange memory space
  v->addr = p->next_vma_addr;
  p->next_vma_addr += PGROUNDUP(length);

  // Setup pagetable

  printf("mmap: %p (%d)\n", v->addr, v->length);

  // Done
  return v->addr;
}

// unmap at the start, or at the end, or the whole region (but not punch a hole in the middle of a region)
int munmap(struct proc *p, uint64 addr, int length)
{

  // Check if addr falls into any vma
  struct vma *vma = 0;
  for (int i = 0; i < VMA_AREA_CNT; i++)
  {
    if (p->vma[i].length == 0)
      continue;
    if (p->vma[i].addr <= addr && addr < p->vma[i].addr + p->vma[i].length)
    {
      vma = &p->vma[i];
      break;
    }
  }

  // Not a mapped page
  if (!vma)
    return -1;

  uint64 start_va = max(addr, vma->addr);
  uint64 end_va = min(addr + length, vma->addr + length);
  uint64 actual_length = end_va - start_va;

  if (actual_length == vma->length)
  {
    // whole region
    vma->length = 0;

    fileclose(vma->file);

    start_va = PGROUNDDOWN(start_va);
    end_va = PGROUNDUP(end_va);
  }
  else if (start_va == vma->addr)
  {
    // unmap at the start, deal with the reserved area at the end
    uint64 pa = walkaddr(p->pagetable, end_va);
    uint64 fileoff = vma->fileoff + (PGROUNDDOWN(end_va) - vma->addr);
    uint64 size = end_va - PGROUNDDOWN(end_va);
    if (size != 0 && pa != 0)
    {
      if (vma->flags & MAP_SHARED)
      {
        begin_op();
        ilock(vma->file->ip);
        writei(vma->file->ip, 0, pa, fileoff, size);
        iunlock(vma->file->ip);
        end_op();
      }

      memset((void *)(pa), 0, size);
    }

    vma->addr = end_va;
    vma->length -= actual_length;
    vma->fileoff += actual_length;

    start_va = PGROUNDDOWN(start_va);
    end_va = PGROUNDDOWN(end_va);
  }
  else if (end_va == vma->addr + length)
  {
    // unmap at the end, deal with the reserved area at the start
    uint64 pa = walkaddr(p->pagetable, start_va);
    uint64 memoff = start_va - PGROUNDDOWN(start_va);
    uint64 size = PGSIZE - memoff;
    if (pa != 0)
    {
      if (vma->flags & MAP_SHARED)
      {
        begin_op();
        ilock(vma->file->ip);
        writei(vma->file->ip, 0, pa + memoff, vma->fileoff, size);
        iunlock(vma->file->ip);
        end_op();  
      }
      memset((void *)(pa + memoff), 0, size);
    }

    vma->length -= actual_length;

    start_va = PGROUNDUP(start_va);
    end_va = PGROUNDUP(end_va);
  }
  else
  {
    // punch a hole
    panic("munmap: punch hole");
  }

  // free internal pages
  for (uint64 va = start_va; va < end_va; va += PGSIZE)
  {
    pte_t *pte = walk(p->pagetable, va, 0);

    // Not allocated
    if (!pte || !(PTE_FLAGS(*pte) & PTE_V))
      continue;

    uint64 fileoff = vma->fileoff + (va - vma->addr);
    uint64 pa = PTE2PA(*pte);

    if (vma->flags & MAP_SHARED)
    {
      begin_op();
      ilock(vma->file->ip);
      writei(vma->file->ip, 0, pa, fileoff, PGSIZE);
      iunlock(vma->file->ip);
      end_op();
    }

    // unmap and free
    uvmunmap(p->pagetable, va, 1, 1);
  }
  return 0;
}

void vmprint_walk(pagetable_t pagetable, int depth, uint64 va)
{
  // sv39 RISC-V has a 3 layer page table.
  // Layer 4 means actural physical page address.
  if (depth == 4)
  {
    return;
  }

  for (uint i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if (PTE_FLAGS(pte) & PTE_V)
    {
      pagetable_t pa = (pagetable_t)PTE2PA(pte);

      // Pretty print info
      for (int dots = 0; dots < depth; dots++)
        printf(" ..");
      char bits[] = "      ";
      if (pte & PTE_U)
        bits[0] = 'U';
      if (pte & PTE_X)
        bits[1] = 'X';
      if (pte & PTE_W)
        bits[2] = 'W';
      if (pte & PTE_R)
        bits[3] = 'R';
      if (pte & PTE_V)
        bits[4] = 'V';

      uint64 next_va = (i << PXSHIFT(3 - depth)) | va;
      printf("%d: va %p | pa %p %s\n", i, next_va, pa, bits);

      vmprint_walk(pa, depth + 1, next_va);
    }
  }
}

void vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", pagetable);
  vmprint_walk(pagetable, 1, 0);
}