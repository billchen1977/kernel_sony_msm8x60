/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */


#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/pcm_params.h>
#include <sound/timer.h>
#include <sound/apr_audio.h>
#include <sound/q6asm.h>
#include <sound/compress_params.h>
#include <sound/compress_offload.h>
#include <sound/compress_driver.h>
#include <asm/dma.h>
#include <linux/dma-mapping.h>

#include "msm8x60-fuji.h"

/* Default values used if user space does not set */
#define COMPR_PLAYBACK_MIN_FRAGMENT_SIZE (8 * 1024)
#define COMPR_PLAYBACK_MAX_FRAGMENT_SIZE (128 * 1024)
#define COMPR_PLAYBACK_MIN_NUM_FRAGMENTS (4)
#define COMPR_PLAYBACK_MAX_NUM_FRAGMENTS (16 * 4)

struct msm_compr_audio {
	struct snd_compr_stream *cstream;
	struct snd_compr_caps compr_cap;
	struct snd_compr_codec_caps codec_caps;
	struct snd_compr_params codec_param;
	struct audio_client *audio_client;

	uint32_t codec;
	void    *buffer; /* virtual address */
	uint32_t buffer_paddr; /* physical address */
	uint32_t buffer_size;
	uint32_t byte_offset;
	uint32_t copied_total;
	uint32_t bytes_received;

	uint16_t session_id;

	uint32_t sample_rate;
	uint32_t num_channels;

	uint32_t cmd_ack;
	uint32_t cmd_interrupt;
	uint32_t drain_ready;

	atomic_t start;
	atomic_t eos;
	atomic_t drain;
	atomic_t xrun;

	wait_queue_head_t eos_wait;
	wait_queue_head_t drain_wait;
	wait_queue_head_t flush_wait;

	spinlock_t lock;
};

static int msm_compr_send_buffer(struct msm_compr_audio *prtd)
{
	int buffer_length;
	int bytes_available;
	struct audio_aio_write_param param;

	if (!atomic_read(&prtd->start)) {
		pr_err("%s: stream is not in started state\n", __func__);
		return -EINVAL;
	}

	if (atomic_read(&prtd->xrun)) {
		WARN(1, "%s called while xrun is true", __func__);
		return -EPERM;
	}

	pr_debug("%s: bytes_received = %d copied_total = %d\n",
		__func__, prtd->bytes_received, prtd->copied_total);
	buffer_length = prtd->codec_param.buffer.fragment_size;
	bytes_available = prtd->bytes_received - prtd->copied_total;
	if (bytes_available < prtd->codec_param.buffer.fragment_size)
		buffer_length = bytes_available;

	if (prtd->byte_offset + buffer_length > prtd->buffer_size) {
		buffer_length = (prtd->buffer_size - prtd->byte_offset);
		pr_debug("wrap around situation, send partial data %d now", buffer_length);
	}

	param.paddr	= prtd->buffer_paddr + prtd->byte_offset;
	WARN(param.paddr % 32 != 0, "param.paddr %lx not multiple of 32", param.paddr);

	param.len	= buffer_length;
	param.msw_ts	= 0;
	param.lsw_ts	= 0;
	param.flags	= NO_TIMESTAMP;
	param.uid	= buffer_length;

	pr_debug("%s: sending %d bytes to DSP byte_offset = %d\n",
		__func__, buffer_length, prtd->byte_offset);
	if (q6asm_async_write(prtd->audio_client, &param) < 0)
		pr_err("%s:q6asm_async_write failed\n", __func__);

	return 0;
}

static void compr_event_handler(uint32_t opcode,
		uint32_t token, uint32_t *payload, void *priv)
{
	struct msm_compr_audio *prtd = priv;
	struct snd_compr_stream *cstream = prtd->cstream;
	uint32_t chan_mode = 0;
	uint32_t sample_rate = 0;
	int bytes_available;

	pr_debug("%s opcode =%08x\n", __func__, opcode);
	switch (opcode) {
	case ASM_DATA_EVENT_WRITE_DONE:
		spin_lock(&prtd->lock);

		prtd->byte_offset += prtd->codec_param.buffer.fragment_size;
		prtd->copied_total += prtd->codec_param.buffer.fragment_size;
		if (prtd->byte_offset >= prtd->buffer_size)
			prtd->byte_offset -= prtd->buffer_size;

		snd_compr_fragment_elapsed(cstream);

		if (!atomic_read(&prtd->start)) {
			/* Writes must be restarted from _copy() */
			pr_debug("write_done received while not started, treat as xrun");
			atomic_set(&prtd->xrun, 1);
			spin_unlock(&prtd->lock);
			break;
		}

		bytes_available = prtd->bytes_received - prtd->copied_total;
		if (bytes_available < cstream->runtime->fragment_size) {
			pr_debug("WRITE_DONE Insufficient data to send. break out\n");
			atomic_set(&prtd->xrun, 1);

			if (atomic_read(&prtd->drain)) {
				pr_debug("wake up on drain");
				prtd->drain_ready = 1;
				wake_up(&prtd->drain_wait);
				atomic_set(&prtd->drain, 0);
			}
		} else
			msm_compr_send_buffer(prtd);

		spin_unlock(&prtd->lock);
		break;
	case ASM_DATA_CMDRSP_EOS:
		pr_debug("ASM_DATA_CMDRSP_EOS\n");
		if (atomic_read(&prtd->eos)) {
			pr_debug("ASM_DATA_CMDRSP_EOS wake up\n");
			prtd->cmd_ack = 1;
			wake_up(&prtd->eos_wait);
			atomic_set(&prtd->eos, 0);
		}
		break;
	case ASM_DATA_EVENT_SR_CM_CHANGE_NOTIFY:
		pr_debug("ASM_DATA_EVENT_SR_CM_CHANGE_NOTIFY\n");
		chan_mode = payload[1] >> 16;
		sample_rate = payload[2] >> 16;
		if (prtd && (chan_mode != prtd->num_channels ||
				sample_rate != prtd->sample_rate)) {
			prtd->num_channels = chan_mode;
			prtd->sample_rate = sample_rate;
		}
	    break;
	case APR_BASIC_RSP_RESULT: {
		switch (payload[0]) {
		case ASM_SESSION_CMD_RUN:
			/* check if the first buffer need to be sent to DSP */
			pr_debug("ASM_SESSION_CMD_RUN\n");

			spin_lock(&prtd->lock);
			/* FIXME: A state is a much better way of dealing with this */
			if (!prtd->copied_total) {
				bytes_available = prtd->bytes_received - prtd->copied_total;
				if (bytes_available < cstream->runtime->fragment_size) {
					pr_debug("CMD_RUN Insufficient data to send. break out\n");
					atomic_set(&prtd->xrun, 1);
				} else
					msm_compr_send_buffer(prtd);
			}
			spin_unlock(&prtd->lock);
			break;
		case ASM_STREAM_CMD_FLUSH:
			pr_debug("ASM_STREAM_CMD_FLUSH\n");
			prtd->cmd_ack = 1;
			wake_up(&prtd->flush_wait);
			break;
		default:
			break;
		}
		break;
	}
	default:
		pr_debug("Not Supported Event opcode[0x%x]\n", opcode);
		break;
	}
}

static void populate_codec_list(struct msm_compr_audio *prtd)
{
	pr_debug("%s\n", __func__);
	prtd->compr_cap.direction = SND_COMPRESS_PLAYBACK;
	prtd->compr_cap.min_fragment_size =
			COMPR_PLAYBACK_MIN_FRAGMENT_SIZE;
	prtd->compr_cap.max_fragment_size =
			COMPR_PLAYBACK_MAX_FRAGMENT_SIZE;
	prtd->compr_cap.min_fragments =
			COMPR_PLAYBACK_MIN_NUM_FRAGMENTS;
	prtd->compr_cap.max_fragments =
			COMPR_PLAYBACK_MAX_NUM_FRAGMENTS;
	prtd->compr_cap.num_codecs = 4;
	prtd->compr_cap.codecs[0] = SND_AUDIOCODEC_MP3;
	prtd->compr_cap.codecs[1] = SND_AUDIOCODEC_AAC;
	prtd->compr_cap.codecs[2] = SND_AUDIOCODEC_WMA;
	prtd->compr_cap.codecs[3] = SND_AUDIOCODEC_WMA_PRO;
}

static int msm_compr_send_media_format_block(struct snd_compr_stream *cstream)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct msm_compr_audio *prtd = runtime->private_data;
	struct asm_aac_cfg aac_cfg;
	struct asm_wma_cfg wma_cfg;
	struct asm_wmapro_cfg wma_pro_cfg;
	int ret = 0;

	switch (prtd->codec) {
	case FORMAT_MP3:
		/* no media format block needed */
		break;
	case FORMAT_MPEG4_AAC:
	case FORMAT_MPEG4_MULTI_AAC:
		memset(&aac_cfg, 0x0, sizeof(struct asm_aac_cfg));
		aac_cfg.aot = AAC_ENC_MODE_EAAC_P;
/*
		switch (prtd->codec_param.codec.format) {
		case SND_AUDIOSTREAMFORMAT_MP4ADTS:
			aac_cfg.format = 0x00;
			break;
		case SND_AUDIOSTREAMFORMAT_MP4LOAS:
			aac_cfg.format = 0x01;
			break;
		case SND_AUDIOSTREAMFORMAT_ADIF:
			aac_cfg.format = 0x02;
			break;
		default:
			aac_cfg.format = 0x03;
		}
*/
		aac_cfg.format = 0x03;
		aac_cfg.ch_cfg = prtd->num_channels;
		aac_cfg.sample_rate = prtd->sample_rate;
		if (prtd->codec == FORMAT_MPEG4_AAC)
		    ret = q6asm_media_format_block_aac(prtd->audio_client, &aac_cfg);
	    else
	        ret = q6asm_media_format_block_multi_aac(prtd->audio_client, &aac_cfg);
		if (ret < 0)
			pr_err("%s: CMD Format block failed\n", __func__);
		break;
	case FORMAT_WMA_V9:
		memset(&wma_cfg, 0x0, sizeof(struct asm_wma_cfg));
		wma_cfg.format_tag = prtd->codec_param.codec.format;
		wma_cfg.ch_cfg = prtd->codec_param.codec.ch_in;
		wma_cfg.sample_rate = prtd->codec_param.codec.sample_rate;
		wma_cfg.avg_bytes_per_sec =
			prtd->codec_param.codec.bit_rate/8;
		wma_cfg.block_align = prtd->codec_param.codec.align;
		wma_cfg.valid_bits_per_sample =
		    prtd->codec_param.codec.options.wma.bits_per_sample;
		wma_cfg.ch_mask =
			prtd->codec_param.codec.options.wma.channelmask;
		wma_cfg.encode_opt =
			prtd->codec_param.codec.options.wma.encodeopt;
		ret = q6asm_media_format_block_wma(prtd->audio_client,
					&wma_cfg);
		if (ret < 0)
			pr_err("%s: CMD Format block failed\n", __func__);
		break;
	case FORMAT_WMA_V10PRO:
		memset(&wma_pro_cfg, 0x0, sizeof(struct asm_wmapro_cfg));
		wma_pro_cfg.format_tag = prtd->codec_param.codec.format;
		wma_pro_cfg.ch_cfg = prtd->codec_param.codec.ch_in;
		wma_pro_cfg.sample_rate =
			prtd->codec_param.codec.sample_rate;
		wma_pro_cfg.avg_bytes_per_sec =
			prtd->codec_param.codec.bit_rate/8;
		wma_pro_cfg.block_align = prtd->codec_param.codec.align;
		wma_pro_cfg.valid_bits_per_sample =
			prtd->codec_param.codec.options.wma.bits_per_sample;
		wma_pro_cfg.ch_mask =
			prtd->codec_param.codec.options.wma.channelmask;
		wma_pro_cfg.encode_opt =
			prtd->codec_param.codec.options.wma.encodeopt;
		wma_pro_cfg.adv_encode_opt =
			prtd->codec_param.codec.options.wma.encodeopt1;
		wma_pro_cfg.adv_encode_opt2 =
			prtd->codec_param.codec.options.wma.encodeopt2;
		ret = q6asm_media_format_block_wmapro(prtd->audio_client,
				&wma_pro_cfg);
		if (ret < 0)
			pr_err("%s: CMD Format block failed\n", __func__);
		break;
	default:
		pr_debug("%s, unsupported format, skip", __func__);
		break;
	}
	return ret;
}

static int msm_compr_configure_dsp(struct snd_compr_stream *cstream)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct msm_compr_audio *prtd = runtime->private_data;
	int dir = IN, ret = 0;
	struct asm_softpause_params softpause = {
		.enable = SOFT_PAUSE_ENABLE,
		.period = SOFT_PAUSE_PERIOD,
		.step = SOFT_PAUSE_STEP,
		.rampingcurve = SOFT_PAUSE_CURVE_LINEAR,
	};
	struct asm_softvolume_params softvol = {
		.period = SOFT_VOLUME_PERIOD,
		.step = SOFT_VOLUME_STEP,
		.rampingcurve = SOFT_VOLUME_CURVE_LINEAR,
	};

	pr_debug("%s\n", __func__);
	ret = q6asm_open_write(prtd->audio_client,
				prtd->codec);
	if (ret < 0) {
		pr_err("%s: Session out open failed\n", __func__);
		 return -ENOMEM;
	}

	ret = q6asm_set_softpause(prtd->audio_client,
					&softpause);
	if (ret < 0)
		pr_err("%s: Send SoftPause Param failed ret=%d\n",
			__func__, ret);

	ret = q6asm_set_softvolume(prtd->audio_client, &softvol);
	if (ret < 0)
		pr_err("%s: Send SoftVolume Param failed ret=%d\n",
			__func__, ret);

	ret = q6asm_set_io_mode(prtd->audio_client, ASYNC_IO_MODE);
	if (ret < 0) {
		pr_err("%s: Set IO mode failed\n", __func__);
		return -EINVAL;
	}
	
	msm_session_open(SESSION_COMPRESS, cstream->direction,
        prtd->audio_client);

	runtime->fragments = prtd->codec_param.buffer.fragments;
	runtime->fragment_size = prtd->codec_param.buffer.fragment_size;
	pr_debug("allocate %d buffers each of size %d\n",
			runtime->fragments,
			runtime->fragment_size);
	ret = q6asm_audio_client_buf_alloc_contiguous(dir,
					prtd->audio_client,
					runtime->fragment_size,
					runtime->fragments);
	if (ret < 0) {
		pr_err("Audio Start: Buffer Allocation failed rc = %d\n", ret);
		return -ENOMEM;
	}

	prtd->byte_offset  = 0;
	prtd->copied_total = 0;
	prtd->bytes_received = 0;
	prtd->buffer       = prtd->audio_client->port[dir].buf[0].data;
	prtd->buffer_paddr = prtd->audio_client->port[dir].buf[0].phys;
	prtd->buffer_size  = runtime->fragments * runtime->fragment_size;

	ret = msm_compr_send_media_format_block(cstream);

	if (ret < 0) {
		pr_err("%s, failed to send media format block\n", __func__);
	}

	return ret;
}

static int msm_compr_open(struct snd_compr_stream *cstream)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct msm_compr_audio *prtd;

	pr_debug("%s\n", __func__);
	prtd = kzalloc(sizeof(struct msm_compr_audio), GFP_KERNEL);
	if (prtd == NULL) {
		pr_err("Failed to allocate memory for msm_compr_audio\n");
		return -ENOMEM;
	}

	prtd->cstream = cstream;
	prtd->audio_client = q6asm_audio_client_alloc(
				(app_cb)compr_event_handler, prtd);
	if (!prtd->audio_client) {
		pr_err("%s: Could not allocate memory\n", __func__);
		kfree(prtd);
		return -ENOMEM;
	}

	pr_debug("%s: session ID %d\n", __func__, prtd->audio_client->session);
	prtd->audio_client->perf_mode = false;
	prtd->session_id = prtd->audio_client->session;
	prtd->codec = FORMAT_MP3;
	prtd->bytes_received = 0;
	prtd->copied_total = 0;
	prtd->byte_offset = 0;
	prtd->sample_rate = 44100;
	prtd->num_channels = 2;
	prtd->drain_ready = 0;

	spin_lock_init(&prtd->lock);

	atomic_set(&prtd->eos, 0);
	atomic_set(&prtd->start, 0);
	atomic_set(&prtd->drain, 0);
	atomic_set(&prtd->xrun, 0);

	init_waitqueue_head(&prtd->eos_wait);
	init_waitqueue_head(&prtd->drain_wait);
	init_waitqueue_head(&prtd->flush_wait);

	runtime->private_data = prtd;
	populate_codec_list(prtd);

	return 0;
}

static int msm_compr_free(struct snd_compr_stream *cstream)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct msm_compr_audio *prtd = runtime->private_data;
	int dir = IN, ret = 0;

	pr_debug("%s\n", __func__);

	if (atomic_read(&prtd->eos)) {
		ret = wait_event_timeout(prtd->eos_wait,
					 prtd->cmd_ack, 5 * HZ);
		if (!ret)
			pr_err("%s: CMD_EOS failed\n", __func__);
	}

	q6asm_cmd(prtd->audio_client, CMD_CLOSE);

	q6asm_audio_client_buf_free_contiguous(dir,
					prtd->audio_client);

	msm_session_close(SESSION_COMPRESS, SNDRV_PCM_STREAM_PLAYBACK);

	q6asm_audio_client_free(prtd->audio_client);

	kfree(prtd);

	return 0;
}

/* compress stream operations */
static int msm_compr_set_params(struct snd_compr_stream *cstream,
				struct snd_compr_params *params)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct msm_compr_audio *prtd = runtime->private_data;
	int ret = 0;

	pr_debug("%s\n", __func__);

	memcpy(&prtd->codec_param, params, sizeof(struct snd_compr_params));

	/* ToDo: remove duplicates */
	prtd->num_channels = prtd->codec_param.codec.ch_in;

	switch (prtd->codec_param.codec.sample_rate) {
	case SNDRV_PCM_RATE_8000:
		prtd->sample_rate = 8000;
		break;
	case SNDRV_PCM_RATE_11025:
		prtd->sample_rate = 11025;
		break;
	/* ToDo: What about 12K and 24K sample rates ? */
	case SNDRV_PCM_RATE_16000:
		prtd->sample_rate = 16000;
		break;
	case SNDRV_PCM_RATE_22050:
		prtd->sample_rate = 22050;
		break;
	case SNDRV_PCM_RATE_32000:
		prtd->sample_rate = 32000;
		break;
	case SNDRV_PCM_RATE_44100:
		prtd->sample_rate = 44100;
		break;
	case SNDRV_PCM_RATE_48000:
		prtd->sample_rate = 48000;
		break;
	}

	pr_debug("%s: sample_rate %d\n", __func__, prtd->sample_rate);

	switch (params->codec.id) {
	case SND_AUDIOCODEC_MP3:
		pr_debug("SND_AUDIOCODEC_MP3\n");
		prtd->codec = FORMAT_MP3;
		break;
	case SND_AUDIOCODEC_AAC:
		pr_debug("SND_AUDIOCODEC_AAC\n");
		if (prtd->num_channels > 2)
		    prtd->codec = FORMAT_MPEG4_MULTI_AAC;
		else
		    prtd->codec = FORMAT_MPEG4_AAC;
		break;
	case SND_AUDIOCODEC_WMA:
		pr_debug("SND_AUDIOCODEC_WMA\n");
		prtd->codec = FORMAT_WMA_V9;
		break;
	case SND_AUDIOCODEC_WMA_PRO:
		pr_debug("SND_AUDIOCODEC_WMA_PRO\n");
		prtd->codec = FORMAT_WMA_V10PRO;
		break;
	default:
		pr_err("codec not supported, id =%d\n", params->codec.id);
		return -EINVAL;
	}

	ret = msm_compr_configure_dsp(cstream);

	return ret;
}

static int msm_compr_trigger(struct snd_compr_stream *cstream, int cmd)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct msm_compr_audio *prtd = runtime->private_data;
	int rc = 0;
	int bytes_to_write;
	unsigned long flags;

	if (cstream->direction != SND_COMPRESS_PLAYBACK) {
		pr_err("%s: Unsupported stream type\n", __func__);
		return -EINVAL;
	}

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		pr_debug("%s: SNDRV_PCM_TRIGGER_START\n", __func__);
		q6asm_run_nowait(prtd->audio_client, 0, 0, 0);
		atomic_set(&prtd->start, 1);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		pr_debug("%s: SNDRV_PCM_TRIGGER_STOP\n", __func__);
		spin_lock_irqsave(&prtd->lock, flags);

		atomic_set(&prtd->start, 0);
		if (atomic_read(&prtd->eos)) {
			pr_debug("%s: interrupt drain and eos wait queues", __func__);
			prtd->cmd_interrupt = 1;
			prtd->drain_ready = 1;
			wake_up(&prtd->drain_wait);
			wake_up(&prtd->eos_wait);
			atomic_set(&prtd->eos, 0);
		}

		pr_debug("issue CMD_FLUSH \n");
		prtd->cmd_ack = 0;
		spin_unlock_irqrestore(&prtd->lock, flags);
		rc = q6asm_cmd(prtd->audio_client, CMD_FLUSH);
		if (rc < 0) {
			pr_err("%s: flush cmd failed rc=%d\n",
			       __func__, rc);
			return rc;
		}
		rc = wait_event_timeout(prtd->flush_wait,
					prtd->cmd_ack, 1 * HZ);
		if (!rc) {
			rc = -ETIMEDOUT;
			pr_err("Flush cmd timeout\n");
		} else
			rc = 0; /* prtd->cmd_status == OK? 0 : -EPERM */

		spin_lock_irqsave(&prtd->lock, flags);
		/* FIXME. only reset if flush was successful */
		prtd->byte_offset  = 0;
		prtd->copied_total = 0;
		prtd->bytes_received = 0;
		atomic_set(&prtd->xrun, 0);
		spin_unlock_irqrestore(&prtd->lock, flags);
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		pr_debug("SNDRV_PCM_TRIGGER_PAUSE_PUSH\n");
		q6asm_cmd_nowait(prtd->audio_client, CMD_PAUSE);
		atomic_set(&prtd->start, 0);
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		pr_debug("SNDRV_PCM_TRIGGER_PAUSE_RELEASE\n");
		atomic_set(&prtd->start, 1);
		q6asm_run_nowait(prtd->audio_client, 0, 0, 0);
		break;
	case SND_COMPR_TRIGGER_PARTIAL_DRAIN:
		pr_debug("%s: SND_COMPR_TRIGGER_PARTIAL_DRAIN\n", __func__);
	case SND_COMPR_TRIGGER_DRAIN:
		pr_debug("%s: SNDRV_COMPRESS_DRAIN\n", __func__);
		/* Make sure all the data is sent to DSP before sending EOS */
		spin_lock_irqsave(&prtd->lock, flags);

		if (!atomic_read(&prtd->start)) {
			pr_err("%s: stream is not in started state\n",
				__func__);
			rc = -EPERM;
			spin_unlock_irqrestore(&prtd->lock, flags);
			break;
		}
		atomic_set(&prtd->eos, 1);

		if (prtd->bytes_received > prtd->copied_total) {
			atomic_set(&prtd->drain, 1);
			prtd->drain_ready = 0;
			spin_unlock_irqrestore(&prtd->lock, flags);
			pr_debug("%s: wait till all the data is sent to dsp\n",
				__func__);

			/*
			 * FIXME: Bug.
			 * Write(32767)
			 * Start
			 * Drain <- Indefinite wait
			 * sol1 : if (prtd->copied_total) then wait?
			 * sol2 : prtd->cmd_interrupt || prtd->drain_ready || atomic_read(xrun)
			 */
			rc = wait_event_interruptible(prtd->drain_wait,
						      prtd->cmd_interrupt || prtd->drain_ready ||
						      atomic_read(&prtd->xrun));

			spin_lock_irqsave(&prtd->lock, flags);
			if (!prtd->cmd_interrupt) {
				bytes_to_write = prtd->bytes_received - prtd->copied_total;
				WARN(bytes_to_write > runtime->fragment_size,
				     "last write %d cannot be > than fragment_size",
				     bytes_to_write);

				if (bytes_to_write > 0) {
					pr_debug("%s: send %d partial bytes at the end",
					       __func__, bytes_to_write);
					atomic_set(&prtd->xrun, 0);
					msm_compr_send_buffer(prtd);
				}
			}
		}

		if (!atomic_read(&prtd->start) || prtd->cmd_interrupt) {
			pr_debug("%s: stream is not started (interrupted by flush?)\n", __func__);
			rc = -EINTR;
			prtd->cmd_interrupt = 0;
			spin_unlock_irqrestore(&prtd->lock, flags);
			break;
		}

		pr_debug("%s: CMD_EOS\n", __func__);

		prtd->cmd_ack = 0;
		q6asm_cmd_nowait(prtd->audio_client, CMD_EOS);

		spin_unlock_irqrestore(&prtd->lock, flags);

/*
		if (cmd == SND_COMPR_TRIGGER_PARTIAL_DRAIN) {
			pr_err("PARTIAL DRAIN, do not wait for EOS ack");
			break;
		}
*/

		/* Wait indefinitely for  DRAIN. Flush can also signal this*/
		rc = wait_event_interruptible(prtd->eos_wait,
					      (prtd->cmd_ack || prtd->cmd_interrupt));

		if (rc < 0)
			pr_err("%s: EOS wait failed\n", __func__);

		pr_debug("%s: SNDRV_COMPRESS_DRAIN  out of wait for EOS\n", __func__);

		if (prtd->cmd_interrupt)
			rc = -EINTR;

		/*FIXME : what if a flush comes while PC is here */
		if (rc == 0 && (cmd == SND_COMPR_TRIGGER_PARTIAL_DRAIN)) {
			spin_lock_irqsave(&prtd->lock, flags);
			pr_debug("%s: issue CMD_PAUSE ", __func__);
			q6asm_cmd_nowait(prtd->audio_client, CMD_PAUSE);
			prtd->cmd_ack = 0;
			spin_unlock_irqrestore(&prtd->lock, flags);
			pr_debug("%s: issue CMD_FLUSH", __func__);
			q6asm_cmd(prtd->audio_client, CMD_FLUSH);
			wait_event_timeout(prtd->flush_wait,
					   prtd->cmd_ack, 1 * HZ / 4);

			spin_lock_irqsave(&prtd->lock, flags);
			prtd->byte_offset = 0;
			/* Don't reset these as these vars map
			   to total_bytes_transferred and total_bytes_available directly,
			   only total_bytes_transferred will be updated in the next avail()
			   ioctl
			   prtd->copied_total = 0;
			   prtd->bytes_received = 0;
			*/
			atomic_set(&prtd->drain, 0);
			atomic_set(&prtd->xrun, 1);
			pr_debug("%s: issue CMD_RESUME", __func__);
			q6asm_run_nowait(prtd->audio_client, 0, 0, 0);
			spin_unlock_irqrestore(&prtd->lock, flags);
		}
		pr_debug("%s: out of drain", __func__);

		prtd->cmd_interrupt = 0;
		break;
	case SND_COMPR_TRIGGER_NEXT_TRACK:
		pr_debug("%s: SND_COMPR_TRIGGER_NEXT_TRACK\n", __func__);
		break;
	}

	return rc;
}

static int msm_compr_pointer(struct snd_compr_stream *cstream,
				struct snd_compr_tstamp *arg)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct msm_compr_audio *prtd = runtime->private_data;
	struct snd_compr_tstamp tstamp;
	uint64_t timestamp = 0;
	int rc = 0;
	unsigned long flags;

	pr_debug("%s\n", __func__);
	memset(&tstamp, 0x0, sizeof(struct snd_compr_tstamp));

	spin_lock_irqsave(&prtd->lock, flags);
	tstamp.sampling_rate = prtd->sample_rate;
	tstamp.byte_offset = prtd->byte_offset;
	tstamp.copied_total = prtd->copied_total;
	spin_unlock_irqrestore(&prtd->lock, flags);

	/*
	 Query timestamp from DSP if some data is with it.
	 This prevents timeouts.
	*/
	if (prtd->copied_total) {
		rc = q6asm_get_session_time(prtd->audio_client, &timestamp);
		if (rc < 0) {
			pr_err("%s: Get Session Time return value =%lld\n",
				__func__, timestamp);
			return -EAGAIN;
		}
	}

	/* DSP returns timestamp in usec */
	pr_debug("%s: timestamp = %lld usec\n", __func__, timestamp);
	timestamp *= prtd->sample_rate;
	tstamp.pcm_io_frames = (snd_pcm_uframes_t)div64_u64(timestamp, 1000000);
	memcpy(arg, &tstamp, sizeof(struct snd_compr_tstamp));

	return 0;
}

static int msm_compr_copy(struct snd_compr_stream *cstream,
			  const char __user *buf, size_t count)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct msm_compr_audio *prtd = runtime->private_data;
	void *dstn;
	size_t copy;
	size_t bytes_available = 0;
	unsigned long flags;

	pr_debug("%s: count = %d\n", __func__, count);
	if (!prtd->buffer) {
		pr_err("%s: Buffer is not allocated yet ??", __func__);
		return 0;
	}

	dstn = prtd->buffer + runtime->app_pointer;
	if (count < prtd->buffer_size - runtime->app_pointer) {
		if (copy_from_user(dstn, buf, count))
			return -EFAULT;
		runtime->app_pointer += count;
	} else {
		copy = prtd->buffer_size - runtime->app_pointer;
		if (copy_from_user(dstn, buf, copy))
			return -EFAULT;
		if (copy_from_user(prtd->buffer, buf + copy, count - copy))
			return -EFAULT;
		runtime->app_pointer = count - copy;
	}

	/*
	 * If stream is started and there has been an xrun,
	 * since the available bytes fits fragment_size, copy the data right away
	 */
	spin_lock_irqsave(&prtd->lock, flags);

	prtd->bytes_received += count;
	if (atomic_read(&prtd->start)) {
		if (atomic_read(&prtd->xrun)) {
			pr_debug("%s: in xrun, count = %d\n", __func__, count);
			bytes_available = prtd->bytes_received - prtd->copied_total;
			if (bytes_available >= runtime->fragment_size) {
				pr_debug("%s: handle xrun, bytes_to_write = %d\n",
					 __func__,
					 bytes_available);
				atomic_set(&prtd->xrun, 0);
				msm_compr_send_buffer(prtd);
			} /* else not sufficient data */
		} /* writes will continue on the next write_done */
	}

	spin_unlock_irqrestore(&prtd->lock, flags);

	return count;
}

static int msm_compr_get_caps(struct snd_compr_stream *cstream,
				struct snd_compr_caps *arg)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct msm_compr_audio *prtd = runtime->private_data;

	pr_debug("%s\n", __func__);
	memcpy(arg, &prtd->compr_cap, sizeof(struct snd_compr_caps));

	return 0;
}

static int msm_compr_get_codec_caps(struct snd_compr_stream *cstream,
				struct snd_compr_codec_caps *codec)
{
	pr_debug("%s\n", __func__);

	switch (codec->codec) {
	case SND_AUDIOCODEC_MP3:
		codec->num_descriptors = 1;
		codec->descriptor[0].max_ch = 2;
		codec->descriptor[0].sample_rates = SNDRV_PCM_RATE_8000_48000;
		codec->descriptor[0].bit_rate[0] = 320; /* 320kbps */
		codec->descriptor[0].bit_rate[1] = 128;
		codec->descriptor[0].num_bitrates = 2;
		codec->descriptor[0].profiles = 0;
		codec->descriptor[0].modes =
		    (SND_AUDIOCHANMODE_MP3_MONO | SND_AUDIOCHANMODE_MP3_STEREO |
		        SND_AUDIOCHANMODE_MP3_JOINTSTEREO| SND_AUDIOCHANMODE_MP3_DUAL);
		codec->descriptor[0].formats = 0;
		break;
	case SND_AUDIOCODEC_AAC:
		codec->num_descriptors = 1;
		codec->descriptor[0].max_ch = 8;
		codec->descriptor[0].sample_rates = SNDRV_PCM_RATE_8000_48000;
		codec->descriptor[0].bit_rate[0] = 320; /* 320kbps */
		codec->descriptor[0].bit_rate[1] = 128;
		codec->descriptor[0].num_bitrates = 2;
		codec->descriptor[0].profiles = 0;
		codec->descriptor[0].modes = 0;
		codec->descriptor[0].formats =
			(SND_AUDIOSTREAMFORMAT_MP4ADTS | SND_AUDIOSTREAMFORMAT_MP4LOAS |
				SND_AUDIOSTREAMFORMAT_ADIF | SND_AUDIOSTREAMFORMAT_RAW);
		break;
	case SND_AUDIOCODEC_WMA:
		codec->num_descriptors = 1;
		codec->descriptor[0].max_ch = 2;
		codec->descriptor[0].sample_rates = SNDRV_PCM_RATE_8000_48000;
		codec->descriptor[0].bit_rate[0] = 320; /* 320kbps */
		codec->descriptor[0].bit_rate[1] = 128;
		codec->descriptor[0].num_bitrates = 2;
		codec->descriptor[0].profiles =
		    (SND_AUDIOPROFILE_WMA7 | SND_AUDIOPROFILE_WMA8 | 
		        SND_AUDIOPROFILE_WMA9);
		codec->descriptor[0].modes =
		    (SND_AUDIOMODE_WMA_LEVEL1 | SND_AUDIOMODE_WMA_LEVEL2 |
		        SND_AUDIOMODE_WMA_LEVEL3 | SND_AUDIOMODE_WMA_LEVEL4);
		codec->descriptor[0].formats = SND_AUDIOSTREAMFORMAT_WMA_ASF;
		break;
	case SND_AUDIOCODEC_WMA_PRO:
		codec->num_descriptors = 1;
		codec->descriptor[0].max_ch = 8;
		codec->descriptor[0].sample_rates = SNDRV_PCM_RATE_8000_48000;
		codec->descriptor[0].bit_rate[0] = 320; /* 320kbps */
		codec->descriptor[0].bit_rate[1] = 128;
		codec->descriptor[0].num_bitrates = 2;
		codec->descriptor[0].profiles = SND_AUDIOPROFILE_WMA10;
		codec->descriptor[0].modes =
		    (SND_AUDIOMODE_WMAPRO_LEVELM0 | SND_AUDIOMODE_WMAPRO_LEVELM1 |
		        SND_AUDIOMODE_WMAPRO_LEVELM2 | SND_AUDIOMODE_WMAPRO_LEVELM3);
		codec->descriptor[0].formats = SND_AUDIOSTREAMFORMAT_WMA_ASF;
		break;
	default:
		pr_err("%s: Unsupported audio codec %d\n",
			__func__, codec->codec);
		return -EINVAL;
	}

	return 0;
}

static int msm_compr_set_metadata(struct snd_compr_stream *cstream,
				struct snd_compr_metadata *metadata)
{
	pr_debug("%s\n", __func__);

	if (!metadata || !cstream)
		return -EINVAL;

	if (metadata->key == SNDRV_COMPRESS_ENCODER_PADDING) {
		pr_debug("%s, got encoder padding %u", __func__, metadata->value[0]);
	} else if (metadata->key == SNDRV_COMPRESS_ENCODER_DELAY) {
		pr_debug("%s, got encoder delay %u", __func__, metadata->value[0]);
	}

	return 0;
}

static struct snd_compr_ops msm_compr_ops = {
	.open		= msm_compr_open,
	.free		= msm_compr_free,
	.trigger	= msm_compr_trigger,
	.pointer	= msm_compr_pointer,
	.set_params	= msm_compr_set_params,
	.set_metadata	= msm_compr_set_metadata,
	.copy		= msm_compr_copy,
	.get_caps	= msm_compr_get_caps,
	.get_codec_caps = msm_compr_get_codec_caps,
};

static struct snd_soc_platform_driver msm_soc_platform = {
	.compr_ops	= &msm_compr_ops,
};

static __devinit int msm_compr_dev_probe(struct platform_device *pdev)
{
	if (pdev->dev.of_node)
		dev_set_name(&pdev->dev, "%s", "msm-compress-dsp");

	pr_debug("%s: dev name %s\n", __func__, dev_name(&pdev->dev));
	return snd_soc_register_platform(&pdev->dev,
					&msm_soc_platform);
}

static int msm_compr_remove(struct platform_device *pdev)
{
	snd_soc_unregister_platform(&pdev->dev);
	return 0;
}

static struct platform_driver msm_compr_driver = {
	.driver = {
		.name = "msm-compr-dsp",
		.owner = THIS_MODULE,
	},
	.probe = msm_compr_dev_probe,
	.remove = __devexit_p(msm_compr_remove),
};

static int __init msm_soc_platform_init(void)
{
	return platform_driver_register(&msm_compr_driver);
}
module_init(msm_soc_platform_init);

static void __exit msm_soc_platform_exit(void)
{
	platform_driver_unregister(&msm_compr_driver);
}
module_exit(msm_soc_platform_exit);

MODULE_DESCRIPTION("Compress Offload platform driver");
MODULE_LICENSE("GPL v2");
