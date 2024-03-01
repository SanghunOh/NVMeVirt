// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ktime.h>
#include <linux/sched/clock.h>
#include <linux/hashtable.h>

#include "nvmev.h"
#include "dftl.h"

void schedule_internal_operation(int sqid, unsigned long long nsecs_target,
				 struct buffer *write_buffer, unsigned int buffs_to_release);
static struct cmt_entry *cmt_check(struct dftl *dftl, int vpn);
static struct cmt_entry *cmt_update(struct dftl *dftl, uint64_t lpn, struct ppa ppa);
static void cmt_mark_head_dirty(struct dftl *dftl);
static uint64_t dftl_addr_translation(struct dftl *dftl, uint64_t nsecs_start, uint32_t sqid,
									struct ppa *ppa, uint64_t lpn, uint32_t io_type, int *nand_write, bool read);
static void cmt_print(struct dftl *dftl);
static inline struct ppa get_translation_ppa(struct gtd* gtd, int vpn);

static inline bool last_pg_in_wordline(struct dftl *dftl, struct ppa *ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	return (ppa->g.pg % spp->pgs_per_oneshotpg) == (spp->pgs_per_oneshotpg - 1);
}

static bool should_gc(struct dftl *dftl)
{
	return (dftl->lm.free_line_cnt <= dftl->dp.gc_thres_lines);
}

static inline bool should_gc_high(struct dftl *dftl)
{
	return dftl->lm.free_line_cnt <= dftl->dp.gc_thres_lines_high;
}

static uint64_t ppa2pgidx(struct dftl *dftl, struct ppa *ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	uint64_t pgidx;

	NVMEV_DEBUG_VERBOSE("%s: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", __func__,
			ppa->g.ch, ppa->g.lun, ppa->g.pl, ppa->g.blk, ppa->g.pg);

	pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
		ppa->g.pl * spp->pgs_per_pl + ppa->g.blk * spp->pgs_per_blk + ppa->g.pg;

	NVMEV_ASSERT(pgidx < spp->tt_pgs);

	return pgidx;
}

static inline uint64_t get_rmap_ent(struct dftl *dftl, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(dftl, ppa);

	return dftl->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct dftl *dftl, uint64_t lpn, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(dftl, ppa);

	dftl->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
	return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
	return ((struct dftl_line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
	((struct dftl_line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
	return ((struct dftl_line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
	((struct dftl_line *)a)->pos = pos;
}

static inline void consume_write_credit(struct dftl *dftl)
{
	dftl->wfc.write_credits--;
}

static void foreground_gc(struct dftl *dftl);

// static inline void check_and_refill_write_credit(struct dftl *dftl)
// {
// 	struct dftl_write_flow_control *wfc = &(dftl->wfc);
// 	if (wfc->write_credits <= 0) {
// 		foreground_gc(dftl);

// 		wfc->write_credits += wfc->credits_to_refill;
// 	}
// }

static void init_lines(struct dftl *dftl)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct dftl_line_mgmt *lm = &dftl->lm;
	struct dftl_line *line;
	int i;

	lm->tt_lines = spp->blks_per_pl;
	NVMEV_ASSERT(lm->tt_lines == spp->tt_lines);
	lm->lines = vmalloc(sizeof(struct dftl_line) * lm->tt_lines);

	INIT_LIST_HEAD(&lm->free_line_list);
	INIT_LIST_HEAD(&lm->full_line_list);

	lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri, victim_line_get_pri,
					 victim_line_set_pri, victim_line_get_pos,
					 victim_line_set_pos);

	lm->free_line_cnt = 0;
	for (i = 0; i < lm->tt_lines; i++) {
		lm->lines[i] = (struct dftl_line){
			.id = i,
			.ipc = 0,
			.vpc = 0,
			.pos = 0,
			.translation = false,
			.entry = LIST_HEAD_INIT(lm->lines[i].entry),
		};

		/* initialize all the lines as free lines */
		list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
		lm->free_line_cnt++;
	}

	NVMEV_ASSERT(lm->free_line_cnt == lm->tt_lines);
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
	lm->translation_line_cnt = 0;
}

static void init_gtd(struct dftl *dftl)
{
	struct ssd *ssd = dftl->ssd;
	struct ssdparams *spp = &ssd->sp;
	struct gtd *gtd = &dftl->gtd;
	int i;

	gtd->map_per_pg = spp->pgsz / sizeof (struct ppa);
	gtd->tt_tpgs = spp->tt_pgs / gtd->map_per_pg;
	gtd->tbl = vmalloc(sizeof(struct ppa) * gtd->tt_tpgs);

	for (i = 0; i < gtd->tt_tpgs; i++)
	{
		gtd->tbl[i].ppa = UNMAPPED_PPA;
	}
}

static void remove_gtd(struct dftl *dftl)
{
	vfree(dftl->gtd.tbl);
}

static void init_cmt(struct dftl *dftl)
{
	struct cmt *cmt = &dftl->cmt;

	// CMT SIZE
	cmt->tt_tpgs = 64;
	cmt->entry_cnt = 0;

	INIT_LIST_HEAD(&cmt->lru_list);
	hash_init(cmt->lru_hash);

	cmt->hit_cnt = 0;
	cmt->miss_cnt = 0;
	cmt->cold_miss_cnt = 0;
	cmt->read_miss_cnt = 0;
	cmt->write_miss_cnt = 0;
	cmt->flush_cnt = 0;

	NVMEV_INFO("CMT size: %uKiB", BYTE_TO_KB(cmt->tt_tpgs * dftl->ssd->sp.pgsz));
}

static void remove_cmt(struct dftl *dftl)
{
	unsigned bkt;
	struct cmt_entry *cur;
	hash_for_each(dftl->cmt.lru_hash, bkt, cur, hnode) {
		vfree(cur->l2p);
	}
}

static void remove_lines(struct dftl *dftl)
{
	pqueue_free(dftl->lm.victim_line_pq);
	vfree(dftl->lm.lines);
}

// static void init_write_flow_control(struct dftl *dftl)
// {
// 	struct dftl_write_flow_control *wfc = &(dftl->wfc);
// 	struct ssdparams *spp = &dftl->ssd->sp;

// 	wfc->write_credits = spp->pgs_per_line;
// 	wfc->credits_to_refill = spp->pgs_per_line;
// }

static inline void check_addr(int a, int max)
{
	NVMEV_ASSERT(a >= 0 && a < max);
}

static struct dftl_line *get_next_free_line(struct dftl *dftl)
{
	struct dftl_line_mgmt *lm = &dftl->lm;
	struct dftl_line *curline = list_first_entry_or_null(&lm->free_line_list, struct dftl_line, entry);

	if (!curline) {
		NVMEV_ERROR("No free line left in VIRT !!!!\n");
		return NULL;
	}

	list_del_init(&curline->entry);
	lm->free_line_cnt--;

	return curline;
}

static struct dftl_write_pointer *__get_wp(struct dftl *ftl, uint32_t io_type)
{
	if (io_type == USER_IO) {
		return &ftl->wp;
	} else if (io_type == GC_IO) {
		return &ftl->gc_wp;
	}

	NVMEV_ASSERT(0);
	return NULL;
}

static void prepare_write_pointer(struct dftl *dftl, uint32_t io_type)
{
	struct dftl_write_pointer *wp = __get_wp(dftl, io_type);
	struct dftl_line *curline = get_next_free_line(dftl);

	NVMEV_ASSERT(wp);
	NVMEV_ASSERT(curline);

	/* wp->curline is always our next-to-write super-block */
	*wp = (struct dftl_write_pointer){
		.curline = curline,
		.ch = 0,
		.lun = 0,
		.pg = 0,
		.blk = curline->id,
		.pl = 0,
	};
}

static void advance_write_pointer(struct dftl *dftl, uint32_t io_type)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct dftl_line_mgmt *lm = &dftl->lm;
	struct dftl_write_pointer *wpp = __get_wp(dftl, io_type);

	NVMEV_DEBUG_VERBOSE("current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg);

	check_addr(wpp->pg, spp->pgs_per_blk);
	wpp->pg++;
	if ((wpp->pg % spp->pgs_per_oneshotpg) != 0)
		goto out;

	wpp->pg -= spp->pgs_per_oneshotpg;
	check_addr(wpp->ch, spp->nchs);
	wpp->ch++;
	if (wpp->ch != spp->nchs)
		goto out;

	wpp->ch = 0;
	check_addr(wpp->lun, spp->luns_per_ch);
	wpp->lun++;
	/* in this case, we should go to next lun */
	if (wpp->lun != spp->luns_per_ch)
		goto out;

	wpp->lun = 0;
	/* go to next wordline in the block */
	wpp->pg += spp->pgs_per_oneshotpg;
	if (wpp->pg != spp->pgs_per_blk)
		goto out;

	wpp->pg = 0;
	/* move current line to {victim,full} line list */
	if (wpp->curline->vpc == spp->pgs_per_line) {
		/* all pgs are still valid, move to full line list */
		NVMEV_ASSERT(wpp->curline->ipc == 0);
		list_add_tail(&wpp->curline->entry, &lm->full_line_list);
		lm->full_line_cnt++;
		NVMEV_DEBUG_VERBOSE("wpp: move line to full_line_list\n");
	} else {
		NVMEV_DEBUG_VERBOSE("wpp: line is moved to victim list\n");
		NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
		/* there must be some invalid pages in this line */
		NVMEV_ASSERT(wpp->curline->ipc > 0);
		pqueue_insert(lm->victim_line_pq, wpp->curline);
		lm->victim_line_cnt++;
	}
	/* current line is used up, pick another empty line */
	check_addr(wpp->blk, spp->blks_per_pl);
	wpp->curline = get_next_free_line(dftl);
	NVMEV_DEBUG_VERBOSE("wpp: got new clean line %d\n", wpp->curline->id);

	wpp->blk = wpp->curline->id;
	check_addr(wpp->blk, spp->blks_per_pl);

	/* make sure we are starting from page 0 in the super block */
	NVMEV_ASSERT(wpp->pg == 0);
	NVMEV_ASSERT(wpp->lun == 0);
	NVMEV_ASSERT(wpp->ch == 0);
	/* TODO: assume # of pl_per_lun is 1, fix later */
	NVMEV_ASSERT(wpp->pl == 0);
out:
	NVMEV_DEBUG_VERBOSE("advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);
}

static struct ppa get_new_page(struct dftl *dftl, uint32_t io_type)
{
	struct ppa ppa;
	struct dftl_write_pointer *wp = __get_wp(dftl, io_type);

	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;

	NVMEV_ASSERT(ppa.g.pl == 0);

	return ppa;
}

// static void init_maptbl(struct dftl *dftl)
// {
// 	int i;
// 	struct ssdparams *spp = &dftl->ssd->sp;

// 	dftl->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
// 	for (i = 0; i < spp->tt_pgs; i++) {
// 		dftl->maptbl[i].ppa = UNMAPPED_PPA;
// 	}
// }

// static void remove_maptbl(struct dftl *dftl)
// {
// 	vfree(dftl->maptbl);
// }

static void init_rmap(struct dftl *dftl)
{
	int i;
	struct ssdparams *spp = &dftl->ssd->sp;

	dftl->rmap = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
		dftl->rmap[i] = INVALID_LPN;
	}
}

static void remove_rmap(struct dftl *dftl)
{
	vfree(dftl->rmap);
}

static void dftl_init_ftl(struct dftl *dftl, struct dftlparams *dpp, struct ssd *ssd)
{
	/*copy dftlparams*/
	dftl->dp = *dpp;

	dftl->ssd = ssd;

	/* initialize rmap */
	init_rmap(dftl); // reverse mapping table (?)

	/* initialize all the lines */
	init_lines(dftl);

	/* initialize write pointer, this is how we allocate new pages for writes */
	prepare_write_pointer(dftl, USER_IO);
	prepare_write_pointer(dftl, GC_IO);

	init_gtd(dftl);

	init_cmt(dftl);

	dftl->gc_cnt = 0;

	NVMEV_INFO("Init FTL instance with %d channels (%ld pages)\n", dftl->ssd->sp.nchs,
		   dftl->ssd->sp.tt_pgs);

	return;
}

static void dftl_remove_ftl(struct dftl *dftl)
{
	remove_lines(dftl);
	remove_rmap(dftl);
	remove_gtd(dftl);
	remove_cmt(dftl);
}

static void dftl_init_params(struct dftlparams *dpp)
{
	dpp->op_area_pcent = OP_AREA_PERCENT;
	dpp->gc_thres_lines = 8; /* Need only two lines.(host write, gc)*/
	dpp->gc_thres_lines_high = 8; /* Need only two lines.(host write, gc)*/
	dpp->enable_gc_delay = 1;
	dpp->pba_pcent = (int)((1 + dpp->op_area_pcent) * 100);
}

void dftl_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher)
{
	struct ssdparams spp;
	struct dftlparams dpp;
	struct dftl *dftls;
	struct ssd *ssd;
	uint32_t i;
	const uint32_t nr_parts = SSD_PARTITIONS;

	ssd_init_params(&spp, size, nr_parts);
	dftl_init_params(&dpp);

	dftls = kmalloc(sizeof(struct dftl) * nr_parts, GFP_KERNEL);

	for (i = 0; i < nr_parts; i++) {
		ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL);
		ssd_init(ssd, &spp, cpu_nr_dispatcher);
		dftl_init_ftl(&dftls[i], &dpp, ssd);
	}

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		kfree(dftls[i].ssd->pcie->perf_model);
		kfree(dftls[i].ssd->pcie);
		kfree(dftls[i].ssd->write_buffer);

		dftls[i].ssd->pcie = dftls[0].ssd->pcie;
		dftls[i].ssd->write_buffer = dftls[0].ssd->write_buffer;
	}

	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->nr_parts = nr_parts;
	ns->ftls = (void *)dftls;
	ns->size = (uint64_t)((size * 100) / dpp.pba_pcent);
	ns->mapped = mapped_addr;
	/*register io command handler*/
	ns->proc_io_cmd = dftl_proc_nvme_io_cmd;

	NVMEV_INFO("FTL physical space: %lld, logical space: %lld (physical/logical * 100 = %d)\n",
		   size, ns->size, dpp.pba_pcent);

	return;
}

void dftl_remove_namespace(struct nvmev_ns *ns)
{
	struct dftl *dftls = (struct dftl *)ns->ftls;
	const uint32_t nr_parts = SSD_PARTITIONS;
	uint32_t i;

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		/*
		 * These were freed from dftl_init_namespace() already.
		 * Mark these NULL so that ssd_remove() skips it.
		 */
		dftls[i].ssd->pcie = NULL;
		dftls[i].ssd->write_buffer = NULL;
	}

	for (i = 0; i < nr_parts; i++) {
		dftl_remove_ftl(&dftls[i]);
		ssd_remove(dftls[i].ssd);
		kfree(dftls[i].ssd);
	}

	kfree(dftls);
	ns->ftls = NULL;
}

static inline bool valid_ppa(struct dftl *dftl, struct ppa *ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	int ch = ppa->g.ch;
	int lun = ppa->g.lun;
	int pl = ppa->g.pl;
	int blk = ppa->g.blk;
	int pg = ppa->g.pg;
	//int sec = ppa->g.sec;

	if (ch < 0 || ch >= spp->nchs)
		return false;
	if (lun < 0 || lun >= spp->luns_per_ch)
		return false;
	if (pl < 0 || pl >= spp->pls_per_lun)
		return false;
	if (blk < 0 || blk >= spp->blks_per_pl)
		return false;
	if (pg < 0 || pg >= spp->pgs_per_blk)
		return false;

	return true;
}

static inline bool valid_lpn(struct dftl *dftl, uint64_t lpn)
{
	return (lpn < dftl->ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
	return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct dftl_line *get_line(struct dftl *dftl, struct ppa *ppa)
{
	return &(dftl->lm.lines[ppa->g.blk]);
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct dftl *dftl, struct ppa *ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct dftl_line_mgmt *lm = &dftl->lm;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	bool was_full_line = false;
	struct dftl_line *line;

	/* update corresponding page status */
	pg = get_pg(dftl->ssd, ppa);
	NVMEV_ASSERT(pg->status == PG_VALID);
	pg->status = PG_INVALID;

	/* update corresponding block status */
	blk = get_blk(dftl->ssd, ppa);
	NVMEV_ASSERT(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
	blk->ipc++;
	NVMEV_ASSERT(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
	blk->vpc--;

	/* update corresponding line status */
	line = get_line(dftl, ppa);
	NVMEV_ASSERT(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
	if (line->vpc == spp->pgs_per_line) {
		NVMEV_ASSERT(line->ipc == 0);
		was_full_line = true;
	}
	line->ipc++;
	NVMEV_ASSERT(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
	/* Adjust the position of the victime line in the pq under over-writes */
	if (line->pos) {
		/* Note that line->vpc will be updated by this call */
		pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
	} else {
		line->vpc--;
	}

	if (was_full_line) {
		/* move line: "full" -> "victim" */
		list_del_init(&line->entry);
		lm->full_line_cnt--;
		pqueue_insert(lm->victim_line_pq, line);
		lm->victim_line_cnt++;
	}
}

static void mark_page_valid(struct dftl *dftl, struct ppa *ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	struct dftl_line *line;

	/* update page status */
	pg = get_pg(dftl->ssd, ppa);
	NVMEV_ASSERT(pg->status == PG_FREE);
	pg->status = PG_VALID;

	/* update corresponding block status */
	blk = get_blk(dftl->ssd, ppa);
	NVMEV_ASSERT(blk->vpc >= 0 && blk->vpc < spp->pgs_per_blk);
	blk->vpc++;

	/* update corresponding line status */
	line = get_line(dftl, ppa);
	NVMEV_ASSERT(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
	line->vpc++;
}

static void mark_block_free(struct dftl *dftl, struct ppa *ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct nand_block *blk = get_blk(dftl->ssd, ppa);
	struct nand_page *pg = NULL;
	int i;

	for (i = 0; i < spp->pgs_per_blk; i++) {
		/* reset page status */
		pg = &blk->pg[i];
		NVMEV_ASSERT(pg->nsecs == spp->secs_per_pg);
		pg->status = PG_FREE;
	}

	/* reset block status */
	NVMEV_ASSERT(blk->npgs == spp->pgs_per_blk);
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt++;
}

static void gc_read_page(struct dftl *dftl, struct ppa *ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct dftlparams *dpp = &dftl->dp;
	/* advance dftl status, we don't care about how long it takes */
	if (dpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz,
			.interleave_pci_dma = false,
			.ppa = ppa,
		};
		ssd_advance_nand(dftl->ssd, &gcr);
	}
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_data_page(struct dftl *dftl, struct ppa *old_ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct dftlparams *dpp = &dftl->dp;
	struct gtd *gtd = &dftl->gtd;

	struct cmt *cmt = &dftl->cmt;
	struct cmt_entry *cmt_entry;
	struct cmt_entry *victim_entry;

	struct nand_page *old_pg, *new_pg, *old_tr_pg, *new_tr_pg;
	struct ppa new_ppa, old_tr_ppa, new_tr_ppa;

	uint64_t lpn = get_rmap_ent(dftl, old_ppa);
	int vpn = lpn / gtd->map_per_pg;
	int offset = lpn % gtd->map_per_pg;
	uint64_t nsecs_translation_completed;
	uint64_t nsecs_read = 0;
	int nand_write = 0;
	int i;

	struct nand_cmd gcr = {
		.type = GC_IO,
		.cmd = NAND_READ,
		.stime = 0,
		.interleave_pci_dma = true,
		.xfer_size = spp->pgsz,
	};

	NVMEV_ASSERT(valid_lpn(dftl, lpn));
	
	new_ppa = get_new_page(dftl, GC_IO);
	advance_write_pointer(dftl, GC_IO);

	old_tr_ppa = gtd->tbl[vpn];
	old_tr_pg = get_pg(dftl->ssd, &old_tr_ppa);

	new_tr_ppa = get_new_page(dftl, GC_IO);
	advance_write_pointer(dftl, GC_IO);

	new_tr_pg = get_pg(dftl->ssd, &new_tr_ppa);

	if ((cmt_entry = cmt_check(dftl, vpn))) {
		cmt->hit_cnt++;
		cmt_entry->l2p[offset] = new_ppa;

		new_tr_pg->l2p = vmalloc(sizeof(struct ppa) * gtd->map_per_pg);
		memcpy(new_tr_pg->l2p, cmt_entry->l2p, sizeof(struct ppa) * gtd->map_per_pg);

		cmt_entry->dirty = false;
		vfree(old_tr_pg->l2p);
	}
	else {
		cmt->miss_cnt++;
		old_tr_pg->l2p[offset] = new_ppa;
				
		new_tr_pg->l2p = vmalloc(sizeof(struct ppa) * gtd->map_per_pg);
		memcpy(new_tr_pg->l2p, old_tr_pg->l2p, sizeof(struct ppa) * gtd->map_per_pg);

		gcr.ppa = &old_tr_ppa;
		nsecs_read = ssd_advance_nand(dftl->ssd, &gcr);

		vfree(old_tr_pg->l2p);
	}
	old_tr_pg->l2p = NULL;

	new_tr_pg->translation = true;
	old_tr_pg->translation = false;

	set_rmap_ent(dftl, INVALID_LPN, &old_tr_ppa);
	set_rmap_ent(dftl, vpn, &new_tr_ppa);

	gtd->tbl[vpn] = new_tr_ppa;

	mark_page_valid(dftl, &new_tr_ppa);
	mark_page_invalid(dftl, &old_tr_ppa);

	/* update rmap */
	set_rmap_ent(dftl, lpn, &new_ppa);

	mark_page_valid(dftl, &new_ppa);

	/* need to advance the write pointer here */

	if (dpp->enable_gc_delay) {
		/* write for translation page & data page */
		struct nand_cmd gcw = {
			.type = GC_IO,
			.cmd = NAND_NOP,
			.stime = nsecs_read,
			.interleave_pci_dma = false,
			.ppa = &new_tr_ppa,
		};

		if (last_pg_in_wordline(dftl, &new_ppa)) {
			gcw.cmd = NAND_WRITE;
			gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
		}

		ssd_advance_nand(dftl->ssd, &gcw);

		gcw.cmd = NAND_NOP;
		gcw.ppa = &new_ppa;

		if (last_pg_in_wordline(dftl, &new_ppa)) {
			gcw.cmd = NAND_WRITE;
			gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
		}

		ssd_advance_nand(dftl->ssd, &gcw);
	}

	/* advance per-ch gc_endtime as well */
#if 0
	new_ch = get_ch(dftl, &new_ppa);
	new_ch->gc_endtime = new_ch->next_ch_avail_time;

	new_lun = get_lun(dftl, &new_ppa);
	new_lun->gc_endtime = new_lun->next_lun_avail_time;
#endif

	return 0;
}

static uint64_t gc_write_translation_page(struct dftl *dftl, struct ppa *old_tr_ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct dftlparams *dpp = &dftl->dp;
	struct cmt *cmt = &dftl->cmt;
	struct ppa new_tr_ppa;
	struct cmt_entry *cmt_entry;

	struct nand_page *old_tr_pg, *new_tr_pg;
	struct gtd *gtd = &dftl->gtd;
	uint64_t nsecs_read = 0;

	int vpn = get_rmap_ent(dftl, old_tr_ppa);

	old_tr_pg = get_pg(dftl->ssd, old_tr_ppa);
	NVMEV_ASSERT(old_tr_pg->translation == true);
	
	new_tr_ppa = get_new_page(dftl, GC_IO);
	new_tr_pg = get_pg(dftl->ssd, &new_tr_ppa);

	NVMEV_ASSERT(new_tr_pg->translation == false);

	if ((cmt_entry = cmt_check(dftl, vpn))) {
		/* if l2p in victim translation page is cached in cmt,
		 copy the latest l2p to new translation page */
		 cmt->hit_cnt++;

		new_tr_pg->l2p = vmalloc(sizeof(struct ppa) * gtd->map_per_pg);
		memcpy(new_tr_pg->l2p, cmt_entry->l2p, sizeof(struct ppa) * gtd->map_per_pg);
		cmt_entry->dirty = false;

		vfree(old_tr_pg->l2p);
	}
	else {
		/* if l2p in victim translation page is not cached in cmt,
		 then need to copy it */
		
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.interleave_pci_dma = true,
			.ppa = old_tr_ppa,
			.xfer_size = spp->pgsz,
		};
		cmt->miss_cnt++;
		new_tr_pg->l2p = old_tr_pg->l2p;
		nsecs_read = ssd_advance_nand(dftl->ssd, &gcr);
	}

	old_tr_pg->l2p = NULL;
	new_tr_pg->translation = true;
	old_tr_pg->translation = false;

	gtd->tbl[vpn] = new_tr_ppa;

	set_rmap_ent(dftl, vpn, &new_tr_ppa);

	mark_page_valid(dftl, &new_tr_ppa);

	advance_write_pointer(dftl, GC_IO);

	if (dpp->enable_gc_delay) {
		struct nand_cmd gcw = {
			.type = GC_IO,
			.cmd = NAND_NOP,
			.stime = nsecs_read,
			.interleave_pci_dma = false,
			.ppa = &new_tr_ppa,
		};
		if (last_pg_in_wordline(dftl, &new_tr_ppa)) {
			gcw.cmd = NAND_WRITE,
			gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
		}

		ssd_advance_nand(dftl->ssd, &gcw);
	}

	/* advance per-ch gc_endtime as well */
#if 0
	new_ch = get_ch(dftl, &new_ppa);
	new_ch->gc_endtime = new_ch->next_ch_avail_time;

	new_lun = get_lun(dftl, &new_ppa);
	new_lun->gc_endtime = new_lun->next_lun_avail_time;
#endif
	return 0;
}

static struct dftl_line *select_victim_line(struct dftl *dftl, bool force)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct dftl_line_mgmt *lm = &dftl->lm;
	struct dftl_line *victim_line = NULL;

	victim_line = pqueue_peek(lm->victim_line_pq);
	if (!victim_line) {
		return NULL;
	}

	if (!force && (victim_line->vpc > (spp->pgs_per_line / 8))) {
		return NULL;
	}

	pqueue_pop(lm->victim_line_pq);
	victim_line->pos = 0;
	lm->victim_line_cnt--;

	/* victim_line is a danggling node now */
	return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_flashpg(struct dftl *dftl, struct ppa *ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct dftlparams *dpp = &dftl->dp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0, i = 0;
	uint64_t completed_time = 0;
	struct ppa ppa_copy = *ppa;

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(dftl->ssd, &ppa_copy);
		/* there shouldn't be any free page in victim blocks */
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID)
			cnt++;

		ppa_copy.g.pg++;
	}

	ppa_copy = *ppa;

	if (cnt <= 0)
		return;

	if (dpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz * cnt,
			.interleave_pci_dma = false,
			.ppa = &ppa_copy,
		};
		completed_time = ssd_advance_nand(dftl->ssd, &gcr);
	}

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(dftl->ssd, &ppa_copy);

		/* there shouldn't be any free page in victim blocks */
		if (pg_iter->status == PG_VALID) {
			/* delay the maptbl update until "write" happens */
			if (pg_iter->translation)
				gc_write_translation_page(dftl, &ppa_copy);
			else
				gc_write_data_page(dftl, &ppa_copy);
		}

		ppa_copy.g.pg++;
	}
}

static void mark_line_free(struct dftl *dftl, struct ppa *ppa)
{
	struct dftl_line_mgmt *lm = &dftl->lm;
	struct dftl_line *line = get_line(dftl, ppa);
	line->ipc = 0;
	line->vpc = 0;
	/* move this line to free line list */
	list_add_tail(&line->entry, &lm->free_line_list);
	lm->free_line_cnt++;
}

static int do_gc(struct dftl *dftl, bool force)
{
	struct dftl_line *victim_line = NULL;
	struct ssdparams *spp = &dftl->ssd->sp;
	struct ppa ppa;
	int flashpg;

	victim_line = select_victim_line(dftl, force);
	if (!victim_line) {
		return -1;
	}
	dftl->gc_cnt++;

	ppa.g.blk = victim_line->id;
	NVMEV_DEBUG_VERBOSE("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,
		    victim_line->ipc, victim_line->vpc, dftl->lm.victim_line_cnt,
		    dftl->lm.full_line_cnt, dftl->lm.free_line_cnt);

	/* copy back valid data */
	for (flashpg = 0; flashpg < spp->flashpgs_per_blk; flashpg++) {
		int ch, lun;

		ppa.g.pg = flashpg * spp->pgs_per_flashpg;
		for (ch = 0; ch < spp->nchs; ch++) {
			for (lun = 0; lun < spp->luns_per_ch; lun++) {
				struct nand_lun *lunp;

				ppa.g.ch = ch;
				ppa.g.lun = lun;
				ppa.g.pl = 0;
				lunp = get_lun(dftl->ssd, &ppa);
				clean_one_flashpg(dftl, &ppa);

				if (flashpg == (spp->flashpgs_per_blk - 1)) {
					struct dftlparams *dpp = &dftl->dp;

					mark_block_free(dftl, &ppa);

					if (dpp->enable_gc_delay) {
						struct nand_cmd gce = {
							.type = GC_IO,
							.cmd = NAND_ERASE,
							.stime = 0,
							.interleave_pci_dma = false,
							.ppa = &ppa,
						};
						ssd_advance_nand(dftl->ssd, &gce);
					}

					lunp->gc_endtime = lunp->next_lun_avail_time;
				}
			}
		}
	}

	/* update line status */
	mark_line_free(dftl, &ppa);

	return 0;
}

static void foreground_gc(struct dftl *dftl)
{
	if (should_gc_high(dftl)) {
		NVMEV_DEBUG_VERBOSE("should_gc_high passed");
		/* perform GC here until !should_gc(dftl) */
		do_gc(dftl, true);
	}
}

static bool is_same_flash_page(struct dftl *dftl, struct ppa ppa1, struct ppa ppa2)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	uint32_t ppa1_page = ppa1.g.pg / spp->pgs_per_flashpg;
	uint32_t ppa2_page = ppa2.g.pg / spp->pgs_per_flashpg;

	return (ppa1.h.blk_in_ssd == ppa2.h.blk_in_ssd) && (ppa1_page == ppa2_page);
}

static void cmt_print(struct dftl *dftl)
{
	struct cmt *cmt = &dftl->cmt;
	struct cmt_entry *cur;
	list_for_each_entry(cur, &cmt->lru_list, entry) {
		printk(KERN_CONT "%d(%d) ", cur->vpn, cur->dirty);
	}
	NVMEV_INFO("");
}

static struct cmt_entry *cmt_check(struct dftl *dftl, int vpn)
{
	struct cmt *cmt = &dftl->cmt;
	struct cmt_entry *cur;
	// hash 
	// hash_for_each_possible(cmt->lru_hash, cur, hnode, vpn) {
	// 	if (cur->vpn == vpn) {
	// 		NVMEV_INFO("cur->vpn: %d, vpn: %d", cur->vpn, vpn);
	// 		return cur;
	// 	}
	// }
	list_for_each_entry(cur, &cmt->lru_list, entry) {
		if (cur->vpn == vpn) {
			return cur;
		}
	}
	return NULL;
}

static struct cmt_entry *select_victim_cmt_entry(struct dftl *dftl)
{
	struct cmt *cmt = &dftl->cmt;

	/* remove an entry in the tail of the list(least recently used) */
	struct cmt_entry *victim_entry = container_of(cmt->lru_list.prev, struct cmt_entry, entry);
	list_del_init(&victim_entry->entry);
	cmt->entry_cnt--;

	return victim_entry;
}

static inline bool cmt_full(struct cmt *cmt)
{
	return (cmt->entry_cnt >= cmt->tt_tpgs);
}

static inline void cmt_insert(struct cmt *cmt, struct cmt_entry *cmt_entry)
{
	// hash_add(cmt->lru_hash, &cmt_entry->hnode, cmt_entry->vpn);

	list_add(&cmt_entry->entry, &cmt->lru_list);
	cmt->entry_cnt++;
}

static struct cmt_entry* cmt_update(struct dftl *dftl, uint64_t lpn, struct ppa ppa)
{
	struct cmt *cmt = &dftl->cmt;
	struct gtd *gtd = &dftl->gtd;
	struct cmt_entry *cmt_entry = cmt_check(dftl, lpn / gtd->map_per_pg);
	int offset = lpn % gtd->map_per_pg;

	if (cmt_entry == NULL)
		return NULL;
	
	if (cmt_entry->vpn != lpn / gtd->map_per_pg) {
		NVMEV_INFO("%d", cmt_entry->vpn);
		NVMEV_INFO("%lld", lpn);
		NVMEV_INFO("%lld", lpn / gtd->map_per_pg);
		cmt_print(dftl);
	}
	NVMEV_ASSERT(cmt_entry->vpn == lpn / gtd->map_per_pg);

	cmt_entry->l2p[offset] = ppa;
	return cmt_entry;
}

static void cmt_mark_head_dirty(struct dftl *dftl)
{
	struct cmt *cmt = &dftl->cmt;
	struct cmt_entry *cmt_entry = container_of(cmt->lru_list.next, struct cmt_entry, entry);
	cmt_entry->dirty = true;
}

static inline struct ppa get_translation_ppa(struct gtd* gtd, int vpn) 
{
	return gtd->tbl[vpn];
}

static uint64_t dftl_addr_translation(struct dftl *dftl, uint64_t nsecs_start, uint32_t sqid,
												 struct ppa *ppa, uint64_t lpn, uint32_t io_type, int* nand_write, bool read)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct buffer *wbuf = dftl->ssd->write_buffer;
	struct gtd *gtd = &dftl->gtd;
	struct cmt *cmt = &dftl->cmt;
	struct cmt_entry *cmt_entry;
	struct cmt_entry *victim_entry;

	struct ppa victim_tr_ppa, new_tr_ppa, tr_ppa, *tmp_l2p, *t_l2p;
	struct nand_page *victim_tr_pg, *new_tr_pg, *tr_pg;
	int victim_vpn;
	int i;

	uint64_t nsecs_read;
	uint64_t nsecs_wb;
	uint64_t nsecs_xfer_completed;
	uint32_t allocated_buf_size;

	uint64_t cmd_stime = (nsecs_start == 0) ? cpu_clock(dftl->ssd->cpu_nr_dispatcher) : nsecs_start;

	struct nand_cmd srd = {
		.type = io_type,
		.cmd = NAND_READ,
		.stime = cmd_stime,
		.interleave_pci_dma = true,
		.xfer_size = spp->pgsz,
	};

	struct nand_cmd swr = {
		.type = io_type,
		.cmd = NAND_WRITE,
		.interleave_pci_dma = false,
		.xfer_size = spp->pgsz,
	};

	int vpn = lpn / gtd->map_per_pg;
	int offset = lpn % gtd->map_per_pg;
	// NVMEV_INFO("ADDR TRANSLATION");

	nsecs_xfer_completed = cmd_stime;
	if (!(cmt_entry = cmt_check(dftl, vpn))) {
		/* miss in CMT */
		// NVMEV_INFO("MISS vpn: %d, offset: %d", vpn, offset);
		
		cmt->miss_cnt++;
		if (read)
			cmt->read_miss_cnt++;
		else
			cmt->write_miss_cnt++;
		tr_ppa = get_translation_ppa(gtd, vpn);

		if (!mapped_ppa(&tr_ppa)) {
			/* initial translation page */
			// tr_ppa = get_new_page(dftl, TRANSLATION_IO);
			tr_ppa = get_new_page(dftl, io_type);
			tr_pg = get_pg(dftl->ssd, &tr_ppa);

			tr_pg->l2p = vmalloc(sizeof(struct ppa) * gtd->map_per_pg);
			tr_pg->translation = true;

			for (i = 0; i < gtd->map_per_pg; i++)
				tr_pg->l2p[i].ppa = UNMAPPED_PPA;

			gtd->tbl[vpn] = tr_ppa;
			// NVMEV_INFO("HERE new tr page: %lld", tr_ppa.ppa);
			set_rmap_ent(dftl, vpn, &tr_ppa);

			mark_page_valid(dftl, &tr_ppa);

			advance_write_pointer(dftl, io_type);

			// if (io_type == USER_IO) {
			// 	consume_write_credit(dftl);
			// 	check_and_refill_write_credit(dftl);
			// }

			cmt->cold_miss_cnt++;
			*nand_write = *nand_write + 1;
		}
		else {
			// NVMEV_INFO("EXISTING TR PG: %lld", tr_ppa.ppa);
			tr_pg = get_pg(dftl->ssd, &tr_ppa);
		}
		NVMEV_ASSERT(tr_pg->translation == true);
		NVMEV_ASSERT(tr_pg->l2p != NULL);
		t_l2p = tr_pg->l2p;

		cmt_entry = vmalloc(sizeof(struct cmt_entry));
		cmt_entry->l2p = vmalloc(sizeof(struct ppa) * gtd->map_per_pg);

		if (tr_pg->l2p == NULL) {
			NVMEV_INFO("ppa: %lld, lpn: %lld, vpn:%d", tr_ppa.ppa, lpn, vpn);
			NVMEV_INFO("%p %p", t_l2p, tmp_l2p);
			NVMEV_INFO("%d", dftl->gc_cnt);
			NVMEV_INFO("%d", get_line(dftl, &tr_ppa)->id);
			cmt_print(dftl);
		}
		NVMEV_ASSERT(tr_pg->l2p != NULL);
		memcpy(cmt_entry->l2p, tr_pg->l2p, sizeof(struct ppa) * gtd->map_per_pg);

		cmt_entry->vpn = vpn;
		cmt_entry->dirty = false;

		// NVMEV_INFO("before TPAGE READ: %lld, %lld", srd.stime, srd.stime + spp->fw_4kb_rd_lat);
		/* read translation page if CMT miss */
		srd.ppa = &tr_ppa;
		// srd.stime += spp->fw_4kb_rd_lat;
		nsecs_read = ssd_advance_nand(dftl->ssd, &srd);
		nsecs_xfer_completed = nsecs_read;

		if (cmt_full(cmt)) {
			victim_entry = select_victim_cmt_entry(dftl);

			if (victim_entry->dirty) {
				victim_vpn = victim_entry->vpn;
				victim_tr_ppa = get_translation_ppa(gtd, victim_vpn);
				victim_tr_pg = get_pg(dftl->ssd, &victim_tr_ppa);

				NVMEV_ASSERT(victim_tr_pg->translation == true);

				// new_tr_ppa = get_new_page(dftl, TRANSLATION_IO);
				new_tr_ppa = get_new_page(dftl, io_type);
				new_tr_pg = get_pg(dftl->ssd, &new_tr_ppa);

				NVMEV_ASSERT(new_tr_pg->l2p == NULL);
				new_tr_pg->l2p = vmalloc(sizeof(struct ppa) * gtd->map_per_pg);
				memcpy(new_tr_pg->l2p, victim_entry->l2p, sizeof(struct ppa) * gtd->map_per_pg);

				gtd->tbl[victim_vpn] = new_tr_ppa;
				new_tr_pg->translation = true;

				mark_page_invalid(dftl, &victim_tr_ppa);
				mark_page_valid(dftl, &new_tr_ppa);

				// NVMEV_INFO("evict2 vpn: %d %lld %lld", victim_vpn, victim_tr_ppa.ppa, new_tr_ppa.ppa);
				set_rmap_ent(dftl, get_rmap_ent(dftl, &victim_tr_ppa), &new_tr_ppa);
				set_rmap_ent(dftl, INVALID_LPN, &victim_tr_ppa);

				// NVMEV_INFO("evict victim tr ppa: %lld, new tr ppa: %lld", victim_tr_ppa.ppa, new_tr_ppa.ppa);
				tmp_l2p = victim_tr_pg->l2p;
				vfree(victim_tr_pg->l2p);
				
				victim_tr_pg->l2p = NULL;
				victim_tr_pg->translation = false;

				// advance_write_pointer(dftl, TRANSLATION_IO);
				advance_write_pointer(dftl, io_type);

				// allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(spp->secs_per_pg));
				// nsecs_wb = ssd_advance_write_buffer(dftl->ssd, req->nsecs_start, LBA_TO_BYTE(spp->secs_per_pg));

				swr.stime = nsecs_read;
				swr.ppa = &new_tr_ppa;
				if (last_pg_in_wordline(dftl, &new_tr_ppa) && io_type == USER_IO) {
					schedule_internal_operation(sqid, nsecs_xfer_completed, wbuf,
												spp->pgs_per_oneshotpg * spp->pgsz);
				}
				nsecs_xfer_completed = ssd_advance_nand(dftl->ssd, &swr);
				// NVMEV_INFO("TPAGE READ: %lld, after write: %lld", nsecs_read, nsecs_xfer_completed);

				// if (io_type == USER_IO) {
				// 	consume_write_credit(dftl);
				// 	check_and_refill_write_credit(dftl);
				// }
				*nand_write = *nand_write + 1;
				cmt->flush_cnt++;
			}
			vfree(victim_entry->l2p);
			vfree(victim_entry);
		}

		// NVMEV_INFO("insert: %d", cmt_entry->vpn);
		cmt_insert(cmt, cmt_entry);
		// NVMEV_INFO("MISS: %d", vpn);
	}
	else {
		/* hit in CMT */
		/* move to front of the lru list */
		// NVMEV_INFO("HIT: %d", vpn);
		// NVMEV_INFO("HIT vpn: %d, offset: %d", vpn, offset);
		// cmt_print(dftl);

		cmt->hit_cnt++;
		list_move(&cmt_entry->entry, &cmt->lru_list);
	}
	*ppa = cmt_entry->l2p[offset];
	// NVMEV_INFO("ADDR TRANSLATION END");
	// NVMEV_INFO("%lld %lld", cmd_stime, nsecs_xfer_completed);
	
	return nsecs_xfer_completed;
}

static bool dftl_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct dftl *dftls = (struct dftl *)ns->ftls;
	struct dftl *dftl = &dftls[0];
	/* spp are shared by all instances*/
	struct ssdparams *spp = &dftl->ssd->sp;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
	uint64_t lpn;
	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed, nsecs_latest = nsecs_start;
	uint64_t nsecs_translation_completed;
	uint32_t xfer_size, i;
	uint32_t nr_parts = ns->nr_parts;

	struct ppa prev_ppa;
	struct nand_cmd srd = {
		.type = USER_IO,
		.cmd = NAND_READ,
		.stime = nsecs_start,
		.interleave_pci_dma = true,
	};

	int nand_write;

	NVMEV_ASSERT(dftls);
	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn, nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n", __func__,
			    start_lpn, spp->tt_pgs);
		return false;
	}

	if (LBA_TO_BYTE(nr_lba) <= (KB(4) * nr_parts)) {
		srd.stime += spp->fw_4kb_rd_lat;
	} else {
		srd.stime += spp->fw_rd_lat;
	}
	nsecs_start = srd.stime;

	for (i = 0; (i < nr_parts) && (start_lpn <= end_lpn); i++, start_lpn++) {
		dftl = &dftls[start_lpn % nr_parts];
		xfer_size = 0;
		nand_write = 0;
		nsecs_translation_completed = dftl_addr_translation(dftl, nsecs_start, req->sq_id, 
															&prev_ppa, start_lpn / nr_parts, USER_IO, &nand_write, true);

		/* normal IO read path */
		for (lpn = start_lpn; lpn <= end_lpn; lpn += nr_parts) {
			uint64_t local_lpn;
			struct ppa cur_ppa;

			local_lpn = lpn / nr_parts;
			nsecs_translation_completed = dftl_addr_translation(dftl, nsecs_start, req->sq_id,
																&cur_ppa, local_lpn, USER_IO, &nand_write, true);
			
			if (!mapped_ppa(&cur_ppa) || !valid_ppa(dftl, &cur_ppa)) {
				NVMEV_DEBUG_VERBOSE("lpn 0x%llx not mapped to valid ppa\n", local_lpn);
				NVMEV_DEBUG_VERBOSE("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
					    cur_ppa.g.ch, cur_ppa.g.lun, cur_ppa.g.blk,
					    cur_ppa.g.pl, cur_ppa.g.pg);
				continue;
			}

			// aggregate read io in same flash page
			if (mapped_ppa(&prev_ppa) &&
			    is_same_flash_page(dftl, cur_ppa, prev_ppa)) {
				xfer_size += spp->pgsz;
				continue;
			}

			if (xfer_size > 0) {
				srd.stime = max(nsecs_translation_completed, nsecs_latest);
				srd.xfer_size = xfer_size;
				srd.ppa = &prev_ppa;
				nsecs_completed = ssd_advance_nand(dftl->ssd, &srd);
				nsecs_latest = max(nsecs_completed, nsecs_latest);

				req->nsecs_start = nsecs_latest;
			}

			xfer_size = spp->pgsz;
			prev_ppa = cur_ppa;
		}

		// issue remaining io
		if (xfer_size > 0) {
			srd.stime = max(nsecs_translation_completed, nsecs_latest);
			srd.xfer_size = xfer_size;
			srd.ppa = &prev_ppa;
			nsecs_completed = ssd_advance_nand(dftl->ssd, &srd);
			nsecs_latest = max(nsecs_completed, nsecs_latest);

			req->nsecs_start = nsecs_latest;
		}
	}

	ret->nsecs_target = nsecs_latest;
	ret->status = NVME_SC_SUCCESS;
	return true;
}

static bool dftl_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct dftl *dftls = (struct dftl *)ns->ftls;
	struct dftl *dftl = &dftls[0];

	/* wbuf and spp are shared by all instances */
	struct ssdparams *spp = &dftl->ssd->sp;
	struct buffer *wbuf = dftl->ssd->write_buffer;
	struct gtd *gtd = &dftl->gtd;
	struct cmt_entry *cmt_entry;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;

	uint64_t lpn;
	uint32_t nr_parts = ns->nr_parts;

	uint64_t nsecs_latest, nsecs_start;
	uint64_t nsecs_xfer_completed;
	uint64_t nsecs_translation_completed;
	uint32_t allocated_buf_size;

	int nand_write;

	struct nand_cmd swr = {
		.type = USER_IO,
		.cmd = NAND_WRITE,
		.interleave_pci_dma = false,
		.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg,
	};

	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn, nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n",
				__func__, start_lpn, spp->tt_pgs);
		return false;
	}

	allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba));
	if (allocated_buf_size < LBA_TO_BYTE(nr_lba)) {
		return false;
	}

	nsecs_latest =
		ssd_advance_write_buffer(dftl->ssd, req->nsecs_start, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;
	req->nsecs_start = nsecs_latest;
	nsecs_start = nsecs_latest;

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		uint64_t local_lpn;
		uint64_t nsecs_completed = 0;
		struct ppa ppa, new_data_ppa;

		dftl = &dftls[lpn % nr_parts];
		local_lpn = lpn / nr_parts;

		nand_write = 0;		
		nsecs_translation_completed = dftl_addr_translation(dftl, nsecs_start, req->sq_id,
															&ppa, local_lpn, USER_IO, &nand_write, false);

		// ppa = get_maptbl_ent(dftl, local_lpn); // Check whether the given LPN has been written before
		if (mapped_ppa(&ppa)) {
			/* update old page information first */
			mark_page_invalid(dftl, &ppa);
			set_rmap_ent(dftl, INVALID_LPN, &ppa);
			NVMEV_DEBUG("%s: %lld is invalid, ", __func__, ppa2pgidx(dftl, &ppa));
		}

		/* new write */
		new_data_ppa = get_new_page(dftl, USER_IO);

		if (cmt_update(dftl, local_lpn, new_data_ppa) != NULL)
			cmt_mark_head_dirty(dftl);

		NVMEV_DEBUG("%s: got new ppa %lld, ", __func__, ppa2pgidx(dftl, &new_data_ppa));
		/* update rmap */
		set_rmap_ent(dftl, local_lpn, &new_data_ppa);

		mark_page_valid(dftl, &new_data_ppa);

		/* need to advance the write pointer here */
		advance_write_pointer(dftl, USER_IO);

		/* Aggregate write io in flash page */
		if (last_pg_in_wordline(dftl, &new_data_ppa)) {
			// NVMEV_INFO("latest: %lld, translation fin: %lld", nsecs_latest, nsecs_translation_completed);
			// swr.stime = nsecs_latest;
			swr.stime = max(nsecs_latest, nsecs_translation_completed);
			swr.ppa = &new_data_ppa;

			nsecs_completed = ssd_advance_nand(dftl->ssd, &swr);
			nsecs_latest = max(nsecs_completed, nsecs_latest);

			schedule_internal_operation(req->sq_id, nsecs_completed, wbuf,
						    spp->pgs_per_oneshotpg * spp->pgsz);

			req->nsecs_start = nsecs_latest;
		}
	}

	if ((cmd->rw.control & NVME_RW_FUA) || (spp->write_early_completion == 0)) {
		/* Wait all flash operations */
		ret->nsecs_target = nsecs_latest;
	} else {
		/* Early completion */
		ret->nsecs_target = nsecs_xfer_completed;
	}
	ret->status = NVME_SC_SUCCESS;

	return true;
}

static void dftl_flush(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	uint64_t start, latest;
	uint32_t i;
	struct dftl *dftls = (struct dftl *)ns->ftls;

	start = local_clock();
	latest = start;
	for (i = 0; i < ns->nr_parts; i++) {
		latest = max(latest, ssd_next_idle_time(dftls[i].ssd));
	}

	NVMEV_DEBUG_VERBOSE("%s: latency=%llu\n", __func__, latest - start);

	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = latest;
	return;
}

static void conv_print_cmt(struct nvmev_ns *ns, struct nvmev_request *req)
{
	struct dftl *dftls = (struct dftl *)ns->ftls;
	struct dftl *dftl = &dftls[0];
	struct cmt *cmt = &dftl->cmt;

	NVMEV_INFO("----------------- CMT -----------------");
	NVMEV_INFO("CMT hit: %lld, CMT miss: %lld", cmt->hit_cnt, cmt->miss_cnt);
	NVMEV_INFO("CMT read miss: %lld, write miss: %lld", cmt->read_miss_cnt, cmt->write_miss_cnt);
	NVMEV_INFO("GC: %d", dftl->gc_cnt);
	NVMEV_INFO("flush: %lld, cold miss: %lld", cmt->flush_cnt, cmt->cold_miss_cnt);
	cmt_print(dftl);
}

bool dftl_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct nvme_command *cmd = req->cmd;
	struct dftl *dftls = (struct dftl *)ns->ftls;
	struct dftl *dftl = &dftls[0];

	NVMEV_ASSERT(ns->csi == NVME_CSI_NVM);

	switch (cmd->common.opcode) {
	case nvme_cmd_write:
		if (!dftl_write(ns, req, ret))
			return false;
		break;
	case nvme_cmd_read:
		if (!dftl_read(ns, req, ret))
			return false;
		break;
	case nvme_cmd_flush:
		dftl_flush(ns, req, ret);
		break;
	case nvme_cmd_print_cmt:
		conv_print_cmt(ns, req);
		break;
	default:
		NVMEV_ERROR("%s: command not implemented: %s (0x%x)\n", __func__,
				nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		break;
	}

	while(should_gc_high(dftl)) {
		do_gc(dftl, true);
	}

	return true;
}