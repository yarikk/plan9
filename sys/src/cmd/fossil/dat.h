typedef struct Arch Arch;
typedef struct BList BList;
typedef struct Block Block;
typedef struct Cache Cache;
typedef struct Disk Disk;
typedef struct Entry Entry;
typedef struct Header Header;
typedef struct Label Label;
typedef struct Periodic Periodic;
typedef struct Snap Snap;
typedef struct Source Source;
typedef struct Super Super;
typedef struct WalkPtr WalkPtr;

/* tuneable parameters - probably should not be constants */
enum {
	BytesPerEntry = 100,	/* estimate of bytes per dir entries - determines number of index entries in the block */
	FullPercentage = 80,	/* don't allocate in block if more than this percentage full */
	FlushSize = 200,	/* number of blocks to flush */
	DirtyPercentage = 50,	/* maximum percentage of dirty blocks */
};

enum {
	NilBlock	= (~0UL),
	MaxBlock	= (1UL<<31),
};

enum {
	HeaderMagic = 0x3776ae89,
	HeaderVersion = 1,
	HeaderOffset = 128*1024,
	HeaderSize = 512,
	SuperMagic = 0x2340a3b1,
	SuperSize = 512,
	SuperVersion = 1,
	LabelSize = 14,
};

/* well known tags */
enum {
	BadTag = 0,		/* this tag should not be used */
	RootTag = 1,		/* root of fs */
	EnumTag,		/* root of a dir listing */
	UserTag = 32,		/* all other tags should be >= UserTag */
};

struct Super {
	u16int version;
	u32int epochLow;
	u32int epochHigh;
	u64int qid;			/* next qid */
	u32int active;			/* root of active file system */
	u32int next;			/* root of next snapshot to archive */
	u32int current;			/* root of snapshot currently archiving */
	uchar last[VtScoreSize];	/* last snapshot successfully archived */
	char name[128];			/* label */
};


struct Fs {
	Arch *arch;		/* immutable */
	Cache *cache;		/* immutable */
	int mode;		/* immutable */
	int blockSize;		/* immutable */
	VtSession *z;		/* immutable */
	Snap *snap;	/* immutable */

	Periodic *metaFlush;	/* periodically flushes meta data cached in files */

	

	/*
	 * epoch lock.
	 * Most operations on the fs require a read lock of elk, ensuring that
	 * the current high and low epochs do not change under foot.
	 * This lock is mostly acquired via a call to fileLock or fileRlock.
	 * Deletion and creation of snapshots occurs under a write lock of elk,
	 * ensuring no file operations are occurring concurrently.
	 */
	VtLock *elk;		/* epoch lock */
	u32int ehi;		/* epoch high */
	u32int elo;		/* epoch low */

	Source *source;		/* immutable: root of sources */
	File *file;		/* immutable: root of files */
};

/*
 * variant on VtEntry
 * there are extra fields when stored locally
 */
struct Entry {
	u32int gen;			/* generation number */
	ushort psize;			/* pointer block size */
	ushort dsize;			/* data block size */
	uchar depth;			/* unpacked from flags */
	uchar flags;
	uvlong size;
	uchar score[VtScoreSize];
	u32int tag;			/* tag for local blocks: zero if stored on Venti */
	u32int snap;			/* non zero -> entering snapshot of given epoch */
	uchar archive;			/* archive this snapshot: only valid for snap != 0 */
};

struct Source {
	Fs *fs;		/* immutable */
	int mode;	/* immutable */
	u32int gen;	/* immutable */
	int dsize;	/* immutable */
	int dir;	/* immutable */

	Source *parent;	/* immutable */

	VtLock *lk;
	int ref;
	/*
	 * epoch for the source 
	 * for ReadWrite sources, epoch is used to lazily notice
	 * sources that must be split from the snapshots.
	 * for ReadOnly sources, the epoch represents the minimum epoch
	 * along the chain from the root, and is used to lazily notice
	 * sources that have become invalid because they belong to an old
	 * snapshot.
	 */
	u32int epoch;
	Block *b;			/* block containing this source */
	uchar score[VtScoreSize];	/* score of block containing this source */
	u32int scoreEpoch;	/* epoch of block containing this source */
	int epb;			/* immutable: entries per block in parent */
	u32int tag;			/* immutable: tag of parent */
	u32int offset; 			/* immutable: entry offset in parent */
};


struct Header {
	ushort version;
	ushort blockSize;
	ulong super;	/* super blocks */
	ulong label;	/* start of labels */
	ulong data;	/* end of labels - start of data blocks */
	ulong end;	/* end of data blocks */
};

/*
 * contains a one block buffer
 * to avoid problems of the block changing underfoot
 * and to enable an interface that supports unget.
 */
struct DirEntryEnum {
	File *file;

	u32int boff; 	/* block offset */

	int i, n;
	DirEntry *buf;
};

/* Block states; two orthogonal fields, Bv* and Ba* */
enum {
	BsFree = 0,		/* available for allocation */
	BsBad = 0xFF,		/* something is wrong with this block */

	/* bit fields */
	BsAlloc = 1<<0,	/* block is in use */
	BsCopied = 1<<1,	/* block has been copied */
	BsVenti = 1<<2,	/* block has been stored on Venti */
	BsClosed = 1<<3,	/* block has been unlinked from active file system */
	BsMask = BsAlloc|BsCopied|BsVenti|BsClosed,
};

/*
 * Each block has a state and generation
 * The following invariants are maintained
 * 	Each block has no more than than one parent per generation
 * 	For Active*, no child has a parent of a greater generation
 *	For Snap*, there is a snap parent of given generation and there are
 *		no parents of greater gen - implies no children snaps
 *		of a lesser gen
 *	For *RO, the block is fixed - no change can be made - all pointers
 *		are valid venti addresses
 *	For *A, the block is on the venti server
 *	There are no pointers to Zombie blocks
 *
 * Transitions
 *	Archiver at generation g
 *	Mutator at generation h
 *	
 *	Want to modify a block
 *		Venti: create new Active(h)
 *		Active(x): x == h: do nothing
 *		Active(x): x < h: change to Snap(h-1) + add Active(h)
 *		ActiveRO(x): change to SnapRO(h-1) + add Active(h)
 *		ActiveA(x): add Active(h)
 *		Snap*(x): should not occur
 *		Zombie(x): should not occur
 *	Want to archive
 *		Active(x): x != g: should never happen
 *		Active(x): x == g fix children and free them: move to ActiveRO(g);
 *		ActiveRO(x): x != g: should never happen
 *		ActiveRO(x): x == g: wait until it hits ActiveA or SnapA
 *		ActiveA(x): done
 *		Snap(x): x < g: should never happen
 *		Snap(x): x >= g: fix children, freeing all SnapA(y) x == y;
 *		SnapRO(x): wait until it hits SnapA
 *
 */

/* 
 * block types
 * more regular than Venti block types
 * bit 3 -> block or data block
 * bits 2-0 -> level of block
 */
enum {
	BtData,
	BtDir = 1<<3,
	BtLevelMask = 7,
	BtMax = 1<<4,
};

/* io states */
enum {
	BioEmpty,	/* label & data are not valid */
	BioLabel,	/* label is good */
	BioClean,	/* data is on the disk */
	BioDirty,	/* data is not yet on the disk */
	BioReading,	/* in process of reading data */
	BioWriting,	/* in process of writing data */
	BioReadError,	/* error reading: assume disk always handles write errors */
	BioVentiError,	/* error reading from venti (probably disconnected) */
	BioMax
};

struct Label {
	uchar type;
	uchar state;
	u32int tag;
	u32int epoch;
	u32int epochClose;
};

struct Block {
	Cache *c;
	int ref;
	int nlock;
	ulong	pc;		/* pc that fetched this block from the cache */

	VtLock *lk;
	
	int 	part;
	u32int	addr;
	uchar	score[VtScoreSize];	/* score */
	Label l;

	uchar	*dmap;

	uchar 	*data;

	/* the following is private; used by cache */

	Block	*next;			/* doubly linked hash chains */
	Block	**prev;
	u32int	heap;			/* index in heap table */
	u32int	used;			/* last reference times */

	u32int	vers;			/* version of dirty flag */

	BList	*uhead;			/* blocks to unlink when this block is written */
	BList	*utail;

	/* block ordering for cache -> disk */
	BList	*prior;			/* list of blocks before this one */

	Block	*ionext;
	int	iostate;
	VtRendez *ioready;
};

/* tree walker, for gc and archiver */
struct WalkPtr
{
	uchar *data;
	int isEntry;
	int n;
	int m;
	Entry e;
	uchar type;
	u32int tag;
};

/* disk partitions */
enum {
	PartError,
	PartSuper,
	PartLabel,
	PartData,
	PartVenti,	/* fake partition */
};

extern vtType[BtMax];