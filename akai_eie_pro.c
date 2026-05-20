// SPDX-License-Identifier: GPL-2.0-only
/*
 * AKAI EIE Pro USB Audio Driver
 * Based on mmm444/eie-pro-linux working driver
 * Enhanced for stable audio streaming with noise reduction
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/kernel.h>

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/control.h>

MODULE_DESCRIPTION("AKAI EIE Pro USB Audio Driver");
MODULE_AUTHOR("Enhanced by Jakob, based on Michal Rydlo <michal.rydlo@gmail.com>");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

static DEFINE_MUTEX(devices_mutex);
static unsigned int devices_used;
static struct usb_driver eie_driver;

#define SYNC_URB_CNT 2
#define PLAY_URB_CNT 4
#define PLAY_PKT_CNT 40
#define CAP_URB_CNT 2

#define BYTES_PER_FRAME 12

enum {
	PLAYBACK_RUNNING,
	CAPTURE_RUNNING,
	URBS_FLOWING,
	DISCONNECTED,
};

struct eie_playback_urb {
	struct eie *eie;
	struct urb *urb;
	bool silent;
	unsigned int len; /* in frames */
};

struct eie {
	struct usb_device *udev;
	struct usb_interface *ifa;
	struct usb_interface *ifb;

	struct snd_card *card;
	unsigned int card_index;
	struct snd_pcm *pcm;
	unsigned int rate;

	__u8 sync_endpoint_addr;
	size_t sync_packet_size;
	struct urb *sync_urbs[SYNC_URB_CNT];

	__u8 cap_endpoint_addr;
	size_t cap_packet_size;

	__u8 play_endpoint_addr;
	size_t play_packet_size;
	struct eie_playback_urb play_urbs[PLAY_URB_CNT];
	struct snd_pcm_substream *play_substream;
	wait_queue_head_t urbs_flow_wait;

	unsigned int play_buf_pos;
	unsigned int played_frames;
	unsigned char wanted_idx;

	struct urb *cap_urbs[CAP_URB_CNT];
	struct snd_pcm_substream *cap_substream;
	unsigned int cap_buf_pos;
	unsigned int cap_frames;
	unsigned int cap_frame;          /* Current frame position in ALSA buffer */
	unsigned int cap_period_pos;     /* Position within current period */

	atomic_t frames_elapsed; /**< frames elapsed as reported by EIE */
	spinlock_t lock;
	unsigned long states;
};

/* Forward declarations */
static int reset_eie(struct eie *eie, unsigned int rate);

static struct snd_pcm_hardware eie_playback_hw = {
	.info = (SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BATCH |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_FIFO_IN_FRAMES),
	.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_3LE,
	.rates = (SNDRV_PCM_RATE_44100 |
		SNDRV_PCM_RATE_48000 |
		SNDRV_PCM_RATE_88200 |
		SNDRV_PCM_RATE_96000),
	.rate_min = 44100,
	.rate_max = 96000,
	.channels_min = 4,
	.channels_max = 4,
	.buffer_bytes_max = 1024 * 1024,  /* 1MB buffer */
	.period_bytes_min = 64*BYTES_PER_FRAME,
	.period_bytes_max = 8192*BYTES_PER_FRAME,  /* Limit max period size */
	.periods_min = 4,  /* More periods for smoother playback */
	.periods_max = 64,
};

static struct snd_pcm_hardware eie_capture_hw = {
	.info = (SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BATCH |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_FIFO_IN_FRAMES),
	.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE,
	.rates = (SNDRV_PCM_RATE_44100 |
		SNDRV_PCM_RATE_48000 |
		SNDRV_PCM_RATE_88200 |
		SNDRV_PCM_RATE_96000),
	.rate_min = 44100,
	.rate_max = 96000,
	.channels_min = 1,
	.channels_max = 4,
	.buffer_bytes_max = 2 * 1024 * 1024,  /* 2MB buffer for better stability */
	.period_bytes_min = 64,  /* Minimum period size */
	.period_bytes_max = 16384 * 16,  /* Larger max period size for 4ch S32_LE */
	.periods_min = 2,  /* Reduce minimum periods for lower latency */
	.periods_max = 128,  /* More periods for stability */
};



/* Forward declarations */
static void kill_all_urbs(struct eie *eie);
static int submit_init_play_urbs(struct eie *eie);
static int submit_init_cap_urbs(struct eie *eie);
static int submit_init_sync_urbs(struct eie *eie);

static int eie_set_alt_setting(struct eie *eie)
{
	int err;

	err = usb_set_interface(eie->udev, 0, 1);
	if (err == 0)
		err = usb_set_interface(eie->udev, 1, 1);
	return err;
}

static int eie_prepare_hw(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;
	int err;

	/* Ensure device is properly initialized */
	err = reset_eie(eie, 48000); /* Default to 48kHz */
	if (err < 0) {
		dev_err(&eie->udev->dev, "Failed to initialize device: %d", err);
		return err;
	}

	/* Set appropriate hardware constraints based on stream direction */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		substream->runtime->hw = eie_playback_hw;
	} else {
		substream->runtime->hw = eie_capture_hw;
	}
	
	return snd_pcm_hw_constraint_minmax(substream->runtime,
		SNDRV_PCM_HW_PARAM_BUFFER_TIME, 10*1000, UINT_MAX);
}

static int eie_ppcm_open(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;
	int err;

	err = eie_prepare_hw(substream);
	if (err < 0)
		return err;
	eie->play_substream = substream;
	
	/* Set a descriptive name for playback */
	strcpy(substream->name, "AKAI EIE Pro Playback");
	return 0;
}

static int eie_cpcm_open(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;
	int err;

	err = eie_prepare_hw(substream);
	if (err < 0)
		return err;
	eie->cap_substream = substream;
	
	/* Set a descriptive name for capture */
	strcpy(substream->name, "AKAI EIE Pro Capture");
	return 0;
}

static int eie_ppcm_close(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;
	eie->play_substream = NULL;
	return 0;
}

static int eie_cpcm_close(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;
	eie->cap_substream = NULL;
	return 0;
}

static int eie_pcm_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *hw_params)
{
	/* Use ALSA's built-in buffer allocation which handles DMA properly */
	return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
}

static int eie_pcm_hw_free(struct snd_pcm_substream *substream)
{
	/* Use ALSA's built-in buffer deallocation */
	return snd_pcm_lib_free_pages(substream);
}

/* Magic sequences for hardware initialization */
struct magic_seq {
	__u8 type;
	__u8 request;
	__u16 value;
	__u16 index;
	__u16 size;
};

static struct magic_seq magic_seq1[] = {
	{0xc0, 86, 0, 0, 3},
	{0xc0, 86, 0, 0, 5},
	{0xc0, 73, 0, 0, 1},
	{0xa2, 129, 0x0100, 0, 3},
	{0, 0, 0, 0, 0}
};

static struct magic_seq magic_seq2[] = {
	{0x22, 1, 0x0100, 134, 3},
	{0x22, 1, 0x0100, 2, 3},
	{0x22, 1, 0x0100, 134, 3},
	{0xa2, 129, 0x0100, 134, 3},
	{0xc0, 73, 0, 0, 1},
	{0x40, 73, 0x0032, 0, 0},
	{0, 0, 0, 0, 0}
};

#define MAX_MAGIC_SEQ_LENGTH 5

static int send_magic_sequence(struct eie *eie, struct magic_seq *m, char *data)
{
	int err;

	while (m->type != 0) {
		unsigned int pipe = m->type & 0x80 ? usb_rcvctrlpipe(eie->udev, 0)
			: usb_sndctrlpipe(eie->udev, 0);

		err = usb_control_msg(eie->udev, pipe, m->request, m->type,
			m->value, m->index, data, m->size, 1000);
		if (err < 0) {
			dev_err(&eie->udev->dev, "Magic sequence failed: %d", err);
			return err;
		}
		m++;
	}
	return 0;
}

static unsigned int calc_frames_wanted(struct eie *eie)
{
	/* More consistent frame calculation to avoid plucking */
	if (eie->rate == 44100) {
		/* 44.1kHz: alternate between 220 and 221 frames for smooth timing */
		eie->wanted_idx = 1 - eie->wanted_idx;
		return 220 + eie->wanted_idx;
	} else if (eie->rate == 48000) {
		/* 48kHz: exactly 240 frames per 5ms */
		return 240;
	} else if (eie->rate == 96000) {
		/* 96kHz: exactly 480 frames per 5ms */
		return 480;
	} else {
		/* Other rates: calculate precisely */
		return (eie->rate * 5) / 1000;
	}
}

static int fill_playback_urb(struct eie_playback_urb *epu)
{
	struct snd_pcm_runtime *runtime;
	struct eie *eie = epu->eie;
	struct urb *urb = epu->urb;
	unsigned int frames_wanted = calc_frames_wanted(eie);
	unsigned int frames_elapsed = atomic_xchg(&eie->frames_elapsed, 0);
	unsigned int frames_filled = 0;
	unsigned int bytes_wanted;
	unsigned char *start;
	int i;

	/* Use hardware feedback if reasonable - stable tolerance */
	if ((frames_elapsed > frames_wanted - 1)
		&& (frames_elapsed < frames_wanted + 1)
		&& frames_elapsed > 0) {
		frames_wanted = frames_elapsed;
	}
	bytes_wanted = BYTES_PER_FRAME * frames_wanted;

	if (bytes_wanted > urb->transfer_buffer_length)
		return -EINVAL;

	if (test_bit(PLAYBACK_RUNNING, &eie->states)) {
		runtime = eie->play_substream->runtime;

		if (frames_wanted > runtime->buffer_size)
			return -EINVAL;

		/* Copy from ALSA's buffer to URB */
		start = runtime->dma_area + eie->play_buf_pos * BYTES_PER_FRAME;
		if (eie->play_buf_pos + frames_wanted <= runtime->buffer_size) {
			memcpy(urb->transfer_buffer, start, bytes_wanted);
			eie->play_buf_pos += frames_wanted;
		} else {
			unsigned int part_bytes = BYTES_PER_FRAME *
				(runtime->buffer_size - eie->play_buf_pos);
			memcpy(urb->transfer_buffer, start, part_bytes);
			memcpy(urb->transfer_buffer + part_bytes,
				runtime->dma_area,
				bytes_wanted - part_bytes);
			eie->play_buf_pos += frames_wanted;
		}
		eie->play_buf_pos %= runtime->buffer_size;
		eie->played_frames += frames_wanted;
		runtime->delay += frames_wanted;
		epu->silent = false;
		epu->len = frames_wanted;
	} else {
		/* Always clear buffer and update length for silent frames */
		memset(urb->transfer_buffer, 0, bytes_wanted);
		epu->len = frames_wanted;
		epu->silent = true;
	}

	/* Setup ISO frame descriptors */
	for (i = 0; i < PLAY_PKT_CNT; i++) {
		int len = frames_wanted * (i+1) / PLAY_PKT_CNT - frames_filled;
		urb->iso_frame_desc[i].offset = frames_filled * BYTES_PER_FRAME;
		urb->iso_frame_desc[i].length = len * BYTES_PER_FRAME;
		frames_filled += len;
	}

	return 0;
}

static bool check_period_elapsed(struct eie *eie)
{
	struct snd_pcm_substream *substream = eie->play_substream;

	if (substream != NULL
		&& eie->played_frames >= substream->runtime->period_size) {
		eie->played_frames %= substream->runtime->period_size;
		return true;
	}
	return false;
}

static void abort_playback(struct eie *eie)
{
	unsigned long flags;

	if (test_bit(PLAYBACK_RUNNING, &eie->states)
		&& eie->play_substream != NULL) {
		snd_pcm_stream_lock_irqsave(eie->play_substream, flags);
		snd_pcm_stop(eie->play_substream, SNDRV_PCM_STATE_XRUN);
		snd_pcm_stream_unlock_irqrestore(eie->play_substream, flags);
	}

	if (test_bit(CAPTURE_RUNNING, &eie->states)
		&& eie->cap_substream != NULL) {
		snd_pcm_stream_lock_irqsave(eie->cap_substream, flags);
		snd_pcm_stop(eie->cap_substream, SNDRV_PCM_STATE_XRUN);
		snd_pcm_stream_unlock_irqrestore(eie->cap_substream, flags);
	}

	spin_lock_irqsave(&eie->lock, flags);
	eie->rate = 0;
	spin_unlock_irqrestore(&eie->lock, flags);
}

static void abort_capture(struct eie *eie)
{
	unsigned long flags;

	if (test_bit(CAPTURE_RUNNING, &eie->states)
		&& eie->cap_substream != NULL) {
		snd_pcm_stream_lock_irqsave(eie->cap_substream, flags);
		snd_pcm_stop(eie->cap_substream, SNDRV_PCM_STATE_XRUN);
		snd_pcm_stream_unlock_irqrestore(eie->cap_substream, flags);
	}
}

static void play_urb_complete(struct urb *urb)
{
	struct eie_playback_urb *epu = urb->context;
	struct eie *eie = epu->eie;
	unsigned long flags;
	int err;
	bool elapsed = false;
	bool abort = false;

	if (urb->status != 0) {
		dev_dbg(&eie->udev->dev, "Play urb complete. %d", urb->status);
		return;
	}

	/* first URB */
	if (!test_and_set_bit(URBS_FLOWING, &eie->states))
		wake_up(&eie->urbs_flow_wait);

	spin_lock_irqsave(&eie->lock, flags);
	if (!epu->silent
		&& eie->play_substream && eie->play_substream->runtime) {
		/* Prevent negative delay values that can cause glitches */
		if (eie->play_substream->runtime->delay >= epu->len) {
			eie->play_substream->runtime->delay -= epu->len;
		} else {
			eie->play_substream->runtime->delay = 0;
		}
	}

	err = fill_playback_urb(epu);
	if (err < 0) {
		abort = true;
		goto err;
	}

	elapsed = test_bit(PLAYBACK_RUNNING, &eie->states)
		&& check_period_elapsed(eie);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0) {
		dev_err(&eie->udev->dev, "Cannot resubmit play urb.");
		abort = true;
	}
err:
	spin_unlock_irqrestore(&eie->lock, flags);
	
	/* Period elapsed notification - SAFE: called outside spinlock */
	if (elapsed)
		snd_pcm_period_elapsed(eie->play_substream);
	if (abort)
		abort_playback(eie);
}

static void cap_urb_complete(struct urb *urb)
{
	struct eie *eie = urb->context;
	struct snd_pcm_substream *substream = eie->cap_substream;
	unsigned long flags;
	int err;
	bool elapsed = false;
	bool abort = false;

		/* Debug: Only log URB completion errors, not successful transfers */
	static int success_log_count = 0;
	if (urb->status != 0) {
		dev_warn(&eie->udev->dev, "Capture URB error: status=%d, length=%d", 
			 urb->status, urb->actual_length);
	} else if (urb->actual_length > 0 && success_log_count < 1) {
		dev_info(&eie->udev->dev, "Capture URB working: length=%d", urb->actual_length);
		success_log_count++;
	}

	if (!substream)
		return;

	if (urb->status) {
		if (urb->status == -ENOENT || /* unlinked */
		    urb->status == -ECONNRESET || /* unlinked */
		    urb->status == -ESHUTDOWN) /* device disabled */
			return;

		dev_dbg(&eie->udev->dev, "cap urb status: %d", urb->status);
	}

	spin_lock_irqsave(&eie->lock, flags);

	if (test_bit(CAPTURE_RUNNING, &eie->states) && substream) {
		struct snd_pcm_runtime *runtime = substream->runtime;
		
		/* Process any received bulk data */
		if (urb->status == 0 && urb->actual_length > 0) {
			unsigned char *buf = urb->transfer_buffer;
			unsigned int frames_received = urb->actual_length / 64;
			int i;
			
			/* Log data size and sample content for debugging - very infrequently */
			static int data_log_counter = 0;
			if (data_log_counter++ % 50000 == 0) {  /* Much less frequent logging */
				dev_info(&eie->udev->dev, "Capture processing: %d bytes, %d frames", 
					 urb->actual_length, frames_received);
			}
			
			for (i = 0; i < frames_received; i++) {
				unsigned char *frame_buf = buf + (i * 64);
				
				/* 64-byte frame - decode with improved bit manipulation */
				int ch1, ch2, ch3, ch4;
				int j;
				
				ch1 = ch2 = ch3 = ch4 = 0;
				
				/* First 24 bytes for channels 1 and 3 - improved bit extraction */
				for (j = 0; j < 24; j++) {
					ch1 |= (frame_buf[j] & 1) << (23-j);  /* MSB alignment for better volume */
					ch3 |= ((frame_buf[j] & 2) >> 1) << (23-j);
				}
				
				/* Bytes 32-55 for channels 2 and 4 - improved bit extraction */  
				for (j = 32; j < 56; j++) {
					ch2 |= (frame_buf[j] & 1) << (55-j);
					ch4 |= ((frame_buf[j] & 2) >> 1) << (55-j);
				}
				
				/* Sign extend from 24-bit to 32-bit for proper audio levels */
				if (ch1 & 0x800000) ch1 |= 0xFF000000;  /* Sign extend */
				if (ch2 & 0x800000) ch2 |= 0xFF000000;
				if (ch3 & 0x800000) ch3 |= 0xFF000000;
				if (ch4 & 0x800000) ch4 |= 0xFF000000;
				
				unsigned int format = runtime->format;
				unsigned int channels = runtime->channels;
				unsigned int frame_bytes = snd_pcm_format_size(format, channels);
				unsigned char *out = runtime->dma_area + eie->cap_frame * frame_bytes;
				
				/* Advance buffer position */
				eie->cap_frame = (eie->cap_frame + 1) % runtime->buffer_size;
				
				if (format == SNDRV_PCM_FORMAT_S32_LE) {
					__le32 *out32 = (__le32 *)out;
					if (channels == 4) {
						/* Use proper 24-bit values shifted to 32-bit range for full volume */
						out32[0] = cpu_to_le32(ch1 << 8);  /* Shift 24-bit to 32-bit MSB */
						out32[1] = cpu_to_le32(ch2 << 8);
						out32[2] = cpu_to_le32(ch3 << 8);
						out32[3] = cpu_to_le32(ch4 << 8);
					} else if (channels == 2) {
						/* Use first two channels with proper level adjustment for stereo */
						out32[0] = cpu_to_le32(ch1 << 8);
						out32[1] = cpu_to_le32(ch2 << 8);
					} else if (channels == 1) {
						/* Mix first two channels for mono to avoid mono-only issue */
						int mixed = (ch1 + ch2) / 2;
						out32[0] = cpu_to_le32(mixed << 8);
					}
				} else if (format == SNDRV_PCM_FORMAT_S24_LE) {
					if (channels == 4) {
						/* For S24_LE, use the raw 24-bit values with better packing */
						*((__le32 *)&out[0]) = cpu_to_le32(ch1 & 0x00ffffff);
						*((__le32 *)&out[3]) = cpu_to_le32(ch2 & 0x00ffffff);
						*((__le32 *)&out[6]) = cpu_to_le32(ch3 & 0x00ffffff);
						*((__le32 *)&out[9]) = cpu_to_le32(ch4 & 0x00ffffff);
					} else if (channels == 2) {
						*((__le32 *)&out[0]) = cpu_to_le32(ch1 & 0x00ffffff);
						*((__le32 *)&out[3]) = cpu_to_le32(ch2 & 0x00ffffff);
					} else if (channels == 1) {
						int mixed = (ch1 + ch2) / 2;
						*((__le32 *)&out[0]) = cpu_to_le32(mixed & 0x00ffffff);
					}
				} else if (format == SNDRV_PCM_FORMAT_S16_LE) {
					__le16 *out16 = (__le16 *)out;
					if (channels == 4) {
						/* For S16_LE, use upper 16 bits with proper scaling */
						out16[0] = cpu_to_le16(ch1 >> 8);  /* 24-bit to 16-bit conversion */
						out16[1] = cpu_to_le16(ch2 >> 8);
						out16[2] = cpu_to_le16(ch3 >> 8);
						out16[3] = cpu_to_le16(ch4 >> 8);
					} else if (channels == 2) {
						/* Ensure proper stereo: use different channels for L/R */
						out16[0] = cpu_to_le16(ch1 >> 8);  /* Left channel */
						out16[1] = cpu_to_le16(ch2 >> 8);  /* Right channel */
					} else if (channels == 1) {
						/* Mix first two channels for mono */
						int mixed = (ch1 + ch2) / 2;
						out16[0] = cpu_to_le16(mixed >> 8);
					}
				}
				
				/* Debug: Log decoded values very infrequently to avoid spam */
				static int decode_log_counter = 0;
				if (i == 0 && (decode_log_counter++ % 100000 == 0)) {  /* Very infrequent logging */
					dev_info(&eie->udev->dev, "Audio processing active: Format=%d, Channels=%d", 
						format, channels);
				}
			}
			
			/* Update period tracking */
			eie->cap_period_pos += frames_received;
			if (eie->cap_period_pos >= runtime->period_size) {
				eie->cap_period_pos = 0;
				elapsed = true;
			}
		}
	}

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0) {
		/* Reduce log spam - only log serious errors */
		if (err != -ENODEV && err != -ESHUTDOWN && err != -ENOENT && err != -EPERM) {
			/* Only log every 100th error to prevent log flooding */
			static int error_count = 0;
			if ((error_count++ % 100) == 0) {
				dev_warn(&eie->udev->dev, "URB resubmit error %d (logged every 100 errors, count: %d)", err, error_count);
			}
			/* Try to resubmit after a brief delay for transient errors */
			if (error_count < 300) { /* Give up after too many errors */
				/* Don't abort immediately on -EBUSY or similar transient errors */
				if (err != -EBUSY && err != -EAGAIN) {
					abort = true;
				}
			} else {
				abort = true;
			}
		} else {
			abort = true;
		}
	}

	spin_unlock_irqrestore(&eie->lock, flags);
	
	/* Period elapsed notification */
	if (elapsed)
		snd_pcm_period_elapsed(eie->cap_substream);
	if (abort)
		abort_capture(eie);
}

static void sync_urb_complete(struct urb *urb)
{
	struct eie *eie = urb->context;
	int i;
	int err;

	if (urb->status != 0) {
		dev_dbg(&eie->udev->dev, "Sync urb complete. status = %d, packets = %d",
			urb->status, urb->number_of_packets);
		return;
	}

	for (i = 0; i < urb->number_of_packets; i++) {
		if (urb->iso_frame_desc[i].actual_length > 0) {
			unsigned char *buf = urb->transfer_buffer;
			unsigned int offset = urb->iso_frame_desc[i].offset;
			unsigned char d = buf[offset];

			if (d == 0) {
				/* the device did not advance clock - skip this sync packet */
				dev_dbg(&eie->udev->dev, "EIE sync: device clock not advancing");
			} else if (d > 0 && d < 50) {
				/* Only accept reasonable sync values to reduce timing jitter */
				atomic_add(d, &eie->frames_elapsed);
			} else {
				/* Ignore unreasonable sync values that could cause glitches */
				dev_dbg(&eie->udev->dev, "EIE sync: ignoring value %d", d);
			}
		}
	}

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0) {
		dev_err(&eie->udev->dev, "Cannot resubmit sync urb: %d", err);
		/* Don't call abort from atomic context */
	}
}

static int reset_eie(struct eie *eie, unsigned int rate)
{
	int err = 0;
	unsigned char *data;

	data = kmalloc(MAX_MAGIC_SEQ_LENGTH, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	/* Only log rate changes, not every reset */
	static int last_logged_rate = 0;
	if (last_logged_rate != rate) {
		dev_info(&eie->udev->dev, "Resetting device for %dHz", rate);
		last_logged_rate = rate;
	}
	kill_all_urbs(eie);
	eie_set_alt_setting(eie);

	err = send_magic_sequence(eie, &magic_seq1[0], data);
	if (err < 0)
		goto out;

	*((__le32 *) data) = __cpu_to_le32(rate);
	err = send_magic_sequence(eie, &magic_seq2[0], data);
	if (err < 0)
		goto out;

	eie->rate = rate;

	/* Only prepare the device, don't submit URBs until playback starts */

out:
	kfree(data);
	return err;
}

static int eie_ppcm_prepare(struct snd_pcm_substream *substream)
{
	int err = 0;
	struct eie *eie = substream->private_data;

	if (substream->runtime->rate != eie->rate)
		err = reset_eie(eie, substream->runtime->rate);

	eie->played_frames = 0;
	eie->play_buf_pos = 0;
	substream->runtime->delay = 0;
	
	/* Reset timing state for clean startup */
	eie->wanted_idx = 0;
	atomic_set(&eie->frames_elapsed, 0);

	return err;
}

static int eie_cpcm_prepare(struct snd_pcm_substream *substream)
{
	int err = 0;
	struct eie *eie = substream->private_data;

	if (substream->runtime->rate != eie->rate)
		err = reset_eie(eie, substream->runtime->rate);

	eie->cap_frame = 0;
	eie->cap_period_pos = 0;
	substream->runtime->delay = 0;

	return err;
}

static int submit_init_play_urbs(struct eie *eie)
{
	unsigned long flags;
	int err = 0;
	int i;

	spin_lock_irqsave(&eie->lock, flags);

	for (i = 0; i < PLAY_URB_CNT; i++) {
		/* init the urb state */
		eie->play_urbs[i].silent = true;
		eie->play_urbs[i].len = 0;

		err = fill_playback_urb(&eie->play_urbs[i]);
		if (err < 0)
			goto out;
		err = usb_submit_urb(eie->play_urbs[i].urb, GFP_ATOMIC);
		if (err < 0)
			goto out;
	}

out:
	spin_unlock_irqrestore(&eie->lock, flags);
	return err;
}

static int submit_init_cap_urbs(struct eie *eie)
{
	unsigned long flags;
	int err = 0;
	int i;

	spin_lock_irqsave(&eie->lock, flags);

	/* Reset capture position tracking */
	eie->cap_frame = 0;
	eie->cap_period_pos = 0;

	for (i = 0; i < CAP_URB_CNT; i++) {
		if (eie->cap_urbs[i] == NULL) {
			dev_err(&eie->udev->dev, "cap urb %d is NULL", i);
			err = -EINVAL;
			goto out;
		}
		
		err = usb_submit_urb(eie->cap_urbs[i], GFP_ATOMIC);
		if (err < 0) {
			dev_err(&eie->udev->dev, "Cannot submit cap urb %d: %d", i, err);
			goto out;
		}
	}

out:
	spin_unlock_irqrestore(&eie->lock, flags);
	return err;
}

static int eie_ppcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct eie *eie = substream->private_data;
	int err;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		/* Submit URBs when playback actually starts */
		if (!test_bit(URBS_FLOWING, &eie->states)) {
			if (eie->sync_endpoint_addr) {
				err = submit_init_sync_urbs(eie);
				if (err < 0)
					return err;
			}
			err = submit_init_play_urbs(eie);
			if (err < 0)
				return err;
			wait_event(eie->urbs_flow_wait, test_bit(URBS_FLOWING, &eie->states));
			
			/* Small delay to let hardware stabilize and avoid initial plucks */
			usleep_range(1000, 2000);  /* 1-2ms stabilization delay */
		}
		
		set_bit(PLAYBACK_RUNNING, &eie->states);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		clear_bit(PLAYBACK_RUNNING, &eie->states);
		return 0;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		clear_bit(PLAYBACK_RUNNING, &eie->states);
		return 0;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		set_bit(PLAYBACK_RUNNING, &eie->states);
		return 0;
	default:
		return -EINVAL;
	}
}

static int eie_cpcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct eie *eie = substream->private_data;
	int err;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		/* Ensure device is configured for the current sample rate */
		if (substream->runtime->rate != eie->rate) {
			err = reset_eie(eie, substream->runtime->rate);
			if (err < 0) {
				dev_err(&eie->udev->dev, "Failed to configure device for capture: %d", err);
				return err;
			}
		}
		
		/* CRITICAL: Device needs both playbook and capture URBs running for capture to work */
		if (!test_bit(URBS_FLOWING, &eie->states)) {
			/* Start playback URBs first */
			if (eie->sync_endpoint_addr) {
				err = submit_init_sync_urbs(eie);
				if (err < 0) {
					dev_err(&eie->udev->dev, "Failed to start sync URBs for capture: %d", err);
					return err;
				}
			}
			err = submit_init_play_urbs(eie);
			if (err < 0) {
				dev_err(&eie->udev->dev, "Failed to start playbook URBs for capture: %d", err);
				return err;
			}
			wait_event(eie->urbs_flow_wait, test_bit(URBS_FLOWING, &eie->states));
		}
		
		set_bit(CAPTURE_RUNNING, &eie->states);
		err = submit_init_cap_urbs(eie);
		if (err < 0) {
			clear_bit(CAPTURE_RUNNING, &eie->states);
			return err;
		}
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		clear_bit(CAPTURE_RUNNING, &eie->states);
		return 0;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		clear_bit(CAPTURE_RUNNING, &eie->states);
		return 0;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		set_bit(CAPTURE_RUNNING, &eie->states);
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t eie_ppcm_pointer(struct snd_pcm_substream *substream)
{
	unsigned long flags;
	struct eie *eie = substream->private_data;
	snd_pcm_uframes_t pos;

	spin_lock_irqsave(&eie->lock, flags);
	pos = eie->play_buf_pos;
	spin_unlock_irqrestore(&eie->lock, flags);

	return pos;
}

static snd_pcm_uframes_t eie_cpcm_pointer(struct snd_pcm_substream *substream)
{
	unsigned long flags;
	struct eie *eie = substream->private_data;
	snd_pcm_uframes_t pos;

	spin_lock_irqsave(&eie->lock, flags);
	pos = eie->cap_frame;
	spin_unlock_irqrestore(&eie->lock, flags);

	return pos;
}

static const struct snd_pcm_ops eie_playback_pcm_ops = {
	.open = eie_ppcm_open,
	.close = eie_ppcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = eie_pcm_hw_params,
	.hw_free = eie_pcm_hw_free,
	.prepare = eie_ppcm_prepare,
	.trigger = eie_ppcm_trigger,
	.pointer = eie_ppcm_pointer,
};

static const struct snd_pcm_ops eie_capture_pcm_ops = {
	.open = eie_cpcm_open,
	.close = eie_cpcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = eie_pcm_hw_params,
	.hw_free = eie_pcm_hw_free,
	.prepare = eie_cpcm_prepare,
	.trigger = eie_cpcm_trigger,
	.pointer = eie_cpcm_pointer,
};

static int submit_init_sync_urbs(struct eie *eie)
{
	int err;
	int i;

	for (i = 0; i < SYNC_URB_CNT; i++) {
		err = usb_submit_urb(eie->sync_urbs[i], GFP_KERNEL);
		if (err < 0) {
			dev_err(&eie->udev->dev, "USB sync URB error %d", err);
			return err;
		}
	}

	dev_info(&eie->udev->dev, "Submitted sync urbs.");
	return 0;
}

static void kill_all_urbs(struct eie *eie)
{
	struct urb *urb;
	int i;

	for (i = 0; i < PLAY_URB_CNT; i++) {
		urb = eie->play_urbs[i].urb;
		if (urb)
			usb_kill_urb(urb);
	}

	for (i = 0; i < SYNC_URB_CNT; i++) {
		urb = eie->sync_urbs[i];
		if (urb)
			usb_kill_urb(urb);
	}

	for (i = 0; i < CAP_URB_CNT; i++) {
		urb = eie->cap_urbs[i];
		if (urb)
			usb_kill_urb(urb);
	}
	
	/* Clear the URBS_FLOWING state when URBs are killed */
	clear_bit(URBS_FLOWING, &eie->states);
}

static void kill_and_free_urb(struct eie *eie, struct urb **urbp)
{
	struct urb *urb = *urbp;

	if (urb) {
		usb_kill_urb(urb);
		if (urb->transfer_buffer != NULL)
			usb_free_coherent(eie->udev,
				urb->transfer_buffer_length,
				urb->transfer_buffer,
				urb->transfer_dma);
		usb_free_urb(urb);
		(*urbp) = NULL;
	}
}

static void free_usb_related_resources(struct eie *eie)
{
	int i;

	for (i = 0; i < PLAY_URB_CNT; i++)
		kill_and_free_urb(eie, &eie->play_urbs[i].urb);

	for (i = 0; i < SYNC_URB_CNT; i++)
		kill_and_free_urb(eie, &eie->sync_urbs[i]);

	for (i = 0; i < CAP_URB_CNT; i++)
		kill_and_free_urb(eie, &eie->cap_urbs[i]);

	if (eie->ifb) {
		usb_set_intfdata(eie->ifb, NULL);
		usb_driver_release_interface(&eie_driver, eie->ifb);
	}

	if (eie->ifa)
		usb_set_intfdata(eie->ifa, NULL);
}

static int init_play_urbs(struct eie *eie, struct usb_endpoint_descriptor *endpoint)
{
	unsigned char *buf;
	struct urb *urb;
	int j;
	int err = 0;

	eie->play_packet_size = usb_endpoint_maxp(endpoint);
	eie->play_endpoint_addr = endpoint->bEndpointAddress;

	for (j = 0; j < PLAY_URB_CNT; j++) {
		urb = usb_alloc_urb(PLAY_PKT_CNT, GFP_KERNEL);
		if (urb == NULL) {
			err = -ENOMEM;
			break;
		}

		buf = usb_alloc_coherent(eie->udev,
			PLAY_PKT_CNT * eie->play_packet_size,
			GFP_KERNEL, &urb->transfer_dma);
		if (buf == NULL) {
			usb_free_urb(urb);
			err = -ENOMEM;
			break;
		}

		urb->dev = eie->udev;
		urb->pipe = usb_sndisocpipe(eie->udev, eie->play_endpoint_addr);
		urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
		urb->transfer_buffer = buf;
		urb->transfer_buffer_length = PLAY_PKT_CNT * eie->play_packet_size;
		urb->number_of_packets = PLAY_PKT_CNT;
		urb->interval = 1;
		urb->context = &eie->play_urbs[j];
		urb->complete = play_urb_complete;

		eie->play_urbs[j].urb = urb;
		eie->play_urbs[j].eie = eie;
	}

	return err;
}

static int init_sync_urbs(struct eie *eie, struct usb_endpoint_descriptor *endpoint)
{
	unsigned char *buf;
	struct urb *urb;
	int j;
	int err = 0;

	eie->sync_packet_size = usb_endpoint_maxp(endpoint);
	eie->sync_endpoint_addr = endpoint->bEndpointAddress;

	for (j = 0; j < SYNC_URB_CNT; j++) {
		urb = usb_alloc_urb(1, GFP_KERNEL);
		if (urb == NULL) {
			err = -ENOMEM;
			break;
		}

		buf = usb_alloc_coherent(eie->udev, eie->sync_packet_size,
			GFP_KERNEL, &urb->transfer_dma);
		if (buf == NULL) {
			usb_free_urb(urb);
			err = -ENOMEM;
			break;
		}

		urb->dev = eie->udev;
		urb->pipe = usb_rcvisocpipe(eie->udev, eie->sync_endpoint_addr);
		urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
		urb->transfer_buffer = buf;
		urb->transfer_buffer_length = eie->sync_packet_size;
		urb->number_of_packets = 1;
		urb->interval = 1;
		urb->context = eie;
		urb->complete = sync_urb_complete;
		urb->iso_frame_desc[0].offset = 0;
		urb->iso_frame_desc[0].length = eie->sync_packet_size;

		eie->sync_urbs[j] = urb;
	}

	return err;
}

static int init_cap_urbs(struct eie *eie, struct usb_endpoint_descriptor *endpoint)
{
	unsigned char *buf;
	struct urb *urb;
	int j;
	int err = 0;
	int cap_buffer_size = 4096; /* Use larger buffer size like the reference */

	eie->cap_packet_size = usb_endpoint_maxp(endpoint);
	eie->cap_endpoint_addr = endpoint->bEndpointAddress;

	for (j = 0; j < CAP_URB_CNT; j++) {
		urb = usb_alloc_urb(0, GFP_KERNEL); /* Bulk URB */
		if (urb == NULL) {
			err = -ENOMEM;
			break;
		}

		buf = usb_alloc_coherent(eie->udev, cap_buffer_size,
			GFP_KERNEL, &urb->transfer_dma);
		if (buf == NULL) {
			usb_free_urb(urb);
			err = -ENOMEM;
			break;
		}

		/* Use bulk URB for capture with larger buffer */
		usb_fill_bulk_urb(urb, eie->udev,
			usb_rcvbulkpipe(eie->udev, eie->cap_endpoint_addr), buf,
			cap_buffer_size, cap_urb_complete, eie);

		urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

		eie->cap_urbs[j] = urb;
	}

	return err;
}

static int init_urbs(struct eie *eie)
{
	int i, err;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;

	/* Initialize playback URBs from interface 0 */
	iface_desc = eie->ifa->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		endpoint = &iface_desc->endpoint[i].desc;

		if (!eie->play_endpoint_addr && usb_endpoint_is_isoc_out(endpoint)) {
			err = init_play_urbs(eie, endpoint);
			if (err < 0)
				return err;
		}
	}

	/* Initialize sync and capture URBs from interface 1 */  
	iface_desc = eie->ifb->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		endpoint = &iface_desc->endpoint[i].desc;

		if (!eie->sync_endpoint_addr && usb_endpoint_is_isoc_in(endpoint)) {
			/* Check if this is the sync endpoint (typically smaller) */
			if (usb_endpoint_maxp(endpoint) < 64) {
				err = init_sync_urbs(eie, endpoint);
				if (err < 0)
					return err;
			}
		}
		
		/* Look specifically for endpoint 0x86 for capture */
		if (!eie->cap_endpoint_addr && usb_endpoint_is_bulk_in(endpoint) && 
		    endpoint->bEndpointAddress == 0x86) {
			dev_info(&eie->udev->dev, "Found audio capture endpoint: 0x%02x", endpoint->bEndpointAddress);
			err = init_cap_urbs(eie, endpoint);
			if (err < 0)
				return err;
		}
	}

	if (!eie->play_endpoint_addr) {
		dev_err(&eie->udev->dev, "Cannot find playback endpoint.");
		return -ENOENT;
	}
	
	if (!eie->cap_endpoint_addr) {
		dev_err(&eie->udev->dev, "Cannot find capture endpoint 0x86.");
		return -ENOENT;
	}
	
	if (!eie->sync_endpoint_addr) {
		dev_warn(&eie->udev->dev, "No sync endpoint found - using default timing.");
	}

	return 0;
}

/* Mixer control functions */
static int eie_capture_volume_info(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2; /* stereo */
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 65535;
	return 0;
}

static int eie_capture_volume_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	/* Always return full volume for now */
	ucontrol->value.integer.value[0] = 65535;
	ucontrol->value.integer.value[1] = 65535;
	return 0;
}

static int eie_capture_volume_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	/* Volume control not implemented in hardware, just return changed */
	return 1;
}

static int eie_probe(struct usb_interface *interface, const struct usb_device_id *usb_id)
{
	unsigned int card_index;
	struct snd_card *card;
	struct eie *eie;
	int err;

	mutex_lock(&devices_mutex);

	for (card_index = 0; card_index < SNDRV_CARDS; ++card_index)
		if (enable[card_index] && !(devices_used & (1 << card_index)))
			break;

	if (card_index >= SNDRV_CARDS) {
		mutex_unlock(&devices_mutex);
		return -ENOENT;
	}

	err = snd_card_new(&interface_to_usbdev(interface)->dev, 
			   index[card_index], id[card_index], THIS_MODULE,
			   sizeof(*eie), &card);
	if (err < 0) {
		mutex_unlock(&devices_mutex);
		return err;
	}

	eie = card->private_data;
	eie->udev = interface_to_usbdev(interface);
	eie->card = card;
	eie->card_index = card_index;

	spin_lock_init(&eie->lock);
	init_waitqueue_head(&eie->urbs_flow_wait);

	eie->ifa = interface;
	eie->ifb = usb_ifnum_to_if(eie->udev, 1);
	if (!eie->ifb) {
		err = -ENXIO;
		goto probe_err;
	}
	err = usb_driver_claim_interface(&eie_driver, eie->ifb, eie);
	if (err < 0) {
		eie->ifb = NULL;
		err = -EBUSY;
		goto probe_err;
	}

	err = eie_set_alt_setting(eie);
	if (err < 0)
		goto probe_err;

	/* Setup card info to appear as standard USB Audio device */
	snd_card_set_dev(card, &interface->dev);
	strcpy(card->driver, "USB-Audio");
	strcpy(card->shortname, "AKAI EIE Pro");
	snprintf(card->longname, sizeof(card->longname),
		 "AKAI EIE Pro at %s", dev_name(&eie->udev->dev));

	err = snd_pcm_new(card, "USB Audio", 0, 1, 1, &eie->pcm);
	if (err < 0)
		goto probe_err;
	eie->pcm->private_data = eie;
	strcpy(eie->pcm->name, "USB Audio");

	snd_pcm_set_ops(eie->pcm, SNDRV_PCM_STREAM_PLAYBACK, &eie_playback_pcm_ops);
	snd_pcm_set_ops(eie->pcm, SNDRV_PCM_STREAM_CAPTURE, &eie_capture_pcm_ops);

	/* Pre-allocate buffers for both streams */
	snd_pcm_lib_preallocate_pages_for_all(eie->pcm, SNDRV_DMA_TYPE_VMALLOC,
					       NULL, 0, 512*1024);

	/* Add minimal mixer control for browser volume detection */
	{
		struct snd_kcontrol_new capture_volume = {
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = "Capture Volume",
			.info = eie_capture_volume_info,
			.get = eie_capture_volume_get,
			.put = eie_capture_volume_put,
		};
		err = snd_ctl_add(card, snd_ctl_new1(&capture_volume, eie));
		if (err < 0) {
			dev_warn(&eie->udev->dev, "Failed to add capture volume: %d", err);
		}
	}

	err = snd_card_register(card);
	if (err < 0)
		goto probe_err;

	init_urbs(eie);

	usb_set_intfdata(interface, eie);
	devices_used |= 1 << card_index;

	mutex_unlock(&devices_mutex);
	
	dev_info(&eie->udev->dev, "AKAI EIE Pro driver ready - Playback: 4ch, Capture: 1-4ch, Sample rates: 44.1-96kHz");
	return 0;

probe_err:
	free_usb_related_resources(eie);
	snd_card_free(card);
	mutex_unlock(&devices_mutex);
	return err;
}

static void eie_disconnect(struct usb_interface *interface)
{
	struct eie *eie = usb_get_intfdata(interface);

	if (!eie)
		return;

	mutex_lock(&devices_mutex);

	set_bit(DISCONNECTED, &eie->states);
	wake_up(&eie->urbs_flow_wait);

	free_usb_related_resources(eie);
	snd_card_free_when_closed(eie->card);

	mutex_unlock(&devices_mutex);
}

static struct usb_device_id eie_ids[] = {
	{ USB_DEVICE(0x09e8, 0x0010) }, /* EIE pro */
	{ }
};
MODULE_DEVICE_TABLE(usb, eie_ids);

static struct usb_driver eie_driver = {
	.name = "snd-eie",
	.id_table = eie_ids,
	.probe = eie_probe,
	.disconnect = eie_disconnect,
};

module_usb_driver(eie_driver);
