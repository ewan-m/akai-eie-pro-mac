#ifndef AKAI_EIE_PROTOCOL_HPP
#define AKAI_EIE_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>

namespace akai_eie {

constexpr std::uint16_t kVendorId = 0x09e8;
constexpr std::uint16_t kProductId = 0x0010;

constexpr std::uint8_t kKnownCaptureEndpointAddress = 0x86;
constexpr std::size_t kPlaybackBytesPerFrame = 12;
constexpr std::size_t kCaptureBytesPerUsbFrame = 64;
constexpr std::size_t kDefaultCaptureTransferBytes = 4096;
constexpr std::uint32_t kDefaultPlaybackIsoPacketCount = 40;

constexpr std::size_t kMaxControlPayloadBytes = 5;
constexpr std::size_t kResetControlTransferCount = 10;

enum class PcmSampleFormat {
	S16LE,
	S24LEPacked,
	S32LE,
};

struct ControlTransfer {
	std::uint8_t request_type = 0;
	std::uint8_t request = 0;
	std::uint16_t value = 0;
	std::uint16_t index = 0;
	std::uint16_t length = 0;
	std::uint8_t data[kMaxControlPayloadBytes] = {};
};

constexpr bool IsDeviceToHost(const ControlTransfer &transfer) noexcept
{
	return (transfer.request_type & 0x80) != 0;
}

bool IsSupportedSampleRate(std::uint32_t sample_rate) noexcept;

// Writes the vendor-specific control transfer sequence needed after selecting
// the active USB alternate settings. Returns 0 for an unsupported sample rate
// or if the output array is too small.
std::size_t BuildResetControlTransfers(std::uint32_t sample_rate,
	ControlTransfer *out_transfers, std::size_t transfer_capacity) noexcept;

struct PlaybackTimingState {
	std::uint8_t wanted_index = 0;
};

std::uint32_t CalculatePlaybackFramesWanted(std::uint32_t sample_rate,
	PlaybackTimingState &state, std::uint32_t feedback_frames = 0) noexcept;

constexpr bool IsReasonableSyncFeedback(std::uint8_t frames_elapsed) noexcept
{
	return frames_elapsed > 0 && frames_elapsed < 50;
}

struct IsoPacket {
	std::uint32_t offset_bytes = 0;
	std::uint32_t length_bytes = 0;
};

bool BuildIsoPacketLayout(std::uint32_t frames_wanted,
	std::uint32_t packet_count, std::uint32_t bytes_per_frame,
	IsoPacket *packets) noexcept;

struct PlaybackCopyState {
	std::size_t ring_frame = 0;
};

// Copies or silences one playback transfer. The ring buffer is expected to
// already contain AKAI playback wire frames of kPlaybackBytesPerFrame bytes.
bool FillPlaybackTransfer(const std::uint8_t *ring_frames,
	std::size_t ring_frame_count, bool running,
	std::uint32_t frames_wanted, PlaybackCopyState &state,
	std::uint8_t *usb_buffer, std::size_t usb_buffer_bytes,
	std::size_t *bytes_written) noexcept;

struct CaptureSampleFrame {
	std::int32_t ch1 = 0;
	std::int32_t ch2 = 0;
	std::int32_t ch3 = 0;
	std::int32_t ch4 = 0;
};

CaptureSampleFrame DecodeCaptureUsbFrame(
	const std::uint8_t frame[kCaptureBytesPerUsbFrame]) noexcept;

std::size_t PcmFrameBytes(PcmSampleFormat format,
	std::uint32_t channels) noexcept;

bool WritePcmFrame(const CaptureSampleFrame &frame, PcmSampleFormat format,
	std::uint32_t channels, std::uint8_t *out,
	std::size_t out_bytes) noexcept;

// Decodes whole 64-byte capture USB frames. Returns the number of PCM frames
// written and stops before writing a partial PCM frame.
std::size_t DecodeCaptureBuffer(const std::uint8_t *usb_buffer,
	std::size_t usb_buffer_bytes, PcmSampleFormat format,
	std::uint32_t channels, std::uint8_t *pcm_buffer,
	std::size_t pcm_buffer_bytes) noexcept;

} // namespace akai_eie

#endif // AKAI_EIE_PROTOCOL_HPP
