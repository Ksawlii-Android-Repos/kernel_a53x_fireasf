/*
 * Copyright (c) 2018 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */


#include "exynos-bcm_dbg.h"


static int exynos_bcm_dbg_run(unsigned int bcm_run,
		struct exynos_bcm_dbg_data *data);
static int exynos_bcm_dbg_early_init(struct exynos_bcm_dbg_data *data,
		unsigned int mode);
static struct exynos_bcm_dump_addr bcm_reserved;
static struct exynos_bcm_dbg_data *bcm_dbg_data;
static bool pd_sync_init = false;
static struct exynos_bcm_bw *bcm_bw;
static struct mutex bcm_bw_lock;
#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
static void *bcm_addr;
static struct bin_system_func *bin_func;
static struct os_system_func os_func;
typedef struct bin_system_func*(*start_up_func_t)(void **func);
#endif

static unsigned int num_sample;
#if defined(CONFIG_EXYNOS_ADV_TRACER) || defined(CONFIG_EXYNOS_ADV_TRACER_MODULE) \
|| defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
static enum exynos_bcm_err_code exynos_bcm_dbg_ipc_err_handle(unsigned int cmd)
{
	enum exynos_bcm_err_code err_code;

	err_code = BCM_CMD_GET(cmd, BCM_ERR_MASK, BCM_ERR_SHIFT);
	if (err_code)
		BCM_ERR("%s: BCM IPC error return(%u)\n", __func__, err_code);

	return err_code;
}
#endif

static int exynos_bcm_ip_validate(unsigned int ip_range, unsigned int ip_index,
					unsigned int bcm_ip_nr)
{
	if (ip_range >= BCM_RANGE_MAX) {
		BCM_ERR("%s: Invalid ip range(%u)\n", __func__, ip_range);
		BCM_ERR("%s: BCM_EACH(%d), BCM_ALL(%d)\n",
				__func__, BCM_EACH, BCM_ALL);
		return -EINVAL;
	}

	if (ip_index >= bcm_ip_nr) {
		BCM_ERR("%s: Invalid ip index(%u), ip_max_nr(%u)\n",
			__func__, ip_index, bcm_ip_nr - 1);
		return -EINVAL;
	}

	return 0;
}

static int exynos_bcm_is_running(unsigned int run_state)
{
	if (run_state == BCM_RUN) {
		BCM_ERR("%s: do not set when bcm is running(%u)\n",
				__func__, run_state);
		return -EBUSY;
	}

	return 0;
}

static int __exynos_bcm_dbg_ipc_send_data(enum exynos_bcm_dbg_ipc_type ipc_type,
				struct exynos_bcm_dbg_data *data,
				unsigned int *cmd)
{
	int ret = 0;
#if defined(CONFIG_EXYNOS_ADV_TRACER) || defined(CONFIG_EXYNOS_ADV_TRACER_MODULE)
	int i = 0;
	struct adv_tracer_ipc_cmd config;
#elif defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
	int i = 0;
	struct cmd_data config;
#endif
	enum exynos_bcm_err_code ipc_err = -1;
	unsigned int *bcm_cmd;

	if ((ipc_type < IPC_BCM_DBG_EVENT) ||
		(ipc_type >= IPC_BCM_DBG_MAX)) {
		BCM_ERR("%s: Invalid IPC Type: %d\n", __func__, ipc_type);
		ret = -EINVAL;
		return ret;
	}

	bcm_cmd = cmd;
#if defined(CONFIG_EXYNOS_ADV_TRACER) || defined(CONFIG_EXYNOS_ADV_TRACER_MODULE)
	config.cmd_raw.cmd = BCM_CMD_SET(ipc_type, BCM_CMD_ID_MASK, BCM_CMD_ID_SHIFT);
	memcpy(&config.buffer[1], bcm_cmd, sizeof(unsigned int) * CMD_DATA_MAX);
	ret = adv_tracer_ipc_send_data_polling(data->ipc_ch_num, &config);
#elif defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
	config.raw_cmd = BCM_CMD_SET(ipc_type, BCM_CMD_ID_MASK, BCM_CMD_ID_SHIFT);
	memcpy(config.cmd, bcm_cmd, sizeof(unsigned int) * CMD_DATA_MAX);
	ret = bin_func->send_data(&config);
#endif
	if (ret) {
		BCM_ERR("%s: Failed to send IPC(%d:%u) data to origin\n",
			__func__, ipc_type, data->ipc_ch_num);
		return ret;
	}

#if defined(CONFIG_EXYNOS_ADV_TRACER) || defined(CONFIG_EXYNOS_ADV_TRACER_MODULE)
	for (i = 0; i < data->ipc_size; i++)
		BCM_DBG("%s: received data[%d]: 0x%08x\n",
				__func__, i, config.buffer[i]);

	memcpy(bcm_cmd, &config.buffer[1], sizeof(unsigned int) * CMD_DATA_MAX);

	ipc_err = exynos_bcm_dbg_ipc_err_handle(config.cmd_raw.cmd);
#elif defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
	BCM_DBG("%s: received data raw: 0x%08x\n", __func__, config.raw_cmd);
	for (i = 0; i < CMD_DATA_MAX; i++)
		BCM_DBG("%s: received data[%d]: 0x%08x\n",
				__func__, i, config.cmd[i]);

	memcpy(bcm_cmd, config.cmd, sizeof(unsigned int) * CMD_DATA_MAX);

	ipc_err = exynos_bcm_dbg_ipc_err_handle(config.raw_cmd);
#endif
	if (ipc_err != -1) {
		ret = -EBADMSG;
		return ret;
	}

	return 0;
}

int exynos_bcm_dbg_ipc_send_data(enum exynos_bcm_dbg_ipc_type ipc_type,
				struct exynos_bcm_dbg_data *data,
				unsigned int *cmd)
{
	int ret;
	unsigned long flags;

	if (!data) {
		BCM_ERR("%s: data is not ready!\n", __func__);
		return -ENODEV;
	}

	spin_lock_irqsave(&data->lock, flags);
	ret = __exynos_bcm_dbg_ipc_send_data(ipc_type, data, cmd);
	spin_unlock_irqrestore(&data->lock, flags);

	return ret;
}
EXPORT_SYMBOL(exynos_bcm_dbg_ipc_send_data);

#if defined(CONFIG_EXYNOS_ADV_TRACER) || defined(CONFIG_EXYNOS_ADV_TRACER_MODULE)
static int adv_tracer_bcm_dbg_handler(struct adv_tracer_ipc_cmd *cmd, unsigned int len)
{
	return 0;
}

static int exynos_bcm_dbg_ipc_channel_request(struct exynos_bcm_dbg_data *data)
{
	int ret = 0;

	ret = adv_tracer_ipc_request_channel(data->ipc_node,
				(ipc_callback)adv_tracer_bcm_dbg_handler,
				&data->ipc_ch_num, &data->ipc_size);
	if (ret) {
		BCM_ERR("%s: adv tracer request channel is failed\n", __func__);
		return ret;
	}

	BCM_INFO("ipc channel info: ch_num(%u), size(%u)\n",
				data->ipc_ch_num, data->ipc_size);

	return ret;
}

static void exynos_bcm_dbg_ipc_channel_release(struct exynos_bcm_dbg_data *data)
{
	adv_tracer_ipc_release_channel(data->ipc_ch_num);
}
#else
static inline
int exynos_bcm_dbg_ipc_channel_request(struct exynos_bcm_dbg_data *data)
{
	return 0;
}

static inline
void exynos_bcm_dbg_ipc_channel_release(struct exynos_bcm_dbg_data *data)
{
}
#endif

#if defined(CONFIG_EXYNOS_PD) || defined(CONFIG_EXYNOS_PD_MODULE)
static int exynos_bcm_dbg_early_pd_sync(unsigned int cal_pdid, bool on)
{
	unsigned int cmd[4] = {0, };
	unsigned long flags;
	struct exynos_bcm_pd_info *bcm_pd_info = NULL;
	int i, ret = 0;

	spin_lock_irqsave(&bcm_dbg_data->lock, flags);

	for (i = 0; i < bcm_dbg_data->pd_size; i++) {
		if (bcm_dbg_data->pd_info[i]->cal_pdid == cal_pdid) {
			bcm_pd_info = bcm_dbg_data->pd_info[i];
			break;
		}
	}

	if (!bcm_pd_info) {
		ret = -EINVAL;
		goto out;
	}

	if (on ^ bcm_pd_info->on) {
		bcm_pd_info->on = on;
		/* Generate IPC command for PD sync */
		cmd[0] |= BCM_CMD_SET(bcm_pd_info->pd_index, BCM_PD_INFO_MASK,
					BCM_PD_INFO_SHIFT);
		cmd[0] |= BCM_CMD_SET((unsigned int)on, BCM_ONE_BIT_MASK,
					BCM_PD_ON_SHIFT);
		cmd[1] = 0;
		cmd[2] = 0;
		cmd[3] = 0;

		/* send command for PD sync */
		ret = __exynos_bcm_dbg_ipc_send_data(IPC_BCM_DBG_PD,
							bcm_dbg_data, cmd);
		if (ret) {
			BCM_ERR("%s: Failed send data for pd sync\n", __func__);
			goto out;
		}
	}

out:
	spin_unlock_irqrestore(&bcm_dbg_data->lock, flags);

	return ret;
}
#endif

int exynos_bcm_dbg_pd_sync(unsigned int cal_pdid, bool on)
{
	unsigned int cmd[4] = {0, };
	unsigned long flags;
	struct exynos_bcm_pd_info *bcm_pd_info = NULL;
	int i, ret = 0;

	if (!bcm_dbg_data || !pd_sync_init) {
		BCM_DBG("%s: do not pd_sync_init(%s)\n", __func__,
				pd_sync_init ? "true" : "false");
		return 0;
	}

	spin_lock_irqsave(&bcm_dbg_data->lock, flags);

	for (i = 0; i < bcm_dbg_data->pd_size; i++) {
		if (bcm_dbg_data->pd_info[i]->cal_pdid == cal_pdid) {
			bcm_pd_info = bcm_dbg_data->pd_info[i];
			break;
		}
	}

	if (!bcm_pd_info) {
		ret = -EINVAL;
		goto out;
	}

	if (on ^ bcm_pd_info->on) {
		bcm_pd_info->on = on;
		/* Generate IPC command for PD sync */
		cmd[0] |= BCM_CMD_SET(bcm_pd_info->pd_index, BCM_PD_INFO_MASK,
					BCM_PD_INFO_SHIFT);
		cmd[0] |= BCM_CMD_SET((unsigned int)on, BCM_ONE_BIT_MASK,
					BCM_PD_ON_SHIFT);
		cmd[1] = 0;
		cmd[2] = 0;
		cmd[3] = 0;

		/* send command for PD sync */
		ret = __exynos_bcm_dbg_ipc_send_data(IPC_BCM_DBG_PD,
							bcm_dbg_data, cmd);
		if (ret) {
			BCM_ERR("%s: Failed send data for pd sync\n", __func__);
			goto out;
		}
	}

out:
	spin_unlock_irqrestore(&bcm_dbg_data->lock, flags);

	return ret;
}
EXPORT_SYMBOL(exynos_bcm_dbg_pd_sync);

static int exynos_bcm_dbg_pd_sync_init(struct exynos_bcm_dbg_data *data)
{
	int ret = 0;
#if defined(CONFIG_EXYNOS_PD) || defined(CONFIG_EXYNOS_PD_MODULE)
	struct exynos_pm_domain *exynos_pd;
	unsigned int pd_index, pd_size;

	if (pd_sync_init) {
		BCM_ERR("%s: already pd_sync_init(%s)\n",
			__func__, pd_sync_init ? "true" : "false");
		return -EINVAL;
	}


	pd_size = data->pd_size;
	for (pd_index = 0; pd_index < pd_size; pd_index++) {
		exynos_pd = NULL;
		data->pd_info[pd_index]->on = false;
		exynos_pd = exynos_pd_lookup_name(data->pd_info[pd_index]->pd_name);
		if (exynos_pd) {
			mutex_lock(&exynos_pd->access_lock);
			exynos_pd->bcm = data->pd_info[pd_index];
			data->pd_info[pd_index]->cal_pdid = exynos_pd->cal_pdid;
			if (cal_pd_status(exynos_pd->cal_pdid)) {
				ret = exynos_bcm_dbg_pd_sync(data->pd_info[pd_index]->cal_pdid, true);
				if (ret) {
					mutex_unlock(&exynos_pd->access_lock);
					return ret;
				}
			}
			mutex_unlock(&exynos_pd->access_lock);
		} else {
			ret = exynos_bcm_dbg_early_pd_sync(data->pd_info[pd_index]->cal_pdid, true);
			if (ret)
				return ret;
		}
	}

	exynos_cal_pd_bcm_sync = exynos_bcm_dbg_pd_sync;
	pd_sync_init = true;
#endif

	return ret;
}

static int exynos_bcm_dbg_pd_sync_exit(struct exynos_bcm_dbg_data *data)
{
	int ret = 0;
#if defined(CONFIG_EXYNOS_PD) || defined(CONFIG_EXYNOS_PD_MODULE)
	struct exynos_pm_domain *exynos_pd;
	unsigned int pd_index, pd_size;

	if (!pd_sync_init) {
		BCM_ERR("%s: already pd_sync_exit(%s)\n",
			__func__, pd_sync_init ? "true" : "false");
		return -EINVAL;
	}

	pd_size = data->pd_size;
	for (pd_index = 0; pd_index < pd_size; pd_index++) {
		exynos_pd = exynos_pd_lookup_name(data->pd_info[pd_index]->pd_name);
		if (exynos_pd) {
			mutex_lock(&exynos_pd->access_lock);
			exynos_pd->bcm = NULL;
			ret = exynos_bcm_dbg_pd_sync(data->pd_info[pd_index]->cal_pdid, false);
			if (ret) {
				mutex_unlock(&exynos_pd->access_lock);
				return ret;
			}
			mutex_unlock(&exynos_pd->access_lock);
		} else {
			ret = exynos_bcm_dbg_pd_sync(data->pd_info[pd_index]->cal_pdid, false);
			if (ret)
				return ret;
		}
	}

	pd_sync_init = false;
#endif

	return ret;
}

void exynos_bcm_dbg_get_data(void *bcm)
{
	if (bcm_bw)
		bcm = bcm_bw;
	else
		bcm = NULL;
}
EXPORT_SYMBOL(exynos_bcm_dbg_get_data);

static int exynos_bcm_get_sample_cnt(struct exynos_bcm_dbg_data *data)
{
	u32 buff_size = data->dump_addr.buff_size - EXYNOS_BCM_KTIME_SIZE;
	u32 dump_entry_size = sizeof(struct exynos_bcm_dump_info);
	u32 max_index = buff_size / dump_entry_size;

	return max_index/data->bcm_ip_nr;
}

static void exynos_bcm_get_req_data(struct exynos_bcm_dbg_data *data)
{
	void __iomem *v_addr = data->dump_addr.v_addr;
	struct exynos_bcm_dump_info *dump_info = NULL;
	u32 buff_size = data->dump_addr.buff_size - EXYNOS_BCM_KTIME_SIZE;
	u32 dump_entry_size = sizeof(struct exynos_bcm_dump_info);
	u32 max_index = buff_size / dump_entry_size;

	unsigned int i, j, last_seq_no, target_seq_no, pos_idx, cal;
	static unsigned int pos;
	int dump_time = 0;
	int tmp_pos, seq_no_tmp;

	if (!bcm_bw)
		return;

	exynos_bcm_dbg_run(0, data);

	/* initialized bw data */
	for (i = 0; i < bcm_bw->num_ip; i++) {
		bcm_bw->bw_data[i].ccnt = 0;
		bcm_bw->bw_data[i].dump_time = 0;
		for (j = 0; j < 8; j++)
			bcm_bw->bw_data[i].pmcnt[j] = 0;

		bcm_bw->bw_data[i].seq_no = 0;
	}

	/* Find start index */
	dump_info = (struct exynos_bcm_dump_info *)(v_addr + EXYNOS_BCM_KTIME_SIZE);
	last_seq_no = dump_info[pos].dump_seq_no;

	/* Find last seq_no in dump data */
	while (true) {
		if (last_seq_no > dump_info[pos].dump_seq_no ||	dump_info[pos].dump_header == 0)
			break;

		last_seq_no = dump_info[pos].dump_seq_no;
		pos = (pos + 1) % max_index;
	}

	tmp_pos = pos - 1;
	seq_no_tmp = last_seq_no;

	/* get start seq_no for requested measure time */
	while (true) {
		pos_idx = BCM_CMD_GET(dump_info[tmp_pos].dump_header, BCM_IP_MASK, 0);

		if (seq_no_tmp == dump_info[tmp_pos].dump_seq_no
				&& bcm_bw->ip_idx[0] == pos_idx) {
			dump_time += dump_info[tmp_pos].dump_time;

			if (dump_time > bcm_bw->measure_time * 1000000) {
				if (dump_time - bcm_bw->measure_time * 1000000 > 500000)
					cal = 1;
				else
					cal = 0;

				target_seq_no = dump_info[tmp_pos].dump_seq_no + cal;
				break;
			}
			seq_no_tmp--;
		}
		tmp_pos--;
		if (tmp_pos < 0)
			tmp_pos = max_index - 1;

		tmp_pos = tmp_pos % max_index;
	}

	i = 0;

	while (true) {
		pos_idx = BCM_CMD_GET(dump_info[pos].dump_header, BCM_IP_MASK, 0);

		if (target_seq_no == dump_info[pos].dump_seq_no
				&& bcm_bw->ip_idx[i] == pos_idx) {
			bcm_bw->bw_data[i].ccnt += dump_info[pos].out_data.ccnt;
			bcm_bw->bw_data[i].dump_time +=	dump_info[pos].dump_time;

			for (j = 0; j < 8; j++)
				bcm_bw->bw_data[i].pmcnt[j]
					+= dump_info[pos].out_data.pmcnt[j];

			bcm_bw->bw_data[i].seq_no = dump_info[pos].dump_seq_no;

			i++;
			if (i >= bcm_bw->num_ip) {
				i = 0;
				target_seq_no++;
				if (target_seq_no > last_seq_no)
					break;
			}
		}

		pos = (pos + 1) % max_index;
	}

	exynos_bcm_dbg_run(1, data);

	return;
}

static void exynos_bcm_find_dump_data(struct exynos_bcm_dbg_data *data)
{
	void __iomem *v_addr = data->dump_addr.v_addr;
	struct exynos_bcm_dump_info *dump_info = NULL;
	struct exynos_bcm_calc *bcm_calc = data->bcm_calc;
	u32 buff_size = data->dump_addr.buff_size - EXYNOS_BCM_KTIME_SIZE;
	u32 dump_entry_size = sizeof(struct exynos_bcm_dump_info);
	u32 max_index = buff_size / dump_entry_size;

	u32 tmp_ktime[2];
	u64 last_ktime;
	unsigned int i, j, prev_seq_no, pos_idx;
	static unsigned int pos = 0;

	exynos_bcm_dbg_run(0, data);

	/* get kernel time */
	tmp_ktime[0] = __raw_readl(v_addr);
	tmp_ktime[1] = __raw_readl(v_addr + 0x4);
	last_ktime = (((u64)tmp_ktime[1] << EXYNOS_BCM_32BIT_SHIFT) &
			EXYNOS_BCM_U64_HIGH_MASK) |
		((u64)tmp_ktime[0] & EXYNOS_BCM_U64_LOW_MASK);

	BCM_DBG("%s: show last_ktime %llu, cur time %llu\n", __func__, last_ktime,
			sched_clock());

	/* Find start index */
	dump_info = (struct exynos_bcm_dump_info *)(v_addr + EXYNOS_BCM_KTIME_SIZE);
	prev_seq_no = dump_info[pos].dump_seq_no;
	i = 0;

	while (true) {
		/* This is out of latest dump */
		if (prev_seq_no > dump_info[pos].dump_seq_no || dump_info[pos].dump_header == 0)
			break;

		pos_idx = BCM_CMD_GET(dump_info[pos].dump_header, BCM_IP_MASK, 0);

		if (bcm_calc->ip_idx[i] == pos_idx) {
			bcm_calc->acc_data[i].ccnt +=
				dump_info[pos].out_data.ccnt / 1000;

			bcm_calc->acc_data[i].dump_time +=
				dump_info[pos].dump_time;

			for (j = 0; j < 8; j++)
				bcm_calc->acc_data[i].pmcnt[j]
					+= dump_info[pos].out_data.pmcnt[j] / 1000;

			i++;
			if (i >= bcm_calc->num_ip)
				i = 0;
		}
		prev_seq_no = dump_info[pos].dump_seq_no;
		pos = (pos + 1) % max_index;
	}
	exynos_bcm_dbg_run(1, data);

	return;
}

static void exynos_bcm_show_mif_work_func(struct work_struct *work)
{
	struct exynos_bcm_show_bw *bcm_show_bw = bcm_dbg_data->bcm_show_bw;
	struct exynos_bcm_calc *bcm_calc = bcm_dbg_data->bcm_calc;
	unsigned int index;
	u64 dummy[3];

	bcm_show_bw->old_time = bcm_show_bw->new_time;
	bcm_show_bw->new_time = sched_clock();
	/* msec */
	bcm_show_bw->old_time = (bcm_show_bw->new_time - bcm_show_bw->old_time) / (1<<20);

	if (bcm_show_bw->enable_ctrl) {
		mutex_lock(&bcm_show_bw->lock);

		bcm_show_bw->old_bw = bcm_show_bw->mem_bw[0];

		exynos_bcm_get_data(&bcm_show_bw->mem_bw[0], &dummy[0], &dummy[1],
				&dummy[2]);
		bcm_show_bw->new_bw = (bcm_show_bw->mem_bw[0] - bcm_show_bw->old_bw)
						* (1<<10) / bcm_show_bw->old_time;
		__exynos_bcm_trace_mem_bw(bcm_show_bw->new_bw, bcm_show_bw->old_time);

		schedule_delayed_work(&bcm_show_bw->bw_work,
				msecs_to_jiffies(bcm_calc->sample_time));

		mutex_unlock(&bcm_show_bw->lock);
	} else {
		mutex_lock(&bcm_show_bw->lock);
		if (bcm_show_bw->num_sample) {
			index =	num_sample - bcm_show_bw->num_sample;

			exynos_bcm_get_data(&bcm_show_bw->mem_bw[index], &dummy[0], &dummy[1],
					&dummy[2]);
			bcm_show_bw->dump_time[index] = bcm_show_bw->old_time;
			schedule_delayed_work(&bcm_show_bw->bw_work,
					msecs_to_jiffies(bcm_calc->sample_time));

			bcm_show_bw->num_sample--;

			mutex_unlock(&bcm_show_bw->lock);
		} else {
			cancel_delayed_work(&bcm_show_bw->bw_work);
			mutex_unlock(&bcm_show_bw->lock);
			exynos_bcm_calc_enable(0);
		}
	}
}

static void exynos_bcm_calc_work_func(struct work_struct *work)
{
	struct exynos_bcm_calc *bcm_calc = bcm_dbg_data->bcm_calc;

	mutex_lock(&bcm_calc->lock);
	if (bcm_calc->enable) {
		exynos_bcm_find_dump_data(bcm_calc->data);
		schedule_delayed_work(&bcm_calc->work,
			msecs_to_jiffies(bcm_calc->sample_time));
	}
	mutex_unlock(&bcm_calc->lock);
}

bool exynos_bcm_calc_enable(int enable)
{
	struct exynos_bcm_dbg_data *data = bcm_dbg_data;
	struct exynos_bcm_calc *bcm_calc = data->bcm_calc;
	int ret;

	mutex_lock(&bcm_calc->lock);
	if (enable) {
		bcm_calc->usage_cnt++;
	} else if (!enable && bcm_calc->usage_cnt) {
		bcm_calc->usage_cnt--;
	} else {
		mutex_unlock(&bcm_calc->lock);
		return bcm_calc->enable;
	}

	/* en_cnt == 0(disable) or first enable */
	if (bcm_calc->usage_cnt > 1 ||
			(bcm_calc->usage_cnt == 1 && !enable)) {
		mutex_unlock(&bcm_calc->lock);
		return 0;
	}
	mutex_unlock(&bcm_calc->lock);

	/* Initalize BCM Plugin */

	/* request enable */
	if (!bcm_calc->enable) {
		ret = exynos_bcm_dbg_early_init(data, PERF_MODE);
		if (ret)
			BCM_ERR("%s: failed to early bcm initialize\n", __func__);
	}

	mutex_lock(&bcm_calc->lock);
	if (enable && !bcm_calc->enable) {
		bcm_calc->enable = enable;
		exynos_bcm_dbg_run(1, data);
		/* start 50ms worker to gathering logs */
		schedule_delayed_work(&bcm_calc->work,
				msecs_to_jiffies(bcm_calc->sample_time));
	}
	else if (!enable && bcm_calc->enable){
		cancel_delayed_work(&bcm_calc->work);
		exynos_bcm_dbg_run(0, data);
		memset(bcm_calc->acc_data, 0, sizeof(struct exynos_bcm_calc_data) *
				bcm_calc->num_ip);
		bcm_calc->enable = enable;
	}
	mutex_unlock(&bcm_calc->lock);

	/* request disable */
	if (!bcm_calc->enable) {
		ret = exynos_bcm_dbg_early_init(data, DBG_MODE);
		if (ret)
			BCM_ERR("%s: failed to early bcm initialize\n", __func__);
	}

	BCM_INFO("%s: %s\n", __func__, bcm_calc->enable? "enable" : "disable");

	return bcm_calc->enable;
}
EXPORT_SYMBOL(exynos_bcm_calc_enable);

void exynos_bcm_get_data(u64 *freq_stat0, u64 *freq_stat1, u64
		*freq_stat2, u64 *freq_stat3)
{
	struct exynos_bcm_calc* bcm_calc = bcm_dbg_data->bcm_calc;

	if (bcm_calc &&	bcm_calc->sample_time) {
		mutex_lock(&bcm_calc->lock);
		if (bcm_calc->enable) {
			cancel_delayed_work(&bcm_calc->work);
			exynos_bcm_find_dump_data(bcm_dbg_data);
			__exynos_bcm_get_data(bcm_calc, freq_stat0, freq_stat1, freq_stat2, freq_stat3);
			schedule_delayed_work(&bcm_calc->work, msecs_to_jiffies(bcm_calc->sample_time));
		}
		mutex_unlock(&bcm_calc->lock);
	}
	return;
}
EXPORT_SYMBOL(exynos_bcm_get_data);

u64 exynos_bcm_get_ccnt(unsigned int idx)
{
	struct exynos_bcm_calc *bcm_calc = bcm_dbg_data->bcm_calc;
	u64 ret = -EINVAL;

	if (idx < bcm_calc->num_ip)
		ret = bcm_calc->acc_data[idx].ccnt;

	return ret;
}
EXPORT_SYMBOL(exynos_bcm_get_ccnt);

static void exynos_bcm_dbg_set_base_info(
				struct exynos_bcm_ipc_base_info *ipc_base_info,
				enum exynos_bcm_event_id event_id,
				enum exynos_bcm_event_dir direction,
				enum exynos_bcm_ip_range ip_range)
{
	ipc_base_info->event_id = event_id;
	ipc_base_info->ip_range = ip_range;
	ipc_base_info->direction = direction;
}

static void exynos_bcm_dbg_set_base_cmd(unsigned int *cmd,
				struct exynos_bcm_ipc_base_info *ipc_base_info)
{
	cmd[0] = 0;
	cmd[0] |= BCM_CMD_SET(ipc_base_info->event_id, BCM_EVT_ID_MASK,
					BCM_EVT_ID_SHIFT);
	cmd[0] |= BCM_CMD_SET(ipc_base_info->ip_range, BCM_ONE_BIT_MASK,
					BCM_IP_RANGE_SHIFT);
	cmd[0] |= BCM_CMD_SET(ipc_base_info->direction, BCM_ONE_BIT_MASK,
					BCM_EVT_DIR_SHIFT);
}

static int exynos_bcm_dbg_event_ctrl(struct exynos_bcm_ipc_base_info *ipc_base_info,
					struct exynos_bcm_event *bcm_event,
					unsigned int bcm_ip_index,
					struct exynos_bcm_dbg_data *data)
{
	unsigned int cmd[4] = {0, 0, 0, 0};
	int i, ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);

	if (!ipc_base_info || !bcm_event) {
		BCM_ERR("%s: pointer is NULL\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	exynos_bcm_dbg_set_base_cmd(cmd, ipc_base_info);

	if (ipc_base_info->event_id != BCM_EVT_PRE_DEFINE &&
		ipc_base_info->event_id != BCM_EVT_EVENT) {
		BCM_ERR("%s: Invalid Event ID(%d)\n", __func__,
					ipc_base_info->event_id);
		ret = -EINVAL;
		goto out;
	}

	if (ipc_base_info->ip_range == BCM_EACH)
		cmd[0] |= BCM_CMD_SET(bcm_ip_index, BCM_IP_MASK, BCM_IP_SHIFT);

	if (ipc_base_info->direction == BCM_EVT_SET) {
		/*
		 * check bcm running state
		 * When bcm is running, value can not set
		 */
		ret = exynos_bcm_is_running(data->bcm_run_state);
		if (ret)
			goto out;

		cmd[0] |= BCM_CMD_SET(bcm_event->index, BCM_EVT_PRE_DEFINE_MASK,
					BCM_EVT_PRE_DEFINE_SHIFT);

		for (i = 0; i < BCM_EVT_EVENT_MAX / 2; i++) {
			cmd[1] |= BCM_CMD_SET(bcm_event->event[i], BCM_EVT_EVENT_MASK,
						BCM_EVT_EVENT_SHIFT(i));
			if (data->bcm_cnt_nr > 4) {
				cmd[2] |= BCM_CMD_SET(bcm_event->event[i + 4], BCM_EVT_EVENT_MASK,
						BCM_EVT_EVENT_SHIFT(i + 4));
			}
		}
	}

	/* send command for BCM Event */
	ret = __exynos_bcm_dbg_ipc_send_data(IPC_BCM_DBG_EVENT, data, cmd);
	if (ret) {
		BCM_ERR("%s: Failed send data\n", __func__);
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_GET) {
		bcm_event->index = BCM_CMD_GET(cmd[0], BCM_EVT_PRE_DEFINE_MASK,
						BCM_EVT_PRE_DEFINE_SHIFT);

		for (i = 0; i < BCM_EVT_EVENT_MAX / 2; i++) {
			bcm_event->event[i] = BCM_CMD_GET(cmd[1], BCM_EVT_EVENT_MASK,
							BCM_EVT_EVENT_SHIFT(i));
			if (data->bcm_cnt_nr > 4) {
				bcm_event->event[i + 4] = BCM_CMD_GET(cmd[2], BCM_EVT_EVENT_MASK,
						BCM_EVT_EVENT_SHIFT(i + 4));
			}
		}
	}

out:
	spin_unlock_irqrestore(&data->lock, flags);

	return ret;
}

static int exynos_bcm_dbg_filter_id_ctrl(struct exynos_bcm_ipc_base_info *ipc_base_info,
					struct exynos_bcm_filter_id *filter_id,
					unsigned int bcm_ip_index,
					struct exynos_bcm_dbg_data *data)
{
	unsigned int cmd[4] = {0, 0, 0, 0};
	int i, ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);

	if (!ipc_base_info || !filter_id) {
		BCM_ERR("%s: pointer is NULL\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	exynos_bcm_dbg_set_base_cmd(cmd, ipc_base_info);

	if (ipc_base_info->event_id != BCM_EVT_EVENT_FLT_ID) {
		BCM_ERR("%s: Invalid Event ID(%d)\n", __func__,
					ipc_base_info->event_id);
		ret = -EINVAL;
		goto out;
	}

	if (ipc_base_info->ip_range == BCM_EACH)
		cmd[0] |= BCM_CMD_SET(bcm_ip_index, BCM_IP_MASK, BCM_IP_SHIFT);

	if (ipc_base_info->direction == BCM_EVT_SET) {
		/*
		 * check bcm running state
		 * When bcm is running, value can not set
		 */
		ret = exynos_bcm_is_running(data->bcm_run_state);
		if (ret)
			goto out;

		cmd[1] = filter_id->sm_id_mask;
		cmd[2] = filter_id->sm_id_value;
		for (i = 0; i < data->bcm_cnt_nr; i++)
			cmd[3] |= BCM_CMD_SET(filter_id->sm_id_active[i],
					BCM_ONE_BIT_MASK, BCM_EVT_FLT_ACT_SHIFT(i));
	}

	/* send command for BCM Filter ID */
	ret = __exynos_bcm_dbg_ipc_send_data(IPC_BCM_DBG_EVENT, data, cmd);
	if (ret) {
		BCM_ERR("%s: Failed send data\n", __func__);
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_GET) {
		filter_id->sm_id_mask = cmd[1];
		filter_id->sm_id_value = cmd[2];
		for (i = 0; i < data->bcm_cnt_nr; i++)
			filter_id->sm_id_active[i] = BCM_CMD_GET(cmd[3],
							BCM_ONE_BIT_MASK,
							BCM_EVT_FLT_ACT_SHIFT(i));
	}

out:
	spin_unlock_irqrestore(&data->lock, flags);

	return ret;
}

static int exynos_bcm_dbg_filter_others_ctrl(
					struct exynos_bcm_ipc_base_info *ipc_base_info,
					struct exynos_bcm_filter_others *filter_others,
					unsigned int bcm_ip_index,
					struct exynos_bcm_dbg_data *data)
{
	unsigned int cmd[4] = {0, 0, 0, 0};
	int i, ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);

	if (!ipc_base_info || !filter_others) {
		BCM_ERR("%s: pointer is NULL\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	exynos_bcm_dbg_set_base_cmd(cmd, ipc_base_info);

	if (ipc_base_info->event_id != BCM_EVT_EVENT_FLT_OTHERS) {
		BCM_ERR("%s: Invalid Event ID(%d)\n", __func__,
					ipc_base_info->event_id);
		ret = -EINVAL;
		goto out;
	}

	if (ipc_base_info->ip_range == BCM_EACH)
		cmd[0] |= BCM_CMD_SET(bcm_ip_index, BCM_IP_MASK, BCM_IP_SHIFT);

	if (ipc_base_info->direction == BCM_EVT_SET) {
		/*
		 * check bcm running state
		 * When bcm is running, value can not set
		 */
		ret = exynos_bcm_is_running(data->bcm_run_state);
		if (ret)
			goto out;

		for (i = 0; i < BCM_EVT_FLT_OTHR_MAX; i++) {
			cmd[1] |= BCM_CMD_SET(filter_others->sm_other_type[i],
						BCM_EVT_FLT_OTHR_TYPE_MASK,
						BCM_EVT_FLT_OTHR_TYPE_SHIFT(i));
			cmd[1] |= BCM_CMD_SET(filter_others->sm_other_mask[i],
						BCM_EVT_FLT_OTHR_MASK_MASK,
						BCM_EVT_FLT_OTHR_MASK_SHIFT(i));
			cmd[1] |= BCM_CMD_SET(filter_others->sm_other_value[i],
						BCM_EVT_FLT_OTHR_VALUE_MASK,
						BCM_EVT_FLT_OTHR_VALUE_SHIFT(i));
		}

		for (i = 0; i < data->bcm_cnt_nr; i++)
			cmd[2] |= BCM_CMD_SET(filter_others->sm_other_active[i],
					BCM_ONE_BIT_MASK, BCM_EVT_FLT_ACT_SHIFT(i));
	}

	/* send command for BCM Filter Others */
	ret = __exynos_bcm_dbg_ipc_send_data(IPC_BCM_DBG_EVENT, data, cmd);
	if (ret) {
		BCM_ERR("%s: Failed send data\n", __func__);
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_GET) {
		for (i = 0; i < BCM_EVT_FLT_OTHR_MAX; i++) {
			filter_others->sm_other_type[i] =
				BCM_CMD_GET(cmd[1], BCM_EVT_FLT_OTHR_TYPE_MASK,
						BCM_EVT_FLT_OTHR_TYPE_SHIFT(i));
			filter_others->sm_other_mask[i] =
				BCM_CMD_GET(cmd[1], BCM_EVT_FLT_OTHR_MASK_MASK,
						BCM_EVT_FLT_OTHR_MASK_SHIFT(i));
			filter_others->sm_other_value[i] =
				BCM_CMD_GET(cmd[1], BCM_EVT_FLT_OTHR_VALUE_MASK,
						BCM_EVT_FLT_OTHR_VALUE_SHIFT(i));
		}

		for (i = 0; i < data->bcm_cnt_nr; i++)
			filter_others->sm_other_active[i] =
				BCM_CMD_GET(cmd[2], BCM_ONE_BIT_MASK,
						BCM_EVT_FLT_ACT_SHIFT(i));
	}

out:
	spin_unlock_irqrestore(&data->lock, flags);

	return ret;
}

static int exynos_bcm_dbg_sample_id_ctrl(struct exynos_bcm_ipc_base_info *ipc_base_info,
					struct exynos_bcm_sample_id *sample_id,
					unsigned int bcm_ip_index,
					struct exynos_bcm_dbg_data *data)
{
	unsigned int cmd[4] = {0, 0, 0, 0};
	int i, ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);

	if (!ipc_base_info || !sample_id) {
		BCM_ERR("%s: pointer is NULL\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	exynos_bcm_dbg_set_base_cmd(cmd, ipc_base_info);

	if (ipc_base_info->event_id != BCM_EVT_EVENT_SAMPLE_ID) {
		BCM_ERR("%s: Invalid Event ID(%d)\n", __func__,
					ipc_base_info->event_id);
		ret = -EINVAL;
		goto out;
	}

	if (ipc_base_info->ip_range == BCM_EACH)
		cmd[0] |= BCM_CMD_SET(bcm_ip_index, BCM_IP_MASK, BCM_IP_SHIFT);

	if (ipc_base_info->direction == BCM_EVT_SET) {
		/*
		 * check bcm running state
		 * When bcm is running, value can not set
		 */
		ret = exynos_bcm_is_running(data->bcm_run_state);
		if (ret)
			goto out;

		cmd[1] = sample_id->peak_mask;
		cmd[2] = sample_id->peak_id;
		for (i = 0; i < data->bcm_cnt_nr; i++)
			cmd[3] |= BCM_CMD_SET(sample_id->peak_enable[i],
					BCM_ONE_BIT_MASK, BCM_EVT_FLT_ACT_SHIFT(i));
	}

	/* send command for BCM Sample ID */
	ret = __exynos_bcm_dbg_ipc_send_data(IPC_BCM_DBG_EVENT, data, cmd);
	if (ret) {
		BCM_ERR("%s: Failed send data\n", __func__);
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_GET) {
		sample_id->peak_mask = cmd[1];
		sample_id->peak_id = cmd[2];
		for (i = 0; i < data->bcm_cnt_nr; i++)
			sample_id->peak_enable[i] = BCM_CMD_GET(cmd[3],
							BCM_ONE_BIT_MASK,
							BCM_EVT_FLT_ACT_SHIFT(i));
	}

out:
	spin_unlock_irqrestore(&data->lock, flags);

	return ret;
}

static int exynos_bcm_dbg_run_ctrl(struct exynos_bcm_ipc_base_info *ipc_base_info,
					unsigned int *bcm_run,
					struct exynos_bcm_dbg_data *data)
{
	unsigned int cmd[4] = {0, 0, 0, 0};
	unsigned int run, low_ktime, high_ktime;
	int ret = 0;
	u64 ktime;
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);

	if (!ipc_base_info) {
		BCM_ERR("%s: pointer is NULL\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	exynos_bcm_dbg_set_base_cmd(cmd, ipc_base_info);

	if (ipc_base_info->event_id != BCM_EVT_RUN_CONT) {
		BCM_ERR("%s: Invalid Event ID(%d)\n", __func__,
					ipc_base_info->event_id);
		ret = -EINVAL;
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_SET) {
		run = *bcm_run;

		if (!(run ^ data->bcm_run_state)) {
			BCM_INFO("%s: same run control command(%u) bcm_run_state(%u)\n",
					__func__, run, data->bcm_run_state);
			goto out;
		}

		cmd[0] |= BCM_CMD_SET(run, BCM_ONE_BIT_MASK,
					BCM_EVT_RUN_CONT_SHIFT);

		if (run == BCM_STOP) {
			ktime = sched_clock();
			low_ktime = (unsigned int)(ktime & EXYNOS_BCM_U64_LOW_MASK);
			high_ktime = (unsigned int)((ktime & EXYNOS_BCM_U64_HIGH_MASK)
							>> EXYNOS_BCM_32BIT_SHIFT);
			cmd[1] = low_ktime;
			cmd[2] = high_ktime;

#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
			if (data->bcm_mode != BCM_MODE_USERCTRL)
				hrtimer_try_to_cancel(&data->bcm_hrtimer);
#endif
		}

#if defined(CONFIG_CPU_IDLE)
	if (*bcm_run == BCM_RUN)
		exynos_update_ip_idle_status(bcm_dbg_data->idle_ip_index, 0);
	else
		exynos_update_ip_idle_status(bcm_dbg_data->idle_ip_index, 1);
#endif
	}

	/* send command for BCM Run */
	ret = __exynos_bcm_dbg_ipc_send_data(IPC_BCM_DBG_EVENT, data, cmd);
	if (ret) {
		BCM_ERR("%s: Failed send data\n", __func__);
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_GET) {
		run = BCM_CMD_GET(cmd[0], BCM_ONE_BIT_MASK,
					BCM_EVT_RUN_CONT_SHIFT);
		*bcm_run = run;
	} else if (ipc_base_info->direction == BCM_EVT_SET) {
		data->bcm_run_state = run;
#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
		if (run == BCM_RUN && data->bcm_mode != BCM_MODE_USERCTRL)
			hrtimer_start(&data->bcm_hrtimer,
				ms_to_ktime(data->period), HRTIMER_MODE_REL);
#endif
	}

	spin_unlock_irqrestore(&data->lock, flags);

	/* dumping data from buffer */
	if (run == BCM_STOP &&
		ipc_base_info->direction == BCM_EVT_SET)
		exynos_bcm_dbg_buffer_dump(data, data->dump_klog);

	return ret;

out:
	spin_unlock_irqrestore(&data->lock, flags);

	return ret;
}

static int exynos_bcm_dbg_period_ctrl(struct exynos_bcm_ipc_base_info *ipc_base_info,
					unsigned int *bcm_period,
					struct exynos_bcm_dbg_data *data)
{
	unsigned int cmd[4] = {0, 0, 0, 0};
	unsigned int period;
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);

	if (!ipc_base_info) {
		BCM_ERR("%s: pointer is NULL\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	exynos_bcm_dbg_set_base_cmd(cmd, ipc_base_info);

	if (ipc_base_info->event_id != BCM_EVT_PERIOD_CONT) {
		BCM_ERR("%s: Invalid Event ID(%d)\n", __func__,
					ipc_base_info->event_id);
		ret = -EINVAL;
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_SET) {
		/*
		 * check bcm running state
		 * When bcm is running, value can not set
		 */
		ret = exynos_bcm_is_running(data->bcm_run_state);
		if (ret)
			goto out;

		period = *bcm_period;

		/* valid check for period range */
		if (!(period >= BCM_TIMER_PERIOD_MIN &&
			period <= BCM_TIMER_PERIOD_MAX)) {
			BCM_ERR("%s: Invalid period range(%umsec),(%d ~ %dmsec)\n",
					__func__, period,
					BCM_TIMER_PERIOD_MIN, BCM_TIMER_PERIOD_MAX);
			ret = -EINVAL;
			goto out;
		}

		cmd[1] |= BCM_CMD_SET(period, BCM_EVT_PERIOD_CONT_MASK,
					BCM_EVT_PERIOD_CONT_SHIFT);
	}

	/* send command for BCM Period */
	ret = __exynos_bcm_dbg_ipc_send_data(IPC_BCM_DBG_EVENT, data, cmd);
	if (ret) {
		BCM_ERR("%s: Failed send data\n", __func__);
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_GET) {
		period = BCM_CMD_GET(cmd[1], BCM_EVT_PERIOD_CONT_MASK,
					BCM_EVT_PERIOD_CONT_SHIFT);
		*bcm_period = period;
	}

#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
	data->period = period;
#endif
out:
	spin_unlock_irqrestore(&data->lock, flags);

	return ret;
}

static int exynos_bcm_dbg_mode_ctrl(struct exynos_bcm_ipc_base_info *ipc_base_info,
					unsigned int *bcm_mode,
					struct exynos_bcm_dbg_data *data)
{
	unsigned int cmd[4] = {0, 0, 0, 0};
	unsigned int mode;
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);

	if (!ipc_base_info) {
		BCM_ERR("%s: pointer is NULL\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	exynos_bcm_dbg_set_base_cmd(cmd, ipc_base_info);

	if (ipc_base_info->event_id != BCM_EVT_MODE_CONT) {
		BCM_ERR("%s: Invalid Event ID(%d)\n", __func__,
					ipc_base_info->event_id);
		ret = -EINVAL;
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_SET) {
		/*
		 * check bcm running state
		 * When bcm is running, value can not set
		 */
		ret = exynos_bcm_is_running(data->bcm_run_state);
		if (ret)
			goto out;

		mode = *bcm_mode;

		if (mode >= BCM_MODE_MAX) {
			BCM_ERR("%s: Invalid BCM mode(%u), BCM mode max(%d)\n",
					__func__, mode, BCM_MODE_MAX);
			ret = -EINVAL;
			goto out;
		}

		cmd[0] |= BCM_CMD_SET(mode, BCM_EVT_MODE_CONT_MASK,
					BCM_EVT_MODE_CONT_SHIFT);
	}

	/* send command for BCM Mode */
	ret = __exynos_bcm_dbg_ipc_send_data(IPC_BCM_DBG_EVENT, data, cmd);
	if (ret) {
		BCM_ERR("%s: Failed send data\n", __func__);
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_GET) {
		mode = BCM_CMD_GET(cmd[0], BCM_EVT_MODE_CONT_MASK,
					BCM_EVT_MODE_CONT_SHIFT);
		*bcm_mode = mode;
	}

#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
	data->bcm_mode = mode;
#endif

out:
	spin_unlock_irqrestore(&data->lock, flags);

	return ret;
}

static int exynos_bcm_dbg_str_ctrl(struct exynos_bcm_ipc_base_info *ipc_base_info,
					unsigned int *ap_suspend,
					struct exynos_bcm_dbg_data *data)
{
	unsigned int cmd[4] = {0, 0, 0, 0};
	unsigned int suspend;
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);

	if (!ipc_base_info) {
		BCM_ERR("%s: pointer is NULL\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	exynos_bcm_dbg_set_base_cmd(cmd, ipc_base_info);

	if (ipc_base_info->event_id != BCM_EVT_STR_STATE) {
		BCM_ERR("%s: Invalid Event ID(%d)\n", __func__,
					ipc_base_info->event_id);
		ret = -EINVAL;
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_SET) {
		suspend = *ap_suspend;
		cmd[0] |= BCM_CMD_SET(suspend, BCM_ONE_BIT_MASK,
					BCM_EVT_STR_STATE_SHIFT);

#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
		if (suspend && data->bcm_mode != BCM_MODE_USERCTRL)
			hrtimer_try_to_cancel(&data->bcm_hrtimer);
#endif
	}

	/* send command for BCM STR state */
	ret = __exynos_bcm_dbg_ipc_send_data(IPC_BCM_DBG_EVENT, data, cmd);
	if (ret) {
		BCM_ERR("%s: Failed send data\n", __func__);
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_GET) {
		suspend = BCM_CMD_GET(cmd[0], BCM_ONE_BIT_MASK,
					BCM_EVT_STR_STATE_SHIFT);
		*ap_suspend = suspend;
	}
#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
	if (ipc_base_info->direction == BCM_EVT_SET) {
		if (!suspend && data->bcm_mode != BCM_MODE_USERCTRL)
			hrtimer_start(&data->bcm_hrtimer,
				ms_to_ktime(data->period), HRTIMER_MODE_REL);
	}
#endif

out:
	spin_unlock_irqrestore(&data->lock, flags);

	return ret;
}

static int exynos_bcm_dbg_ip_ctrl(struct exynos_bcm_ipc_base_info *ipc_base_info,
					unsigned int *bcm_ip_enable,
					unsigned int bcm_ip_index,
					struct exynos_bcm_dbg_data *data)
{
	unsigned int cmd[4] = {0, 0, 0, 0};
	unsigned int ip_enable;
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);

	if (!ipc_base_info) {
		BCM_ERR("%s: pointer is NULL\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	exynos_bcm_dbg_set_base_cmd(cmd, ipc_base_info);

	if (ipc_base_info->event_id != BCM_EVT_IP_CONT) {
		BCM_ERR("%s: Invalid Event ID(%d)\n", __func__,
					ipc_base_info->event_id);
		ret = -EINVAL;
		goto out;
	}

	if (ipc_base_info->ip_range == BCM_EACH)
		cmd[0] |= BCM_CMD_SET(bcm_ip_index, BCM_IP_MASK, BCM_IP_SHIFT);

	if (ipc_base_info->direction == BCM_EVT_SET) {
		/*
		 * check bcm running state
		 * When bcm is running, value can not set
		 */
		ret = exynos_bcm_is_running(data->bcm_run_state);
		if (ret)
			goto out;

		ip_enable = *bcm_ip_enable;
		cmd[0] |= BCM_CMD_SET(ip_enable, BCM_ONE_BIT_MASK,
					BCM_EVT_IP_CONT_SHIFT);
	}

	/* send command for BCM IP control */
	ret = __exynos_bcm_dbg_ipc_send_data(IPC_BCM_DBG_EVENT, data, cmd);
	if (ret) {
		BCM_ERR("%s: Failed send data\n", __func__);
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_GET) {
		ip_enable = BCM_CMD_GET(cmd[0], BCM_ONE_BIT_MASK,
					BCM_EVT_IP_CONT_SHIFT);
		*bcm_ip_enable = ip_enable;
	}

out:
	spin_unlock_irqrestore(&data->lock, flags);

	return ret;
}

static int exynos_bcm_dbg_dump_addr_ctrl(struct exynos_bcm_ipc_base_info *ipc_base_info,
					struct exynos_bcm_dump_addr *dump_addr,
					struct exynos_bcm_dbg_data *data)
{
	unsigned int cmd[4] = {0, 0, 0, 0};
	int ret = 0;
	unsigned long flags;
#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
	u64 v_addr;
#endif

	spin_lock_irqsave(&data->lock, flags);

	if (!ipc_base_info) {
		BCM_ERR("%s: pointer is NULL\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	exynos_bcm_dbg_set_base_cmd(cmd, ipc_base_info);

	if (ipc_base_info->event_id != BCM_EVT_DUMP_ADDR) {
		BCM_ERR("%s: Invalid Event ID(%d)\n", __func__,
					ipc_base_info->event_id);
		ret = -EINVAL;
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_SET) {
		/*
		 * check bcm running state
		 * When bcm is running, value can not set
		 */
		ret = exynos_bcm_is_running(data->bcm_run_state);
		if (ret)
			goto out;

#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
		v_addr = (u64)dump_addr->v_addr;
		if (!v_addr || !dump_addr->buff_size) {
			BCM_ERR("%s: No dump address info: v_addr(%llu), buff_size(0x%08x)\n",
					__func__, v_addr, dump_addr->buff_size);
			ret = -EINVAL;
			goto out;
		}

		cmd[1] = (unsigned int)(v_addr & EXYNOS_BCM_U64_LOW_MASK);
		cmd[2] = (unsigned int)((v_addr & EXYNOS_BCM_U64_HIGH_MASK)
						>> EXYNOS_BCM_32BIT_SHIFT);
		cmd[3] = (unsigned int)dump_addr->buff_size;
#else
		if (!dump_addr->p_addr || !dump_addr->buff_size) {
			BCM_ERR("%s: No dump address info: p_addr(0x%08x), buff_size(0x%08x)\n",
					__func__, dump_addr->p_addr, dump_addr->buff_size);
			ret = -EINVAL;
			goto out;
		}

		cmd[1] = (unsigned int)dump_addr->p_addr;
		cmd[2] = (unsigned int)dump_addr->buff_size;
#endif
	}

	/* send command for BCM Dump address info */
	ret = __exynos_bcm_dbg_ipc_send_data(IPC_BCM_DBG_EVENT, data, cmd);
	if (ret) {
		BCM_ERR("%s: Failed send data\n", __func__);
		goto out;
	}

out:
	spin_unlock_irqrestore(&data->lock, flags);

	return ret;
}

static int exynos_bcm_dbg_glb_auto_ctrl(struct exynos_bcm_ipc_base_info *ipc_base_info,
					unsigned int *glb_auto_en,
					struct exynos_bcm_dbg_data *data)
{
	unsigned int cmd[4] = {0, 0, 0, 0};
	unsigned int glb_en;
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);

	if (!ipc_base_info) {
		BCM_ERR("%s: pointer is NULL\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	exynos_bcm_dbg_set_base_cmd(cmd, ipc_base_info);

	if (ipc_base_info->event_id != BCM_EVT_GLBAUTO_CONT) {
		BCM_ERR("%s: Invalid Event ID(%d)\n", __func__,
					ipc_base_info->event_id);
		ret = -EINVAL;
		goto out;
	}

	glb_en = *glb_auto_en;
	if (ipc_base_info->direction == BCM_EVT_SET) {
		cmd[0] |= BCM_CMD_SET(glb_en, BCM_ONE_BIT_MASK,
					BCM_EVT_GLBAUTO_SHIFT);

	}

	/* send command for BCM glb en */
	ret = __exynos_bcm_dbg_ipc_send_data(IPC_BCM_DBG_EVENT, data, cmd);
	if (ret) {
		BCM_ERR("%s: Failed send data\n", __func__);
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_GET) {
		glb_en = BCM_CMD_GET(cmd[0], BCM_ONE_BIT_MASK,
					BCM_EVT_GLBAUTO_SHIFT);
		*glb_auto_en = glb_en;
	}

out:
	spin_unlock_irqrestore(&data->lock, flags);

	return ret;
}

static int exynos_bcm_dbg_histogram_id(struct exynos_bcm_ipc_base_info *ipc_base_info,
					unsigned int *id0, unsigned int *id1,
					unsigned int bcm_ip_index,
					struct exynos_bcm_dbg_data *data)
{
	unsigned int cmd[4] = {0, 0, 0, 0};
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);

	if (!ipc_base_info) {
		BCM_ERR("%s: pointer is NULL\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	exynos_bcm_dbg_set_base_cmd(cmd, ipc_base_info);

	if (ipc_base_info->event_id != BCM_EVT_HISTOGRAM_ID) {
		BCM_ERR("%s: Invalid Event ID(%d)\n", __func__,
					ipc_base_info->event_id);
		ret = -EINVAL;
		goto out;
	}

	if (ipc_base_info->ip_range == BCM_EACH)
		cmd[0] |= BCM_CMD_SET(bcm_ip_index, BCM_IP_MASK, BCM_IP_SHIFT);

	if (ipc_base_info->direction == BCM_EVT_SET) {
		/*
		 * check bcm running state
		 * When bcm is running, value can not set
		 */
		ret = exynos_bcm_is_running(data->bcm_run_state);
		if (ret)
			goto out;

		cmd[1] = *id0;
		cmd[2] = *id1;
	}

	/* send command for BCM histogram config */
	ret = __exynos_bcm_dbg_ipc_send_data(IPC_BCM_DBG_EVENT, data, cmd);
	if (ret) {
		BCM_ERR("%s: Failed send data\n", __func__);
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_GET) {
		*id0 = cmd[1];
		*id1 = cmd[2];
	}

out:
	spin_unlock_irqrestore(&data->lock, flags);

	return ret;
}

static int exynos_bcm_dbg_histogram_config(struct exynos_bcm_ipc_base_info *ipc_base_info,
					unsigned int *histogram_config,
					unsigned int bcm_ip_index,
					struct exynos_bcm_dbg_data *data)
{
	unsigned int cmd[4] = {0, 0, 0, 0};
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);

	if (!ipc_base_info) {
		BCM_ERR("%s: pointer is NULL\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	exynos_bcm_dbg_set_base_cmd(cmd, ipc_base_info);

	if (ipc_base_info->event_id != BCM_EVT_HISTOGRAM_CONFIG) {
		BCM_ERR("%s: Invalid Event ID(%d)\n", __func__,
					ipc_base_info->event_id);
		ret = -EINVAL;
		goto out;
	}

	if (ipc_base_info->ip_range == BCM_EACH)
		cmd[0] |= BCM_CMD_SET(bcm_ip_index, BCM_IP_MASK, BCM_IP_SHIFT);

	if (ipc_base_info->direction == BCM_EVT_SET) {
		/*
		 * check bcm running state
		 * When bcm is running, value can not set
		 */
		ret = exynos_bcm_is_running(data->bcm_run_state);
		if (ret)
			goto out;

		cmd[1] = *histogram_config;
	}

	/* send command for BCM histogram config */
	ret = __exynos_bcm_dbg_ipc_send_data(IPC_BCM_DBG_EVENT, data, cmd);
	if (ret) {
		BCM_ERR("%s: Failed send data\n", __func__);
		goto out;
	}

	if (ipc_base_info->direction == BCM_EVT_GET)
		*histogram_config = cmd[1];

out:
	spin_unlock_irqrestore(&data->lock, flags);

	return ret;
}

static int exynos_bcm_dbg_early_init(struct exynos_bcm_dbg_data *data,
		unsigned int mode)
{
	struct exynos_bcm_ipc_base_info ipc_base_info;
	struct exynos_bcm_event bcm_event;
	struct exynos_bcm_filter_id filter_id;
	struct exynos_bcm_filter_others filter_others;
	struct exynos_bcm_sample_id sample_id;
	unsigned int event = 0;
	int ev_cnt, othr_cnt, ip_cnt;
	unsigned int temp;
	int ret = 0;
	int i = 0;

	/* pre-defined event set */
	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_PRE_DEFINE,
					BCM_EVT_SET, BCM_ALL);

	if (mode == DBG_MODE) {
		event = data->default_define_event;
	} else if (mode == PERF_MODE) {
		mutex_lock(&data->bcm_calc->lock);
		event = data->bcm_calc->perf_define_event;
		mutex_unlock(&data->bcm_calc->lock);
	}

	bcm_event.index = data->define_event[event].index;
	for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
		bcm_event.event[ev_cnt] =
			data->define_event[event].event[ev_cnt];

	ret = exynos_bcm_dbg_event_ctrl(&ipc_base_info, &bcm_event, 0, data);
	if (ret) {
		BCM_ERR("%s: failed set event\n", __func__);
		return ret;
	}

	/* default filter id set */
	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_EVENT_FLT_ID,
					BCM_EVT_SET, BCM_ALL);

	filter_id.sm_id_mask = data->define_filter_id[event].sm_id_mask;
	filter_id.sm_id_value = data->define_filter_id[event].sm_id_value;
	for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
		filter_id.sm_id_active[ev_cnt] =
			data->define_filter_id[event].sm_id_active[ev_cnt];

	ret = exynos_bcm_dbg_filter_id_ctrl(&ipc_base_info, &filter_id,
						0, data);
	if (ret) {
		BCM_ERR("%s: failed set filter ID\n", __func__);
		return ret;
	}

	/* default filter others set */
	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_EVENT_FLT_OTHERS,
					BCM_EVT_SET, BCM_ALL);

	for (othr_cnt = 0; othr_cnt < BCM_EVT_FLT_OTHR_MAX; othr_cnt++) {
		filter_others.sm_other_type[othr_cnt] =
			data->define_filter_others[event].sm_other_type[othr_cnt];
		filter_others.sm_other_mask[othr_cnt] =
			data->define_filter_others[event].sm_other_mask[othr_cnt];
		filter_others.sm_other_value[othr_cnt] =
			data->define_filter_others[event].sm_other_value[othr_cnt];
	}

	for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
		filter_others.sm_other_active[ev_cnt] =
			data->define_filter_others[event].sm_other_active[ev_cnt];

	ret = exynos_bcm_dbg_filter_others_ctrl(&ipc_base_info,
					&filter_others, 0, data);
	if (ret) {
		BCM_ERR("%s: failed set filter others\n", __func__);
		return ret;
	}

	/* default sample id set */
	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_EVENT_SAMPLE_ID,
					BCM_EVT_SET, BCM_ALL);

	sample_id.peak_mask = data->define_sample_id[event].peak_mask;
	sample_id.peak_id = data->define_sample_id[event].peak_id;
	for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
		sample_id.peak_enable[ev_cnt] =
			data->define_sample_id[event].peak_enable[ev_cnt];

	ret = exynos_bcm_dbg_sample_id_ctrl(&ipc_base_info, &sample_id,
						0, data);
	if (ret) {
		BCM_ERR("%s: failed set sample ID\n", __func__);
		return ret;
	}

	/* default period set */
	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_PERIOD_CONT,
					BCM_EVT_SET, 0);

	ret = exynos_bcm_dbg_period_ctrl(&ipc_base_info,
					&data->initial_period, data);
	if (ret) {
		BCM_ERR("%s: failed set period\n", __func__);
		return ret;
	}

	/* default mode set */
	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_MODE_CONT,
					BCM_EVT_SET, 0);

	ret = exynos_bcm_dbg_mode_ctrl(&ipc_base_info,
					&data->initial_bcm_mode, data);
	if (ret) {
		BCM_ERR("%s: failed set mode\n", __func__);
		return ret;
	}

	/* default run ip set */
	for (ip_cnt = 0; ip_cnt < data->bcm_ip_nr; ip_cnt++) {
		exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_IP_CONT,
				BCM_EVT_SET, BCM_EACH);

		if (mode == PERF_MODE) {
			/* back up */
			temp = data->initial_run_ip[ip_cnt];

			mutex_lock(&data->bcm_calc->lock);
			if (data->bcm_calc->ip_idx[i] == ip_cnt) {
				data->initial_run_ip[ip_cnt] = BCM_IP_EN;
				if (i < data->bcm_calc->num_ip - 1)
					i++;
			} else {
				data->initial_run_ip[ip_cnt] = BCM_IP_DIS;
			}

			mutex_unlock(&data->bcm_calc->lock);
		}

		ret = exynos_bcm_dbg_ip_ctrl(&ipc_base_info,
				&data->initial_run_ip[ip_cnt], ip_cnt, data);

		/* restore */
		if (mode == PERF_MODE)
			data->initial_run_ip[ip_cnt] = temp;

		if (ret) {
			BCM_ERR("%s: failed set IP control\n", __func__);
			return ret;
		}
	}

	/* glb_auto mode set */
	if (data->glb_auto_en) {
		exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_GLBAUTO_CONT,
						BCM_EVT_SET, 0);

		ret = exynos_bcm_dbg_glb_auto_ctrl(&ipc_base_info,
						&data->glb_auto_en, data);
		if (ret) {
			BCM_ERR("%s: failed set mode\n", __func__);
			return ret;
		}
	}

	return 0;
}

static int exynos_bcm_dbg_run(unsigned int bcm_run,
				struct exynos_bcm_dbg_data *data)
{
	struct exynos_bcm_ipc_base_info ipc_base_info;
	int ret;

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_RUN_CONT,
					BCM_EVT_SET, 0);

	ret = exynos_bcm_dbg_run_ctrl(&ipc_base_info, &bcm_run, data);
	if (ret) {
		BCM_ERR("%s: failed set Run state\n", __func__);
		return ret;
	}

	return 0;
}

void exynos_bcm_dbg_start(void)
{
	int ret;
	int enable;

	if (!bcm_dbg_data) {
		BCM_ERR("%s: bcm_dbg_data is not ready!\n", __func__);
		return;
	}

	mutex_lock(&bcm_dbg_data->bcm_calc->lock);
	enable = bcm_dbg_data->bcm_calc->enable;
	mutex_unlock(&bcm_dbg_data->bcm_calc->lock);

	if (enable) {
		BCM_INFO("%s: bcm_calc is enabled!\n", __func__);

		ret = exynos_bcm_dbg_run(BCM_STOP, bcm_dbg_data);
		if (ret) {
			BCM_ERR("%s: failed to bcm start\n", __func__);
			return;
		}
#if !(defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE))
		ret = exynos_bcm_dbg_early_init(bcm_dbg_data, DBG_MODE);
		if (ret)
			BCM_ERR("%s: failed to early bcm initialize\n",	__func__);
#endif
	}

#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
	if (!bcm_dbg_data->bcm_load_bin) {
		ret = exynos_bcm_dbg_load_bin();
		if (ret) {
			BCM_ERR("%s failed to load BCM bin\n", __func__);
			return;
		}
	}
#endif

#if defined(CONFIG_CPU_IDLE)
	exynos_update_ip_idle_status(bcm_dbg_data->idle_ip_index, 0);
#endif
	ret = exynos_bcm_dbg_run(BCM_RUN, bcm_dbg_data);
	if (ret) {
		BCM_ERR("%s: failed to bcm start\n", __func__);
		return;
	}

	BCM_INFO("%s\n", __func__);
}
EXPORT_SYMBOL(exynos_bcm_dbg_start);

void exynos_bcm_dbg_stop(unsigned int bcm_stop_owner)
{
	int ret;
	int enable;

	if (!bcm_dbg_data) {
		BCM_ERR("%s: bcm_dbg_data is not ready!\n", __func__);
		return;
	}
#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
	if (!bcm_dbg_data->bcm_load_bin) {
		BCM_ERR("BCM bin has not been loaded yet!!\n");
		return;
	}
#endif
	if (bcm_stop_owner >= STOP_OWNER_MAX) {
		BCM_ERR("Invalid stop owner (%u)\n", bcm_stop_owner);
		return;
	}

	if (!bcm_dbg_data->available_stop_owner[bcm_stop_owner]) {
		BCM_ERR("Have not stop permission (%u)\n", bcm_stop_owner);
		return;
	}

#if defined(CONFIG_CPU_IDLE)
	exynos_update_ip_idle_status(bcm_dbg_data->idle_ip_index, 0);
#endif
	ret = exynos_bcm_dbg_run(BCM_STOP, bcm_dbg_data);
	if (ret) {
		BCM_ERR("%s: failed to bcm stop\n", __func__);
		return;
	}

	if (bcm_stop_owner != PANIC_HANDLE) {
		mutex_lock(&bcm_dbg_data->bcm_calc->lock);
		enable = bcm_dbg_data->bcm_calc->enable;
		mutex_unlock(&bcm_dbg_data->bcm_calc->lock);
		/* restart perf mode after finishing dbg */
		if (enable) {
			ret = exynos_bcm_dbg_early_init(bcm_dbg_data, PERF_MODE);
			if (ret)
				BCM_ERR("%s: failed to early bcm initialize\n", __func__);

			exynos_bcm_dbg_run(BCM_RUN, bcm_dbg_data);
			if (ret) {
				BCM_ERR("%s: failed to bcm start\n", __func__);
				return;
			}
		}
	}

	BCM_INFO("%s\n", __func__);
}
EXPORT_SYMBOL(exynos_bcm_dbg_stop);

static int exynos_bcm_dbg_str(unsigned int suspend,
				struct exynos_bcm_dbg_data *data)
{
	struct exynos_bcm_ipc_base_info ipc_base_info;
	int ret;

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_STR_STATE,
					BCM_EVT_SET, 0);

	ret = exynos_bcm_dbg_str_ctrl(&ipc_base_info, &suspend, data);
	if (ret) {
		BCM_ERR("%s:failed set str state\n", __func__);
		return ret;
	}

	return 0;
}

/* SYSFS Interface */
static ssize_t show_bcm_dbg_data_pd(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	int i;
	ssize_t count = 0;

	if (off > 0)
		return 0;

	count += snprintf(buf, PAGE_SIZE, "=== IPC node info ===\n");

	count += snprintf(buf + count, PAGE_SIZE, "IPC node name: %s\n",
					data->ipc_node->name);

	count += snprintf(buf + count, PAGE_SIZE,
				"\n=== Local Power Domain info ===\n");
	count += snprintf(buf + count, PAGE_SIZE,
				"pd_size: %u, pd_sync_init: %s\n",
				data->pd_size,
				pd_sync_init ? "true" : "false");

	for (i = 0; i < data->pd_size; i++)
		count += snprintf(buf + count, PAGE_SIZE,
				"pd_name: %12s, pd_index: %2u, pd_on: %s, cal_pdid: 0x%08x\n",
				data->pd_info[i]->pd_name, data->pd_info[i]->pd_index,
				data->pd_info[i]->on ? "true" : "false",
				data->pd_info[i]->cal_pdid);

	return count;
}

static ssize_t show_bcm_dbg_data_df_event(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	int i, j;
	ssize_t count = 0;

	if (off > 0)
		return 0;

	count += snprintf(buf + count, PAGE_SIZE,
				"\n=== Pre-defined Event info ===\n");
	for (i = 0; i < data->define_event_max; i++) {
		count += snprintf(buf + count, PAGE_SIZE,
				"Pre-defined Event index: %2u\n",
				data->define_event[i].index);
		for (j = 0; j < data->bcm_cnt_nr; j++)
			count += snprintf(buf + count, PAGE_SIZE,
				" Event[%d]: 0x%02x\n", j, data->define_event[i].event[j]);
	}

	count += snprintf(buf + count, PAGE_SIZE,
				"Default Pre-defined Event index: %2u\n",
				data->default_define_event);
	count += snprintf(buf + count, PAGE_SIZE,
				"Pre-defined Event Max: %2u\n",
				data->define_event_max);

	return count;
}

static ssize_t show_bcm_dbg_data_df_filter(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	int i, j;
	ssize_t count = 0;

	if (off > 0)
		return 0;

	count += snprintf(buf + count, PAGE_SIZE, "\n=== Filter ID info ===\n");
	for (i = 0; i < data->define_event_max; i++) {
		count += snprintf(buf + count, PAGE_SIZE,
				"Pre-defined Event index: %2u\n",
				data->define_event[i].index);
		count += snprintf(buf + count, PAGE_SIZE, " Filter ID mask: 0x%08x\n",
				data->define_filter_id[i].sm_id_mask);
		count += snprintf(buf + count, PAGE_SIZE, " Filter ID value: 0x%08x\n",
				data->define_filter_id[i].sm_id_value);
		count += snprintf(buf + count, PAGE_SIZE, " Filter ID active\n");

		for (j = 0; j < data->bcm_cnt_nr; j++)
			count += snprintf(buf + count, PAGE_SIZE,
					"  Event[%d]: %u\n", j,
					data->define_filter_id[i].sm_id_active[j]);
	}

	count += snprintf(buf + count, PAGE_SIZE, "\n=== Filter Others info ===\n");
	for (i = 0; i < data->define_event_max; i++) {
		count += snprintf(buf + count, PAGE_SIZE,
				"Pre-defined Event index: %2u\n",
				data->define_event[i].index);

		for (j = 0; j < BCM_EVT_FLT_OTHR_MAX; j++) {
			count += snprintf(buf + count, PAGE_SIZE,
					" Filter Others type[%d]: 0x%02x\n",
					j, data->define_filter_others[i].sm_other_type[j]);
			count += snprintf(buf + count, PAGE_SIZE,
					" Filter Others mask[%d]: 0x%02x\n",
					j, data->define_filter_others[i].sm_other_mask[j]);
			count += snprintf(buf + count, PAGE_SIZE,
					" Filter Others value[%d]: 0x%02x\n",
					j, data->define_filter_others[i].sm_other_value[j]);
		}

		count += snprintf(buf + count, PAGE_SIZE, " Filter Others active\n");

		for (j = 0; j < data->bcm_cnt_nr; j++)
			count += snprintf(buf + count, PAGE_SIZE,
					"  Event[%d]: %u\n", j,
					data->define_filter_others[i].sm_other_active[j]);
	}

	return count;
}

static ssize_t show_bcm_dbg_data_df_sample(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	int i, j;
	ssize_t count = 0;

	if (off > 0)
		return 0;

	count += snprintf(buf + count, PAGE_SIZE, "\n=== Sample ID info ===\n");
	for (i = 0; i < data->define_event_max; i++) {
		count += snprintf(buf + count, PAGE_SIZE,
				"Pre-defined Event index: %2u\n",
				data->define_event[i].index);

		count += snprintf(buf + count, PAGE_SIZE, " Sample ID: peak_mask: 0x%08x\n",
					data->define_sample_id[i].peak_mask);
		count += snprintf(buf + count, PAGE_SIZE, " Sample ID: peak_id: 0x%08x\n",
					data->define_sample_id[i].peak_id);
		count += snprintf(buf + count, PAGE_SIZE, " Sample ID active\n");

		for (j = 0; j < data->bcm_cnt_nr; j++)
			count += snprintf(buf + count, PAGE_SIZE,
					"  Event[%d]: %u\n", j,
					data->define_sample_id[i].peak_enable[j]);
	}

	return count;
}

static ssize_t show_bcm_dbg_data_df_attr(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	ssize_t count = 0;
	static int ip_cnt = 0;

	if (ip_cnt >= data->bcm_ip_nr) {
		ip_cnt = 0;
		return 0;
	}

	if (off == 0) {
		count += snprintf(buf + count, PAGE_SIZE, "\n=== Ctrl Attr info ===\n");
		count += snprintf(buf + count, PAGE_SIZE, "Initial BCM run: %s\n",
				data->initial_bcm_run ? "true" : "false");
		count += snprintf(buf + count, PAGE_SIZE,
				"Initial monitor period: %u msec\n",
				data->initial_period);
		count += snprintf(buf + count, PAGE_SIZE, "Initial BCM mode: %u\n",
				data->initial_bcm_mode);
		count += snprintf(buf + count, PAGE_SIZE, "Initial Run IPs\n");
	}

	do {
		count += snprintf(buf + count, PAGE_SIZE,
				" BCM IP[%d]: %s\n", ip_cnt,
				data->initial_run_ip[ip_cnt] ? "true" : "false");
		ip_cnt++;
	} while ((ip_cnt < data->bcm_ip_nr) && (ip_cnt % data->bcm_ip_print_nr));

	return count;
}

static ssize_t show_get_event(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	struct exynos_bcm_event bcm_event;
	ssize_t count = 0;
	int ev_cnt, ret;
	static int ip_cnt = 0;

	if (ip_cnt >= data->bcm_ip_nr) {
		ip_cnt = 0;
		return 0;
	}

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_EVENT,
					BCM_EVT_GET, BCM_EACH);

	do {
		ret = exynos_bcm_dbg_event_ctrl(&ipc_base_info, &bcm_event,
						ip_cnt, data);
		if (ret) {
			BCM_ERR("%s: failed get event(ip:%d)\n",
					__func__, ip_cnt);
			ip_cnt = 0;
			return ret;
		}

		count += snprintf(buf + count, PAGE_SIZE,
					"bcm[%2d]: def(%2u),",
					ip_cnt, bcm_event.index);
		for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
			count += snprintf(buf + count, PAGE_SIZE,
					" (0x%02x),", bcm_event.event[ev_cnt]);
		count += snprintf(buf + count, PAGE_SIZE, "\n");
		ip_cnt++;
	} while ((ip_cnt < data->bcm_ip_nr) && (ip_cnt % data->bcm_ip_print_nr));

	return count;
}

static ssize_t show_event_ctrl_help(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	ssize_t count = 0;

	if (off > 0)
		return 0;

	/* help store_event_ctrl */
	count += snprintf(buf + count, PAGE_SIZE, "\n= event_ctrl set help =\n");
	count += snprintf(buf + count, PAGE_SIZE, "Usage:\n");
	count += snprintf(buf + count, PAGE_SIZE,
			"echo [ip_range] [ip_index] [define_index]	\
			[ev0] [ev1] [ev2] [ev3] [ev4] [ev5] [ev6] [ev7] >	\
			event_ctrl\n");
	count += snprintf(buf + count, PAGE_SIZE,
			" ip_range: BCM_EACH(%d), BCM_ALL(%d)\n",
				BCM_EACH, BCM_ALL);
	count += snprintf(buf + count, PAGE_SIZE,
			" ip_index: number of bcm ip (0 ~ %u)\n"
			"           (if ip_range is all, set to 0)\n",
				data->bcm_ip_nr - 1);
	count += snprintf(buf + count, PAGE_SIZE,
			" define_index: index of pre-defined event (0 ~ %u)\n"
			"               0 means no pre-defined event\n",
				data->define_event_max - 1);
	count += snprintf(buf + count, PAGE_SIZE,
			" evX: event value of counter (if define_index is not 0,	\
			set to 0\n"
			"      event value should be hexa value\n");

	return count;
}

static ssize_t store_event_ctrl(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	struct exynos_bcm_event bcm_event;
	unsigned int bcm_ip_index = 0;
	unsigned int ip_range = 0;
	unsigned int event_id, defined_index;
	unsigned int *event;
	int ev_cnt, dfd_cnt, ret;

	if (off != 0)
		return size;

	event = kzalloc(sizeof(int) * data->bcm_cnt_nr, GFP_KERNEL);
	if (event == NULL) {
		BCM_ERR("%s: faild allocated of event memory\n", __func__);
		return -ENOMEM;
	}

	if (data->bcm_cnt_nr == 4) {
		ret = sscanf(buf, "%u %u %u %x %x %x %x",
				&ip_range, &bcm_ip_index, &defined_index,
				&event[0], &event[1], &event[2], &event[3]);
	} else if (data->bcm_cnt_nr == 8) {
		ret = sscanf(buf, "%u %u %u %x %x %x %x %x %x %x %x",
				&ip_range, &bcm_ip_index, &defined_index,
				&event[0], &event[1], &event[2], &event[3],
				&event[4], &event[5], &event[6], &event[7]);
	} else {
		BCM_ERR("%s: invalid bcm_cnt_nr\n", __func__);
		kfree(event);
		return -EINVAL;
	}

	/* 3 means is the number of index */
	if (ret != data->bcm_cnt_nr + 3) {
		kfree(event);
		return -EINVAL;
	}

	ret = exynos_bcm_ip_validate(ip_range, bcm_ip_index, data->bcm_ip_nr);
	if (ret) {
		kfree(event);
		return ret;
	}

	if (defined_index >= data->define_event_max) {
		BCM_ERR("%s: Invalid defined index(%u),"
			" defined_max_nr(%u)\n", __func__,
				defined_index, data->define_event_max - 1);
		kfree(event);
		return -EINVAL;
	}

	if (ip_range == BCM_ALL)
		bcm_ip_index = 0;

	if (defined_index != NO_PRE_DEFINE_EVT) {
		event_id = BCM_EVT_PRE_DEFINE;
		for (dfd_cnt = 1; dfd_cnt < data->define_event_max; dfd_cnt++) {
			if (defined_index ==
				data->define_event[dfd_cnt].index)
				break;
		}

		bcm_event.index = data->define_event[dfd_cnt].index;
		for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
			bcm_event.event[ev_cnt] =
				data->define_event[dfd_cnt].event[ev_cnt];
	} else {
		event_id = BCM_EVT_EVENT;
		bcm_event.index = NO_PRE_DEFINE_EVT;
		for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
			bcm_event.event[ev_cnt] = event[ev_cnt];
	}

	exynos_bcm_dbg_set_base_info(&ipc_base_info, event_id,
					BCM_EVT_SET, ip_range);

	ret = exynos_bcm_dbg_event_ctrl(&ipc_base_info, &bcm_event,
						bcm_ip_index, data);
	if (ret) {
		BCM_ERR("%s:failed set event\n", __func__);
		kfree(event);
		return ret;
	}

	kfree(event);
	return size;
}

static ssize_t show_get_filter_id(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	struct exynos_bcm_filter_id filter_id;
	ssize_t count = 0;
	int ret;
	static int ip_cnt = 0;

	if (ip_cnt >= data->bcm_ip_nr) {
		ip_cnt = 0;
		return 0;
	}

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_EVENT_FLT_ID,
					BCM_EVT_GET, BCM_EACH);

	do {
		ret = exynos_bcm_dbg_filter_id_ctrl(&ipc_base_info,
						&filter_id, ip_cnt, data);
		if (ret) {
			BCM_ERR("%s: failed get filter id(ip:%d)\n",
					__func__, ip_cnt);
			ip_cnt = 0;
			return ret;
		}

		count += snprintf(buf + count, PAGE_SIZE,
				"bcm[%2d]: mask(0x%08x), value(0x%08x)\n",
				ip_cnt, filter_id.sm_id_mask, filter_id.sm_id_value);
		ip_cnt++;
	} while ((ip_cnt < data->bcm_ip_nr) && (ip_cnt % data->bcm_ip_print_nr));

	return count;
}

static ssize_t show_get_filter_id_active(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	struct exynos_bcm_filter_id filter_id;
	ssize_t count = 0;
	int ev_cnt, ret;
	static int ip_cnt = 0;

	if (ip_cnt >= data->bcm_ip_nr) {
		ip_cnt = 0;
		return 0;
	}

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_EVENT_FLT_ID,
					BCM_EVT_GET, BCM_EACH);

	do {
		ret = exynos_bcm_dbg_filter_id_ctrl(&ipc_base_info,
						&filter_id, ip_cnt, data);
		if (ret) {
			BCM_ERR("%s: failed get filter id(ip:%d)\n",
					__func__, ip_cnt);
			ip_cnt = 0;
			return ret;
		}

		count += snprintf(buf + count, PAGE_SIZE, "bcm[%2d]:", ip_cnt);
		for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
			count += snprintf(buf + count, PAGE_SIZE,
					" ev%d %u,", ev_cnt,
					filter_id.sm_id_active[ev_cnt]);
		count += snprintf(buf + count, PAGE_SIZE, "\n");
		ip_cnt++;
	} while ((ip_cnt < data->bcm_ip_nr) && (ip_cnt % data->bcm_ip_print_nr));

	return count;
}

static ssize_t show_filter_id_ctrl_help(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	ssize_t count = 0;

	if (off > 0)
		return 0;

	/* help store_filter_id_ctrl */
	count += snprintf(buf + count, PAGE_SIZE, "\n= filter_id_ctrl set help =\n");
	count += snprintf(buf + count, PAGE_SIZE, "Usage:\n");
	count += snprintf(buf + count, PAGE_SIZE,
			"echo [ip_range] [ip_index] [define_index] [mask] [value]	\
			[ev0] [ev1] [ev2] [ev3] [ev4] [ev5] [ev6] [ev7] >	\
			filter_id_ctrl\n");
	count += snprintf(buf + count, PAGE_SIZE,
			" ip_range: BCM_EACH(%d), BCM_ALL(%d)\n",
				BCM_EACH, BCM_ALL);
	count += snprintf(buf + count, PAGE_SIZE,
			" ip_index: number of bcm ip (0 ~ %u)\n"
			"           (if ip_range is all, set to 0)\n",
				data->bcm_ip_nr - 1);
	count += snprintf(buf + count, PAGE_SIZE,
			" define_index: index of pre-defined event (0 ~ %u)\n"
			"               0 means no pre-defined event\n",
				data->define_event_max - 1);
	count += snprintf(buf + count, PAGE_SIZE,
			" mask: masking for filter id (if define_index is not 0,	\
			set to 0)\n"
			"       mask value should be hexa value\n");
	count += snprintf(buf + count, PAGE_SIZE,
			" value: value of filter id (if define_index is not 0, set to 0)\n"
			"        value should be hexa value\n");
	count += snprintf(buf + count, PAGE_SIZE,
			" evX: event counter alloc for filter id	\
			(if define_index is not 0, set to 0)\n"
			"      value should be 0 or 1\n");

	return count;
}

static ssize_t store_filter_id_ctrl(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	struct exynos_bcm_filter_id filter_id;
	unsigned int bcm_ip_index = 0;
	unsigned int ip_range = 0;
	unsigned int defined_index;
	unsigned int sm_id_mask, sm_id_value;
	unsigned int *sm_id_active;
	int ev_cnt, ret;

	if (off != 0)
		return size;

	sm_id_active = kzalloc(sizeof(int) * data->bcm_cnt_nr, GFP_KERNEL);
	if (sm_id_active == NULL) {
		BCM_ERR("%s: faild allocated of sm_id_active memory\n", __func__);
		return -ENOMEM;
	}

	if (data->bcm_cnt_nr == 4) {
		ret = sscanf(buf, "%u %u %u %x %x %u %u %u %u",
				&ip_range, &bcm_ip_index, &defined_index, &sm_id_mask, &sm_id_value,
				&sm_id_active[0], &sm_id_active[1], &sm_id_active[2], &sm_id_active[3]);
	} else if (data->bcm_cnt_nr == 8) {
		ret = sscanf(buf, "%u %u %u %x %x %u %u %u %u %u %u %u %u",
				&ip_range, &bcm_ip_index, &defined_index, &sm_id_mask, &sm_id_value,
				&sm_id_active[0], &sm_id_active[1],
				&sm_id_active[2], &sm_id_active[3],
				&sm_id_active[4], &sm_id_active[5],
				&sm_id_active[6], &sm_id_active[7]);
	} else {
		BCM_ERR("%s: invalid bcm_cnt_nr\n", __func__);
		kfree(sm_id_active);
		return -EINVAL;
	}

	/* 5 --> the number of index */
	if (ret != data->bcm_cnt_nr + 5) {
		kfree(sm_id_active);
		return -EINVAL;
	}

	ret = exynos_bcm_ip_validate(ip_range, bcm_ip_index, data->bcm_ip_nr);
	if (ret) {
		kfree(sm_id_active);
		return ret;
	}

	for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++) {
		if (sm_id_active[ev_cnt])
			sm_id_active[ev_cnt] = true;
	}

	if (defined_index >= data->define_event_max) {
		BCM_ERR("%s: Invalid defined index(%u),"
			" defined_max_nr(%u)\n", __func__,
				defined_index, data->define_event_max - 1);
		kfree(sm_id_active);
		return -EINVAL;
	}

	if (ip_range == BCM_ALL)
		bcm_ip_index = 0;

	if (defined_index != NO_PRE_DEFINE_EVT) {
		filter_id.sm_id_mask = data->define_filter_id[defined_index].sm_id_mask;
		filter_id.sm_id_value = data->define_filter_id[defined_index].sm_id_value;
		for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
			filter_id.sm_id_active[ev_cnt] =
				data->define_filter_id[defined_index].sm_id_active[ev_cnt];
	} else {
		filter_id.sm_id_mask = sm_id_mask;
		filter_id.sm_id_value = sm_id_value;
		for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
			filter_id.sm_id_active[ev_cnt] = sm_id_active[ev_cnt];
	}

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_EVENT_FLT_ID,
					BCM_EVT_SET, ip_range);

	ret = exynos_bcm_dbg_filter_id_ctrl(&ipc_base_info, &filter_id,
						bcm_ip_index, data);
	if (ret) {
		BCM_ERR("%s:failed set filter ID\n", __func__);
		kfree(sm_id_active);
		return ret;
	}

	kfree(sm_id_active);
	return size;
}

static ssize_t show_get_filter_others(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	struct exynos_bcm_filter_others filter_others;
	ssize_t count = 0;
	int othr_cnt, ret;
	static int ip_cnt = 0;

	if (ip_cnt >= data->bcm_ip_nr) {
		ip_cnt = 0;
		return 0;
	}

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_EVENT_FLT_OTHERS,
					BCM_EVT_GET, BCM_EACH);

	do {
		ret = exynos_bcm_dbg_filter_others_ctrl(&ipc_base_info,
					&filter_others, ip_cnt, data);
		if (ret) {
			BCM_ERR("%s: failed get filter others(ip:%d)\n",
					__func__, ip_cnt);
			ip_cnt = 0;
			return ret;
		}

		count += snprintf(buf + count, PAGE_SIZE, "bcm[%2d]:", ip_cnt);
		for (othr_cnt = 0; othr_cnt < BCM_EVT_FLT_OTHR_MAX; othr_cnt++)
			count += snprintf(buf + count, PAGE_SIZE,
					" type%d(0x%02x), mask%d(0x%02x), value%d(0x%02x),",
					othr_cnt, filter_others.sm_other_type[othr_cnt],
					othr_cnt, filter_others.sm_other_mask[othr_cnt],
					othr_cnt, filter_others.sm_other_value[othr_cnt]);
		count += snprintf(buf + count, PAGE_SIZE, "\n");
		ip_cnt++;
	} while ((ip_cnt < data->bcm_ip_nr) && (ip_cnt % data->bcm_ip_print_nr));

	return count;
}

static ssize_t show_get_filter_others_active(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	struct exynos_bcm_filter_others filter_others;
	ssize_t count = 0;
	int ev_cnt, ret;
	static int ip_cnt = 0;

	if (ip_cnt >= data->bcm_ip_nr) {
		ip_cnt = 0;
		return 0;
	}

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_EVENT_FLT_OTHERS,
					BCM_EVT_GET, BCM_EACH);

	do {
		ret = exynos_bcm_dbg_filter_others_ctrl(&ipc_base_info,
					&filter_others, ip_cnt, data);
		if (ret) {
			BCM_ERR("%s: failed get filter others(ip:%d)\n",
					__func__, ip_cnt);
			ip_cnt = 0;
			return ret;
		}

		count += snprintf(buf + count, PAGE_SIZE, "bcm[%2d]:", ip_cnt);
		for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
			count += snprintf(buf + count, PAGE_SIZE,
					" ev%d %u,", ev_cnt,
					filter_others.sm_other_active[ev_cnt]);
		count += snprintf(buf + count, PAGE_SIZE, "\n");
		ip_cnt++;
	} while ((ip_cnt < data->bcm_ip_nr) && (ip_cnt % data->bcm_ip_print_nr));

	return count;
}

static ssize_t show_filter_others_ctrl_help(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	ssize_t count = 0;
	int othr_cnt;

	if (off > 0)
		return 0;

	/* help store_filter_others_ctrl */
	count += snprintf(buf + count, PAGE_SIZE, "\n= filter_others_ctrl set help =\n");
	count += snprintf(buf + count, PAGE_SIZE, "Usage:\n");
	count += snprintf(buf + count, PAGE_SIZE,
			"echo [ip_range] [ip_index] [define_index]	\
			[type0] [mask0] [value0] [type1] [mask1] [value1]	\
			[ev0] [ev1] [ev2] [ev3] [ev4] [ev5] [ev6] [ev7] >	\
			filter_others_ctrl\n");
	count += snprintf(buf + count, PAGE_SIZE,
			" ip_range: BCM_EACH(%d), BCM_ALL(%d)\n",
				BCM_EACH, BCM_ALL);
	count += snprintf(buf + count, PAGE_SIZE,
			" ip_index: number of bcm ip (0 ~ %u)\n"
			"           (if ip_range is all, set to 0)\n",
				data->bcm_ip_nr - 1);
	count += snprintf(buf + count, PAGE_SIZE,
			" define_index: index of pre-defined event (0 ~ %u)\n"
			"               0 means no pre-defined event\n",
				data->define_event_max - 1);
	for (othr_cnt = 0; othr_cnt < BCM_EVT_FLT_OTHR_MAX; othr_cnt++) {
		count += snprintf(buf + count, PAGE_SIZE,
				" type%d: type%d for filter others	\
				(if define_index is not 0, set to 0)\n"
				"         type%d value should be hexa value\n",
				othr_cnt, othr_cnt, othr_cnt);
		count += snprintf(buf + count, PAGE_SIZE,
				" mask%d: mask%d for filter others	\
				(if define_index is not 0, set to 0)\n"
				"         mask%d value should be hexa value\n",
				othr_cnt, othr_cnt, othr_cnt);
		count += snprintf(buf + count, PAGE_SIZE,
				" value%d: value%d of filter others	\
				(if define_index is not 0, set to 0)\n"
				"          value%d should be hexa value\n",
				othr_cnt, othr_cnt, othr_cnt);
	}
	count += snprintf(buf + count, PAGE_SIZE,
			" evX: event counter alloc for filter others	\
			(if define_index is not 0, set to 0)\n"
			"      value should be 0 or 1\n");

	return count;
}

static ssize_t store_filter_others_ctrl(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	struct exynos_bcm_filter_others filter_others;
	unsigned int bcm_ip_index = 0;
	unsigned int ip_range = 0;
	unsigned int defined_index;
	unsigned int sm_other_type[BCM_EVT_FLT_OTHR_MAX];
	unsigned int sm_other_mask[BCM_EVT_FLT_OTHR_MAX];
	unsigned int sm_other_value[BCM_EVT_FLT_OTHR_MAX];
	unsigned int *sm_other_active;
	int ev_cnt, othr_cnt, ret;

	if (off != 0)
		return size;

	sm_other_active = kzalloc(sizeof(int) * data->bcm_cnt_nr, GFP_KERNEL);
	if (sm_other_active == NULL) {
		BCM_ERR("%s: faild allocated of sm_other_active memory\n", __func__);
		return -ENOMEM;
	}

	if (data->bcm_cnt_nr == 4) {
		ret = sscanf(buf, "%u %u %u %x %x %x %x %x %x %u %u %u %u",
				&ip_range, &bcm_ip_index, &defined_index,
				&sm_other_type[0], &sm_other_mask[0], &sm_other_value[0],
				&sm_other_type[1], &sm_other_mask[1], &sm_other_value[1],
				&sm_other_active[0], &sm_other_active[1],
				&sm_other_active[2], &sm_other_active[3]);
	} else if (data->bcm_cnt_nr == 8) {
		ret = sscanf(buf, "%u %u %u %x %x %x %x %x %x %u %u %u %u %u %u %u %u",
				&ip_range, &bcm_ip_index, &defined_index,
				&sm_other_type[0], &sm_other_mask[0], &sm_other_value[0],
				&sm_other_type[1], &sm_other_mask[1], &sm_other_value[1],
				&sm_other_active[0], &sm_other_active[1],
				&sm_other_active[2], &sm_other_active[3],
				&sm_other_active[4], &sm_other_active[5],
				&sm_other_active[6], &sm_other_active[7]);
	} else {
		BCM_ERR("%s: invalid bcm_cnt_nr\n", __func__);
		kfree(sm_other_active);
		return -EINVAL;
	}

	/* 9 --> the number of index */
	if (ret != data->bcm_cnt_nr + 9) {
		kfree(sm_other_active);
		return -EINVAL;
	}

	ret = exynos_bcm_ip_validate(ip_range, bcm_ip_index, data->bcm_ip_nr);
	if (ret) {
		kfree(sm_other_active);
		return ret;
	}

	for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++) {
		if (sm_other_active[ev_cnt])
			sm_other_active[ev_cnt] = true;
	}

	if (defined_index >= data->define_event_max) {
		BCM_ERR("%s: Invalid defined index(%u),"
			" defined_max_nr(%u)\n", __func__,
				defined_index, data->define_event_max - 1);
		kfree(sm_other_active);
		return -EINVAL;
	}

	if (ip_range == BCM_ALL)
		bcm_ip_index = 0;

	if (defined_index != NO_PRE_DEFINE_EVT) {
		for (othr_cnt = 0; othr_cnt < BCM_EVT_FLT_OTHR_MAX; othr_cnt++) {
			filter_others.sm_other_type[othr_cnt] =
				data->define_filter_others[defined_index].sm_other_type[othr_cnt];
			filter_others.sm_other_mask[othr_cnt] =
				data->define_filter_others[defined_index].sm_other_mask[othr_cnt];
			filter_others.sm_other_value[othr_cnt] =
				data->define_filter_others[defined_index].sm_other_value[othr_cnt];
		}

		for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
			filter_others.sm_other_active[ev_cnt] =
				data->define_filter_others[defined_index].sm_other_active[ev_cnt];
	} else {
		for (othr_cnt = 0; othr_cnt < BCM_EVT_FLT_OTHR_MAX; othr_cnt++) {
			filter_others.sm_other_type[othr_cnt] = sm_other_type[othr_cnt];
			filter_others.sm_other_mask[othr_cnt] = sm_other_mask[othr_cnt];
			filter_others.sm_other_value[othr_cnt] = sm_other_value[othr_cnt];
		}

		for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
			filter_others.sm_other_active[ev_cnt] = sm_other_active[ev_cnt];
	}

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_EVENT_FLT_OTHERS,
					BCM_EVT_SET, ip_range);

	ret = exynos_bcm_dbg_filter_others_ctrl(&ipc_base_info,
				&filter_others, bcm_ip_index, data);
	if (ret) {
		BCM_ERR("%s:failed set filter others\n", __func__);
		kfree(sm_other_active);
		return ret;
	}

	kfree(sm_other_active);
	return size;
}

static ssize_t show_get_sample_id(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	struct exynos_bcm_sample_id sample_id;
	ssize_t count = 0;
	int ret;
	static int ip_cnt = 0;

	if (ip_cnt >= data->bcm_ip_nr) {
		ip_cnt = 0;
		return 0;
	}

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_EVENT_SAMPLE_ID,
					BCM_EVT_GET, BCM_EACH);

	do {
		ret = exynos_bcm_dbg_sample_id_ctrl(&ipc_base_info,
						&sample_id, ip_cnt, data);
		if (ret) {
			BCM_ERR("%s: failed get sample id(ip:%d)\n",
					__func__, ip_cnt);
			ip_cnt = 0;
			return ret;
		}

		count += snprintf(buf + count, PAGE_SIZE,
				"bcm[%2d]: mask(0x%08x), id(0x%08x)\n",
				ip_cnt, sample_id.peak_mask, sample_id.peak_id);
		ip_cnt++;
	} while ((ip_cnt < data->bcm_ip_nr) && (ip_cnt % data->bcm_ip_print_nr));

	return count;
}

static ssize_t show_get_sample_id_active(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	struct exynos_bcm_sample_id sample_id;
	ssize_t count = 0;
	int ev_cnt, ret;
	static int ip_cnt = 0;

	if (ip_cnt >= data->bcm_ip_nr) {
		ip_cnt = 0;
		return 0;
	}

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_EVENT_SAMPLE_ID,
					BCM_EVT_GET, BCM_EACH);

	do {
		ret = exynos_bcm_dbg_sample_id_ctrl(&ipc_base_info,
						&sample_id, ip_cnt, data);
		if (ret) {
			BCM_ERR("%s: failed get sample id(ip:%d)\n",
					__func__, ip_cnt);
			ip_cnt = 0;
			return ret;
		}

		count += snprintf(buf + count, PAGE_SIZE, "bcm[%2d]:", ip_cnt);
		for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
			count += snprintf(buf + count, PAGE_SIZE,
					" ev%d %u,", ev_cnt,
					sample_id.peak_enable[ev_cnt]);
		count += snprintf(buf + count, PAGE_SIZE, "\n");
		ip_cnt++;
	} while ((ip_cnt < data->bcm_ip_nr) && (ip_cnt % data->bcm_ip_print_nr));

	return count;
}

static ssize_t show_sample_id_ctrl_help(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	ssize_t count = 0;

	if (off > 0)
		return 0;

	/* help store_sample_id_ctrl */
	count += snprintf(buf + count, PAGE_SIZE, "\n= sample_id_ctrl set help =\n");
	count += snprintf(buf + count, PAGE_SIZE, "Usage:\n");
	count += snprintf(buf + count, PAGE_SIZE,
			"echo [ip_range] [ip_index] [define_index] [mask] [id]	\
			[ev0] [ev1] [ev2] [ev3] [ev4] [ev5] [ev6] [ev7] >	\
			sample_id_ctrl\n");
	count += snprintf(buf + count, PAGE_SIZE,
			" ip_range: BCM_EACH(%d), BCM_ALL(%d)\n",
				BCM_EACH, BCM_ALL);
	count += snprintf(buf + count, PAGE_SIZE,
			" ip_index: number of bcm ip (0 ~ %u)\n"
			"           (if ip_range is all, set to 0)\n",
				data->bcm_ip_nr - 1);
	count += snprintf(buf + count, PAGE_SIZE,
			" define_index: index of pre-defined event (0 ~ %u)\n"
			"               0 means no pre-defined event\n",
				data->define_event_max - 1);
	count += snprintf(buf + count, PAGE_SIZE,
			" mask: masking for sample id	\
			(if define_index is not 0, set to 0)\n"
			"       mask value should be hexa value\n");
	count += snprintf(buf + count, PAGE_SIZE,
			" id: id of sample id	\
			(if define_index is not 0, set to 0)\n"
			"     id should be hexa value\n");
	count += snprintf(buf + count, PAGE_SIZE,
			" evX: event counter enable for sample id	\
			(if define_index is not 0, set to 0)\n"
			"      value should be 0 or 1\n");

	return count;
}

static ssize_t store_sample_id_ctrl(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	struct exynos_bcm_sample_id sample_id;
	unsigned int bcm_ip_index = 0;
	unsigned int ip_range = 0;
	unsigned int defined_index;
	unsigned int peak_mask, peak_id;
	unsigned int *peak_enable;
	int ev_cnt, ret;

	if (off != 0)
		return size;

	peak_enable = kzalloc(sizeof(int) * data->bcm_cnt_nr, GFP_KERNEL);
	if (peak_enable == NULL) {
		BCM_ERR("%s: faild allocated of peak_enable memory\n", __func__);
		return -ENOMEM;
	}

	if (data->bcm_cnt_nr == 4) {
		ret = sscanf(buf, "%u %u %u %x %x %u %u %u %u",
				&ip_range, &bcm_ip_index, &defined_index,
				&peak_mask, &peak_id, &peak_enable[0], &peak_enable[1],
				&peak_enable[2], &peak_enable[3]);
	} else if (data->bcm_cnt_nr == 8) {
		ret = sscanf(buf, "%u %u %u %x %x %u %u %u %u %u %u %u %u",
				&ip_range, &bcm_ip_index, &defined_index,
				&peak_mask, &peak_id, &peak_enable[0], &peak_enable[1],
				&peak_enable[2], &peak_enable[3],
				&peak_enable[4], &peak_enable[5],
				&peak_enable[6], &peak_enable[7]);
	} else {
		BCM_ERR("%s: invalid bcm_cnt_nr\n", __func__);
		kfree(peak_enable);
		return -EINVAL;
	}

	/* 5 --> the number of index */
	if (ret != data->bcm_cnt_nr + 5) {
		kfree(peak_enable);
		return -EINVAL;
	}

	ret = exynos_bcm_ip_validate(ip_range, bcm_ip_index, data->bcm_ip_nr);
	if (ret) {
		kfree(peak_enable);
		return ret;
	}

	for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++) {
		if (peak_enable[ev_cnt])
			peak_enable[ev_cnt] = true;
	}

	if (defined_index >= data->define_event_max) {
		BCM_ERR("%s: Invalid defined index(%u),"
			" defined_max_nr(%u)\n", __func__,
				defined_index, data->define_event_max - 1);
		kfree(peak_enable);
		return -EINVAL;
	}

	if (ip_range == BCM_ALL)
		bcm_ip_index = 0;

	if (defined_index != NO_PRE_DEFINE_EVT) {
		sample_id.peak_mask = data->define_sample_id[defined_index].peak_mask;
		sample_id.peak_id = data->define_sample_id[defined_index].peak_id;
		for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
			sample_id.peak_enable[ev_cnt] =
				data->define_sample_id[defined_index].peak_enable[ev_cnt];
	} else {
		sample_id.peak_mask = peak_mask;
		sample_id.peak_id = peak_id;
		for (ev_cnt = 0; ev_cnt < data->bcm_cnt_nr; ev_cnt++)
			sample_id.peak_enable[ev_cnt] = peak_enable[ev_cnt];
	}

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_EVENT_SAMPLE_ID,
					BCM_EVT_SET, ip_range);

	ret = exynos_bcm_dbg_sample_id_ctrl(&ipc_base_info, &sample_id,
						bcm_ip_index, data);
	if (ret) {
		BCM_ERR("%s:failed set sample ID\n", __func__);
		kfree(peak_enable);
		return ret;
	}

	kfree(peak_enable);
	return size;
}

static ssize_t show_get_run(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	unsigned int bcm_run;
	ssize_t count = 0;
	int ret;

	if (off > 0)
		return 0;

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_RUN_CONT,
					BCM_EVT_GET, 0);

	ret = exynos_bcm_dbg_run_ctrl(&ipc_base_info, &bcm_run, data);
	if (ret) {
		count += snprintf(buf + count, PAGE_SIZE,
					"failed get run state\n");
		return count;
	}

	count += snprintf(buf + count, PAGE_SIZE,
				"run state: raw state(%s), sw state(%s)\n",
				bcm_run ? "run" : "stop",
				data->bcm_run_state ? "run" : "stop");

	return count;
}

static ssize_t show_run_ctrl_help(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	ssize_t count = 0;

	if (off > 0)
		return 0;

	/* help store_run_ctrl */
	count += snprintf(buf + count, PAGE_SIZE, "\n= run_ctrl set help =\n");
	count += snprintf(buf + count, PAGE_SIZE, "Usage:\n");
	count += snprintf(buf + count, PAGE_SIZE, "echo [run_state] > run_ctrl\n");
	count += snprintf(buf + count, PAGE_SIZE,
			" run_state: BCM_RUN(%d), BCM_STOP(%d)\n",
				BCM_RUN, BCM_STOP);

	return count;
}

static ssize_t store_run_ctrl(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	unsigned int bcm_run;
	int ret;

	if (off != 0)
		return size;

	ret = kstrtouint(buf, 0, &bcm_run);
	if (ret)
		return ret;

	if (!(bcm_run == 0 || bcm_run == 1)) {
		BCM_ERR("%s: invalid parameter (%u)\n", __func__, bcm_run);
		return -EINVAL;
	}

	if (bcm_run)
		bcm_run = true;

	ret = exynos_bcm_dbg_run(bcm_run, data);

	if (ret) {
		BCM_ERR("%s:failed set Run state\n", __func__);
		return ret;
	}

	return size;
}

static ssize_t show_get_period(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	unsigned int period;
	ssize_t count = 0;
	int ret;

	if (off > 0)
		return 0;

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_PERIOD_CONT,
					BCM_EVT_GET, 0);

	ret = exynos_bcm_dbg_period_ctrl(&ipc_base_info, &period, data);
	if (ret) {
		count += snprintf(buf + count, PAGE_SIZE,
					"failed get period\n");
		return count;
	}

	count += snprintf(buf + count, PAGE_SIZE,
				"monitor period: %u msec\n", period);

	return count;
}

static ssize_t show_period_ctrl_help(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	ssize_t count = 0;

	if (off > 0)
		return 0;

	/* help store_period_ctrl */
	count += snprintf(buf + count, PAGE_SIZE, "\n= period_ctrl set help =\n");
	count += snprintf(buf + count, PAGE_SIZE, "Usage:\n");
	count += snprintf(buf + count, PAGE_SIZE, "echo [period] > period_ctrl\n");
	count += snprintf(buf + count, PAGE_SIZE,
			" period: monitor period (unit: msec),	\
			min(%d msec) ~ max(%d msec)\n",
			BCM_TIMER_PERIOD_MIN, BCM_TIMER_PERIOD_MAX);

	return count;
}

static ssize_t store_period_ctrl(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	unsigned int period;
	int ret;

	if (off != 0)
		return size;

	ret = kstrtouint(buf, 0, &period);
	if (ret)
		return ret;

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_PERIOD_CONT,
					BCM_EVT_SET, 0);

	ret = exynos_bcm_dbg_period_ctrl(&ipc_base_info, &period, data);
	if (ret) {
		BCM_ERR("%s:failed set period\n", __func__);
		return ret;
	}

	return size;
}

static ssize_t show_get_mode(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	unsigned int bcm_mode;
	ssize_t count = 0;
	int ret;

	if (off > 0)
		return 0;

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_MODE_CONT,
					BCM_EVT_GET, 0);

	ret = exynos_bcm_dbg_mode_ctrl(&ipc_base_info, &bcm_mode, data);
	if (ret) {
		count += snprintf(buf + count, PAGE_SIZE,
					"failed get mode\n");
		return count;
	}

	count += snprintf(buf + count, PAGE_SIZE,
			"mode: %d (%d:Interval, %d:Once, %d:User_ctrl)\n",
			bcm_mode,
			BCM_MODE_INTERVAL, BCM_MODE_ONCE, BCM_MODE_USERCTRL);

	return count;
}

static ssize_t show_mode_ctrl_help(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	ssize_t count = 0;

	if (off > 0)
		return 0;

	/* help store_mode_ctrl */
	count += snprintf(buf + count, PAGE_SIZE, "\n= mode_ctrl set help =\n");
	count += snprintf(buf + count, PAGE_SIZE, "Usage:\n");
	count += snprintf(buf + count, PAGE_SIZE, "echo [mode] > mode_ctrl\n");
	count += snprintf(buf + count, PAGE_SIZE,
			" mode: Interval(%d), Once(%d), User_ctrl(%d)\n",
			BCM_MODE_INTERVAL, BCM_MODE_ONCE, BCM_MODE_USERCTRL);

	return count;
}

static ssize_t store_mode_ctrl(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	unsigned int bcm_mode;
	int ret;

	if (off != 0)
		return size;

	ret = kstrtouint(buf, 0, &bcm_mode);
	if (ret)
		return ret;

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_MODE_CONT,
					BCM_EVT_SET, 0);

	ret = exynos_bcm_dbg_mode_ctrl(&ipc_base_info, &bcm_mode, data);
	if (ret) {
		BCM_ERR("%s:failed set mode\n", __func__);
		return ret;
	}

	return size;
}

static ssize_t show_get_glbauto(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	unsigned int glb_en;
	ssize_t count = 0;
	int ret;

	if (off > 0)
		return 0;

	if (!data->glb_auto_en) {
		BCM_ERR("%s: don't support glb_auto_en\n", __func__);
		return -ENODEV;
	}

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_GLBAUTO_CONT,
					BCM_EVT_GET, 0);

	ret = exynos_bcm_dbg_glb_auto_ctrl(&ipc_base_info, &glb_en, data);
	if (ret) {
		count += snprintf(buf + count, PAGE_SIZE,
					"failed get glb_en\n");
		return count;
	}

	count += snprintf(buf + count, PAGE_SIZE, ": %d\n", glb_en);

	return count;
}

static ssize_t show_glbauto_ctrl_help(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	ssize_t count = 0;

	if (off > 0)
		return 0;

	/* help store_glbauto_ctrl */
	count += snprintf(buf + count, PAGE_SIZE, "\n= glbauto_ctrl set help =\n");
	count += snprintf(buf + count, PAGE_SIZE, "Usage:\n");
	count += snprintf(buf + count, PAGE_SIZE, "echo [en] > glbauto_ctrl\n");

	return count;
}

static ssize_t store_glbauto_ctrl(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	unsigned int glb_en;
	int ret;

	if (off != 0)
		return size;

	ret = kstrtouint(buf, 0, &glb_en);
	if (ret)
		return ret;

	if (!data->glb_auto_en) {
		BCM_ERR("%s: don't support glb_auto_en\n", __func__);
		return -ENODEV;
	}

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_GLBAUTO_CONT,
					BCM_EVT_SET, 0);

	ret = exynos_bcm_dbg_glb_auto_ctrl(&ipc_base_info, &glb_en, data);
	if (ret) {
		BCM_ERR("%s:failed set glb_en\n", __func__);
		return ret;
	}

	return size;
}

static ssize_t show_get_str(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	unsigned int suspend;
	ssize_t count = 0;
	int ret;

	if (off > 0)
		return 0;

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_STR_STATE,
					BCM_EVT_GET, 0);

	ret = exynos_bcm_dbg_str_ctrl(&ipc_base_info, &suspend, data);
	if (ret) {
		count += snprintf(buf + count, PAGE_SIZE,
					"failed get str state\n");
		return count;
	}

	count += snprintf(buf + count, PAGE_SIZE,
			"str state: %s\n", suspend ? "suspend" : "resume");

	return count;
}

static ssize_t show_str_ctrl_help(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	ssize_t count = 0;

	if (off > 0)
		return 0;

	/* help store_str_ctrl */
	count += snprintf(buf + count, PAGE_SIZE, "\n= str_ctrl set help =\n");
	count += snprintf(buf + count, PAGE_SIZE, "Usage:\n");
	count += snprintf(buf + count, PAGE_SIZE, "echo [str_state] > str_ctrl\n");
	count += snprintf(buf + count, PAGE_SIZE,
			" str_state: suspend(1), resume(0)\n");

	return count;
}

static ssize_t store_str_ctrl(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	unsigned int suspend;
	int ret;

	if (off != 0)
		return size;

	ret = kstrtouint(buf, 0, &suspend);
	if (ret)
		return ret;

	if (suspend)
		suspend = true;

	ret = exynos_bcm_dbg_str(suspend, data);
	if (ret) {
		BCM_ERR("%s:failed set str state\n", __func__);
		return ret;
	}

	return size;
}

static ssize_t show_get_ip(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	unsigned int ip_enable;
	ssize_t count = 0;
	int ret;
	static int ip_cnt = 0;

	if (ip_cnt >= data->bcm_ip_nr) {
		ip_cnt = 0;
		return 0;
	}

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_IP_CONT,
					BCM_EVT_GET, BCM_EACH);

	do {
		ret = exynos_bcm_dbg_ip_ctrl(&ipc_base_info,
						&ip_enable, ip_cnt, data);
		if (ret) {
			BCM_ERR("%s: failed get ip_enable state(ip:%d)\n",
					__func__, ip_cnt);
			ip_cnt = 0;
			return ret;
		}

		count += snprintf(buf + count, PAGE_SIZE, "bcm[%2d]: enabled (%s)\n",
					ip_cnt, ip_enable ? "true" : "false");
		ip_cnt++;
	} while ((ip_cnt < data->bcm_ip_nr) && (ip_cnt % data->bcm_ip_print_nr));

	return count;
}

static ssize_t show_ip_ctrl_help(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	ssize_t count = 0;

	if (off > 0)
		return 0;

	/* help store_ip_ctrl */
	count += snprintf(buf + count, PAGE_SIZE, "\n= ip_ctrl set help =\n");
	count += snprintf(buf + count, PAGE_SIZE, "Usage:\n");
	count += snprintf(buf + count, PAGE_SIZE,
			"echo [ip_index] [enable] > ip_ctrl\n");
	count += snprintf(buf + count, PAGE_SIZE,
			" ip_index: number of bcm ip (0 ~ %u)\n",
				data->bcm_ip_nr - 1);
	count += snprintf(buf + count, PAGE_SIZE,
			" enable: ip enable state (1:enable, 0:disable)\n");

	return count;
}

static ssize_t store_ip_ctrl(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	unsigned int bcm_ip_index, ip_enable;
	int ret;

	if (off != 0)
		return size;

	ret = sscanf(buf, "%u %u", &bcm_ip_index, &ip_enable);
	if (ret != 2)
		return -EINVAL;

	ret = exynos_bcm_ip_validate(BCM_EACH, bcm_ip_index, data->bcm_ip_nr);
	if (ret)
		return ret;

	if (ip_enable)
		ip_enable = true;

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_IP_CONT,
					BCM_EVT_SET, BCM_EACH);

	ret = exynos_bcm_dbg_ip_ctrl(&ipc_base_info, &ip_enable,
						bcm_ip_index, data);
	if (ret) {
		BCM_ERR("%s:failed set IP control\n", __func__);
		return ret;
	}

	return size;
}

static int exynos_bcm_dbg_set_dump_info(struct exynos_bcm_dbg_data *data)
{
	struct exynos_bcm_ipc_base_info ipc_base_info;
	int ret;

	if (data->dump_addr.buff_size == 0 ||
		data->dump_addr.buff_size > data->dump_addr.p_size)
		data->dump_addr.buff_size = data->dump_addr.p_size;

	BCM_INFO("%s: buffer size for reserved memory: buff_size = 0x%x\n",
			__func__, data->dump_addr.buff_size);

	if (!data->rmem_acquired) {
		BCM_INFO("%s: BCM will not save dump data\n", __func__);
		return 0;
	}

	/* send physical address info to BCM plugin */
	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_DUMP_ADDR,
					BCM_EVT_SET, 0);

	ret = exynos_bcm_dbg_dump_addr_ctrl(&ipc_base_info, &data->dump_addr, data);
	if (ret) {
		BCM_ERR("%s: failed set dump address info\n", __func__);
		return ret;
	}

	return 0;
}

static ssize_t show_dump_addr_info(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	ssize_t count = 0;

	if (off > 0)
		return 0;

	count += snprintf(buf + count, PAGE_SIZE, "\n= BCM dump address info =\n");
	count += snprintf(buf + count, PAGE_SIZE,
			"physical address = 0x%08x\n", data->dump_addr.p_addr);
	count += snprintf(buf + count, PAGE_SIZE,
			"virtual address = 0x%p\n", data->dump_addr.v_addr);
	count += snprintf(buf + count, PAGE_SIZE,
			"dump region size = 0x%08x\n", data->dump_addr.p_size);
	count += snprintf(buf + count, PAGE_SIZE,
			"actual use size = 0x%08x\n", data->dump_addr.buff_size);

	return count;
}

static ssize_t store_dump_addr_info(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	unsigned int buff_size;
	int ret;

	if (off != 0)
		return size;

	ret = kstrtouint(buf, 16, &buff_size);
	if (ret)
		return ret;

	if (buff_size <= EXYNOS_BCM_KTIME_SIZE) {
		BCM_ERR("%s: failed set dump info\n", __func__);
		return size;
	}

	data->dump_addr.buff_size = buff_size;

	ret = exynos_bcm_dbg_set_dump_info(data);
	if (ret) {
		BCM_ERR("%s: failed set dump info\n", __func__);
		return ret;
	}

	return size;
}

static ssize_t show_enable_dump_klog(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	ssize_t count = 0;

	if (off > 0)
		return 0;

	count += snprintf(buf + count, PAGE_SIZE, "\n= BCM dump to kernel log =\n");
	count += snprintf(buf + count, PAGE_SIZE, "%s\n",
				data->dump_klog ? "enabled" : "disabled");

	return count;
}

static ssize_t store_enable_dump_klog(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	unsigned int enable;
	int ret;

	if (off != 0)
		return size;

	ret = kstrtouint(buf, 0, &enable);
	if (ret)
		return ret;

	if (enable)
		data->dump_klog = true;
	else
		data->dump_klog = false;

	return size;
}

static ssize_t show_enable_stop_owner(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	ssize_t count = 0;
	int i;

	if (off > 0)
		return 0;

	count += snprintf(buf + count, PAGE_SIZE, "\n= BCM Available stop owner =\n");
	for (i = 0; i < STOP_OWNER_MAX; i++)
		count += snprintf(buf + count, PAGE_SIZE, " stop owner[%d]: %s\n",
				i, data->available_stop_owner[i] ? "true" : "false");

	return count;
}

static ssize_t store_enable_stop_owner(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	unsigned int owner_index, enable;
	int ret;

	if (off != 0)
		return size;

	ret = sscanf(buf, "%u %u", &owner_index, &enable);
	if (ret != 2)
		return -EINVAL;

	if (owner_index >= STOP_OWNER_MAX) {
		BCM_ERR("Invalid stop owner (%u)\n", owner_index);
		return -EINVAL;
	}

	if (enable)
		data->available_stop_owner[owner_index] = true;
	else
		data->available_stop_owner[owner_index] = false;

	return size;
}

static ssize_t show_bcm_calc(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_calc *bcm_calc = data->bcm_calc;
	ssize_t count = 0;
	int i;

	mutex_lock(&bcm_calc->lock);
	if (!bcm_calc->enable) {
		count += snprintf(buf + count, PAGE_SIZE, "ppmu_work not working!!\n");
		mutex_unlock(&bcm_calc->lock);
		return count;
	}

	cancel_delayed_work(&bcm_calc->work);

	// accmulate latest info
	exynos_bcm_find_dump_data(data);

	count += snprintf(buf + count, PAGE_SIZE, "ip_name, time, ccnt, pmcnt0, pmcnt1, pmcnt2, pmcnt3, pmcnt4, pmcnt5, pmcnt6, pmcnt7\n");

	for (i = 0; i < bcm_calc->num_ip; i++)
		count += snprintf(buf + count, PAGE_SIZE, "%s, %llu, %llu, %llu, %llu, %llu, %llu, %llu, %llu, %llu, %llu\n",
				bcm_calc->ip_name[i], bcm_calc->acc_data[i].dump_time, bcm_calc->acc_data[i].ccnt,
				bcm_calc->acc_data[i].pmcnt[0], bcm_calc->acc_data[i].pmcnt[1],
				bcm_calc->acc_data[i].pmcnt[2], bcm_calc->acc_data[i].pmcnt[3],
				bcm_calc->acc_data[i].pmcnt[4], bcm_calc->acc_data[i].pmcnt[5],
				bcm_calc->acc_data[i].pmcnt[6], bcm_calc->acc_data[i].pmcnt[7]);

	schedule_delayed_work(&bcm_calc->work, msecs_to_jiffies(bcm_calc->sample_time));
	mutex_unlock(&bcm_calc->lock);

	return count;
}

static ssize_t store_bcm_calc(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int enable;
	int ret;

	ret = sscanf(buf, "%u", &enable);

	if (ret != 1)
		return -EINVAL;

	exynos_bcm_calc_enable(enable);

	return count;
}

static ssize_t show_bcm_bw(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	ssize_t count = 0;
	int i;

	if (!bcm_bw) {
		count += snprintf(buf + count, PAGE_SIZE, "retry after setting bcm_bw data\n");
		return count;
	}

	exynos_bcm_get_req_data(data);

	count += snprintf(buf + count, PAGE_SIZE, "seq_no , ip_idx, time, ccnt, read BW, write BW\n");

	mutex_lock(&bcm_bw_lock);
	for (i = 0; i < bcm_bw->num_ip; i++)
		count += snprintf(buf + count, PAGE_SIZE, "%u, %u, %llu, %llu, %llu, %llu\n",
				bcm_bw->bw_data[i].seq_no,
				bcm_bw->ip_idx[i],
				bcm_bw->bw_data[i].dump_time, bcm_bw->bw_data[i].ccnt,
				bcm_bw->bw_data[i].pmcnt[0], bcm_bw->bw_data[i].pmcnt[4]);
	mutex_unlock(&bcm_bw_lock);

	return count;
}

static ssize_t store_bcm_bw(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int enable, measure_time, num_ip;
	unsigned int ip_idx[10];
	int i, ret, sample_cnt;

	ret = sscanf(buf, "%u %u %u %u %u %u %u %u %u %u %u %u %u",
			&enable, &measure_time, &num_ip,
			&ip_idx[0], &ip_idx[1], &ip_idx[2], &ip_idx[3],
			&ip_idx[4], &ip_idx[5], &ip_idx[6], &ip_idx[7],
			&ip_idx[8], &ip_idx[9]);

	if (num_ip > 10) {
		pr_info("num_ip should not be bigger than 10 (yours: %d)\n",
			num_ip);
		return count;
	}

	sample_cnt = exynos_bcm_get_sample_cnt(bcm_dbg_data);

	if (sample_cnt < measure_time) {
		pr_info("Failed to set: sample_cnt:%d, measure_time: %d\n",
			sample_cnt, measure_time);
		return count;
	}

	mutex_lock(&bcm_bw_lock);
	if (enable) {
		if (bcm_bw) {
			pr_info("bcm_bw was already started\n");
			mutex_unlock(&bcm_bw_lock);
			return count;
		}

		bcm_bw = kzalloc(sizeof(struct exynos_bcm_bw), GFP_KERNEL);
		bcm_bw->ip_idx = kcalloc(num_ip, sizeof(unsigned int), GFP_KERNEL);
		bcm_bw->bw_data = kcalloc(num_ip, sizeof(struct exynos_bcm_bw_data), GFP_KERNEL);
		bcm_bw->measure_time = measure_time;
		bcm_bw->num_ip = num_ip;

		for (i = 0; i < num_ip; i++)
			bcm_bw->ip_idx[i] = ip_idx[i];
	} else {
		if (bcm_bw->bw_data)
			kfree(bcm_bw->bw_data);

		if (bcm_bw->ip_idx)
			kfree(bcm_bw->ip_idx);

		if (bcm_bw)
			kfree(bcm_bw);
	}
	mutex_unlock(&bcm_bw_lock);

	return count;
}

static ssize_t show_histogram_27c_id(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	ssize_t count = 0;
	int i, ret, bcm_ip_index;
	unsigned int id0, id1;

	if (off > 0)
		return 0;

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_HISTOGRAM_ID,
					BCM_EVT_GET, BCM_EACH);

	for (i = 0; i < 4; i++) {
		bcm_ip_index = BCM_IP_D0_INDEX + i;
		ret = exynos_bcm_dbg_histogram_id(&ipc_base_info, &id0, &id1,
						bcm_ip_index, data);
		if (ret) {
			BCM_ERR("%s: failed get histogram_id(%d)\n",
					__func__, i);
			return ret;
		}
		count += snprintf(buf + count, PAGE_SIZE,
				"csis_d%d id0 = 0x%08X, id1 = 0x%08X\n", i, id0, id1);
	}

	return count;
}

static ssize_t store_histogram_27c_id(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	unsigned int histogram_27c_index, bcm_ip_index, id0, id1;
	int ret;

	if (off != 0)
		return size;

	ret = sscanf(buf, "%u %x %x", &histogram_27c_index, &id0, &id1);
	if (ret != 3 || histogram_27c_index >= 4)
		return -EINVAL;

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_HISTOGRAM_ID,
					BCM_EVT_SET, BCM_EACH);

	bcm_ip_index = BCM_IP_D0_INDEX + histogram_27c_index;
	ret = exynos_bcm_dbg_histogram_id(&ipc_base_info, &id0, &id1,
						bcm_ip_index, data);
	if (ret) {
		BCM_ERR("%s:failed set histogram config\n", __func__);
		return ret;
	}

	return size;
}

static ssize_t show_histogram_27c_config(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	ssize_t count = 0;
	int i, ret, bcm_ip_index;
	unsigned int config;

	if (off > 0)
		return 0;

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_HISTOGRAM_CONFIG,
					BCM_EVT_GET, BCM_EACH);

	for (i = 0; i < 4; i++) {
		bcm_ip_index = BCM_IP_D0_INDEX + i;
		ret = exynos_bcm_dbg_histogram_config(&ipc_base_info,
						&config, bcm_ip_index, data);
		if (ret) {
			BCM_ERR("%s: failed get histogram_config(%d)\n",
					__func__, i);
			return ret;
		}
		count += snprintf(buf + count, PAGE_SIZE,
				"csis_d%d config = 0x%08X\n", i, config);
	}

	return count;
}

static ssize_t store_histogram_27c_config(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	unsigned int histogram_27c_index, bcm_ip_index, config;
	int ret;

	if (off != 0)
		return size;

	ret = sscanf(buf, "%u %x", &histogram_27c_index, &config);
	if (ret != 2 || histogram_27c_index >= 4)
		return -EINVAL;

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_HISTOGRAM_CONFIG,
					BCM_EVT_SET, BCM_EACH);

	bcm_ip_index = BCM_IP_D0_INDEX + histogram_27c_index;
	ret = exynos_bcm_dbg_histogram_config(&ipc_base_info, &config,
						bcm_ip_index, data);
	if (ret) {
		BCM_ERR("%s:failed set histogram config\n", __func__);
		return ret;
	}

	return size;
}

static ssize_t show_histogram_27d_config(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	ssize_t count = 0;
	int i, ret, bcm_ip_index;
	unsigned int config;

	if (off > 0)
		return 0;

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_HISTOGRAM_CONFIG,
					BCM_EVT_GET, BCM_EACH);

	for (i = 0; i < 4; i++) {
		bcm_ip_index = BCM_IP_DPUF0D0_INDEX + i;
		ret = exynos_bcm_dbg_histogram_config(&ipc_base_info,
						&config, bcm_ip_index, data);
		if (ret) {
			BCM_ERR("%s: failed get histogram_config(%d)\n",
					__func__, i);
			return ret;
		}
		count += snprintf(buf + count, PAGE_SIZE,
				"dpuf%ud%u config = 0x%08X\n", i / 2, i & 0x1, config);
	}

	return count;
}

static ssize_t store_histogram_27d_config(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	struct exynos_bcm_ipc_base_info ipc_base_info;
	unsigned int histogram_27d_index, bcm_ip_index, config;
	int ret;

	if (off != 0)
		return size;

	ret = sscanf(buf, "%u %x", &histogram_27d_index, &config);
	if (ret != 2 || histogram_27d_index >= 4)
		return -EINVAL;

	exynos_bcm_dbg_set_base_info(&ipc_base_info, BCM_EVT_HISTOGRAM_CONFIG,
					BCM_EVT_SET, BCM_EACH);

	bcm_ip_index = BCM_IP_DPUF0D0_INDEX + histogram_27d_index;
	ret = exynos_bcm_dbg_histogram_config(&ipc_base_info, &config,
						bcm_ip_index, data);
	if (ret) {
		BCM_ERR("%s:failed set histogram config\n", __func__);
		return ret;
	}

	return size;
}

static ssize_t show_bcm_dbg_show_mif_auto(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct exynos_bcm_show_bw *bcm_show_bw = bcm_dbg_data->bcm_show_bw;
	ssize_t count = 0;
	int i;
	u64 temp, dump_time_total = 0;
	unsigned int num_sample_local;

	if (off > 0)
		return 0;

	if (!bcm_show_bw->mem_bw) {
		count += snprintf(buf + count, PAGE_SIZE, "There is not bw data\n");
		return count;
	}

	if (bcm_show_bw->enable_ctrl)
		num_sample_local = 1;
	else
		num_sample_local = num_sample;


	for (i = 0; i < num_sample_local; i++) {
		if (i)
			temp = bcm_show_bw->mem_bw[i] -
				bcm_show_bw->mem_bw[i - 1];
		else
			temp = bcm_show_bw->mem_bw[i];

		count += snprintf(buf + count, PAGE_SIZE, "[%3d] mem_bw: %llu MB/sec\n",
				  i, temp  * (1<<10) / bcm_show_bw->dump_time[i]);
	}

	for (i=0; i < num_sample_local; i++)
		dump_time_total += bcm_show_bw->dump_time[i];

	/* just for preventing divide by zero */
	dump_time_total = dump_time_total == 0 ? 1 : dump_time_total;

	count += snprintf(buf + count, PAGE_SIZE,
				"Total Profile time is %llu msec, Total MIF BW: %llu MB/sec\n",
				dump_time_total,
				bcm_show_bw->mem_bw[num_sample_local - 1] * (1<<10) / dump_time_total);

	return count;
}

static ssize_t store_bcm_dbg_show_mif_auto(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct exynos_bcm_show_bw *bcm_show_bw = bcm_dbg_data->bcm_show_bw;
	struct exynos_bcm_calc *bcm_calc = bcm_dbg_data->bcm_calc;
	int ret;

	if (off != 0)
		return size;

	/* msec */
	ret = sscanf(buf, "%u", &num_sample);

	exynos_bcm_calc_enable(1);

	mutex_lock(&bcm_show_bw->lock);
	bcm_show_bw->num_sample = num_sample;

	if (bcm_show_bw->mem_bw)
		kfree(bcm_show_bw->mem_bw);
	if (bcm_show_bw->dump_time)
		kfree(bcm_show_bw->dump_time);

	bcm_show_bw->mem_bw = kzalloc(sizeof(u64) * num_sample, GFP_KERNEL);
	bcm_show_bw->dump_time = kzalloc(sizeof(u64) * num_sample, GFP_KERNEL);
	bcm_show_bw->new_time = sched_clock();

	schedule_delayed_work(&bcm_show_bw->bw_work,
			msecs_to_jiffies(bcm_calc->sample_time));
	mutex_unlock(&bcm_show_bw->lock);

	return size;
}

/* this is for ftrace */
static ssize_t store_bcm_dbg_show_mif_ctrl(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct exynos_bcm_show_bw *bcm_show_bw = bcm_dbg_data->bcm_show_bw;
	struct exynos_bcm_calc *bcm_calc = bcm_dbg_data->bcm_calc;
	int enable;
	int ret;

	if (off != 0)
		return size;

	/* msec */
	ret = sscanf(buf, "%u", &enable);
	enable = !!enable;

	if (bcm_show_bw->enable_ctrl == enable) {
		BCM_ERR("%s: Your input was already applied\n", __func__);
		return size;
	}

	bcm_show_bw->enable_ctrl = enable;

	if (enable) {
		exynos_bcm_calc_enable(enable);

		mutex_lock(&bcm_show_bw->lock);
		if (!bcm_show_bw->mem_bw)
			bcm_show_bw->mem_bw = kzalloc(sizeof(u64), GFP_KERNEL);

		schedule_delayed_work(&bcm_show_bw->bw_work,
				msecs_to_jiffies(bcm_calc->sample_time));
		mutex_unlock(&bcm_show_bw->lock);
	} else {
		exynos_bcm_calc_enable(enable);

		mutex_lock(&bcm_show_bw->lock);
		cancel_delayed_work(&bcm_show_bw->bw_work);
		mutex_unlock(&bcm_show_bw->lock);
	}

	return size;
}

#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
static ssize_t show_bcm_dbg_load_bin(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_bcm_dbg_data *data = platform_get_drvdata(pdev);
	ssize_t count = 0;

	if (off > 0)
		return 0;

	count += snprintf(buf + count, PAGE_SIZE, "\n= BCM Load Bin =\n");
	count += snprintf(buf + count, PAGE_SIZE, " bcm load bin: %s\n",
				data->bcm_load_bin ? "true" : "false");

	return count;
}

static ssize_t store_bcm_dbg_load_bin(struct file *fp, struct kobject *kobj,
		struct bin_attribute *battr, char *buf, loff_t off, size_t size)
{
	unsigned int load;
	int ret;

	if (off != 0)
		return size;

	ret = kstrtouint(buf, 0, &load);
	if (ret)
		return ret;

	/* loading firmware */
	if (load) {
		ret = exynos_bcm_dbg_load_bin();
		if (ret) {
			BCM_ERR("%s: failed to load BCM bin\n", __func__);
			return ret;
		}
	}

	return size;
}
#endif

static BIN_ATTR(bcm_dbg_data_pd, 0440, show_bcm_dbg_data_pd, NULL, 0);
static BIN_ATTR(bcm_dbg_data_df_event, 0440,
			show_bcm_dbg_data_df_event, NULL, 0);
static BIN_ATTR(bcm_dbg_data_df_filter, 0440,
			show_bcm_dbg_data_df_filter, NULL, 0);
static BIN_ATTR(bcm_dbg_data_df_sample, 0440,
			show_bcm_dbg_data_df_sample, NULL, 0);
static BIN_ATTR(bcm_dbg_data_df_attr, 0440,
			show_bcm_dbg_data_df_attr, NULL, 0);
static BIN_ATTR(get_event, 0440, show_get_event, NULL, 0);
static BIN_ATTR(event_ctrl_help, 0440, show_event_ctrl_help, NULL, 0);
static BIN_ATTR(event_ctrl, 0640, NULL, store_event_ctrl, 0);
static BIN_ATTR(get_filter_id, 0440, show_get_filter_id, NULL, 0);
static BIN_ATTR(get_filter_id_active, 0440,
			show_get_filter_id_active, NULL, 0);
static BIN_ATTR(filter_id_ctrl_help, 0440,
			show_filter_id_ctrl_help, NULL, 0);
static BIN_ATTR(filter_id_ctrl, 0640, NULL, store_filter_id_ctrl, 0);
static BIN_ATTR(get_filter_others, 0440, show_get_filter_others, NULL, 0);
static BIN_ATTR(get_filter_others_active, 0440,
			show_get_filter_others_active, NULL, 0);
static BIN_ATTR(filter_others_ctrl_help, 0440,
			show_filter_others_ctrl_help, NULL, 0);
static BIN_ATTR(filter_others_ctrl, 0640, NULL, store_filter_others_ctrl, 0);
static BIN_ATTR(get_sample_id, 0440, show_get_sample_id, NULL, 0);
static BIN_ATTR(get_sample_id_active, 0440,
			show_get_sample_id_active, NULL, 0);
static BIN_ATTR(sample_id_ctrl_help, 0440,
			show_sample_id_ctrl_help, NULL, 0);
static BIN_ATTR(sample_id_ctrl, 0640, NULL, store_sample_id_ctrl, 0);
static BIN_ATTR(get_run, 0440, show_get_run, NULL, 0);
static BIN_ATTR(run_ctrl_help, 0440, show_run_ctrl_help, NULL, 0);
static BIN_ATTR(run_ctrl, 0640, NULL, store_run_ctrl, 0);
static BIN_ATTR(get_period, 0440, show_get_period, NULL, 0);
static BIN_ATTR(period_ctrl_help, 0440, show_period_ctrl_help, NULL, 0);
static BIN_ATTR(period_ctrl, 0640, NULL, store_period_ctrl, 0);
static BIN_ATTR(get_mode, 0440, show_get_mode, NULL, 0);
static BIN_ATTR(mode_ctrl_help, 0440, show_mode_ctrl_help, NULL, 0);
static BIN_ATTR(mode_ctrl, 0640, NULL, store_mode_ctrl, 0);
static BIN_ATTR(get_glbauto, 0440, show_get_glbauto, NULL, 0);
static BIN_ATTR(glbauto_ctrl_help, 0440, show_glbauto_ctrl_help, NULL, 0);
static BIN_ATTR(glbauto_ctrl, 0640, NULL, store_glbauto_ctrl, 0);
static BIN_ATTR(get_str, 0440, show_get_str, NULL, 0);
static BIN_ATTR(str_ctrl_help, 0440, show_str_ctrl_help, NULL, 0);
static BIN_ATTR(str_ctrl, 0640, NULL, store_str_ctrl, 0);
static BIN_ATTR(get_ip, 0440, show_get_ip, NULL, 0);
static BIN_ATTR(ip_ctrl_help, 0440, show_ip_ctrl_help, NULL, 0);
static BIN_ATTR(ip_ctrl, 0640, NULL, store_ip_ctrl, 0);
static BIN_ATTR(dump_addr_info, 0640, show_dump_addr_info, store_dump_addr_info, 0);
static BIN_ATTR(enable_dump_klog, 0640, show_enable_dump_klog, store_enable_dump_klog, 0);
static BIN_ATTR(enable_stop_owner, 0640, show_enable_stop_owner, store_enable_stop_owner, 0);

static BIN_ATTR(histogram_27c_id, 0640, show_histogram_27c_id, store_histogram_27c_id, 0);
static BIN_ATTR(histogram_27c_config, 0640, show_histogram_27c_config, store_histogram_27c_config, 0);
static BIN_ATTR(histogram_27d_config, 0640, show_histogram_27d_config, store_histogram_27d_config, 0);
static BIN_ATTR(bcm_dbg_show_mif_auto, 0640, show_bcm_dbg_show_mif_auto, store_bcm_dbg_show_mif_auto, 0);
static BIN_ATTR(bcm_dbg_show_mif_ctrl, 0640, NULL, store_bcm_dbg_show_mif_ctrl, 0);
#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
static BIN_ATTR(bcm_dbg_load_bin, 0640, show_bcm_dbg_load_bin, store_bcm_dbg_load_bin, 0);
#endif

static struct bin_attribute *exynos_bcm_dbg_sysfs_entries[] = {
	&bin_attr_bcm_dbg_data_pd,
	&bin_attr_bcm_dbg_data_df_event,
	&bin_attr_bcm_dbg_data_df_filter,
	&bin_attr_bcm_dbg_data_df_sample,
	&bin_attr_bcm_dbg_data_df_attr,
	&bin_attr_get_event,
	&bin_attr_event_ctrl_help,
	&bin_attr_event_ctrl,
	&bin_attr_get_filter_id,
	&bin_attr_get_filter_id_active,
	&bin_attr_filter_id_ctrl_help,
	&bin_attr_filter_id_ctrl,
	&bin_attr_get_filter_others,
	&bin_attr_get_filter_others_active,
	&bin_attr_filter_others_ctrl_help,
	&bin_attr_filter_others_ctrl,
	&bin_attr_get_sample_id,
	&bin_attr_get_sample_id_active,
	&bin_attr_sample_id_ctrl_help,
	&bin_attr_sample_id_ctrl,
	&bin_attr_get_run,
	&bin_attr_run_ctrl_help,
	&bin_attr_run_ctrl,
	&bin_attr_get_period,
	&bin_attr_period_ctrl_help,
	&bin_attr_period_ctrl,
	&bin_attr_get_mode,
	&bin_attr_mode_ctrl_help,
	&bin_attr_mode_ctrl,
	&bin_attr_get_glbauto,
	&bin_attr_glbauto_ctrl_help,
	&bin_attr_glbauto_ctrl,
	&bin_attr_get_str,
	&bin_attr_str_ctrl_help,
	&bin_attr_str_ctrl,
	&bin_attr_get_ip,
	&bin_attr_ip_ctrl_help,
	&bin_attr_ip_ctrl,
	&bin_attr_dump_addr_info,
	&bin_attr_enable_dump_klog,
	&bin_attr_enable_stop_owner,
	&bin_attr_histogram_27c_id,
	&bin_attr_histogram_27c_config,
	&bin_attr_histogram_27d_config,
	&bin_attr_bcm_dbg_show_mif_auto,
	&bin_attr_bcm_dbg_show_mif_ctrl,
#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
	&bin_attr_bcm_dbg_load_bin,
#endif
	NULL,
};

static DEVICE_ATTR(bcm_calc, 0660, show_bcm_calc, store_bcm_calc);
static DEVICE_ATTR(bcm_bw, 0660, show_bcm_bw, store_bcm_bw);

static struct attribute *exynos_bcm_dbg_sysfs_dev_entries[] = {
	&dev_attr_bcm_calc.attr,
	&dev_attr_bcm_bw.attr,
	NULL,
};

static struct attribute_group exynos_bcm_dbg_attr_group = {
	.name	= "bcm_attr",
	.bin_attrs	= exynos_bcm_dbg_sysfs_entries,
	.attrs	= exynos_bcm_dbg_sysfs_dev_entries,
};

static void __iomem *exynos_bcmdbg_remap(unsigned long addr, unsigned int size)
{
	int i;
	unsigned int num_pages = (size >> PAGE_SHIFT);
	pgprot_t prot = pgprot_writecombine(PAGE_KERNEL);
	struct page **pages = NULL;
	void __iomem *v_addr = NULL;

	if (!addr)
		return 0;

	pages = kmalloc_array(num_pages, sizeof(struct page *), GFP_ATOMIC);
	if (!pages)
		return 0;

	for (i = 0; i < num_pages; i++) {
		pages[i] = phys_to_page(addr);
		addr += PAGE_SIZE;
	}

	v_addr = vmap(pages, num_pages, VM_MAP, prot);
	kfree(pages);

	return v_addr;
}

static int exynos_bcm_dbg_dump_config(struct exynos_bcm_dbg_data *data)
{
	int ret;

	if (!data->rmem_acquired) {
		data->dump_addr.p_addr = bcm_reserved.p_addr;
		data->dump_addr.p_size = bcm_reserved.p_size;
		data->dump_addr.v_addr = 0;
	} else {
		data->dump_addr.p_addr = bcm_reserved.p_addr;
		data->dump_addr.p_size = bcm_reserved.p_size;
		data->dump_addr.v_addr = exynos_bcmdbg_remap(data->dump_addr.p_addr,
				data->dump_addr.p_size);
		dbg_snapshot_add_bl_item_info(BCM_DSS_NAME,
				data->dump_addr.p_addr, data->dump_addr.p_size);

		if (!data->dump_addr.p_addr) {
			BCM_ERR("%s: failed get dump address\n", __func__);
			return -ENOMEM;
		}
		if (!data->dump_addr.v_addr) {
			BCM_ERR("%s: failed get virtual address\n", __func__);
			return -ENOMEM;
		}
	}

	ret = exynos_bcm_dbg_set_dump_info(data);
	if (ret) {
		BCM_ERR("%s: failed set dump info\n", __func__);
		return ret;
	}

	return 0;
}

#if defined(CONFIG_EXYNOS_ITMON) || defined(CONFIG_EXYNOS_ITMON_MODULE)
static int exynos_bcm_dbg_itmon_notifier(struct notifier_block *nb,
					unsigned long val, void *v)
{
	struct itmon_notifier *itmon_info = (struct itmon_notifier *)v;

	BCM_INFO("%s: itmon error code %u\n", __func__, itmon_info->errcode);

	if (itmon_info->errcode == ERRCODE_ITMON_SLVERR && (itmon_info->dest &&
			(!strcmp("DREX_IRPS0", itmon_info->dest) ||
			!strcmp("DREX_IRPS1", itmon_info->dest) ||
			!strcmp("DREX_IRPS2", itmon_info->dest) ||
			!strcmp("DREX_IRPS3", itmon_info->dest)))) {
		return NOTIFY_BAD;
	}

	if (itmon_info->errcode == ERRCODE_ITMON_TIMEOUT) {
		BCM_INFO("%s: Note: It can occurred be IPC timeout	\
				because can be trying access to timeout block	\
				from BCMDBG plugin\n", __func__);
		exynos_bcm_dbg_stop(ITMON_HANDLE);
	}

	return NOTIFY_OK;
}
#endif

static int exynos_bcm_dbg_init(struct exynos_bcm_dbg_data *data)
{
	int ret = 0;

	/* parsing dts data for BCM debug */
	ret = exynos_bcm_dbg_parse_dt(data->dev->of_node, data);
	if (ret) {
		BCM_ERR("%s: failed to parse private data\n", __func__);
		goto err_parse_dt;
	}

	/* Request IPC channel */
	ret = exynos_bcm_dbg_ipc_channel_request(data);
	if (ret) {
		BCM_ERR("%s: failed to ipc channel request\n", __func__);
		goto err_ipc_channel;
	}

	ret = exynos_bcm_dbg_dump_config(data);
	if (ret) {
		BCM_ERR("%s: failed to dump config\n", __func__);
		goto err_dump_config;
	}
	data->dump_klog = false;

	/* Initalize BCM Plugin */
	ret = exynos_bcm_dbg_early_init(data, DBG_MODE);
	if (ret) {
		BCM_ERR("%s: failed to early bcm initialize\n", __func__);
		goto err_early_init;
	}

	/* initial Local Power Domain sync-up */
	ret = exynos_bcm_dbg_pd_sync_init(data);
	if (ret) {
		BCM_ERR("%s: failed to pd_sync_init\n", __func__);
		goto err_pd_sync_init;
	}

	/* BCM plugin run */
	if (data->initial_bcm_run) {
		ret = exynos_bcm_dbg_run(data->initial_bcm_run, data);
		if (ret) {
			BCM_ERR("%s: failed to bcm initial run\n", __func__);
			goto err_initial_run;
		}
	}

	return 0;

err_initial_run:
err_pd_sync_init:
err_early_init:
err_dump_config:
	exynos_bcm_dbg_ipc_channel_release(data);
err_ipc_channel:
err_parse_dt:
	return ret;
}

static void init_exynos_bcm_show_bw(struct exynos_bcm_dbg_data *data) {
	struct exynos_bcm_show_bw *bcm_show_bw;

	bcm_show_bw = kzalloc(sizeof(struct exynos_bcm_show_bw), GFP_KERNEL);
	data->bcm_show_bw = bcm_show_bw;

	INIT_DELAYED_WORK(&bcm_show_bw->bw_work, exynos_bcm_show_mif_work_func);

	mutex_init(&bcm_show_bw->lock);
	BCM_INFO("%s: Prepare bcm_show_bw done\n", __func__);
}

static void init_exynos_bcm_calc(struct exynos_bcm_dbg_data *data) {
	struct exynos_bcm_calc *bcm_calc = data->bcm_calc;
	int i;

	INIT_DELAYED_WORK(&bcm_calc->work, exynos_bcm_calc_work_func);

	bcm_calc->acc_data =
		kzalloc(sizeof(struct exynos_bcm_calc_data)
				* bcm_calc->num_ip, GFP_KERNEL);

	BCM_INFO("%s: num_ip: %d\n", __func__, bcm_calc->num_ip);
	BCM_INFO("%s: sample_time: %d\n", __func__,  bcm_calc->sample_time);
	for (i = 0; i < bcm_calc->num_ip; i++)
		BCM_INFO("%s: [idx:%d] ip_idx: %d, ip_name: %s,	ip_cnt: %d, \
		bus_width: %d\n", __func__, i, bcm_calc->ip_idx[i],
		bcm_calc->ip_name[i], bcm_calc->ip_cnt[i],
		bcm_calc->bus_width[i]);

	mutex_init(&bcm_calc->lock);
	BCM_INFO("%s: Prepare PPMU accumulator done\n",	__func__);
}

#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
static enum hrtimer_restart bcm_monitor(struct hrtimer *hrtimer)
{
	unsigned long flags;
	u32 period;
	enum hrtimer_restart ret = HRTIMER_NORESTART;

	spin_lock_irqsave(&bcm_dbg_data->lock, flags);
	period = bin_func->timer_event();
	spin_unlock_irqrestore(&bcm_dbg_data->lock, flags);

	if (bcm_dbg_data->bcm_mode == BCM_MODE_ONCE ||
		bcm_dbg_data->bcm_run_state == BCM_STOP)
		return ret;

	if (period > 0) {
		hrtimer_forward_now(hrtimer, ms_to_ktime(period));
		ret = HRTIMER_RESTART;
	}

	return ret;
}

static void __iomem *bcm_ioremap(phys_addr_t phys_addr, size_t size)
{
	void __iomem *ret;

	ret = ioremap(phys_addr, size);
	if (!ret)
		BCM_ERR("failed to map bcm physical address\n");
	return ret;
}

struct page_change_data {
	pgprot_t set_mask;
	pgprot_t clear_mask;
};

static int bcm_change_page_range(pte_t *ptep, pgtable_t token, unsigned long addr,
			void *data)
{
	struct page_change_data *cdata = data;
	pte_t pte = *ptep;

	pte = clear_pte_bit(pte, cdata->clear_mask);
	pte = set_pte_bit(pte, cdata->set_mask);

	set_pte(ptep, pte);
	return 0;
}

static int bcm_change_memory_common(unsigned long addr, int numpages,
				pgprot_t set_mask, pgprot_t clear_mask)
{
	unsigned long start = addr;
	unsigned long size = PAGE_SIZE * numpages;
	unsigned long end = start + size;
	int ret;
	struct page_change_data data;

	if (!PAGE_ALIGNED(addr)) {
		start &= PAGE_MASK;
		end = start + size;
		WARN_ON_ONCE(1);
	}

	if (!numpages)
		return 0;

	data.set_mask = set_mask;
	data.clear_mask = clear_mask;

	ret = apply_to_page_range(&init_mm, start, size, bcm_change_page_range,
					&data);

	flush_tlb_kernel_range(start, end);
	return ret;
}

int exynos_bcm_dbg_load_bin(void)
{
	int ret = 0;
	struct file *fp = NULL;
	long fsize, nread;
	u8 *buf = NULL;
	char *lib_bcm = NULL;
	mm_segment_t old_fs;

	if (bcm_dbg_data->bcm_load_bin)
		return 0;

	ret = bcm_change_memory_common((unsigned long)bcm_addr,
				BCM_BIN_SIZE, __pgprot(0), __pgprot(PTE_PXN));
	if (ret) {
		BCM_ERR("%s: failed to change memory common\n", __func__);
		goto err_out;
	}

	os_func.print = printk;
	os_func.snprint = snprintf;
	os_func.ioremap = bcm_ioremap;
	os_func.iounmap = iounmap;
	os_func.sched_clock = sched_clock;

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	fp = filp_open(BCM_BIN_NAME, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		BCM_ERR("%s: filp_open fail!!\n", __func__);
		ret = -EIO;
		goto err_fopen;
	}

	fsize = BCM_BIN_SIZE;
	BCM_INFO("%s: start, file path %s, size %ld Bytes\n",
			__func__, BCM_BIN_NAME, fsize);
	buf = vmalloc(fsize);
	if (!buf) {
		BCM_ERR("%s: failed to allocate memory\n", __func__);
		ret = -ENOMEM;
		goto err_alloc;
	}

	nread = vfs_read(fp, (char __user *)buf, fsize, &fp->f_pos);
	if (nread != fsize) {
		BCM_ERR("%s: failed to read firmware file, %ld Bytes\n",
				__func__, nread);
		ret = -EIO;
		goto err_vfs_read;
	}

	lib_bcm = (char *)bcm_addr;
	memset((char *)bcm_addr, 0x0, BCM_BIN_SIZE);

	flush_icache_range((unsigned long)lib_bcm,
			   (unsigned long)lib_bcm + BCM_BIN_SIZE);
	memcpy((void *)lib_bcm, (void *)buf, fsize);
	flush_cache_all();

	bin_func = ((start_up_func_t)lib_bcm)((void **)&os_func);

	bcm_dbg_data->bcm_load_bin = true;

	hrtimer_init(&bcm_dbg_data->bcm_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	bcm_dbg_data->bcm_hrtimer.function = bcm_monitor;

	ret = exynos_bcm_dbg_init(bcm_dbg_data);
	if (ret) {
		BCM_ERR("%s: failed to bcm init\n", __func__);
		goto err_init;
	}

err_init:
err_vfs_read:
	vfree((void *)buf);
err_alloc:
	filp_close(fp, NULL);
err_fopen:
	set_fs(old_fs);
err_out:
	return ret;
}
EXPORT_SYMBOL(exynos_bcm_dbg_load_bin);
#endif

static int exynos_bcm_dbg_pm_suspend(struct device *dev)
{
	unsigned int suspend = true;
	int ret;
#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
	if (!bcm_dbg_data->bcm_load_bin)
		return 0;
#endif
	BCM_DBG("%s: ++\n", __func__);

	if (bcm_dbg_data->bcm_calc->enable) {
		exynos_bcm_calc_enable(0);
	}

	ret = exynos_bcm_dbg_str(suspend, bcm_dbg_data);
	if (ret) {
		BCM_ERR("%s: failed set str state\n", __func__);
		return ret;
	}

	BCM_DBG("%s: --\n", __func__);

	return 0;
}

static int exynos_bcm_dbg_pm_resume(struct device *dev)
{
	unsigned int suspend = false;
	int ret;
#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
	if (!bcm_dbg_data->bcm_load_bin)
		return 0;
#endif
	BCM_DBG("%s: ++\n", __func__);

	ret = exynos_bcm_dbg_str(suspend, bcm_dbg_data);
	if (ret) {
		BCM_ERR("%s: failed set str state\n", __func__);
		return ret;
	}

	BCM_DBG("%s: --\n", __func__);

	return 0;
}

static struct dev_pm_ops exynos_bcm_dbg_pm_ops = {
	.suspend	= exynos_bcm_dbg_pm_suspend,
	.resume		= exynos_bcm_dbg_pm_resume,
};

static int bcmdbg_panic_handler(struct notifier_block *nb, unsigned long l,
		void *buf)
{
	exynos_bcm_dbg_stop(PANIC_HANDLE);

	return 0;
}

static struct notifier_block nb_bcmdbg_panic = {
	.notifier_call = bcmdbg_panic_handler,
	.priority = INT_MAX - 1,
};

static void exynos_bcm_dbg_shutdown(struct platform_device *pdev)
{
	if (bcm_dbg_data->bcm_calc->enable)
		exynos_bcm_calc_enable(0);
}

static int exynos_bcm_dbg_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct reserved_mem *rmem;
	struct device_node *rmem_np;
	struct exynos_bcm_dbg_data *data;

	data = kzalloc(sizeof(struct exynos_bcm_dbg_data), GFP_KERNEL);
	if (data == NULL) {
		BCM_ERR("%s: failed to allocate BCM debug device\n", __func__);
		ret = -ENOMEM;
		goto err_data;
	}

	bcm_dbg_data = data;
	data->dev = &pdev->dev;

	spin_lock_init(&data->lock);
	mutex_init(&bcm_bw_lock);

	rmem_np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	rmem = of_reserved_mem_lookup(rmem_np);
	if (rmem != NULL) {
		data->rmem_acquired = true;
		bcm_reserved.p_addr = rmem->base;
		bcm_reserved.p_size = rmem->size;
	} else {
		data->rmem_acquired = false;

		bcm_reserved.p_addr = 0;
		bcm_reserved.p_size = 0;

		BCM_INFO("%s: failed to acquire memory region\n", __func__);
	}

#if !(defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE))
	ret = exynos_bcm_dbg_init(data);
	if (ret) {
		BCM_ERR("%s: failed to bcm init\n", __func__);
		goto err_init;
	}
	memset((char *)data->dump_addr.v_addr, 0x0, data->dump_addr.p_size);
#endif
	atomic_notifier_chain_register(&panic_notifier_list, &nb_bcmdbg_panic);

	platform_set_drvdata(pdev, data);

	ret = sysfs_create_group(&data->dev->kobj, &exynos_bcm_dbg_attr_group);
	if (ret)
		BCM_ERR("%s: failed creat sysfs for Exynos BCM DBG\n", __func__);

#if defined(CONFIG_CPU_IDLE)
	data->idle_ip_index = exynos_get_idle_ip_index(dev_name(&pdev->dev), 1);
	BCM_INFO("%s: BCM idle_ip_index[%d]\n", __func__, data->idle_ip_index);
	if (data->idle_ip_index < 0)
		BCM_ERR("%s: idle ip index is not valid\n", __func__);
	exynos_update_ip_idle_status(data->idle_ip_index, 1);
#endif

#if defined(CONFIG_EXYNOS_ITMON) || defined(CONFIG_EXYNOS_ITMON_MODULE)
	data->itmon_notifier.notifier_call = exynos_bcm_dbg_itmon_notifier;
	itmon_notifier_chain_register(&data->itmon_notifier);
#endif
	init_exynos_bcm_calc(data);
	init_exynos_bcm_show_bw(data);

	BCM_INFO("%s: exynos bcm is initialized!!\n", __func__);

	return 0;

#if !(defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE))
err_init:
#endif
	kfree(data);
	data = NULL;
	bcm_dbg_data = NULL;
err_data:
	return ret;
}

static int exynos_bcm_dbg_remove(struct platform_device *pdev)
{
	struct exynos_bcm_dbg_data *data =
					platform_get_drvdata(pdev);
	int ret;

	sysfs_remove_group(&data->dev->kobj, &exynos_bcm_dbg_attr_group);
	platform_set_drvdata(pdev, NULL);
	ret = exynos_bcm_dbg_pd_sync_exit(data);
	if (ret) {
		BCM_ERR("%s: failed to pd_sync_exit\n", __func__);
		return ret;
	}

	exynos_bcm_dbg_ipc_channel_release(data);
	kfree(data->bcm_calc);
	kfree(data);
	data = NULL;

	BCM_INFO("%s: exynos bcm is removed!!\n", __func__);

	return 0;
}

#if defined(CONFIG_EXYNOS_BCM_DBG_GNR) || defined(CONFIG_EXYNOS_BCM_DBG_GNR_MODULE)
static int bcm_setup(char *str)
{
	if (kstrtoul(str, 0, (unsigned long *)&bcm_addr))
		goto out;

	return 0;
out:
	return -EINVAL;
}
__setup("reserve-fimc=", bcm_setup);
#endif

static struct platform_device_id exynos_bcm_dbg_driver_ids[] = {
	{ .name = EXYNOS_BCM_DBG_MODULE_NAME, },
	{},
};
MODULE_DEVICE_TABLE(platform, exynos_bcm_dbg_driver_ids);

static const struct of_device_id exynos_bcm_dbg_match[] = {
	{ .compatible = "samsung,exynos-bcm_dbg", },
	{},
};

static struct platform_driver exynos_bcm_dbg_driver = {
	.remove = exynos_bcm_dbg_remove,
	.id_table = exynos_bcm_dbg_driver_ids,
	.shutdown = exynos_bcm_dbg_shutdown,
	.driver = {
		.name = EXYNOS_BCM_DBG_MODULE_NAME,
		.owner = THIS_MODULE,
		.pm = &exynos_bcm_dbg_pm_ops,
		.of_match_table = exynos_bcm_dbg_match,
	},
};

module_platform_driver_probe(exynos_bcm_dbg_driver, exynos_bcm_dbg_probe);

MODULE_AUTHOR("Taekki Kim <taekki.kim@samsung.com>");
MODULE_DESCRIPTION("Samsung BCM Debug driver");
MODULE_LICENSE("GPL");
