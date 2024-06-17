#ifndef VSMART_H
#define VSMART_H

struct vsmart
{
	__u32 percentage_used;
	__u32 data_units_read;
	__u32 data_units_written;
	__u32 gc_trigger_count_convftl;
	__u32 gc_trigger_count_dftl;
	__u32 wl_trigger_count;
	__u64 copy;
};

#define DEVICE_PE_CYCLE (8000)

void update_data_units(__u8 opcode);
void update_gc_trigger_count_convftl(void);
void update_gc_trigger_count_dftl(void);
void update_wl_trigger_count(void);
void update_copy(__u64);

__u32 get_freeblock_count_convftl(size_t nsid);
__u32 get_freeblock_count_dftl(size_t nsid);

__u32 get_percentage_used_convftl(size_t nsid);
__u32 get_percentage_used_dftl(size_t nsid);

__u32 get_data_unit_written(void);
__u32 get_data_unit_read(void);

__u32 get_total_ec_convftl(size_t nsid);
__u32 get_min_ec_convftl(size_t nsid);
__u32 get_max_ec_convftl(size_t nsid);

__u32 get_total_ec_dftl(size_t nsid);
__u32 get_min_ec_dftl(size_t nsid);
__u32 get_max_ec_dftl(size_t nsid);

__u32 get_gc_trigger_count_convftl(void);
__u32 get_gc_trigger_count_dftl(void);
__u32 get_wl_trigger_count(void);
__u32 get_copy_per_gc_convftl(void);
__u32 get_copy_per_gc_dftl(void);

#endif // VSMART_H