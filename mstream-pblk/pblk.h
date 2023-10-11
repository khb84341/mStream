/*
 * Copyright (C) 2015 IT University of Copenhagen (rrpc.h)
 * Copyright (C) 2016 CNEX Labs
 * Initial release: Matias Bjorling <matias@cnexlabs.com>
 * Write buffering: Javier Gonzalez <javier@cnexlabs.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Implementation of a Physical Block-device target for Open-channel SSDs.
 *
 */

#ifndef PBLK_H_
#define PBLK_H_

#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/vmalloc.h>
#include <linux/crc32.h>
#include <linux/uuid.h>
#ifdef CONFIG_PBLK_MULTIMAP
#include <linux/delay.h>
#endif

#include <linux/lightnvm.h>

/* Run only GC if less than 1/X blocks are free */
#define GC_LIMIT_INVERSE 5
#define GC_TIME_MSECS 1000

#define PBLK_SECTOR (512)
#define PBLK_EXPOSED_PAGE_SIZE (4096)

////////////////////////////////////////
#define RWB_BUFFER	(32)
////////////////////////////////////////

#ifdef RWB_BUFFER
#define PBLK_MAX_REQ_ADDRS (16)
#define PBLK_MAX_REQ_ADDRS_PW (4)
#else
#define PBLK_MAX_REQ_ADDRS (64)
#define PBLK_MAX_REQ_ADDRS_PW (6)
#endif

#define PBLK_WS_POOL_SIZE (128)
#define PBLK_META_POOL_SIZE (128)
#define PBLK_READ_REQ_POOL_SIZE (1024)

#define PBLK_NR_CLOSE_JOBS (4)

#define PBLK_CACHE_NAME_LEN (DISK_NAME_LEN + 16)

#define PBLK_COMMAND_TIMEOUT_MS 30000

/* Max 512 LUNs per device */
#define PBLK_MAX_LUNS_BITMAP (4)

#define NR_PHY_IN_LOG (PBLK_EXPOSED_PAGE_SIZE / PBLK_SECTOR)

#define pblk_for_each_lun(pblk, rlun, i) \
		for ((i) = 0, rlun = &(pblk)->luns[0]; \
			(i) < (pblk)->nr_luns; (i)++, rlun = &(pblk)->luns[(i)])

#define ERASE 2 /* READ = 0, WRITE = 1 */

////////////////////////////////////////
#ifdef CONFIG_PBLK_MULTIMAP
#define GC_TIME
#define MAX_MAPLOG	4
#define OP_RATE (25)
#define NR_DATA (4)
#define DATA_START_STREAM	4	// 2: MHC, 3: Meta, HC
#define RESERVED_SPACE (16+5)

#define PBLK_ALL_PMT 		// F2FS-PM
//#define PBLK_NODE_BMT		// F2FS-HM
//#define PBLK_NODE_COLD	// F2FS-HM

//#define DB_PMT
#define EXT4_TEST

////////////////////////////////////// GC 
//#define PBLK_FORCE_GC_CB
//#define PBLK_GC_STREAM			// MUST ENABLE with PBLK_FORCE_GC_CB
#define MAX_HISTORY	(1024)
#define GC_AGE_START (100)
#define GC_AGE_SUM (100)

//#define PREINVALID_TRIM  // Test
/////////////////////////////////////
#define MAX_PSTREAM (DATA_START_STREAM + NR_DATA) // 3+N
#ifdef PBLK_ALL_PMT
#if (NR_DATA == 1)
#define GC_NRB	(MAX_PSTREAM-1)
#else
#define GC_NRB	(MAX_PSTREAM-2)
#endif
#else
#ifdef PBLK_NODE_COLD
#define GC_NRB	(DATA_START_STREAM - 3)
#else
#define GC_NRB	(DATA_START_STREAM - 2)
#endif
#endif
#endif

enum {
	/* IO Types */
	PBLK_IOTYPE_USER	= 1 << 0,
	PBLK_IOTYPE_GC		= 1 << 1,

	/* Write buffer flags */
	PBLK_FLUSH_ENTRY	= 1 << 2,
	PBLK_WRITTEN_DATA	= 1 << 3,
	PBLK_SUBMITTED_ENTRY	= 1 << 4,
	PBLK_WRITABLE_ENTRY	= 1 << 5,
#ifdef CONFIG_PBLK_MULTIMAP
	PBLK_RESERVED_ENTRY = 1 << 6,
	PBLK_WAITREAD_ENTRY = 1 << 7,
	PBLK_INVALIDATE_ENTRY = 1 << 8,
#endif
};

enum {
	PBLK_BLK_ST_OPEN =	0x1,
	PBLK_BLK_ST_CLOSED =	0x2,
};

struct pblk_sec_meta {
	u64 reserved;
	__le64 lba;
};

/* The number of GC lists and the rate-limiter states go together. This way the
 * rate-limiter can dictate how much GC is needed based on resource utilization.
 */
#define PBLK_GC_NR_LISTS 3

enum {
	PBLK_RL_HIGH = 1,
	PBLK_RL_MID = 2,
	PBLK_RL_LOW = 3,
};

#define pblk_dma_meta_size (sizeof(struct pblk_sec_meta) * PBLK_MAX_REQ_ADDRS)

/* write buffer completion context */
struct pblk_c_ctx {
	struct list_head list;		/* Head for out-of-order completion */

	unsigned long *lun_bitmap;	/* Luns used on current request */
	unsigned int sentry;
	unsigned int nr_valid;
	unsigned int nr_padded;
};

/* generic context */
struct pblk_g_ctx {
	void *private;
};

/* Pad context */
struct pblk_pad_rq {
	struct pblk *pblk;
	struct completion wait;
	struct kref ref;
};

/* Recovery context */
struct pblk_rec_ctx {
	struct pblk *pblk;
	struct nvm_rq *rqd;
	struct list_head failed;
	struct work_struct ws_rec;
};

/* Write context */
struct pblk_w_ctx {
	struct bio_list bios;		/* Original bios - used for completion
					 * in REQ_FUA, REQ_FLUSH case
					 */
	u64 lba;			/* Logic addr. associated with entry */
	struct ppa_addr ppa;		/* Physic addr. associated with entry */
	int flags;			/* Write context flags */
	int submit_flags;
#ifdef GC_TIME
	u64 write_time;
#endif
};

struct pblk_rb_entry {
	struct ppa_addr cacheline;	/* Cacheline for this entry */
	void *data;			/* Pointer to data on this entry */
	struct pblk_w_ctx w_ctx;	/* Context for this entry */
	struct list_head index;		/* List head to enable indexes */
};

#define EMPTY_ENTRY (~0U)

struct pblk_rb_pages {
	struct page *pages;
	int order;
	struct list_head list;
};

struct pblk_rb {
	u8 index;
	struct pblk *pblk;
	struct pblk_rb_entry *entries;	/* Ring buffer entries */
	unsigned int mem;		/* Write offset - points to next
					 * writable entry in memory
					 */
	unsigned int subm;		/* Read offset - points to last entry
					 * that has been submitted to the media
					 * to be persisted
					 */
	unsigned int sync;		/* Synced - backpointer that signals
					 * the last submitted entry that has
					 * been successfully persisted to media
					 */
	unsigned int sync_point;	/* Sync point - last entry that must be
					 * flushed to the media. Used with
					 * REQ_FLUSH and REQ_FUA
					*/
	unsigned int l2p_update;	/* l2p update point - next entry for
					 * which l2p mapping will be updated to
					 * contain a device ppa address (instead
					 * of a cacheline
					 */
	unsigned int nr_entries;	/* Number of entries in write buffer -
					 * must be a power of two
					 */
	unsigned int seg_size;		/* Size of the data segments being
					 * stored on each entry. Typically this
					 * will be 4KB
					 */
#ifdef CONFIG_PBLK_MULTIMAP
	unsigned int submit_point;
	unsigned int nr_user;
#endif
	struct list_head pages;		/* List of data pages */

	spinlock_t w_lock;		/* Write lock */
	spinlock_t s_lock;		/* Sync lock */

#ifdef CONFIG_NVM_DEBUG
	atomic_t inflight_sync_point;	/* Not served REQ_FLUSH | REQ_FUA */
#endif
};

#define PBLK_RECOVERY_SECTORS 16

struct pblk_lun {
	struct ppa_addr bppa;

	u8 *bb_list;			/* Bad block list for LUN. Only used on
					 * bring up. Bad blocks are managed
					 * within lines on run-time.
					 */

	struct semaphore wr_sem;
};

struct pblk_gc_rq {
	struct pblk_line *line;
	void *data;
	u64 lba_list[PBLK_MAX_REQ_ADDRS];
	int nr_secs;
	int secs_to_gc;
	struct list_head list;
};

struct pblk_gc {
	/* These states are not protected by a lock since (i) they are in the
	 * fast path, and (ii) they are not critical.
	 */
	int gc_active;
	int gc_enabled;
	int gc_forced;

	struct task_struct *gc_ts;
	struct task_struct *gc_writer_ts;
	struct task_struct *gc_reader_ts;

	struct workqueue_struct *gc_line_reader_wq;
	struct workqueue_struct *gc_reader_wq;

	struct timer_list gc_timer;

	struct semaphore gc_sem;
	atomic_t inflight_gc;
	int w_entries;

	struct list_head w_list;
	struct list_head r_list;

	spinlock_t lock;
	spinlock_t w_lock;
	spinlock_t r_lock;
};

struct pblk_rl {
	struct pblk *pblk;
	unsigned int high;	/* Upper threshold for rate limiter (free run -
				 * user I/O rate limiter
				 */
	unsigned int low;	/* Lower threshold for rate limiter (user I/O
				 * rate limiter - stall)
				 */
	unsigned int high_pw;	/* High rounded up as a power of 2 */

#if OP_RATE == 20
#define PBLK_USER_HIGH_THRS 8	/* Begin write limit at 12% available blks */
#define PBLK_USER_LOW_THRS 10 	/* Aggressive GC at 10% available blocks */
#elif OP_RATE == 10
#define PBLK_USER_HIGH_THRS 16	/* Begin write limit at 12% available blks */
#define PBLK_USER_LOW_THRS 20 	/* Aggressive GC at 10% available blocks */
#elif OP_RATE == 9
#define PBLK_USER_HIGH_THRS 17	/* Begin write limit at 12% available blks */
#define PBLK_USER_LOW_THRS 22 	/* Aggressive GC at 10% available blocks */
#elif OP_RATE == 8
#define PBLK_USER_HIGH_THRS 20	/* Begin write limit at 12% available blks */
#define PBLK_USER_LOW_THRS 25 	/* Aggressive GC at 10% available blocks */
#elif OP_RATE == 7
#define PBLK_USER_HIGH_THRS (22)	/* Begin write limit at 12% available blks */
#define PBLK_USER_LOW_THRS (28)	/* Aggressive GC at 10% available blocks */
#elif OP_RATE == 6
#define PBLK_USER_HIGH_THRS (26)	/* Begin write limit at 12% available blks */
#define PBLK_USER_LOW_THRS (33)	/* Aggressive GC at 10% available blocks */
#elif OP_RATE == 5
#define PBLK_USER_HIGH_THRS (32)	/* Begin write limit at 12% available blks */
#define PBLK_USER_LOW_THRS (40)	/* Aggressive GC at 10% available blocks */
#elif OP_RATE == 4
#define PBLK_USER_HIGH_THRS (40)	/* Begin write limit at 12% available blks */
#define PBLK_USER_LOW_THRS (50)	/* Aggressive GC at 10% available blocks */
#elif OP_RATE == 3
#define PBLK_USER_HIGH_THRS (53)	/* Begin write limit at 12% available blks */
#define PBLK_USER_LOW_THRS (67)	/* Aggressive GC at 10% available blocks */
#else
#define PBLK_USER_HIGH_THRS (53)	/* Begin write limit at 12% available blks */
#define PBLK_USER_LOW_THRS (67)	/* Aggressive GC at 10% available blocks */
#endif

	int rb_windows_pw;	/* Number of rate windows in the write buffer
				 * given as a power-of-2. This guarantees that
				 * when user I/O is being rate limited, there
				 * will be reserved enough space for the GC to
				 * place its payload. A window is of
				 * pblk->max_write_pgs size, which in NVMe is
				 * 64, i.e., 256kb.
				 */
	int rb_budget;		/* Total number of entries available for I/O */
	int rb_user_max;	/* Max buffer entries available for user I/O */
	int rb_gc_max;		/* Max buffer entries available for GC I/O */
	int rb_gc_rsv;		/* Reserved buffer entries for GC I/O */
	int rb_state;		/* Rate-limiter current state */
	int rb_max_io;		/* Maximum size for an I/O giving the config */

	int rsv_blocks;		/* Reserved blocks for GC */

	unsigned long long nr_secs;
	unsigned long total_blocks;
	atomic_t free_blocks;
};

#define PBLK_LINE_EMPTY (~0U)

enum {
	/* Line Types */
	PBLK_LINETYPE_FREE = 0,
	PBLK_LINETYPE_LOG = 1,
	PBLK_LINETYPE_DATA = 2,

	/* Line state */
	PBLK_LINESTATE_FREE = 10,
	PBLK_LINESTATE_OPEN = 11,
	PBLK_LINESTATE_CLOSED = 12,
	PBLK_LINESTATE_GC = 13,
	PBLK_LINESTATE_BAD = 14,
	PBLK_LINESTATE_CORRUPT = 15,

	/* GC group */
	PBLK_LINEGC_NONE = 20,
	PBLK_LINEGC_EMPTY = 21,
	PBLK_LINEGC_LOW = 22,
	PBLK_LINEGC_MID = 23,
	PBLK_LINEGC_HIGH = 24,
	PBLK_LINEGC_FULL = 25,
};

#define PBLK_MAGIC 0x70626c6b /*pblk*/

struct line_header {
	__le32 crc;
	__le32 identifier;	/* pblk identifier */
	__u8 uuid[16];		/* instance uuid */
	__le16 type;		/* line type */
	__le16 version;		/* type version */
	__le32 id;		/* line id for current line */
};

struct line_smeta {
	struct line_header header;

	__le32 crc;		/* Full structure including struct crc */
	/* Previous line metadata */
	__le32 prev_id;		/* Line id for previous line */

	/* Current line metadata */
	__le64 seq_nr;		/* Sequence number for current line */

	/* Active writers */
	__le32 window_wr_lun;	/* Number of parallel LUNs to write */

	__le32 rsvd[2];

	__le64 lun_bitmap[];
};

/*
 * Metadata layout in media:
 *	First sector:
 *		1. struct line_emeta
 *		2. bad block bitmap (u64 * window_wr_lun)
 *	Mid sectors (start at lbas_sector):
 *		3. nr_lbas (u64) forming lba list
 *	Last sectors (start at vsc_sector):
 *		4. u32 valid sector count (vsc) for all lines (~0U: free line)
 */
struct line_emeta {
	struct line_header header;

	__le32 crc;		/* Full structure including struct crc */

	/* Previous line metadata */
	__le32 prev_id;		/* Line id for prev line */

	/* Current line metadata */
	__le64 seq_nr;		/* Sequence number for current line */

	/* Active writers */
	__le32 window_wr_lun;	/* Number of parallel LUNs to write */

	/* Bookkeeping for recovery */
	__le32 next_id;		/* Line id for next line */
	__le64 nr_lbas;		/* Number of lbas mapped in line */
	__le64 nr_valid_lbas;	/* Number of valid lbas mapped in line */
	__le64 bb_bitmap[];	/* Updated bad block bitmap for line */
};

struct pblk_emeta {
	struct line_emeta *buf;		/* emeta buffer in media format */
	int mem;			/* Write offset - points to next
					 * writable entry in memory
					 */
	atomic_t sync;			/* Synced - backpointer that signals the
					 * last entry that has been successfully
					 * persisted to media
					 */
	unsigned int nr_entries;	/* Number of emeta entries */
};

struct pblk_smeta {
	struct line_smeta *buf;		/* smeta buffer in persistent format */
};

struct pblk_line {
	struct pblk *pblk;
	unsigned int id;		/* Line number corresponds to the
					 * block line
					 */
	unsigned int seq_nr;		/* Unique line sequence number */

	int state;			/* PBLK_LINESTATE_X */
	int type;			/* PBLK_LINETYPE_X */
	int gc_group;			/* PBLK_LINEGC_X */
	struct list_head list;		/* Free, GC lists */

	unsigned long *lun_bitmap;	/* Bitmap for LUNs mapped in line */

	struct pblk_smeta *smeta;	/* Start metadata */
	struct pblk_emeta *emeta;	/* End medatada */

	int meta_line;			/* Metadata line id */
	int meta_distance;		/* Distance between data and metadata */

	u64 smeta_ssec;			/* Sector where smeta starts */
	u64 emeta_ssec;			/* Sector where emeta starts */

#ifdef CONFIG_PBLK_MULTIMAP
	u8 pstream;
	unsigned long ctime;
#endif

	unsigned int sec_in_line;	/* Number of usable secs in line */

	atomic_t blk_in_line;		/* Number of good blocks in line */
	unsigned long *blk_bitmap;	/* Bitmap for valid/invalid blocks */
	unsigned long *erase_bitmap;	/* Bitmap for erased blocks */

	unsigned long *map_bitmap;	/* Bitmap for mapped sectors in line */
	unsigned long *invalid_bitmap;	/* Bitmap for invalid sectors in line */

	atomic_t left_eblks;		/* Blocks left for erasing */
	atomic_t left_seblks;		/* Blocks left for sync erasing */

	int left_msecs;			/* Sectors left for mapping */
	unsigned int cur_sec;		/* Sector map pointer */
	unsigned int nr_valid_lbas;	/* Number of valid lbas in line */

	__le32 *vsc;			/* Valid sector count in line */

	struct kref ref;		/* Write buffer L2P references */

	spinlock_t lock;		/* Necessary for invalid_bitmap only */
};

#define PBLK_DATA_LINES 18

enum {
	PBLK_KMALLOC_META = 1,
	PBLK_VMALLOC_META = 2,
};

enum {
	PBLK_EMETA_TYPE_HEADER = 1,	/* struct line_emeta first sector */
	PBLK_EMETA_TYPE_LLBA = 2,	/* lba list - type: __le64 */
	PBLK_EMETA_TYPE_VSC = 3,	/* vsc list - type: __le32 */
};

struct pblk_line_mgmt {
	int nr_lines;			/* Total number of full lines */
	int nr_free_lines;		/* Number of full lines in free list */

	/* Free lists - use free_lock */
	struct list_head free_list;	/* Full lines ready to use */
	struct list_head corrupt_list;	/* Full lines corrupted */
	struct list_head bad_list;	/* Full lines bad */

	/* GC lists - use gc_lock */
	struct list_head *gc_lists[PBLK_GC_NR_LISTS];
	struct list_head gc_high_list;	/* Full lines ready to GC, high isc */
	struct list_head gc_mid_list;	/* Full lines ready to GC, mid isc */
	struct list_head gc_low_list;	/* Full lines ready to GC, low isc */

	struct list_head gc_full_list;	/* Full lines ready to GC, no valid */
	struct list_head gc_empty_list;	/* Full lines close, all valid */

	struct pblk_line *log_line;	/* Current FTL log line */
	struct pblk_line **data_line;	/* Current data line */
	struct pblk_line *log_next;	/* Next FTL log line */
	struct pblk_line **data_next;	/* Next data line */

	struct list_head *emeta_list;	/* Lines queued to schedule emeta */

	__le32 *vsc_list;		/* Valid sector counts for all lines */

	/* Metadata allocation type: VMALLOC | KMALLOC */
	int emeta_alloc_type;

	/* Pre-allocated metadata for data lines */
	struct pblk_smeta *sline_meta[PBLK_DATA_LINES];
	struct pblk_emeta *eline_meta[PBLK_DATA_LINES];
	unsigned long meta_bitmap;

	/* Helpers for fast bitmap calculations */
	unsigned long *bb_template;
	unsigned long *bb_aux;

	unsigned long d_seq_nr;		/* Data line unique sequence number */
	unsigned long l_seq_nr;		/* Log line unique sequence number */

	spinlock_t free_lock;
	spinlock_t *close_lock;
	spinlock_t gc_lock;
};

struct pblk_line_meta {
	unsigned int smeta_len;		/* Total length for smeta */
	unsigned int smeta_sec;		/* Sectors needed for smeta */

	unsigned int emeta_len[4];	/* Lengths for emeta:
					 *  [0]: Total length
					 *  [1]: struct line_emeta length
					 *  [2]: L2P portion length
					 *  [3]: vsc list length
					 */
	unsigned int emeta_sec[4];	/* Sectors needed for emeta. Same layout
					 * as emeta_len
					 */

	unsigned int emeta_bb;		/* Boundary for bb that affects emeta */

	unsigned int vsc_list_len;	/* Length for vsc list */
	unsigned int sec_bitmap_len;	/* Length for sector bitmap in line */
	unsigned int blk_bitmap_len;	/* Length for block bitmap in line */
	unsigned int lun_bitmap_len;	/* Length for lun bitmap in line */

	unsigned int blk_per_line;	/* Number of blocks in a full line */
	unsigned int sec_per_line;	/* Number of sectors in a line */
	unsigned int dsec_per_line;	/* Number of data sectors in a line */
	unsigned int min_blk_line;	/* Min. number of good blocks in line */

	unsigned int mid_thrs;		/* Threshold for GC mid list */
	unsigned int high_thrs;		/* Threshold for GC high list */

	unsigned int meta_distance;	/* Distance between data and metadata */
};

struct pblk_addr_format {
	u64	ch_mask;
	u64	lun_mask;
	u64	pln_mask;
	u64	blk_mask;
	u64	pg_mask;
	u64	sec_mask;
	u8	ch_offset;
	u8	lun_offset;
	u8	pln_offset;
	u8	blk_offset;
	u8	pg_offset;
	u8	sec_offset;
};

enum {
	PBLK_STATE_RUNNING = 0,
	PBLK_STATE_STOPPING = 1,
	PBLK_STATE_RECOVERING = 2,
	PBLK_STATE_STOPPED = 3,
};

struct pblk_rl_per_rb {
	atomic_t rb_space;	/* Space limit in case of reaching capacity */

	atomic_t rb_user_cnt;
	atomic_t rb_gc_cnt;

	int rb_user_active;
	int rb_gc_active;

	struct timer_list u_timer;
};

struct pblk_rb_ctx {
	u8 index;
//	struct pblk *pblk;
	struct pblk_rb rwb;
	struct task_struct *writer_ts;
#ifdef CONFIG_PBLK_MULTIMAP
	spinlock_t lock;
#endif

	/* pblk provisioning values. Used by rate limiter */
	struct pblk_rl_per_rb rb_rl;
	struct list_head compl_list;
};

#ifdef CONFIG_PBLK_MULTIMAP
struct pblk_read_entry {
	struct list_head list;
	
	struct ppa_addr p_addr;
	struct ppa_addr c_addr;
};

#define MAX_LSTREAM MAX_PSTREAM

enum {
	NONE_MAP = 0,
	BLOCK_MAP = 1, 
	SECTOR_MAP = 2,
	PAGE_MAP = 3,
//	BLOCK_LOG = 4,
};

struct pblk_lstream
{
	int map_type;
	int map_size;
	int log_size;
	int lstream_num;
	int invalid_line;
	int expect_lifetime; 
	sector_t last_use_rrwb;
};

struct map_entry {
	struct rb_node rb_node;
	int lseg;
	int dirty;
	unsigned int nr_log;
	unsigned int map_size;
	struct list_head list;
};

struct pblk_cmt_mgt
{
	unsigned int max_res;		// unit: entry(4 byte)
	unsigned int alloc_res;
	unsigned int list_size;
	struct rb_root root;
	struct map_entry global_map;
	struct list_head map_list;
	spinlock_t map_lock;
};

#ifdef PBLK_GC_STREAM
struct kmeans_history {
	unsigned int min[NR_DATA];
	unsigned int max[NR_DATA];
	unsigned int seq;
}; 
#endif



#endif

struct pblk {
	u8 nr_rwb;
	u8 user_rb_option;
	u8 gc_rb_option;
	u8 wt_pin;
	u8 wt_nice;
#ifdef CONFIG_PBLK_MULTIMAP
	u8 nr_lstream;
#endif
	struct nvm_tgt_dev *dev;
	struct gendisk *disk;

	struct kobject kobj;

	struct pblk_lun *luns;

	struct pblk_rb_ctx *rb_ctx;
	struct pblk_line *lines;		/* Line array */
	struct pblk_line_mgmt l_mg;		/* Line management */
	struct pblk_line_meta lm;		/* Line metadata */

	struct pblk_rl rl;
#ifdef CONFIG_PBLK_MULTIMAP
	struct pblk_lstream *lstream;
	struct pblk_cmt_mgt cmt_mgt;
#endif

	int ppaf_bitsize;
	struct pblk_addr_format ppaf;
#ifdef CONFIG_PBLK_MULTIMAP
	int pbaf_bitsize;
	int gc_target;
#endif

	int state;			/* pblk line state */

	int min_write_pgs; /* Minimum amount of pages required by controller */
	int max_write_pgs; /* Maximum amount of pages supported by controller */
	int pgs_in_buffer; /* Number of pages that need to be held in buffer to
			    * guarantee successful reads.
			    */

	sector_t capacity; /* Device capacity when bad blocks are subtracted */
	int over_pct;      /* Percentage of device used for over-provisioning */

	int sec_per_write;

	unsigned char instance_uuid[16];

#ifdef GC_TIME
	u64 gc_time;
#endif

#ifdef CONFIG_NVM_DEBUG
	/* All debug counters apply to 4kb sector I/Os */
	atomic_long_t inflight_writes;	/* Inflight writes (user and gc) */
	atomic_long_t padded_writes;	/* Sectors padded due to flush/fua */
	atomic_long_t padded_wb;	/* Sectors padded in write buffer */
	atomic_long_t nr_flush;		/* Number of flush/fua I/O */
#ifdef CONFIG_PBLK_MULTIMAP
	atomic_long_t req_writes[MAX_LSTREAM];	/* Sectors stored on write buffer */
#else
	atomic_long_t req_writes;	/* Sectors stored on write buffer */
#endif
#ifdef CONFIG_PBLK_MULTIMAP
	atomic_long_t sub_writes[MAX_LSTREAM];	/* Sectors submitted from buffer */
#else
	atomic_long_t sub_writes;	/* Sectors submitted from buffer */
#endif
	atomic_long_t sync_writes;	/* Sectors synced to media */
	atomic_long_t inflight_reads;	/* Inflight sector read requests */
	atomic_long_t cache_reads;	/* Read requests that hit the cache */
	atomic_long_t sync_reads;	/* Completed sector read requests */
	atomic_long_t recov_writes;	/* Sectors submitted from recovery */
#ifdef CONFIG_PBLK_MULTIMAP
	atomic_long_t recov_gc_writes[MAX_LSTREAM];	/* Sectors submitted from write GC */
	atomic_long_t preinvalid_gc[MAX_LSTREAM];
#else
	atomic_long_t recov_gc_writes;	/* Sectors submitted from write GC */
#endif
	atomic_long_t recov_gc_reads;	/* Sectors submitted from read GC */
#endif
#ifdef CONFIG_PBLK_MULTIMAP
	atomic_long_t inflight_ws;
	atomic_long_t nr_erase[MAX_LSTREAM];
	atomic_long_t nr_gcskip[MAX_LSTREAM];
	atomic_long_t nr_definemap;
	atomic_long_t nr_sectormap;
	atomic_long_t nr_pagemap;
	atomic_long_t nr_blockmap;
	atomic_long_t nr_pagelog;
	atomic_long_t nr_blocklog;
	atomic_long_t nr_mapread;
	atomic_long_t nr_mapwrite;
#endif
	spinlock_t lock;

	atomic_long_t read_failed;
	atomic_long_t read_empty;
	atomic_long_t read_high_ecc;
	atomic_long_t read_failed_gc;
	atomic_long_t write_failed;
	atomic_long_t erase_failed;

	atomic_t inflight_io;		/* General inflight I/O counter */

	/* Simple translation map of logical addresses to physical addresses.
	 * The logical addresses is known by the host system, while the physical
	 * addresses are used when writing to the disk block device.
	 */
	unsigned char *trans_map;
#ifdef CONFIG_PBLK_MULTIMAP
	unsigned char *preinvalid_map;
#endif

#ifdef EXT4_TEST
	unsigned char *stream_map;
#endif
	spinlock_t trans_lock;

#ifdef PBLK_GC_STREAM
	struct kmeans_history *kmeans_history;
	int kmeans_count;
#endif

#ifdef CONFIG_PBLK_MULTIMAP
	unsigned int min_seq;
	unsigned int max_seq;
	atomic_long_t g_writes;
#endif 

	mempool_t *page_pool;
	mempool_t *line_ws_pool;
	mempool_t *rec_pool;
	mempool_t *g_rq_pool;
	mempool_t *w_rq_pool;
	mempool_t *line_meta_pool;
#ifdef CONFIG_PBLK_DFTL
	mempool_t *dftl_pool;
#else
	error
#endif
	struct workqueue_struct *close_wq;
	struct workqueue_struct *bb_wq;

	struct timer_list wtimer;
#ifdef CONFIG_PBLK_MULTIMAP
	struct timer_list rtimer;
#endif

	struct pblk_gc gc;
};

struct pblk_line_ws {
	struct pblk *pblk;
	struct pblk_line *line;
	void *priv;
	struct work_struct ws;
};

#define pblk_g_rq_size (sizeof(struct nvm_rq) + sizeof(struct pblk_g_ctx))
#define pblk_w_rq_size (sizeof(struct nvm_rq) + sizeof(struct pblk_c_ctx))

/*
 * pblk ring buffer operations
 */
int pblk_rb_init(struct pblk_rb *rb, unsigned int nrb, struct pblk_rb_entry *rb_entry_base,
		 unsigned int power_size, unsigned int power_seg_sz);
unsigned int pblk_rb_calculate_size(unsigned int nr_entries);
void *pblk_rb_entries_ref(struct pblk_rb *rb);
int pblk_rb_may_write_user(struct pblk_rb *rb, unsigned int nrb, struct bio *bio,
			   unsigned int nr_entries, unsigned int *pos);
int pblk_rb_may_write_gc(struct pblk_rb *rb, unsigned int nrb, unsigned int nr_entries,
			 unsigned int *pos);
void pblk_rb_write_entry_user(struct pblk_rb *rb, void *data,
			      struct pblk_w_ctx w_ctx, unsigned int pos);
#ifdef CONFIG_PBLK_MULTIMAP
void pblk_rb_write_entry_user_to_rrwb(struct pblk_rb *rb, void *data,
			      struct pblk_w_ctx w_ctx, unsigned int pos);
void pblk_rb_write_read_data_to_rrwb(struct pblk_rb *rb, void *data,
			      struct pblk_w_ctx w_ctx, unsigned int pos);
void pblk_rb_reserved_entry(struct pblk_rb *rb,
			      struct pblk_w_ctx w_ctx, unsigned int pos);
int pblk_rb_gc_data_to_rrwb(struct pblk *pblk, 
				void *data, struct pblk_w_ctx w_ctx, struct pblk_line *gc_line);
#endif
void pblk_rb_write_entry_gc(struct pblk_rb *rb, void *data,
			    struct pblk_w_ctx w_ctx, struct pblk_line *gc_line,
			    unsigned int pos);
struct pblk_w_ctx *pblk_rb_w_ctx(struct pblk_rb *rb, unsigned int pos);
void pblk_rb_flush(struct pblk_rb *rb, unsigned int nrb);

void pblk_rb_sync_l2p(struct pblk_rb *rb);
unsigned int pblk_rb_read_to_bio(struct pblk_rb *rb, struct nvm_rq *rqd,
				 struct bio *bio, unsigned int pos,
				 unsigned int nr_entries, unsigned int count);
unsigned int pblk_rb_read_to_bio_list(struct pblk_rb *rb, struct bio *bio,
				      struct list_head *list,
				      unsigned int max);
int pblk_rb_copy_to_bio(struct pblk_rb *rb, struct bio *bio, sector_t lba,
			struct ppa_addr ppa, int bio_iter, bool advanced_bio);
unsigned int pblk_rb_read_commit(struct pblk_rb *rb, unsigned int entries);

unsigned int pblk_rb_sync_init(struct pblk_rb *rb, unsigned long *flags);
unsigned int pblk_rb_sync_advance(struct pblk_rb *rb, unsigned int nr_entries);
struct pblk_rb_entry *pblk_rb_sync_scan_entry(struct pblk_rb *rb,
					      struct ppa_addr *ppa);
void pblk_rb_sync_end(struct pblk_rb *rb, unsigned long *flags);
unsigned int pblk_rb_sync_point_count(struct pblk_rb *rb);

unsigned int pblk_rb_read_count(struct pblk_rb *rb);
unsigned int pblk_rb_sync_count(struct pblk_rb *rb);
unsigned int pblk_rb_wrap_pos(struct pblk_rb *rb, unsigned int pos);

int pblk_rb_tear_down_check(struct pblk_rb *rb);
int pblk_rb_pos_oob(struct pblk_rb *rb, u64 pos);
void pblk_rb_data_free(struct pblk_rb *rb);
ssize_t pblk_rb_sysfs(struct pblk_rb *rb, char *buf);

unsigned int pblk_rb_random(unsigned int nr_rwb);
unsigned int pblk_rb_remain_max(struct pblk *pblk, int nr_entries, int nr_rwb);
unsigned int pblk_rb_gc_remain_max(struct pblk *pblk, int nr_entries);

/*
 * pblk core
 */
struct nvm_rq *pblk_alloc_rqd(struct pblk *pblk, int rw);
void pblk_set_sec_per_write(struct pblk *pblk, int sec_per_write);
int pblk_setup_w_rec_rq(struct pblk *pblk, struct nvm_rq *rqd,
			struct pblk_c_ctx *c_ctx);
void pblk_free_rqd(struct pblk *pblk, struct nvm_rq *rqd, int rw);
void pblk_wait_for_meta(struct pblk *pblk);
struct ppa_addr pblk_get_lba_map(struct pblk *pblk, sector_t lba);
void pblk_discard(struct pblk *pblk, struct bio *bio);
void pblk_log_write_err(struct pblk *pblk, struct nvm_rq *rqd);
void pblk_log_read_err(struct pblk *pblk, struct nvm_rq *rqd);
int pblk_submit_io(struct pblk *pblk, struct nvm_rq *rqd);
int pblk_submit_meta_io(struct pblk *pblk, struct pblk_line *meta_line, unsigned int nrb);
struct bio *pblk_bio_map_addr(struct pblk *pblk, void *data,
			      unsigned int nr_secs, unsigned int len,
			      int alloc_type, gfp_t gfp_mask);
struct pblk_line *pblk_line_get(struct pblk *pblk);
struct pblk_line *pblk_line_get_first_data(struct pblk *pblk, unsigned int nrb);
void pblk_line_replace_data(struct pblk *pblk, unsigned int nrb);
int pblk_line_recov_alloc(struct pblk *pblk, struct pblk_line *line);
void pblk_line_recov_close(struct pblk *pblk, struct pblk_line *line);
struct pblk_line *pblk_line_get_data(struct pblk *pblk, unsigned int nrb);
struct pblk_line *pblk_line_get_erase(struct pblk *pblk, unsigned int nrb);
int pblk_line_erase(struct pblk *pblk, struct pblk_line *line);
int pblk_line_is_full(struct pblk_line *line);
void pblk_line_free(struct pblk *pblk, struct pblk_line *line);
void pblk_line_close_meta(struct pblk *pblk, struct pblk_line *line, unsigned int nrb);
void pblk_line_close(struct pblk *pblk, struct pblk_line *line);
void pblk_line_close_meta_sync(struct pblk *pblk, unsigned int nrb);
void pblk_line_close_ws(struct work_struct *work);
void pblk_pipeline_stop(struct pblk *pblk);
void pblk_line_mark_bb(struct work_struct *work);
void pblk_line_run_ws(struct pblk *pblk, struct pblk_line *line, void *priv,
		      void (*work)(struct work_struct *),
		      struct workqueue_struct *wq);
u64 pblk_line_smeta_start(struct pblk *pblk, struct pblk_line *line);
int pblk_line_read_smeta(struct pblk *pblk, struct pblk_line *line);
int pblk_line_read_emeta(struct pblk *pblk, struct pblk_line *line,
			 void *emeta_buf);
int pblk_blk_erase_async(struct pblk *pblk, struct ppa_addr erase_ppa);
void pblk_line_put(struct kref *ref);
struct list_head *pblk_line_gc_list(struct pblk *pblk, struct pblk_line *line);
u64 pblk_lookup_page(struct pblk *pblk, struct pblk_line *line);
void pblk_dealloc_page(struct pblk *pblk, struct pblk_line *line, int nr_secs);
u64 pblk_alloc_page(struct pblk *pblk, struct pblk_line *line, int nr_secs);
u64 __pblk_alloc_page(struct pblk *pblk, struct pblk_line *line, int nr_secs);
int pblk_calc_secs(struct pblk *pblk, unsigned long secs_avail,
		   unsigned long secs_to_flush);
void pblk_up_page(struct pblk *pblk, struct ppa_addr *ppa_list, int nr_ppas);
void pblk_down_rq(struct pblk *pblk, struct ppa_addr *ppa_list, int nr_ppas,
		  unsigned long *lun_bitmap);
void pblk_down_page(struct pblk *pblk, struct ppa_addr *ppa_list, int nr_ppas);
void pblk_up_rq(struct pblk *pblk, struct ppa_addr *ppa_list, int nr_ppas,
		unsigned long *lun_bitmap);
void pblk_end_bio_sync(struct bio *bio);
void pblk_end_io_sync(struct nvm_rq *rqd);
int pblk_bio_add_pages(struct pblk *pblk, struct bio *bio, gfp_t flags,
		       int nr_pages);
void pblk_bio_free_pages(struct pblk *pblk, struct bio *bio, int off,
			 int nr_pages);
void pblk_map_invalidate(struct pblk *pblk, struct ppa_addr ppa);
void __pblk_map_invalidate(struct pblk *pblk, struct pblk_line *line,
			   u64 paddr);
void pblk_update_map(struct pblk *pblk, sector_t lba, struct ppa_addr ppa);
void pblk_update_map_cache(struct pblk *pblk, sector_t lba,
			   struct ppa_addr ppa);
void pblk_update_map_dev(struct pblk *pblk, sector_t lba,
			 struct ppa_addr ppa, struct ppa_addr entry_line);
#ifdef CONFIG_PBLK_MULTIMAP
int pblk_update_map_gc(struct pblk *pblk, sector_t lba, struct ppa_addr ppa,
		       struct pblk_line *gc_line, struct ppa_addr *rppa);
#else
int pblk_update_map_gc(struct pblk *pblk, sector_t lba, struct ppa_addr ppa,
		       struct pblk_line *gc_line);
#endif
void pblk_lookup_l2p_rand(struct pblk *pblk, struct ppa_addr *ppas,
			  u64 *lba_list, int nr_secs);
void pblk_lookup_l2p_seq(struct pblk *pblk, struct ppa_addr *ppas,
			 sector_t blba, int nr_secs);

/*
 * pblk user I/O write path
 */
int pblk_write_to_cache(struct pblk *pblk, struct bio *bio,
			unsigned long flags);
int pblk_write_gc_to_cache(struct pblk *pblk, void *data, u64 *lba_list,
			   unsigned int nr_entries, unsigned int nr_rec_entries,
			   struct pblk_line *gc_line, unsigned long flags);

/*
 * pblk map
 */
void pblk_map_erase_rq(struct pblk *pblk, struct nvm_rq *rqd,
		       unsigned int sentry, unsigned long *lun_bitmap,
		       unsigned int valid_secs, struct ppa_addr *erase_ppa, unsigned int nrb);
void pblk_map_rq(struct pblk *pblk, struct nvm_rq *rqd, unsigned int sentry,
		 unsigned long *lun_bitmap, unsigned int valid_secs,
		 unsigned int off, unsigned int nrb);

/*
 * pblk write thread
 */
int pblk_write_ts(void *data);
void pblk_write_timer_fn(unsigned long data);
void pblk_write_should_kick(struct pblk *pblk, unsigned int nrb);

#ifdef CONFIG_PBLK_MULTIMAP
int pblk_read_ts(void *data);
void pblk_read_kick(struct pblk *pblk);
void pblk_read_timer_fn(unsigned long data);
#endif

/*
 * pblk read path
 */
extern struct bio_set *pblk_bio_set;
int pblk_submit_read(struct pblk *pblk, struct bio *bio);
int pblk_submit_read_gc(struct pblk *pblk, u64 *lba_list, void *data,
			unsigned int nr_secs, unsigned int *secs_to_gc,
			struct pblk_line *line);
/*
 * pblk recovery
 */
void pblk_submit_rec(struct work_struct *work);
struct pblk_line *pblk_recov_l2p(struct pblk *pblk);
int pblk_recov_pad(struct pblk *pblk, unsigned int nrb);
__le64 *pblk_recov_get_lba_list(struct pblk *pblk, struct line_emeta *emeta);
int pblk_recov_setup_rq(struct pblk *pblk, struct pblk_rb *rb,
			struct pblk_c_ctx *c_ctx, struct pblk_rec_ctx *recovery,
			u64 *comp_bits, 	unsigned int comp);

/*
 * pblk gc
 */
#define PBLK_GC_MAX_READERS 8	/* Max number of outstanding GC reader jobs */
#define PBLK_GC_W_QD 128	/* Queue depth for inflight GC write I/Os */
#define PBLK_GC_L_QD 4		/* Queue depth for inflight GC lines */
#define PBLK_GC_RSV_LINE 1	/* Reserved lines for GC */

int pblk_gc_init(struct pblk *pblk);
void pblk_gc_exit(struct pblk *pblk);
void pblk_gc_should_start(struct pblk *pblk);
void pblk_gc_should_stop(struct pblk *pblk);
void pblk_gc_should_kick(struct pblk *pblk);
void pblk_gc_kick(struct pblk *pblk);
void pblk_gc_sysfs_state_show(struct pblk *pblk, int *gc_enabled,
			      int *gc_active);
int pblk_gc_sysfs_force(struct pblk *pblk, int force);

/*
 * pblk rate limiter
 */
void pblk_rl_init(struct pblk *pblk, struct pblk_rl *rl, int budget);
void pblk_rl_free(struct pblk_rl_per_rb *rl);
int pblk_rl_high_thrs(struct pblk_rl *rl);
int pblk_rl_low_thrs(struct pblk_rl *rl);
unsigned long pblk_rl_nr_free_blks(struct pblk_rl *rl);
int pblk_rl_user_may_insert(struct pblk_rl *rl, struct pblk_rl_per_rb *rb_rl, int nr_entries);
void pblk_rl_inserted(struct pblk_rl_per_rb *rb_rl, int nr_entries);
void pblk_rl_user_in(struct pblk_rl_per_rb *rb_rl, int nr_entries);
int pblk_rl_gc_may_insert(struct pblk_rl *rl, struct pblk_rl_per_rb *rb_rl, int nr_entries);
void pblk_rl_gc_in(struct pblk_rl_per_rb *rb_rl, int nr_entries);
void pblk_rl_out(struct pblk_rl_per_rb *rb_rl, int nr_user, int nr_gc);
int pblk_rl_sysfs_rate_show(struct pblk_rl *rl);
int pblk_rl_max_io(struct pblk_rl *rl);
void pblk_rl_free_lines_inc(struct pblk_rl *rl, struct pblk_line *line);
void pblk_rl_free_lines_dec(struct pblk_rl *rl, struct pblk_line *line);
void pblk_rl_set_space_limit(struct pblk_rl *rl, int entries_left);
int pblk_rl_is_limit(struct pblk_rl_per_rb *rb_rl);

/*
 * pblk sysfs
 */
int pblk_sysfs_init(struct gendisk *tdisk);
void pblk_sysfs_exit(struct gendisk *tdisk);

#ifdef CONFIG_PBLK_DFTL
int pblk_load_cmt(struct pblk *pblk, int lseg, int dirty);
int pblk_delete_cmt(struct pblk *pblk, int lseg);
int pblk_alloc_log(struct pblk *pblk,
								int lseg, int type, unsigned int log_size);
int pblk_free_log(struct pblk *pblk, int lseg, 
								int type, unsigned int log_size);
int pblk_change_map(struct pblk *pblk, int lseg);
#endif

static inline void *pblk_malloc(size_t size, int type, gfp_t flags)
{
	if (type == PBLK_KMALLOC_META)
		return kmalloc(size, flags);
	return vmalloc(size);
}

static inline void pblk_mfree(void *ptr, int type)
{
	if (type == PBLK_KMALLOC_META)
		kfree(ptr);
	else
		vfree(ptr);
}

static inline struct nvm_rq *nvm_rq_from_c_ctx(void *c_ctx)
{
	return c_ctx - sizeof(struct nvm_rq);
}

static inline void *emeta_to_bb(struct line_emeta *emeta)
{
	return emeta->bb_bitmap;
}

static inline void *emeta_to_lbas(struct pblk *pblk, struct line_emeta *emeta)
{
	return ((void *)emeta + pblk->lm.emeta_len[1]);
}

static inline void *emeta_to_vsc(struct pblk *pblk, struct line_emeta *emeta)
{
	return (emeta_to_lbas(pblk, emeta) + pblk->lm.emeta_len[2]);
}

static inline int pblk_line_vsc(struct pblk_line *line)
{
	int vsc;

	spin_lock(&line->lock);
	vsc = le32_to_cpu(*line->vsc);
	spin_unlock(&line->lock);

	return vsc;
}

#define NVM_MEM_PAGE_WRITE (8)

static inline int pblk_pad_distance(struct pblk *pblk)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;

	return NVM_MEM_PAGE_WRITE * geo->nr_luns * geo->sec_per_pl;
}

static inline int pblk_dev_ppa_to_line(struct ppa_addr p)
{
	return p.g.blk;
}

static inline int pblk_tgt_ppa_to_line(struct ppa_addr p)
{
	return p.g.blk;
}

static inline int pblk_ppa_to_pos(struct nvm_geo *geo, struct ppa_addr p)
{
	return p.g.lun * geo->nr_chnls + p.g.ch;
}

/* A block within a line corresponds to the lun */
static inline int pblk_dev_ppa_to_pos(struct nvm_geo *geo, struct ppa_addr p)
{
	return p.g.lun * geo->nr_chnls + p.g.ch;
}

static inline struct ppa_addr pblk_ppa32_to_ppa64(struct pblk *pblk, u32 ppa32)
{
	struct ppa_addr ppa64;

	ppa64.ppa = 0;

	if (ppa32 == -1) {
		ppa64.ppa = ADDR_EMPTY;
	} else if (ppa32 & (1U << 31)) {
		ppa64.c.line = ppa32 & ((~0U) >> 1);
		ppa64.c.is_cached = 1;
	} else {
		ppa64.g.blk = (ppa32 & pblk->ppaf.blk_mask) >>
							pblk->ppaf.blk_offset;
		ppa64.g.pg = (ppa32 & pblk->ppaf.pg_mask) >>
							pblk->ppaf.pg_offset;
		ppa64.g.lun = (ppa32 & pblk->ppaf.lun_mask) >>
							pblk->ppaf.lun_offset;
		ppa64.g.ch = (ppa32 & pblk->ppaf.ch_mask) >>
							pblk->ppaf.ch_offset;
		ppa64.g.pl = (ppa32 & pblk->ppaf.pln_mask) >>
							pblk->ppaf.pln_offset;
		ppa64.g.sec = (ppa32 & pblk->ppaf.sec_mask) >>
							pblk->ppaf.sec_offset;
	}

	return ppa64;
}

static inline int pblk_ppa_empty(struct ppa_addr ppa_addr)
{
	return (ppa_addr.ppa == ADDR_EMPTY);
}

static inline void pblk_ppa_set_empty(struct ppa_addr *ppa_addr)
{
	ppa_addr->ppa = ADDR_EMPTY;
}

static inline bool pblk_ppa_comp(struct ppa_addr lppa, struct ppa_addr rppa)
{
	if (lppa.ppa == rppa.ppa)
		return true;

	return false;
}

static inline int pblk_addr_in_cache(struct ppa_addr ppa)
{
	return (ppa.ppa != ADDR_EMPTY && ppa.c.is_cached);
}

static inline int pblk_addr_to_cacheline(struct ppa_addr ppa)
{
	return ppa.c.line;
}

static inline u64 pblk_addr_to_nrb(struct ppa_addr ppa)
{
	return ppa.c.nrb;
}

static inline struct ppa_addr pblk_cacheline_to_addr(int addr, int nrb)
{
	struct ppa_addr p;

	p.c.line = addr;
	p.c.nrb = nrb;
	p.c.is_cached = 1;

	return p;
}

static inline struct ppa_addr addr_to_gen_ppa(struct pblk *pblk, u64 paddr,
					      u64 line_id)
{
	struct ppa_addr ppa;

	ppa.ppa = 0;
	ppa.g.blk = line_id;
	ppa.g.pg = (paddr & pblk->ppaf.pg_mask) >> pblk->ppaf.pg_offset;
	ppa.g.lun = (paddr & pblk->ppaf.lun_mask) >> pblk->ppaf.lun_offset;
	ppa.g.ch = (paddr & pblk->ppaf.ch_mask) >> pblk->ppaf.ch_offset;
	ppa.g.pl = (paddr & pblk->ppaf.pln_mask) >> pblk->ppaf.pln_offset;
	ppa.g.sec = (paddr & pblk->ppaf.sec_mask) >> pblk->ppaf.sec_offset;

	return ppa;
}

static inline struct ppa_addr addr_to_pblk_ppa(struct pblk *pblk, u64 paddr,
					 u64 line_id)
{
	struct ppa_addr ppa;

	ppa = addr_to_gen_ppa(pblk, paddr, line_id);

	return ppa;
}

static inline struct ppa_addr pblk_trans_map_get(struct pblk *pblk,
								sector_t lba)
{
	struct ppa_addr ppa;
#ifndef CONFIG_PBLK_MULTIMAP
	if (pblk->ppaf_bitsize < 32) {
		u32 *map = (u32 *)pblk->trans_map;

		ppa = pblk_ppa32_to_ppa64(pblk, map[lba]);
	} else {
		struct ppa_addr *map = (struct ppa_addr *)pblk->trans_map;
		ppa = map[lba];
	}
#ifdef CONFIG_PBLK_DFTL
	spin_lock(&pblk->cmt_mgt.map_lock);
	pblk_load_cmt(pblk, lseg, 0);
	spin_unlock(&pblk->cmt_mgt.map_lock);
#else
error
#endif

#else
	struct pba_addr *bmap = (struct pba_addr *)pblk->trans_map;
	sector_t lseg = lba >> pblk->ppaf.blk_offset;
	int entry_num = (1 << pblk->ppaf.blk_offset);
	struct pba_addr pba = bmap[lseg];
	int lstream; 
	struct pblk_lstream *log;
	int loff = lba & (entry_num - 1);

	if (pba.m.map == NONE_MAP)
	{
		pblk_ppa_set_empty(&ppa);
		return ppa;
	}
	lstream = (u64)pba.m.lstream;
	if (lstream >= pblk->nr_lstream)
	{
		printk("pblk_trans_map_get lstream:%d %d\n", lstream, pblk->nr_lstream);
		BUG_ON(lstream >= pblk->nr_lstream);
	}
	log = &(pblk->lstream[lstream]);

//	if (log->map_type == PAGE_MAP)
//		printk("pblk_trans_map_get: start lba:%lu lseg %lu map %d loff %d\n", lba, lseg, pba.m.map, loff);
//	printk("pblk_trans_map_get: start lba:%lu lseg %lu map %d loff %d\n", lba, lseg, pba.m.map, loff);

#ifdef CONFIG_PBLK_DFTL
	spin_lock(&pblk->cmt_mgt.map_lock);
	pblk_load_cmt(pblk, lseg, 0);
	spin_unlock(&pblk->cmt_mgt.map_lock);
#else
error
#endif

	if (log->map_type == SECTOR_MAP)
	{
		struct ppa_addr* smap = (struct ppa_addr*) pba.pointer;
		if (pba.m.map != SECTOR_MAP) {
			printk("pba.m.map:%u\n", pba.m.map);
			BUG_ON(1);
		}
		ppa = smap[loff];
		if (ppa.c.is_cached == 0)
		{
			if (pblk->l_mg.nr_lines <= ppa.g.blk)
			{
				printk("SECTOR_MAP1: lba:%lu loff:%d lseg:%lu blk:%d pl:%d sec:%d map:%d lstream:%d\n", lba, loff, lseg, ppa.g.blk, ppa.g.pl, ppa.g.sec, pba.m.map, lstream);
				BUG_ON(1);
			}
		}
		return ppa;
	}
	else if (log->map_type == BLOCK_MAP)
	{
		if (pba.m.map != BLOCK_MAP)
			BUG_ON(1);

		if (pba.m.is_cached == 1) {
			struct ppa_addr* bmtlog = (struct ppa_addr*) pba.pointer;
			ppa = bmtlog[loff];
		}
		else if (pba.pba == ADDR_EMPTY) {
			pblk_ppa_set_empty(&ppa);
		}
		else if (loff < 8) {
			pblk_ppa_set_empty(&ppa);
		}
		else if (loff > entry_num - 17) {
			pblk_ppa_set_empty(&ppa);
		}
		else 
		{
			ppa.ppa = 0;
			ppa.g.blk = pba.b.blk;
			ppa.g.pg = (lba & pblk->ppaf.pg_mask) 
					>> pblk->ppaf.pg_offset;
			ppa.g.lun = (lba & pblk->ppaf.lun_mask) 
					>> pblk->ppaf.lun_offset;
			ppa.g.ch = (lba & pblk->ppaf.ch_mask) 
					>> pblk->ppaf.ch_offset;
			ppa.g.pl = (lba & pblk->ppaf.pln_mask) 
					>> pblk->ppaf.pln_offset;
			ppa.g.sec = (lba & pblk->ppaf.sec_mask) 
					>> pblk->ppaf.sec_offset;

			if (pblk->l_mg.nr_lines <= ppa.g.blk)
			{
				printk("BLOCK_MAP: lba:%lu loff:%d lseg:%lu blk:%d pl:%d sec:%d map:%d lstream:%d\n", lba, loff, lseg, ppa.g.blk, ppa.g.pl, ppa.g.sec, pba.m.map, lstream);
				BUG_ON(1);
			}
		}
	}
	else if (log->map_type == PAGE_MAP)
	{
		struct pba_addr* pmap = (struct pba_addr*) pba.pointer;
		int pindex = loff >> pblk->ppaf.pln_offset;
		int poffset = (lba & ((1 << pblk->ppaf.pln_offset) - 1)); 
	//	printk("pindex:%d poffset:%d\n", pindex, poffset);
		if (pba.m.map != PAGE_MAP)
			BUG_ON(1);
		if (pmap[pindex].m.is_cached == 0) {
			if (pmap[pindex].pba == ADDR_EMPTY)
				pblk_ppa_set_empty(&ppa);
			else {
				ppa.ppa = 0;
				ppa.g.blk = pmap[pindex].b.blk;
				ppa.g.pg = pmap[pindex].b.pg;
				ppa.g.lun = pmap[pindex].b.lun;
				ppa.g.ch = pmap[pindex].b.ch;
				ppa.g.pl = pmap[pindex].b.pl;
				ppa.g.sec = poffset;
			}
			// printk("PAGE map_get nopmtlog lba:%lu pindex:%d; poffset:%d ppa.g.sec:%d\n", 
		//				lba, pindex, poffset, ppa.g.sec);
		} else {
			struct ppa_addr *pmap_log = (struct ppa_addr*) pmap[pindex].pointer;
			ppa = pmap_log[poffset];
			// printk("PAGE map_get pmtlog lba:%lu pindex:%d; poffset:%d ppa.g.sec:%d\n", 
		//				lba, pindex, poffset, ppa.g.sec);
		}
		return ppa;
	}
	else
		BUG_ON(1);

#endif
	return ppa;
}

#ifdef CONFIG_PBLK_MULTIMAP
static inline struct ppa_addr pblk_cache_invalidate(struct pblk *pblk, struct ppa_addr r_ppa)
{
	struct ppa_addr ppa = r_ppa;

	if (pblk_addr_in_cache(r_ppa))
	{
		struct pblk_rb *rb = &pblk->rb_ctx[r_ppa.c.nrb].rwb;
		struct pblk_w_ctx *w_ctx = pblk_rb_w_ctx(rb, r_ppa.c.line);
		unsigned int ring_pos = pblk_rb_wrap_pos(rb, r_ppa.c.line);
		struct pblk_rb_entry *entry = &rb->entries[ring_pos];
		int flags = READ_ONCE(entry->w_ctx.flags);

		if (flags & PBLK_INVALIDATE_ENTRY);
/*		else if (flags & PBLK_WRITTEN_DATA)
			flags |= PBLK_INVALIDATE_ENTRY;
		else if (flags & PBLK_SUBMITTED_ENTRY)
			flags |= PBLK_INVALIDATE_ENTRY;
			*/
		else if (flags & PBLK_RESERVED_ENTRY) {
			ppa = w_ctx->ppa;
			pblk_ppa_set_empty(&w_ctx->ppa);
//			flags &= ~PBLK_RESERVED_ENTRY;
			flags |= PBLK_INVALIDATE_ENTRY;
			smp_store_release(&entry->w_ctx.flags, flags);	
		}
		else if (flags & PBLK_WAITREAD_ENTRY) {
			ppa = w_ctx->ppa;
			pblk_ppa_set_empty(&w_ctx->ppa);
			flags &= ~PBLK_WAITREAD_ENTRY;
			flags |= PBLK_INVALIDATE_ENTRY;
			smp_store_release(&entry->w_ctx.flags, flags);	
		}
	}
	
	return ppa;
}

static inline struct ppa_addr pblk_cache_reserved_clear(struct pblk *pblk, struct ppa_addr r_ppa)
{
	struct ppa_addr ppa = r_ppa;
	if (r_ppa.c.is_reserved == 1)
	{
		struct pblk_rb *rb = &pblk->rb_ctx[r_ppa.c.nrb].rwb;
		struct pblk_w_ctx *w_ctx = pblk_rb_w_ctx(rb, r_ppa.c.line);
		unsigned int ring_pos = pblk_rb_wrap_pos(rb, r_ppa.c.line);
		struct pblk_rb_entry *entry = &rb->entries[ring_pos];
		int flags = READ_ONCE(entry->w_ctx.flags);
		ppa = w_ctx->ppa;
		pblk_ppa_set_empty(&w_ctx->ppa);
		if (flags & PBLK_RESERVED_ENTRY) {
			flags &= ~PBLK_RESERVED_ENTRY;
		}
		else if (flags & PBLK_WAITREAD_ENTRY) {
			flags &= ~PBLK_WAITREAD_ENTRY;
		}
		else {
			printk("pblk_cache_reserved_clear: %d\n", flags); 
			BUG_ON(1);
		}
		flags |= PBLK_INVALIDATE_ENTRY;
		/*
		if (flags & PBLK_SUBMITTED_ENTRY)
			flags |= PBLK_INVALIDATE_ENTRY;
		else {
			flags &= ~PBLK_WRITTEN_DATA;
			flags &= ~PBLK_WRITABLE_ENTRY;
			flags &= ~PBLK_RESERVED_ENTRY;
			flags &= ~PBLK_WAITREAD_ENTRY;
			flags |= PBLK_INVALIDATE_ENTRY;
		}*/
		smp_store_release(&entry->w_ctx.flags, flags);	
	}
	
	return ppa;
}

static inline struct ppa_addr pblk_get_reserved_old_addr(struct pblk *pblk, struct ppa_addr r_ppa)
{
	struct ppa_addr ppa = r_ppa;

	if (pblk_addr_in_cache(r_ppa))
	{
		struct pblk_rb *rb = &pblk->rb_ctx[r_ppa.c.nrb].rwb;
		struct pblk_w_ctx *w_ctx = pblk_rb_w_ctx(rb, r_ppa.c.line);
		unsigned int ring_pos = pblk_rb_wrap_pos(rb, r_ppa.c.line);
		struct pblk_rb_entry *entry = &rb->entries[ring_pos];
		int flags = READ_ONCE(entry->w_ctx.flags);

		if (flags & PBLK_WRITTEN_DATA);
		else if (flags & PBLK_SUBMITTED_ENTRY);
		else if (flags & PBLK_RESERVED_ENTRY)
			ppa = w_ctx->ppa;
		else if (flags & PBLK_WAITREAD_ENTRY)
			ppa = w_ctx->ppa;
	}

	return ppa;
}

static inline struct ppa_addr pblk_get_reserved_old_addr2(struct pblk *pblk, struct ppa_addr r_ppa)
{
	struct ppa_addr ppa = r_ppa;

	if (pblk_addr_in_cache(r_ppa))
	{
		struct pblk_rb *rb = &pblk->rb_ctx[r_ppa.c.nrb].rwb;
		struct pblk_w_ctx *w_ctx = pblk_rb_w_ctx(rb, r_ppa.c.line);
		unsigned int ring_pos = pblk_rb_wrap_pos(rb, r_ppa.c.line);
		struct pblk_rb_entry *entry = &rb->entries[ring_pos];
		int flags = READ_ONCE(entry->w_ctx.flags);

		if (flags & PBLK_WRITTEN_DATA);
		else if (flags & PBLK_SUBMITTED_ENTRY)
			ppa = w_ctx->ppa;
		else if (flags & PBLK_RESERVED_ENTRY)
			ppa = w_ctx->ppa;
		else if (flags & PBLK_WAITREAD_ENTRY)
			ppa = w_ctx->ppa;
	}

	return ppa;
}

/*
enum EXT4_STREAM
{
	META_STREAM = 0,
	INODE_STREAM = 1, 
	JOURNAL_STREAM = 2,
	DIR_STREAM = 3,
	DATA_0_STREAM = 4,
	DATA_1_STREAM = 5,
	DATA_2_STREAM = 6,
}
*/

static inline int pblk_get_lstream(struct pblk *pblk,
								sector_t lba)
{
#ifndef EXT4_TEST
	struct pba_addr *bmap = (struct pba_addr *)pblk->trans_map;
	sector_t lseg = lba >> pblk->ppaf.blk_offset;

	struct pba_addr pba = bmap[lseg];

	return pba.m.lstream;
#else
	unsigned long group_number = lba / 524288;

	if (lba >= 1081344 && lba < 1114112)
		return JOURNAL_STREAM;

	if (group_number == 0) {
		if (lba < 793)
			return MISC_STREAM;
		else if (lba < 8983)
			return INODE_STREAM;
		else
			return DIR_STREAM; 
	}
	else if (group_number == 5) {
		if (lba < 2621470)
			return MISC_STREAM;
		else if (lba < 2628638 + 512)
			return INODE_STREAM;
		else
			return DIR_STREAM; 
	}
	if (lba < group_number * 524288 + 32)
		return MISC_STREAM;
	else if (lba < group_number * 524288 + 8224)
		return INODE_STREAM;
	else
		return DIR_STREAM;

#if 0 // OP=20
	if (lba >= 1606441 && lba < 1639209)
		return JOURNAL_STREAM;

	if (group_number == 0) {
		if (lba < 841) 
			return MISC_STREAM;
		else if (lba < 9033)
			return INODE_STREAM;
		else
			return DIR_STREAM; 
	}
	else if (group_number == 6) {
		if (lba < 3145738) 
			return MISC_STREAM;
		else if (lba < 3148278)
			return INODE_STREAM;
		else
			return DIR_STREAM; 
	}
	if (lba < group_number * 524288 + 32)
		return MISC_STREAM;
	else if (lba < group_number * 524288 + 8224)
		return INODE_STREAM;
	else
		return DIR_STREAM;

#endif



#endif
}


static inline int pblk_get_maptype(struct pblk *pblk,
								sector_t lba)
{
	struct pba_addr *bmap = (struct pba_addr *)pblk->trans_map;
	sector_t lseg = lba >> pblk->ppaf.blk_offset;

	struct pba_addr pba = bmap[lseg];

	return pba.m.map;
}

static inline int pblk_get_log_maptype(struct pblk *pblk,
								sector_t lba)
{
	struct pba_addr *bmap = (struct pba_addr *)pblk->trans_map;
	sector_t lseg = lba >> pblk->ppaf.blk_offset;

	struct pba_addr pba = bmap[lseg];
	struct pblk_lstream *log;
	int lstream = (u64)pba.m.lstream;
	BUG_ON(lstream >= pblk->nr_lstream);
	log = &(pblk->lstream[lstream]); 

	return log->map_type;
}

static inline int pblk_get_reserved_addr(struct pblk *pblk,
								sector_t lba, struct ppa_addr *r_ppa)
{
	struct pba_addr *bmap = (struct pba_addr *)pblk->trans_map;
	sector_t lseg = lba >> pblk->ppaf.blk_offset;
	struct pba_addr pba;

//	printk("pblk_get_reserved_addr: lba:%lu lseg:%d\n", lba, lseg);
	pba = bmap[lseg];
	if (pba.m.map == PAGE_MAP) {
		//printk("PAGEMAP pblk_get_reserved_addr: lba:%lu lseg:%d\n", lba, lseg);
		*r_ppa = pblk_trans_map_get(pblk, lba);
		if (r_ppa->c.is_reserved == 1 && (!pblk_ppa_empty(*r_ppa))) {
			unsigned int pos = pblk_rb_wrap_pos(&pblk->rb_ctx[r_ppa->c.nrb].rwb, r_ppa->c.line);
			struct pblk_rb_entry *entry = &(pblk->rb_ctx[r_ppa->c.nrb].rwb.entries[pos]);
			int flags = READ_ONCE(entry->w_ctx.flags);
//			printk("lba:%lu pos:%d flags:0x%x\n", lba, pos, flags);
			if (flags & PBLK_RESERVED_ENTRY)
				return 1;
		}
		return 0;
	}
//	printk("pblk_get_reserved_addr end\n");
	return -1;
}

static inline void pblk_empty_map_set(struct pblk *pblk, sector_t lba)
{
	struct pba_addr *bmap = (struct pba_addr *)pblk->trans_map;
	sector_t lseg = lba >> pblk->ppaf.blk_offset;
	struct pba_addr pba = bmap[lseg];

	pblk->preinvalid_map[lba] = 0;

	if (pba.m.map == SECTOR_MAP)
	{
		vfree(pba.pointer);
#ifdef CONFIG_NVM_DEBUG
		atomic_long_dec(&pblk->nr_sectormap);
		atomic_long_inc(&pblk->nr_blockmap);
#endif
	}
	else if (pba.m.map == BLOCK_MAP)
	{
		if (pba.m.is_cached == 1)
		{
			vfree(pba.pointer);
#ifdef CONFIG_NVM_DEBUG
			atomic_long_dec(&pblk->nr_blocklog);
#endif	
		}
	}
	else if (pba.m.map == PAGE_MAP)
	{
		struct pba_addr *pmap = (struct pba_addr*) pba.pointer;
		int pmap_entry_num = (1 << pblk->ppaf.blk_offset) >> pblk->ppaf.pln_offset;
		int i;
		for (i = 0; i < pmap_entry_num; i++)
		{
			if (pmap[i].m.is_cached == 1) {
				vfree(pmap[i].pointer);
				pmap[i].m.is_cached = 0;
#ifdef CONFIG_NVM_DEBUG
				atomic_long_dec(&pblk->nr_pagelog);
#endif
			}
		}
		vfree(pba.pointer);

#ifdef CONFIG_NVM_DEBUG
		atomic_long_dec(&pblk->nr_pagemap);
		atomic_long_inc(&pblk->nr_blockmap);
#endif
	}
#ifdef CONFIG_PBLK_DFTL
	spin_lock(&pblk->cmt_mgt.map_lock);
	pblk_delete_cmt(pblk, lseg);
	spin_unlock(&pblk->cmt_mgt.map_lock);
#endif
	pba.m.map = BLOCK_MAP;
	pba.pba = ADDR_EMPTY;
	pba.m.is_cached = 0;
	bmap[lseg] = pba;

	#ifdef CONFIG_PBLK_DFTL
	spin_lock(&pblk->cmt_mgt.map_lock);
	pblk_change_map(pblk, lseg);
	spin_unlock(&pblk->cmt_mgt.map_lock);
	#endif
}
#endif

static inline u32 pblk_ppa64_to_ppa32(struct pblk *pblk, struct ppa_addr ppa64)
{
	u32 ppa32 = 0;

	if (ppa64.ppa == ADDR_EMPTY) {
		ppa32 = ~0U;
	} else if (ppa64.c.is_cached) {
		ppa32 |= ppa64.c.line;
		ppa32 |= 1U << 31;
	} else {
		ppa32 |= ppa64.g.blk << pblk->ppaf.blk_offset;
		ppa32 |= ppa64.g.pg << pblk->ppaf.pg_offset;
		ppa32 |= ppa64.g.lun << pblk->ppaf.lun_offset;
		ppa32 |= ppa64.g.ch << pblk->ppaf.ch_offset;
		ppa32 |= ppa64.g.pl << pblk->ppaf.pln_offset;
		ppa32 |= ppa64.g.sec << pblk->ppaf.sec_offset;
	}

	return ppa32;
}

static inline int pblk_check_page_align(struct pblk *pblk, sector_t start_lba, 
															struct ppa_addr *copy_ppa)
{
	u32 ppa32[4]; 
	
	if ((start_lba % 4) != 0)
		printk("wrong start_lba: %lu\n", start_lba);

	copy_ppa[0] = pblk_trans_map_get(pblk, start_lba);
	copy_ppa[1] = pblk_trans_map_get(pblk, start_lba + 1);
	copy_ppa[2] = pblk_trans_map_get(pblk, start_lba + 2);
	copy_ppa[3] = pblk_trans_map_get(pblk, start_lba + 3);

	if (copy_ppa[0].ppa == ADDR_EMPTY && copy_ppa[1].ppa == ADDR_EMPTY &&
			copy_ppa[2].ppa == ADDR_EMPTY && copy_ppa[3].ppa == ADDR_EMPTY) {
			return 1;
	}
	else if(copy_ppa[0].c.is_cached == 0 && copy_ppa[1].c.is_cached == 0 &&
		copy_ppa[2].c.is_cached == 0 && copy_ppa[3].c.is_cached == 0)
	{
		ppa32[0] = pblk_ppa64_to_ppa32(pblk, copy_ppa[0]);
		ppa32[1] = pblk_ppa64_to_ppa32(pblk, copy_ppa[1]);
		ppa32[2] = pblk_ppa64_to_ppa32(pblk, copy_ppa[2]);
		ppa32[3] = pblk_ppa64_to_ppa32(pblk, copy_ppa[3]);

		if (ppa32[1] == ppa32[0] + 1 && ppa32[2] == ppa32[0] + 2 &&
				ppa32[3] == ppa32[0] + 3 && copy_ppa[0].g.sec == 0) {
			return 1;
		}
	}

	return 0;
}

static inline void pblk_setting_page_map(struct pblk *pblk, sector_t lba)
{
	struct pba_addr *bmap = (struct pba_addr *)pblk->trans_map;
	sector_t lseg = lba >> pblk->ppaf.blk_offset;
	int entry_num = (1 << pblk->ppaf.blk_offset);
	sector_t start_lba = lseg << pblk->ppaf.blk_offset;

	int pmap_entry_num = entry_num >> pblk->ppaf.pln_offset;
	struct pba_addr *pmap;
	int index = 0;
	int old_type = bmap[lseg].m.map;
	int i;

	pmap = vzalloc(sizeof(struct pba_addr) * pmap_entry_num);
#ifdef CONFIG_NVM_DEBUG
	atomic_long_inc(&pblk->nr_pagemap);
#endif

	for (i = 0; i < entry_num; i+=4, index++)
	{
		struct pba_addr page_addr;
		struct ppa_addr copy_ppa[4];
		struct ppa_addr *pmt_log;
		int found;

		page_addr.pba = ADDR_EMPTY;

		found = pblk_check_page_align(pblk, start_lba+i, copy_ppa);

		if (found) {
			if (copy_ppa[0].ppa != ADDR_EMPTY) {
				page_addr.pba = 0;
				page_addr.b.blk = copy_ppa[0].g.blk;
				page_addr.b.pg = copy_ppa[0].g.pg;
				page_addr.b.pl = copy_ppa[0].g.pl;
				page_addr.b.lun = copy_ppa[0].g.lun;
				page_addr.b.ch = copy_ppa[0].g.ch;
			}
			page_addr.m.is_cached = 0;

			pmap[index] = page_addr;
			continue;
		}

		page_addr.pointer = vzalloc(sizeof(struct ppa_addr) * 4);
		#ifdef CONFIG_NVM_DEBUG
		atomic_long_inc(&pblk->nr_pagelog);
		#endif
		page_addr.m.is_cached = 1;
		pmt_log = (struct ppa_addr*) page_addr.pointer;
		pmt_log[0] = copy_ppa[0];
		pmt_log[1] = copy_ppa[1];
		pmt_log[2] = copy_ppa[2];
		pmt_log[3] = copy_ppa[3];

		pmap[index] = page_addr;
	}
	
	if(old_type == SECTOR_MAP) {
		vfree(bmap[lseg].pointer);
		#ifdef CONFIG_NVM_DEBUG
		atomic_long_dec(&pblk->nr_sectormap);
		#endif
	} else if(old_type == BLOCK_MAP) {
		if (bmap[lseg].m.is_cached == 1) {
			vfree(bmap[lseg].pointer);
		#ifdef CONFIG_NVM_DEBUG
			atomic_long_dec(&pblk->nr_blocklog);
		#endif
			bmap[lseg].m.is_cached = 0;
		}
		#ifdef CONFIG_NVM_DEBUG
		atomic_long_dec(&pblk->nr_blockmap);
		#endif
	}
	bmap[lseg].pointer = pmap;
	bmap[lseg].m.map = PAGE_MAP;

	#ifdef CONFIG_PBLK_DFTL
	spin_lock(&pblk->cmt_mgt.map_lock);
	pblk_change_map(pblk, lseg);
	spin_unlock(&pblk->cmt_mgt.map_lock);
	#endif
}


static inline void pblk_trans_map_set(struct pblk *pblk, sector_t lba,
						struct ppa_addr ppa)
{
#ifdef CONFIG_PBLK_MULTIMAP
	struct pba_addr *bmap = (struct pba_addr *)pblk->trans_map;
	sector_t lseg = lba >> pblk->ppaf.blk_offset;
	int entry_num = (1 << pblk->ppaf.blk_offset);
	struct pba_addr pba = bmap[lseg];
	int lstream;
	struct pblk_lstream *log;
	int loff = lba & (entry_num - 1);

	pblk->preinvalid_map[lba] = 0;

//	printk("pblk_trans_map_set start %lu %lu\n", lba, lseg);

	if (pba.m.map == NONE_MAP) {
		struct ppa_addr empty_ppa;
		struct ppa_addr *smap; 
		int i;
		pblk_ppa_set_empty(&empty_ppa);
		smap = vzalloc(8 * entry_num);
		#ifdef CONFIG_NVM_DEBUG
		atomic_long_inc(&pblk->nr_definemap);
		atomic_long_inc(&pblk->nr_sectormap);
		#endif

		if (smap == NULL)
			BUG_ON(1);

		for (i = 0; i < entry_num; i++)
		{
			smap[i] = empty_ppa;
		}
		pba.pointer = smap;
		pba.m.map = SECTOR_MAP;
		pba.m.lstream = 0;
		pba.m.is_cached = 0;
		bmap[lseg] = pba;
		printk("none_map allocation(map_set): %lu %lu\n", lba, lseg);
		#ifdef CONFIG_PBLK_DFTL
		spin_lock(&pblk->cmt_mgt.map_lock);
		pblk_change_map(pblk, lseg);
		spin_unlock(&pblk->cmt_mgt.map_lock);
		#else
		error
		#endif
	}
	#ifdef CONFIG_PBLK_DFTL
	spin_lock(&pblk->cmt_mgt.map_lock);
	pblk_load_cmt(pblk, lseg, 1);
	spin_unlock(&pblk->cmt_mgt.map_lock);
	#else
	error
	#endif

	lstream = (u64)pba.m.lstream;
	BUG_ON(lstream >= pblk->nr_lstream);
	log = &(pblk->lstream[lstream]); 
//	printk("set lstream %d map_type %d lseg %ld\n", lstream, log->map_type, lseg);
	if (log->map_type == SECTOR_MAP)
	{
		struct ppa_addr *smap; 
		if (pba.m.map != SECTOR_MAP)
		{
			BUG_ON(1);
		}
		smap = pba.pointer;
		smap[loff] = ppa;
	}
	else if (log->map_type == BLOCK_MAP)
	{
		struct ppa_addr *bmtlog;
		if ((loff < 8) || (loff >= entry_num - 16))
			return;

		if (pba.m.is_cached == 0)
		{
			sector_t start_lba = lba - loff;
			int i;
			bmtlog = (struct ppa_addr*) vzalloc(8 * entry_num);
			for (i = 0; i < entry_num; i++) {
				struct ppa_addr copy_ppa = pblk_trans_map_get(pblk, start_lba + i);
				bmtlog[i].ppa = copy_ppa.ppa;
			}
			atomic_long_inc(&pblk->nr_blocklog);
			#ifdef CONFIG_PBLK_DFTL
			spin_lock(&pblk->cmt_mgt.map_lock);
			pblk_alloc_log(pblk, lseg, BLOCK_MAP, entry_num);
			spin_unlock(&pblk->cmt_mgt.map_lock);
			#else
			error
			#endif
			pba.m.is_cached = 1;
			pba.pointer = bmtlog;
			bmap[lseg] = pba;
		}
		else
			bmtlog = pba.pointer;

		bmtlog[loff] = ppa;

		if (loff == entry_num - 17)
		{
			if (pblk_ppa_empty(bmtlog[8]) && pblk_ppa_empty(bmtlog[entry_num - 17]))
			{
				#ifdef CONFIG_PBLK_DFTL
				spin_lock(&pblk->cmt_mgt.map_lock);
				pblk_free_log(pblk, lseg, BLOCK_MAP, entry_num);
				spin_unlock(&pblk->cmt_mgt.map_lock);
				#else
				error
				#endif
				vfree(bmtlog);
				atomic_long_dec(&pblk->nr_blocklog);
				pba.m.is_cached = 0;
				pba.pba = ADDR_EMPTY; 
				bmap[lseg] = pba; 
			}
			else {
				printk("8: %d entry-16:%d\n", pblk_ppa_empty(bmtlog[8]), 
								pblk_ppa_empty(bmtlog[entry_num - 17]));
			}
		}
	}
	else if (log->map_type == PAGE_MAP)
	{
		struct pba_addr *pmap;
		int pindex = loff >> pblk->ppaf.pln_offset;
		int poffset = (lba & ((1 << pblk->ppaf.pln_offset) - 1)); 

		int log_entry_num = 1 << pblk->ppaf.pln_offset;
		struct ppa_addr *pmtlog;

		if (pba.m.map != PAGE_MAP) {
			BUG_ON(1);
			pblk_setting_page_map(pblk, lba);
		}
		pmap = (struct pba_addr*) bmap[lseg].pointer;

		if (pmap[pindex].m.is_cached == 0) 
		{
			int i;
			sector_t start_lba = lba - poffset;

			pmtlog = (struct ppa_addr*) vzalloc(8 * log_entry_num);
			if (pmtlog == NULL)
				BUG_ON(1);
			#ifdef CONFIG_NVM_DEBUG
			atomic_long_inc(&pblk->nr_pagelog);
			#endif
			#ifdef CONFIG_PBLK_DFTL
			spin_lock(&pblk->cmt_mgt.map_lock);
			pblk_alloc_log(pblk, lseg, PAGE_MAP, log_entry_num);
			spin_unlock(&pblk->cmt_mgt.map_lock);
			#endif
			for (i = 0; i < log_entry_num; i++) {
				struct ppa_addr copy_ppa = pblk_trans_map_get(pblk, start_lba + i);
				pmtlog[i].ppa = copy_ppa.ppa;
			}
			pmap[pindex].pointer = pmtlog;
			pmap[pindex].m.is_cached = 1;
		}
		else
			pmtlog = pmap[pindex].pointer;

		pmtlog[poffset] = ppa;

		if ((poffset == (log_entry_num -1)) && !pblk_addr_in_cache(ppa))
		{
			struct ppa_addr copy_ppa[4];
			sector_t start_lba = lba - poffset;

			if (pblk_check_page_align(pblk, start_lba, copy_ppa)) {
				struct pba_addr page_addr;

				if (copy_ppa[0].ppa == ADDR_EMPTY)
					page_addr.pba = copy_ppa[0].ppa;
				else {
					page_addr.pba = 0;
					page_addr.b.blk = copy_ppa[0].g.blk;
					page_addr.b.pg = copy_ppa[0].g.blk;
					page_addr.b.pl = copy_ppa[0].g.pl;
					page_addr.b.lun = copy_ppa[0].g.lun;
					page_addr.b.ch = copy_ppa[0].g.ch;
				}
				#ifdef CONFIG_PBLK_DFTL
				spin_lock(&pblk->cmt_mgt.map_lock);
				pblk_free_log(pblk, lseg, PAGE_MAP, log_entry_num);
				spin_unlock(&pblk->cmt_mgt.map_lock);
				#endif
				page_addr.m.is_cached = 0;
				vfree(pmtlog);
				#ifdef CONFIG_NVM_DEBUG
				atomic_long_dec(&pblk->nr_pagelog);
				#endif
				pmap[pindex] = page_addr;
			}
			else
			{
				printk("map_set no align: 0 %d %d %d %d %d %d %d, 1 %d %d %d %d %d %d %d, 2 %d %d %d %d %d %d %d, 3 %d %d %d %d %d %d %d\n",
				copy_ppa[0].c.is_cached, copy_ppa[0].g.blk, copy_ppa[0].g.pg, copy_ppa[0].g.sec, copy_ppa[0].g.pl, copy_ppa[0].g.lun, copy_ppa[0].g.ch,
				copy_ppa[1].c.is_cached, copy_ppa[1].g.blk, copy_ppa[1].g.pg, copy_ppa[1].g.sec, copy_ppa[1].g.pl, copy_ppa[1].g.lun, copy_ppa[1].g.ch,
				copy_ppa[2].c.is_cached, copy_ppa[2].g.blk, copy_ppa[2].g.pg, copy_ppa[2].g.sec, copy_ppa[2].g.pl, copy_ppa[2].g.lun, copy_ppa[2].g.ch,
				copy_ppa[3].c.is_cached, copy_ppa[3].g.blk, copy_ppa[3].g.pg, copy_ppa[3].g.sec, copy_ppa[3].g.pl, copy_ppa[3].g.lun, copy_ppa[3].g.ch);
			}
		}
	}
	else {
		printk("GC Cache: lstream %d map_type %d lseg %ld\n", lstream, log->map_type, lseg);
		BUG_ON(1);
	}
#else
	if (pblk->ppaf_bitsize < 32) {
		u32 *map = (u32 *)pblk->trans_map;

		map[lba] = pblk_ppa64_to_ppa32(pblk, ppa);
	} else {
		u64 *map = (u64 *)pblk->trans_map;

		map[lba] = ppa.ppa;
	}
#endif
}


#ifdef CONFIG_PBLK_MULTIMAP
static inline void pblk_dev_map_set(struct pblk *pblk, sector_t lba,
				struct ppa_addr ppa)
{
	struct pba_addr *bmap = (struct pba_addr *)pblk->trans_map;
	sector_t lseg = lba >> pblk->ppaf.blk_offset;
	struct pba_addr pba = bmap[lseg];
	int lstream = (int)pba.m.lstream;
	int entry_num = (1ULL << pblk->ppaf.blk_offset);
	int loff = lba & (entry_num - 1);
	struct pblk_lstream *log;

	BUG_ON(lstream >= pblk->nr_lstream);

	log = &(pblk->lstream[lstream]);

	#ifdef CONFIG_PBLK_DFTL
	spin_lock(&pblk->cmt_mgt.map_lock);
	pblk_load_cmt(pblk, lseg, 1);
	spin_unlock(&pblk->cmt_mgt.map_lock);
	#else
	error
	#endif

	if (log->map_type == SECTOR_MAP)
	{
		u64 *map; 
		if (pba.m.map != SECTOR_MAP)
			BUG_ON(1);
		map = (u64 *)pba.pointer;
		map[loff] = ppa.ppa;
	}
	else if (log->map_type == BLOCK_MAP)
	{
		struct ppa_addr *bmtlog;
		int poff = 0; 
		poff |= (ppa.g.pg << pblk->ppaf.pg_offset);
		poff |= (ppa.g.lun << pblk->ppaf.lun_offset);
		poff |= (ppa.g.ch << pblk->ppaf.ch_offset);
		poff |= (ppa.g.pl << pblk->ppaf.pln_offset);
		poff |= (ppa.g.sec << pblk->ppaf.sec_offset);

		if (loff != poff) {
			printk("pblk_dev_map_set %lu %d\n", lba, lstream);
			printk("BLOCK DEV: l:%d p:%d p:%d %d %d %d %d %d\n", loff, poff, ppa.g.blk, 
						ppa.g.pg, ppa.g.sec, ppa.g.pl, ppa.g.lun, ppa.g.ch);
			BUG_ON(1);
		}

		if (pba.m.is_cached == 0) {
			printk("dev_map_set: lstream %d map_type %d lseg %ld loff %d\n", lstream, log->map_type, lseg, loff);
			BUG_ON(1);
		}
		bmtlog = pba.pointer;

		bmtlog[loff] = ppa;
		if (loff == entry_num - 17)
		{
			if ((!pblk_addr_in_cache(bmtlog[8])) && (!pblk_ppa_empty(bmtlog[8])) && 
						(pblk_dev_ppa_to_line(bmtlog[8]) == pblk_dev_ppa_to_line(bmtlog[loff])))
			{
#ifdef CONFIG_PBLK_DFTL
				spin_lock(&pblk->cmt_mgt.map_lock);
				pblk_free_log(pblk, lseg, BLOCK_MAP, entry_num);
				spin_unlock(&pblk->cmt_mgt.map_lock);
#endif
#ifdef CONFIG_NVM_DEBUG
				atomic_long_dec(&pblk->nr_blocklog);
#endif
				vfree(pba.pointer);
				pba.pba = 0;
				pba.m.is_cached = 0;
				pba.b.blk = ppa.g.blk;
				bmap[lseg] = pba; 
			}
			else {
				printk("bmt[8]: c%d e:%d line:%d   bmt[%d]: line:%d\n", pblk_addr_in_cache(bmtlog[8]),
								pblk_ppa_empty(bmtlog[8]), pblk_dev_ppa_to_line(bmtlog[8]),
								loff, pblk_dev_ppa_to_line(bmtlog[loff]));
			}
		}
		else if (loff > (entry_num - 17))
		{
			BUG_ON(1);
		}
	}
	else if (log->map_type == PAGE_MAP)
	{
		struct pba_addr *pmap;
		int loff = lba & (entry_num - 1);
		int pindex = loff >> pblk->ppaf.pln_offset;
		int poffset = (lba & ((1 << pblk->ppaf.pln_offset) - 1)); 

		int log_entry_num = 1 << pblk->ppaf.pln_offset;
		struct ppa_addr *pmtlog;

		if (pba.m.map != PAGE_MAP) {
			BUG_ON(1);
		}
		pmap = (struct pba_addr*) bmap[lseg].pointer;

		if (pmap[pindex].m.is_cached == 0) {
			BUG_ON(1);
		}
		else
			pmtlog = (struct ppa_addr*) pmap[pindex].pointer;

		pmtlog[poffset].ppa = ppa.ppa;

		if (poffset == (log_entry_num - 1)) {
			struct ppa_addr copy_ppa[4];
			sector_t start_lba = lba - poffset;
			
			if (pblk_check_page_align(pblk, start_lba, copy_ppa)) {
				struct pba_addr page_addr;

				if (copy_ppa[0].ppa == ADDR_EMPTY)
					page_addr.pba = copy_ppa[0].ppa;
				else
				{
					page_addr.pba = 0;
					page_addr.b.blk = copy_ppa[0].g.blk;
					page_addr.b.pg = copy_ppa[0].g.pg;
					page_addr.b.pl = copy_ppa[0].g.pl;
					page_addr.b.lun = copy_ppa[0].g.lun;
					page_addr.b.ch = copy_ppa[0].g.ch;
				}
				#ifdef CONFIG_PBLK_DFTL
				spin_lock(&pblk->cmt_mgt.map_lock);
				pblk_free_log(pblk, lseg, PAGE_MAP, log_entry_num);
				spin_unlock(&pblk->cmt_mgt.map_lock);
				#endif

				page_addr.m.is_cached = 0;
				vfree(pmtlog);
#ifdef CONFIG_NVM_DEBUG
				atomic_long_dec(&pblk->nr_pagelog);
#endif
				pmap[pindex] = page_addr;
			}
			else {
				printk("dev_map_set no align: start_lba:%lu lba:%lu poffset:%d ppa: %d %d %d %d %d %d\n", 
								start_lba, lba, poffset, ppa.c.is_cached, ppa.g.blk, ppa.g.pg, ppa.g.sec, ppa.g.pl, ppa.g.ch);
				printk("dev_map_set no align: off0 c%d r%d %d %d %d %d %d %d, off1 c%d r%d %d %d %d %d %d %d, off2 c%d r%d %d %d %d %d %d %d, off3 c%d r%d %d %d %d %d %d %d\n",
				copy_ppa[0].c.is_cached, copy_ppa[0].c.is_reserved, copy_ppa[0].g.blk, copy_ppa[0].g.pg, copy_ppa[0].g.sec, copy_ppa[0].g.pl, copy_ppa[0].g.lun, copy_ppa[0].g.ch,
				copy_ppa[1].c.is_cached, copy_ppa[0].c.is_reserved, copy_ppa[1].g.blk, copy_ppa[1].g.pg, copy_ppa[1].g.sec, copy_ppa[1].g.pl, copy_ppa[1].g.lun, copy_ppa[1].g.ch,
				copy_ppa[2].c.is_cached, copy_ppa[2].c.is_reserved, copy_ppa[2].g.blk, copy_ppa[2].g.pg, copy_ppa[2].g.sec, copy_ppa[2].g.pl, copy_ppa[2].g.lun, copy_ppa[2].g.ch,
				copy_ppa[3].c.is_cached, copy_ppa[3].c.is_reserved,copy_ppa[3].g.blk, copy_ppa[3].g.pg, copy_ppa[3].g.sec, copy_ppa[3].g.pl, copy_ppa[3].g.lun, copy_ppa[3].g.ch);
			}
		}

	}
//	printk("pblk_dev_map_set end\n");
}

static inline int pblk_cache_map_set(struct pblk *pblk, sector_t lba,
				struct ppa_addr ppa)
{
	struct pba_addr *bmap = (struct pba_addr *)pblk->trans_map;
	sector_t lseg = lba >> pblk->ppaf.blk_offset;
	struct pba_addr pba = bmap[lseg];
	int entry_num = (1 << pblk->ppaf.blk_offset);
	struct pblk_lstream *log;
	int lstream;
	int loff = lba & (entry_num - 1);

	pblk->preinvalid_map[lba] = 0;

	if (pba.m.map == NONE_MAP) {
		u64 *smap = vzalloc(8 * entry_num);
		struct ppa_addr empty_ppa;
		int i;
		pblk_ppa_set_empty(&empty_ppa);

		if (smap == NULL)
			BUG_ON(1);

		#ifdef CONFIG_NVM_DEBUG
		atomic_long_inc(&pblk->nr_definemap);
		atomic_long_inc(&pblk->nr_sectormap);
		#endif

		#ifdef CONFIG_PBLK_DFTL
		spin_lock(&pblk->cmt_mgt.map_lock);
		pblk_change_map(pblk, lseg);
		spin_unlock(&pblk->cmt_mgt.map_lock);
		#endif

		for (i = 0; i < entry_num; i++) {
			smap[i] = empty_ppa.ppa;
		}
		smap[loff] = ppa.ppa;

		pba.m.lstream = 0;
		pba.m.is_cached = 0;
		pba.m.map = SECTOR_MAP;
		pba.pointer = smap;
		bmap[lseg] = pba;
		printk("none_map allocation(cache_set): %lu %lu\n", lba, lseg);
		return 0;
	}

	#ifdef CONFIG_PBLK_DFTL
	spin_lock(&pblk->cmt_mgt.map_lock);
	pblk_load_cmt(pblk, lseg, 1);
	spin_unlock(&pblk->cmt_mgt.map_lock);
	#else
	error
	#endif

	lstream = (u64)pba.m.lstream;
	BUG_ON(lstream >= pblk->nr_lstream);

	log = &(pblk->lstream[lstream]); 

//	if (log->map_type == PAGE_MAP)
//		printk("cache_map_set: lstream %d map_type %d lseg %ld loff %d\n", lstream, log->map_type, lseg, loff);

	if (log->map_type == SECTOR_MAP)
	{
		u64 *map;
		if (pba.m.map != SECTOR_MAP)
		{
			BUG_ON(1);
		}
		map = (u64 *)pba.pointer;
		map[loff] = ppa.ppa;
	}
	else if (log->map_type == BLOCK_MAP) 
	{
		struct ppa_addr *bmtlog; 
		int i;
		
		if (pba.m.is_cached == 0) {
			sector_t start_lba = lba - loff;
			bmtlog = (struct ppa_addr*) vzalloc(8 * entry_num);
			if (loff != 8) {
				printk("cache_map_set: lstream %d map_type %d lseg %ld loff %d\n", lstream, log->map_type, lseg, loff);
				BUG_ON(1);
			}
			for (i = 0; i < entry_num; i++) {
				struct ppa_addr copy_ppa = pblk_trans_map_get(pblk, start_lba + i);
				bmtlog[i] = copy_ppa;
			}
			#ifdef CONFIG_PBLK_DFTL
			spin_lock(&pblk->cmt_mgt.map_lock);
			pblk_alloc_log(pblk, lseg, BLOCK_MAP, entry_num);
			spin_unlock(&pblk->cmt_mgt.map_lock);
			#endif
			pba.m.is_cached = 1;
			atomic_long_inc(&pblk->nr_blocklog);

			pba.pointer = bmtlog;
			bmap[lseg] = pba;
		}
		else
			bmtlog = (struct ppa_addr*) pba.pointer;

		bmtlog[loff] = ppa;
	}
	else if (log->map_type == PAGE_MAP)
	{
		struct pba_addr *pmap;
		int pindex = loff >> pblk->ppaf.pln_offset;
		int poffset = (lba & ((1 << pblk->ppaf.pln_offset) - 1));
		struct ppa_addr *pmtlog;

	//	printk("cache_map: %lu %d %d %d\n", lba, loff, pindex, poffset);

		if (pba.m.map != PAGE_MAP) {
			BUG_ON(1);
			pblk_setting_page_map(pblk, lba);
		}
		pmap = (struct pba_addr*) bmap[lseg].pointer;

		if (pmap[pindex].m.is_cached == 0) 
		{
			int log_entry_num = 1 << pblk->ppaf.pln_offset;
			sector_t start_lba = lba - poffset;
			int i;
			pmtlog = (struct ppa_addr*) vzalloc(8 * log_entry_num);
			atomic_long_inc(&pblk->nr_pagelog);
			#ifdef CONFIG_PBLK_DFTL
			spin_lock(&pblk->cmt_mgt.map_lock);
			pblk_alloc_log(pblk, lseg, PAGE_MAP, log_entry_num);
			spin_unlock(&pblk->cmt_mgt.map_lock);
			#endif
			for (i = 0; i < log_entry_num; i++) {
				struct ppa_addr copy_ppa = pblk_trans_map_get(pblk, start_lba + i);
				pmtlog[i].ppa = copy_ppa.ppa;
			}
			pmap[pindex].pointer = pmtlog;
			pmap[pindex].m.is_cached = 1;
		}
		else
			pmtlog = pmap[pindex].pointer;

		pmtlog[poffset] = ppa;
	}

	return 0;
}
#endif


static inline u64 pblk_dev_ppa_to_line_addr(struct pblk *pblk,
							struct ppa_addr p)
{
	u64 paddr;

	paddr = 0;
	paddr |= (u64)p.g.pg << pblk->ppaf.pg_offset;
	paddr |= (u64)p.g.lun << pblk->ppaf.lun_offset;
	paddr |= (u64)p.g.ch << pblk->ppaf.ch_offset;
	paddr |= (u64)p.g.pl << pblk->ppaf.pln_offset;
	paddr |= (u64)p.g.sec << pblk->ppaf.sec_offset;

	return paddr;
}

#ifdef CONFIG_PBLK_MULTIMAP
static inline int pblk_addr_reserved(struct ppa_addr ppa)
{
	return (ppa.ppa != ADDR_EMPTY && ppa.c.is_cached && ppa.c.is_reserved);
}
#endif
static inline u32 pblk_calc_meta_header_crc(struct pblk *pblk,
					    struct line_header *header)
{
	u32 crc = ~(u32)0;

	crc = crc32_le(crc, (unsigned char *)header + sizeof(crc),
				sizeof(struct line_header) - sizeof(crc));

	return crc;
}

static inline u32 pblk_calc_smeta_crc(struct pblk *pblk,
				      struct line_smeta *smeta)
{
	struct pblk_line_meta *lm = &pblk->lm;
	u32 crc = ~(u32)0;

	crc = crc32_le(crc, (unsigned char *)smeta +
				sizeof(struct line_header) + sizeof(crc),
				lm->smeta_len -
				sizeof(struct line_header) - sizeof(crc));

	return crc;
}

static inline u32 pblk_calc_emeta_crc(struct pblk *pblk,
				      struct line_emeta *emeta)
{
	struct pblk_line_meta *lm = &pblk->lm;
	u32 crc = ~(u32)0;

	crc = crc32_le(crc, (unsigned char *)emeta +
				sizeof(struct line_header) + sizeof(crc),
				lm->emeta_len[0] -
				sizeof(struct line_header) - sizeof(crc));

	return crc;
}

static inline int pblk_set_progr_mode(struct pblk *pblk, int type)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	int flags;

	flags = geo->plane_mode >> 1;

	if (type == WRITE)
		flags |= NVM_IO_SCRAMBLE_ENABLE;

	return flags;
}

enum {
	PBLK_READ_RANDOM	= 0,
	PBLK_READ_SEQUENTIAL	= 1,
};

static inline int pblk_set_read_mode(struct pblk *pblk, int type)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	int flags;

	flags = NVM_IO_SUSPEND | NVM_IO_SCRAMBLE_ENABLE;
	if (type == PBLK_READ_SEQUENTIAL)
		flags |= geo->plane_mode >> 1;

	return flags;
}

static inline int pblk_io_aligned(struct pblk *pblk, int nr_secs)
{
	return !(nr_secs % pblk->min_write_pgs);
}

#ifdef CONFIG_NVM_DEBUG
static inline void print_ppa(struct ppa_addr *p, char *msg, int error)
{
	if (p->c.is_cached) {
		WARN(1, "cache_line\n");
		pr_err("ppa: (%s: %x) cache line: %llu\n",
				msg, error, (u64)p->c.line);
	} else {
		WARN(1, "ppa\n");
		pr_err("ppa: (%s: %x):ch:%d,lun:%d,blk:%d,pg:%d,pl:%d,sec:%d\n",
			msg, error,
			p->g.ch, p->g.lun, p->g.blk,
			p->g.pg, p->g.pl, p->g.sec);
	}
}

static inline void pblk_print_failed_rqd(struct pblk *pblk, struct nvm_rq *rqd,
					 int error)
{
	int bit = -1;

	if (rqd->nr_ppas ==  1) {
		print_ppa(&rqd->ppa_addr, "rqd", error);
		return;
	}

	while ((bit = find_next_bit((void *)&rqd->ppa_status, rqd->nr_ppas,
						bit + 1)) < rqd->nr_ppas) {
		print_ppa(&rqd->ppa_list[bit], "rqd", error);
	}

	pr_err("error:%d, ppa_status:%llx\n", error, rqd->ppa_status);
}
#endif

static inline int pblk_boundary_ppa_checks(struct nvm_tgt_dev *tgt_dev,
				       struct ppa_addr *ppas, int nr_ppas)
{
	struct nvm_geo *geo = &tgt_dev->geo;
	struct ppa_addr *ppa;
	int i;

	for (i = 0; i < nr_ppas; i++) {
		ppa = &ppas[i];

		if (!ppa->c.is_cached &&
				ppa->g.ch < geo->nr_chnls &&
				ppa->g.lun < geo->luns_per_chnl &&
				ppa->g.pl < geo->nr_planes &&
				ppa->g.blk < geo->blks_per_lun &&
				ppa->g.pg < geo->pgs_per_blk &&
				ppa->g.sec < geo->sec_per_pg)
			continue;

#ifdef CONFIG_NVM_DEBUG
		print_ppa(ppa, "boundary", i);
#endif
		return 1;
	}
	return 0;
}

static inline int pblk_boundary_paddr_checks(struct pblk *pblk, u64 paddr)
{
	struct pblk_line_meta *lm = &pblk->lm;

	if (paddr > lm->sec_per_line)
		return 1;

	return 0;
}

static inline unsigned int pblk_get_bi_idx(struct bio *bio)
{
	return bio->bi_iter.bi_idx;
}

static inline sector_t pblk_get_lba(struct bio *bio)
{
	return bio->bi_iter.bi_sector / NR_PHY_IN_LOG;
}

static inline unsigned int pblk_get_secs(struct bio *bio)
{
	return  bio->bi_iter.bi_size / PBLK_EXPOSED_PAGE_SIZE;
}

static inline sector_t pblk_get_sector(sector_t lba)
{
	return lba * NR_PHY_IN_LOG;
}

static inline void pblk_setup_uuid(struct pblk *pblk)
{
	uuid_le uuid;

	uuid_le_gen(&uuid);
	memcpy(pblk->instance_uuid, uuid.b, 16);
}

#ifdef CONFIG_PBLK_MULTIMAP
static inline int pblk_set_lstream(struct pblk *pblk,
								sector_t lba, u8 lstream)
{
	struct pba_addr *bmap = (struct pba_addr *)pblk->trans_map;
	sector_t lseg = lba >> pblk->ppaf.blk_offset;
	int old_lstream;
	int entry_num = (1 << pblk->ppaf.blk_offset);
	int i;
	struct pblk_lstream *log, *log_old;

//	printk("set_lstream: lseg %lu stream %d\n",lseg, lstream);

	if (lstream >= pblk->nr_lstream)
		lstream = 0;

	if (bmap[lseg].m.map == NONE_MAP)
	{
		bmap[lseg].m.map = pblk->lstream[lstream].map_type;
#ifdef CONFIG_NVM_DEBUG
		atomic_long_inc(&pblk->nr_definemap);
#endif
		if (bmap[lseg].m.map == SECTOR_MAP)
		{
			struct ppa_addr ppa;
			struct ppa_addr *smap; 
			pblk_ppa_set_empty(&ppa);
			smap = vzalloc(8 * entry_num);
			if (smap == NULL)
				BUG_ON(1);
#ifdef CONFIG_NVM_DEBUG
			atomic_long_inc(&pblk->nr_sectormap);
#endif
			bmap[lseg].pointer = smap;
			bmap[lseg].m.map = SECTOR_MAP;
			for (i = 0; i < entry_num; i++)
			{
				smap[i] = ppa;
			}
		}
		else if (bmap[lseg].m.map == BLOCK_MAP)
		{
			bmap[lseg].pba = ADDR_EMPTY; 
			bmap[lseg].m.map = BLOCK_MAP;
#ifdef CONFIG_NVM_DEBUG
			atomic_long_inc(&pblk->nr_blockmap);
#endif
		}
		else if (bmap[lseg].m.map == PAGE_MAP)
		{
			int pmap_entry_num = entry_num >> pblk->ppaf.pln_offset;
			struct pba_addr *pmap = vzalloc(sizeof(struct pba_addr) * pmap_entry_num);
			struct pba_addr page_addr;
			page_addr.pba = ADDR_EMPTY;
			page_addr.m.is_cached = 0;

			for (i = 0; i < pmap_entry_num; i++) {
				pmap[i] = page_addr;
			}
			bmap[lseg].m.map = PAGE_MAP;
#ifdef CONFIG_NVM_DEBUG
			atomic_long_inc(&pblk->nr_pagemap);
#endif
		}
		bmap[lseg].m.is_cached = 0;
		bmap[lseg].m.lstream = lstream;
		#ifdef CONFIG_PBLK_DFTL
		spin_lock(&pblk->cmt_mgt.map_lock);
		pblk_change_map(pblk, lseg);
		spin_unlock(&pblk->cmt_mgt.map_lock);
		#endif
		return 0;
	}

	old_lstream = bmap[lseg].m.lstream;
	log = &(pblk->lstream[lstream]);
	log_old = &(pblk->lstream[old_lstream]);
#if 0
	if (log->map_type != log_old->map_type)
	{
		sector_t start_lba = lseg << pblk->ppaf.blk_offset;
		int i;
		for (i = 0; i < entry_num; i++)
		{
			struct ppa_addr copy_ppa = pblk_trans_map_get(pblk, start_lba + i);
			if (pblk_addr_in_cache(copy_ppa)) {
				copy_ppa = pblk_cache_invalidate(pblk, copy_ppa);	
			}
			if (!pblk_addr_in_cache(copy_ppa) && !pblk_ppa_empty(copy_ppa))
				pblk_map_invalidate(pblk, copy_ppa);
		}

		if (log_old->map_type == SECTOR_MAP) {
			atomic_long_dec(&pblk->nr_sectormap);
			vfree(bmap[lseg].pointer);
		} else if (log_old->map_type == PAGE_MAP) {
			struct pba_addr *pmap = (struct pba_addr*) bmap[lseg].pointer;
			int pmap_entry_num = (1 << pblk->ppaf.blk_offset) >> pblk->ppaf.pln_offset;
			for (i = 0; i < pmap_entry_num; i++)
			{
				if (pmap[i].m.is_cached == 1) {
					vfree(pmap[i].pointer);
#ifdef CONFIG_NVM_DEBUG
					atomic_long_dec(&pblk->nr_pagelog);
#endif
				}
			}
			vfree(bmap[lseg].pointer);
#ifdef CONFIG_NVM_DEBUG
			atomic_long_dec(&pblk->nr_pagemap);
#endif
		} else if (log_old->map_type == BLOCK_MAP) {
			if (bmap[lseg].m.is_cached == 1)
			{
				vfree(bmap[lseg].pointer);
#ifdef CONFIG_NVM_DEBUG
				atomic_long_dec(&pblk->nr_blocklog);
#endif
			}
#ifdef CONFIG_NVM_DEBUG
			atomic_long_dec(&pblk->nr_blockmap);
#endif
		}

		if (log->map_type == SECTOR_MAP)
		{
			struct ppa_addr *smap;
			smap = (struct ppa_addr*) vzalloc(8 * entry_num);
			if (smap == NULL)
				BUG_ON(1);
			for (i = 0; i < entry_num; i++) {
				pblk_ppa_set_empty(&smap[i]);	
			}
			bmap[lseg].pointer = smap;
			bmap[lseg].m.map = SECTOR_MAP;
			bmap[lseg].m.is_cached = 0;
			#ifdef CONFIG_NVM_DEBUG
			atomic_long_inc(&pblk->nr_sectormap);
			#endif
		}
		else if (log->map_type == PAGE_MAP)
		{
			struct pba_addr *pmap;
			int pmap_entry_num = (1 << pblk->ppaf.blk_offset) >> pblk->ppaf.pln_offset;
			pmap = (struct pba_addr*) vzalloc(8 * entry_num);

			for (i = 0; i < pmap_entry_num; i++) {
				pmap[i].pba = ADDR_EMPTY;
			}
			bmap[lseg].pointer = pmap;
			bmap[lseg].m.map = PAGE_MAP;
			bmap[lseg].m.is_cached = 0;
#ifdef CONFIG_NVM_DEBUG
			atomic_long_inc(&pblk->nr_pagemap);
#endif
		}
		else if (log->map_type == BLOCK_MAP)
		{
			bmap[lseg].pba = ADDR_EMPTY;
			bmap[lseg].m.map = BLOCK_MAP;
			bmap[lseg].m.is_cached = 0;
#ifdef CONFIG_NVM_DEBUG
			atomic_long_inc(&pblk->nr_blockmap);
#endif
		}
	}
#endif	

#if 1
	if (log->map_type == SECTOR_MAP)
	{
		if (log_old->map_type != SECTOR_MAP)
		{
			struct ppa_addr *smap;
			sector_t start_lba = lseg << pblk->ppaf.blk_offset;
			if (bmap[lseg].m.map != SECTOR_MAP) {
				smap = (struct ppa_addr*) vzalloc(8 * entry_num);
				if (smap == NULL)
					BUG_ON(1);
			} else {
				smap = (struct ppa_addr*) bmap[lseg].pointer;
				BUG_ON(1);
			}
			for (i = 0; i < entry_num; i++)
			{
				struct ppa_addr copy_ppa = pblk_trans_map_get(pblk, start_lba + i);
				if (pblk_addr_in_cache(copy_ppa)) {
					struct pblk_rb *rb = &pblk->rb_ctx[copy_ppa.c.nrb].rwb;
					struct pblk_w_ctx *w_ctx = pblk_rb_w_ctx(rb, copy_ppa.c.line);
					unsigned int ring_pos = pblk_rb_wrap_pos(rb, copy_ppa.c.line);
					struct pblk_rb_entry *entry = &rb->entries[ring_pos];
					int flags = READ_ONCE(entry->w_ctx.flags);

					if (flags & PBLK_INVALIDATE_ENTRY);
					else if (flags & PBLK_WRITTEN_DATA);
					else if (flags & PBLK_SUBMITTED_ENTRY);
					else if (flags & PBLK_RESERVED_ENTRY || flags & PBLK_WAITREAD_ENTRY)
					{
						copy_ppa = w_ctx->ppa;
						pblk_ppa_set_empty(&entry->w_ctx.ppa);
						flags &= ~PBLK_RESERVED_ENTRY;
						flags &= ~PBLK_WAITREAD_ENTRY;
						flags |= PBLK_INVALIDATE_ENTRY;
						smp_store_release(&entry->w_ctx.flags, flags);
					}
				}
				smap[i] = copy_ppa;
			}
			if (bmap[lseg].m.map == PAGE_MAP)
			{
				struct pba_addr *pmap = (struct pba_addr*) bmap[lseg].pointer;
				int pmap_entry_num = (1 << pblk->ppaf.blk_offset) >> pblk->ppaf.pln_offset;
				int i;
				for (i = 0; i < pmap_entry_num; i++)
				{
					if (pmap[i].m.is_cached == 1) {
						vfree(pmap[i].pointer);
#ifdef CONFIG_NVM_DEBUG
						atomic_long_dec(&pblk->nr_pagelog);
#endif
					}
				}
				vfree(bmap[lseg].pointer);
#ifdef CONFIG_NVM_DEBUG
				atomic_long_dec(&pblk->nr_pagemap);
#endif
			} else if (bmap[lseg].m.map == BLOCK_MAP)
			{
				if (bmap[lseg].m.is_cached == 1)
				{
					vfree(bmap[lseg].pointer);
#ifdef CONFIG_NVM_DEBUG
					atomic_long_dec(&pblk->nr_blocklog);
#endif
				}
#ifdef CONFIG_NVM_DEBUG
				atomic_long_dec(&pblk->nr_blockmap);
#endif
			}
			else
				BUG_ON(1);
#ifdef CONFIG_NVM_DEBUG
			atomic_long_inc(&pblk->nr_sectormap);
#endif
			bmap[lseg].pointer = smap;
			bmap[lseg].m.map = SECTOR_MAP;
			bmap[lseg].m.is_cached = 0;
			#ifdef CONFIG_PBLK_DFTL
			spin_lock(&pblk->cmt_mgt.map_lock);
			pblk_change_map(pblk, lseg);
			spin_unlock(&pblk->cmt_mgt.map_lock);
			#endif
		}
	}
	else if ((log->map_type == BLOCK_MAP) && (log_old->map_type != BLOCK_MAP))
	{
		struct ppa_addr ppa;
		sector_t lba_temp;
		sector_t lba_start = lseg << pblk->ppaf.blk_offset;
		for (lba_temp = lba_start; lba_temp < lba_start + entry_num; lba_temp++)
		{
			ppa = pblk_trans_map_get(pblk, lba_temp);
			if (pblk_addr_in_cache(ppa)) {
				ppa = pblk_cache_invalidate(pblk, ppa);
			}
			if (!pblk_addr_in_cache(ppa) && !pblk_ppa_empty(ppa))
				pblk_map_invalidate(pblk, ppa);
		}
		pblk_empty_map_set(pblk, lba);
	}
	else if (log->map_type == PAGE_MAP)
	{
		if (log_old->map_type != PAGE_MAP) {
			pblk_setting_page_map(pblk, lba);
		}
	}
#endif 
	bmap[lseg].m.lstream = lstream;
	// printk("pblk_set_lstream: %lu %lu %u\n", lba, lseg, lstream);
	return 0;
}
#endif


#endif /* PBLK_H_ */
