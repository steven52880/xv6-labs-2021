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

struct {
  struct spinlock lock_del;
  struct buf buf[NBUF];
} bcache;

#define MODNUM 19
struct hashmap
{
  struct spinlock lock;
  struct spinlock lock_add;
  struct buf list;
} bhashmap[MODNUM];

struct hashmap *bhashgetline(uint blockno)
{
  return &bhashmap[blockno % MODNUM];
}

void bhashadd(struct hashmap *h, struct buf *buf)
{
  buf->next = h->list.next;
  buf->prev = &h->list;
  h->list.next->prev = buf;
  h->list.next = buf;
}

struct buf *bhashget(struct hashmap *h, uint dev, uint blockno)
{
  struct buf *b = h->list.next;

  for (; b != &h->list; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
      return b;
  }
  return 0;
}

void bhashdel(struct hashmap *h, struct buf *buf)
{
  buf->next->prev = buf->prev;
  buf->prev->next = buf->next;
  buf->prev = 0;
  buf->next = 0;
}

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock_del, "bcache.lock_del");

  for (int i = 0; i < MODNUM; i++)
  {
    struct hashmap *h = &bhashmap[i];
    initlock(&h->lock, "bcache.hashlock");
    initlock(&h->lock_add, "bcache.hashlockadd");
    h->list.prev = &h->list;
    h->list.next = &h->list;
  }

  // Create linked list of buffers
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    bhashadd(&bhashmap[0], b);
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  struct hashmap *h = bhashgetline(blockno);
  acquire(&h->lock_add);
  acquire(&h->lock);
  // Is the block already cached?
  b = bhashget(h, dev, blockno);
  if (b)
  {
    b->refcnt++;
    release(&h->lock);
    release(&h->lock_add);
    acquiresleep(&b->lock);
    return b;
  }
  release(&h->lock);

  acquire(&bcache.lock_del);
  while (1)
  {
    struct buf *lru_b = 0;
    uint lru_time = 0xFFFFFFFF;

    // Walk through hashmap buckets
    for (int i = 0; i < MODNUM; i++)
    {
      struct hashmap *h = &bhashmap[i];
      acquire(&h->lock);

      // Walk through link list in one bucket
      struct buf *b = h->list.next;
      while (b != &h->list)
      {
        // Check timestamp
        if (b->refcnt == 0 && b->timestamp < lru_time)
        {
          lru_time = b->timestamp;
          lru_b = b;
        }
        b = b->next;
      }

      release(&h->lock);
    }
    
    // No free buf found
    if (lru_b == 0)
      break;

    // We use bcache.lock_del, so lru_b will definitely in its bucket
    struct hashmap *lru_h = bhashgetline(lru_b->blockno);
    acquire(&lru_h->lock);

    // Retry if the selected buf has been used again
    if (lru_b->refcnt != 0 || lru_b->timestamp != lru_time)
    {
      release(&lru_h->lock);
      continue;
    }

    // Remove lru_b from bucket
    bhashdel(lru_h, lru_b);
    release(&lru_h->lock);

    // Allow other cpu to walk through hashmap
    release(&bcache.lock_del);

    b = lru_b;

    // Fill info
    b->dev = dev;
    b->blockno = blockno;
    b->valid = 0;
    b->refcnt = 1;

    // Insert into new hash bucket
    acquire(&h->lock);
    bhashadd(h, b);
    release(&h->lock);

    // Allow other cpu to use bget in the bucket
    release(&h->lock_add);

    // return
    acquiresleep(&b->lock);
    return b;
  }

  panic("bget: no buffers");
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
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&tickslock);
  uint timestamp = ticks;
  release(&tickslock);

  struct hashmap *h = bhashgetline(b->blockno);
  acquire(&h->lock);
  b->refcnt--;
  b->timestamp = timestamp;
  release(&h->lock);
}

void
bpin(struct buf *b) {
  acquire(&bhashgetline(b->blockno)->lock);
  b->refcnt++;
  release(&bhashgetline(b->blockno)->lock);
}

void
bunpin(struct buf *b) {
  acquire(&bhashgetline(b->blockno)->lock);
  b->refcnt--;
  release(&bhashgetline(b->blockno)->lock);
}


