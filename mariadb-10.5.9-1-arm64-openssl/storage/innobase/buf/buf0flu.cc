/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2021, MariaDB Corporation.
Copyright (c) 2013, 2014, Fusion-io

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file buf/buf0flu.cc
The database buffer buf_pool flush algorithm

Created 11/11/1995 Heikki Tuuri
*******************************************************/

#include "univ.i"
#include <my_service_manager.h>
#include <mysql/service_thd_wait.h>
#include <sql_class.h>

#include "buf0flu.h"
#include "buf0buf.h"
#include "buf0checksum.h"
#include "buf0dblwr.h"
#include "srv0start.h"
#include "page0zip.h"
#include "fil0fil.h"
#include "log0crypt.h"
#include "srv0mon.h"
#include "fil0pagecompress.h"
#ifdef UNIV_LINUX
/* include defs for CPU time priority settings */
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/resource.h>
#endif /* UNIV_LINUX */
#ifdef HAVE_LZO
# include "lzo/lzo1x.h"
#elif defined HAVE_SNAPPY
# include "snappy-c.h"
#endif

/** Number of pages flushed via LRU. Protected by buf_pool.mutex.
Also included in buf_flush_page_count. */
ulint buf_lru_flush_page_count;

/** Number of pages flushed. Protected by buf_pool.mutex. */
ulint buf_flush_page_count;

/** Flag indicating if the page_cleaner is in active state. */
bool buf_page_cleaner_is_active;

/** Factor for scan length to determine n_pages for intended oldest LSN
progress */
static constexpr ulint buf_flush_lsn_scan_factor = 3;

/** Average redo generation rate */
static lsn_t lsn_avg_rate = 0;

/** Target oldest_modification for the page cleaner; writes are protected by
buf_pool.flush_list_mutex */
static Atomic_relaxed<lsn_t> buf_flush_sync_lsn;

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t page_cleaner_thread_key;
#endif /* UNIV_PFS_THREAD */

/** Page cleaner structure */
static struct
{
  /** total elapsed time in adaptive flushing, in seconds */
  ulint flush_time;
  /** number of adaptive flushing passes */
  ulint flush_pass;
} page_cleaner;

#ifdef UNIV_DEBUG
my_bool innodb_page_cleaner_disabled_debug;
#endif /* UNIV_DEBUG */

/** If LRU list of a buf_pool is less than this size then LRU eviction
should not happen. This is because when we do LRU flushing we also put
the blocks on free list. If LRU list is very small then we can end up
in thrashing. */
#define BUF_LRU_MIN_LEN		256

/* @} */

#ifdef UNIV_DEBUG
/** Validate the flush list. */
static void buf_flush_validate_low();

/** Validates the flush list some of the time. */
static void buf_flush_validate_skip()
{
/** Try buf_flush_validate_low() every this many times */
# define BUF_FLUSH_VALIDATE_SKIP	23

	/** The buf_flush_validate_low() call skip counter.
	Use a signed type because of the race condition below. */
	static int buf_flush_validate_count = BUF_FLUSH_VALIDATE_SKIP;

	/* There is a race condition below, but it does not matter,
	because this call is only for heuristic purposes. We want to
	reduce the call frequency of the costly buf_flush_validate_low()
	check in debug builds. */
	if (--buf_flush_validate_count > 0) {
		return;
	}

	buf_flush_validate_count = BUF_FLUSH_VALIDATE_SKIP;
	buf_flush_validate_low();
}
#endif /* UNIV_DEBUG */

/** Wake up the page cleaner if needed */
inline void buf_pool_t::page_cleaner_wakeup()
{
  if (!page_cleaner_idle())
    return;
  double dirty_pct= double(UT_LIST_GET_LEN(buf_pool.flush_list)) * 100.0 /
    double(UT_LIST_GET_LEN(buf_pool.LRU) + UT_LIST_GET_LEN(buf_pool.free));
  double pct_lwm= srv_max_dirty_pages_pct_lwm;
  if ((pct_lwm != 0.0 && pct_lwm <= dirty_pct) ||
      srv_max_buf_pool_modified_pct <= dirty_pct)
  {
    page_cleaner_is_idle= false;
    pthread_cond_signal(&do_flush_list);
  }
}

/** Insert a modified block into the flush list.
@param[in,out]	block	modified block
@param[in]	lsn	oldest modification */
void buf_flush_insert_into_flush_list(buf_block_t* block, lsn_t lsn)
{
	mysql_mutex_assert_not_owner(&buf_pool.mutex);
	mysql_mutex_assert_owner(&log_sys.flush_order_mutex);
	ut_ad(lsn);
	ut_ad(!fsp_is_system_temporary(block->page.id().space()));

	mysql_mutex_lock(&buf_pool.flush_list_mutex);
	block->page.set_oldest_modification(lsn);
	MEM_CHECK_DEFINED(block->page.zip.data
			  ? block->page.zip.data : block->frame,
			  block->physical_size());
	buf_pool.stat.flush_list_bytes += block->physical_size();
	ut_ad(buf_pool.stat.flush_list_bytes <= buf_pool.curr_pool_size);

	UT_LIST_ADD_FIRST(buf_pool.flush_list, &block->page);
	ut_d(buf_flush_validate_skip());
	buf_pool.page_cleaner_wakeup();
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);
}

/** Remove a block from buf_pool.flush_list */
static void buf_flush_remove_low(buf_page_t *bpage)
{
  ut_ad(!fsp_is_system_temporary(bpage->id().space()));
  mysql_mutex_assert_owner(&buf_pool.mutex);
  mysql_mutex_assert_owner(&buf_pool.flush_list_mutex);
  ut_ad(!bpage->oldest_modification());
  buf_pool.flush_hp.adjust(bpage);
  UT_LIST_REMOVE(buf_pool.flush_list, bpage);
  buf_pool.stat.flush_list_bytes -= bpage->physical_size();
#ifdef UNIV_DEBUG
  buf_flush_validate_skip();
#endif /* UNIV_DEBUG */
}

/** Remove a block from the flush list of modified blocks.
@param[in,out]	bpage	block to be removed from the flush list */
static void buf_flush_remove(buf_page_t *bpage)
{
  bpage->clear_oldest_modification();
  buf_flush_remove_low(bpage);
}

/** Remove all dirty pages belonging to a given tablespace when we are
deleting the data file of that tablespace.
The pages still remain a part of LRU and are evicted from
the list as they age towards the tail of the LRU.
@param id    tablespace identifier */
void buf_flush_remove_pages(ulint id)
{
  const page_id_t first(id, 0), end(id + 1, 0);
  ut_ad(id);
  mysql_mutex_lock(&buf_pool.mutex);

  for (;;)
  {
    bool deferred= false;

    mysql_mutex_lock(&buf_pool.flush_list_mutex);

    for (buf_page_t *bpage= UT_LIST_GET_LAST(buf_pool.flush_list); bpage; )
    {
      ut_d(const auto s= bpage->state());
      ut_ad(s == BUF_BLOCK_ZIP_PAGE || s == BUF_BLOCK_FILE_PAGE ||
            s == BUF_BLOCK_REMOVE_HASH);
      buf_page_t *prev= UT_LIST_GET_PREV(list, bpage);

      const page_id_t bpage_id(bpage->id());

      if (bpage_id < first || bpage_id >= end);
      else if (bpage->io_fix() != BUF_IO_NONE)
        deferred= true;
      else
        buf_flush_remove(bpage);

      bpage= prev;
    }

    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    if (!deferred)
      break;

    mysql_mutex_unlock(&buf_pool.mutex);
    os_thread_yield();
    mysql_mutex_lock(&buf_pool.mutex);
    buf_flush_wait_batch_end(false);
  }

  mysql_mutex_unlock(&buf_pool.mutex);
}

/** Try to flush all the dirty pages that belong to a given tablespace.
@param id    tablespace identifier
@return number dirty pages that there were for this tablespace */
ulint buf_flush_dirty_pages(ulint id)
{
  ut_ad(!sync_check_iterate(dict_sync_check()));

  ulint n= 0;

  mysql_mutex_lock(&buf_pool.flush_list_mutex);

  for (buf_page_t *bpage= UT_LIST_GET_FIRST(buf_pool.flush_list); bpage;
       bpage= UT_LIST_GET_NEXT(list, bpage))
  {
    ut_d(const auto s= bpage->state());
    ut_ad(s == BUF_BLOCK_ZIP_PAGE || s == BUF_BLOCK_FILE_PAGE ||
          s == BUF_BLOCK_REMOVE_HASH);
    ut_ad(bpage->oldest_modification());
    if (id == bpage->id().space())
      n++;
  }
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  if (n)
    buf_flush_lists(srv_max_io_capacity, LSN_MAX);
  return n;
}

/*******************************************************************//**
Relocates a buffer control block on the flush_list.
Note that it is assumed that the contents of bpage have already been
copied to dpage.
IMPORTANT: When this function is called bpage and dpage are not
exact copies of each other. For example, they both will have different
::state. Also the ::list pointers in dpage may be stale. We need to
use the current list node (bpage) to do the list manipulation because
the list pointers could have changed between the time that we copied
the contents of bpage to the dpage and the flush list manipulation
below. */
ATTRIBUTE_COLD
void
buf_flush_relocate_on_flush_list(
/*=============================*/
	buf_page_t*	bpage,	/*!< in/out: control block being moved */
	buf_page_t*	dpage)	/*!< in/out: destination block */
{
	buf_page_t*	prev;

	mysql_mutex_assert_owner(&buf_pool.mutex);
	ut_ad(!fsp_is_system_temporary(bpage->id().space()));

	if (!bpage->oldest_modification()) {
		return;
	}

	mysql_mutex_lock(&buf_pool.flush_list_mutex);

	/* FIXME: At this point we have both buf_pool and flush_list
	mutexes. Theoretically removal of a block from flush list is
	only covered by flush_list mutex but currently we do
	have buf_pool mutex in buf_flush_remove() therefore this block
	is guaranteed to be in the flush list. We need to check if
	this will work without the assumption of block removing code
	having the buf_pool mutex. */
	ut_ad(dpage->oldest_modification());

	/* Important that we adjust the hazard pointer before removing
	the bpage from the flush list. */
	buf_pool.flush_hp.adjust(bpage);

	bpage->clear_oldest_modification();

	prev = UT_LIST_GET_PREV(list, bpage);
	UT_LIST_REMOVE(buf_pool.flush_list, bpage);

	if (prev) {
		ut_ad(prev->oldest_modification());
		UT_LIST_INSERT_AFTER(buf_pool.flush_list, prev, dpage);
	} else {
		UT_LIST_ADD_FIRST(buf_pool.flush_list, dpage);
	}

	ut_d(buf_flush_validate_low());
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);
}

/** Complete write of a file page from buf_pool.
@param request write request */
void buf_page_write_complete(const IORequest &request)
{
  ut_ad(request.is_write());
  ut_ad(!srv_read_only_mode/* ||
        request.node->space->purpose == FIL_TYPE_TEMPORARY*/);
  buf_page_t *bpage= request.bpage;
  ut_ad(bpage);
  ut_ad(bpage->in_file());
  ut_ad(bpage->io_fix() == BUF_IO_WRITE);
  ut_ad(!buf_dblwr.is_inside(bpage->id()));
  bool dblwr;
  if (bpage->status == buf_page_t::INIT_ON_FLUSH)
  {
    bpage->status= buf_page_t::NORMAL;
    dblwr= false;
  }
  else
  {
    ut_ad(bpage->status == buf_page_t::NORMAL);
    dblwr= request.node->space->use_doublewrite();
  }

  /* We do not need protect io_fix here by mutex to read it because
  this and buf_page_read_complete() are the only functions where we can
  change the value from BUF_IO_READ or BUF_IO_WRITE to some other
  value, and our code ensures that this is the only thread that handles
  the i/o for this block. */
  if (bpage->slot)
  {
    bpage->slot->release();
    bpage->slot= nullptr;
  }

  if (UNIV_UNLIKELY(MONITOR_IS_ON(MONITOR_MODULE_BUF_PAGE)))
    buf_page_monitor(bpage, BUF_IO_WRITE);
  DBUG_PRINT("ib_buf", ("write page %u:%u",
                        bpage->id().space(), bpage->id().page_no()));
  ut_ad(request.is_LRU() ? buf_pool.n_flush_LRU : buf_pool.n_flush_list);
  const bool temp= fsp_is_system_temporary(bpage->id().space());

  mysql_mutex_lock(&buf_pool.mutex);
  bpage->set_io_fix(BUF_IO_NONE);
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  ut_ad(!temp || bpage->oldest_modification() == 1);
  bpage->clear_oldest_modification();

  if (!temp)
    buf_flush_remove_low(bpage);
  else
    ut_ad(request.is_LRU());

  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  if (dblwr)
  {
    ut_ad(!fsp_is_system_temporary(bpage->id().space()));
    buf_dblwr.write_completed();
  }

  /* Because this thread which does the unlocking might not be the same that
  did the locking, we use a pass value != 0 in unlock, which simply
  removes the newest lock debug record, without checking the thread id. */
  if (bpage->state() == BUF_BLOCK_FILE_PAGE)
    rw_lock_sx_unlock_gen(&((buf_block_t*) bpage)->lock, BUF_IO_WRITE);

  buf_pool.stat.n_pages_written++;

  if (request.is_LRU())
  {
    buf_LRU_free_page(bpage, true);
    if (!--buf_pool.n_flush_LRU)
      pthread_cond_broadcast(&buf_pool.done_flush_LRU);
  }
  else
  {
    if (!--buf_pool.n_flush_list)
      pthread_cond_broadcast(&buf_pool.done_flush_list);
  }

  mysql_mutex_unlock(&buf_pool.mutex);
}

/** Calculate a ROW_FORMAT=COMPRESSED page checksum and update the page.
@param[in,out]	page		page to update
@param[in]	size		compressed page size */
void buf_flush_update_zip_checksum(buf_frame_t *page, ulint size)
{
  ut_ad(size > 0);
  mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM,
                  page_zip_calc_checksum(page, size,
                                         static_cast<srv_checksum_algorithm_t>
                                         (srv_checksum_algorithm)));
}

/** Assign the full crc32 checksum for non-compressed page.
@param[in,out]	page	page to be updated */
void buf_flush_assign_full_crc32_checksum(byte* page)
{
	ut_d(bool compressed = false);
	ut_d(bool corrupted = false);
	ut_d(const uint size = buf_page_full_crc32_size(page, &compressed,
							&corrupted));
	ut_ad(!compressed);
	ut_ad(!corrupted);
	ut_ad(size == uint(srv_page_size));
	const ulint payload = srv_page_size - FIL_PAGE_FCRC32_CHECKSUM;
	mach_write_to_4(page + payload, ut_crc32(page, payload));
}

/** Initialize a page for writing to the tablespace.
@param[in]	block			buffer block; NULL if bypassing
					the buffer pool
@param[in,out]	page			page frame
@param[in,out]	page_zip_		compressed page, or NULL if
					uncompressed
@param[in]	use_full_checksum	whether tablespace uses full checksum */
void
buf_flush_init_for_writing(
	const buf_block_t*	block,
	byte*			page,
	void*			page_zip_,
	bool			use_full_checksum)
{
	if (block != NULL && block->frame != page) {
		/* If page is encrypted in full crc32 format then
		checksum stored already as a part of fil_encrypt_buf() */
		ut_ad(use_full_checksum);
		return;
	}

	ut_ad(block == NULL || block->frame == page);
	ut_ad(block == NULL || page_zip_ == NULL
	      || &block->page.zip == page_zip_);
	ut_ad(page);

	if (page_zip_) {
		page_zip_des_t*	page_zip;
		ulint		size;

		page_zip = static_cast<page_zip_des_t*>(page_zip_);
		size = page_zip_get_size(page_zip);

		ut_ad(size);
		ut_ad(ut_is_2pow(size));
		ut_ad(size <= UNIV_ZIP_SIZE_MAX);

		switch (fil_page_get_type(page)) {
		case FIL_PAGE_TYPE_ALLOCATED:
		case FIL_PAGE_INODE:
		case FIL_PAGE_IBUF_BITMAP:
		case FIL_PAGE_TYPE_FSP_HDR:
		case FIL_PAGE_TYPE_XDES:
			/* These are essentially uncompressed pages. */
			memcpy(page_zip->data, page, size);
			/* fall through */
		case FIL_PAGE_TYPE_ZBLOB:
		case FIL_PAGE_TYPE_ZBLOB2:
		case FIL_PAGE_INDEX:
		case FIL_PAGE_RTREE:
			buf_flush_update_zip_checksum(page_zip->data, size);
			return;
		}

		ib::error() << "The compressed page to be written"
			" seems corrupt:";
		ut_print_buf(stderr, page, size);
		fputs("\nInnoDB: Possibly older version of the page:", stderr);
		ut_print_buf(stderr, page_zip->data, size);
		putc('\n', stderr);
		ut_error;
	}

	if (use_full_checksum) {
		static_assert(FIL_PAGE_FCRC32_END_LSN % 4 == 0, "aligned");
		static_assert(FIL_PAGE_LSN % 4 == 0, "aligned");
		memcpy_aligned<4>(page + srv_page_size
				  - FIL_PAGE_FCRC32_END_LSN,
				  FIL_PAGE_LSN + 4 + page, 4);
		return buf_flush_assign_full_crc32_checksum(page);
	}

	static_assert(FIL_PAGE_END_LSN_OLD_CHKSUM % 8 == 0, "aligned");
	static_assert(FIL_PAGE_LSN % 8 == 0, "aligned");
	memcpy_aligned<8>(page + srv_page_size - FIL_PAGE_END_LSN_OLD_CHKSUM,
			  FIL_PAGE_LSN + page, 8);

	if (block && srv_page_size == 16384) {
		/* The page type could be garbage in old files
		created before MySQL 5.5. Such files always
		had a page size of 16 kilobytes. */
		ulint	page_type = fil_page_get_type(page);
		ulint	reset_type = page_type;

		switch (block->page.id().page_no() % 16384) {
		case 0:
			reset_type = block->page.id().page_no() == 0
				? FIL_PAGE_TYPE_FSP_HDR
				: FIL_PAGE_TYPE_XDES;
			break;
		case 1:
			reset_type = FIL_PAGE_IBUF_BITMAP;
			break;
		case FSP_TRX_SYS_PAGE_NO:
			if (block->page.id()
			    == page_id_t(TRX_SYS_SPACE, TRX_SYS_PAGE_NO)) {
				reset_type = FIL_PAGE_TYPE_TRX_SYS;
				break;
			}
			/* fall through */
		default:
			switch (page_type) {
			case FIL_PAGE_INDEX:
			case FIL_PAGE_TYPE_INSTANT:
			case FIL_PAGE_RTREE:
			case FIL_PAGE_UNDO_LOG:
			case FIL_PAGE_INODE:
			case FIL_PAGE_IBUF_FREE_LIST:
			case FIL_PAGE_TYPE_ALLOCATED:
			case FIL_PAGE_TYPE_SYS:
			case FIL_PAGE_TYPE_TRX_SYS:
			case FIL_PAGE_TYPE_BLOB:
			case FIL_PAGE_TYPE_ZBLOB:
			case FIL_PAGE_TYPE_ZBLOB2:
				break;
			case FIL_PAGE_TYPE_FSP_HDR:
			case FIL_PAGE_TYPE_XDES:
			case FIL_PAGE_IBUF_BITMAP:
				/* These pages should have
				predetermined page numbers
				(see above). */
			default:
				reset_type = FIL_PAGE_TYPE_UNKNOWN;
				break;
			}
		}

		if (UNIV_UNLIKELY(page_type != reset_type)) {
			ib::info()
				<< "Resetting invalid page "
				<< block->page.id() << " type "
				<< page_type << " to "
				<< reset_type << " when flushing.";
			fil_page_set_type(page, reset_type);
		}
	}

	uint32_t checksum = BUF_NO_CHECKSUM_MAGIC;

	switch (srv_checksum_algorithm_t(srv_checksum_algorithm)) {
	case SRV_CHECKSUM_ALGORITHM_INNODB:
	case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:
		checksum = buf_calc_page_new_checksum(page);
		mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM,
				checksum);
		/* With the InnoDB checksum, we overwrite the first 4 bytes of
		the end lsn field to store the old formula checksum. Since it
		depends also on the field FIL_PAGE_SPACE_OR_CHKSUM, it has to
		be calculated after storing the new formula checksum. */
		checksum = buf_calc_page_old_checksum(page);
		break;
	case SRV_CHECKSUM_ALGORITHM_FULL_CRC32:
	case SRV_CHECKSUM_ALGORITHM_STRICT_FULL_CRC32:
	case SRV_CHECKSUM_ALGORITHM_CRC32:
	case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
		/* In other cases we write the same checksum to both fields. */
		checksum = buf_calc_page_crc32(page);
		mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM,
				checksum);
		break;
	case SRV_CHECKSUM_ALGORITHM_NONE:
	case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:
		mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM,
				checksum);
		break;
		/* no default so the compiler will emit a warning if
		new enum is added and not handled here */
	}

	mach_write_to_4(page + srv_page_size - FIL_PAGE_END_LSN_OLD_CHKSUM,
			checksum);
}

/** Reserve a buffer for compression.
@param[in,out]  slot    reserved slot */
static void buf_tmp_reserve_compression_buf(buf_tmp_buffer_t* slot)
{
  if (slot->comp_buf)
    return;
  /* Both Snappy and LZO compression methods require that the output
  buffer be bigger than input buffer. Adjust the allocated size. */
  ulint size= srv_page_size;
#ifdef HAVE_LZO
  size+= LZO1X_1_15_MEM_COMPRESS;
#elif defined HAVE_SNAPPY
  size= snappy_max_compressed_length(size);
#endif
  slot->comp_buf= static_cast<byte*>(aligned_malloc(size, srv_page_size));
}

/** Encrypt a buffer of temporary tablespace
@param[in]      offset  Page offset
@param[in]      s       Page to encrypt
@param[in,out]  d       Output buffer
@return encrypted buffer or NULL */
static byte* buf_tmp_page_encrypt(ulint offset, const byte* s, byte* d)
{
  /* Calculate the start offset in a page */
  uint srclen= static_cast<uint>(srv_page_size) -
    (FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION +
     FIL_PAGE_FCRC32_CHECKSUM);
  const byte* src= s + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION;
  byte* dst= d + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION;

  memcpy(d, s, FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);

  if (!log_tmp_block_encrypt(src, srclen, dst, (offset * srv_page_size), true))
    return NULL;

  const ulint payload= srv_page_size - FIL_PAGE_FCRC32_CHECKSUM;
  mach_write_to_4(d + payload, ut_crc32(d, payload));

  srv_stats.pages_encrypted.inc();
  srv_stats.n_temp_blocks_encrypted.inc();
  return d;
}

/** Encryption and page_compression hook that is called just before
a page is written to disk.
@param[in,out]  space   tablespace
@param[in,out]  bpage   buffer page
@param[in]      s       physical page frame that is being encrypted
@param[in,out]  size    payload size in bytes
@return page frame to be written to file
(may be src_frame or an encrypted/compressed copy of it) */
static byte *buf_page_encrypt(fil_space_t* space, buf_page_t* bpage, byte* s,
                              size_t *size)
{
  ut_ad(bpage->status != buf_page_t::FREED);
  ut_ad(space->id == bpage->id().space());

  ut_d(fil_page_type_validate(space, s));
  const uint32_t page_no= bpage->id().page_no();

  switch (page_no) {
  case TRX_SYS_PAGE_NO:
    if (bpage->id().space() != TRX_SYS_SPACE)
      break;
    /* The TRX_SYS page is neither encrypted nor compressed, because
    it contains the address of the doublewrite buffer. */
    /* fall through */
  case 0:
    /* Page 0 of a tablespace is not encrypted/compressed */
    return s;
  }

  fil_space_crypt_t *crypt_data= space->crypt_data;
  bool encrypted, page_compressed;
  if (space->purpose == FIL_TYPE_TEMPORARY)
  {
    ut_ad(!crypt_data);
    encrypted= innodb_encrypt_temporary_tables;
    page_compressed= false;
  }
  else
  {
    encrypted= crypt_data && !crypt_data->not_encrypted() &&
      crypt_data->type != CRYPT_SCHEME_UNENCRYPTED &&
      (!crypt_data->is_default_encryption() || srv_encrypt_tables);
    page_compressed= space->is_compressed();
  }

  const bool full_crc32= space->full_crc32();

  if (!encrypted && !page_compressed)
  {
    /* No need to encrypt or compress. Clear key-version & crypt-checksum. */
    static_assert(FIL_PAGE_FCRC32_KEY_VERSION % 4 == 0, "alignment");
    static_assert(FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION % 4 == 2,
                  "not perfect alignment");
    if (full_crc32)
      memset_aligned<4>(s + FIL_PAGE_FCRC32_KEY_VERSION, 0, 4);
    else
      memset_aligned<2>(s + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, 0, 8);
    return s;
  }

  static_assert(FIL_PAGE_FCRC32_END_LSN % 4 == 0, "alignment");
  static_assert(FIL_PAGE_LSN % 8 == 0, "alignment");
  if (full_crc32)
    memcpy_aligned<4>(s + srv_page_size - FIL_PAGE_FCRC32_END_LSN,
                      FIL_PAGE_LSN + 4 + s, 4);

  ut_ad(!bpage->zip_size() || !page_compressed);
  /* Find free slot from temporary memory array */
  buf_tmp_buffer_t *slot= buf_pool.io_buf_reserve();
  ut_a(slot);
  slot->allocate();
  slot->out_buf= NULL;
  bpage->slot= slot;

  byte *d= slot->crypt_buf;

  if (!page_compressed)
  {
not_compressed:
    byte *tmp= space->purpose == FIL_TYPE_TEMPORARY
      ? buf_tmp_page_encrypt(page_no, s, d)
      : fil_space_encrypt(space, page_no, s, d);

    slot->out_buf= d= tmp;

    ut_d(fil_page_type_validate(space, tmp));
  }
  else
  {
    ut_ad(space->purpose != FIL_TYPE_TEMPORARY);
    /* First we compress the page content */
    buf_tmp_reserve_compression_buf(slot);
    byte *tmp= slot->comp_buf;
    ulint len= fil_page_compress(s, tmp, space->flags,
                                 fil_space_get_block_size(space, page_no),
                                 encrypted);

    if (!len)
      goto not_compressed;

    *size= len;

    if (full_crc32)
    {
      ut_d(bool compressed = false);
      len= buf_page_full_crc32_size(tmp,
#ifdef UNIV_DEBUG
                                    &compressed,
#else
                                    NULL,
#endif
                                    NULL);
      ut_ad(compressed);
    }

    /* Workaround for MDEV-15527. */
    memset(tmp + len, 0 , srv_page_size - len);
    ut_d(fil_page_type_validate(space, tmp));

    if (encrypted)
      tmp = fil_space_encrypt(space, page_no, tmp, d);

    if (full_crc32)
    {
      static_assert(FIL_PAGE_FCRC32_CHECKSUM == 4, "alignment");
      mach_write_to_4(tmp + len - 4, ut_crc32(tmp, len - 4));
      ut_ad(!buf_page_is_corrupted(true, tmp, space->flags));
    }

    slot->out_buf= d= tmp;
  }

  ut_d(fil_page_type_validate(space, d));
  return d;
}

/** The following function deals with freed page during flushing.
     i)  Writing zeros to the file asynchronously if scrubbing is enabled
     ii) Punch the hole to the file synchoronously if page_compressed is
         enabled for the tablespace
This function also resets the IO_FIX to IO_NONE and making the
page status as NORMAL. It initiates the write to the file only after
releasing the page from flush list and its associated mutex.
@param[in,out]	bpage	freed buffer page */
static void buf_release_freed_page(buf_page_t *bpage)
{
  ut_ad(bpage->in_file());
  const bool uncompressed= bpage->state() == BUF_BLOCK_FILE_PAGE;
  mysql_mutex_lock(&buf_pool.mutex);
  bpage->set_io_fix(BUF_IO_NONE);
  bpage->status= buf_page_t::NORMAL;
  const bool temp= fsp_is_system_temporary(bpage->id().space());
  ut_ad(!temp || uncompressed);
  ut_ad(!temp || bpage->oldest_modification() == 1);
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  bpage->clear_oldest_modification();
  if (!temp)
    buf_flush_remove_low(bpage);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  if (uncompressed)
    rw_lock_sx_unlock_gen(&reinterpret_cast<buf_block_t*>(bpage)->lock,
                          BUF_IO_WRITE);

  buf_LRU_free_page(bpage, true);
  mysql_mutex_unlock(&buf_pool.mutex);
}

/** Write a flushable page from buf_pool to a file.
buf_pool.mutex must be held.
@param bpage       buffer control block
@param lru         true=buf_pool.LRU; false=buf_pool.flush_list
@param space       tablespace
@return whether the page was flushed and buf_pool.mutex was released */
static bool buf_flush_page(buf_page_t *bpage, bool lru, fil_space_t *space)
{
  ut_ad(bpage->in_file());
  ut_ad(bpage->ready_for_flush());
  ut_ad((space->purpose == FIL_TYPE_TEMPORARY) ==
        (space == fil_system.temp_space));
  ut_ad(space->purpose == FIL_TYPE_TABLESPACE ||
        space->atomic_write_supported);
  ut_ad(space->referenced());

  rw_lock_t *rw_lock;

  if (bpage->state() != BUF_BLOCK_FILE_PAGE)
    rw_lock= nullptr;
  else
  {
    rw_lock= &reinterpret_cast<buf_block_t*>(bpage)->lock;
    if (!rw_lock_sx_lock_nowait(rw_lock, BUF_IO_WRITE))
      return false;
  }

  bpage->set_io_fix(BUF_IO_WRITE);
  buf_flush_page_count++;
  mysql_mutex_unlock(&buf_pool.mutex);
  mysql_mutex_assert_not_owner(&buf_pool.flush_list_mutex);

  /* We are holding rw_lock = buf_block_t::lock in SX mode except if
  this is a ROW_FORMAT=COMPRESSED page whose uncompressed page frame
  has been evicted from the buffer pool.

  Apart from possible rw_lock protection, bpage is also protected by
  io_fix and oldest_modification()!=0. Thus, it cannot be relocated in
  the buffer pool or removed from flush_list or LRU_list. */

  DBUG_PRINT("ib_buf", ("%s %u page %u:%u",
                        lru ? "LRU" : "flush_list",
                        bpage->id().space(), bpage->id().page_no()));
  ut_ad(bpage->io_fix() == BUF_IO_WRITE);
  ut_ad(bpage->oldest_modification());
  ut_ad(bpage->state() ==
        (rw_lock ? BUF_BLOCK_FILE_PAGE : BUF_BLOCK_ZIP_PAGE));
  ut_ad(ULINT_UNDEFINED >
        (lru ? buf_pool.n_flush_LRU : buf_pool.n_flush_list));

  /* Because bpage->status can only be changed while buf_block_t
  exists, it cannot be modified for ROW_FORMAT=COMPRESSED pages
  without first allocating the uncompressed page frame. Such
  allocation cannot be completed due to our io_fix. So, bpage->status
  is protected even if !rw_lock. */
  const auto status= bpage->status;

  buf_block_t *block= reinterpret_cast<buf_block_t*>(bpage);
  page_t *frame= bpage->zip.data;

  if (UNIV_LIKELY(space->purpose == FIL_TYPE_TABLESPACE))
  {
    const lsn_t lsn= mach_read_from_8(my_assume_aligned<8>
                                      (FIL_PAGE_LSN +
                                       (frame ? frame : block->frame)));
    ut_ad(lsn);
    ut_ad(lsn >= bpage->oldest_modification());
    ut_ad(!srv_read_only_mode);
    if (UNIV_UNLIKELY(lsn > log_sys.get_flushed_lsn()))
    {
      if (rw_lock)
        rw_lock_sx_unlock_gen(rw_lock, BUF_IO_WRITE);
      mysql_mutex_lock(&buf_pool.mutex);
      bpage->set_io_fix(BUF_IO_NONE);
      return false;
    }
  }

  if (status == buf_page_t::FREED)
    buf_release_freed_page(&block->page);
  else
  {
    space->reacquire();
    ut_ad(status == buf_page_t::NORMAL || status == buf_page_t::INIT_ON_FLUSH);
    size_t size;
#if defined HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE || defined _WIN32
    size_t orig_size;
#endif
    IORequest::Type type= lru ? IORequest::WRITE_LRU : IORequest::WRITE_ASYNC;

    if (UNIV_UNLIKELY(!rw_lock)) /* ROW_FORMAT=COMPRESSED */
    {
      ut_ad(!space->full_crc32());
      ut_ad(!space->is_compressed()); /* not page_compressed */
      size= bpage->zip_size();
#if defined HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE || defined _WIN32
      orig_size= size;
#endif
      buf_flush_update_zip_checksum(frame, size);
      frame= buf_page_encrypt(space, bpage, frame, &size);
      ut_ad(size == bpage->zip_size());
    }
    else
    {
      byte *page= block->frame;
      size= block->physical_size();
#if defined HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE || defined _WIN32
      orig_size= size;
#endif

      if (space->full_crc32())
      {
        /* innodb_checksum_algorithm=full_crc32 is not implemented for
        ROW_FORMAT=COMPRESSED pages. */
        ut_ad(!frame);
	page= buf_page_encrypt(space, bpage, page, &size);
	buf_flush_init_for_writing(block, page, nullptr, true);
      }
      else
      {
        buf_flush_init_for_writing(block, page, frame ? &bpage->zip : nullptr,
                                   false);
        page= buf_page_encrypt(space, bpage, frame ? frame : page, &size);
      }

#if defined HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE || defined _WIN32
      if (size != orig_size && space->punch_hole)
        type= lru ? IORequest::PUNCH_LRU : IORequest::PUNCH;
#endif
      frame=page;
    }

    ut_ad(status == bpage->status);

    if (lru)
      buf_pool.n_flush_LRU++;
    else
      buf_pool.n_flush_list++;
    if (status != buf_page_t::NORMAL || !space->use_doublewrite())
      space->io(IORequest(type, bpage),
                bpage->physical_offset(), size, frame, bpage);
    else
      buf_dblwr.add_to_batch(IORequest(bpage, space->chain.start, type), size);
  }

  /* Increment the I/O operation count used for selecting LRU policy. */
  buf_LRU_stat_inc_io();
  return true;
}

/** Check whether a page can be flushed from the buf_pool.
@param id          page identifier
@param fold        id.fold()
@param lru         true=buf_pool.LRU; false=buf_pool.flush_list
@return whether the page can be flushed */
static bool buf_flush_check_neighbor(const page_id_t id, ulint fold, bool lru)
{
  mysql_mutex_assert_owner(&buf_pool.mutex);
  ut_ad(fold == id.fold());

  buf_page_t *bpage= buf_pool.page_hash_get_low(id, fold);

  if (!bpage || buf_pool.watch_is_sentinel(*bpage))
    return false;

  /* We avoid flushing 'non-old' blocks in an LRU flush, because the
  flushed blocks are soon freed */

  return (!lru || bpage->is_old()) && bpage->ready_for_flush();
}

/** Check which neighbors of a page can be flushed from the buf_pool.
@param space       tablespace
@param id          page identifier of a dirty page
@param contiguous  whether to consider contiguous areas of pages
@param lru         true=buf_pool.LRU; false=buf_pool.flush_list
@return last page number that can be flushed */
static page_id_t buf_flush_check_neighbors(const fil_space_t &space,
                                           page_id_t &id, bool contiguous,
                                           bool lru)
{
  ut_ad(id.page_no() < space.size);
  /* When flushed, dirty blocks are searched in neighborhoods of this
  size, and flushed along with the original page. */
  const ulint s= buf_pool.curr_size / 16;
  const uint32_t read_ahead= buf_pool.read_ahead_area;
  const uint32_t buf_flush_area= read_ahead > s
    ? static_cast<uint32_t>(s) : read_ahead;
  page_id_t low= id - (id.page_no() % buf_flush_area);
  page_id_t high= low + buf_flush_area;
  high.set_page_no(std::min(high.page_no(), space.last_page_number()));

  if (!contiguous)
  {
    high= std::max(id + 1, high);
    id= low;
    return high;
  }

  /* Determine the contiguous dirty area around id. */
  const ulint id_fold= id.fold();

  mysql_mutex_lock(&buf_pool.mutex);

  if (id > low)
  {
    ulint fold= id_fold;
    for (page_id_t i= id - 1;; --i)
    {
      fold--;
      if (!buf_flush_check_neighbor(i, fold, lru))
      {
        low= i + 1;
        break;
      }
      if (i == low)
        break;
    }
  }

  page_id_t i= id;
  id= low;
  ulint fold= id_fold;
  while (++i < high)
  {
    ++fold;
    if (!buf_flush_check_neighbor(i, fold, lru))
      break;
  }

  mysql_mutex_unlock(&buf_pool.mutex);
  return i;
}

MY_ATTRIBUTE((nonnull))
/** Write punch-hole or zeroes of the freed ranges when
innodb_immediate_scrub_data_uncompressed from the freed ranges.
@param space   tablespace which may contain ranges of freed pages */
static void buf_flush_freed_pages(fil_space_t *space)
{
  const bool punch_hole= space->punch_hole;
  if (!srv_immediate_scrub_data_uncompressed && !punch_hole)
    return;
  lsn_t flush_to_disk_lsn= log_sys.get_flushed_lsn();

  std::unique_lock<std::mutex> freed_lock(space->freed_range_mutex);
  if (space->freed_ranges.empty()
      || flush_to_disk_lsn < space->get_last_freed_lsn())
  {
    freed_lock.unlock();
    return;
  }

  range_set freed_ranges= std::move(space->freed_ranges);
  freed_lock.unlock();

  for (const auto &range : freed_ranges)
  {
    const ulint physical_size= space->physical_size();

    if (punch_hole)
    {
      space->reacquire();
      space->io(IORequest(IORequest::PUNCH_RANGE),
                          os_offset_t{range.first} * physical_size,
                          (range.last - range.first + 1) * physical_size,
                          nullptr);
    }
    else if (srv_immediate_scrub_data_uncompressed)
    {
      for (os_offset_t i= range.first; i <= range.last; i++)
      {
        space->reacquire();
        space->io(IORequest(IORequest::WRITE_ASYNC),
                  i * physical_size, physical_size,
                  const_cast<byte*>(field_ref_zero));
      }
    }
    buf_pool.stat.n_pages_written+= (range.last - range.first + 1);
  }
}

/** Flushes to disk all flushable pages within the flush area
and also write zeroes or punch the hole for the freed ranges of pages.
@param space       tablespace
@param page_id     page identifier
@param contiguous  whether to consider contiguous areas of pages
@param lru         true=buf_pool.LRU; false=buf_pool.flush_list
@param n_flushed   number of pages flushed so far in this batch
@param n_to_flush  maximum number of pages we are allowed to flush
@return number of pages flushed */
static ulint buf_flush_try_neighbors(fil_space_t *space,
                                     const page_id_t page_id,
                                     bool contiguous, bool lru,
                                     ulint n_flushed, ulint n_to_flush)
{
  ut_ad(space->id == page_id.space());

  ulint count= 0;
  page_id_t id= page_id;
  page_id_t high= buf_flush_check_neighbors(*space, id, contiguous, lru);

  ut_ad(page_id >= id);
  ut_ad(page_id < high);

  for (ulint id_fold= id.fold(); id < high && !space->is_stopping();
       ++id, ++id_fold)
  {
    if (count + n_flushed >= n_to_flush)
    {
      if (id > page_id)
        break;
      /* If the page whose neighbors we are flushing has not been
      flushed yet, we must flush the page that we selected originally. */
      id= page_id;
      id_fold= id.fold();
    }

    mysql_mutex_lock(&buf_pool.mutex);

    if (buf_page_t *bpage= buf_pool.page_hash_get_low(id, id_fold))
    {
      ut_ad(bpage->in_file());
      /* We avoid flushing 'non-old' blocks in an LRU flush,
      because the flushed blocks are soon freed */
      if (!lru || id == page_id || bpage->is_old())
      {
        if (!buf_pool.watch_is_sentinel(*bpage) &&
            bpage->ready_for_flush() && buf_flush_page(bpage, lru, space))
        {
          ++count;
          continue;
        }
      }
    }

    mysql_mutex_unlock(&buf_pool.mutex);
  }

  if (auto n= count - 1)
  {
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_NEIGHBOR_TOTAL_PAGE,
                                 MONITOR_FLUSH_NEIGHBOR_COUNT,
                                 MONITOR_FLUSH_NEIGHBOR_PAGES, n);
  }

  return count;
}

/*******************************************************************//**
This utility moves the uncompressed frames of pages to the free list.
Note that this function does not actually flush any data to disk. It
just detaches the uncompressed frames from the compressed pages at the
tail of the unzip_LRU and puts those freed frames in the free list.
Note that it is a best effort attempt and it is not guaranteed that
after a call to this function there will be 'max' blocks in the free
list.
@param[in]	max		desired number of blocks in the free_list
@return number of blocks moved to the free list. */
static ulint buf_free_from_unzip_LRU_list_batch(ulint max)
{
	ulint		scanned = 0;
	ulint		count = 0;

	mysql_mutex_assert_owner(&buf_pool.mutex);

	buf_block_t*	block = UT_LIST_GET_LAST(buf_pool.unzip_LRU);

	while (block
	       && count < max
	       && UT_LIST_GET_LEN(buf_pool.free) < srv_LRU_scan_depth
	       && UT_LIST_GET_LEN(buf_pool.unzip_LRU)
	       > UT_LIST_GET_LEN(buf_pool.LRU) / 10) {

		++scanned;
		if (buf_LRU_free_page(&block->page, false)) {
			/* Block was freed. buf_pool.mutex potentially
			released and reacquired */
			++count;
			block = UT_LIST_GET_LAST(buf_pool.unzip_LRU);
		} else {
			block = UT_LIST_GET_PREV(unzip_LRU, block);
		}
	}

	mysql_mutex_assert_owner(&buf_pool.mutex);

	if (scanned) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_LRU_BATCH_SCANNED,
			MONITOR_LRU_BATCH_SCANNED_NUM_CALL,
			MONITOR_LRU_BATCH_SCANNED_PER_CALL,
			scanned);
	}

	return(count);
}

/** Start writing out pages for a tablespace.
@param id   tablespace identifier
@return tablespace
@retval nullptr if the pages for this tablespace should be discarded */
static fil_space_t *buf_flush_space(const uint32_t id)
{
  fil_space_t *space= fil_space_t::get(id);
  if (space)
    buf_flush_freed_pages(space);
  return space;
}

struct flush_counters_t
{
  /** number of dirty pages flushed */
  ulint flushed;
  /** number of clean pages evicted */
  ulint evicted;
};

/** Try to discard a dirty page.
@param bpage      dirty page whose tablespace is not accessible */
static void buf_flush_discard_page(buf_page_t *bpage)
{
  mysql_mutex_assert_owner(&buf_pool.mutex);
  mysql_mutex_assert_not_owner(&buf_pool.flush_list_mutex);
  ut_ad(bpage->in_file());
  ut_ad(bpage->oldest_modification());

  rw_lock_t *rw_lock;

  if (bpage->state() != BUF_BLOCK_FILE_PAGE)
    rw_lock= nullptr;
  else
  {
    rw_lock= &reinterpret_cast<buf_block_t*>(bpage)->lock;
    if (!rw_lock_sx_lock_nowait(rw_lock, 0))
      return;
  }

  bpage->status= buf_page_t::NORMAL;
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  buf_flush_remove(bpage);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  if (rw_lock)
    rw_lock_sx_unlock(rw_lock);

  buf_LRU_free_page(bpage, true);
}

/** Flush dirty blocks from the end of the LRU list.
@param max   maximum number of blocks to make available in buf_pool.free
@param n     counts of flushed and evicted pages */
static void buf_flush_LRU_list_batch(ulint max, flush_counters_t *n)
{
  ulint scanned= 0;
  ulint free_limit= srv_LRU_scan_depth;

  mysql_mutex_assert_owner(&buf_pool.mutex);
  if (buf_pool.withdraw_target && buf_pool.curr_size < buf_pool.old_size)
    free_limit+= buf_pool.withdraw_target - UT_LIST_GET_LEN(buf_pool.withdraw);

  const auto neighbors= UT_LIST_GET_LEN(buf_pool.LRU) < BUF_LRU_OLD_MIN_LEN
    ? 0 : srv_flush_neighbors;
  fil_space_t *space= nullptr;
  uint32_t last_space_id= FIL_NULL;
  static_assert(FIL_NULL > SRV_TMP_SPACE_ID, "consistency");
  static_assert(FIL_NULL > SRV_SPACE_ID_UPPER_BOUND, "consistency");

  for (buf_page_t *bpage= UT_LIST_GET_LAST(buf_pool.LRU);
       bpage && n->flushed + n->evicted < max &&
       UT_LIST_GET_LEN(buf_pool.LRU) > BUF_LRU_MIN_LEN &&
       UT_LIST_GET_LEN(buf_pool.free) < free_limit;
       ++scanned, bpage= buf_pool.lru_hp.get())
  {
    buf_page_t *prev= UT_LIST_GET_PREV(LRU, bpage);
    buf_pool.lru_hp.set(prev);

    if (bpage->ready_for_replace())
    {
      /* block is ready for eviction i.e., it is clean and is not
      IO-fixed or buffer fixed. */
      if (buf_LRU_free_page(bpage, true))
        ++n->evicted;
    }
    else if (bpage->ready_for_flush())
    {
      /* Block is ready for flush. Dispatch an IO request. The IO
      helper thread will put it on free list in IO completion routine. */
      const page_id_t page_id(bpage->id());
      const uint32_t space_id= page_id.space();
      if (!space || space->id != space_id)
      {
        if (last_space_id != space_id)
        {
          if (space)
            space->release();
          space= buf_flush_space(space_id);
          last_space_id= space_id;
        }
        else
          ut_ad(!space);
      }
      else if (space->is_stopping())
      {
        space->release();
        space= nullptr;
      }

      if (!space)
        buf_flush_discard_page(bpage);
      else if (neighbors && space->is_rotational())
      {
        mysql_mutex_unlock(&buf_pool.mutex);
        n->flushed+= buf_flush_try_neighbors(space, page_id, neighbors == 1,
                                             true, n->flushed, max);
reacquire_mutex:
        mysql_mutex_lock(&buf_pool.mutex);
      }
      else if (buf_flush_page(bpage, true, space))
      {
        ++n->flushed;
        goto reacquire_mutex;
      }
    }
    else
      /* Can't evict or dispatch this block. Go to previous. */
      ut_ad(buf_pool.lru_hp.is_hp(prev));
  }

  buf_pool.lru_hp.set(nullptr);

  if (space)
    space->release();

  /* We keep track of all flushes happening as part of LRU flush. When
  estimating the desired rate at which flush_list should be flushed,
  we factor in this value. */
  buf_lru_flush_page_count+= n->flushed;

  mysql_mutex_assert_owner(&buf_pool.mutex);

  if (scanned)
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_LRU_BATCH_SCANNED,
                                 MONITOR_LRU_BATCH_SCANNED_NUM_CALL,
                                 MONITOR_LRU_BATCH_SCANNED_PER_CALL,
                                 scanned);
}

/** Flush and move pages from LRU or unzip_LRU list to the free list.
Whether LRU or unzip_LRU is used depends on the state of the system.
@param max   maximum number of blocks to make available in buf_pool.free
@return number of flushed pages */
static ulint buf_do_LRU_batch(ulint max)
{
  const ulint n_unzip_LRU_evicted= buf_LRU_evict_from_unzip_LRU()
    ? buf_free_from_unzip_LRU_list_batch(max)
    : 0;
  flush_counters_t n;
  n.flushed= 0;
  n.evicted= n_unzip_LRU_evicted;
  buf_flush_LRU_list_batch(max, &n);

  if (const ulint evicted= n.evicted - n_unzip_LRU_evicted)
  {
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_LRU_BATCH_EVICT_TOTAL_PAGE,
                                 MONITOR_LRU_BATCH_EVICT_COUNT,
                                 MONITOR_LRU_BATCH_EVICT_PAGES,
                                 evicted);
  }

  return n.flushed;
}

/** This utility flushes dirty blocks from the end of the flush_list.
The calling thread is not allowed to own any latches on pages!
@param max_n    maximum mumber of blocks to flush
@param lsn      once an oldest_modification>=lsn is found, terminate the batch
@return number of blocks for which the write request was queued */
static ulint buf_do_flush_list_batch(ulint max_n, lsn_t lsn)
{
  ulint count= 0;
  ulint scanned= 0;

  mysql_mutex_assert_owner(&buf_pool.mutex);

  const auto neighbors= UT_LIST_GET_LEN(buf_pool.LRU) < BUF_LRU_OLD_MIN_LEN
    ? 0 : srv_flush_neighbors;
  fil_space_t *space= nullptr;
  uint32_t last_space_id= FIL_NULL;
  static_assert(FIL_NULL > SRV_TMP_SPACE_ID, "consistency");
  static_assert(FIL_NULL > SRV_SPACE_ID_UPPER_BOUND, "consistency");

  /* Start from the end of the list looking for a suitable block to be
  flushed. */
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  ulint len= UT_LIST_GET_LEN(buf_pool.flush_list);

  /* In order not to degenerate this scan to O(n*n) we attempt to
  preserve pointer of previous block in the flush list. To do so we
  declare it a hazard pointer. Any thread working on the flush list
  must check the hazard pointer and if it is removing the same block
  then it must reset it. */
  for (buf_page_t *bpage= UT_LIST_GET_LAST(buf_pool.flush_list);
       bpage && len && count < max_n;
       bpage= buf_pool.flush_hp.get(), ++scanned, len--)
  {
    const lsn_t oldest_modification= bpage->oldest_modification();
    if (oldest_modification >= lsn)
      break;
    ut_ad(oldest_modification);

    buf_page_t *prev= UT_LIST_GET_PREV(list, bpage);
    buf_pool.flush_hp.set(prev);
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    ut_ad(bpage->in_file());
    const bool flushed= bpage->ready_for_flush();

    if (flushed)
    {
      const page_id_t page_id(bpage->id());
      const uint32_t space_id= page_id.space();
      if (!space || space->id != space_id)
      {
        if (last_space_id != space_id)
        {
          if (space)
            space->release();
          space= buf_flush_space(space_id);
          last_space_id= space_id;
        }
        else
          ut_ad(!space);
      }
      else if (space->is_stopping())
      {
        space->release();
        space= nullptr;
      }

      if (!space)
        buf_flush_discard_page(bpage);
      else if (neighbors && space->is_rotational())
      {
        mysql_mutex_unlock(&buf_pool.mutex);
        count+= buf_flush_try_neighbors(space, page_id, neighbors == 1,
                                        false, count, max_n);
reacquire_mutex:
        mysql_mutex_lock(&buf_pool.mutex);
      }
      else if (buf_flush_page(bpage, false, space))
      {
        ++count;
        goto reacquire_mutex;
      }
    }

    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    ut_ad(flushed || buf_pool.flush_hp.is_hp(prev));
  }

  buf_pool.flush_hp.set(nullptr);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  if (space)
    space->release();

  if (scanned)
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_BATCH_SCANNED,
                                 MONITOR_FLUSH_BATCH_SCANNED_NUM_CALL,
                                 MONITOR_FLUSH_BATCH_SCANNED_PER_CALL,
                                 scanned);
  if (count)
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_BATCH_TOTAL_PAGE,
                                 MONITOR_FLUSH_BATCH_COUNT,
                                 MONITOR_FLUSH_BATCH_PAGES,
                                 count);
  mysql_mutex_assert_owner(&buf_pool.mutex);
  return count;
}

/** Wait until a flush batch ends.
@param lru    true=buf_pool.LRU; false=buf_pool.flush_list */
void buf_flush_wait_batch_end(bool lru)
{
  const auto &n_flush= lru ? buf_pool.n_flush_LRU : buf_pool.n_flush_list;

  if (n_flush)
  {
    auto cond= lru ? &buf_pool.done_flush_LRU : &buf_pool.done_flush_list;
    tpool::tpool_wait_begin();
    thd_wait_begin(nullptr, THD_WAIT_DISKIO);
    do
      my_cond_wait(cond, &buf_pool.mutex.m_mutex);
    while (n_flush);
    tpool::tpool_wait_end();
    thd_wait_end(nullptr);
    pthread_cond_broadcast(cond);
  }
}

/** Whether a background log flush is pending */
static std::atomic_flag log_flush_pending;

/** Advance log_sys.get_flushed_lsn() */
static void log_flush(void *)
{
  /* Between batches, we try to prevent I/O stalls by these calls.
  This should not be needed for correctness. */
  os_aio_wait_until_no_pending_writes();
  fil_flush_file_spaces();

  /* Guarantee progress for buf_flush_lists(). */
  log_write_up_to(log_sys.get_lsn(), true);
  log_flush_pending.clear();
}

static tpool::waitable_task log_flush_task(log_flush, nullptr, nullptr);

/** Write out dirty blocks from buf_pool.flush_list.
@param max_n    wished maximum mumber of blocks flushed
@param lsn      buf_pool.get_oldest_modification(LSN_MAX) target (0=LRU flush)
@return the number of processed pages
@retval 0 if a batch of the same type (lsn==0 or lsn!=0) is already running */
ulint buf_flush_lists(ulint max_n, lsn_t lsn)
{
  auto &n_flush= lsn ? buf_pool.n_flush_list : buf_pool.n_flush_LRU;

  if (n_flush)
    return 0;

  if (log_sys.get_lsn() > log_sys.get_flushed_lsn())
  {
    log_flush_task.wait();
    if (log_sys.get_lsn() > log_sys.get_flushed_lsn() &&
        !log_flush_pending.test_and_set())
      srv_thread_pool->submit_task(&log_flush_task);
#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
    if (UNIV_UNLIKELY(ibuf_debug))
      log_write_up_to(log_sys.get_lsn(), true);
#endif
  }

  auto cond= lsn ? &buf_pool.done_flush_list : &buf_pool.done_flush_LRU;

  mysql_mutex_lock(&buf_pool.mutex);
  const bool running= n_flush != 0;
  /* FIXME: we are performing a dirty read of buf_pool.flush_list.count
  while not holding buf_pool.flush_list_mutex */
  if (running || (lsn && !UT_LIST_GET_LEN(buf_pool.flush_list)))
  {
    if (!running)
      pthread_cond_broadcast(cond);
    mysql_mutex_unlock(&buf_pool.mutex);
    return 0;
  }
  n_flush++;

  ulint n_flushed= lsn
    ? buf_do_flush_list_batch(max_n, lsn)
    : buf_do_LRU_batch(max_n);

  const auto n_flushing= --n_flush;

  buf_pool.try_LRU_scan= true;

  mysql_mutex_unlock(&buf_pool.mutex);

  if (!n_flushing)
    pthread_cond_broadcast(cond);

  buf_dblwr.flush_buffered_writes();

  DBUG_PRINT("ib_buf", ("%s completed, " ULINTPF " pages",
                        lsn ? "flush_list" : "LRU flush", n_flushed));
  return n_flushed;
}


/** Initiate a log checkpoint, discarding the start of the log.
@param oldest_lsn   the checkpoint LSN
@param end_lsn      log_sys.get_lsn()
@return true if success, false if a checkpoint write was already running */
static bool log_checkpoint_low(lsn_t oldest_lsn, lsn_t end_lsn)
{
  ut_ad(!srv_read_only_mode);
  mysql_mutex_assert_owner(&log_sys.mutex);
  ut_ad(oldest_lsn <= end_lsn);
  ut_ad(end_lsn == log_sys.get_lsn());
  ut_ad(!recv_no_log_write);

  ut_ad(oldest_lsn >= log_sys.last_checkpoint_lsn);

  if (oldest_lsn > log_sys.last_checkpoint_lsn + SIZE_OF_FILE_CHECKPOINT)
    /* Some log has been written since the previous checkpoint. */;
  else if (srv_shutdown_state > SRV_SHUTDOWN_INITIATED)
    /* MariaDB startup expects the redo log file to be logically empty
    (not even containing a FILE_CHECKPOINT record) after a clean shutdown.
    Perform an extra checkpoint at shutdown. */;
  else
  {
    /* Do nothing, because nothing was logged (other than a
    FILE_CHECKPOINT record) since the previous checkpoint. */
    mysql_mutex_unlock(&log_sys.mutex);
    return true;
  }

  /* Repeat the FILE_MODIFY records after the checkpoint, in case some
  log records between the checkpoint and log_sys.lsn need them.
  Finally, write a FILE_CHECKPOINT record. Redo log apply expects to
  see a FILE_CHECKPOINT after the checkpoint, except on clean
  shutdown, where the log will be empty after the checkpoint.

  It is important that we write out the redo log before any further
  dirty pages are flushed to the tablespace files.  At this point,
  because we hold log_sys.mutex, mtr_t::commit() in other threads will
  be blocked, and no pages can be added to the flush lists. */
  lsn_t flush_lsn= oldest_lsn;

  if (fil_names_clear(flush_lsn, oldest_lsn != end_lsn ||
                      srv_shutdown_state <= SRV_SHUTDOWN_INITIATED))
  {
    flush_lsn= log_sys.get_lsn();
    ut_ad(flush_lsn >= end_lsn + SIZE_OF_FILE_CHECKPOINT);
    mysql_mutex_unlock(&log_sys.mutex);
    log_write_up_to(flush_lsn, true, true);
    mysql_mutex_lock(&log_sys.mutex);
    if (log_sys.last_checkpoint_lsn >= oldest_lsn)
    {
      mysql_mutex_unlock(&log_sys.mutex);
      return true;
    }
  }
  else
    ut_ad(oldest_lsn >= log_sys.last_checkpoint_lsn);

  ut_ad(log_sys.get_flushed_lsn() >= flush_lsn);

  if (log_sys.n_pending_checkpoint_writes)
  {
    /* A checkpoint write is running */
    mysql_mutex_unlock(&log_sys.mutex);
    return false;
  }

  log_sys.next_checkpoint_lsn= oldest_lsn;
  log_write_checkpoint_info(end_lsn);
  mysql_mutex_assert_not_owner(&log_sys.mutex);

  return true;
}

/** Make a checkpoint. Note that this function does not flush dirty
blocks from the buffer pool: it only checks what is lsn of the oldest
modification in the pool, and writes information about the lsn in
log file. Use log_make_checkpoint() to flush also the pool.
@retval true if the checkpoint was or had been made
@retval false if a checkpoint write was already running */
static bool log_checkpoint()
{
  if (recv_recovery_is_on())
    recv_sys.apply(true);

  switch (srv_file_flush_method) {
  case SRV_NOSYNC:
  case SRV_O_DIRECT_NO_FSYNC:
    break;
  default:
    fil_flush_file_spaces();
  }

  mysql_mutex_lock(&log_sys.mutex);
  const lsn_t end_lsn= log_sys.get_lsn();
  mysql_mutex_lock(&log_sys.flush_order_mutex);
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  const lsn_t oldest_lsn= buf_pool.get_oldest_modification(end_lsn);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  mysql_mutex_unlock(&log_sys.flush_order_mutex);
  return log_checkpoint_low(oldest_lsn, end_lsn);
}

/** Make a checkpoint. */
ATTRIBUTE_COLD void log_make_checkpoint()
{
  buf_flush_wait_flushed(log_sys.get_lsn());
  while (!log_checkpoint());
}

/** Wait until all persistent pages are flushed up to a limit.
@param sync_lsn   buf_pool.get_oldest_modification(LSN_MAX) to wait for */
ATTRIBUTE_COLD void buf_flush_wait_flushed(lsn_t sync_lsn)
{
  ut_ad(sync_lsn);
  ut_ad(sync_lsn < LSN_MAX);
  mysql_mutex_assert_not_owner(&log_sys.mutex);
  ut_ad(!srv_read_only_mode);

  if (recv_recovery_is_on())
    recv_sys.apply(true);

  mysql_mutex_lock(&buf_pool.flush_list_mutex);

  if (buf_pool.get_oldest_modification(sync_lsn) < sync_lsn)
  {
#if 1 /* FIXME: remove this, and guarantee that the page cleaner serves us */
    if (UNIV_UNLIKELY(!buf_page_cleaner_is_active)
        ut_d(|| innodb_page_cleaner_disabled_debug))
    {
      do
      {
        mysql_mutex_unlock(&buf_pool.flush_list_mutex);
        ulint n_pages= buf_flush_lists(srv_max_io_capacity, sync_lsn);
        buf_flush_wait_batch_end_acquiring_mutex(false);
        if (n_pages)
        {
          MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_SYNC_TOTAL_PAGE,
                                       MONITOR_FLUSH_SYNC_COUNT,
                                       MONITOR_FLUSH_SYNC_PAGES, n_pages);
        }
        MONITOR_INC(MONITOR_FLUSH_SYNC_WAITS);
        mysql_mutex_lock(&buf_pool.flush_list_mutex);
      }
      while (buf_pool.get_oldest_modification(sync_lsn) < sync_lsn);

      goto try_checkpoint;
    }
#endif
    if (buf_flush_sync_lsn < sync_lsn)
    {
      buf_flush_sync_lsn= sync_lsn;
      pthread_cond_signal(&buf_pool.do_flush_list);
    }

    do
    {
      tpool::tpool_wait_begin();
      thd_wait_begin(nullptr, THD_WAIT_DISKIO);
      my_cond_wait(&buf_pool.done_flush_list,
                   &buf_pool.flush_list_mutex.m_mutex);
      thd_wait_end(nullptr);
      tpool::tpool_wait_end();

      MONITOR_INC(MONITOR_FLUSH_SYNC_WAITS);
    }
    while (buf_pool.get_oldest_modification(sync_lsn) < sync_lsn);
  }

try_checkpoint:
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  if (UNIV_UNLIKELY(log_sys.last_checkpoint_lsn < sync_lsn))
    log_checkpoint();
}

/** If innodb_flush_sync=ON, initiate a furious flush.
@param lsn buf_pool.get_oldest_modification(LSN_MAX) target */
void buf_flush_ahead(lsn_t lsn)
{
  mysql_mutex_assert_not_owner(&log_sys.mutex);
  ut_ad(!srv_read_only_mode);

  if (recv_recovery_is_on())
    recv_sys.apply(true);

  if (buf_flush_sync_lsn < lsn)
  {
    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    if (buf_flush_sync_lsn < lsn)
    {
      buf_flush_sync_lsn= lsn;
      pthread_cond_signal(&buf_pool.do_flush_list);
    }
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  }
}

/** Wait for pending flushes to complete. */
void buf_flush_wait_batch_end_acquiring_mutex(bool lru)
{
  if (lru ? buf_pool.n_flush_LRU : buf_pool.n_flush_list)
  {
    mysql_mutex_lock(&buf_pool.mutex);
    buf_flush_wait_batch_end(lru);
    mysql_mutex_unlock(&buf_pool.mutex);
  }
}

/** Conduct checkpoint-related flushing for innodb_flush_sync=ON,
and try to initiate checkpoints until the target is met.
@param lsn   minimum value of buf_pool.get_oldest_modification(LSN_MAX) */
ATTRIBUTE_COLD static void buf_flush_sync_for_checkpoint(lsn_t lsn)
{
  ut_ad(!srv_read_only_mode);

  for (;;)
  {
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    if (ulint n_flushed= buf_flush_lists(srv_max_io_capacity, lsn))
    {
      MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_SYNC_TOTAL_PAGE,
                                   MONITOR_FLUSH_SYNC_COUNT,
                                   MONITOR_FLUSH_SYNC_PAGES, n_flushed);
    }

    /* Attempt to perform a log checkpoint upon completing each batch. */
    if (recv_recovery_is_on())
      recv_sys.apply(true);

    switch (srv_file_flush_method) {
    case SRV_NOSYNC:
    case SRV_O_DIRECT_NO_FSYNC:
      break;
    default:
      fil_flush_file_spaces();
    }

    mysql_mutex_lock(&log_sys.mutex);
    const lsn_t newest_lsn= log_sys.get_lsn();
    mysql_mutex_lock(&log_sys.flush_order_mutex);
    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    lsn_t measure= buf_pool.get_oldest_modification(0);
    mysql_mutex_unlock(&log_sys.flush_order_mutex);
    const lsn_t checkpoint_lsn= measure ? measure : newest_lsn;

    if (checkpoint_lsn > log_sys.last_checkpoint_lsn + SIZE_OF_FILE_CHECKPOINT)
    {
      mysql_mutex_unlock(&buf_pool.flush_list_mutex);
      log_checkpoint_low(checkpoint_lsn, newest_lsn);
      mysql_mutex_lock(&buf_pool.flush_list_mutex);
      measure= buf_pool.get_oldest_modification(LSN_MAX);
    }
    else
    {
      mysql_mutex_unlock(&log_sys.mutex);
      if (!measure)
        measure= LSN_MAX;
    }

    mysql_mutex_assert_not_owner(&log_sys.mutex);

    /* After attempting log checkpoint, check if we have reached our target. */
    const lsn_t target= buf_flush_sync_lsn;

    if (measure >= target)
      buf_flush_sync_lsn= 0;

    /* wake up buf_flush_wait_flushed() */
    pthread_cond_broadcast(&buf_pool.done_flush_list);

    lsn= std::max(lsn, target);

    if (measure >= lsn)
      return;
  }
}

/*********************************************************************//**
Calculates if flushing is required based on redo generation rate.
@return percent of io_capacity to flush to manage redo space */
static
ulint
af_get_pct_for_lsn(
/*===============*/
	lsn_t	age)	/*!< in: current age of LSN. */
{
	lsn_t	af_lwm = static_cast<lsn_t>(
		srv_adaptive_flushing_lwm
		* static_cast<double>(log_sys.log_capacity) / 100);

	if (age < af_lwm) {
		/* No adaptive flushing. */
		return(0);
	}

	lsn_t lsn_age_factor = (age * 100) / log_sys.max_modified_age_async;

	ut_ad(srv_max_io_capacity >= srv_io_capacity);
	return static_cast<ulint>(
		(static_cast<double>(srv_max_io_capacity / srv_io_capacity
				     * lsn_age_factor)
		 * sqrt(static_cast<double>(lsn_age_factor))
		 / 7.5));
}

/** This function is called approximately once every second by the
page_cleaner thread if innodb_adaptive_flushing=ON.
Based on various factors it decides if there is a need to do flushing.
@return number of pages recommended to be flushed
@param last_pages_in  number of pages flushed in previous batch
@param oldest_lsn     buf_pool.get_oldest_modification(0)
@param dirty_blocks   UT_LIST_GET_LEN(buf_pool.flush_list)
@param dirty_pct      100*flush_list.count / (LRU.count + free.count) */
static ulint page_cleaner_flush_pages_recommendation(ulint last_pages_in,
                                                     lsn_t oldest_lsn,
                                                     ulint dirty_blocks,
                                                     double dirty_pct)
{
	static	lsn_t		prev_lsn = 0;
	static	ulint		sum_pages = 0;
	static	ulint		avg_page_rate = 0;
	static	ulint		n_iterations = 0;
	static	time_t		prev_time;
	lsn_t			lsn_rate;
	ulint			n_pages = 0;

	const lsn_t cur_lsn = log_sys.get_lsn();
	ut_ad(oldest_lsn <= cur_lsn);
	ulint pct_for_lsn = af_get_pct_for_lsn(cur_lsn - oldest_lsn);
	time_t curr_time = time(nullptr);
	const double max_pct = srv_max_buf_pool_modified_pct;

	if (!prev_lsn || !pct_for_lsn) {
		prev_time = curr_time;
		prev_lsn = cur_lsn;
		if (max_pct > 0.0) {
			dirty_pct /= max_pct;
		}

		n_pages = ulint(dirty_pct * double(srv_io_capacity));
		if (n_pages < dirty_blocks) {
			n_pages= std::min<ulint>(srv_io_capacity, dirty_blocks);
		}

		return n_pages;
	}

	sum_pages += last_pages_in;

	double	time_elapsed = difftime(curr_time, prev_time);

	/* We update our variables every srv_flushing_avg_loops
	iterations to smooth out transition in workload. */
	if (++n_iterations >= srv_flushing_avg_loops
	    || time_elapsed >= static_cast<double>(srv_flushing_avg_loops)) {

		if (time_elapsed < 1) {
			time_elapsed = 1;
		}

		avg_page_rate = static_cast<ulint>(
			((static_cast<double>(sum_pages)
			  / time_elapsed)
			 + static_cast<double>(avg_page_rate)) / 2);

		/* How much LSN we have generated since last call. */
		lsn_rate = static_cast<lsn_t>(
			static_cast<double>(cur_lsn - prev_lsn)
			/ time_elapsed);

		lsn_avg_rate = (lsn_avg_rate + lsn_rate) / 2;

		ulint	flush_tm = page_cleaner.flush_time;
		ulint	flush_pass = page_cleaner.flush_pass;

		page_cleaner.flush_time = 0;
		page_cleaner.flush_pass = 0;

		if (flush_pass) {
			flush_tm /= flush_pass;
		}

		MONITOR_SET(MONITOR_FLUSH_ADAPTIVE_AVG_TIME, flush_tm);
		MONITOR_SET(MONITOR_FLUSH_ADAPTIVE_AVG_PASS, flush_pass);

		prev_lsn = cur_lsn;
		prev_time = curr_time;

		n_iterations = 0;

		sum_pages = 0;
	}

	const ulint pct_for_dirty = srv_max_dirty_pages_pct_lwm == 0
		? (dirty_pct >= max_pct ? 100 : 0)
		: static_cast<ulint>
		(max_pct > 0.0 ? dirty_pct / max_pct : dirty_pct);
	ulint pct_total = std::max(pct_for_dirty, pct_for_lsn);

	/* Estimate pages to be flushed for the lsn progress */
	lsn_t	target_lsn = oldest_lsn
		+ lsn_avg_rate * buf_flush_lsn_scan_factor;
	ulint	pages_for_lsn = 0;

	mysql_mutex_lock(&buf_pool.flush_list_mutex);

	for (buf_page_t* b = UT_LIST_GET_LAST(buf_pool.flush_list);
	     b != NULL;
	     b = UT_LIST_GET_PREV(list, b)) {
		if (b->oldest_modification() > target_lsn) {
			break;
		}
		if (++pages_for_lsn >= srv_max_io_capacity) {
			break;
		}
	}
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);

	pages_for_lsn /= buf_flush_lsn_scan_factor;
	if (pages_for_lsn < 1) {
		pages_for_lsn = 1;
	}

	n_pages = (ulint(double(srv_io_capacity) * double(pct_total) / 100.0)
		   + avg_page_rate + pages_for_lsn) / 3;

	if (n_pages > srv_max_io_capacity) {
		n_pages = srv_max_io_capacity;
	}

	MONITOR_SET(MONITOR_FLUSH_N_TO_FLUSH_REQUESTED, n_pages);

	MONITOR_SET(MONITOR_FLUSH_N_TO_FLUSH_BY_AGE, pages_for_lsn);

	MONITOR_SET(MONITOR_FLUSH_AVG_PAGE_RATE, avg_page_rate);
	MONITOR_SET(MONITOR_FLUSH_LSN_AVG_RATE, lsn_avg_rate);
	MONITOR_SET(MONITOR_FLUSH_PCT_FOR_DIRTY, pct_for_dirty);
	MONITOR_SET(MONITOR_FLUSH_PCT_FOR_LSN, pct_for_lsn);

	return(n_pages);
}

/******************************************************************//**
page_cleaner thread tasked with flushing dirty pages from the buffer
pools. As of now we'll have only one coordinator.
@return a dummy parameter */
static os_thread_ret_t DECLARE_THREAD(buf_flush_page_cleaner)(void*)
{
  my_thread_init();
#ifdef UNIV_PFS_THREAD
  pfs_register_thread(page_cleaner_thread_key);
#endif /* UNIV_PFS_THREAD */
  ut_ad(!srv_read_only_mode);
  ut_ad(buf_page_cleaner_is_active);

#ifdef UNIV_DEBUG_THREAD_CREATION
  ib::info() << "page_cleaner thread running, id "
             << os_thread_get_curr_id();
#endif /* UNIV_DEBUG_THREAD_CREATION */
#ifdef UNIV_LINUX
  /* linux might be able to set different setting for each thread.
  worth to try to set high priority for the page cleaner thread */
  const pid_t tid= static_cast<pid_t>(syscall(SYS_gettid));
  setpriority(PRIO_PROCESS, tid, -20);
  if (getpriority(PRIO_PROCESS, tid) != -20)
    ib::info() << "If the mysqld execution user is authorized,"
                  " page cleaner thread priority can be changed."
                  " See the man page of setpriority().";
#endif /* UNIV_LINUX */

  ulint last_pages= 0;
  timespec abstime;
  set_timespec(abstime, 1);

  mysql_mutex_lock(&buf_pool.flush_list_mutex);

  lsn_t lsn_limit;

  for (;;)
  {
    lsn_limit= buf_flush_sync_lsn;

    if (UNIV_UNLIKELY(lsn_limit != 0))
    {
furious_flush:
      if (UNIV_LIKELY(srv_flush_sync))
      {
        buf_flush_sync_for_checkpoint(lsn_limit);
        last_pages= 0;
        set_timespec(abstime, 1);
        continue;
      }
    }
    else if (srv_shutdown_state > SRV_SHUTDOWN_INITIATED)
      break;

    if (buf_pool.page_cleaner_idle())
      my_cond_wait(&buf_pool.do_flush_list,
                   &buf_pool.flush_list_mutex.m_mutex);
    else
      my_cond_timedwait(&buf_pool.do_flush_list,
                        &buf_pool.flush_list_mutex.m_mutex, &abstime);

    set_timespec(abstime, 1);

    lsn_limit= buf_flush_sync_lsn;

    if (UNIV_UNLIKELY(lsn_limit != 0))
    {
      if (UNIV_LIKELY(srv_flush_sync))
        goto furious_flush;
    }
    else if (srv_shutdown_state > SRV_SHUTDOWN_INITIATED)
      break;

    const ulint dirty_blocks= UT_LIST_GET_LEN(buf_pool.flush_list);

    if (!dirty_blocks)
    {
      if (UNIV_UNLIKELY(lsn_limit != 0))
      {
        buf_flush_sync_lsn= 0;
        /* wake up buf_flush_wait_flushed() */
        pthread_cond_broadcast(&buf_pool.done_flush_list);
      }
unemployed:
      buf_pool.page_cleaner_set_idle(true);
      continue;
    }

    /* We perform dirty reads of the LRU+free list lengths here.
    Division by zero is not possible, because buf_pool.flush_list is
    guaranteed to be nonempty, and it is a subset of buf_pool.LRU. */
    const double dirty_pct= double(dirty_blocks) * 100.0 /
      double(UT_LIST_GET_LEN(buf_pool.LRU) + UT_LIST_GET_LEN(buf_pool.free));

    if (lsn_limit);
    else if (srv_max_dirty_pages_pct_lwm != 0.0)
    {
      if (dirty_pct < srv_max_dirty_pages_pct_lwm)
        goto unemployed;
    }
    else if (dirty_pct < srv_max_buf_pool_modified_pct)
      goto unemployed;

    const lsn_t oldest_lsn= buf_pool.get_oldest_modified()
      ->oldest_modification();
    ut_ad(oldest_lsn);

    if (UNIV_UNLIKELY(lsn_limit != 0) && oldest_lsn >= lsn_limit)
      buf_flush_sync_lsn= 0;

    buf_pool.page_cleaner_set_idle(false);
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    ulint n_flushed;

    if (UNIV_UNLIKELY(lsn_limit != 0))
    {
      n_flushed= buf_flush_lists(srv_max_io_capacity, lsn_limit);
      /* wake up buf_flush_wait_flushed() */
      pthread_cond_broadcast(&buf_pool.done_flush_list);
      goto try_checkpoint;
    }
    else if (!srv_adaptive_flushing)
    {
      n_flushed= buf_flush_lists(srv_io_capacity, LSN_MAX);
try_checkpoint:
      if (n_flushed)
      {
        MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_BACKGROUND_TOTAL_PAGE,
                                     MONITOR_FLUSH_BACKGROUND_COUNT,
                                     MONITOR_FLUSH_BACKGROUND_PAGES,
                                     n_flushed);
do_checkpoint:
        /* The periodic log_checkpoint() call here makes it harder to
        reproduce bugs in crash recovery or mariabackup --prepare, or
        in code that writes the redo log records. Omitting the call
        here should not affect correctness, because log_free_check()
        should still be invoking checkpoints when needed. */
        DBUG_EXECUTE_IF("ib_log_checkpoint_avoid", goto next;);

        if (!recv_recovery_is_on() && srv_operation == SRV_OPERATION_NORMAL)
          log_checkpoint();
      }
    }
    else if (ulint n= page_cleaner_flush_pages_recommendation(last_pages,
                                                              oldest_lsn,
                                                              dirty_blocks,
                                                              dirty_pct))
    {
      page_cleaner.flush_pass++;
      const ulint tm= ut_time_ms();
      last_pages= n_flushed= buf_flush_lists(n, LSN_MAX);
      page_cleaner.flush_time+= ut_time_ms() - tm;

      if (n_flushed)
      {
        MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_ADAPTIVE_TOTAL_PAGE,
                                     MONITOR_FLUSH_ADAPTIVE_COUNT,
                                     MONITOR_FLUSH_ADAPTIVE_PAGES,
                                     n_flushed);
        goto do_checkpoint;
      }
    }
    else
    {
      mysql_mutex_lock(&buf_pool.flush_list_mutex);
      goto unemployed;
    }

#ifdef UNIV_DEBUG
    while (innodb_page_cleaner_disabled_debug && !buf_flush_sync_lsn &&
           srv_shutdown_state == SRV_SHUTDOWN_NONE)
      os_thread_sleep(100000);
#endif /* UNIV_DEBUG */

#ifndef DBUG_OFF
next:
#endif /* !DBUG_OFF */
    mysql_mutex_lock(&buf_pool.flush_list_mutex);
  }

  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  if (srv_fast_shutdown != 2)
  {
    buf_flush_wait_batch_end_acquiring_mutex(true);
    buf_flush_wait_batch_end_acquiring_mutex(false);
  }

  log_flush_task.wait();

  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  lsn_limit= buf_flush_sync_lsn;
  if (UNIV_UNLIKELY(lsn_limit != 0))
    goto furious_flush;
  buf_page_cleaner_is_active= false;
  pthread_cond_broadcast(&buf_pool.done_flush_list);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  my_thread_end();
  /* We count the number of threads in os_thread_exit(). A created
  thread should always use that to exit and not use return() to exit. */
  os_thread_exit();

  OS_THREAD_DUMMY_RETURN;
}

/** Initialize page_cleaner. */
ATTRIBUTE_COLD void buf_flush_page_cleaner_init()
{
  ut_ad(!buf_page_cleaner_is_active);
  ut_ad(srv_operation == SRV_OPERATION_NORMAL ||
        srv_operation == SRV_OPERATION_RESTORE ||
        srv_operation == SRV_OPERATION_RESTORE_EXPORT);
  buf_flush_sync_lsn= 0;
  buf_page_cleaner_is_active= true;
  os_thread_create(buf_flush_page_cleaner);
}

/** @return the number of dirty pages in the buffer pool */
static ulint buf_flush_list_length()
{
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  const ulint len= UT_LIST_GET_LEN(buf_pool.flush_list);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  return len;
}

/** Flush the buffer pool on shutdown. */
ATTRIBUTE_COLD void buf_flush_buffer_pool()
{
  ut_ad(!buf_page_cleaner_is_active);
  ut_ad(!buf_flush_sync_lsn);

  service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                 "Waiting to flush the buffer pool");

  while (buf_pool.n_flush_list || buf_flush_list_length())
  {
    buf_flush_lists(srv_max_io_capacity, LSN_MAX);
    timespec abstime;

    if (buf_pool.n_flush_list)
    {
      service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                     "Waiting to flush " ULINTPF " pages",
                                     buf_flush_list_length());
      set_timespec(abstime, INNODB_EXTEND_TIMEOUT_INTERVAL / 2);
      mysql_mutex_lock(&buf_pool.mutex);
      while (buf_pool.n_flush_list)
        my_cond_timedwait(&buf_pool.done_flush_list, &buf_pool.mutex.m_mutex,
                          &abstime);
      mysql_mutex_unlock(&buf_pool.mutex);
    }
  }

  ut_ad(!buf_pool.any_io_pending());
  log_flush_task.wait();
}

/** Synchronously flush dirty blocks.
NOTE: The calling thread is not allowed to hold any buffer page latches! */
void buf_flush_sync()
{
  ut_ad(!sync_check_iterate(dict_sync_check()));

  for (;;)
  {
    const ulint n_flushed= buf_flush_lists(srv_max_io_capacity, LSN_MAX);
    buf_flush_wait_batch_end_acquiring_mutex(false);
    if (!n_flushed)
    {
      mysql_mutex_lock(&buf_pool.flush_list_mutex);
      const auto len= UT_LIST_GET_LEN(buf_pool.flush_list);
      mysql_mutex_unlock(&buf_pool.flush_list_mutex);
      if (!len)
        return;
    }
  }
}

#ifdef UNIV_DEBUG
/** Functor to validate the flush list. */
struct	Check {
	void operator()(const buf_page_t* elem) const
	{
		ut_ad(elem->oldest_modification());
		ut_ad(!fsp_is_system_temporary(elem->id().space()));
	}
};

/** Validate the flush list. */
static void buf_flush_validate_low()
{
	buf_page_t*		bpage;

	mysql_mutex_assert_owner(&buf_pool.flush_list_mutex);

	ut_list_validate(buf_pool.flush_list, Check());

	bpage = UT_LIST_GET_FIRST(buf_pool.flush_list);

	while (bpage != NULL) {
		const lsn_t	om = bpage->oldest_modification();
		/* A page in buf_pool.flush_list can be in
		BUF_BLOCK_REMOVE_HASH state. This happens when a page
		is in the middle of being relocated. In that case the
		original descriptor can have this state and still be
		in the flush list waiting to acquire the
		buf_pool.flush_list_mutex to complete the relocation. */
		ut_d(const auto s= bpage->state());
		ut_ad(s == BUF_BLOCK_ZIP_PAGE || s == BUF_BLOCK_FILE_PAGE
		      || s == BUF_BLOCK_REMOVE_HASH);
		ut_ad(om > 0);

		bpage = UT_LIST_GET_NEXT(list, bpage);
		ut_ad(!bpage || recv_recovery_is_on()
		      || om >= bpage->oldest_modification());
	}
}

/** Validate the flush list. */
void buf_flush_validate()
{
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  buf_flush_validate_low();
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
}
#endif /* UNIV_DEBUG */
