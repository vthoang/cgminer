#include "driver-gekko.h"
#include "crc.h"
#include <unistd.h>

uint32_t bmcrc(unsigned char *ptr, uint32_t len)
{
	unsigned char c[5] = {1, 1, 1, 1, 1};
	uint32_t i, c1, ptr_idx = 0;

	for (i = 0; i < len; i++) {
		c1 = c[1];
		c[1] = c[0];
		c[0] = c[4] ^ ((ptr[ptr_idx] & (0x80 >> (i % 8))) ? 1 : 0);
		c[4] = c[3];
		c[3] = c[2];
		c[2] = c1 ^ c[0];

		if (((i + 1) % 8) == 0)
			ptr_idx++;
	}
	return (c[4] * 0x10) | (c[3] * 0x08) | (c[2] * 0x04) | (c[1] * 0x02) | (c[0] * 0x01);
}

void dumpbuffer(struct cgpu_info *compac, int LOG_LEVEL, char *note, unsigned char *ptr, uint32_t len)
{
	struct COMPAC_INFO *info = compac->device_data;
	char str[256];

	const char * hex = "0123456789ABCDEF";
	char * pout = str;
	int i = 0;
	for(; i < len - 1; ++i){
		*pout++ = hex[(*ptr>>4)&0xF];
		*pout++ = hex[(*ptr++)&0xF];
		*pout++ = ':';
	}
	*pout++ = hex[(*ptr>>4)&0xF];
	*pout++ = hex[(*ptr)&0xF];
	*pout = 0;

	applog(LOG_LEVEL, "%s %i %s: %s", compac->drv->name, compac->device_id, note, str);
}

static void compac_send(struct cgpu_info *compac, unsigned char *req_tx, uint32_t bytes, uint32_t crc_bits)
{
	struct COMPAC_INFO *info = compac->device_data;
	int read_bytes = 1;
	int read_wait = 0;
	int i;

	//leave original buffer intact
	for (i = 0; i < bytes; i++) {
		info->cmd[i] = req_tx[i];
	}
	info->cmd[bytes - 1] |= bmcrc(req_tx, crc_bits);

	cgsleep_ms(1);
	dumpbuffer(compac, LOG_INFO, "TX", info->cmd, bytes);
	usb_write(compac, info->cmd, bytes, &read_bytes, C_REQUESTRESULTS);
}

static void compac_send_chain_inactive(struct cgpu_info *compac)
{
	struct COMPAC_INFO *info = compac->device_data;
	int i;

	applog(LOG_INFO,"%s %d: sending chain inactive for %d chip(s)", compac->drv->name, compac->device_id, info->chips);
	if (info->asic_type == BM1384) {
		unsigned char buffer[] = {0x85, 0x00, 0x00, 0x00};
		compac_send(compac, (char *)buffer, sizeof(buffer), 8 * sizeof(buffer) - 5); // chain inactive
		for (i = 0; i < info->chips; i++) {
			buffer[0] = 0x01;
			buffer[1] = (0x100 / info->chips) * i;
			compac_send(compac, (char *)buffer, sizeof(buffer), 8 * sizeof(buffer) - 5);
		}
		buffer[0] = 0x86; // GATEBLK
		buffer[1] = 0x00;
		buffer[2] = 0x9a; // 0x80 | 0x1a;
		//compac_send(compac, (char *)buffer, sizeof(buffer), 8 * sizeof(buffer) - 5);
	}

	if (info->mining_state != MINER_MINING) {
		applog(LOG_WARNING, "%s %d: open cores @ %.2fMHz", compac->drv->name, compac->device_id, info->frequency);
		info->zero_check = 0;
		info->task_hcn = 0;
		info->mining_state = MINER_OPEN_CORE;
	}
}

static void compac_set_frequency(struct cgpu_info *compac, float frequency)
{
	struct COMPAC_INFO *info = compac->device_data;
	uint32_t i, r, r1, r2, r3, p1, p2, pll;

	if (info->asic_type == BM1384) {
		unsigned char buffer[] = {0x82, 0x0b, 0x83, 0x00};

		frequency = bound(frequency, 6, 500);
		frequency = ceil(100 * (frequency) / 625.0) * 6.25;

		info->frequency = frequency;

		r = floor(log(info->frequency/25) / log(2));

		r1 = 0x0785 - r;
		r2 = 0x200 / pow(2, r);
		r3 = 25 * pow(2, r);

		p1 = r1 + r2 * (info->frequency - r3) / 6.25;
		p2 = p1 * 2 + (0x7f + r);

		pll = ((uint32_t)(info->frequency) % 25 == 0 ? p1 : p2);

		if (info->frequency < 100) {
			pll = 0x0783 - 0x80 * (100 - info->frequency) / 6.25;
		}

		buffer[1] = (pll >> 8) & 0xff;
		buffer[2] = (pll) & 0xff;

		applog(LOG_WARNING, "%s %d: setting frequency to %.2fMHz", compac->drv->name, compac->device_id, frequency);
		compac_send(compac, (char *)buffer, sizeof(buffer), 8 * sizeof(buffer) - 5);
		buffer[0] = 0x84;
		buffer[1] = 0x00;
		buffer[2] = 0x00;
//		compac_send(compac, (char *)buffer, sizeof(buffer), 8 * sizeof(buffer) - 5);
		buffer[2] = 0x04;
		compac_send(compac, (char *)buffer, sizeof(buffer), 8 * sizeof(buffer) - 5);
	}

	info->hashrate = info->chips * info->frequency * info->cores * 1000000;
	info->fullscan_ms = 1000.0 * 0xffffffffull / info->hashrate;
	info->ticket_mask = bound(pow(2, ceil(log(info->hashrate / (2.0 * 0xffffffffull)) / log(2))) - 1, 0, 4000);
	info->difficulty = info->ticket_mask + 1;

}

static uint64_t compac_check_nonce(struct cgpu_info *compac)
{
	struct COMPAC_INFO *info = compac->device_data;
	uint32_t nonce = (info->rx[3] << 0) | (info->rx[2] << 8) | (info->rx[1] << 16) | (info->rx[0] << 24);

	uint64_t hashes = 0;
	uint32_t hwe = compac->hw_errors;
	struct timeval now;

	uint32_t job_id;

	if (info->asic_type == BM1384) {
		job_id = info->rx[4] ^ 0x80;
	}

	struct work *work = info->work[job_id];

	if (nonce == 0 || nonce == 0xffffffff || !work || job_id >= info->max_job_id) {
		return hashes;
	}

	cgtime(&now);

	info->nonces++;
	info->nonceless = 0;
	if (nonce == info->prev_nonce) {
		applog(LOG_WARNING, "%s %d: Duplicate Nonce : %08x @ %02x [%02x %02x %02x %02x %02x %02x %02x]", compac->drv->name, compac->device_id, nonce, job_id,
			info->rx[0], info->rx[1], info->rx[2], info->rx[3], info->rx[4], info->rx[5], info->rx[6]);
		info->dups++;
		if (info->dups == 1) {
			info->mining_state = MINER_MINING_DUPS;
		}
		return hashes;
	} else {
		info->dups = 0;
	}

	hashes = info->difficulty * 0xffffffffull; 
	info->prev_nonce = nonce;
	work->device_diff = info->difficulty;

	applog(LOG_INFO, "%s %d: Device reported nonce: %08x @ %02x", compac->drv->name, compac->device_id, nonce, job_id);

	if (info->update_work) {
		return hashes;
	}

	if (submit_nonce(info->thr, work, nonce)) {
		cgtime(&info->last_nonce);
		info->accepted++;
		info->failing = false;
	} else {
		if (hwe != compac->hw_errors) {
			cgtime(&info->last_hwerror);
		}
	}

	return hashes;
}

static void compac_update_work(struct cgpu_info *compac)
{
	struct COMPAC_INFO *info = compac->device_data;
	info->update_work = 1;
}

static void compac_flush_buffer(struct cgpu_info *compac)
{
	int read_bytes = 1;
	unsigned char resp[32];

	while (read_bytes) {
		usb_read_timeout(compac, (char *)resp, 32, &read_bytes, 5, C_REQUESTRESULTS);
	}
}

static void compac_flush_work(struct cgpu_info *compac)
{
	compac_flush_buffer(compac);
	compac_update_work(compac);
}

static void *compac_listen(void *object)
{
	struct cgpu_info *compac = (struct cgpu_info *)object;
	struct COMPAC_INFO *info = compac->device_data;
	int read_bytes, crc_ok, cmd_resp;
	uint32_t err = 0;
	struct timeval now;

	while (info->mining_state != MINER_SHUTDOWN) {
		memset(info->rx, 0, info->rx_len);
		err = usb_read_timeout(compac, (char *)info->rx, info->rx_len, &read_bytes, 100, C_GETRESULTS);
		if (read_bytes > 0) {
			cmd_resp = (info->rx[read_bytes - 1] <= 0x1F && bmcrc(info->rx, 8 * read_bytes - 5) == info->rx[read_bytes - 1]) ? 1 : 0;
			dumpbuffer(compac, LOG_INFO, "RX", info->rx, read_bytes);

			if (cmd_resp && info->rx[0] == 0x80) {
				float frequency;
				cgtime(&now);

				if (info->asic_type == BM1384) {
					frequency = (info->rx[1] + 1) * 6.25 / (1 + info->rx[2] & 0x0f) * pow(2, (3 - info->rx[3])) + ((info->rx[2] >> 4) * 6.25);
				}

				if (frequency != info->frequency) {
					applog(LOG_WARNING,"%s %d: frequency changed %.2fMHz -> %.2fMHz", compac->drv->name, compac->device_id, info->frequency, frequency);
				} else {
					applog(LOG_INFO,"%s %d: chip reported frequency of %.2fMHz", compac->drv->name, compac->device_id, frequency);
				}
				cgtime(&info->last_frequency_report);

				info->frequency = frequency;
				info->hashrate = info->chips * info->frequency * info->cores * 1000000;
				info->fullscan_ms = 1000.0 * 0xffffffffull / info->hashrate;
				info->ticket_mask = bound(pow(2, ceil(log(info->hashrate / (2.0 * 0xffffffffull)) / log(2))) - 1, 0, 4000);
				info->difficulty = info->ticket_mask + 1;

			}

			switch (info->mining_state) {
				case MINER_CHIP_COUNT:
				case MINER_CHIP_COUNT_XX:
					if (cmd_resp && info->rx[0] == 0x13) {
						info->chips++;
						info->mining_state = MINER_CHIP_COUNT_XX;
					}
					break;
				case MINER_OPEN_CORE:
					if ((info->rx[0] == 0x72 && info->rx[1] == 0x03 && info->rx[2] == 0xEA && info->rx[3] == 0x83) ||
					    (info->rx[0] == 0xE1 && info->rx[0] == 0x6B && info->rx[0] == 0xF8 && info->rx[0] == 0x09)) {
						//open core nonces = healthy chips.
						info->zero_check++;
					}
					break;
				case MINER_MINING:
					if (!cmd_resp) {
						mutex_lock(&info->lock);
						info->hashes += compac_check_nonce(compac);
						mutex_unlock(&info->lock);
					}
					break;
				default:
					break;
			}
		} else {
			switch (info->mining_state) {
				case MINER_CHIP_COUNT_XX:
					applog(LOG_INFO, "%s %d: found %d chip(s)", compac->drv->name, compac->device_id, info->chips);
					info->mining_state = MINER_CHIP_COUNT_OK;
					break;
				default:
					break;
			}
		}
	}
	info->mining_state = MINER_SHUTDOWN_OK;
}

static void init_task(struct COMPAC_INFO *info)
{
	struct work *work = info->work[info->job_id];

	memset(info->task, 0, info->task_len);

	if (info->asic_type == BM1384) {
		if (info->mining_state == MINER_MINING) {
			stuff_reverse(info->task, work->midstate, 32);
			stuff_reverse(info->task + 52, work->data + 64, 12);
			info->task[39] = info->ticket_mask & 0xff;
			stuff_msb(info->task + 40, info->task_hcn);
		} else {
			//memset(info->task, 0xff, 32);
			//info->task[39] = 0x3f;
			//info->task[63] = 0x01;
		}
		info->task[51] = info->job_id & 0xff;
	}
}

static int64_t compac_scanwork(struct thr_info *thr)
{
	struct cgpu_info *compac = thr->cgpu;
	struct COMPAC_INFO *info = compac->device_data;
	struct timeval now;
	int i, read_bytes, sleep_ms;
	uint32_t err = 0;
	uint64_t max_task_wait = 0;
	uint64_t hashes = 0;

	if (compac->usbinfo.nodev)
		return -1;

	cgtime(&now);
	info->scanhash_ms = (info->scanhash_ms * 9 + ms_tdiff(&now, &info->last_scanhash)) / 10;
	cgtime(&info->last_scanhash);
	max_task_wait = bound(0.60 * info->fullscan_ms - 5, 5, info->fullscan_ms);
	sleep_ms = bound(ceil(max_task_wait/15.0), 1, 200);

	switch (info->mining_state) {
		case MINER_INIT:
			info->mining_state = MINER_CHIP_COUNT;
			info->chips = 0;
			info->ramping = 0;
			if (info->asic_type == BM1384) {
				unsigned char buffer[] = {0x84, 0x00, 0x00, 0x00};
				compac_send(compac, (char *)buffer, sizeof(buffer), 8 * sizeof(buffer) - 5);
			}
			break;
		case MINER_CHIP_COUNT:
			cgsleep_ms(20);
			break;
		case MINER_CHIP_COUNT_OK:
			cgsleep_ms(100);

			compac_set_frequency(compac, info->frequency_start);
			compac_send_chain_inactive(compac);
			break;
		case MINER_OPEN_CORE:
			if (ms_tdiff(&now, &info->last_task) > (max_task_wait / 2)) {
				if (info->zero_check) {
					info->hashes += info->hashrate * info->task_ms / 1000;
				}
				info->job_id = info->ramping % info->max_job_id;

				//info->task_hcn = (0xffffffff / info->chips) * (1 + info->ramping) / info->cores;
				init_task(info);
				dumpbuffer(compac, LOG_DEBUG, "RAMP", info->task, info->task_len);

				usb_write(compac, (char *)info->task, info->task_len, &read_bytes, C_SENDWORK);
				if (info->ramping > info->cores) {
					info->job_id = 0;
					info->mining_state = MINER_OPEN_CORE_OK;
					info->task_hcn = (0xffffffff / info->chips);
				}

				info->ramping++;
				info->task_ms = (info->task_ms * 9 + ms_tdiff(&now, &info->last_task)) / 10;
				cgtime(&info->last_task);
			}
			cgsleep_ms(sleep_ms);
			break;
		case MINER_OPEN_CORE_OK:
			applog(LOG_WARNING, "%s %d: start work @ %.2fMHz", compac->drv->name, compac->device_id, info->frequency);
			cgtime(&info->start_time);
			cgtime(&info->last_frequency_adjust);
			cgtime(&info->last_frequency_ping);
			cgtime(&info->last_frequency_report);
			info->mining_state = MINER_MINING;
			break;
		case MINER_MINING:
			if (info->update_work || (ms_tdiff(&now, &info->last_task) > max_task_wait)) {
				if (ms_tdiff(&now, &info->last_frequency_ping) > 5000) {
					if (info->asic_type == BM1384) {
						unsigned char buffer[] = {0x84, 0x00, 0x04, 0x00};
						compac_send(compac, (char *)buffer, sizeof(buffer), 8 * sizeof(buffer) - 5);
					}
					cgtime(&info->last_frequency_ping);
				}

				if (info->accepted > 10 && ms_tdiff(&now, &info->last_frequency_ping) > 100 && 
					ms_tdiff(&info->last_nonce, &info->last_frequency_adjust) > 0 && 
					ms_tdiff(&now, &info->last_frequency_adjust) >= bound(opt_gekko_step_delay, 1, 600) * 1000) {
					if (info->frequency != info->frequency_requested) {
						float new_frequency;
						if (info->frequency < info->frequency_requested) {
							new_frequency = info->frequency + opt_gekko_step_freq;
							if (new_frequency > info->frequency_requested) {
								new_frequency = info->frequency_requested;
							}
						} else {
							new_frequency = info->frequency - opt_gekko_step_freq;
							if (new_frequency < info->frequency_requested) {
								new_frequency = info->frequency_requested;
							}
						}
						compac_set_frequency(compac, new_frequency);
						compac_send_chain_inactive(compac);
						info->accepted = 0;
					}

					cgtime(&info->last_frequency_adjust);
					//uint64_t hashrate_5m, hashrate_1m;

					//hashrate_1m = (double)rolling1 * 1000000ull;
					//hashrate_5m = (double)rolling5 * 1000000ull;
					//if ((hashrate_1m < (0.75 * info->hashrate)) && ms_tdiff(&now, &info->start_time) > (3 * 60 * 1000)) {
					//	applog(LOG_INFO, "%" PRIu64 " : %" PRIu64 " : %" PRIu64, hashrate_1m, hashrate_5m, info->hashrate);
					//	applog(LOG_INFO,"%s %d: unhealthy miner", compac->drv->name, compac->device_id);
					//	info->ramping = 0;
					//	info->mining_state = MINER_CHIP_COUNT_OK;
					//	inc_hw_errors(info->thr);
					//}

					//if (ms_tdiff(&now, &info->last_frequency_report) > (30 + 7500 * 3)) {
					//	applog(LOG_WARNING,"%s %d: asic(s) went offline", compac->drv->name, compac->device_id);
					//	usb_nodev(compac);
					//	return -1;
					//}
				}

				info->job_id = (info->job_id + 1) % info->max_job_id;

				if (info->update_work) {
					mutex_lock(&info->lock);
					for (i = 0; i < info->max_job_id; i++) {
						if (info->work[i])
							free_work(info->work[i]);
						info->work[i] = NULL;
					}
					mutex_unlock(&info->lock);
					info->update_work = 0;
				}

				if (info->work[info->job_id] && info->work[info->job_id]->drv_rolllimit <= 0) {
					free_work(info->work[info->job_id]);
					info->work[info->job_id] = NULL;
				}

				if (!info->work[info->job_id]) {
					info->work[info->job_id] = get_work(thr, thr->id);
				} else {
					info->work[info->job_id]->drv_rolllimit--;
					roll_work(info->work[info->job_id]);
				}

				init_task(info);
				dumpbuffer(compac, LOG_DEBUG, "TASK", info->task, info->task_len);

				err = usb_write(compac, (char *)info->task, info->task_len, &read_bytes, C_SENDWORK);
				if (err != LIBUSB_SUCCESS || read_bytes != info->task_len) {
					applog(LOG_INFO,"%s %d: Write error", compac->drv->name, compac->device_id);
					return -1;
				}
				info->task_ms = (info->task_ms * 9 + ms_tdiff(&now, &info->last_task)) / 10;
				cgtime(&info->last_task);
				return hashes;
			} 
			
			cgsleep_ms(sleep_ms);
			break;
		case MINER_MINING_DUPS:
			info->mining_state = MINER_MINING;
			if ((int)info->frequency == 200) {
				//possible terminus reset condition.
				compac_set_frequency(compac, info->frequency);
				compac_send_chain_inactive(compac);
				cgtime(&info->last_frequency_adjust);
			} else {
				//check for reset condition
				if (info->asic_type == BM1384) {
					unsigned char buffer[] = {0x84, 0x00, 0x04, 0x00};
					compac_send(compac, (char *)buffer, sizeof(buffer), 8 * sizeof(buffer) - 5);
				}
				cgtime(&info->last_frequency_ping);
			}
			break;
		default:
			cgsleep_ms(20);
			break;
	}
	hashes = info->hashes;
	info->hashes -= hashes;

	return hashes;
}

static struct cgpu_info *compac_detect_one(struct libusb_device *dev, struct usb_find_devices *found)
{
	struct cgpu_info *compac;
	struct COMPAC_INFO *info;
	int err, i;
	uint32_t baudrate = CP210X_DATA_BAUD;
	unsigned int bits = CP210X_BITS_DATA_8 | CP210X_BITS_PARITY_MARK;

	compac = usb_alloc_cgpu(&gekko_drv, 1);

	if (!usb_init(compac, dev, found)) {
		applog(LOG_ERR, "failed usb_init");
		compac = usb_free_cgpu(compac);
		return NULL;
	}

	info = cgcalloc(1, sizeof(struct COMPAC_INFO));
	compac->device_data = (void *)info;

	info->ident = usb_ident(compac);

	switch (info->ident) {
		case IDENT_BSC:
		case IDENT_GSC:
		case IDENT_BSD:
		case IDENT_GSD:
		case IDENT_BSE:
		case IDENT_GSE:
			info->asic_type = BM1384;
			info->cores = 55;
			info->max_job_id = 0x1f;
			info->rx_len = 5;
			info->task_len = 64;
			info->tx_len = 4;

			usb_transfer_data(compac, CP210X_TYPE_OUT, CP210X_REQUEST_IFC_ENABLE, CP210X_VALUE_UART_ENABLE, info->interface, NULL, 0, C_ENABLE_UART);
			usb_transfer_data(compac, CP210X_TYPE_OUT, CP210X_REQUEST_DATA, CP210X_VALUE_DATA, info->interface, NULL, 0, C_SETDATA);
			usb_transfer_data(compac, CP210X_TYPE_OUT, CP210X_REQUEST_BAUD, 0, info->interface, &baudrate, sizeof (baudrate), C_SETBAUD);
			usb_transfer_data(compac, CP210X_TYPE_OUT, CP210X_SET_LINE_CTL, bits, info->interface, NULL, 0, C_SETPARITY);
			break;
		default:
			quit(1, "%s compac_detect_one() invalid %s ident=%d",
				compac->drv->dname, compac->drv->dname, info->ident);
	}

	info->interface = usb_interface(compac);

	applog(LOG_DEBUG, "Using interface %d", info->interface);

	if (!add_cgpu(compac))
		quit(1, "Failed to add_cgpu in compac_detect_one");

	update_usb_stats(compac);

	for (i = 0; i < 8; i++) {
		compac->unique_id[i] = compac->unique_id[i+3];
	}
	compac->unique_id[8] = 0;

	applog(LOG_WARNING, "%s %d: %s (%s)", compac->drv->name, compac->device_id, compac->usbdev->prod_string, compac->unique_id);

	return compac;
}

static void compac_detect(bool __maybe_unused hotplug)
{
	usb_detect(&gekko_drv, compac_detect_one);
}

static void *compac_unplug(void *object)
{
	struct cgpu_info *compac = (struct cgpu_info *)object;
	struct COMPAC_INFO *info = compac->device_data;
	int err = 0;
	applog(LOG_INFO, "%s-%i: waiting for unplug", compac->drv->name, compac->device_id);
	while (!err) {
		cgsleep_ms(1000);
		err = usb_transfer_data(compac, CP210X_TYPE_OUT, CP210X_REQUEST_DATA, CP210X_VALUE_DATA, info->interface, NULL, 0, C_SETDATA);
	}
	applog(LOG_INFO, "%s-%i: unplug detected", compac->drv->name, compac->device_id);
	usb_uninit(compac);
}

static bool compac_prepare(struct thr_info *thr)
{
	struct cgpu_info *compac = thr->cgpu;
	struct COMPAC_INFO *info = compac->device_data;
	int i;
	int read_bytes = 1;
	bool miner_ok = true;

	info->thr = thr;

	//Sanity check and abort to prevent miner thread from being created.
	if (info->asic_type == BM1384) {
		unsigned char buffer[] = {0x84, 0x00, 0x00, 0x00};
		compac_send(compac, (char *)buffer, sizeof(buffer), 8 * sizeof(buffer) - 5);
		compac_send(compac, (char *)buffer, sizeof(buffer), 8 * sizeof(buffer) - 5);
		miner_ok = false;
		while (read_bytes) {
			memset(info->rx, 0, info->rx_len);
			usb_read_timeout(compac, (char *)info->rx, info->rx_len, &read_bytes, 200, C_GETRESULTS);
			if (read_bytes > 0 && info->rx[0] == 0x13) {
				miner_ok = true;
			}
		}
	}

	if (!miner_ok) {
		if (info->ident != IDENT_BSD && info->ident != IDENT_GSD) {
			usb_nodev(compac);
		} else {
			//DOA.   Only thing left to do is unplug.
			compac->deven = DEV_DISABLED;
			if (thr_info_create(&(info->xthr), NULL, compac_unplug, (void *)compac)) {
				applog(LOG_ERR, "%s-%i: unplug thread create failed", compac->drv->name, compac->device_id);
				return false;
			}
			pthread_detach(info->xthr.pth);
		}
	}

	return miner_ok;
}

static bool compac_init(struct thr_info *thr)
{
	struct cgpu_info *compac = thr->cgpu;
	struct COMPAC_INFO *info = compac->device_data;

	info->mining_state = MINER_INIT;
	info->prev_nonce = 0;

	cgtime(&info->last_frequency_adjust);
	cgtime(&info->last_frequency_ping);
	cgtime(&info->last_scanhash);
	cgtime(&info->last_task);
	cgtime(&info->start_time);

	switch (info->ident) {
		case IDENT_BSC:
		case IDENT_GSC:
			info->frequency_requested = opt_gekko_gsc_freq;
			info->frequency_start = info->frequency_requested;
			break;
		case IDENT_BSD:
		case IDENT_GSD:
			info->frequency_requested = opt_gekko_gsd_freq;
			info->frequency_start = opt_gekko_start_freq;
			break;
		case IDENT_BSE:
		case IDENT_GSE:
			info->frequency_requested = opt_gekko_gse_freq;
			info->frequency_start = opt_gekko_start_freq;
			break;
		default:
			info->frequency_requested = 200;
			info->frequency_start = info->frequency_requested;
			break;
	}
	info->frequency_requested = ceil(100 * (info->frequency_requested) / 625.0) * 6.25;
	info->frequency_start = ceil(100 * (info->frequency_start) / 625.0) * 6.25;

	pthread_mutex_init(&info->lock, NULL);

	if (thr_info_create(&(info->rthr), NULL, compac_listen, (void *)compac)) {
		applog(LOG_ERR, "%s-%i: thread create failed", compac->drv->name, compac->device_id);
		return false;
	}
	pthread_detach(info->rthr.pth);

	return true;
}

static void compac_statline(char *buf, size_t bufsiz, struct cgpu_info *compac)
{
	struct COMPAC_INFO *info = compac->device_data;
	if (info->chips == 0) {
		return;
	}
	if (1 || opt_log_output) {
		tailsprintf(buf, bufsiz, "BM1384:%i %.2fMHz (%d/%d/%d/%d)", info->chips, info->frequency, info->scanhash_ms, info->task_ms, info->fullscan_ms, compac->hw_errors);
	} else {
		tailsprintf(buf, bufsiz, "BM1384:%i %.2fMHz HW:%d", info->chips, info->frequency_requested, compac->hw_errors);
	}
}

static struct api_data *compac_api_stats(struct cgpu_info *compac)
{
	struct COMPAC_INFO *info = compac->device_data;
	struct api_data *root = NULL;

	root = api_add_int(root, "Nonces", &info->nonces, false);
	//root = api_add_int(root, "Accepted", &info->accepted, false);

	return root;
}

static void compac_shutdown(struct thr_info *thr)
{
	struct cgpu_info *compac = thr->cgpu;
	struct COMPAC_INFO *info = compac->device_data;
	if (info->asic_type == BM1384 && info->frequency != 100) {
		compac_set_frequency(compac, 100);
	}
	info->mining_state = MINER_SHUTDOWN;
	usleep(200); // Let threads close.
}

uint64_t bound(uint64_t value, uint64_t lower_bound, uint64_t upper_bound)
{
	if (value < lower_bound)
		return lower_bound;
	if (value > upper_bound)
		return upper_bound;
	return value;
}

void stuff_reverse(unsigned char *dst, unsigned char *src, uint32_t len)
{
	uint32_t i;
	for (i = 0; i < len; i++) {
		dst[i] = src[len - i - 1];
	}
}

void stuff_lsb(unsigned char *dst, uint32_t x)
{
	dst[0] = (x >>  0) & 0xff;
	dst[1] = (x >>  8) & 0xff;
	dst[2] = (x >> 16) & 0xff;
	dst[3] = (x >> 24) & 0xff;
}

void stuff_msb(unsigned char *dst, uint32_t x)
{
	dst[0] = (x >> 24) & 0xff;
	dst[1] = (x >> 16) & 0xff;
	dst[2] = (x >>  8) & 0xff;
	dst[3] = (x >>  0) & 0xff;
}

struct device_drv gekko_drv = {
	.drv_id              = DRIVER_gekko,
	.dname               = "GekkoScience",
	.name                = "GSX",
	.hash_work           = &hash_driver_work,
	.get_api_stats       = compac_api_stats,
	.get_statline_before = compac_statline,
	.drv_detect          = compac_detect,
	.scanwork            = compac_scanwork,
	.flush_work          = compac_flush_work,
	.update_work         = compac_update_work,
	.thread_prepare      = compac_prepare,
	.thread_init         = compac_init,
	.thread_shutdown     = compac_shutdown,
};
