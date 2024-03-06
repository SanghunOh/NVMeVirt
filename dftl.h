// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_DFTL_H
#define _NVMEVIRT_DFTL_H

#include <linux/types.h>
#include "pqueue/pqueue.h"
#include "ssd_config.h"
#include "ssd.h"
// #include "conv_ftl.h"

struct dftlparams {
	uint32_t gc_thres_lines;
	uint32_t gc_thres_lines_high;
	bool enable_gc_delay;

	double op_area_pcent;
	int pba_pcent; /* (physical space / logical space) * 100 */
};


struct dftl_line {
	int id; /* line id, the same as corresponding block id */
	int ipc; /* invalid page count in this line */
	int vpc; /* valid page count in this line */
	struct list_head entry;
	/* position in the priority queue for victim lines */
	size_t pos;

	bool translation;
};

/* wp: record next write addr */
struct dftl_write_pointer {
	struct dftl_line *curline;
	uint32_t ch;
	uint32_t lun;
	uint32_t pg;
	uint32_t blk;
	uint32_t pl;
};

struct dftl_line_mgmt {
	struct dftl_line *lines;

	/* free line list, we only need to maintain a list of blk numbers */
	struct list_head free_line_list;
	pqueue_t *victim_line_pq;
	struct list_head full_line_list;

	uint32_t tt_lines;
	uint32_t free_line_cnt;
	uint32_t victim_line_cnt;
	uint32_t full_line_cnt;

	int translation_line_cnt;
};

struct dftl_write_flow_control {
	uint32_t write_credits;
	uint32_t credits_to_refill;
};

struct gtd {
	int map_per_pg;
	int tt_tpgs;
	struct ppa *tbl;
};

struct cmt_entry {
	int vpn;
	struct ppa *l2p;
	bool dirty;

	struct hlist_node hnode;
	struct list_head entry;
};

struct cmt {
	int tt_tpgs;
	int entry_cnt;

	uint64_t hit_cnt;
	uint64_t miss_cnt;
	uint64_t read_miss_cnt;
	uint64_t write_miss_cnt;
	uint64_t cold_miss_cnt;
	uint64_t flush_cnt;

	struct list_head lru_list;
	DECLARE_HASHTABLE(lru_hash, 10);
};

struct dftl {
	struct ssd *ssd;

	struct dftlparams dp;
	// struct ppa *maptbl; /* page level mapping table */
	uint64_t *rmap; /* reverse mapptbl, assume it's stored in OOB */
	struct dftl_write_pointer wp;
	struct dftl_write_pointer gc_wp;
	struct dftl_write_pointer translation_wp;
	struct dftl_write_pointer translation_gc_wp;
	struct dftl_line_mgmt lm;
	struct dftl_write_flow_control wfc;

	struct gtd gtd;
	struct cmt cmt;
	int gc_cnt;
};

void dftl_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher);

void dftl_remove_namespace(struct nvmev_ns *ns);

bool dftl_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
			   struct nvmev_result *ret);

#endif
