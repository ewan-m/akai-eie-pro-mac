#ifndef AKAI_EIE_USB_TRANSPORT_HPP
#define AKAI_EIE_USB_TRANSPORT_HPP

#include <CoreAudio/AudioHardware.h>
#include <IOKit/IOReturn.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include "../protocol/akai_eie_protocol.hpp"

namespace akai_eie_hal {

class AkaiEieUsbTransport {
public:
	AkaiEieUsbTransport();
	~AkaiEieUsbTransport();

	AkaiEieUsbTransport(const AkaiEieUsbTransport &) = delete;
	AkaiEieUsbTransport &operator=(const AkaiEieUsbTransport &) = delete;

	bool Start() noexcept;
	void Stop() noexcept;
	void EnqueueOutput(const Float32 *interleaved, std::uint32_t frameCount) noexcept;

	bool IsStarted() const noexcept { return started_.load(); }

private:
	void WorkerMain() noexcept;
	bool OpenUsb() noexcept;
	void CloseUsb() noexcept;
	bool StartSyncTransfers() noexcept;
	bool StartPlaybackTransfers() noexcept;
	bool SubmitSyncTransfer(std::size_t transferIndex) noexcept;
	bool SubmitPlaybackTransfer(std::size_t transferIndex) noexcept;
	void HandleSyncComplete(std::size_t transferIndex, IOReturn result) noexcept;
	void HandlePlaybackComplete(std::size_t transferIndex, IOReturn result) noexcept;
	bool ReadCurrentUsbFrame(std::uint64_t *outFrame) noexcept;
	bool ReadCurrentDataUsbFrame(std::uint64_t *outFrame) noexcept;
	std::size_t BufferedOutputFrames() noexcept;
	std::size_t PopOutputFrames(std::uint8_t *outFrames,
		std::size_t frameCount) noexcept;
	void ResetOutputRing() noexcept;

	static void SyncComplete(void *refcon, IOReturn result,
		void *arg0) noexcept;
	static void PlaybackComplete(void *refcon, IOReturn result,
		void *arg0) noexcept;

	struct Impl;
	Impl *impl_ = nullptr;

	std::atomic<bool> started_ {false};
	std::atomic<bool> stopRequested_ {false};
	std::atomic<std::uint32_t> inFlightTransfers_ {0};

	std::mutex startMutex_;
	std::condition_variable startCondition_;
	std::thread workerThread_;
	bool startCompleted_ = false;
	bool startSucceeded_ = false;
};

} // namespace akai_eie_hal

#endif // AKAI_EIE_USB_TRANSPORT_HPP
