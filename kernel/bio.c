// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

// 哈希表中的桶号索引
// 希望在每个哈希桶上加锁，只有在两个进程同时访问的区块同时哈希到同一个桶的时候，才会发生锁竞争
// 根据提示，设置质数个桶可以降低哈希冲突的可能性
#define NBUFMAP_BUCKET 13
// 哈希索引，将设备号左移27位后与块号相加，组成一个唯一键，再对桶数取模，得到哈希桶索引
#define BUFMAP_HASH(dev, blockno) ((((dev) << 27) | (blockno)) % NBUFMAP_BUCKET)

// struct {
//   struct spinlock lock;
//   struct buf buf[NBUF];

//   // Linked list of all buffers, through prev/next.
//   // Sorted by how recently the buffer was used.
//   // head.next is most recent, head.prev is least.
//   struct buf head;
// } bcache;
// 修改bcache结构体
struct
{
  struct buf buf[NBUF];
  // 驱逐锁，避免一个区块有多个缓存
  // 因为多线程多cpu在查询blockno的buf时，
  // 如果当前buf不在缓存区中，会释放桶锁去其他桶寻找lru-buf，
  // 然后另外一个线程cpu可能会再次利用blockno索引到相同的桶，拿到桶锁，检查发现buf不在缓存区，也释放桶锁区查询其他桶
  // 然后都找到lru-buf的话，就可能出现：一个区块对应多个缓存
  // 有了驱逐锁，当一个线程查询当前桶，发现找不到该buf，释放桶锁准备去找其他桶，这个时候加上驱逐锁，再次判断buf是否在缓存区内，确保不会创建重复的缓存buf，不存在才会查询其他锁
  // 在把buf从其他桶放入自己桶内才释放驱逐锁
  // 在这个线程找到返回之前，其他的线程通过索引blockno来查找buf时，也发现不存在，但在想要去其他桶查找buf之前，被驱逐锁给阻塞，
  // 直到拥有驱逐锁的线程把buf放入了这个桶，释放了驱逐锁，被阻塞的线程才会拥有这个驱逐锁，但又因为再次查询当前桶的buf已经存在，而放弃去其他桶查询
  struct spinlock eviction_lock;    
  // 哈希表
  struct buf bufmap[NBUFMAP_BUCKET];            // 哈希桶
  struct spinlock bufmap_locks[NBUFMAP_BUCKET]; // 每个哈希桶的锁
} bcache;

void
binit(void)
{
  // struct buf *b;

  // initlock(&bcache.lock, "bcache");

  // // Create linked list of buffers
  // bcache.head.prev = &bcache.head;
  // bcache.head.next = &bcache.head;
  // for(b = bcache.buf; b < bcache.buf+NBUF; b++){
  //   b->next = bcache.head.next;
  //   b->prev = &bcache.head;
  //   initsleeplock(&b->lock, "buffer");
  //   bcache.head.next->prev = b;
  //   bcache.head.next = b;
  // }

  // 初始化桶锁
  for(int i = 0; i < NBUFMAP_BUCKET; ++i)
  {
    initlock(&bcache.bufmap_locks[i], "bcache_bufmap");
  }

  for(int i = 0; i < NBUF; ++i)
  {
    // 初始化缓存区块
    struct buf* b = &bcache.buf[i];
    initsleeplock(&b->lock, "buffer");
    b->lastuse = 0;
    b->refcnt = 0;
    
    // 将所有缓存区块添加到bufmap[0]
    b->next = bcache.bufmap[0].next;
    bcache.bufmap[0].next = b;
  }
  initlock(&bcache.eviction_lock, "bcache_eviction");
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
// static struct buf*
// bget(uint dev, uint blockno)
// {
//   struct buf *b;

//   acquire(&bcache.lock);

//   // Is the block already cached?
//   for(b = bcache.head.next; b != &bcache.head; b = b->next){
//     if(b->dev == dev && b->blockno == blockno){
//       b->refcnt++;
//       release(&bcache.lock);
//       acquiresleep(&b->lock);
//       return b;
//     }
//   }

//   // Not cached.
//   // Recycle the least recently used (LRU) unused buffer.
//   for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
//     if(b->refcnt == 0) {
//       b->dev = dev;
//       b->blockno = blockno;
//       b->valid = 0;
//       b->refcnt = 1;
//       release(&bcache.lock);
//       acquiresleep(&b->lock);
//       return b;
//     }
//   }
//   panic("bget: no buffers");
// }
// 重新设计查询buf的策略

static struct buf* bget(uint dev, uint blockno)
{
  struct buf *b;
  // 通过哈希运算获取桶号，并获取桶锁
  uint key = BUFMAP_HASH(dev, blockno);
  acquire(&bcache.bufmap_locks[key]);

  // blockno的缓存区块是否已经在缓存区内
  for(b = bcache.bufmap[key].next; b; b = b->next)
  {
    if(b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      release(&bcache.bufmap_locks[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  // 不在缓存区内：

  // 为了防止死锁，先释放当前桶锁
  release(&bcache.bufmap_locks[key]);
  // 但避免引入多线程对同一blockno在当前桶查询导致缓存区块重复创建，加上驱逐锁，其他的线程会卡在这里
  acquire(&bcache.eviction_lock);
  // 释放桶锁，加上驱逐锁的间隙，可能创建了新的blockno的缓存区块，因此需要再次检查一遍
  for(b = bcache.bufmap[key].next; b; b = b->next)
  {
    if(b->dev == dev && b->blockno == blockno)
    {
      acquire(&bcache.bufmap_locks[key]); // 添加引用次数之前要加锁
      ++b->refcnt;
      release(&bcache.bufmap_locks[key]);
      release(&bcache.eviction_lock);   // 找到buf需要释放驱逐锁
      acquiresleep(&b->lock);   // 获取找到的buf的锁
      return b;
    }
  }

  // 当再次查询仍然不在缓存区的时候
  // 此时只持有驱逐锁，不持有其他任何桶锁，查询所有桶中bru-buf
  struct buf* before_least = 0;    // lru-buf的前一个块
  uint holding_bucket = -1;       // 记录当前持有那个桶锁

  // 循环查询所有桶
  for(int i = 0; i < NBUFMAP_BUCKET; ++i)
  {
    acquire(&bcache.bufmap_locks[i]); // 获取当前便利的桶锁
    int newfound = 0;   // 是否在当前桶中，找到新的lru-buf

    // 查询当前桶内所有的buf，尝试找到一个合适的，并标记
    for(b = &bcache.bufmap[i]; b->next; b = b->next)
    {
      if(b->next->refcnt == 0 && (!before_least || b->next->lastuse < before_least->next->lastuse))
      {
        before_least = b;
        newfound = 1;
      }
    }
    // 读个没找到新的lru-buf，就释放当前的桶锁
    if(!newfound)
    {
      release(&bcache.bufmap_locks[i]);
    }
    else
    {
      // 知道了新的lru-buf
      if(holding_bucket != -1)
      {
        // 如果当前找到的不是第一个lru-buf，之前肯定持有某个桶锁，需要释放
        release(&bcache.bufmap_locks[holding_bucket]);
      }
      holding_bucket = i; // 把标记 holding_bucket 更改成当前桶锁的编号
    }
  }

  // 如果没有找到任何一个lru-buf，表示没有任何空闲缓存块了
  if(!before_least)
  {
    panic("bget: no buffers");
  }

  b = before_least->next;
  if(holding_bucket != key)
  {
    // 想要偷的块如果不在key桶，就要把这个块从他所在的桶内驱逐出来
    before_least->next = b->next;
    release(&bcache.bufmap_locks[holding_bucket]);
    // 将lru-buf加入到key桶
    acquire(&bcache.bufmap_locks[key]);
    b->next = bcache.bufmap[key].next;
    bcache.bufmap[key].next = b;
  }

  // 设置新buf的字段
  b->dev = dev;
  b->blockno = blockno;
  b->refcnt = 1;
  b->valid = 0;

  // 终于可以释放相关的锁了
  release(&bcache.bufmap_locks[key]);
  release(&bcache.eviction_lock);
  acquiresleep(&b->lock);
  return b;
}
// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
// void
// brelse(struct buf *b)
// {
//   if(!holdingsleep(&b->lock))
//     panic("brelse");

//   releasesleep(&b->lock);

//   acquire(&bcache.lock);
//   b->refcnt--;
//   if (b->refcnt == 0) {
//     // no one is waiting for it.
//     b->next->prev = b->prev;
//     b->prev->next = b->next;
//     b->next = bcache.head.next;
//     b->prev = &bcache.head;
//     bcache.head.next->prev = b;
//     bcache.head.next = b;
//   }
  
//   release(&bcache.lock);
// }

void brelse(struct buf *b)
{
  // 如果当前buf的锁被其他持有
  if(!holdingsleep(&b->lock))
  {
    panic("brelse");
  }
  // 释放锁
  releasesleep(&b->lock);

  // 索引桶号
  uint key = BUFMAP_HASH(b->dev, b->blockno);

  // 获取桶锁，引用数-1
  acquire(&bcache.bufmap_locks[key]);
  b->refcnt--;
  // 如果不再有人引用，更新，用于跟踪lru-buf
  if(b->refcnt == 0)
  {
    b->lastuse = ticks;
  }
  release(&bcache.bufmap_locks[key]);
}


// void
// bpin(struct buf *b) {
//   acquire(&bcache.lock);
//   b->refcnt++;
//   release(&bcache.lock);
// }
void bpin(struct buf* b)
{
  uint key = BUFMAP_HASH(b->dev, b->blockno);
  acquire(&bcache.bufmap_locks[key]);
  ++b->refcnt;
  release(&bcache.bufmap_locks[key]);
}

// void
// bunpin(struct buf *b) {
//   acquire(&bcache.lock);
//   b->refcnt--;
//   release(&bcache.lock);
// }

void bunpin(struct buf* b)
{
  uint key = BUFMAP_HASH(b->dev, b->blockno);
  acquire(&bcache.bufmap_locks[key]);
  --b->refcnt;
  release(&bcache.bufmap_locks[key]);
}

