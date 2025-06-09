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
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all free buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;

#define MODNUM 19
struct hashmap{
  struct spinlock lock;
  struct spinlock new_lock;
  struct buf *head;
} bhashmap[MODNUM];

struct hashmap *bhashgetline(uint blockno)
{
  return &bhashmap[blockno % MODNUM];
}

void bhashadd(struct hashmap *h, struct buf *buf)
{
  buf->hashnext = h->head;
  h->head = buf;
}

struct buf *bhashget(struct hashmap *h, uint dev, uint blockno)
{
  struct buf *b = h->head;

  for (; b != 0; b = b->hashnext)
  {
    if (b->dev == dev && b->blockno == blockno)
      return b;
  }
  return 0;
}

void bhashdel(struct hashmap *h, struct buf *buf)
{
  struct buf *b = h->head;
  if (b == buf)
  {
    h->head = b->hashnext;
    return;
  }

  for (; b->hashnext != 0; b = b->hashnext)
  {
    if (b->hashnext == buf)
    {
      b->hashnext = buf->hashnext;
      return;
    }
  }

  // Not found
}

void binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  for (int i = 0; i < MODNUM; i++)
  {
    initlock(&bhashmap[i].lock, "bcache.hashlock");
    initlock(&bhashmap[i].new_lock, "bcache.hashnewlock");
  }

  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  int i = 0;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;

    b->blockno = i++;
    bhashadd(bhashgetline(b->blockno), b);
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
  acquire(&h->new_lock);
  acquire(&h->lock);
  // Is the block already cached?
  b = bhashget(h, dev, blockno);
  if (b)
  {
    if (b->refcnt == 0)
    {
      acquire(&bcache.lock);
      b->prev->next = b->next;
      b->next->prev = b->prev;
      // b->prev = 0;
      // b->next = 0;
      release(&bcache.lock);  
    }
    b->refcnt++;
    release(&h->lock);
    release(&h->new_lock);
    acquiresleep(&b->lock);
    return b;
  }

  release(&h->lock);

  //acquire(&bcache.lock);

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.

  while ((b = bcache.head.prev) != &bcache.head)
  {
    //release(&bcache.lock);

    struct hashmap *oh;
    oh = bhashgetline(b->blockno);
    acquire(&oh->lock);

    if ((!bhashget(oh, b->dev, b->blockno)))
    {
      release(&oh->lock);
      continue;
    }
    // b has been locked

    if (b->refcnt != 0)
    {
      release(&oh->lock);
      continue;
    }
    // b is free to use

    // Remove from free buf list
    acquire(&bcache.lock);
    b->next->prev = b->prev;
    b->prev->next = b->next;
    // b->next = 0;
    // b->prev = 0;
    release(&bcache.lock);

    // Remove from hashmap
    bhashdel(oh, b);
    release(&oh->lock);

    // Set info
    b->dev = dev;
    b->blockno = blockno;
    b->valid = 0;
    b->refcnt = 1;

    // Insert into hashmap
    acquire(&h->lock);
    bhashadd(h, b);
    release(&h->lock);

    release(&h->new_lock);

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
 
  struct hashmap *h = bhashgetline(b->blockno);
  acquire(&h->lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    // insert it into the HEAD of the free buffer list
    acquire(&bcache.lock);
    bcache.head.next->prev = b;
    b->next = bcache.head.next;
    bcache.head.next = b;
    b->prev = &bcache.head;
    release(&bcache.lock);
  }
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


