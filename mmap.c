#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "defs.h"
#include "memlayout.h"
#include "mmap.h"

// <!! ---------------- Mmap Utils -------------------- !!>

// whole mmap region structure
void zero_mmap_region_struct(struct mmap_vas *mr) {
  mr->virt_addr = 0;
  mr->size = 0;
  mr->flags = 0;
  mr->protection = 0;
  mr->f = 0;
  mr->offset = 0;
  mr->flags = 0;
  mr->stored_size = 0;
}

void copy_mmap(struct mmap_vas *m1, struct mmap_vas *m2) {
  m1->virt_addr = m2->virt_addr;
  m1->size = m2->size;
  m1->flags = m2->flags;
  m1->protection = m2->protection;
  m1->f = m2->f;
  m1->offset = m2->offset;
}


// Get physical Address of page from virtual address of process
uint get_physical_page(struct proc *p, uint tempaddr, pte_t **pte) {
  *pte = walkpgdir(p->pgdir, (char *)tempaddr, 0);
  if (!*pte) {
    return 0;
  }
  uint pa = PTE_ADDR(**pte);
  return pa;
}

// Copy mmaps from parent to child process
int copy_maps(struct proc *parent, struct proc *child) {
  pte_t *pte;
  int i = 0;
  while (i < parent->total_mmaps) {
    uint virt_addr = parent->mmaps[i].virt_addr;
    int protection = parent->mmaps[i].protection;
    int isshared = parent->mmaps[i].flags & MAP_SHARED;
    uint size = parent->mmaps[i].size;
    uint start = virt_addr;
    for (; start < virt_addr + size; start += PGSIZE) {
      uint pa = get_physical_page(parent, start, &pte);
      if (isshared) {
        // If pa is zero then page is not allocated yet, allocate and continue
        if (pa == 0) {
          int total_mmap_size =
              parent->mmaps[i].size - parent->mmaps[i].stored_size;
          int size = PGSIZE > total_mmap_size ? total_mmap_size : PGSIZE;
          if (mmap_store_data(parent, start, size, parent->mmaps[i].flags,
                              protection, parent->mmaps[i].f,
                              parent->mmaps[i].offset) < 0) {
            return -1;
          }
          parent->mmaps[i].stored_size += size;
        }
        pa = get_physical_page(parent, start, &pte);
        // If the page is shared and then all the data should be stored in page
        // and mapped to each process
        char *parentmem = (char *)P2V(pa);
        if (mappages(child->pgdir, (void *)start, PGSIZE, V2P(parentmem),
                     protection) < 0) {
          // ERROR: Shared mappages failed
          cprintf("CopyMaps: mappages failed\n");
        }
      } else {
        // If the mapping is private, lazy mapping can be done
        if (pa == 0) {
          continue;
        }
        char *mem = kalloc();
        if (!mem) {
          // ERROR: Private kalloc failed
          return -1;
        }
        char *parentmem = (char *)P2V(pa);
        memmove(mem, parentmem, PGSIZE);
        if (mappages(child->pgdir, (void *)start, PGSIZE, V2P(mem),
                     protection) < 0) {
          // ERROR: Private mappages failed
          return -1;
        }
      }
    }
    copy_mmap(&child->mmaps[i], &parent->mmaps[i]);
    if (isshared) {
      child->mmaps[i].ref_count = 1;
    }
    i += 1;
  }
  child->total_mmaps = parent->total_mmaps;
  return 0;
}

// 
int shift_mmap_arr(struct proc *p, int size, int idx, uint mmap_addr) {
  int j = p->total_mmaps;
  // shift mmaps to make room
  while (j > idx + 1) {
    copy_mmap(&p->mmaps[j], &p->mmaps[j - 1]);
    j--;
  }
  if (PGROUNDUP(mmap_addr + size) >= KERNBASE) {
    return -1;
  }
  p->mmaps[idx + 1].virt_addr = mmap_addr;
  p->mmaps[idx + 1].size = size;
  return idx + 1; // Return new index of mmap
}


// check if the given address is available 
int check_mem_addr(struct proc *p, uint addr, int size) {
  uint mmap_addr = PGROUNDUP(addr); // Round up the requested address

  // Check if there's enough space at the beginning of the address range
  if (mmap_addr > PGROUNDUP(p->mmaps[p->total_mmaps - 1].virt_addr + p->mmaps[p->total_mmaps - 1].size)) {
    // If there's space, insert the new mapping at the beginning
    return shift_mmap_arr(p, size, p->total_mmaps - 1, mmap_addr);
  }

  // Iterate through existing mappings
  int i = 0;
  while (i < p->total_mmaps - 1) {
    // Check if the new mapping can fit between the current and next existing mapping
    int start_addr = PGROUNDUP(p->mmaps[i].virt_addr + p->mmaps[i].size);
    int end_addr = PGROUNDUP(p->mmaps[i + 1].virt_addr);

    // If there's space, insert the new mapping here
    if (mmap_addr > start_addr && end_addr > mmap_addr + size) {
      return shift_mmap_arr(p, size, i, mmap_addr);
    }

    // If the new mapping's address overlaps with the current mapping, return -1
    if (p->mmaps[i].virt_addr >= mmap_addr) {
      return -1;
    }

    i++;
  }

  // If no suitable space was found, return -1
  return -1;
}

// find an appropriate address for mmap
int get_mmap_addr(struct proc *p, int size) {
  if (p->total_mmaps == 0) {
    if (PGROUNDUP(MMAPBASE + size) >= KERNBASE) {
      // Address out of bounds
      return -1;
    }
    p->mmaps[0].virt_addr = PGROUNDUP(MMAPBASE);
    p->mmaps[0].size = size;
    return 0; // index in mmap array
  }
  // find available map address
  int i = 0;
  for (; i < p->total_mmaps && p->mmaps[i + 1].virt_addr != 0; i++) {
    uint start_addr = PGROUNDUP(p->mmaps[i].virt_addr + p->mmaps[i].size);
    uint end_addr = PGROUNDUP(p->mmaps[i + 1].virt_addr);
    if (end_addr - start_addr > size) {
      break;
    }
  }
  uint mmapaddr = PGROUNDUP(p->mmaps[i].virt_addr + p->mmaps[i].size);
  if (mmapaddr + size > KERNBASE) {
    return -1;
  }
  return shift_mmap_arr(p, size, i, mmapaddr); 
}


// Main function which does file backed memory mapping
static int map_file(struct proc *p, struct file *f, uint mmapaddr,
                              int protection, int offset, int size) {
  int currsize = 0;
  int mainsize = size;
  for (; currsize < mainsize; currsize += PGSIZE) {
    int mapsize = PGSIZE > size ? size : PGSIZE;

    char *temp = kalloc(); // Allocate a temporary page
  
    if (!temp) {
      // fail kalloc
      return -1;
    }
    memset(temp, 0, PGSIZE);
    // copy file content to alloced memory
    int tempsize = mapsize;
    int i = 0;
    while (tempsize != 0) {
      // get page
      int curroff = offset % PGSIZE;
      int currsize = PGSIZE - curroff > tempsize ? tempsize : PGSIZE - curroff;
      if (curroff > f->ip->size) {
        break;
      }
      int a = copyPage(f->ip, offset + PGSIZE * i, f->ip->inum, f->ip->dev,
                      temp + mapsize - tempsize, currsize, curroff);
      if (a == -1)
        return -1;
      tempsize -= currsize;
      offset = 0;
      i += 1;
    }

    // Map the page to user process
    if (mappages(p->pgdir, (void *)mmapaddr, PGSIZE, V2P(temp), protection) < 0) {
      return -1;
    }

    size -= PGSIZE;
  }
  return size;
}

// anonymous
static int map_anon(struct proc *p, uint mmapaddr, int protection, int size) {
  int i, mapped = 0;
  for (i = 0; i < size; i += PGSIZE) {
    char *mapped_page = kalloc();
    if (!mapped_page) {
      // kalloc failed
      return -1;
    }
    memset(mapped_page, 0, PGSIZE);
    if (mappages(p->pgdir, (void *)(mmapaddr + i), PGSIZE, V2P(mapped_page), protection) < 0) {
      // mappages fail
      deallocuvm(p->pgdir, mmapaddr, mmapaddr + mapped);
      kfree(mapped_page);
      return -1;
    }
    mapped += PGSIZE;
  }
  return mapped;
}

// main func for handing anon and file mapping
// called from trap.c in lazy allocation
int mmap_store_data(struct proc *p, int addr, int size, int flags,
                    int protection, struct file *f, int offset) {
  if (!(flags & MAP_ANONYMOUS)) { // File backed mapping
    if (map_file(p, f, addr, protection, offset, size) == -1) {
      return -1;
    }
  } else { // Anonymous mapping
    if (map_anon(p, addr, protection, size) < 0) {
      return -1;
    }
  }
  return 0;
}

// mmap system call main function
void *mmap(int addr, struct file *f, int size, int offset, int flags,
              int protection) {
  if (!(flags & MAP_PRIVATE) && !(flags & MAP_SHARED)) {
    // Invalid arguements
    return (void *)-1;
  }
  // When size provided is less or equal to zero and offset is less than zero
  if (size <= 0 || offset < 0) {
    return (void *)-1;
  }
  // File mapping without read permission on file
  if (!(flags & MAP_ANONYMOUS) && !f->readable) {
    return (void *)-1;
  }
  // When the mapping is shared and write permission is provided but opened file
  // is not opened in write mode
  if ((flags & MAP_SHARED) && (protection & PROT_WRITE) && !f->writable) {
    return (void *)-1;
  }
  struct proc *p = myproc();
  if (p->total_mmaps == MAX_MMAPS) {
    // Mappings count exceeds
    return (void *)-1;
  }
  int i = -1;
  if (flags & MAP_FIXED) {
    if (addr != 0) {
      uint rounded_addr = PGROUNDUP(PGROUNDUP(addr) + size);
      if (addr < MMAPBASE || rounded_addr > KERNBASE || addr % PGSIZE != 0) {
        return (void *)-1;
      }
      i = check_mem_addr(p, (uint)addr, size);
      if (i == -1) {
        return (void *)-1;
      }
    }
  }
  else {
    int flag = 0;
    if ((void *)addr != (void *)0) {
      uint rounded_addr = PGROUNDUP(PGROUNDUP(addr) + size);
      if (addr < MMAPBASE || rounded_addr > KERNBASE) {
        return (void *)-1;
      }
      i = check_mem_addr(p, (uint)addr, size);
      if (i != -1) {
        flag = 1;
      }
    }
    if (!flag) {
      i = get_mmap_addr(p, size);
    }
    if (i == -1) {
      return (void *)-1;
    }
  }
  // Store mmap info in process's mmap array
  p->mmaps[i].flags = flags;
  if (protection == PROT_NONE) {
    p->mmaps[i].protection = 0;
  } else {
    p->mmaps[i].protection = PTE_U | protection;
  }
  p->mmaps[i].offset = offset;
  p->mmaps[i].f = f;
  p->total_mmaps += 1;
  return (void *)p->mmaps[i].virt_addr;
}

// Main function of munmap system call
int munmap(struct proc *p, int addr, int size) {
  pte_t *pte;
  uint mainaddr = PGROUNDUP(addr);
  int unmapping_size = PGROUNDUP(size);
  int i = 0;
  int total_size = 0;
  // Find the mmap entry
  for (; i < MAX_MMAPS; i++) {
    if (p->mmaps[i].virt_addr == mainaddr) {
      total_size = p->mmaps[i].size;
      break;
    }
  }
  // Page with given address does not exist
  if (i == MAX_MMAPS || total_size == 0) {
    // Addr not present in mappings
    return -1;
  }
  uint isanon = p->mmaps[i].flags & MAP_ANONYMOUS;
  uint isshared = p->mmaps[i].flags & MAP_SHARED;
  if (isshared && !isanon && (p->mmaps[i].protection & PROT_WRITE)) {
    // write into the file
    p->mmaps[i].f->off = p->mmaps[i].offset;
    if (filewrite(p->mmaps[i].f, (char *)p->mmaps[i].virt_addr,
                  p->mmaps[i].size) < 0) {
      // File write failed
      return -1;
    }
  }
  // Free the allocated page
  int currsize = 0;
  int main_map_size = unmapping_size > total_size ? total_size: unmapping_size;
  for (; currsize < main_map_size; currsize += PGSIZE) {
    uint tempaddr = addr + currsize;
    uint pa = get_physical_page(p, tempaddr, &pte);
    if (pa == 0) {
      // Page was not mapped yet
      continue;
    }
    char *v = P2V(pa);
    kfree(v);
    *pte = 0;
  }
  if (p->mmaps[i].size <= unmapping_size) {
    zero_mmap_region_struct(&p->mmaps[i]);
    // Left shift the mmap array
    while (i < MAX_MMAPS && p->mmaps[i + 1].virt_addr) {
      copy_mmap(&p->mmaps[i], &p->mmaps[i + 1]);
      i += 1;
    }
    p->total_mmaps -= 1;
  } else {
    p->mmaps[i].virt_addr += unmapping_size;
    p->mmaps[i].size -= unmapping_size;
  }
  return 0;
}

void remove_mmaps(struct proc *p) {
  int total_maps = p->total_mmaps;
  while (total_maps > 0) {
    if (p->mmaps[p->total_mmaps - 1].ref_count == 0) {
      munmap(p, p->mmaps[total_maps - 1].virt_addr,
                p->mmaps[total_maps - 1].size);
    }
    total_maps--;
  }
  p->total_mmaps = 0;
}
