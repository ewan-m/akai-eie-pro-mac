#include "akai_eie_protocol.hpp"

#include <cstring>

namespace akai_eie
{
	namespace
	{

		struct ControlTemplate
		{
			std::uint8_t request_type;
			std::uint8_t request;
			std::uint16_t value;
			std::uint16_t index;
			std::uint16_t length;
			bool uses_sample_rate_payload;
		};

		constexpr ControlTemplate kResetControlTemplates[kResetControlTransferCount] = {
			{0xc0, 86, 0, 0, 3, false},
			{0xc0, 86, 0, 0, 5, false},
			{0xc0, 73, 0, 0, 1, false},
			{0xa2, 129, 0x0100, 0, 3, false},
			{0x22, 1, 0x0100, 134, 3, true},
			{0x22, 1, 0x0100, 2, 3, true},
			{0x22, 1, 0x0100, 134, 3, true},
			{0xa2, 129, 0x0100, 134, 3, false},
			{0xc0, 73, 0, 0, 1, false},
			{0x40, 73, 0x0032, 0, 0, false},
		};

		void WriteLE16(std::uint8_t *out, std::uint16_t value) noexcept
		{
			out[0] = static_cast<std::uint8_t>(value & 0xffu);
			out[1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
		}

		void WriteLE24(std::uint8_t *out, std::uint32_t value) noexcept
		{
			out[0] = static_cast<std::uint8_t>(value & 0xffu);
			out[1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
			out[2] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
		}

		void WriteLE32(std::uint8_t *out, std::uint32_t value) noexcept
		{
			out[0] = static_cast<std::uint8_t>(value & 0xffu);
			out[1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
			out[2] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
			out[3] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
		}

		void WriteSampleRatePayload(std::uint32_t sample_rate,
									std::uint8_t data[kMaxControlPayloadBytes]) noexcept
		{
			// The AKAI rate requests use the USB Audio Class 3-byte little-endian
			// sample-rate payload. The extra bytes stay zeroed for templates whose
			// control transfer length sends fewer than kMaxControlPayloadBytes bytes.
			data[0] = static_cast<std::uint8_t>(sample_rate & 0xffu);
			data[1] = static_cast<std::uint8_t>((sample_rate >> 8) & 0xffu);
			data[2] = static_cast<std::uint8_t>((sample_rate >> 16) & 0xffu);
			data[3] = static_cast<std::uint8_t>((sample_rate >> 24) & 0xffu);
			data[4] = 0;
		}

		std::int32_t SignExtend24(std::uint32_t value) noexcept
		{
			value &= 0x00ffffffu;
			if ((value & 0x00800000u) != 0)
				value |= 0xff000000u;
			return static_cast<std::int32_t>(value);
		}

		std::uint32_t Sample24Bits(std::int32_t sample) noexcept
		{
			return static_cast<std::uint32_t>(sample) & 0x00ffffffu;
		}

		std::uint16_t Sample24ToS16Bits(std::int32_t sample) noexcept
		{
			return static_cast<std::uint16_t>((Sample24Bits(sample) >> 8) & 0xffffu);
		}

		std::int32_t Float32ToS24(float sample) noexcept
		{
			if (!(sample == sample))
				return 0;
			if (sample >= 1.0f)
				return 0x7fffff;
			if (sample <= -1.0f)
				return -0x800000;

			const float scale = sample < 0.0f ? 8388608.0f : 8388607.0f;
			const float scaled = sample * scale;
			return static_cast<std::int32_t>(
				scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
		}

		bool IsSupportedChannelCount(std::uint32_t channels) noexcept
		{
			return channels == 1 || channels == 2 || channels == 4;
		}

		std::int32_t ChannelSample(const CaptureSampleFrame &frame,
								   std::uint32_t channel) noexcept
		{
			switch (channel)
			{
			case 0:
				return frame.ch1;
			case 1:
				return frame.ch2;
			case 2:
				return frame.ch3;
			default:
				return frame.ch4;
			}
		}

		void WriteOnePcmSample(std::int32_t sample, PcmSampleFormat format,
							   std::uint8_t *out) noexcept
		{
			switch (format)
			{
			case PcmSampleFormat::S16LE:
				WriteLE16(out, Sample24ToS16Bits(sample));
				break;
			case PcmSampleFormat::S24LEPacked:
				WriteLE24(out, Sample24Bits(sample));
				break;
			case PcmSampleFormat::S32LE:
				WriteLE32(out, static_cast<std::uint32_t>(sample) << 8);
				break;
			}
		}

	} // namespace

	bool IsSupportedSampleRate(std::uint32_t sample_rate) noexcept
	{
		switch (sample_rate)
		{
		case 44100:
		case 48000:
		case 88200:
		case 96000:
			return true;
		default:
			return false;
		}
	}

	std::size_t BuildResetControlTransfers(std::uint32_t sample_rate,
										   ControlTransfer *out_transfers, std::size_t transfer_capacity) noexcept
	{
		if (!IsSupportedSampleRate(sample_rate) || out_transfers == nullptr || transfer_capacity < kResetControlTransferCount)
		{
			return 0;
		}

		for (std::size_t i = 0; i < kResetControlTransferCount; ++i)
		{
			const ControlTemplate &src = kResetControlTemplates[i];
			ControlTransfer &dst = out_transfers[i];
			dst.request_type = src.request_type;
			dst.request = src.request;
			dst.value = src.value;
			dst.index = src.index;
			dst.length = src.length;
			std::memset(dst.data, 0, sizeof(dst.data));
			if (src.uses_sample_rate_payload)
				WriteSampleRatePayload(sample_rate, dst.data);
		}

		return kResetControlTransferCount;
	}

	std::uint32_t CalculatePlaybackFramesWanted(std::uint32_t sample_rate,
												PlaybackTimingState &state, std::uint32_t feedback_frames) noexcept
	{
		std::uint32_t frames_wanted = 0;

		switch (sample_rate)
		{
		case 44100:
			state.wanted_index = state.wanted_index == 0 ? 1 : 0;
			frames_wanted = 220 + state.wanted_index;
			break;
		case 48000:
			frames_wanted = 240;
			break;
		case 96000:
			frames_wanted = 480;
			break;
		default:
			frames_wanted = static_cast<std::uint32_t>(
				(static_cast<std::uint64_t>(sample_rate) * 5) / 1000);
			break;
		}

		if (feedback_frames > 0 &&
			feedback_frames + 1 >= frames_wanted &&
			feedback_frames <= frames_wanted + 1)
			frames_wanted = feedback_frames;

		return frames_wanted;
	}

	bool BuildIsoPacketLayout(std::uint32_t frames_wanted,
							  std::uint32_t packet_count, std::uint32_t bytes_per_frame,
							  IsoPacket *packets) noexcept
	{
		if (packet_count == 0 || bytes_per_frame == 0 || packets == nullptr)
			return false;
		if (static_cast<std::uint64_t>(frames_wanted) * bytes_per_frame >
			UINT32_MAX)
		{
			return false;
		}

		std::uint32_t frames_filled = 0;
		for (std::uint32_t i = 0; i < packet_count; ++i)
		{
			const std::uint32_t next_frame = static_cast<std::uint32_t>(
				(static_cast<std::uint64_t>(frames_wanted) *
				 (static_cast<std::uint64_t>(i) + 1)) /
				packet_count);
			const std::uint32_t packet_frames = next_frame - frames_filled;
			packets[i].offset_bytes = static_cast<std::uint32_t>(
				static_cast<std::uint64_t>(frames_filled) * bytes_per_frame);
			packets[i].length_bytes = static_cast<std::uint32_t>(
				static_cast<std::uint64_t>(packet_frames) * bytes_per_frame);
			frames_filled += packet_frames;
		}

		return frames_filled == frames_wanted;
	}

	bool EncodePlaybackFrameFromFloat32(
		const float pcm[kPlaybackChannelCount],
		std::uint8_t out_frame[kPlaybackBytesPerFrame]) noexcept
	{
		if (pcm == nullptr || out_frame == nullptr)
			return false;

		for (std::uint32_t channel = 0; channel < kPlaybackChannelCount; ++channel)
		{
			const std::uint32_t sample_bits = Sample24Bits(
				Float32ToS24(pcm[channel]));
			WriteLE24(out_frame + channel * 3, sample_bits);
		}

		return true;
	}

	std::size_t EncodePlaybackBufferFromFloat32(const float *pcm_interleaved,
												std::size_t frame_count, std::uint8_t *out_frames,
												std::size_t out_frame_bytes) noexcept
	{
		if (pcm_interleaved == nullptr || out_frames == nullptr)
			return 0;

		const std::size_t max_frames = out_frame_bytes / kPlaybackBytesPerFrame;
		const std::size_t frames_to_encode =
			frame_count < max_frames ? frame_count : max_frames;

		for (std::size_t frame_index = 0; frame_index < frames_to_encode;
			 ++frame_index)
		{
			EncodePlaybackFrameFromFloat32(
				pcm_interleaved + frame_index * kPlaybackChannelCount,
				out_frames + frame_index * kPlaybackBytesPerFrame);
		}

		return frames_to_encode;
	}

	bool FillPlaybackTransfer(const std::uint8_t *ring_frames,
							  std::size_t ring_frame_count, bool running,
							  std::uint32_t frames_wanted, PlaybackCopyState &state,
							  std::uint8_t *usb_buffer, std::size_t usb_buffer_bytes,
							  std::size_t *bytes_written) noexcept
	{
		if (usb_buffer == nullptr ||
			frames_wanted > usb_buffer_bytes / kPlaybackBytesPerFrame)
		{
			return false;
		}

		const std::size_t bytes_wanted =
			static_cast<std::size_t>(frames_wanted) * kPlaybackBytesPerFrame;

		if (bytes_written != nullptr)
			*bytes_written = bytes_wanted;

		if (!running)
		{
			std::memset(usb_buffer, 0, bytes_wanted);
			return true;
		}

		if (ring_frames == nullptr || ring_frame_count == 0 || state.ring_frame >= ring_frame_count || frames_wanted > ring_frame_count)
		{
			return false;
		}

		const std::size_t remaining_frames = ring_frame_count - state.ring_frame;
		const std::size_t first_frames =
			frames_wanted <= remaining_frames ? frames_wanted : remaining_frames;
		const std::size_t first_bytes = first_frames * kPlaybackBytesPerFrame;

		std::memcpy(usb_buffer,
					ring_frames + state.ring_frame * kPlaybackBytesPerFrame,
					first_bytes);

		if (first_frames < frames_wanted)
		{
			const std::size_t second_bytes = bytes_wanted - first_bytes;
			std::memcpy(usb_buffer + first_bytes, ring_frames, second_bytes);
		}

		state.ring_frame = (state.ring_frame + frames_wanted) % ring_frame_count;
		return true;
	}

	CaptureSampleFrame DecodeCaptureUsbFrame(
		const std::uint8_t frame[kCaptureBytesPerUsbFrame]) noexcept
	{
		std::uint32_t ch1 = 0;
		std::uint32_t ch2 = 0;
		std::uint32_t ch3 = 0;
		std::uint32_t ch4 = 0;

		for (std::uint32_t j = 0; j < 24; ++j)
		{
			ch1 |= static_cast<std::uint32_t>(frame[j] & 1u) << (23 - j);
			ch3 |= static_cast<std::uint32_t>((frame[j] & 2u) >> 1)
				   << (23 - j);
		}

		for (std::uint32_t j = 32; j < 56; ++j)
		{
			ch2 |= static_cast<std::uint32_t>(frame[j] & 1u) << (55 - j);
			ch4 |= static_cast<std::uint32_t>((frame[j] & 2u) >> 1)
				   << (55 - j);
		}

		return {SignExtend24(ch1), SignExtend24(ch2), SignExtend24(ch3),
				SignExtend24(ch4)};
	}

	float S24ToFloat32(std::int32_t sample) noexcept
	{
		return sample < 0
				   ? static_cast<float>(sample / 8388608.0f)
				   : static_cast<float>(sample / 8388607.0f);
	}

	bool DecodeCaptureUsbFrameToFloat32(
		const std::uint8_t frame[kCaptureBytesPerUsbFrame],
		float out_interleaved[kCaptureChannelCount]) noexcept
	{
		if (frame == nullptr || out_interleaved == nullptr)
			return false;

		const CaptureSampleFrame decoded = DecodeCaptureUsbFrame(frame);
		out_interleaved[0] = S24ToFloat32(decoded.ch1);
		out_interleaved[1] = S24ToFloat32(decoded.ch2);
		out_interleaved[2] = S24ToFloat32(decoded.ch3);
		out_interleaved[3] = S24ToFloat32(decoded.ch4);
		return true;
	}

	std::size_t DecodeCaptureBufferToFloat32(
		const std::uint8_t *usb_buffer, std::size_t usb_buffer_bytes,
		float *pcm_interleaved, std::size_t pcm_frame_capacity) noexcept
	{
		if (usb_buffer == nullptr || pcm_interleaved == nullptr)
			return 0;

		const std::size_t usb_frames =
			usb_buffer_bytes / kCaptureBytesPerUsbFrame;
		const std::size_t frames_to_decode =
			usb_frames < pcm_frame_capacity ? usb_frames : pcm_frame_capacity;

		for (std::size_t frame_index = 0; frame_index < frames_to_decode;
			 ++frame_index)
		{
			DecodeCaptureUsbFrameToFloat32(
				usb_buffer + frame_index * kCaptureBytesPerUsbFrame,
				pcm_interleaved + frame_index * kCaptureChannelCount);
		}

		return frames_to_decode;
	}

	std::size_t PcmFrameBytes(PcmSampleFormat format,
							  std::uint32_t channels) noexcept
	{
		if (!IsSupportedChannelCount(channels))
			return 0;

		switch (format)
		{
		case PcmSampleFormat::S16LE:
			return static_cast<std::size_t>(channels) * 2;
		case PcmSampleFormat::S24LEPacked:
			return static_cast<std::size_t>(channels) * 3;
		case PcmSampleFormat::S32LE:
			return static_cast<std::size_t>(channels) * 4;
		}

		return 0;
	}

	bool WritePcmFrame(const CaptureSampleFrame &frame, PcmSampleFormat format,
					   std::uint32_t channels, std::uint8_t *out,
					   std::size_t out_bytes) noexcept
	{
		const std::size_t frame_bytes = PcmFrameBytes(format, channels);
		if (out == nullptr || frame_bytes == 0 || out_bytes < frame_bytes)
			return false;

		const std::size_t sample_bytes = frame_bytes / channels;
		if (channels == 1)
		{
			const std::int32_t mixed = (frame.ch1 + frame.ch2) / 2;
			WriteOnePcmSample(mixed, format, out);
			return true;
		}

		for (std::uint32_t channel = 0; channel < channels; ++channel)
		{
			WriteOnePcmSample(ChannelSample(frame, channel), format,
							  out + channel * sample_bytes);
		}

		return true;
	}

	std::size_t DecodeCaptureBuffer(const std::uint8_t *usb_buffer,
									std::size_t usb_buffer_bytes, PcmSampleFormat format,
									std::uint32_t channels, std::uint8_t *pcm_buffer,
									std::size_t pcm_buffer_bytes) noexcept
	{
		if (usb_buffer == nullptr || pcm_buffer == nullptr)
			return 0;

		const std::size_t frame_bytes = PcmFrameBytes(format, channels);
		if (frame_bytes == 0)
			return 0;

		const std::size_t usb_frames =
			usb_buffer_bytes / kCaptureBytesPerUsbFrame;
		const std::size_t max_pcm_frames = pcm_buffer_bytes / frame_bytes;
		const std::size_t frames_to_decode =
			usb_frames < max_pcm_frames ? usb_frames : max_pcm_frames;

		for (std::size_t frame_index = 0; frame_index < frames_to_decode;
			 ++frame_index)
		{
			const CaptureSampleFrame decoded = DecodeCaptureUsbFrame(
				usb_buffer + frame_index * kCaptureBytesPerUsbFrame);
			WritePcmFrame(decoded, format, channels,
						  pcm_buffer + frame_index * frame_bytes, frame_bytes);
		}

		return frames_to_decode;
	}

} // namespace akai_eie
