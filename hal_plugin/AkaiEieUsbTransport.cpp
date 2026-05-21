#include "AkaiEieUsbTransport.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>
#include <os/log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>
#include <pthread.h>

namespace akai_eie_hal
{
	namespace
	{

		using DeviceInterface = IOUSBDeviceInterface942;
		using InterfaceInterface = IOUSBInterfaceInterface942;

		constexpr std::uint32_t kDefaultSampleRateHz = 48000;
		constexpr std::uint32_t kMaxSampleRateHz = 96000;
		constexpr std::size_t kOutputRingFrameCount = kMaxSampleRateHz / 2;
		constexpr std::size_t kInputRingFrameCount = kMaxSampleRateHz / 2;
		constexpr std::size_t kCaptureTransferCount = 2;
		constexpr std::size_t kCaptureTransferBytes =
			4 * akai_eie::kDefaultCaptureTransferBytes;
		constexpr std::size_t kPlaybackTransferCount = 8;
		constexpr std::size_t kSyncTransferCount = 2;
		constexpr std::size_t kMaxSyncPacketBytes = 64;
		constexpr std::uint32_t kPlaybackIsoPacketCount =
			akai_eie::kDefaultPlaybackIsoPacketCount;
		constexpr std::uint32_t kMaxPlaybackFramesPerTransfer = 480;
		constexpr std::size_t kMaxPlaybackTransferBytes =
			kMaxPlaybackFramesPerTransfer * akai_eie::kPlaybackBytesPerFrame;
		constexpr std::uint64_t kUsbScheduleLeadBusFrames = 16;
		constexpr std::uint64_t kUsbScheduleRecoveryLeadBusFrames = 24;
		constexpr std::uint32_t kIsoSubmitAttemptCount = 4;
		constexpr std::uint32_t kMaxConsecutivePlaybackErrors = 3;
		constexpr std::uint32_t kMaxConsecutiveCaptureErrors = 3;
		constexpr std::uint32_t kMaxConsecutiveEmptySyncPackets = 64;
		constexpr std::size_t kSilenceRampFrameCount = 128;
		constexpr double kNominalPlaybackTransferSeconds = 0.005;
		constexpr double kPlaybackServoFilterSeconds = 2.0;
		constexpr double kPlaybackServoDeadbandFrames = 512.0;
		constexpr double kPlaybackServoPpmAtTargetError = 1000.0;
		constexpr double kPlaybackServoMaxPpm = 1000.0;
		constexpr std::chrono::milliseconds kStartTimeout{12000};
		constexpr std::chrono::milliseconds kRestartBackoff{250};

		std::size_t OutputPrimeFrameCount(std::uint32_t sampleRateHz) noexcept
		{
			return std::max<std::size_t>(1, sampleRateHz / 20);
		}

		std::size_t OutputReprimeFrameCount(std::uint32_t sampleRateHz) noexcept
		{
			return OutputPrimeFrameCount(sampleRateHz);
		}

		std::size_t OutputServoTargetFrameCount(
			std::uint32_t sampleRateHz) noexcept
		{
			return OutputPrimeFrameCount(sampleRateHz);
		}

		double PlaybackTransferSeconds(std::uint32_t frameListMicros) noexcept
		{
			const double transferSeconds =
				(static_cast<double>(kPlaybackIsoPacketCount) *
				 static_cast<double>(std::max<std::uint32_t>(
					 frameListMicros, 1))) /
				1000000.0;
			constexpr double kMaxProtocolTransferSeconds =
				static_cast<double>(kMaxPlaybackFramesPerTransfer) /
				static_cast<double>(kMaxSampleRateHz);
			return transferSeconds > 0.0 &&
						   transferSeconds <= kMaxProtocolTransferSeconds
					   ? transferSeconds
					   : kNominalPlaybackTransferSeconds;
		}

		double PlaybackServoCorrectionPpm(
			std::uint32_t sampleRateHz,
			double filteredOutputFrames) noexcept
		{
			const double targetFrames = static_cast<double>(
				OutputServoTargetFrameCount(sampleRateHz));
			const double errorFrames =
				filteredOutputFrames - targetFrames;
			const double absoluteErrorFrames = std::fabs(errorFrames);
			if (absoluteErrorFrames <= kPlaybackServoDeadbandFrames)
				return 0.0;

			const double effectiveErrorFrames = std::copysign(
				absoluteErrorFrames - kPlaybackServoDeadbandFrames,
				errorFrames);
			const double correctionPpm =
				(effectiveErrorFrames / targetFrames) *
				kPlaybackServoPpmAtTargetError;
			return std::clamp(correctionPpm, -kPlaybackServoMaxPpm,
							  kPlaybackServoMaxPpm);
		}

		std::uint64_t PlaybackTransferDurationBusFrames(
			std::uint32_t frameListMicros) noexcept
		{
			const std::uint64_t durationMicros =
				static_cast<std::uint64_t>(kPlaybackIsoPacketCount) * std::max<std::uint32_t>(frameListMicros, 1);
			return std::max<std::uint64_t>(1, (durationMicros + 999) / 1000);
		}

		const char *IOReturnName(IOReturn value) noexcept
		{
			switch (value)
			{
			case kIOReturnSuccess:
				return "success";
			case kIOReturnExclusiveAccess:
				return "exclusive access";
			case kIOReturnNoDevice:
				return "no device";
			case kIOReturnNotOpen:
				return "not open";
			case kIOReturnNoMemory:
				return "no memory";
			case kIOReturnNotFound:
				return "not found";
			case kIOReturnAborted:
				return "aborted";
			case kIOReturnIsoTooOld:
				return "iso too old";
			case kIOReturnIsoTooNew:
				return "iso too new";
			case kIOReturnBadArgument:
				return "bad argument";
			case kIOReturnNoBandwidth:
				return "no bandwidth";
			case kIOReturnUnderrun:
				return "underrun";
			case kIOReturnOverrun:
				return "overrun";
			default:
				return "error";
			}
		}

		bool IsPlausibleSyncFeedback(
			std::uint32_t sampleRateHz,
			std::uint32_t framesPerMillisecond) noexcept
		{
			const std::uint32_t nominalFramesPerMillisecond =
				(sampleRateHz + 500) / 1000;
			constexpr std::uint32_t kToleranceFrames = 8;
			const std::uint32_t low =
				nominalFramesPerMillisecond > kToleranceFrames
					? nominalFramesPerMillisecond - kToleranceFrames
					: 1;
			const std::uint32_t high =
				nominalFramesPerMillisecond + kToleranceFrames;
			return framesPerMillisecond >= low &&
				   framesPerMillisecond <= high;
		}

		std::uint32_t DecodeSyncFeedbackFrames(
			std::uint32_t sampleRateHz, const std::uint8_t *buffer,
			std::uint16_t actualBytes) noexcept
		{
			if (buffer == nullptr || actualBytes == 0)
				return 0;

			// The EIE Pro reports a custom one-byte elapsed-frame count; accept
			// standard USB Audio fixed-point feedback as a fallback.
			const std::uint8_t akaiFrames = buffer[0];
			if (IsPlausibleSyncFeedback(sampleRateHz, akaiFrames))
				return akaiFrames;

			if (actualBytes >= 3)
			{
				const std::uint32_t raw10_14 =
					static_cast<std::uint32_t>(buffer[0]) | (static_cast<std::uint32_t>(buffer[1]) << 8) | (static_cast<std::uint32_t>(buffer[2]) << 16);
				const std::uint32_t frames =
					(raw10_14 + (1u << 13)) >> 14;
				if (IsPlausibleSyncFeedback(sampleRateHz, frames))
					return frames;
			}

			if (actualBytes >= 4)
			{
				const std::uint32_t raw16_16 =
					static_cast<std::uint32_t>(buffer[0]) | (static_cast<std::uint32_t>(buffer[1]) << 8) | (static_cast<std::uint32_t>(buffer[2]) << 16) | (static_cast<std::uint32_t>(buffer[3]) << 24);
				const std::uint32_t frames = static_cast<std::uint32_t>(
					((static_cast<std::uint64_t>(raw16_16) * 8u) + (1u << 15)) >> 16);
				if (IsPlausibleSyncFeedback(sampleRateHz, frames))
					return frames;
			}

			return 0;
		}

		std::int32_t ReadS24LE(const std::uint8_t *frame) noexcept
		{
			std::uint32_t value =
				static_cast<std::uint32_t>(frame[0]) | (static_cast<std::uint32_t>(frame[1]) << 8) | (static_cast<std::uint32_t>(frame[2]) << 16);
			if ((value & 0x00800000u) != 0)
				value |= 0xff000000u;
			return static_cast<std::int32_t>(value);
		}

		Float32 S24ToFloat32(std::int32_t sample) noexcept
		{
			return sample < 0
					   ? static_cast<Float32>(sample / 8388608.0f)
					   : static_cast<Float32>(sample / 8388607.0f);
		}

		void WriteS24LE(std::int32_t sample, std::uint8_t *frame) noexcept
		{
			const std::uint32_t value =
				static_cast<std::uint32_t>(sample) & 0x00ffffffu;
			frame[0] = static_cast<std::uint8_t>(value & 0xffu);
			frame[1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
			frame[2] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
		}

		std::int32_t ScaleS24(std::int32_t sample, std::size_t numerator,
							  std::size_t denominator) noexcept
		{
			if (denominator == 0 || numerator == denominator)
				return sample;
			if (numerator == 0)
				return 0;
			return static_cast<std::int32_t>(
				(static_cast<std::int64_t>(sample) * static_cast<std::int64_t>(numerator)) / static_cast<std::int64_t>(denominator));
		}

		bool SetMatchingNumber(CFMutableDictionaryRef dictionary, const char *key,
							   std::uint16_t value) noexcept
		{
			if (dictionary == nullptr || key == nullptr)
				return false;

			CFStringRef cfKey = CFStringCreateWithCString(
				kCFAllocatorDefault, key, kCFStringEncodingUTF8);
			if (cfKey == nullptr)
				return false;

			const SInt32 signedValue = static_cast<SInt32>(value);
			CFNumberRef number = CFNumberCreate(kCFAllocatorDefault,
												kCFNumberSInt32Type, &signedValue);
			if (number == nullptr)
			{
				CFRelease(cfKey);
				return false;
			}

			CFDictionarySetValue(dictionary, cfKey, number);
			CFRelease(number);
			CFRelease(cfKey);
			return true;
		}

		io_service_t FindDeviceWithClassName(const char *className) noexcept
		{
			CFMutableDictionaryRef matching = IOServiceMatching(className);
			if (matching == nullptr)
				return IO_OBJECT_NULL;

			if (!SetMatchingNumber(matching, kUSBVendorID, akai_eie::kVendorId) || !SetMatchingNumber(matching, kUSBProductID,
																									  akai_eie::kProductId))
			{
				CFRelease(matching);
				return IO_OBJECT_NULL;
			}

			io_iterator_t iterator = IO_OBJECT_NULL;
			const kern_return_t result = IOServiceGetMatchingServices(
				kIOMainPortDefault, matching, &iterator);
			if (result != KERN_SUCCESS)
				return IO_OBJECT_NULL;

			io_service_t service = IOIteratorNext(iterator);
			IOObjectRelease(iterator);
			return service;
		}

		io_service_t FindAkaiDeviceService() noexcept
		{
			io_service_t service = FindDeviceWithClassName(kIOUSBDeviceClassName);
			if (service != IO_OBJECT_NULL)
				return service;
			return FindDeviceWithClassName(kIOUSBHostDeviceClassName);
		}

		bool CreateDeviceInterface(io_service_t service,
								   DeviceInterface ***outDevice) noexcept
		{
			if (service == IO_OBJECT_NULL || outDevice == nullptr)
				return false;

			IOCFPlugInInterface **plugin = nullptr;
			SInt32 score = 0;
			IOReturn result = IOCreatePlugInInterfaceForService(service,
																kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin,
																&score);
			if (result != kIOReturnSuccess || plugin == nullptr)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: device plugin failed %{public}s (%{public}#x)",
							 IOReturnName(result), result);
				return false;
			}

			result = (*plugin)->QueryInterface(plugin,
											   CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID942),
											   reinterpret_cast<LPVOID *>(outDevice));
			(*plugin)->Release(plugin);

			if (result != kIOReturnSuccess || *outDevice == nullptr)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: device interface failed %{public}s (%{public}#x)",
							 IOReturnName(result), result);
				return false;
			}

			return true;
		}

		bool CreateInterfaceInterface(io_service_t service,
									  InterfaceInterface ***outInterface) noexcept
		{
			if (service == IO_OBJECT_NULL || outInterface == nullptr)
				return false;

			IOCFPlugInInterface **plugin = nullptr;
			SInt32 score = 0;
			IOReturn result = IOCreatePlugInInterfaceForService(service,
																kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin,
																&score);
			if (result != kIOReturnSuccess || plugin == nullptr)
				return false;

			result = (*plugin)->QueryInterface(plugin,
											   CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID942),
											   reinterpret_cast<LPVOID *>(outInterface));
			(*plugin)->Release(plugin);
			return result == kIOReturnSuccess && *outInterface != nullptr;
		}

		void ReleaseInterface(InterfaceInterface ***interfacePtr,
							  bool *interfaceOpen) noexcept
		{
			if (interfacePtr == nullptr || *interfacePtr == nullptr)
				return;

			if (interfaceOpen != nullptr && *interfaceOpen)
			{
				(**interfacePtr)->USBInterfaceClose(*interfacePtr);
				*interfaceOpen = false;
			}
			(**interfacePtr)->Release(*interfacePtr);
			*interfacePtr = nullptr;
		}

	} // namespace

	struct AkaiEieUsbTransport::Impl
	{
		struct PlaybackTransfer
		{
			AkaiEieUsbTransport *owner = nullptr;
			std::size_t index = 0;
			bool submitted = false;
			std::array<std::uint8_t, kMaxPlaybackTransferBytes> buffer{};
			std::array<IOUSBIsocFrame, kPlaybackIsoPacketCount> frames{};
		};

		struct SyncTransfer
		{
			AkaiEieUsbTransport *owner = nullptr;
			std::size_t index = 0;
			bool submitted = false;
			std::array<std::uint8_t, kMaxSyncPacketBytes> buffer{};
			IOUSBIsocFrame frame{};
		};

		struct CaptureTransfer
		{
			AkaiEieUsbTransport *owner = nullptr;
			std::size_t index = 0;
			bool submitted = false;
			std::array<std::uint8_t, kCaptureTransferBytes> buffer{};
			UInt32 requestedBytes = 0;
		};

		DeviceInterface **device = nullptr;
		InterfaceInterface **playInterface = nullptr;
		InterfaceInterface **dataInterface = nullptr;
		InterfaceInterface **syncInterface = nullptr;
		CFRunLoopSourceRef playAsyncSource = nullptr;
		CFRunLoopSourceRef dataAsyncSource = nullptr;
		bool deviceOpen = false;
		bool playInterfaceOpen = false;
		bool dataInterfaceOpen = false;
		UInt8 playPipe = 0;
		UInt8 syncPipe = 0;
		UInt8 capturePipe = 0;
		bool captureRunning = false;
		UInt16 playMaxPacketSize = 0;
		UInt16 syncMaxPacketSize = 0;
		UInt16 captureMaxPacketSize = 0;
		std::uint32_t frameListMicros = 1000;
		std::uint64_t nextFrameStart = 0;
		std::uint64_t syncNextFrameStart = 0;
		std::atomic<std::uint32_t> sampleRateHz{kDefaultSampleRateHz};
		std::array<PlaybackTransfer, kPlaybackTransferCount> playbackTransfers{};
		std::array<SyncTransfer, kSyncTransferCount> syncTransfers{};
		std::array<CaptureTransfer, kCaptureTransferCount> captureTransfers{};

		std::mutex outputMutex;
		std::array<std::uint8_t,
				   kOutputRingFrameCount * akai_eie::kPlaybackBytesPerFrame>
			outputRing{};
		std::size_t outputReadFrame = 0;
		std::size_t outputWriteFrame = 0;
		std::size_t outputFrameCount = 0;
		bool outputPrimed = false;
		std::uint64_t underrunCount = 0;
		std::uint64_t rebufferCount = 0;
		std::uint64_t droppedFrameCount = 0;
		std::uint64_t syncErrorCount = 0;
		std::uint64_t playbackCompletionErrorCount = 0;
		std::uint32_t consecutivePlaybackErrorCount = 0;
		std::uint32_t consecutiveEmptySyncPacketCount = 0;
		double playbackFrameAccumulator = 0.0;
		double playbackServoCorrectionPpm = 0.0;
		double playbackServoFilteredFrames = 0.0;
		bool playbackServoFilterPrimed = false;
		std::chrono::steady_clock::time_point playbackStatsStarted{};
		std::uint64_t playbackStatsTransferCount = 0;
		std::uint64_t playbackStatsFrameSum = 0;
		std::uint64_t playbackStatsUnderrunFrameCount = 0;
		std::uint64_t playbackStatsLateScheduleCount = 0;
		std::uint64_t playbackStatsMaxLateBusFrames = 0;
		std::uint64_t playbackStatsSubmitRetryCount = 0;
		std::uint64_t playbackStatsCompletionErrorCount = 0;
		std::uint64_t playbackStatsBadIsoFrameCount = 0;
		std::uint64_t playbackStatsCaptureCompletionCount = 0;
		std::uint64_t playbackStatsCaptureByteCount = 0;
		std::uint32_t playbackStatsMinFrames = 0;
		std::uint32_t playbackStatsMaxFrames = 0;
		std::size_t playbackStatsLastBufferedFrames = 0;
		double playbackStatsLastFilteredFrames = 0.0;
		bool syncFeedbackFallbackLogged = false;
		bool syncFeedbackEmptyLogged = false;
		std::array<std::int32_t, akai_eie::kPlaybackChannelCount>
			lastPlaybackSamples{};
		bool outputWasSilent = true;

		std::mutex inputMutex;
		std::array<Float32, kInputRingFrameCount * akai_eie::kPlaybackChannelCount>
			inputRing{};
		std::size_t inputReadFrame = 0;
		std::size_t inputWriteFrame = 0;
		std::size_t inputFrameCount = 0;
		std::uint64_t inputOverrunCount = 0;
		std::uint64_t captureCompletionErrorCount = 0;
		std::uint32_t consecutiveCaptureErrorCount = 0;

		std::mutex runLoopMutex;
		CFRunLoopRef workerRunLoop = nullptr;
	};

	AkaiEieUsbTransport::AkaiEieUsbTransport()
		: impl_(new (std::nothrow) Impl)
	{
		if (impl_ != nullptr)
		{
			for (std::size_t i = 0; i < impl_->playbackTransfers.size(); ++i)
			{
				impl_->playbackTransfers[i].owner = this;
				impl_->playbackTransfers[i].index = i;
			}
			for (std::size_t i = 0; i < impl_->syncTransfers.size(); ++i)
			{
				impl_->syncTransfers[i].owner = this;
				impl_->syncTransfers[i].index = i;
			}
			for (std::size_t i = 0; i < impl_->captureTransfers.size(); ++i)
			{
				impl_->captureTransfers[i].owner = this;
				impl_->captureTransfers[i].index = i;
			}
		}
	}

	AkaiEieUsbTransport::~AkaiEieUsbTransport()
	{
		Stop();
		delete impl_;
		impl_ = nullptr;
	}

	bool AkaiEieUsbTransport::Start(std::uint32_t sampleRateHz) noexcept
	{
		if (impl_ == nullptr)
			return false;
		if (!akai_eie::IsSupportedSampleRate(sampleRateHz))
			return false;

		std::unique_lock<std::mutex> lock(startMutex_);
		if (started_.load())
		{
			if (SampleRateHz() == sampleRateHz)
				return true;
			lock.unlock();
			Stop();
			lock.lock();
		}
		if (workerThread_.joinable())
		{
			lock.unlock();
			Stop();
			lock.lock();
		}

		impl_->sampleRateHz.store(sampleRateHz, std::memory_order_release);
		ResetOutputRing();
		ResetInputRing();
		stopRequested_.store(false);
		restartRequested_.store(false);
		captureRequested_.store(false);
		inFlightTransfers_.store(0);
		startCompleted_ = false;
		startSucceeded_ = false;

		try
		{
			workerThread_ = std::thread(&AkaiEieUsbTransport::WorkerMain, this);
		}
		catch (...)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: failed to start worker thread");
			return false;
		}

		const bool completed = startCondition_.wait_for(lock, kStartTimeout,
														[this]() noexcept
														{ return startCompleted_; });
		if (!completed)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: worker start timed out");
			lock.unlock();
			Stop();
			return false;
		}

		const bool ok = startSucceeded_;
		lock.unlock();
		if (!ok && workerThread_.joinable())
			workerThread_.join();
		return ok;
	}

	void AkaiEieUsbTransport::Stop() noexcept
	{
		if (impl_ == nullptr)
			return;

		stopRequested_.store(true);
		restartRequested_.store(false);
		captureRequested_.store(false);

		CFRunLoopRef runLoop = nullptr;
		{
			std::lock_guard<std::mutex> lock(impl_->runLoopMutex);
			runLoop = impl_->workerRunLoop;
			if (runLoop != nullptr)
				CFRetain(runLoop);
		}
		if (runLoop != nullptr)
		{
			CFRunLoopStop(runLoop);
			CFRelease(runLoop);
		}

		if (workerThread_.joinable())
			workerThread_.join();

		started_.store(false);
		ResetOutputRing();
		ResetInputRing();
	}

	void AkaiEieUsbTransport::EnqueueOutput(const Float32 *interleaved,
											std::uint32_t frameCount) noexcept
	{
		if (impl_ == nullptr || interleaved == nullptr || frameCount == 0 || !started_.load())
			return;

		std::lock_guard<std::mutex> lock(impl_->outputMutex);
		for (std::uint32_t frame = 0; frame < frameCount; ++frame)
		{
			if (impl_->outputFrameCount == kOutputRingFrameCount)
			{
				impl_->outputReadFrame =
					(impl_->outputReadFrame + 1) % kOutputRingFrameCount;
				--impl_->outputFrameCount;
				++impl_->droppedFrameCount;
			}

			akai_eie::EncodePlaybackFrameFromFloat32(
				interleaved + frame * akai_eie::kPlaybackChannelCount,
				impl_->outputRing.data() + impl_->outputWriteFrame * akai_eie::kPlaybackBytesPerFrame);
			impl_->outputWriteFrame =
				(impl_->outputWriteFrame + 1) % kOutputRingFrameCount;
			++impl_->outputFrameCount;
		}

		if (!impl_->outputPrimed &&
			impl_->outputFrameCount >= OutputPrimeFrameCount(SampleRateHz()))
		{
			impl_->outputPrimed = true;
			os_log(OS_LOG_DEFAULT,
				   "AKAI EIE Pro USB: playback buffer primed %{public}zu frames",
				   impl_->outputFrameCount);
		}
	}

	std::uint32_t AkaiEieUsbTransport::DequeueInput(
		Float32 *interleaved, std::uint32_t frameCount) noexcept
	{
		if (interleaved == nullptr || frameCount == 0)
			return 0;

		std::memset(interleaved, 0,
					static_cast<std::size_t>(frameCount) *
						akai_eie::kPlaybackChannelCount * sizeof(Float32));

		if (impl_ == nullptr || !started_.load())
			return 0;

		captureRequested_.store(true, std::memory_order_release);

		std::uint32_t copied = 0;
		std::lock_guard<std::mutex> lock(impl_->inputMutex);
		while (copied < frameCount && impl_->inputFrameCount > 0)
		{
			const std::size_t contiguousAvailable =
				std::min(impl_->inputFrameCount,
						 kInputRingFrameCount - impl_->inputReadFrame);
			const std::size_t contiguousNeeded = frameCount - copied;
			const std::size_t framesToCopy =
				std::min(contiguousAvailable, contiguousNeeded);
			const std::size_t samplesToCopy =
				framesToCopy * akai_eie::kPlaybackChannelCount;

			std::memcpy(interleaved +
							copied * akai_eie::kPlaybackChannelCount,
						impl_->inputRing.data() +
							impl_->inputReadFrame *
								akai_eie::kPlaybackChannelCount,
						samplesToCopy * sizeof(Float32));
			impl_->inputReadFrame =
				(impl_->inputReadFrame + framesToCopy) % kInputRingFrameCount;
			impl_->inputFrameCount -= framesToCopy;
			copied += static_cast<std::uint32_t>(framesToCopy);
		}

		return copied;
	}

	void AkaiEieUsbTransport::WorkerMain() noexcept
	{
		const int qosResult =
			pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
		if (qosResult != 0)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: failed to raise worker QoS %{public}d",
						 qosResult);
		}

		CFRunLoopRef runLoop = CFRunLoopGetCurrent();
		CFRetain(runLoop);
		{
			std::lock_guard<std::mutex> lock(impl_->runLoopMutex);
			impl_->workerRunLoop = runLoop;
		}

		auto abortActivePipes = [this]() noexcept
		{
			if (impl_->playInterface != nullptr && impl_->playPipe != 0)
				(*impl_->playInterface)->AbortPipe(impl_->playInterface, impl_->playPipe);
			if (impl_->syncInterface != nullptr && impl_->syncPipe != 0)
				(*impl_->syncInterface)->AbortPipe(impl_->syncInterface, impl_->syncPipe);
			if (impl_->dataInterface != nullptr && impl_->capturePipe != 0)
				(*impl_->dataInterface)->AbortPipe(impl_->dataInterface, impl_->capturePipe);
		};
		auto clearSubmittedTransfers = [this]() noexcept
		{
			for (auto &transfer : impl_->playbackTransfers)
				transfer.submitted = false;
			for (auto &transfer : impl_->syncTransfers)
				transfer.submitted = false;
			for (auto &transfer : impl_->captureTransfers)
				transfer.submitted = false;
			inFlightTransfers_.store(0);
		};
		auto drainInFlightTransfers = [this, &clearSubmittedTransfers]() noexcept
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (inFlightTransfers_.load() > 0 && std::chrono::steady_clock::now() < deadline)
			{
				CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
			}
			const std::uint32_t remaining = inFlightTransfers_.load();
			if (remaining > 0)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: timed out waiting for %{public}u transfers",
							 remaining);
				clearSubmittedTransfers();
				stopRequested_.store(true);
				return false;
			}
			return true;
		};
		auto publishStartResult = [this](bool open) noexcept
		{
			bool shouldNotify = false;
			{
				std::lock_guard<std::mutex> lock(startMutex_);
				if (!startCompleted_)
				{
					startSucceeded_ = open;
					startCompleted_ = true;
					shouldNotify = true;
				}
			}
			if (shouldNotify)
				startCondition_.notify_all();
		};
		auto startResultPublished = [this]() noexcept
		{
			std::lock_guard<std::mutex> lock(startMutex_);
			return startCompleted_;
		};
		auto waitBeforeRetry = [this]() noexcept
		{
			const auto deadline = std::chrono::steady_clock::now() + kRestartBackoff;
			while (!stopRequested_.load() && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
		};
		auto startCaptureIfRequested = [this]() noexcept
		{
			if (!captureRequested_.load(std::memory_order_acquire) ||
				impl_->captureRunning || impl_->capturePipe == 0)
			{
				return;
			}

			if (!StartCaptureTransfers())
				RequestRestart("capture transfer start failed");
		};

		while (!stopRequested_.load())
		{
			restartRequested_.store(false);

			const bool usbOpen = OpenUsb();
			const bool open =
				usbOpen && StartPlaybackTransfers();
			if (!open)
			{
				if (usbOpen)
				{
					abortActivePipes();
					if (!drainInFlightTransfers())
					{
						CloseUsb();
						break;
					}
				}
				CloseUsb();
				if (!startResultPublished())
				{
					publishStartResult(false);
					break;
				}
				waitBeforeRetry();
				continue;
			}

			started_.store(true);
			publishStartResult(true);
			while (!stopRequested_.load() && !restartRequested_.load())
			{
				CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);
				startCaptureIfRequested();
			}

			started_.store(false);
			abortActivePipes();
			const bool drained = drainInFlightTransfers();
			CloseUsb();
			if (!drained)
				break;

			if (!restartRequested_.load() || stopRequested_.load())
				break;

			ResetOutputRing();
			ResetInputRing();
			waitBeforeRetry();
		}

		started_.store(false);
		publishStartResult(false);
		CloseUsb();

		{
			std::lock_guard<std::mutex> lock(impl_->runLoopMutex);
			impl_->workerRunLoop = nullptr;
		}
		CFRelease(runLoop);
	}

	bool AkaiEieUsbTransport::OpenUsb() noexcept
	{
		io_service_t service = FindAkaiDeviceService();
		if (service == IO_OBJECT_NULL)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: device 09e8:0010 not found");
			return false;
		}

		const bool deviceInterfaceOk = CreateDeviceInterface(service, &impl_->device);
		IOObjectRelease(service);
		if (!deviceInterfaceOk)
			return false;

		IOReturn result = (*impl_->device)->USBDeviceOpen(impl_->device);
		if (result == kIOReturnExclusiveAccess)
			result = (*impl_->device)->USBDeviceOpenSeize(impl_->device);
		if (result != kIOReturnSuccess)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: USBDeviceOpen failed %{public}s (%{public}#x)",
						 IOReturnName(result), result);
			return false;
		}
		impl_->deviceOpen = true;

		UInt8 configuration = 0;
		(*impl_->device)->GetConfiguration(impl_->device, &configuration);

		IOUSBConfigurationDescriptorPtr descriptor = nullptr;
		result = (*impl_->device)->GetConfigurationDescriptorPtr(impl_->device, 0, &descriptor);
		const UInt8 desiredConfiguration =
			(result == kIOReturnSuccess && descriptor != nullptr)
				? descriptor->bConfigurationValue
				: 1;
		if (configuration != desiredConfiguration)
		{
			result = (*impl_->device)->SetConfiguration(impl_->device, desiredConfiguration);
			if (result != kIOReturnSuccess)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: SetConfiguration(%{public}u) failed %{public}s (%{public}#x)",
							 desiredConfiguration, IOReturnName(result), result);
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}

		auto openInterface = [this](UInt8 number, InterfaceInterface ***outInterface,
									bool *outOpen) noexcept -> bool
		{
			IOUSBFindInterfaceRequest request{};
			request.bInterfaceClass = kIOUSBFindInterfaceDontCare;
			request.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
			request.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
			request.bAlternateSetting = kIOUSBFindInterfaceDontCare;

			io_iterator_t iterator = IO_OBJECT_NULL;
			IOReturn iteratorResult = (*impl_->device)->CreateInterfaceIterator(impl_->device, &request, &iterator);
			if (iteratorResult != kIOReturnSuccess)
				return false;

			bool found = false;
			io_service_t interfaceService = IO_OBJECT_NULL;
			while ((interfaceService = IOIteratorNext(iterator)) != IO_OBJECT_NULL)
			{
				InterfaceInterface **candidate = nullptr;
				const bool created = CreateInterfaceInterface(
					interfaceService, &candidate);
				IOObjectRelease(interfaceService);
				if (!created)
					continue;

				UInt8 interfaceNumber = 0xff;
				(*candidate)->GetInterfaceNumber(candidate, &interfaceNumber);
				if (interfaceNumber != number)
				{
					(*candidate)->Release(candidate);
					continue;
				}

				IOReturn openResult =
					(*candidate)->USBInterfaceOpen(candidate);
				if (openResult == kIOReturnExclusiveAccess)
					openResult = (*candidate)->USBInterfaceOpenSeize(candidate);
				if (openResult != kIOReturnSuccess)
				{
					os_log_error(OS_LOG_DEFAULT,
								 "AKAI EIE Pro USB: interface %{public}u open failed %{public}s (%{public}#x)",
								 number, IOReturnName(openResult), openResult);
					(*candidate)->Release(candidate);
					break;
				}

				IOReturn altResult =
					(*candidate)->SetAlternateInterface(candidate, 1);
				if (altResult != kIOReturnSuccess)
				{
					os_log_error(OS_LOG_DEFAULT,
								 "AKAI EIE Pro USB: interface %{public}u alt 1 failed %{public}s (%{public}#x)",
								 number, IOReturnName(altResult), altResult);
					(*candidate)->USBInterfaceClose(candidate);
					(*candidate)->Release(candidate);
					break;
				}

				*outInterface = candidate;
				*outOpen = true;
				found = true;
				break;
			}

			IOObjectRelease(iterator);
			return found;
		};

		if (!openInterface(0, &impl_->playInterface, &impl_->playInterfaceOpen))
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: playback interface 0 not available");
			return false;
		}
		if (!openInterface(1, &impl_->dataInterface, &impl_->dataInterfaceOpen))
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: data interface 1 not available");
			return false;
		}

		auto findPipe = [](InterfaceInterface **usbInterface, UInt8 direction,
						   UInt8 transferType, bool matchEndpoint, UInt8 endpointNumber,
						   bool requireSmallPacket, UInt8 *outPipe,
						   UInt16 *outMaxPacket) noexcept -> bool
		{
			UInt8 endpointCount = 0;
			if ((*usbInterface)->GetNumEndpoints(usbInterface, &endpointCount) != kIOReturnSuccess)
			{
				return false;
			}

			for (UInt8 pipe = 1; pipe <= endpointCount; ++pipe)
			{
				UInt8 pipeDirection = 0;
				UInt8 pipeNumber = 0;
				UInt8 pipeTransferType = 0;
				UInt16 maxPacketSize = 0;
				UInt8 interval = 0;
				if ((*usbInterface)->GetPipeProperties(usbInterface, pipe, &pipeDirection, &pipeNumber, &pipeTransferType, &maxPacketSize, &interval) != kIOReturnSuccess)
				{
					continue;
				}
				(void)interval;

				if (pipeDirection != direction || pipeTransferType != transferType)
				{
					continue;
				}
				if (matchEndpoint && pipeNumber != endpointNumber)
					continue;
				if (requireSmallPacket && maxPacketSize >= 64)
					continue;

				*outPipe = pipe;
				*outMaxPacket = maxPacketSize;
				return true;
			}
			return false;
		};

		if (!findPipe(impl_->playInterface, kUSBOut, kUSBIsoc, false, 0, false,
					  &impl_->playPipe, &impl_->playMaxPacketSize))
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: playback isoch OUT pipe not found");
			return false;
		}
		if (findPipe(impl_->dataInterface, kUSBIn, kUSBIsoc, false, 0, true,
					 &impl_->syncPipe, &impl_->syncMaxPacketSize))
		{
			impl_->syncInterface = impl_->dataInterface;
		}
		else if (findPipe(impl_->playInterface, kUSBIn, kUSBIsoc, false, 0,
						  true, &impl_->syncPipe, &impl_->syncMaxPacketSize))
		{
			impl_->syncInterface = impl_->playInterface;
		}
		findPipe(impl_->dataInterface, kUSBIn, kUSBBulk, true,
				 akai_eie::kKnownCaptureEndpointAddress & 0x0f, false,
				 &impl_->capturePipe, &impl_->captureMaxPacketSize);

		const std::uint32_t sampleRateHz = SampleRateHz();
		akai_eie::ControlTransfer transfers[akai_eie::kResetControlTransferCount]{};
		const std::size_t transferCount = akai_eie::BuildResetControlTransfers(
			sampleRateHz, transfers, akai_eie::kResetControlTransferCount);
		if (transferCount == 0)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: cannot build reset sequence");
			return false;
		}
		for (std::size_t i = 0; i < transferCount; ++i)
		{
			std::array<std::uint8_t, akai_eie::kMaxControlPayloadBytes> payload{};
			std::memcpy(payload.data(), transfers[i].data, payload.size());

			IOUSBDevRequestTO request{};
			request.bmRequestType = transfers[i].request_type;
			request.bRequest = transfers[i].request;
			request.wValue = transfers[i].value;
			request.wIndex = transfers[i].index;
			request.wLength = transfers[i].length;
			request.pData = transfers[i].length == 0 ? nullptr : payload.data();
			request.noDataTimeout = 1000;
			request.completionTimeout = 1000;

			result = (*impl_->device)->DeviceRequestTO(impl_->device, &request);
			if (result != kIOReturnSuccess)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: reset transfer %{public}zu failed %{public}s (%{public}#x)",
							 i, IOReturnName(result), result);
				return false;
			}
		}

		result = (*impl_->playInterface)->CreateInterfaceAsyncEventSource(impl_->playInterface, &impl_->playAsyncSource);
		if (result != kIOReturnSuccess || impl_->playAsyncSource == nullptr)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: async source failed %{public}s (%{public}#x)",
						 IOReturnName(result), result);
			return false;
		}
		CFRunLoopAddSource(CFRunLoopGetCurrent(), impl_->playAsyncSource,
						   kCFRunLoopDefaultMode);

		const bool needsDataAsyncSource = impl_->capturePipe != 0;
		if (needsDataAsyncSource)
		{
			result = (*impl_->dataInterface)->CreateInterfaceAsyncEventSource(impl_->dataInterface, &impl_->dataAsyncSource);
			if (result != kIOReturnSuccess || impl_->dataAsyncSource == nullptr)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: data async source failed %{public}s (%{public}#x)",
							 IOReturnName(result), result);
				return false;
			}
			CFRunLoopAddSource(CFRunLoopGetCurrent(), impl_->dataAsyncSource,
							   kCFRunLoopDefaultMode);
		}

		(*impl_->playInterface)->GetFrameListTime(impl_->playInterface, &impl_->frameListMicros);
		std::uint64_t currentFrame = 0;
		if (!ReadCurrentUsbFrame(&currentFrame))
			currentFrame = 0;
		impl_->nextFrameStart = currentFrame + kUsbScheduleLeadBusFrames;
		if (impl_->syncPipe != 0)
		{
			if (!ReadCurrentDataUsbFrame(&currentFrame))
				currentFrame = 0;
			impl_->syncNextFrameStart =
				currentFrame + kUsbScheduleLeadBusFrames;
			os_log(OS_LOG_DEFAULT,
				   "AKAI EIE Pro USB: sync feedback endpoint detected but not scheduled; playback uses output buffer servo");
		}

		os_log(OS_LOG_DEFAULT,
			   "AKAI EIE Pro USB: opened %{public}u Hz playback pipe %{public}u maxPacket=%{public}u frameList=%{public}uus startFrame=%{public}llu syncPipe=%{public}u syncMaxPacket=%{public}u syncOnData=%{public}u capturePipe=%{public}u",
			   sampleRateHz, impl_->playPipe, impl_->playMaxPacketSize, impl_->frameListMicros,
			   static_cast<unsigned long long>(impl_->nextFrameStart),
			   impl_->syncPipe, impl_->syncMaxPacketSize,
			   impl_->syncInterface == impl_->dataInterface ? 1u : 0u,
			   impl_->capturePipe);
		return true;
	}

	void AkaiEieUsbTransport::CloseUsb() noexcept
	{
		if (impl_->playAsyncSource != nullptr)
		{
			CFRunLoopRemoveSource(CFRunLoopGetCurrent(), impl_->playAsyncSource,
								  kCFRunLoopDefaultMode);
			CFRelease(impl_->playAsyncSource);
			impl_->playAsyncSource = nullptr;
		}
		if (impl_->dataAsyncSource != nullptr)
		{
			CFRunLoopRemoveSource(CFRunLoopGetCurrent(), impl_->dataAsyncSource,
								  kCFRunLoopDefaultMode);
			CFRelease(impl_->dataAsyncSource);
			impl_->dataAsyncSource = nullptr;
		}

		ReleaseInterface(&impl_->dataInterface, &impl_->dataInterfaceOpen);
		ReleaseInterface(&impl_->playInterface, &impl_->playInterfaceOpen);
		impl_->syncInterface = nullptr;

		if (impl_->device != nullptr)
		{
			if (impl_->deviceOpen)
			{
				(*impl_->device)->USBDeviceClose(impl_->device);
				impl_->deviceOpen = false;
			}
			(*impl_->device)->Release(impl_->device);
			impl_->device = nullptr;
		}

		impl_->playPipe = 0;
		impl_->syncPipe = 0;
		impl_->capturePipe = 0;
		impl_->captureRunning = false;
		impl_->playMaxPacketSize = 0;
		impl_->syncMaxPacketSize = 0;
		impl_->captureMaxPacketSize = 0;
		impl_->nextFrameStart = 0;
		impl_->syncNextFrameStart = 0;
		impl_->playbackFrameAccumulator = 0.0;
		impl_->playbackServoCorrectionPpm = 0.0;
	}

	bool AkaiEieUsbTransport::StartSyncTransfers() noexcept
	{
		if (impl_->syncPipe == 0)
		{
			os_log(OS_LOG_DEFAULT, "AKAI EIE Pro USB: sync feedback endpoint not available");
			return true;
		}

		for (std::size_t i = 0; i < impl_->syncTransfers.size(); ++i)
		{
			if (!SubmitSyncTransfer(i))
				return false;
		}
		return true;
	}

	bool AkaiEieUsbTransport::StartPlaybackTransfers() noexcept
	{
		for (std::size_t i = 0; i < impl_->playbackTransfers.size(); ++i)
		{
			if (!SubmitPlaybackTransfer(i))
				return false;
		}
		return true;
	}

	bool AkaiEieUsbTransport::StartCaptureTransfers() noexcept
	{
		if (impl_->capturePipe == 0)
		{
			os_log(OS_LOG_DEFAULT,
				   "AKAI EIE Pro USB: capture bulk endpoint not available");
			return true;
		}

		for (std::size_t i = 0; i < impl_->captureTransfers.size(); ++i)
		{
			if (!SubmitCaptureTransfer(i))
				return false;
		}
		impl_->captureRunning = true;
		os_log(OS_LOG_DEFAULT,
			   "AKAI EIE Pro USB: capture bulk transfers started on demand");
		return true;
	}

	bool AkaiEieUsbTransport::SubmitSyncTransfer(
		std::size_t transferIndex) noexcept
	{
		if (impl_->syncInterface == nullptr || impl_->syncPipe == 0 || transferIndex >= impl_->syncTransfers.size() || stopRequested_.load())
		{
			return false;
		}

		auto &transfer = impl_->syncTransfers[transferIndex];
		std::memset(transfer.buffer.data(), 0, transfer.buffer.size());
		transfer.frame.frStatus = kIOReturnSuccess;
		transfer.frame.frReqCount = static_cast<UInt16>(
			std::min<std::size_t>(impl_->syncMaxPacketSize,
								  transfer.buffer.size()));
		transfer.frame.frActCount = 0;

		IOReturn lastResult = kIOReturnSuccess;
		std::uint64_t lastFrameStart = 0;
		std::uint64_t lastCurrentFrame = 0;
		bool lastHaveCurrentFrame = false;
		for (std::uint32_t attempt = 0; attempt < kIsoSubmitAttemptCount; ++attempt)
		{
			std::uint64_t currentFrame = 0;
			const bool haveCurrentFrame = ReadCurrentDataUsbFrame(&currentFrame);
			if (haveCurrentFrame && impl_->syncNextFrameStart < currentFrame + kUsbScheduleLeadBusFrames)
			{
				impl_->syncNextFrameStart =
					currentFrame + kUsbScheduleLeadBusFrames;
			}

			const std::uint64_t frameStart = impl_->syncNextFrameStart;
			++impl_->syncNextFrameStart;
			lastFrameStart = frameStart;
			lastCurrentFrame = currentFrame;
			lastHaveCurrentFrame = haveCurrentFrame;

			transfer.submitted = true;
			inFlightTransfers_.fetch_add(1);
			lastResult = (*impl_->syncInterface)->ReadIsochPipeAsync(impl_->syncInterface, impl_->syncPipe, transfer.buffer.data(), frameStart, 1, &transfer.frame, &AkaiEieUsbTransport::SyncComplete, &transfer);
			if (lastResult == kIOReturnSuccess)
				return true;

			transfer.submitted = false;
			inFlightTransfers_.fetch_sub(1);
			if (lastResult == kIOReturnIsoTooOld || lastResult == kIOReturnIsoTooNew)
			{
				if (ReadCurrentDataUsbFrame(&currentFrame))
					impl_->syncNextFrameStart =
						currentFrame + kUsbScheduleRecoveryLeadBusFrames;
				continue;
			}

			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: sync read submit failed %{public}s (%{public}#x) start=%{public}llu current=%{public}llu",
						 IOReturnName(lastResult), lastResult,
						 static_cast<unsigned long long>(lastFrameStart),
						 static_cast<unsigned long long>(
							 lastHaveCurrentFrame ? lastCurrentFrame : 0));
			return false;
		}

		os_log_error(OS_LOG_DEFAULT,
					 "AKAI EIE Pro USB: sync read submit failed %{public}s (%{public}#x) start=%{public}llu current=%{public}llu",
					 IOReturnName(lastResult), lastResult,
					 static_cast<unsigned long long>(lastFrameStart),
					 static_cast<unsigned long long>(
						 lastHaveCurrentFrame ? lastCurrentFrame : 0));
		return false;
	}

	bool AkaiEieUsbTransport::SubmitPlaybackTransfer(
		std::size_t transferIndex) noexcept
	{
		if (impl_->playInterface == nullptr || impl_->playPipe == 0 || transferIndex >= impl_->playbackTransfers.size() || stopRequested_.load())
		{
			return false;
		}

		auto &transfer = impl_->playbackTransfers[transferIndex];
		const std::size_t bufferedFrames = BufferedOutputFrames();
		const std::uint32_t framesWanted =
			CalculatePlaybackFramesWanted(bufferedFrames);

		const std::size_t copiedFrames =
			PopOutputFrames(transfer.buffer.data(), framesWanted);
		RecordPlaybackStats(framesWanted, bufferedFrames, copiedFrames);
		if (copiedFrames < framesWanted)
		{
			++impl_->underrunCount;
			if (impl_->underrunCount <= 5)
			{
				os_log(OS_LOG_DEFAULT,
					   "AKAI EIE Pro USB: playback underrun %zu/%u frames, padding silence",
					   copiedFrames, framesWanted);
			}
		}

		akai_eie::IsoPacket packetLayout[kPlaybackIsoPacketCount]{};
		if (!akai_eie::BuildIsoPacketLayout(framesWanted, kPlaybackIsoPacketCount,
											akai_eie::kPlaybackBytesPerFrame, packetLayout))
		{
			return false;
		}

		for (std::size_t i = 0; i < kPlaybackIsoPacketCount; ++i)
		{
			if (packetLayout[i].length_bytes > impl_->playMaxPacketSize)
				return false;
			transfer.frames[i].frStatus = kIOReturnSuccess;
			transfer.frames[i].frReqCount =
				static_cast<UInt16>(packetLayout[i].length_bytes);
			transfer.frames[i].frActCount = 0;
		}

		const std::uint64_t transferDurationBusFrames =
			PlaybackTransferDurationBusFrames(impl_->frameListMicros);
		for (std::uint32_t attempt = 0; attempt < kIsoSubmitAttemptCount; ++attempt)
		{
			std::uint64_t currentFrame = 0;
			const bool haveCurrentFrame = ReadCurrentUsbFrame(&currentFrame);
			if (haveCurrentFrame &&
				impl_->nextFrameStart <
					currentFrame + kUsbScheduleLeadBusFrames)
			{
				const std::uint64_t lateBusFrames =
					(currentFrame + kUsbScheduleLeadBusFrames) -
					impl_->nextFrameStart;
				++impl_->playbackStatsLateScheduleCount;
				impl_->playbackStatsMaxLateBusFrames =
					std::max(impl_->playbackStatsMaxLateBusFrames,
							 lateBusFrames);
				impl_->nextFrameStart = currentFrame + kUsbScheduleLeadBusFrames;
			}

			const std::uint64_t oldNextFrameStart = impl_->nextFrameStart;
			impl_->nextFrameStart += transferDurationBusFrames;

			transfer.submitted = true;
			inFlightTransfers_.fetch_add(1);
			const IOReturn result = (*impl_->playInterface)->WriteIsochPipeAsync(impl_->playInterface, impl_->playPipe, transfer.buffer.data(), oldNextFrameStart, kPlaybackIsoPacketCount, transfer.frames.data(), &AkaiEieUsbTransport::PlaybackComplete, &transfer);
			if (result == kIOReturnSuccess)
				return true;

			transfer.submitted = false;
			inFlightTransfers_.fetch_sub(1);
			impl_->nextFrameStart = oldNextFrameStart;
			if (result == kIOReturnIsoTooOld || result == kIOReturnIsoTooNew)
			{
				++impl_->playbackStatsSubmitRetryCount;
				if (ReadCurrentUsbFrame(&currentFrame))
					impl_->nextFrameStart =
						currentFrame + kUsbScheduleRecoveryLeadBusFrames;
				continue;
			}

			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: isoch write submit failed %{public}s (%{public}#x) start=%{public}llu current=%{public}llu duration=%{public}llu frameList=%{public}u",
						 IOReturnName(result), result,
						 static_cast<unsigned long long>(oldNextFrameStart),
						 static_cast<unsigned long long>(haveCurrentFrame ? currentFrame : 0),
						 static_cast<unsigned long long>(transferDurationBusFrames),
						 impl_->frameListMicros);
			return false;
		}

		return false;
	}

	bool AkaiEieUsbTransport::SubmitCaptureTransfer(
		std::size_t transferIndex) noexcept
	{
		if (impl_->dataInterface == nullptr || impl_->capturePipe == 0 ||
			transferIndex >= impl_->captureTransfers.size() ||
			stopRequested_.load())
		{
			return false;
		}

		auto &transfer = impl_->captureTransfers[transferIndex];
		std::memset(transfer.buffer.data(), 0, transfer.buffer.size());
		transfer.requestedBytes =
			static_cast<UInt32>(transfer.buffer.size());

		transfer.submitted = true;
		inFlightTransfers_.fetch_add(1);
		const IOReturn result = (*impl_->dataInterface)
									->ReadPipeAsync(impl_->dataInterface,
													impl_->capturePipe,
													transfer.buffer.data(),
													transfer.requestedBytes,
													&AkaiEieUsbTransport::CaptureComplete,
													&transfer);
		if (result == kIOReturnSuccess)
			return true;

		transfer.submitted = false;
		inFlightTransfers_.fetch_sub(1);
		os_log_error(OS_LOG_DEFAULT,
					 "AKAI EIE Pro USB: capture read submit failed %{public}s (%{public}#x)",
					 IOReturnName(result), result);
		return false;
	}

	void AkaiEieUsbTransport::RequestRestart(const char *reason) noexcept
	{
		if (stopRequested_.load())
			return;

		const bool wasRequested = restartRequested_.exchange(true);
		if (!wasRequested)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: requesting transport restart: %{public}s",
						 reason == nullptr ? "unspecified" : reason);
		}

		CFRunLoopRef runLoop = nullptr;
		{
			std::lock_guard<std::mutex> lock(impl_->runLoopMutex);
			runLoop = impl_->workerRunLoop;
			if (runLoop != nullptr)
				CFRetain(runLoop);
		}
		if (runLoop != nullptr)
		{
			CFRunLoopStop(runLoop);
			CFRelease(runLoop);
		}
	}

	void AkaiEieUsbTransport::HandleSyncComplete(std::size_t transferIndex,
												 IOReturn result) noexcept
	{
		if (transferIndex >= impl_->syncTransfers.size())
			return;

		auto &transfer = impl_->syncTransfers[transferIndex];
		if (transfer.submitted)
		{
			transfer.submitted = false;
			inFlightTransfers_.fetch_sub(1);
		}
		else if (!stopRequested_.load())
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: unexpected sync completion %{public}zu",
						 transferIndex);
			return;
		}
		else
		{
			return;
		}

		bool acceptedSyncFeedback = false;
		if (result == kIOReturnSuccess && transfer.frame.frActCount > 0)
		{
			const std::uint32_t framesElapsed = DecodeSyncFeedbackFrames(
				SampleRateHz(), transfer.buffer.data(),
				transfer.frame.frActCount);
			if (framesElapsed > 0)
			{
				impl_->consecutiveEmptySyncPacketCount = 0;
				impl_->syncFeedbackFallbackLogged = false;
				acceptedSyncFeedback = true;
			}
		}

		const bool emptySyncPacket =
			result == kIOReturnSuccess && transfer.frame.frStatus == kIOReturnUnderrun && transfer.frame.frActCount == 0;
		if (!acceptedSyncFeedback && emptySyncPacket)
		{
			++impl_->consecutiveEmptySyncPacketCount;
			if (!impl_->syncFeedbackEmptyLogged)
			{
				impl_->syncFeedbackEmptyLogged = true;
				os_log(OS_LOG_DEFAULT,
					   "AKAI EIE Pro USB: sync feedback endpoint returned empty packets; using output buffer servo");
			}
			if (!impl_->syncFeedbackFallbackLogged && impl_->consecutiveEmptySyncPacketCount >= kMaxConsecutiveEmptySyncPackets)
			{
				impl_->syncFeedbackFallbackLogged = true;
				os_log(OS_LOG_DEFAULT,
					   "AKAI EIE Pro USB: sync feedback still empty after %{public}u packets; continuing playback servo from output buffer fill",
					   impl_->consecutiveEmptySyncPacketCount);
			}
		}

		if (!acceptedSyncFeedback && !emptySyncPacket && (result != kIOReturnSuccess || transfer.frame.frStatus != kIOReturnSuccess) && !stopRequested_.load() && !restartRequested_.load())
		{
			++impl_->syncErrorCount;
			if (impl_->syncErrorCount <= 5)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: sync read completion failed %{public}s (%{public}#x) frameStatus=%{public}s (%{public}#x) act=%{public}u",
							 IOReturnName(result), result,
							 IOReturnName(transfer.frame.frStatus),
							 transfer.frame.frStatus,
							 transfer.frame.frActCount);
			}
		}

		if (!stopRequested_.load() && !restartRequested_.load() && !SubmitSyncTransfer(transferIndex))
		{
			RequestRestart("sync transfer resubmit failed");
		}
	}

	void AkaiEieUsbTransport::HandlePlaybackComplete(std::size_t transferIndex,
													 IOReturn result) noexcept
	{
		if (transferIndex >= impl_->playbackTransfers.size())
			return;

		auto &transfer = impl_->playbackTransfers[transferIndex];
		if (transfer.submitted)
		{
			transfer.submitted = false;
			inFlightTransfers_.fetch_sub(1);
		}
		else if (!stopRequested_.load())
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: unexpected playback completion %{public}zu",
						 transferIndex);
			return;
		}
		else
		{
			return;
		}

		bool completionError = result != kIOReturnSuccess;
		IOReturn firstFrameStatus = kIOReturnSuccess;
		std::uint32_t badIsoFrameCount = 0;
		if (result == kIOReturnSuccess)
		{
			for (const auto &frame : transfer.frames)
			{
				if (frame.frStatus != kIOReturnSuccess)
				{
					completionError = true;
					if (badIsoFrameCount == 0)
						firstFrameStatus = frame.frStatus;
					++badIsoFrameCount;
				}
			}
		}

		if (completionError && !stopRequested_.load() && !restartRequested_.load())
		{
			++impl_->playbackStatsCompletionErrorCount;
			impl_->playbackStatsBadIsoFrameCount += badIsoFrameCount;
			++impl_->playbackCompletionErrorCount;
			++impl_->consecutivePlaybackErrorCount;
			if (impl_->playbackCompletionErrorCount <= 8)
			{
				if (result != kIOReturnSuccess)
				{
					os_log_error(OS_LOG_DEFAULT,
								 "AKAI EIE Pro USB: isoch write completion failed %{public}s (%{public}#x)",
								 IOReturnName(result), result);
				}
				else
				{
					os_log_error(OS_LOG_DEFAULT,
								 "AKAI EIE Pro USB: isoch frame completion failed %{public}s (%{public}#x)",
								 IOReturnName(firstFrameStatus), firstFrameStatus);
				}
			}
			if (impl_->consecutivePlaybackErrorCount >= kMaxConsecutivePlaybackErrors)
			{
				RequestRestart("repeated playback completion errors");
				return;
			}
		}
		else if (!completionError)
		{
			impl_->consecutivePlaybackErrorCount = 0;
		}

		if (!stopRequested_.load() && !restartRequested_.load() && !SubmitPlaybackTransfer(transferIndex))
		{
			RequestRestart("playback transfer resubmit failed");
		}
	}

	void AkaiEieUsbTransport::HandleCaptureComplete(
		std::size_t transferIndex, IOReturn result,
		std::uint32_t actualBytes) noexcept
	{
		if (transferIndex >= impl_->captureTransfers.size())
			return;

		auto &transfer = impl_->captureTransfers[transferIndex];
		if (transfer.submitted)
		{
			transfer.submitted = false;
			inFlightTransfers_.fetch_sub(1);
		}
		else if (!stopRequested_.load())
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: unexpected capture completion %{public}zu",
						 transferIndex);
			return;
		}
		else
		{
			return;
		}

		if (result == kIOReturnSuccess)
		{
			++impl_->playbackStatsCaptureCompletionCount;
			impl_->playbackStatsCaptureByteCount += actualBytes;
			if (actualBytes > 0)
			{
				PushCaptureUsbFrames(transfer.buffer.data(),
									 std::min<std::size_t>(
										 actualBytes, transfer.requestedBytes));
			}
			impl_->consecutiveCaptureErrorCount = 0;
		}
		else if (!stopRequested_.load() && !restartRequested_.load())
		{
			++impl_->captureCompletionErrorCount;
			++impl_->consecutiveCaptureErrorCount;
			if (impl_->captureCompletionErrorCount <= 8)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: capture read completion failed %{public}s (%{public}#x) act=%{public}u",
							 IOReturnName(result), result, actualBytes);
			}
			if (impl_->consecutiveCaptureErrorCount >=
				kMaxConsecutiveCaptureErrors)
			{
				RequestRestart("repeated capture completion errors");
				return;
			}
		}

		if (!stopRequested_.load() && !restartRequested_.load() &&
			!SubmitCaptureTransfer(transferIndex))
		{
			RequestRestart("capture transfer resubmit failed");
		}
	}

	bool AkaiEieUsbTransport::ReadCurrentUsbFrame(
		std::uint64_t *outFrame) noexcept
	{
		if (impl_->playInterface == nullptr || outFrame == nullptr)
			return false;

		AbsoluteTime atTime{};
		// WriteIsochPipeAsync takes a bus frame start, even for 125us frame lists.
		const IOReturn result = (*impl_->playInterface)->GetBusFrameNumber(impl_->playInterface, outFrame, &atTime);
		return result == kIOReturnSuccess;
	}

	bool AkaiEieUsbTransport::ReadCurrentDataUsbFrame(
		std::uint64_t *outFrame) noexcept
	{
		if (impl_->syncInterface == nullptr || outFrame == nullptr)
			return false;

		AbsoluteTime atTime{};
		const IOReturn result = (*impl_->syncInterface)->GetBusFrameNumber(impl_->syncInterface, outFrame, &atTime);
		return result == kIOReturnSuccess;
	}

	std::uint32_t AkaiEieUsbTransport::SampleRateHz() const noexcept
	{
		if (impl_ == nullptr)
			return kDefaultSampleRateHz;
		return impl_->sampleRateHz.load(std::memory_order_acquire);
	}

	void AkaiEieUsbTransport::PushCaptureUsbFrames(
		const std::uint8_t *usbFrames, std::size_t byteCount) noexcept
	{
		if (impl_ == nullptr || usbFrames == nullptr)
			return;

		const std::size_t frameCount =
			byteCount / akai_eie::kCaptureBytesPerUsbFrame;
		if (frameCount == 0)
			return;

		std::lock_guard<std::mutex> lock(impl_->inputMutex);
		for (std::size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
		{
			const akai_eie::CaptureSampleFrame decoded =
				akai_eie::DecodeCaptureUsbFrame(
					usbFrames +
					frameIndex * akai_eie::kCaptureBytesPerUsbFrame);

			if (impl_->inputFrameCount == kInputRingFrameCount)
			{
				impl_->inputReadFrame =
					(impl_->inputReadFrame + 1) % kInputRingFrameCount;
				--impl_->inputFrameCount;
				++impl_->inputOverrunCount;
			}

			Float32 *out =
				impl_->inputRing.data() +
				impl_->inputWriteFrame * akai_eie::kPlaybackChannelCount;
			out[0] = S24ToFloat32(decoded.ch1);
			out[1] = S24ToFloat32(decoded.ch2);
			out[2] = S24ToFloat32(decoded.ch3);
			out[3] = S24ToFloat32(decoded.ch4);

			impl_->inputWriteFrame =
				(impl_->inputWriteFrame + 1) % kInputRingFrameCount;
			++impl_->inputFrameCount;
		}
	}

	void AkaiEieUsbTransport::RememberLastPlaybackFrame(
		const std::uint8_t *frames, std::size_t frameCount) noexcept
	{
		if (frames == nullptr || frameCount == 0)
			return;

		const std::uint8_t *lastFrame =
			frames + (frameCount - 1) * akai_eie::kPlaybackBytesPerFrame;
		for (std::size_t channel = 0; channel < akai_eie::kPlaybackChannelCount; ++channel)
		{
			impl_->lastPlaybackSamples[channel] =
				ReadS24LE(lastFrame + channel * 3);
		}
	}

	void AkaiEieUsbTransport::FadeInFromSilence(
		std::uint8_t *frames, std::size_t frameCount) noexcept
	{
		if (frames == nullptr || frameCount == 0)
			return;

		const std::size_t rampFrames =
			std::min(frameCount, kSilenceRampFrameCount);
		for (std::size_t frame = 0; frame < rampFrames; ++frame)
		{
			const std::size_t numerator = frame + 1;
			for (std::size_t channel = 0; channel < akai_eie::kPlaybackChannelCount; ++channel)
			{
				std::uint8_t *sample =
					frames + frame * akai_eie::kPlaybackBytesPerFrame + channel * 3;
				WriteS24LE(ScaleS24(ReadS24LE(sample), numerator, rampFrames),
						   sample);
			}
		}
	}

	void AkaiEieUsbTransport::FadeOutToSilence(
		std::uint8_t *frames, std::size_t frameCount) noexcept
	{
		if (frames == nullptr || frameCount == 0)
			return;

		const std::size_t rampFrames =
			std::min(frameCount, kSilenceRampFrameCount);
		for (std::size_t frame = 0; frame < rampFrames; ++frame)
		{
			const std::size_t numerator = rampFrames - frame - 1;
			for (std::size_t channel = 0; channel < akai_eie::kPlaybackChannelCount; ++channel)
			{
				std::uint8_t *sample =
					frames + frame * akai_eie::kPlaybackBytesPerFrame + channel * 3;
				WriteS24LE(ScaleS24(impl_->lastPlaybackSamples[channel],
									numerator, rampFrames),
						   sample);
			}
		}

		if (rampFrames < frameCount)
		{
			std::memset(frames + rampFrames * akai_eie::kPlaybackBytesPerFrame,
						0,
						(frameCount - rampFrames) * akai_eie::kPlaybackBytesPerFrame);
		}
		impl_->lastPlaybackSamples.fill(0);
	}

	std::size_t AkaiEieUsbTransport::BufferedOutputFrames() noexcept
	{
		std::lock_guard<std::mutex> lock(impl_->outputMutex);
		return impl_->outputFrameCount;
	}

	std::uint32_t AkaiEieUsbTransport::CalculatePlaybackFramesWanted(
		std::size_t bufferedOutputFrames) noexcept
	{
		const std::uint32_t sampleRateHz = SampleRateHz();
		if (!impl_->playbackServoFilterPrimed)
		{
			impl_->playbackServoFilteredFrames =
				static_cast<double>(OutputServoTargetFrameCount(sampleRateHz));
			impl_->playbackServoFilterPrimed = true;
		}

		const double filterAlpha = std::clamp(
			PlaybackTransferSeconds(impl_->frameListMicros) /
				kPlaybackServoFilterSeconds,
			0.0, 1.0);
		impl_->playbackServoFilteredFrames +=
			(static_cast<double>(bufferedOutputFrames) -
			 impl_->playbackServoFilteredFrames) *
			filterAlpha;
		impl_->playbackServoCorrectionPpm = PlaybackServoCorrectionPpm(
			sampleRateHz, impl_->playbackServoFilteredFrames);

		const double effectiveSampleRate =
			static_cast<double>(sampleRateHz) *
			(1.0 + impl_->playbackServoCorrectionPpm / 1000000.0);
		impl_->playbackFrameAccumulator +=
			effectiveSampleRate *
			PlaybackTransferSeconds(impl_->frameListMicros);

		std::uint32_t framesWanted = static_cast<std::uint32_t>(
			impl_->playbackFrameAccumulator);
		if (framesWanted < 1)
		{
			framesWanted = 1;
			impl_->playbackFrameAccumulator = 0.0;
		}
		else if (framesWanted > kMaxPlaybackFramesPerTransfer)
		{
			framesWanted = kMaxPlaybackFramesPerTransfer;
			impl_->playbackFrameAccumulator = 0.0;
		}
		else
		{
			impl_->playbackFrameAccumulator -=
				static_cast<double>(framesWanted);
		}

		return framesWanted;
	}

	void AkaiEieUsbTransport::RecordPlaybackStats(
		std::uint32_t framesWanted, std::size_t bufferedOutputFrames,
		std::size_t copiedFrames) noexcept
	{
		const auto now = std::chrono::steady_clock::now();
		if (impl_->playbackStatsTransferCount == 0)
		{
			impl_->playbackStatsStarted = now;
			impl_->playbackStatsMinFrames = framesWanted;
			impl_->playbackStatsMaxFrames = framesWanted;
		}

		++impl_->playbackStatsTransferCount;
		impl_->playbackStatsFrameSum += framesWanted;
		impl_->playbackStatsMinFrames =
			std::min(impl_->playbackStatsMinFrames, framesWanted);
		impl_->playbackStatsMaxFrames =
			std::max(impl_->playbackStatsMaxFrames, framesWanted);
		impl_->playbackStatsLastBufferedFrames = bufferedOutputFrames;
		impl_->playbackStatsLastFilteredFrames =
			impl_->playbackServoFilteredFrames;
		if (copiedFrames < framesWanted)
		{
			impl_->playbackStatsUnderrunFrameCount +=
				framesWanted - copiedFrames;
		}

		const auto elapsed = now - impl_->playbackStatsStarted;
		if (elapsed < std::chrono::seconds(1))
			return;

		const double averageFrames =
			static_cast<double>(impl_->playbackStatsFrameSum) /
			static_cast<double>(impl_->playbackStatsTransferCount);
		const std::uint32_t sampleRateHz = SampleRateHz();
		os_log(OS_LOG_DEFAULT,
			   "AKAI EIE Pro USB: playback servo rate=%{public}u frameList=%{public}u transfers=%{public}llu buffered=%{public}zu filtered=%{public}.1f target=%{public}zu correction=%{public}.1fppm frames=%{public}u/%{public}.1f/%{public}u underrunFrames=%{public}llu totalUnderruns=%{public}llu dropped=%{public}llu rebuffer=%{public}llu late=%{public}llu maxLate=%{public}llu retry=%{public}llu completionErr=%{public}llu badIsoFrames=%{public}llu capture=%{public}llu/%{public}llu inputOverruns=%{public}llu inFlight=%{public}u",
			   sampleRateHz, impl_->frameListMicros,
			   static_cast<unsigned long long>(
				   impl_->playbackStatsTransferCount),
			   impl_->playbackStatsLastBufferedFrames,
			   impl_->playbackStatsLastFilteredFrames,
			   OutputServoTargetFrameCount(sampleRateHz),
			   impl_->playbackServoCorrectionPpm,
			   impl_->playbackStatsMinFrames, averageFrames,
			   impl_->playbackStatsMaxFrames,
			   static_cast<unsigned long long>(
				   impl_->playbackStatsUnderrunFrameCount),
			   static_cast<unsigned long long>(impl_->underrunCount),
			   static_cast<unsigned long long>(impl_->droppedFrameCount),
			   static_cast<unsigned long long>(impl_->rebufferCount),
			   static_cast<unsigned long long>(
				   impl_->playbackStatsLateScheduleCount),
			   static_cast<unsigned long long>(
				   impl_->playbackStatsMaxLateBusFrames),
			   static_cast<unsigned long long>(
				   impl_->playbackStatsSubmitRetryCount),
			   static_cast<unsigned long long>(
				   impl_->playbackStatsCompletionErrorCount),
			   static_cast<unsigned long long>(
				   impl_->playbackStatsBadIsoFrameCount),
			   static_cast<unsigned long long>(
				   impl_->playbackStatsCaptureCompletionCount),
			   static_cast<unsigned long long>(
				   impl_->playbackStatsCaptureByteCount),
			   static_cast<unsigned long long>(impl_->inputOverrunCount),
			   inFlightTransfers_.load());

		impl_->playbackStatsStarted = now;
		impl_->playbackStatsTransferCount = 0;
		impl_->playbackStatsFrameSum = 0;
		impl_->playbackStatsUnderrunFrameCount = 0;
		impl_->playbackStatsLateScheduleCount = 0;
		impl_->playbackStatsMaxLateBusFrames = 0;
		impl_->playbackStatsSubmitRetryCount = 0;
		impl_->playbackStatsCompletionErrorCount = 0;
		impl_->playbackStatsBadIsoFrameCount = 0;
		impl_->playbackStatsCaptureCompletionCount = 0;
		impl_->playbackStatsCaptureByteCount = 0;
		impl_->playbackStatsMinFrames = 0;
		impl_->playbackStatsMaxFrames = 0;
		impl_->playbackStatsLastBufferedFrames = 0;
		impl_->playbackStatsLastFilteredFrames = 0.0;
	}

	std::size_t AkaiEieUsbTransport::PopOutputFrames(std::uint8_t *outFrames,
													 std::size_t frameCount) noexcept
	{
		if (outFrames == nullptr || frameCount == 0)
			return 0;

		std::size_t copied = 0;
		{
			std::lock_guard<std::mutex> lock(impl_->outputMutex);

			if (!impl_->outputPrimed &&
				impl_->outputFrameCount >=
					OutputReprimeFrameCount(SampleRateHz()))
			{
				impl_->outputPrimed = true;
				++impl_->rebufferCount;
				if (impl_->rebufferCount <= 5)
				{
					os_log(OS_LOG_DEFAULT,
						   "AKAI EIE Pro USB: playback re-primed %{public}zu frames",
						   impl_->outputFrameCount);
				}
			}

			while (impl_->outputPrimed && copied < frameCount && impl_->outputFrameCount > 0)
			{
				const std::size_t contiguousAvailable =
					std::min(impl_->outputFrameCount,
							 kOutputRingFrameCount - impl_->outputReadFrame);
				const std::size_t contiguousNeeded = frameCount - copied;
				const std::size_t framesToCopy =
					std::min(contiguousAvailable, contiguousNeeded);
				const std::size_t bytesToCopy =
					framesToCopy * akai_eie::kPlaybackBytesPerFrame;

				std::memcpy(
					outFrames + copied * akai_eie::kPlaybackBytesPerFrame,
					impl_->outputRing.data() + impl_->outputReadFrame * akai_eie::kPlaybackBytesPerFrame,
					bytesToCopy);
				impl_->outputReadFrame =
					(impl_->outputReadFrame + framesToCopy) % kOutputRingFrameCount;
				impl_->outputFrameCount -= framesToCopy;
				copied += framesToCopy;
			}

			if (impl_->outputPrimed && copied == 0 && impl_->outputFrameCount == 0)
			{
				impl_->outputPrimed = false;
			}
		}

		if (copied > 0)
		{
			if (impl_->outputWasSilent)
				FadeInFromSilence(outFrames, copied);
			RememberLastPlaybackFrame(outFrames, copied);
			impl_->outputWasSilent = false;
		}

		if (copied < frameCount)
		{
			FadeOutToSilence(
				outFrames + copied * akai_eie::kPlaybackBytesPerFrame,
				frameCount - copied);
			impl_->outputWasSilent = true;
		}

		return copied;
	}

	void AkaiEieUsbTransport::ResetOutputRing() noexcept
	{
		if (impl_ == nullptr)
			return;

		std::lock_guard<std::mutex> lock(impl_->outputMutex);
		impl_->outputReadFrame = 0;
		impl_->outputWriteFrame = 0;
		impl_->outputFrameCount = 0;
		impl_->outputPrimed = false;
		impl_->underrunCount = 0;
		impl_->rebufferCount = 0;
		impl_->droppedFrameCount = 0;
		impl_->syncErrorCount = 0;
		impl_->playbackCompletionErrorCount = 0;
		impl_->consecutivePlaybackErrorCount = 0;
		impl_->consecutiveEmptySyncPacketCount = 0;
		impl_->syncFeedbackFallbackLogged = false;
		impl_->syncFeedbackEmptyLogged = false;
		impl_->lastPlaybackSamples.fill(0);
		impl_->outputWasSilent = true;
		impl_->playbackFrameAccumulator = 0.0;
		impl_->playbackServoCorrectionPpm = 0.0;
		impl_->playbackServoFilteredFrames = 0.0;
		impl_->playbackServoFilterPrimed = false;
		impl_->playbackStatsStarted = {};
		impl_->playbackStatsTransferCount = 0;
		impl_->playbackStatsFrameSum = 0;
		impl_->playbackStatsUnderrunFrameCount = 0;
		impl_->playbackStatsLateScheduleCount = 0;
		impl_->playbackStatsMaxLateBusFrames = 0;
		impl_->playbackStatsSubmitRetryCount = 0;
		impl_->playbackStatsCompletionErrorCount = 0;
		impl_->playbackStatsBadIsoFrameCount = 0;
		impl_->playbackStatsCaptureCompletionCount = 0;
		impl_->playbackStatsCaptureByteCount = 0;
		impl_->playbackStatsMinFrames = 0;
		impl_->playbackStatsMaxFrames = 0;
		impl_->playbackStatsLastBufferedFrames = 0;
		impl_->playbackStatsLastFilteredFrames = 0.0;
	}

	void AkaiEieUsbTransport::ResetInputRing() noexcept
	{
		if (impl_ == nullptr)
			return;

		std::lock_guard<std::mutex> lock(impl_->inputMutex);
		impl_->inputReadFrame = 0;
		impl_->inputWriteFrame = 0;
		impl_->inputFrameCount = 0;
		impl_->inputOverrunCount = 0;
		impl_->captureCompletionErrorCount = 0;
		impl_->consecutiveCaptureErrorCount = 0;
	}

	void AkaiEieUsbTransport::SyncComplete(void *refcon, IOReturn result,
										   void *arg0) noexcept
	{
		(void)arg0;
		auto *transfer = static_cast<Impl::SyncTransfer *>(refcon);
		if (transfer == nullptr || transfer->owner == nullptr)
			return;
		transfer->owner->HandleSyncComplete(transfer->index, result);
	}

	void AkaiEieUsbTransport::PlaybackComplete(void *refcon, IOReturn result,
											   void *arg0) noexcept
	{
		(void)arg0;
		auto *transfer = static_cast<Impl::PlaybackTransfer *>(refcon);
		if (transfer == nullptr || transfer->owner == nullptr)
			return;
		transfer->owner->HandlePlaybackComplete(transfer->index, result);
	}

	void AkaiEieUsbTransport::CaptureComplete(void *refcon, IOReturn result,
											  void *arg0) noexcept
	{
		auto *transfer = static_cast<Impl::CaptureTransfer *>(refcon);
		if (transfer == nullptr || transfer->owner == nullptr)
			return;
		const std::uint32_t actualBytes = static_cast<std::uint32_t>(
			reinterpret_cast<std::uintptr_t>(arg0));
		transfer->owner->HandleCaptureComplete(transfer->index, result,
											   actualBytes);
	}

} // namespace akai_eie_hal
