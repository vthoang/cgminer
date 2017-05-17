#include "driver-gekko.h"

static void compac_send_async(struct cgpu_info *compac, uint32_t b1, uint32_t b2, uint32_t b3, uint32_t b4)
{
	struct COMPAC_INFO *info = compac->device_data;
	unsigned char req_tx[4] = { b1, b2, b3, b4 };

	req_tx[3] |= bmcrc(req_tx, 27);
	thread_queue_add(&info->out_queue, memdup(&req_tx, 4), 0);
	applog(LOG_WARNING, "%s %d TX: %02x %02x %02x %02x", compac->drv->name, compac->device_id, req_tx[0], req_tx[1], req_tx[2], req_tx[3]);
}

static void compac_send(struct cgpu_info *compac, uint32_t b1, uint32_t b2, uint32_t b3, uint32_t b4)
{
	struct COMPAC_INFO *info = compac->device_data;

	unsigned char req_tx[4] = { b1, b2, b3, b4 };
	unsigned char resp_tx[5];
	int read_bytes = 1;
	int read_wait = 0;

	req_tx[3] |= bmcrc(req_tx, 27);

	if (req_tx[0] == 0x84 && req_tx[3] == 0x11)
		info->chips = 0;

	if (req_tx[0] == 0x84)
		read_wait = 250;

	usb_write(compac, (char *)req_tx, 4, &read_bytes, C_REQUESTRESULTS);

	applog(LOG_WARNING, "%s %d TX: %02x %02x %02x %02x", compac->drv->name, compac->device_id, req_tx[0], req_tx[1], req_tx[2], req_tx[3]);
	while (read_bytes && read_wait > 0) {
		usb_read_timeout(compac, (char *)resp_tx, RX_RESP_SIZE, &read_bytes, read_wait, C_GETRESULTS);

		if (read_bytes == RX_RESP_SIZE) {
			applog(LOG_WARNING, "%s %d RX: %02x %02x %02x %02x %02x", compac->drv->name, compac->device_id, resp_tx[0], resp_tx[1], resp_tx[2], resp_tx[3], resp_tx[4]);
			if (req_tx[0] == 0x84 && req_tx[3] == 0x11)
				info->chips++;
		} else {
			applog(LOG_WARNING, "%s %d RX bytes(%d): ", compac->drv->name, compac->device_id, read_bytes);
		}
	}
}

static void compac_send_chain_inactive(struct cgpu_info *compac)
{
	struct COMPAC_INFO *info = compac->device_data;
	int i, interval;
	applog(LOG_INFO,"%s %d: sending chain inactive for %d chips", compac->drv->name, compac->device_id, info->chips);
	compac_send_async(compac, 0x85, 0x00, 0x00, 0x00); // chain inactive
	for (i = 0; i < info->chips; i++) {
		interval = (0x100 / info->chips) * i;
		compac_send_async(compac, 0x01, interval, 0x00, 0x00);
	}
	cgtime(&info->last_chain_inactive);
}

static void compac_set_frequency(struct cgpu_info *compac, float frequency, bool inactive)
{
	struct COMPAC_INFO *info = compac->device_data;

	uint32_t r, r1, r2, r3, p1, p2, pll;
	unsigned char f[2];

	//if (frequency > info->frequency_requested)
	//	frequency = info->frequency_requested;

	frequency = bound(frequency, 6, 500);
	frequency = ceil(100 * (frequency) / 625.0) * 6.25;

	//if (info->frequency == frequency || !info->chips)
	//	return;

	info->frequency = frequency;

	info->hashrate = info->frequency * info->chips * 55 * 1000000;
	info->fullscan_ms = 1000.0 * 0xffffffffull / info->hashrate;
	info->ticket_mask = bound(pow(2, ceil(log(info->hashrate / (2.0 * 0xffffffffull)) / log(2))) - 1, 0, 4000);
	info->difficulty = info->ticket_mask + 1;

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

	f[0] = (pll) & 0xff;
	f[1] = (pll >> 8) & 0xff;

	applog(LOG_WARNING,"%s %d: set frequency: %.2f [%02x %02x]",  compac->drv->name, compac->device_id, info->frequency, f[1], f[0]);
	applog(LOG_WARNING,"%s %d: Ticket mask set to %d", compac->drv->name, compac->device_id, info->ticket_mask);

	compac_send_async(compac, 0x82, f[1], f[0], 0x00); // Set asic frequency

	cgtime(&info->last_freq_set);

	if (inactive)
		compac_send_chain_inactive(compac);

}

static uint64_t compac_check_nonce(struct cgpu_info *compac)
{
	struct COMPAC_INFO *info = compac->device_data;
	uint32_t nonce = (info->work_rx[3] << 0) | (info->work_rx[2] << 8) | (info->work_rx[1] << 16) | (info->work_rx[0] << 24);

	uint64_t hashes = 0;
	uint32_t hwe = compac->hw_errors;
	struct timeval now;

	uint32_t job_id = info->work_rx[4] ^ 0x80;
	struct work *work = info->work[job_id];

	if (nonce == 0 || nonce == 0xffffffff || !work || job_id > MAX_JOBS) {
		return hashes;
	}

	cgtime(&now);

	info->nonces++;
	info->nonceless = 0;
	if (nonce == info->prev_nonce) {
		applog(LOG_INFO, "Dup Nonce : %08x on %s %d", nonce, compac->drv->name, compac->device_id);
		info->dups++;
		return hashes;
	}

	hashes = info->difficulty * 0xffffffffull * info->frequency_requested / info->frequency;

	info->prev_nonce = nonce;

	applog(LOG_INFO, "Device reported nonce: %08x @ %02x", nonce, info->work_rx[4]);

	cgtime(&info->last_nonce);

	work->device_diff = info->difficulty;

	if (submit_nonce(info->thr, work, nonce)) {
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
		usb_read_timeout(compac, (char *)resp, 32, &read_bytes, 10, C_REQUESTRESULTS);
	}
}

static void compac_flush_work(struct cgpu_info *compac)
{
	compac_flush_buffer(compac);
	compac_update_work(compac);
}

static void init_task(struct COMPAC_INFO *info)
{
	struct work *work = info->work[info->job_id];

	memset(info->work_tx, 0, TX_TASK_SIZE);

	if (info->active) {
		stuff_reverse(info->work_tx, work->midstate, 32);
		stuff_reverse(info->work_tx + 52, work->data + 64, 12);

		info->work_tx[39] = info->ticket_mask & 0xff;
	}

	info->work_tx[40] = (info->ramp_hcn >> 24) & 0xff;
	info->work_tx[41] = (info->ramp_hcn >> 16) & 0xff;
	info->work_tx[42] = (info->ramp_hcn >> 8)  & 0xff;
	info->work_tx[43] = (info->ramp_hcn)       & 0xff;
	info->work_tx[51] = info->job_id & 0xff;
}

static void *compac_write(void *object)
{
	struct cgpu_info *compac = (struct cgpu_info *)object;
	struct COMPAC_INFO *info = compac->device_data;
	uint32_t err = 0;
	int write_bytes;
	struct threadmsg msg;
	struct timespec timeout;
	unsigned char *buffer;

	timeout.tv_sec = 1;

	while (info->shutdown == false)
	{

		if (compac->usbinfo.nodev)
			return false;

		thread_queue_get(&info->out_queue, &timeout, &msg);

		if (msg.data) {
			if (msg.msgtype == 0) {
				err = usb_write(compac, (char *)msg.data, 4, &write_bytes, C_REQUESTRESULTS);
				if (err != LIBUSB_SUCCESS || write_bytes != 4) {
					applog(LOG_WARNING,"%s %d: Write error", compac->drv->name, compac->device_id);
				}
			}
			if (msg.msgtype == 1) {
				err = usb_write(compac, (char *)msg.data, TX_TASK_SIZE, &write_bytes, C_SENDWORK);
				if (err != LIBUSB_SUCCESS || write_bytes != TX_TASK_SIZE) {
					applog(LOG_WARNING,"%s %d: Write error", compac->drv->name, compac->device_id);
				}
			}
			free(msg.data);
			msg.data = NULL;
		}
	}

	return false;
}

static void *compac_listen(void *object)
{
	struct cgpu_info *compac = (struct cgpu_info *)object;
	struct COMPAC_INFO *info = compac->device_data;
	uint32_t err = 0;
	int read_bytes;
	int crc;

	while (info->shutdown == false)
	{
		if (compac->usbinfo.nodev)
			return false;

		if (info->active) {
			memset(info->work_rx, 0, RX_RESP_SIZE);
			err = usb_read_timeout(compac, (char *)info->work_rx, RX_RESP_SIZE, &read_bytes, 100, C_GETRESULTS);
			crc = bmcrc(info->work_rx, 35);
			if (read_bytes > 0) {
				applog(LOG_DEBUG,"rx: %02x %02x %02x %02x %02x", info->work_rx[0], info->work_rx[1], info->work_rx[2], info->work_rx[3], info->work_rx[4]);
			}
			if (info->work_rx[4] == crc) {
				applog(LOG_WARNING,"cmd rx: %02x %02x %02x %02x %02x", info->work_rx[0], info->work_rx[1], info->work_rx[2], info->work_rx[3], info->work_rx[4], crc);
				if (info->work_rx[0] == 0x80) {
					float frequency = (info->work_rx[1] + 1) * 6.25 / (1 + info->work_rx[2] & 0x0f) * pow(2, (3 - info->work_rx[3])) + ((info->work_rx[2] >> 4) * 6.25);
					applog(LOG_WARNING,"%s %d: chip reported frequency of %.2f", compac->drv->name, compac->device_id, frequency);
					info->frequency = frequency;
				}
			}
			if (read_bytes == RX_RESP_SIZE && info->work_rx[4] >= 0x80) {
				mutex_lock(&info->lock);
				info->hashes += compac_check_nonce(compac);
				mutex_unlock(&info->lock);
			}
			if (err < 0 && err != LIBUSB_ERROR_TIMEOUT) {
				applog(LOG_ERR, "%s %i: Comms error (rerr=%d amt=%d)", compac->drv->name, compac->device_id, err, read_bytes);
				dev_error(compac, REASON_DEV_COMMS_ERROR);
			}
		} else {
			cgsleep_ms(100);
		}
	}

	return false;
}

static int64_t compac_scanwork(struct thr_info *thr)
{
	struct cgpu_info *compac = thr->cgpu;
	struct COMPAC_INFO *info = compac->device_data;
	struct timeval now;

	int read_bytes = 1;
	int i, cpu_yield;
	float frequency;
	uint64_t hashes = 0;
	uint32_t err = 0;
	uint32_t hcn_max = info->hashrate * RAMP_MS / 1000;
	uint32_t max_task_wait = bound(info->fullscan_ms * 0.40, 5, 1000);

	if (compac->usbinfo.nodev)
		return -1;

	if (!info->chips) {
		usb_nodev(compac);
		return -1;
	}

	if (info->ramping < RAMP_CT)
		max_task_wait = RAMP_MS;

	hashes = info->hashes;
	info->hashes -= hashes;

	cgtime(&now);
	info->scanhash_ms = (info->scanhash_ms * 9 + ms_tdiff(&now, &info->last_scanhash)) / 10;
	cgtime(&info->last_scanhash);

	if (info->nonceless > (MAX_IDLE * 1000 / max_task_wait)) {
		if (info->failing) {
			if (info->nonceless > (2 * MAX_IDLE * 1000 / max_task_wait)) {
				applog(LOG_ERR, "%s %d: Device failed to respond to restart",
					   compac->drv->name, compac->device_id);
				if (info->ident != IDENT_BSC && info->ident != IDENT_GSC)
					usb_nodev(compac);
				return -1;
			}
		} else {
			applog(LOG_WARNING, "%s %d: No valid hashes recently, attempting to reset",
				   compac->drv->name, compac->device_id);
			usb_reset(compac);
			info->failing = true;
			return 0;
		}
	}

	if (info->update_work || (ms_tdiff(&now, &info->last_task) > max_task_wait)) {

		info->job_id = (info->job_id + 1) % MAX_JOBS;

		if (info->update_work) {
			mutex_lock(&info->lock);
			for (i = 0; i < MAX_JOBS; i++) {
				if (info->work[i])
					free_work(info->work[i]);
				info->work[i] = NULL;
			}
			mutex_unlock(&info->lock);
			info->update_work = 0;
			compac_send_async(compac, 0x84, 0x00, 0x04, 0x00); // get frequency
		}

		if (info->work[info->job_id] && info->work[info->job_id]->drv_rolllimit == 0) {
			free_work(info->work[info->job_id]);
			info->work[info->job_id] = NULL;
		}

		if (!info->work[info->job_id]) {
			info->work[info->job_id] = get_work(thr, thr->id);
		} else {
			info->work[info->job_id]->drv_rolllimit--;
			roll_work(info->work[info->job_id]);
		}

		if (info->ramping < RAMP_CT) {
			info->ramping++;
			info->ramp_hcn += hcn_max / RAMP_CT;
			info->ramp_hcn = bound(info->ramp_hcn, 0, 0xffffffff);

			cgtime(&info->last_nonce);
		} else {
			info->nonceless++;
			info->active = true;
			info->ramp_hcn = (0xffffffff / info->chips);
		}

		init_task(info);

		frequency = info->frequency;
		if (info->frequency != info->frequency_requested && ms_tdiff(&now, &info->last_freq_set) > 14 * 1000) {
			if (info->frequency < info->frequency_requested)
				frequency += 25;
			else
				frequency = info->frequency_requested;
		}
		if (frequency != info->frequency)
			compac_set_frequency(compac, frequency, true);

		if (info->dups * 2 >= info->chips && ms_tdiff(&now, &info->last_chain_inactive) > 3000) {
			info->dups = 0;
			compac_send_chain_inactive(compac);
		}

		thread_queue_add(&info->out_queue, memdup(&info->work_tx, TX_TASK_SIZE), 1);

		info->task_ms = (info->task_ms * 9 + ms_tdiff(&now, &info->last_task)) / 10;
		cgtime(&info->last_task);
	}

	cpu_yield = bound(max_task_wait / 20, 1, 100);
	cgsleep_ms(cpu_yield);

	if (compac->shutdown)
		compac_set_frequency(compac, info->frequency_start, false);

	return hashes;
}

static struct cgpu_info *compac_detect_one(struct libusb_device *dev, struct usb_find_devices *found)
{
	struct cgpu_info *compac;
	struct COMPAC_INFO *info;
	uint32_t baudrate = CP210X_DATA_BAUD;
	unsigned int bits = CP210X_BITS_DATA_8 | CP210X_BITS_PARITY_MARK;

	compac = usb_alloc_cgpu(&gekko_drv, 1);

	if (!usb_init(compac, dev, found)) {
		applog(LOG_ERR, "failed usb_init");
		compac = usb_free_cgpu(compac);
		return NULL;
	}

	applog(LOG_INFO, "%s %d: Found at %s", compac->drv->name, compac->device_id, compac->device_path);

	info = cgcalloc(1, sizeof(struct COMPAC_INFO));
	compac->device_data = (void *)info;

	info->ident = usb_ident(compac);

	switch (info->ident) {
		case IDENT_BSC:
		case IDENT_BSD:
		case IDENT_BSE:
		case IDENT_GSC:
		case IDENT_GSD:
		case IDENT_GSE:
			break;
		default:
			quit(1, "%s compac_detect_one() invalid %s ident=%d",
				compac->drv->dname, compac->drv->dname, info->ident);
	}

	info->interface = usb_interface(compac);

	usb_transfer_data(compac, CP210X_TYPE_OUT, CP210X_REQUEST_IFC_ENABLE, CP210X_VALUE_UART_ENABLE, info->interface, NULL, 0, C_ENABLE_UART);
	usb_transfer_data(compac, CP210X_TYPE_OUT, CP210X_REQUEST_DATA, CP210X_VALUE_DATA, info->interface, NULL, 0, C_SETDATA);
	usb_transfer_data(compac, CP210X_TYPE_OUT, CP210X_REQUEST_BAUD, 0, info->interface, &baudrate, sizeof (baudrate), C_SETBAUD);
	usb_transfer_data(compac, CP210X_TYPE_OUT, CP210X_SET_LINE_CTL, bits, info->interface, NULL, 0, C_SETPARITY);
	
	compac_send(compac, 0x84, 0x00, 0x04, 0x00); // get frequency
	compac_send(compac, 0x82, 0x03, 0x83, 0x1f); // set frequency 50Mhz
	compac_send(compac, 0x84, 0x00, 0x04, 0x00); // get frequency
	
	compac_send(compac, 0x84, 0x00, 0x00, 0x11); // get chain reg0x0
	
	if (!info->chips) {
		applog(LOG_INFO,"Device serial %s not responding", compac->usbdev->serial_string);
		usb_uninit(compac);
		free(info);
		compac->device_data = NULL;
		compac = usb_free_cgpu(compac);
		return NULL;
	}

	if (!add_cgpu(compac))
		quit(1, "Failed to add_cgpu in compac_detect_one");

	update_usb_stats(compac);
	return compac;
}

static void compac_detect(bool __maybe_unused hotplug)
{
	usb_detect(&gekko_drv, compac_detect_one);
}

static bool compac_prepare(struct thr_info *thr)
{
	struct cgpu_info *compac = thr->cgpu;
	struct COMPAC_INFO *info = compac->device_data;
	uint32_t i;

	info->thr = thr;

	info->nonces = 0;
	info->ramping = 1;
	info->ticket_mask = 1;
	info->ramp_hcn = 0;
	info->hashes = 0;
	info->active = false;
	info->frequency = 1;

	cgtime(&info->start_time);
	cgtime(&info->last_scanhash);
	cgtime(&info->last_nonce);
	cgtime(&info->last_task);

	pthread_mutex_init(&info->lock, NULL);
	pthread_mutex_init(&info->rx_lock, NULL);
	pthread_mutex_init(&info->tx_lock, NULL);
	
	thread_queue_init(&info->out_queue);

	memset(info->work_rx, 0, RX_RESP_SIZE);

	for (i = 0; i < 9; i++)
		compac->unique_id[i] = compac->unique_id[i+3];

	return true;
}

static bool compac_init(struct thr_info *thr)
{
	struct cgpu_info *compac = thr->cgpu;
	struct COMPAC_INFO *info = compac->device_data;

//	uint32_t baudrate = CP210X_DATA_BAUD * 2;  // Baud 230400
	uint32_t chips = info->chips;

//	compac_send(compac, 0x86, 0x10, 0x0D, 0x00); // Baud 230400
//	usb_transfer_data(compac, CP210X_TYPE_OUT, CP210X_REQUEST_BAUD, 0, info->interface, &baudrate, sizeof (baudrate), C_SETBAUD);

	compac_send(compac, 0x84, 0x00, 0x00, 0x11); // get chain reg0x0
	while (chips != info->chips) {
		chips = info->chips;
		compac_send(compac, 0x84, 0x00, 0x00, 0x11); // get chain reg0x0
	}

	applog(LOG_WARNING,"Found %d chip(s) on %s %d", info->chips, compac->drv->name, compac->device_id);
	
	if (thr_info_create(&(info->rthr), NULL, compac_listen, (void *)compac)) {
		applog(LOG_ERR, "%s-%i: read thread create failed", compac->drv->name, compac->device_id);
		return false;
	}

	if (thr_info_create(&(info->wthr), NULL, compac_write, (void *)compac)) {
		applog(LOG_ERR, "%s-%i: write thread create failed", compac->drv->name, compac->device_id);
		return false;
	}

	pthread_detach(info->rthr.pth);
	pthread_detach(info->wthr.pth);

	switch (info->ident) {
		case IDENT_BSC:
		case IDENT_GSC:
			info->frequency_requested = opt_gekko_gsc_freq;
			info->frequency_start = opt_gekko_gsc_freq;
			break;
		case IDENT_BSD:
		case IDENT_GSD:
			info->frequency_requested = opt_gekko_gsd_freq;
			info->frequency_start = opt_gekko_gsd_freq;
			break;
		case IDENT_BSE:
		case IDENT_GSE:
			info->frequency_requested = opt_gekko_gse_freq;
			info->frequency_start = BASE_FREQ;
			break;
		default:
			info->frequency_requested = BASE_FREQ;
			info->frequency_start = BASE_FREQ;
			break;
	}

	info->frequency_start = (info->frequency_requested < info->frequency_start) ? info->frequency_requested : info->frequency_start;

	compac_set_frequency(compac, info->frequency_start, true);

	return true;
}

static void compac_statline(char *buf, size_t bufsiz, struct cgpu_info *compac)
{
	struct COMPAC_INFO *info = compac->device_data;
	if (opt_log_output) {
		tailsprintf(buf, bufsiz, "COMPAC-%i %.2fMHz (%d/%d/%d/%d)", info->chips, info->frequency, info->scanhash_ms, info->task_ms, info->fullscan_ms, compac->hw_errors);
	} else {
		tailsprintf(buf, bufsiz, "COMPAC-%i %.2fMHz HW:%d", info->chips, info->frequency, compac->hw_errors);
	}
}

static struct api_data *compac_api_stats(struct cgpu_info *compac)
{
	struct COMPAC_INFO *info = compac->device_data;
	struct api_data *root = NULL;

	root = api_add_int(root, "Nonces", &info->nonces, false);
	root = api_add_int(root, "Accepted", &info->accepted, false);

	return root;
}

static void compac_shutdown(struct thr_info *thr)
{
	struct cgpu_info *compac = thr->cgpu;
	struct COMPAC_INFO *info = compac->device_data;
	//uint32_t baudrate = CP210X_DATA_BAUD;

	compac_send_async(compac, 0x82, 0x03, 0x83, 0x1f); // set frequency 50Mhz
	compac_send_async(compac, 0x84, 0x00, 0x04, 0x00); // get frequency

	info->shutdown = true;

	//compac_send(compac, 0x86, 0x10, 0x1a, 0x00); // Baud 115200
	//usb_transfer_data(compac, CP210X_TYPE_OUT, CP210X_REQUEST_BAUD, 0, info->interface, &baudrate, sizeof (baudrate), C_SETBAUD);
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
