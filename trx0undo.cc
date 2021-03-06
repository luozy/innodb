#include "trx0undo.h"

#include "fsp0fsp.h"
#include "mach0data.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "srv0srv.h"
#include "trx0rec.h"
#include "trx0purge.h"

static void trx_undo_page_init(page_t* undo_page, ulint type, mtr_t* mtr);

static trx_undo_t* trx_undo_mem_create(trx_rseg_t* rseg, ulint id, ulint type, dulint trx_id, ulint page_no, ulint offset);

static ulint trx_undo_insert_header_reuse(page_t* undo_page, dulint trx_id, mtr_t* mtr);

static void trx_undo_discard_latest_update_undo(page_t* undo_page, mtr_t* mtr);

/*获得rec的前一条记录且这条记录在前一页中,undo_page前一页的fil_addr保存在TRX_UNDO_PAGE_NODE当中*/
static trx_undo_rec_t* trx_undo_get_prev_rec_from_prev_page(trx_undo_rec_t* rec, ulint page_no, ulint offset, mtr_t* mtr)
{
	ulint	prev_page_no;
	page_t* prev_page;
	page_t*	undo_page;

	undo_page = buf_frame_align(rec);
	prev_page_no = flst_get_prev_addr(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr).page;
	if(prev_page_no == FIL_NULL)
		return NULL;

	/*获取前一页，只需要持有s_latch,因为只是读*/
	prev_page = trx_undo_page_get_s_latched(buf_frame_get_space_id(undo_page), prev_page_no, mtr);

	return trx_undo_page_get_last_rec(prev_page, page_no, offset);
}

/*获取rec在undo page中的前一页*/
trx_undo_rec_t* trx_undo_get_prev_rec(trx_undo_rec_t* rec, ulint page_no, ulint offset, mtr_t* mtr)
{
	trx_undo_rec_t* prev_rec = trx_undo_page_get_prev_rec(rec, page_no, offset);
	if(prev_rec != NULL)
		return prev_rec;
	else /*记录在前一页中*/
		return trx_undo_get_prev_rec_from_prev_page(rec, page_no, offset, mtr);
}
/*获得rec的下一条记录且这条记录在后一页中,undo_page后一页的fil_addr保存在TRX_UNDO_PAGE_NODE当中*/
static trx_undo_rec_t* trx_undo_get_next_rec_from_next_page(page_t* page, ulint page_no, ulint offset, ulint mode, mtr_t* mtr)
{
	trx_ulogf_t*	log_hdr;
	ulint		next_page_no;
	page_t* 	next_page;
	ulint		space;
	ulint		next;

	if (page_no == buf_frame_get_page_no(page)){ /*一个页中有多个事务的undo log,判断如果有多个，说明已经到了undo log的最前面*/
		log_hdr = page + offset;
		next = mach_read_from_2(log_hdr + TRX_UNDO_NEXT_LOG);
		if (next != 0)
			return NULL;
	}

	space = buf_frame_get_space_id(page);
	next_page_no = flst_get_next_addr(page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr).page;
	if(next_page_no == FIL_NULL) /*没有后一页*/
		return NULL;

	if(mode == RW_S_LATCH)
		next_page = trx_undo_page_get_s_latched(space, next_page_no, mtr);
	else /*获取x-latch*/
		next_page = trx_undo_page_get(space, next_page_no, mtr);

	return trx_undo_page_get_first_rec(next_page, page_no, offset);
}

/*获取rec的下一条undo rec*/
trx_undo_rec_t* trx_undo_get_next_rec(trx_undo_rec_t* rec, ulint page_no, ulint offset, mtr_t* mtr)
{
	trx_undo_rec_t*	next_rec;

	next_rec = trx_undo_page_get_next_rec(rec, page_no, offset);
	if(next_rec == NULL)
		next_rec = trx_undo_get_next_rec_from_next_page(buf_frame_align(rec), page_no, offset, RW_S_LATCH, mtr);

	return next_rec;
}

/*获得undo log的第一条undo log rec*/
trx_undo_rec_t* trx_undo_get_first_rec(ulint space, ulint page_no, ulint offset, ulint mode, mtr_t* mtr)
{
	page_t* undo_page;
	trx_undo_rec_t* rec;

	if(mode == RW_S_LATCH)
		undo_page = trx_undo_page_get_s_latched(space, page_no, mtr);
	else
		undo_page = trx_undo_page_get(space, page_no, mtr);

	rec = trx_undo_page_get_first_rec(undo_page, page_no, offset);
	if(rec == NULL)
		rec = trx_undo_get_next_rec_from_next_page(undo_page, page_no, offset, mode, mtr);

	return rec;
}

/*产生一条undo page初始化的mini transaction log*/
UNIV_INLINE void trx_undo_page_init_log(page_t* undo_page, ulint type, mtr_t* mtr)
{
	mlog_write_initial_log_record(undo_page, MLOG_UNDO_INIT, mtr);
	mlog_catenate_ulint_compressed(mtr, type);
}

/*解读和重演初始化undo page的redo mtr log*/
byte* trx_undo_parse_page_init(byte* ptr, byte* end_ptr, page_t* page, mtr_t* mtr)
{
	ulint type;
	ptr = mach_parse_compressed(ptr, end_ptr, &type);
	if(ptr == NULL)
		return NULL;

	if(page != NULL)
		trx_undo_page_init(page, type, mtr);

	return ptr;
}

/*初始化一个undo page*/
static void trx_undo_page_init(page_t* undo_page, ulint type, mtr_t* mtr)
{
	trx_upagef_t* page_hdr;

	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_TYPE, type);	/*设置undo page的类型*/
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE); /*设置记录起始位置*/
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE); /*设置页空闲可写的位置*/

	fil_page_set_type(undo_page, FIL_PAGE_UNDO_LOG); /*在fil_header中设置页为undo page页*/

	trx_undo_page_init_log(undo_page, type, mtr);
}

/*创建一个新的undo log segment*/
static page_t* trx_undo_seg_create(trx_rseg_t* rseg, trx_rsegf_t* rseg_hdr, ulint type, ulint* id, mtr_t* mtr)
{
	ulint		slot_no;
	ulint		space;
	page_t* 	undo_page;
	trx_upagef_t*	page_hdr;
	trx_usegf_t*	seg_hdr;
	ibool		success;

	ut_ad(mtr && id && rseg_hdr);
	ut_ad(mutex_own(&(rseg->mutex)));

	/*在rollback segment中获得一个空闲的undo segment槽位*/
	slot_no = trx_rsegf_undo_find_free(rseg_hdr, mtr);
	if(slot_no == ULINT_UNDEFINED){  /*rollback segment中没有槽位*/
		ut_print_timestamp(stderr);
		fprintf(stderr, "InnoDB: Warning: cannot find a free slot for an undo log. Do you have too\n"
			"InnoDB: many active transactions running concurrently?");

		return NULL;
	}

	/*在事务表空间上开辟1个新的extent,如果没有空间，尝试扩大物理文件区域*/
	space = buf_frame_get_space_id(rseg_hdr);
	success = fsp_reserve_free_extents(space, 2, FSP_UNDO, mtr);
	if(!success)
		return NULL;

	/*在表空间上分配一个undo segment*/
	undo_page = fseg_create_general(space, 0, TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER, TRUE, mtr);
	fil_space_release_free_extents(space, 2);
	if(undo_page == NULL)
		return NULL;

	buf_page_dbg_add_level(undo_page, SYNC_TRX_UNDO_PAGE);
	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;
	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

	trx_undo_page_init(undo_page, type, mtr);
	/*第一个页会增加一个磁盘链表的存储位置,所以需要重新设置*/
	mlog_write_ulint(page_hdr + TRX_UNDO_PAGE_FREE, TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE, MLOG_2BYTES, mtr);
	mlog_write_ulint(seg_hdr + TRX_UNDO_LAST_LOG, 0, MLOG_2BYTES, mtr);
	
	/*初始化page list*/
	flst_init(seg_hdr + TRX_UNDO_PAGE_LIST, mtr);
	flst_add_last(seg_hdr + TRX_UNDO_PAGE_LIST, page_hdr + TRX_UNDO_PAGE_NODE, mtr);

	/*将这个undo page设置到rollback segment slots中*/
	trx_rsegf_set_nth_undo(rseg_hdr, slot_no, buf_frame_get_page_no(undo_page), mtr);

	*id = slot_no;

	return undo_page;
}

/*对初始化undo log header进行写入mini transaction log*/
UNIV_INLINE void trx_undo_header_create_log(page_t* undo_page, dulint trx_id, mtr_t* mtr)
{
	mlog_write_initial_log_record(undo_page, MLOG_UNDO_HDR_CREATE, mtr);
	mlog_catenate_dulint_compressed(mtr, trx_id);
}

/*创建一个undo log header,返回undo log header的起始位置偏移*/
static ulint trx_undo_header_create(page_t* undo_page, dulint trx_id, mtr_t* mtr)
{
	trx_upagef_t*	page_hdr;
	trx_usegf_t*	seg_hdr;
	trx_ulogf_t*	log_hdr;
	trx_ulogf_t*	prev_log_hdr;
	ulint		prev_log;
	ulint		free;
	ulint		new_free;

	ut_ad(mtr && undo_page);

	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;
	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

	/*获取可写空闲位置*/
	free = mach_read_from_2(page_hdr + TRX_UNDO_PAGE_FREE);
	log_hdr = undo_page + free;

	new_free = free + TRX_UNDO_LOG_HDR_SIZE;
	ut_ad(new_free <= UNIV_PAGE_SIZE);

	/*重新改写undo log起始位置和空闲可写位置*/
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START, new_free);
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, new_free);
	mach_write_to_2(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_ACTIVE);

	/*同一个undo page中可能有多个事务的undo log,这个是获取前一个事务undo log的起始偏移*/
	prev_log = mach_read_from_2(seg_hdr + TRX_UNDO_LAST_LOG);
	if(prev_log != 0){
		prev_log_hdr = undo_page + prev_log;
		mach_write_to_2(prev_log_hdr + TRX_UNDO_NEXT_LOG, free); /*设置上一个事务undo log header的next log偏移为本次要创建的undo log起始位置*/
	}
	mach_write_to_2(seg_hdr + TRX_UNDO_LAST_LOG, free);

	/*设置本次创建的undo log header位置*/
	log_hdr = undo_page + free;
	/*对undo log header设置值*/
	mach_write_to_2(log_hdr + TRX_UNDO_DEL_MARKS, TRUE);
	mach_write_to_8(log_hdr + TRX_UNDO_TRX_ID, trx_id);
	mach_write_to_2(log_hdr + TRX_UNDO_LOG_START, new_free);
	mach_write_to_2(log_hdr + TRX_UNDO_DICT_OPERATION, FALSE);
	mach_write_to_2(log_hdr + TRX_UNDO_NEXT_LOG, 0);
	mach_write_to_2(log_hdr + TRX_UNDO_PREV_LOG, prev_log);
	/*记录mini transaction log*/
	trx_undo_header_create_log(undo_page, trx_id, mtr);

	return free;
}

UNIV_INLINE void trx_undo_insert_header_reuse_log(page_t* undo_header, dulint trx_id, mtr_t* mtr)
{
	mlog_write_initial_log_record(undo_header, MLOG_UNDO_HDR_REUSE, mtr);
	mlog_catenate_dulint_compressed(mtr, trx_id);
}

/*解读和重演undo log page header的创建过程mini transaction log*/
byte* trx_undo_parse_page_header(ulint type, byte* ptr, byte* end_ptr, page_t* page, mtr_t* mtr)
{
	dulint trx_id;

	ptr = mach_dulint_parse_compressed(ptr, end_ptr, &trx_id);
	if(ptr == NULL)
		return NULL;

	if(page != NULL){
		if(type == MLOG_UNDO_HDR_CREATE)
			trx_undo_header_create(page, trx_id, mtr);
		else{
			ut_ad(type == MLOG_UNDO_HDR_REUSE);
			trx_undo_insert_header_reuse(page, trx_id, mtr);
		}
	}

	return ptr;
}

/*初始化缓冲中的insert undo log header page*/
static ulint trx_undo_insert_header_reuse(page_t* undo_page, dulint trx_id, mtr_t* mtr)
{
	trx_upagef_t*	page_hdr;
	trx_usegf_t*	seg_hdr;
	trx_ulogf_t*	log_hdr;
	ulint		free;
	ulint		new_free;

	ut_ad(mtr && undo_page);

	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;
	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

	free = TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE;
	log_hdr = undo_page + free;
	new_free = free + TRX_UNDO_LOG_HDR_SIZE;

	ut_a(mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE) == TRX_UNDO_INSERT);

	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START, new_free);
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, new_free);
	mach_write_to_2(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_ACTIVE);

	log_hdr = undo_page + free;
	mach_write_to_8(log_hdr + TRX_UNDO_TRX_ID, trx_id);
	mach_write_to_2(log_hdr + TRX_UNDO_LOG_START, new_free);
	mach_write_to_2(log_hdr + TRX_UNDO_DICT_OPERATION, FALSE);

	trx_undo_insert_header_reuse_log(undo_page, trx_id, mtr);

	return free;
}

/*为废弃一个undo log header写入一条mini transaction log*/
UNIV_INLINE void trx_undo_discard_latest_log(page_t* undo_page, mtr_t* mtr)
{
	mlog_write_initial_log_record(undo_page, MLOG_UNDO_HDR_DISCARD, mtr);
}

/*解读和重演MLOG_UNDO_HDR_DISCARD log*/
byte* trx_undo_parse_discard_latest(byte* ptr, byte* end_ptr, page_t* page, mtr_t* mtr)
{
	ut_ad(end_ptr);
	if(page != NULL)
		trx_undo_discard_latest_update_undo(page, mtr);

	return ptr;
}

/*假如一个upate undo log可以废弃，这个函数会释放对应的存储空间和重置page状态*/
static void trx_undo_discard_latest_update_undo(page_t* undo_page, mtr_t* mtr)
{
	trx_usegf_t*	seg_hdr;
	trx_upagef_t*	page_hdr;
	trx_ulogf_t*	log_hdr;
	trx_ulogf_t*	prev_log_hdr;
	ulint			free;
	ulint			prev_hdr_offset;

	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;
	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;

	/*获得最后一个undo log header的位置*/
	free = mach_read_from_2(seg_hdr + TRX_UNDO_LAST_LOG);
	log_hdr = undo_page + free;
	/*获得倒数第二个undo log header的位置*/
	prev_hdr_offset = mach_read_from_2(log_hdr + TRX_UNDO_PREV_LOG);
	if(prev_hdr_offset != 0){ /*更改倒数第二个undo log header的NEXT LOG为0*/
		prev_log_hdr = undo_page + prev_hdr_offset;
		mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START, mach_read_from_2(prev_log_hdr + TRX_UNDO_LOG_START));
		mach_write_to_2(prev_log_hdr + TRX_UNDO_NEXT_LOG, 0);
	}
	/*重置页的free和状态(TRX_UNDO_CACHED),通过purge来做页回收*/
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, free);
	mach_write_to_2(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_CACHED);
	mach_write_to_2(seg_hdr + TRX_UNDO_LAST_LOG, prev_hdr_offset);

	/*记录redo log*/
	trx_undo_discard_latest_log(undo_page, mtr);
}

/*尝试增加一个undo page到undo log segment中*/
ulint trx_undo_add_page(trx_t* trx, trx_undo_t* undo, mtr_t* mtr)
{
	page_t*		header_page;
	page_t*		new_page;
	trx_rseg_t*	rseg;
	ulint		page_no;
	ibool		success;

	ut_ad(mutex_own(&(trx->undo_mutex)));
	ut_ad(!mutex_own(&kernel_mutex));

	rseg = trx->rseg;
	ut_ad(mutex_own(&(rseg->mutex)));

	/*回滚段到了上限*/
	if(rseg->curr_size >= rseg->max_size)
		return FIL_NULL;

	header_page = trx_undo_page_get(undo->space, undo->hdr_page_no, mtr);
	success = fsp_reserve_free_extents(undo->space, 1, FSP_UNDO, mtr);
	if(!success)
		return FIL_NULL;

	/*获得一个新页*/
	page_no = fseg_alloc_free_page_general(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER, undo->top_page_no + 1, FSP_UP, TRUE, mtr);
	fil_space_release_free_extents(undo->space, 1);

	/*表空间没有更多的空间*/
	if(page_no == FIL_NULL)
		return FIL_NULL;

	/*建立对应关系*/
	undo->last_page_no = page_no;
	new_page = trx_undo_page_get(undo->space, page_no, mtr);
	/*进行undo page页初始化*/
	trx_undo_page_init(new_page, undo->type, mtr);
	flst_add_last(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST, new_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr);
	/*更改undo和rollback segment的状态*/
	undo->size++;
	rseg->curr_size++;

	return page_no;
}

/*释放一个不是undo log segment头页的undo log page*/
static ulint trx_undo_free_page(trx_rseg_t* rseg, ibool in_history, ulint space, ulint hdr_page_no, ulint offset, ulint page_no, mtr_t* mtr)
{
	page_t*		header_page;
	page_t*		undo_page;
	fil_addr_t	last_addr;
	trx_rsegf_t*	rseg_header;
	ulint		hist_size;

	UT_NOT_USED(hdr_offset);
	ut_a(hdr_page_no != page_no);
	ut_ad(!mutex_own(&kernel_mutex));
	ut_ad(mutex_own(&(rseg->mutex)));

	undo_page = trx_undo_page_get(space, page_no, mtr);
	header_page = trx_undo_page_get(space, hdr_page_no, mtr);
	/*将page从头页的磁盘链表中删除*/
	flst_remove(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST,
		undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr);

	/*将page归还个file space*/
	fseg_free_page(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER, space, page_no, mtr);
	/*获得页链表最后一个page的fil_addr*/
	last_addr = flst_get_last(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST, mtr);
	
	rseg->curr_size --;
	if(in_history){ /*更改rollback segment的history信息*/
		rseg_header = trx_rsegf_get(space, rseg->page_no, mtr);
		hist_size = mtr_read_ulint(rseg_header + TRX_RSEG_HISTORY_SIZE, MLOG_4BYTES, mtr);
		ut_ad(hist_size > 0);
		mlog_write_ulint(rseg_header + TRX_RSEG_HISTORY_SIZE, hist_size - 1, MLOG_4BYTES, mtr);
	}

	return last_addr.page;
}
/*回滚时释放一个undo log page*/
static void trx_undo_free_page_in_rollback(trx_t* trx, trx_undo_t* undo, ulint page_no, mtr_t* mtr)
{
	ulint	last_page_no;

	ut_ad(undo->hdr_page_no != page_no);
	ut_ad(mutex_own(&(trx->undo_mutex)));

	last_page_no = trx_undo_free_page(undo->rseg, FALSE, undo->space, undo->hdr_page_no, undo->hdr_offset, page_no, mtr);

	undo->last_page_no = last_page_no;
	undo->size--;
}

static void trx_undo_empty_header_page(ulint space, ulint hdr_page_no, ulint hdr_offset, mtr_t* mtr)
{
	page_t*		header_page;
	trx_ulogf_t*	log_hdr;
	ulint		end;

	header_page = trx_undo_page_get(space, hdr_page_no, mtr);
	log_hdr = header_page + hdr_offset;

	end = trx_undo_page_get_end(header_page, hdr_page_no, hdr_offset);
	mlog_write_ulint(log_hdr + TRX_UNDO_LOG_START, end, MLOG_2BYTES, mtr);
}

/*释放所有undo number > limit的undo rec， 从最后一个page向前释放,一般在rollback时调用*/
void trx_undo_truncate_end(trx_t* trx, trx_undo_t* undo, dulint limit)
{
	page_t*		undo_page;
	ulint		last_page_no;
	trx_undo_rec_t* rec;
	trx_undo_rec_t* trunc_here;
	trx_rseg_t*	rseg;
	mtr_t		mtr;

	ut_ad(mutex_own(&(trx->undo_mutex)));
	ut_ad(mutex_own(&(rseg->mutex)));

	rseg = trx->rseg;
	for(;;){
		mtr_start(&mtr);
		trunc_here = NULL;
		last_page_no = undo->last_page_no;
		undo_page = trx_undo_page_get(undo->space, last_page_no, &mtr);

		rec = trx_undo_page_get_last_rec(undo_page, undo->hdr_page_no, undo->hdr_offset);
		for(;;){
			if (rec == NULL) { /*已经是在最后一页上，如果不是头页，直接释放掉最后一页*/
				if(last_page_no == undo->hdr_page_no)
					goto function_exit;

				trx_undo_free_page_in_rollback(trx, undo, last_page_no, &mtr);
				break;
			}
			/*释放所有undo number > limit的undo rec*/
			if(ut_dulint_cmp(trx_undo_rec_get_undo_no(rec), limit) >= 0)
				trunc_here = rec;
			else
				goto function_exit;

			rec = trx_undo_page_get_prev_rec(rec, undo->hdr_page_no, undo->hdr_offset);
		}

		mtr_commit(&mtr);
	}

function_exit:
	if (trunc_here) 
		mlog_write_ulint(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE, trunc_here - undo_page, MLOG_2BYTES, &mtr);

	mtr_commit(&mtr);
}

/*释放所有小limit的undo log rec,一般在purge时调用*/
void trx_undo_truncate_start(trx_rseg_t* rseg, ulint space, ulint hdr_page_no, ulint hdr_offset, dulint limit)
{
	page_t* 	undo_page;
	trx_undo_rec_t* rec;
	trx_undo_rec_t* last_rec;
	ulint		page_no;
	mtr_t		mtr;

	ut_ad(mutex_own(&(rseg->mutex)));
	/*limit一定要大于0*/
	if(0 == ut_dulint_cmp(limit, ut_dulint_zero))
		return ;

loop:
	mtr_start(&mtr);
	rec = trx_undo_get_first_rec(space, hdr_page_no, hdr_offset, RW_X_LATCH, &mtr);
	if(rec == NULL){
		mtr_commit(&mtr);
		return ;
	}

	undo_page = buf_frame_align(rec);
	last_rec = trx_undo_page_get_last_rec(undo_page, hdr_page_no, hdr_offset);
	if (ut_dulint_cmp(trx_undo_rec_get_undo_no(last_rec), limit) >= 0) { /*已经到了比limit大的undo log rec了，不能再释放了*/
		mtr_commit(&mtr);
		return;
	}

	page_no = buf_frame_get_page_no(undo_page);
	if(page_no == hdr_page_no) /*如果page_no是头页，将记录作废（设置log_hdr的开始位置大于rec的末尾）*/
		trx_undo_empty_header_page(space, hdr_page_no, hdr_offset, &mtr);
	else
		trx_undo_free_page(rseg, TRUE, space, hdr_page_no, hdr_offset, page_no, &mtr);
	/*继续判断下一条undo rec是否不是可以释放*/
	goto loop;
}

/*释放undo log段*/
static void trx_undo_seg_free(trx_undo_t* undo)
{
	trx_rseg_t*	rseg;
	fseg_header_t*	file_seg;
	trx_rsegf_t*	rseg_header;
	trx_usegf_t*	seg_header;
	ibool		finished;
	mtr_t		mtr;

	finished = FALSE;
	rseg = undo->rseg;

	while(!finished){
		mtr_start(&mtr);

		ut_ad(!mutex_own(&kernel_mutex));
		mutex_enter(&(rseg->mutex));

		seg_header = trx_undo_page_get(undo->space, undo->hdr_page_no, &mtr) + TRX_UNDO_SEG_HDR;
		file_seg = seg_header + TRX_UNDO_FSEG_HEADER;

		finished = fseg_free_step(file_seg, &mtr);
		if(finished){ /*直到所有的页全部归还给file space, 归还rollback segment的槽位*/
			rseg_header = trx_rsegf_get(rseg->space, rseg->page_no, &mtr);
			trx_rsegf_set_nth_undo(rseg_header, undo->id, FIL_NULL, &mtr);
		}

		mutex_exit(&(rseg->mutex));
		mtr_commit(&mtr);
	}
}

/*当数据库启动时，根据undo的头页构建内存中的undo对象(trx_undo_t)*/
static trx_undo_t* trx_undo_mem_create_at_db_start(trx_rseg_t* rseg, ulint id, ulint page_no, mtr_t* mtr)
{
	page_t*		undo_page;
	trx_upagef_t*	page_header;
	trx_usegf_t*	seg_header;
	trx_ulogf_t*	undo_header;
	trx_undo_t*	undo;
	ulint		type;
	ulint		state;
	dulint		trx_id;
	ulint		offset;
	fil_addr_t	last_addr;
	page_t*		last_page;
	trx_undo_rec_t*	rec;

	if(id >= TRX_RSEG_N_SLOTS){
		fprintf(stderr, "InnoDB: Error: undo->id is %lu\n", id);
		ut_a(0);
	}
	/*获得undo page指针和undo page header指针*/
	undo_page = trx_undo_page_get(rseg->space, page_no, mtr);
	page_header = undo_page + TRX_UNDO_PAGE_HDR;
	/*获得undo的类型*/
	type = mtr_read_ulint(page_header + TRX_UNDO_PAGE_TYPE, MLOG_2BYTES, mtr);
	/*获得undo segment header*/
	seg_header = undo_page + TRX_UNDO_SEG_HDR;
	state = mach_read_from_2(seg_header + TRX_UNDO_STATE);
	offset = mach_read_from_2(seg_header + TRX_UNDO_LAST_LOG);

	undo_header = undo_page + offset;

	trx_id = mtr_read_dulint(undo_header + TRX_UNDO_TRX_ID, MLOG_8BYTES, mtr);
	
	mutex_enter(&(rseg->mutex));
	undo = trx_undo_mem_create(rseg, id, type, trx_id, page_no, offset);
	mutex_exit(&(rseg->mutex));

	undo->dict_operation = mtr_read_ulint(undo_header + TRX_UNDO_DICT_OPERATION, MLOG_2BYTES, mtr);
	undo->table_id = mtr_read_dulint(undo_header + TRX_UNDO_TABLE_ID, MLOG_8BYTES, mtr);
	undo->state = state;
	undo->size = flst_get_len(seg_header + TRX_UNDO_PAGE_LIST, mtr);
	if(state == TRX_UNDO_TO_FREE)
		goto add_to_list;

	last_addr = flst_get_last(seg_header + TRX_UNDO_PAGE_LIST, mtr);
	undo->last_page_no = last_addr.page;
	undo->top_page_no = last_addr.page;

	/*载入undo的最后一页，并且将最后一条记录的偏移读出*/
	last_page = trx_undo_page_get(rseg->space, undo->last_page_no, mtr);
	rec = trx_undo_page_get_last_rec(last_page, page_no, offset);
	if(rec == NULL)
		undo->empty = TRUE;
	else{
		undo->empty = FALSE;
		undo->top_offset = rec - last_page;						/*最后一条记录的页中的偏移量*/
		undo->top_undo_no = trx_undo_rec_get_undo_no(rec);		/*最后一条记录的undo number*/
	}

	/*将undo 对象加入到rollback segment的对象中*/
add_to_list:	
	if (type == TRX_UNDO_INSERT) { 
		if (state != TRX_UNDO_CACHED)
			UT_LIST_ADD_LAST(undo_list, rseg->insert_undo_list, undo);
		else
			UT_LIST_ADD_LAST(undo_list, rseg->insert_undo_cached, undo);
	} 
	else {
		ut_ad(type == TRX_UNDO_UPDATE);
		if (state != TRX_UNDO_CACHED)
			UT_LIST_ADD_LAST(undo_list, rseg->update_undo_list, undo);
		else
			UT_LIST_ADD_LAST(undo_list, rseg->update_undo_cached, undo);
	}

	return undo;
}

/*对rollback segment中的undo lists进行初始化*/
ulint trx_undo_lists_init(trx_rseg_t* rseg)
{
	ulint		page_no;
	trx_undo_t*	undo;
	ulint		size	= 0;
	trx_rsegf_t*	rseg_header;
	ulint		i;
	mtr_t		mtr;

	UT_LIST_INIT(rseg->update_undo_list);
	UT_LIST_INIT(rseg->update_undo_cached);
	UT_LIST_INIT(rseg->insert_undo_list);
	UT_LIST_INIT(rseg->insert_undo_cached);

	mtr_start(&mtr);

	rseg_header = trx_rsegf_get_new(rseg->space, rseg->page_no, &mtr);
	for(i = 0; i < TRX_RSEG_N_SLOTS; i++){ /*根据rseg slots中的page no进行undo对象构建*/
		page_no = trx_rsegf_get_nth_undo(rseg_header, i, &mtr);
		if(page_no != FIL_NULL && srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN){
			undo = trx_undo_mem_create_at_db_start(rseg, i, page_no, &mtr);
			size += undo->size;

			mtr_commit(&mtr);
			mtr_start(&mtr);

			/*对rseg header page重新获得x-latch所有权，因为上面进行了mtr_commit，x-latch被释放了*/
			rseg_header = trx_rsegf_get(rseg->space, rseg->page_no, &mtr);
		}
	}

	mtr_commit(&mtr);

	return size;
}

/*创建并初始化一个trx_undo_t*/
static trx_undo_t* trx_undo_mem_create(trx_rseg_t* rseg, ulint id, ulint type, dulint trx_id, ulint page_no, ulint offset)
{
	trx_undo_t*	undo;

	ut_ad(mutex_own(&(rseg->mutex)));

	if (id >= TRX_RSEG_N_SLOTS) {
		fprintf(stderr, "InnoDB: Error: undo->id is %lu\n", id);
		ut_a(0);
	}

	undo = mem_alloc(sizeof(trx_undo_t));
	undo->id = id;
	undo->type = type;
	undo->state = TRX_UNDO_ACTIVE;
	undo->del_marks = FALSE;
	undo->trx_id = trx_id;
	undo->dict_operation = FALSE;
	undo->rseg = rseg;

	undo->space = rseg->space;
	undo->hdr_page_no = page_no;
	undo->hdr_offset = offset;
	undo->last_page_no = page_no;
	undo->size = 1;

	undo->empty = TRUE;
	undo->top_page_no = page_no;
	undo->guess_page = NULL;

	return undo;
}

/*初始化一个cached状态的trx_undo_t对象*/
static void trx_undo_mem_init_for_reuse(trx_undo_t* undo, dulint trx_id, ulint offset)
{
	ut_ad(mutex_own(&((undo->rseg)->mutex)));

	if (undo->id >= TRX_RSEG_N_SLOTS) {
		fprintf(stderr, "InnoDB: Error: undo->id is %lu\n", undo->id);
		mem_analyze_corruption((byte*)undo);
		ut_a(0);
	}

	undo->state = TRX_UNDO_ACTIVE;
	undo->del_marks = FALSE;
	undo->trx_id = trx_id;

	undo->dict_operation = FALSE;

	undo->hdr_offset = offset;
	undo->empty = TRUE;
}

/*释放一个内存的undo对象*/
static void trx_undo_mem_free(trx_undo_t* undo)
{
	if (undo->id >= TRX_RSEG_N_SLOTS) {
		fprintf(stderr, "InnoDB: Error: undo->id is %lu\n", undo->id);
		ut_a(0);
	}

	mem_free(undo);
}

/*新建一个undo log对象,可能会有磁盘IO*/
static trx_undo_t* trx_undo_create(trx_rseg_t* rseg, ulint type, dulint trx_id, mtr_t* mtr)
{
	trx_rsegf_t*	rseg_header;
	ulint		page_no;
	ulint		offset;
	ulint		id;
	trx_undo_t*	undo;
	page_t*		undo_page;

	ut_ad(mutex_own(&(rseg->mutex)));
	if(rseg->curr_size == rseg->max_size)
		return NULL;

	rseg->curr_size ++;

	rseg_header = trx_rsegf_get(rseg->space, rseg->page_no, mtr);
	/*新建个undo log段*/
	undo_page = trx_undo_seg_create(rseg, rseg_header, type, &id, mtr);
	if(undo_page == NULL){
		rseg->curr_size --;
		return NULL;
	}

	page_no = buf_frame_get_page_no(undo_page);
	/*创建一个undo log header*/
	offset = trx_undo_header_create(undo_page, trx_id, mtr);
	/*新建一个undo对象*/
	undo = trx_undo_mem_create(rseg, id, type, trx_id, page_no, offset);

	return undo;
}

/*复用cached中的undo对象*/
static trx_undo_t* trx_undo_reuse_cached(trx_rseg_t* rseg, ulint type, dulint trx_id, mtr_t* mtr)
{
	trx_undo_t*	undo;
	page_t*		undo_page;
	ulint		offset;

	ut_ad(mutex_own(&(rseg->mutex)));
	/*从cached中获取一个undo对象*/
	if(type == TRX_UNDO_INSERT){
		undo = UT_LIST_GET_FIRST(rseg->insert_undo_cached);
		if(undo == NULL)
			return NULL;

		UT_LIST_REMOVE(undo_list, rseg->insert_undo_cached, undo);
	}
	else{
		ut_ad(type == TRX_UNDO_UPDATE);
		undo = UT_LIST_GET_FIRST(rseg->update_undo_cached);
		if(undo == NULL)
			return NULL;

		UT_LIST_REMOVE(undo_list, rseg->update_undo_cached, undo);
	}

	ut_ad(undo->size == 1);
	ut_ad(undo->hdr_page_no == undo->top_page_no);

	if(undo->id == TRX_RSEG_N_SLOTS){
		fprintf(stderr, "InnoDB: Error: undo->id is %lu\n", undo->id);
		mem_analyze_corruption((byte*)undo);
		ut_a(0);
	}

	undo_page = trx_undo_page_get(undo->space, undo->hdr_page_no, mtr);
	if(type == TRX_UNDO_INSERT)
		offset = trx_undo_insert_header_reuse(undo_page, trx_id, mtr);
	else{
		ut_a(mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE) == TRX_UNDO_UPDATE);
		offset = trx_undo_header_create(undo_page, trx_id, mtr);
	}

	/*复用赋值*/
	trx_undo_mem_init_for_reuse(undo, trx_id, offset);

	return undo;
}
/*标记undo的DDL操作*/
static void trx_undo_mark_as_dict_operation(trx_t* trx, trx_undo_t* undo, mtr_t* mtr)
{
	page_t*	hdr_page;

	ut_a(trx->dict_operation);

	hdr_page = trx_undo_page_get(undo->space, undo->hdr_page_no, mtr);

	mlog_write_ulint(hdr_page + undo->hdr_offset + TRX_UNDO_DICT_OPERATION, trx->dict_operation, MLOG_2BYTES, mtr);
	mlog_write_dulint(hdr_page + undo->hdr_offset + TRX_UNDO_TABLE_ID, trx->table_id, MLOG_8BYTES, mtr);	

	undo->dict_operation = trx->dict_operation;
	undo->table_id = trx->table_id;
}

/*分配一个undo对象，会创建undo log segment和构建undo内存对象*/
trx_undo_t* trx_undo_assign_undo(trx_t* trx, ulint type)
{
	trx_rseg_t*	rseg;
	trx_undo_t*	undo;
	mtr_t		mtr;

	ut_ad(trx);
	ut_ad(trx->rseg);

	rseg = trx->rseg;

	ut_ad(mutex_own(&(trx->undo_mutex)));
	mtr_start(&mtr);
	ut_ad(!mutex_own(&kernel_mutex));
	
	mutex_enter(&(rseg->mutex));

	undo = trx_undo_reuse_cached(rseg, type, trx->id, &mtr);
	if(undo == NULL){ /*cached list中没有undo对象*/
		undo = trx_undo_create(rseg, type, trx->id, &mtr);
		if(undo == NULL){
			mutex_exit(&(rseg->mutex));
			mtr_commit(&mtr);

			return(NULL);
		}
	}
	/*将undo放入正式使用的undo list中*/
	if (type == TRX_UNDO_INSERT) {
		UT_LIST_ADD_FIRST(undo_list, rseg->insert_undo_list, undo);
		ut_ad(trx->insert_undo == NULL);
		trx->insert_undo = undo;
	} 
	else {
		UT_LIST_ADD_FIRST(undo_list, rseg->update_undo_list, undo);
		ut_ad(trx->update_undo == NULL);
		trx->update_undo = undo;
	}

	if (trx->dict_operation)
		trx_undo_mark_as_dict_operation(trx, undo, &mtr);

	mutex_exit(&(rseg->mutex));
	mtr_commit(&mtr);
}

/*在事务结束时设置对应undo segment的状态，便以释放和复用*/
page_t* trx_undo_set_state_at_finish(trx_t* trx, trx_undo_t* undo, mtr_t* mtr)
{
	trx_usegf_t*	seg_hdr;
	trx_upagef_t*	page_hdr;
	page_t*		undo_page;
	ulint		state;

	ut_ad(trx && undo && mtr);

	if (undo->id >= TRX_RSEG_N_SLOTS) {
		fprintf(stderr, "InnoDB: Error: undo->id is %lu\n", undo->id);
		mem_analyze_corruption((byte*)undo);
		ut_a(0);
	}

	undo_page = trx_undo_page_get(undo->space, undo->hdr_page_no, mtr);
	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;
	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;

	if(undo->size == 1 && mach_read_from_2(page_hdr + TRX_UNDO_PAGE_FREE) < TRX_UNDO_PAGE_REUSE_LIMIT) /*可以复用*/
		state = TRX_UNDO_CACHED;
	else if(undo->type == TRX_UNDO_INSERT)
		state = TRX_UNDO_TO_FREE;
	else
		state = TRX_UNDO_TO_PURGE;

	undo->state = state;
	mlog_write_ulint(seg_hdr + TRX_UNDO_STATE, state, MLOG_2BYTES, mtr);

	return undo_page;
}

/*对update undo segment的清空*/
void trx_undo_update_cleanup(trx_t* trx, page_t* undo_page, mtr_t* mtr)
{
	trx_rseg_t*	rseg;
	trx_undo_t*	undo;

	undo = trx->update_undo;
	rseg = trx->rseg;

	ut_ad(mutex_own(&(rseg->mutex)));

	trx_purge_add_update_undo_to_history(trx, undo_page, mtr);
	UT_LIST_REMOVE(undo_list, rseg->update_undo_list, undo);
	trx->update_undo = NULL;

	if(undo->state == TRX_UNDO_CACHED) /*可以复用,放入cached list中*/
		UT_LIST_ADD_FIRST(undo_list, rseg->update_undo_cached, undo);
	else{
		ut_ad(undo->state == TRX_UNDO_TO_PURGE);
		trx_undo_mem_free(undo);
	}
}

/*这个还不知道啥意思！？？先废弃对应页中的undo log 段数据，再回收到cached当中*/
dulint trx_undo_update_cleanup_by_discard(trx_t* trx, mtr_t* mtr)
{
	trx_rseg_t*	rseg;
	trx_undo_t*	undo;
	page_t*		undo_page;

	undo = trx->update_undo;
	rseg = trx->rseg;

	ut_ad(mutex_own(&(rseg->mutex)));
	ut_ad(mutex_own(&kernel_mutex));
	ut_ad(undo->size == 1);
	ut_ad(undo->del_marks == FALSE);	
	ut_ad(UT_LIST_GET_LEN(trx_sys->view_list) == 1);

	undo_page = trx_undo_page_get(undo->space, undo->hdr_page_no, mtr);
	trx_undo_discard_latest_update_undo(undo_page, mtr);

	undo->state = TRX_UNDO_CACHED;
	UT_LIST_REMOVE(undo_list, rseg->update_undo_list, undo);
	trx->update_undo = NULL;
	UT_LIST_ADD_FIRST(undo_list, rseg->update_undo_cached, undo);

	mtr_commit(mtr);

	return mtr->end_lsn;
}

/*对insert undo对象的清空*/
void trx_undo_insert_cleanup(trx_t* trx)
{
	trx_undo_t*	undo;
	trx_rseg_t*	rseg;

	undo = trx->insert_undo;
	ut_ad(undo);

	rseg = trx->rseg;

	mutex_enter(&(rseg->mutex));

	UT_LIST_REMOVE(undo_list, rseg->insert_undo_list, undo);
	trx->insert_undo = NULL;

	if (undo->state == TRX_UNDO_CACHED)
		UT_LIST_ADD_FIRST(undo_list, rseg->insert_undo_cached, undo);
	else{
		ut_ad(undo->state == TRX_UNDO_TO_FREE);
		mutex_exit(&(rseg->mutex));
		/*释放磁盘空间上的undo log segment*/
		trx_undo_seg_free(undo);

		mutex_enter(&(rseg->mutex));

		ut_ad(rseg->curr_size > undo->size);
		rseg->curr_size -= undo->size;
		/*释放内存中的undo对象*/
		trx_undo_mem_free(undo);
	}

	mutex_exit(&(rseg->mutex));
}

