// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

// struct {
//   struct spinlock lock;
//   struct run *freelist;
// } kmem;
// 给每个CPU分配独立的freelist，多个CPU并发分配物理内存不会相互竞争

struct
{
  struct spinlock lock;
  struct run* freelist;
} kmem[NCPU];

char* kmem_lock_names[] = 
{
  "kmem_cpu_0",
  "kmem_cpu_1",
  "kmem_cpu_2",
  "kmem_cpu_3",
  "kmem_cpu_4",
  "kmem_cpu_5",
  "kmem_cpu_6",
  "kmem_cpu_7",
};

void
kinit()
{
  // 对每个CPU进行锁的初始化
  // initlock(&kmem.lock, "kmem");
  for(int i = 0; i < NCPU; ++i)
  {
    initlock(&kmem[i].lock, kmem_lock_names[i]);
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  

  // acquire(&kmem.lock);
  // r->next = kmem.freelist;
  // kmem.freelist = r;
  // release(&kmem.lock);

  // 关中断，中断关闭时调用cpuid才是安全的，
  push_off();

  int cpu = cpuid();
  // 将释放的页插入当前CPU的空闲列表中
  acquire(&kmem[cpu].lock);
  r->next = kmem[cpu].freelist;
  kmem[cpu].freelist = r;
  release(&kmem[cpu].lock);

  pop_off();  // 重新打开中断
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  // acquire(&kmem.lock);
  // r = kmem.freelist;
  // if(r)
  //   kmem.freelist = r->next;
  // release(&kmem.lock);
  // 按照CPU来分配空闲页表了

  push_off();
  int cpu = cpuid();

  acquire(&kmem[cpu].lock);

  if(!kmem[cpu].freelist)
  {
    // 当前CPU已经没有空闲页了，去尝试窃取其他CPU的内存页
    int steal_left = 64;  // 指定偷取64个内存页
    for(int i = 0; i < NCPU; ++i)
    {
      // 轮询所有cpu，但跳过当前所处cpu
      if(i == cpu)
      {
        continue;
      }
      acquire(&kmem[i].lock);
      if(!kmem[i].freelist)
      {
        release(&kmem[i].lock);
        continue;
      } // 偷的时候，人家CPU得有空闲页才行啊，否则就得释放

      // 现在有的偷了，rr 指向有空闲页cpu的当前空闲列表头
      struct run* rr = kmem[i].freelist;
      while(rr && steal_left)
      {
        // i_cpu还有能偷走的页，并且还没偷够64个，就继续偷
        kmem[i].freelist = rr->next;    // 将 i_cpu的空闲链表头指针往后移
        rr->next = kmem[cpu].freelist;  // 头插法，将i_cpu的第一个空闲页加入当前CPU的空闲链表
        kmem[cpu].freelist = rr;        // 更新当前cpu的空闲链表头
        rr = kmem[i].freelist;          // rr继续指向i_cpu的当前空闲链表头
        --steal_left;                   // 偷到一个空闲页
      }

      release(&kmem[i].lock);

      if(steal_left == 0)
      {
        break;
      } // 偷够64页就退出循环，不用再找一个CPU继续偷了
    }
  }
  r = kmem[cpu].freelist;
  if(r)
  {
    kmem[cpu].freelist = r->next;
  } // 分配到了页面，把其从空闲链表中取出来，更新空闲链表头
  release(&kmem[cpu].lock);
  pop_off();  // 开中断
  
  // 填充一下这个页面
  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
