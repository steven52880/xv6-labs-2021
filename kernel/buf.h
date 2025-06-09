struct bcache;

struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  struct buf *hashnext; // hashmap
  struct bcache *bcache;
  uchar data[BSIZE];
};

struct bcache
{
  struct spinlock lock;

  // Linked list of all free buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
};