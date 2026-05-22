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

namespace akai_eie_hal
{

	class AkaiEieUsbTransport
	{
	public:
		AkaiEieUsbTransport();
		~AkaiEieUsbTransport();

		AkaiEieUsbTransport(const AkaiEieUsbTransport &) = delete;
		AkaiEieUsbTransport &operator=(const AkaiEieUsbTransport &) = delete;

		bool Start(std::uint32_t sampleRateHz) noexcept;
		void Stop() noexcept;
		void EnqueueOutput(const Float32 *interleaved, std::uint32_t frameCount) noexcept;
		std::uint32_t DequeueInput(Float32 *interleaved, std::uint32_t frameCount) noexcept;

		bool IsStarted() const noexcept { return started_.load(); }

	private:
		void WorkerMain() noexcept;
		bool OpenUsb() noexcept;
		void CloseUsb() noexcept;
		bool CreatePlaybackLowLatencyBuffers() noexcept;
		void DestroyPlaybackLowLatencyBuffers() noexcept;
		bool StartSyncTransfers() noexcept;
		bool StartPlaybackTransfers() noexcept;
		bool StartCaptureTransfers() noexcept;
		bool SubmitSyncTransfer(std::size_t transferIndex) noexcept;
		bool SubmitPlaybackTransfer(std::size_t transferIndex) noexcept;
		bool SubmitCaptureTransfer(std::size_t transferIndex) noexcept;
		void RequestRestart(const char *reason) noexcept;
		void HandleSyncComplete(std::size_t transferIndex, IOReturn result) noexcept;
		void HandlePlaybackComplete(std::size_t transferIndex, IOReturn result) noexcept;
		void HandleCaptureComplete(std::size_t transferIndex, IOReturn result,
								   std::uint32_t actualBytes) noexcept;
		bool ReadCurrentUsbFrame(std::uint64_t *outFrame) noexcept;
		bool ReadCurrentDataUsbFrame(std::uint64_t *outFrame) noexcept;
		std::uint32_t SampleRateHz() const noexcept;
		std::size_t BufferedOutputFrames() noexcept;
		std::size_t SubmittedPlaybackFrames() const noexcept;
		std::uint32_t CalculatePlaybackFramesWanted(
			std::size_t pendingOutputFrames) noexcept;
		void RecordPlaybackStats(std::uint32_t framesWanted,
								 std::size_t bufferedOutputFrames,
								 std::size_t pendingOutputFrames,
								 std::size_t copiedFrames,
								 std::uint16_t minPacketBytes,
								 std::uint16_t maxPacketBytes) noexcept;
		void InspectPlaybackPayload(const std::uint8_t *frames,
									std::size_t frameCount) noexcept;
		void PushCaptureUsbFrames(const std::uint8_t *usbFrames,
								  std::size_t byteCount) noexcept;
		std::size_t PopOutputFrames(std::uint8_t *outFrames,
									std::size_t frameCount) noexcept;
		void ResetOutputRing() noexcept;
		void ResetInputRing() noexcept;

		static void SyncComplete(void *refcon, IOReturn result,
								 void *arg0) noexcept;
		static void PlaybackComplete(void *refcon, IOReturn result,
									 void *arg0) noexcept;
		static void CaptureComplete(void *refcon, IOReturn result,
									void *arg0) noexcept;

		struct Impl;
		Impl *impl_ = nullptr;

		std::atomic<bool> started_{false};
		std::atomic<bool> stopRequested_{false};
		std::atomic<bool> restartRequested_{false};
		std::atomic<bool> captureRequested_{false};
		std::atomic<std::uint32_t> inFlightTransfers_{0};

		std::mutex startMutex_;
		std::condition_variable startCondition_;
		std::thread workerThread_;
		bool startCompleted_ = false;
		bool startSucceeded_ = false;
	};

} // namespace akai_eie_hal

#endif // AKAI_EIE_USB_TRANSPORT_HPP
