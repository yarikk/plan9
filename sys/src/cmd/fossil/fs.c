#include "stdinc.h"
#include "dat.h"
#include "fns.h"
#include "error.h"

static void fsMetaFlush(void *a);
static Snap *snapInit(Fs*);
static void snapClose(Snap*);

Fs *
fsOpen(char *file, VtSession *z, long ncache, int mode)
{
	Fs *fs;
	Disk *disk;
	int fd;
	Block *b, *bs;
	Super super;
	int m;
	uchar oscore[VtScoreSize];

	switch(mode){
	default:
		vtSetError(EBadMode);
		return nil;
	case OReadOnly:
		m = OREAD;
		break;
	case OReadWrite:
		m = ORDWR;
		break;
	}
	fd = open(file, m);
	if(fd < 0){
		vtOSError();
		return nil;
	}

	bwatchInit();	
	disk = diskAlloc(fd);
	if(disk == nil){
		close(fd);
		return nil;
	}

	fs = vtMemAllocZ(sizeof(Fs));
	fs->mode = mode;
	fs->blockSize = diskBlockSize(disk);
	fs->elk = vtLockAlloc();
	fs->cache = cacheAlloc(disk, z, ncache, mode);
	if(mode == OReadWrite)
		fs->arch = archInit(fs->cache, disk, fs, z);
	fs->z = z;

	b = cacheLocal(fs->cache, PartSuper, 0, mode);
	if(b == nil)
		goto Err;
	if(!superUnpack(&super, b->data)){
		blockPut(b);
		goto Err;
	}
	blockPut(b);

	fs->ehi = super.epochHigh;
	fs->elo = super.epochLow;

fprint(2, "fs->ehi %d fs->elo %d active=%d\n", fs->ehi, fs->elo, super.active);

	fs->source = sourceRoot(fs, super.active, mode);
	if(fs->source == nil){
		/*
		 * Perhaps it failed because the block is copy-on-write.
		 * Do the copy and try again.
		 */
		if(mode == OReadOnly || strcmp(vtGetError(), EBadRoot) != 0)
			goto Err;
		b = cacheLocalData(fs->cache, super.active, BtDir, RootTag, OReadWrite, 0);
		if(b == nil)
			goto Err;
		if(!(b->l.state&BsClosed) && b->l.epoch == fs->ehi){
			blockPut(b);
			goto Err;
		}
		b = blockCopy(b, RootTag, fs->ehi, fs->elo);
		if(b == nil)
			goto Err;
		localToGlobal(super.active, oscore);
		super.active = b->addr;
		bs = cacheLocal(fs->cache, PartSuper, 0, OReadWrite);
		if(bs == nil){
			blockPut(b);
			goto Err;
		}
		superPack(&super, bs->data);
		blockDependency(bs, b, 0, oscore, nil);
		blockDirty(bs);
		blockPut(bs);
		blockPut(b);
		fs->source = sourceRoot(fs, super.active, mode);
		if(fs->source == nil)
			goto Err;
	}

fprint(2, "got fs source\n");

	vtRLock(fs->elk);
	fs->file = fileRoot(fs->source);
	vtRUnlock(fs->elk);
	if(fs->file == nil)
		goto Err;

fprint(2, "got file root\n");

	if(mode == OReadWrite){
		fs->metaFlush = periodicAlloc(fsMetaFlush, fs, 1000);
		fs->snap = snapInit(fs);
	}
	return fs;

Err:
	fsClose(fs);
	return nil;
}

void
fsClose(Fs *fs)
{
	vtRLock(fs->elk);
	periodicKill(fs->metaFlush);
	snapClose(fs->snap);
	if(fs->file){
		fileMetaFlush(fs->file, 0);
		if(!fileDecRef(fs->file))
			vtFatal("fsClose: files still in use: %r\n");
	}
	fs->file = nil;
	sourceClose(fs->source);
	cacheFree(fs->cache);
	if(fs->arch)
		archFree(fs->arch);
	vtRUnlock(fs->elk);
	vtLockFree(fs->elk);
	memset(fs, ~0, sizeof(Fs));
	vtMemFree(fs);
}

int
fsRedial(Fs *fs, char *host)
{
	if(!vtRedial(fs->z, host))
		return 0;
	if(!vtConnect(fs->z, 0))
		return 0;
	return 1;
}

File *
fsGetRoot(Fs *fs)
{
	return fileIncRef(fs->file);
}

int
fsGetBlockSize(Fs *fs)
{
	return fs->blockSize;
}

Block*
superGet(Cache *c, Super* super)
{
	Block *b;

	if((b = cacheLocal(c, PartSuper, 0, OReadWrite)) == nil){
		fprint(2, "superGet: cacheLocal failed: %R");
		return nil;
	}
	if(!superUnpack(super, b->data)){
		fprint(2, "superGet: superUnpack failed: %R");
		blockPut(b);
		return nil;
	}

	return b;
}

void
superPut(Block* b, Super* super, int forceWrite)
{
	superPack(super, b->data);
	blockDirty(b);
	if(forceWrite){
		while(!blockWrite(b)){
			/* BUG: what should really happen here? */
			fprint(2, "could not write super block; waiting 10 seconds\n");
			sleep(10*000);
		}
		while(b->iostate != BioClean && b->iostate != BioDirty){
			assert(b->iostate == BioWriting);
			vtSleep(b->ioready);
		}
		/*
		 * it's okay that b might still be dirty.
		 * that means it got written out but with an old root pointer,
		 * but the other fields went out, and those are the ones
		 * we really care about.  (specifically, epochHigh; see fsSnapshot).
		 */
	}
	blockPut(b);
}

/*
 * Prepare the directory to store a snapshot.
 * Temporary snapshots go into /snapshot/yyyy/mmdd/hhmm[.#]
 * Archival snapshots go into /archive/yyyy/mmdd[.#].
 *
 * TODO This should be rewritten to eliminate most of the duplication.
 */
static File*
fileOpenSnapshot(Fs *fs, int doarchive)
{
	int n;
	char buf[30], *s;
	File *dir, *f;
	Tm now;

	if(doarchive){
		/* 
		 * a snapshot intended to be archived to venti.
		 */
		dir = fileOpen(fs, "/archive");
		if(dir == nil)
			return nil;
		now = *localtime(time(0));

		/* yyyy */
		snprint(buf, sizeof(buf), "%d", now.year+1900);
		f = fileWalk(dir, buf);
		if(f == nil)
			f = fileCreate(dir, buf, ModeDir|0555, "adm");
		fileDecRef(dir);
		if(f == nil)
			return nil;
		dir = f;

		/* mmdd[#] */
		snprint(buf, sizeof(buf), "%02d%02d", now.mon+1, now.mday);
		s = buf+strlen(buf);
		for(n=0;; n++){
			if(n)
				seprint(s, buf+sizeof(buf), ".%d", n);
			f = fileWalk(dir, buf);
			if(f != nil){
				fileDecRef(f);
				continue;
			}
			f = fileCreate(dir, buf, ModeDir|ModeSnapshot|0555, "adm");
			break;
		}
		fileDecRef(dir);
		return f;
	}else{
		/*
		 * Just a temporary snapshot
		 * We'll use /snapshot/yyyy/mmdd/hhmm.
		 * There may well be a better naming scheme.
		 * (I'd have used hh:mm but ':' is reserved in Microsoft file systems.)
		 */
		dir = fileOpen(fs, "/snapshot");
		if(dir == nil)
			return nil;

/*
 * used to do /snapshot/#
 * 
		for(n=0;; n++){
			if(n)
				seprint(s, buf+sizeof(buf), ".%d", n);
			f = fileWalk(dir, buf);
			if(f != nil){
				fileDecRef(f);
				continue;
			}
			f = fileCreate(dir, buf, ModeDir|ModeSnapshot|0555, "adm");
			break;
		}
		dir = fileOpen(fs, "/snapshot");
		if(dir == nil)
			return nil;
		snprint(buf, sizeof(buf), "%d", fs->ehi);
		f = fileCreate(dir, buf, ModeDir|ModeSnapshot|0555, "adm");
		fileDecRef(dir);
		return f;
*/

		now = *localtime(time(0));

		/* yyyy */
		snprint(buf, sizeof(buf), "%d", now.year+1900);
		f = fileWalk(dir, buf);
		if(f == nil)
			f = fileCreate(dir, buf, ModeDir|0555, "adm");
		fileDecRef(dir);
		if(f == nil)
			return nil;
		dir = f;

		/* mmdd */
		snprint(buf, sizeof(buf), "%02d%02d", now.mon+1, now.mday);
		f = fileWalk(dir, buf);
		if(f == nil)
			f = fileCreate(dir, buf, ModeDir|0555, "adm");
		fileDecRef(dir);
		if(f == nil)
			return nil;
		dir = f;

		/* hhmm[.#] */
		snprint(buf, sizeof buf, "%02d%02d", now.hour, now.min);
		s = buf+strlen(buf);
		for(n=0;; n++){
			if(n)
				seprint(s, buf+sizeof(buf), ".%d", n);
			f = fileWalk(dir, buf);
			if(f != nil){
				fileDecRef(f);
				continue;
			}
			f = fileCreate(dir, buf, ModeDir|ModeSnapshot|0555, "adm");
			break;
		}
		fileDecRef(dir);
		return f;
	}
}

int
fsEpochLow(Fs *fs, u32int low)
{
	Block *bs;
	Super super;

	vtLock(fs->elk);
	if(low > fs->ehi){
		vtSetError("bad low epoch (must be <= %ud)", fs->ehi);
		vtUnlock(fs->elk);
		return 0;
	}

	if((bs = superGet(fs->cache, &super)) == nil){
		vtUnlock(fs->elk);
		return 0;
	}

	super.epochLow = low;
	fs->elo = low;
	superPut(bs, &super, 1);
	vtUnlock(fs->elk);

	return 1;
}

static int
bumpEpoch(Fs *fs, int doarchive)
{
	uchar oscore[VtScoreSize];
	u32int oldaddr;
	Block *b, *bs;
	Entry e;
	Source *r;
	Super super;

	/*
	 * Duplicate the root block.
	 *
	 * As a hint to flchk, the garbage collector,
	 * and any (human) debuggers, store a pointer
	 * to the old root block in entry 1 of the new root block.
	 */
	r = fs->source;
	b = cacheGlobal(fs->cache, r->score, BtDir, RootTag, OReadOnly);
	if(b == nil)
		return 0;

	memset(&e, 0, sizeof e);
	e.flags = VtEntryActive | VtEntryLocal | VtEntryDir;
	memmove(e.score, b->score, VtScoreSize);
	e.tag = RootTag;
	e.snap = b->l.epoch;

	b = blockCopy(b, RootTag, fs->ehi+1, fs->elo);
	if(b == nil){
		fprint(2, "bumpEpoch: blockCopy: %R\n");
		return 0;
	}

	if(0) fprint(2, "snapshot root from %d to %d\n", oldaddr, b->addr);
	entryPack(&e, b->data, 1);
	blockDirty(b);

	/*
	 * Update the superblock with the new root and epoch.
	 */
	if((bs = superGet(fs->cache, &super)) == nil)
		return 0;

	fs->ehi++;
	memmove(r->score, b->score, VtScoreSize);
	r->epoch = fs->ehi;

	super.epochHigh = fs->ehi;
	oldaddr = super.active;
	super.active = b->addr;
	if(doarchive)
		super.next = oldaddr;

	/*
	 * Record that the new super.active can't get written out until
	 * the new b gets written out.  Until then, use the old value.
	 */
	localToGlobal(oldaddr, oscore);
	blockDependency(bs, b, 0, oscore, nil);
	blockPut(b);

	/*
	 * We force the super block to disk so that super.epochHigh gets updated.
	 * Otherwise, if we crash and come back, we might incorrectly treat as active
	 * some of the blocks that making up the snapshot we just created.
	 * Basically every block in the active file system and all the blocks in 
	 * the recently-created snapshot depend on the super block now.
	 * Rather than record all those dependencies, we just force the block to disk.
	 *
	 * Note that blockWrite might actually (will probably) send a slightly outdated 
	 * super.active to disk.  It will be the address of the most recent root that has
	 * gone to disk.
	 */
	superPut(bs, &super, 1);

	return 1;
}

int
saveQid(Fs *fs)
{
	Block *b;
	Super super;
	u64int qidMax;

	if((b = superGet(fs->cache, &super)) == nil)
		return 0;
	qidMax = super.qid;
	blockPut(b);

	if(!fileSetQidSpace(fs->file, 0, qidMax))
		return 0;

	return 1;
}

int
fsSnapshot(Fs *fs, int doarchive)
{
	File *src, *dst;
	
	assert(fs->mode == OReadWrite);

	dst = nil;

	/*
	 * Freeze file system activity.
	 */
	vtLock(fs->elk);

	/*
	 * Get the root of the directory we're going to save.
	 */
	src = fileOpen(fs, "/active");
	if(src == nil)
		goto Err;

	/*
	 * It is important that we maintain the invariant that:
	 *	if both b and bb are marked as Active with epoch e
	 *	and b points at bb, then no other pointers to bb exist.
	 * 
	 * The archiver uses this property to aggressively reclaim
	 * such blocks once they have been stored on Venti, and
	 * blockCleanup knows about this property as well.
	 * 
	 * Let's say src->source is block sb, and src->msource is block
	 * mb.  Let's also say that block b holds the Entry structures for
	 * both src->source and src->msource (their Entry structures might
	 * be in different blocks, but the argument is the same).
	 * That is, right now we have:
	 *
	 *	b	Active w/ epoch e, holds ptrs to sb and mb.
	 *	sb	Active w/ epoch e.
	 *	mb	Active w/ epoch e.
	 *
	 * With things as they are now, the invariant requires that
	 * b holds the only pointers to sb and mb.  We want to record
	 * pointers to sb and mb in new Entries corresponding to dst,
	 * which breaks the invariant.  Thus we need to do something
	 * about b.  Specifically, we bump the file system's epoch and
	 * then rewalk the path from the root down to and including b.  
	 * This will copy-on-write as we walk, so now the state will be:
	 *
	 *	b	Snap w/ epoch e, holds ptrs to sb and mb.
	 *	new-b	Active w/ epoch e+1, holds ptrs to sb and mb.
	 *	sb	Active w/ epoch e.
	 *	mb	Active w/ epoch e.
	 *
	 * In this state, it's perfectly okay to add pointers to dst, which
	 * will live in a block marked Active with epoch e+1.
	 *
	 * Of course, we need to make sure that the copied path makes
	 * it out to disk before the new dst block; if the dst block goes out
	 * first and then we crash, the invariant is violated.  Rather than
	 * deal with the dependencies, we just sync the file system to disk
	 * right now.
	 */
	if(!bumpEpoch(fs, 0) || !fileWalkSources(src))
		goto Err;

	/*
	 * Sync to disk.
	 */
	cacheFlush(fs->cache, 1);

	/*
	 * Create the directory where we will store the copy of src.
	 */
	dst = fileOpenSnapshot(fs, doarchive);
	if(dst == nil)
		goto Err;

	/*
	 * Actually make the copy by setting dst's source and msource
	 * to be src's.
	 */
	if(!fileSnapshot(dst, src, fs->ehi-1, doarchive))
		goto Err;

	fileDecRef(src);
	fileDecRef(dst);
	/*
	 * Make another copy of the file system.  This one is for the
	 * archiver, so that the file system we archive has the recently
	 * added snapshot both in /active and in /archive/yyyy/mmdd[.#].
	 */
	if(doarchive){
		if(!saveQid(fs))
			goto Err;
		if(!bumpEpoch(fs, 1))
			goto Err;
	}

	vtUnlock(fs->elk);

	/* BUG? can fs->arch fall out from under us here? */
	if(doarchive && fs->arch)
		archKick(fs->arch);

	return 1;

Err:
	fprint(2, "fsSnapshot: %R\n");
	if(src)
		fileDecRef(src);
	if(dst)
		fileDecRef(dst);
	vtUnlock(fs->elk);
	return 0;
}

int
fsVac(Fs *fs, char *name, uchar score[VtScoreSize])
{
	int r;
	DirEntry de;
	Entry e, ee;
	File *f;

	vtRLock(fs->elk);
	f = fileOpen(fs, name);
	if(f == nil){
		vtRUnlock(fs->elk);
		return 0;
	}

	if(!fileGetSources(f, &e, &ee, 0) || !fileGetDir(f, &de)){
		fileDecRef(f);
		vtRUnlock(fs->elk);
		return 0;
	}
	fileDecRef(f);

	r = mkVac(fs->z, fs->blockSize, &e, &ee, &de, score);
	vtRUnlock(fs->elk);
	return r;
}

static int
vtWriteBlock(VtSession *z, uchar *buf, uint n, uint type, uchar score[VtScoreSize])
{
	if(!vtWrite(z, score, type, buf, n))
		return 0;
	if(!vtSha1Check(score, buf, n))
		return 0;
	return 1;
}

int
mkVac(VtSession *z, uint blockSize, Entry *pe, Entry *pee, DirEntry *pde, uchar score[VtScoreSize])
{
	uchar buf[8192];
	int i;
	uchar *p;
	uint n;
	DirEntry de;
	Entry e, ee, eee;
	MetaBlock mb;
	MetaEntry me;
	VtRoot root;

	e = *pe;
	ee = *pee;
	de = *pde;

	if(globalToLocal(e.score) != NilBlock
	|| (ee.flags&VtEntryActive && globalToLocal(ee.score) != NilBlock)){
		vtSetError("can only vac paths already stored on venti");
		return 0;
	}

	/*
	 * Build metadata source for root.
	 */
	n = deSize(&de);
	if(n+MetaHeaderSize+MetaIndexSize > sizeof buf){
		vtSetError("DirEntry too big");
		return 0;
	}
	memset(buf, 0, sizeof buf);
	mbInit(&mb, buf, n+MetaHeaderSize+MetaIndexSize, 1);
	p = mbAlloc(&mb, n);
	if(p == nil)
		abort();
	mbSearch(&mb, de.elem, &i, &me);
	assert(me.p == nil);
	me.p = p;
	me.size = n;
	dePack(&de, &me);
	mbInsert(&mb, i, &me);
	mbPack(&mb);

	eee.size = n+MetaHeaderSize+MetaIndexSize;
	if(!vtWriteBlock(z, buf, eee.size, VtDataType, eee.score))
		return 0;
	eee.psize = 8192;
	eee.dsize = 8192;
	eee.depth = 0;
	eee.flags = VtEntryActive;

	/*
	 * Build root source with three entries in it.
	 */
	entryPack(&e, buf, 0);
	entryPack(&ee, buf, 1);
	entryPack(&eee, buf, 2);

	n = VtEntrySize*3;
	memset(&root, 0, sizeof root);
	if(!vtWriteBlock(z, buf, n, VtDirType, root.score))
		return 0;

	/*
	 * Save root.
	 */
	root.version = VtRootVersion;
	strcpy(root.type, "vac");
	strecpy(root.name, root.name+sizeof root.name, de.elem);
	root.blockSize = blockSize;
	vtRootPack(&root, buf);
	if(!vtWriteBlock(z, buf, VtRootSize, VtRootType, score))
		return 0;

	return 1;
}

int
fsSync(Fs *fs)
{
	vtLock(fs->elk);
	cacheFlush(fs->cache, 1);
	vtUnlock(fs->elk);
	return 1;
}

int
fsNextQid(Fs *fs, u64int *qid)
{
	Block *b;
	Super super;

	if((b = superGet(fs->cache, &super)) == nil)
		return 0;

	*qid = super.qid++;

	/*
	 * It's okay if the super block doesn't go to disk immediately,
	 * since fileMetaAlloc will record a dependency between the
	 * block holding this qid and the super block.  See file.c:/^fileMetaAlloc.
	 */
	superPut(b, &super, 0);
	return 1;
}

static void
fsMetaFlush(void *a)
{
	Fs *fs = a;

	vtRLock(fs->elk);
	fileMetaFlush(fs->file, 1);
	vtRUnlock(fs->elk);
	cacheFlush(fs->cache, 0);
}

struct Snap
{
	Fs *fs;
	Periodic *tick;
	VtLock *lk;
	uint snapMinutes;
	uint archMinute;
	u32int lastSnap;
	u32int lastArch;
	uint ignore;
};

static void
snapEvent(void *v)
{
	Snap *s;
	u32int now, min;
	Tm tm;

	s = v;

	now = time(0)/60;
	vtLock(s->lk);

	/*
	 * Snapshots happen every snapMinutes minutes.
	 * If we miss a snapshot (for example, because we
	 * were down), we wait for the next one.
	 */
	if(s->snapMinutes != ~0 && s->snapMinutes != 0
	&& now%s->snapMinutes==0 && now != s->lastSnap){
if(0)fprint(2, "snapshot %02d%02d\n", now/60, now%60);
		if(!fsSnapshot(s->fs, 0))
			fprint(2, "fsSnapshot snap: %R\n");
		s->lastSnap = now;
	}

	/*
	 * Archival snapshots happen at archMinute.
	 */
	tm = *localtime(now*60);
	min = tm.hour*60+tm.min;
	if(s->archMinute != ~0 && min == s->archMinute && now != s->lastArch){
if(0)fprint(2, "archive %02d%02d\n", now/60, now%60);
		if(!fsSnapshot(s->fs, 1))
			fprint(2, "fsSnapshot arch: %R\n");
		s->lastArch = now;
	}
	vtUnlock(s->lk);
}

static Snap*
snapInit(Fs *fs)
{
	Snap *s;

	s = vtMemAllocZ(sizeof(Snap));
	s->fs = fs;
	s->tick = periodicAlloc(snapEvent, s, 10*1000);
	s->lk = vtLockAlloc();
	s->snapMinutes = -1;
	s->archMinute = -1;
	s->ignore = 5*2;	/* wait five minutes for clock to stabilize */
	return s;
}

void
snapGetTimes(Snap *s, u32int *arch, u32int *snap)
{
	vtLock(s->lk);
	*snap = s->snapMinutes;
	*arch = s->archMinute;
	vtUnlock(s->lk);
}

void
snapSetTimes(Snap *s, u32int arch, u32int snap)
{
	vtLock(s->lk);
	s->snapMinutes = snap;
	s->archMinute = arch;
	vtUnlock(s->lk);
}

static void
snapClose(Snap *s)
{
	if(s == nil)
		return;

	periodicKill(s->tick);
	vtMemFree(s);
}
