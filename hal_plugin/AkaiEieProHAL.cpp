#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <os/log.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "AkaiEieUsbTransport.hpp"
#include "../protocol/akai_eie_protocol.hpp"

namespace {

constexpr AudioObjectID kDeviceObjectId = 2;
constexpr AudioObjectID kOutputStreamObjectId = 3;
constexpr AudioObjectID kInputStreamObjectId = 4;

constexpr Float64 kSampleRate = 48000.0;
constexpr UInt32 kChannelCount = 4;
constexpr UInt32 kBytesPerSample = sizeof(Float32);
constexpr UInt32 kBufferFrameSize = 512;
constexpr UInt32 kClockSeed = 1;

constexpr const char *kBundleId = "com.interslice.akai-eie-pro";
constexpr const char *kDeviceName = "AKAI EIE Pro";
constexpr const char *kManufacturerName = "Akai Professional";
constexpr const char *kModelUid = "com.interslice.akai-eie-pro.model";
constexpr const char *kDeviceUid =
	"com.interslice.akai-eie-pro.usb-09e8-0010";
constexpr const char *kOutputStreamName = "AKAI EIE Pro Output";
constexpr const char *kInputStreamName = "AKAI EIE Pro Input";

std::atomic<UInt32> gRefCount {1};
std::atomic<UInt32> gRunningClientCount {0};
AudioServerPlugInHostRef gHost = nullptr;
std::atomic<UInt64> gStartHostTime {0};
akai_eie_hal::AkaiEieUsbTransport gUsbTransport;

extern AudioServerPlugInDriverInterface *gDriverInterfacePtr;

const mach_timebase_info_data_t &TimebaseInfo() noexcept
{
	static const mach_timebase_info_data_t info = []() noexcept {
		mach_timebase_info_data_t timebase {};
		if (mach_timebase_info(&timebase) != KERN_SUCCESS
			|| timebase.numer == 0 || timebase.denom == 0) {
			timebase.numer = 1;
			timebase.denom = 1;
		}
		return timebase;
	}();
	return info;
}

bool UUIDEquals(REFIID lhs, CFUUIDRef rhs) noexcept
{
	const CFUUIDBytes rhsBytes = CFUUIDGetUUIDBytes(rhs);
	return std::memcmp(&lhs, &rhsBytes, sizeof(CFUUIDBytes)) == 0;
}

bool IsMainElement(const AudioObjectPropertyAddress *address) noexcept
{
	return address->mElement == kAudioObjectPropertyElementMain
		|| address->mElement == kAudioObjectPropertyElementWildcard;
}

bool IsGlobalScope(const AudioObjectPropertyAddress *address) noexcept
{
	return address->mScope == kAudioObjectPropertyScopeGlobal
		|| address->mScope == kAudioObjectPropertyScopeWildcard;
}

bool IsInputScope(const AudioObjectPropertyAddress *address) noexcept
{
	return address->mScope == kAudioObjectPropertyScopeInput
		|| address->mScope == kAudioObjectPropertyScopeWildcard;
}

bool IsOutputScope(const AudioObjectPropertyAddress *address) noexcept
{
	return address->mScope == kAudioObjectPropertyScopeOutput
		|| address->mScope == kAudioObjectPropertyScopeWildcard;
}

bool IncludesInputScope(const AudioObjectPropertyAddress *address) noexcept
{
	return address->mScope == kAudioObjectPropertyScopeInput
		|| address->mScope == kAudioObjectPropertyScopeGlobal
		|| address->mScope == kAudioObjectPropertyScopeWildcard;
}

bool IncludesOutputScope(const AudioObjectPropertyAddress *address) noexcept
{
	return address->mScope == kAudioObjectPropertyScopeOutput
		|| address->mScope == kAudioObjectPropertyScopeGlobal
		|| address->mScope == kAudioObjectPropertyScopeWildcard;
}

bool IsKnownObject(AudioObjectID objectId) noexcept
{
	return objectId == kAudioObjectPlugInObject
		|| objectId == kDeviceObjectId
		|| objectId == kOutputStreamObjectId
		|| objectId == kInputStreamObjectId;
}

AudioClassID ClassForObject(AudioObjectID objectId) noexcept
{
	switch (objectId) {
	case kAudioObjectPlugInObject:
		return kAudioPlugInClassID;
	case kDeviceObjectId:
		return kAudioDeviceClassID;
	case kOutputStreamObjectId:
	case kInputStreamObjectId:
		return kAudioStreamClassID;
	default:
		return kAudioObjectClassID;
	}
}

AudioObjectID OwnerForObject(AudioObjectID objectId) noexcept
{
	switch (objectId) {
	case kAudioObjectPlugInObject:
		return kAudioObjectUnknown;
	case kDeviceObjectId:
		return kAudioObjectPlugInObject;
	case kOutputStreamObjectId:
	case kInputStreamObjectId:
		return kDeviceObjectId;
	default:
		return kAudioObjectUnknown;
	}
}

const char *NameForObject(AudioObjectID objectId) noexcept
{
	switch (objectId) {
	case kAudioObjectPlugInObject:
	case kDeviceObjectId:
		return kDeviceName;
	case kOutputStreamObjectId:
		return kOutputStreamName;
	case kInputStreamObjectId:
		return kInputStreamName;
	default:
		return "";
	}
}

template <typename T>
OSStatus CopyScalar(const T &value, UInt32 inDataSize, UInt32 *outDataSize,
	void *outData) noexcept
{
	if (outDataSize == nullptr || outData == nullptr)
		return kAudioHardwareBadPropertySizeError;
	if (inDataSize < sizeof(T))
		return kAudioHardwareBadPropertySizeError;

	*static_cast<T *>(outData) = value;
	*outDataSize = sizeof(T);
	return kAudioHardwareNoError;
}

OSStatus CopyCFString(const char *value, UInt32 inDataSize,
	UInt32 *outDataSize, void *outData) noexcept
{
	if (outDataSize == nullptr || outData == nullptr
		|| inDataSize < sizeof(CFStringRef)) {
		return kAudioHardwareBadPropertySizeError;
	}

	CFStringRef string = CFStringCreateWithCString(
		kCFAllocatorDefault, value, kCFStringEncodingUTF8);
	if (string == nullptr)
		return kAudioHardwareUnspecifiedError;

	// CoreAudio string properties use the Create/Copy ownership rule; the HAL
	// receives this retained CFStringRef and releases it after marshaling.
	*static_cast<CFStringRef *>(outData) = string;
	*outDataSize = sizeof(CFStringRef);
	return kAudioHardwareNoError;
}

bool ClassMatchesQualifier(AudioClassID objectClass, UInt32 qualifierDataSize,
	const void *qualifierData) noexcept
{
	if (qualifierDataSize == 0 || qualifierData == nullptr)
		return true;

	const auto *classes = static_cast<const AudioClassID *>(qualifierData);
	const UInt32 classCount =
		qualifierDataSize / static_cast<UInt32>(sizeof(AudioClassID));
	for (UInt32 i = 0; i < classCount; ++i) {
		if (classes[i] == objectClass || classes[i] == kAudioObjectClassID
			|| classes[i] == kAudioObjectClassIDWildcard) {
			return true;
		}
	}

	return false;
}

UInt32 DeviceStreamList(const AudioObjectPropertyAddress *address,
	AudioObjectID *outObjects, UInt32 maxObjects) noexcept
{
	UInt32 count = 0;
	if (IncludesOutputScope(address)) {
		if (outObjects != nullptr && count < maxObjects)
			outObjects[count] = kOutputStreamObjectId;
		++count;
	}
	if (IncludesInputScope(address)) {
		if (outObjects != nullptr && count < maxObjects)
			outObjects[count] = kInputStreamObjectId;
		++count;
	}
	return count;
}

UInt32 OwnedObjectList(AudioObjectID objectId,
	const AudioObjectPropertyAddress *address, UInt32 qualifierDataSize,
	const void *qualifierData, AudioObjectID *outObjects,
	UInt32 maxObjects) noexcept
{
	UInt32 count = 0;
	auto addIfMatching = [&](AudioObjectID ownedObject) {
		if (!ClassMatchesQualifier(ClassForObject(ownedObject),
				qualifierDataSize, qualifierData)) {
			return;
		}
		if (outObjects != nullptr && count < maxObjects)
			outObjects[count] = ownedObject;
		++count;
	};

	if (objectId == kAudioObjectPlugInObject) {
		addIfMatching(kDeviceObjectId);
	} else if (objectId == kDeviceObjectId) {
		if (IncludesOutputScope(address))
			addIfMatching(kOutputStreamObjectId);
		if (IncludesInputScope(address))
			addIfMatching(kInputStreamObjectId);
	}

	return count;
}

AudioStreamBasicDescription StreamFormat() noexcept
{
	AudioStreamBasicDescription format {};
	format.mSampleRate = kSampleRate;
	format.mFormatID = kAudioFormatLinearPCM;
	format.mFormatFlags = kAudioFormatFlagIsFloat
		| kAudioFormatFlagIsPacked
		| kAudioFormatFlagsNativeEndian;
	format.mBytesPerPacket = kChannelCount * kBytesPerSample;
	format.mFramesPerPacket = 1;
	format.mBytesPerFrame = kChannelCount * kBytesPerSample;
	format.mChannelsPerFrame = kChannelCount;
	format.mBitsPerChannel = 8 * kBytesPerSample;
	return format;
}

AudioStreamRangedDescription StreamRangedDescription() noexcept
{
	AudioStreamRangedDescription description {};
	description.mFormat = StreamFormat();
	description.mSampleRateRange.mMinimum = kSampleRate;
	description.mSampleRateRange.mMaximum = kSampleRate;
	return description;
}

std::size_t AudioBufferListSize(UInt32 bufferCount) noexcept
{
	if (bufferCount == 0)
		return offsetof(AudioBufferList, mBuffers);
	return offsetof(AudioBufferList, mBuffers)
		+ static_cast<std::size_t>(bufferCount) * sizeof(AudioBuffer);
}

UInt32 StreamConfigurationBufferCount(
	const AudioObjectPropertyAddress *address) noexcept
{
	if (address->mScope == kAudioObjectPropertyScopeInput
		|| address->mScope == kAudioObjectPropertyScopeOutput) {
		return 1;
	}
	return 2;
}

OSStatus CopyStreamConfiguration(const AudioObjectPropertyAddress *address,
	UInt32 inDataSize, UInt32 *outDataSize, void *outData) noexcept
{
	const UInt32 bufferCount = StreamConfigurationBufferCount(address);
	const std::size_t bytes = AudioBufferListSize(bufferCount);
	if (outDataSize == nullptr || outData == nullptr || inDataSize < bytes)
		return kAudioHardwareBadPropertySizeError;

	auto *bufferList = static_cast<AudioBufferList *>(outData);
	bufferList->mNumberBuffers = bufferCount;
	for (UInt32 i = 0; i < bufferCount; ++i) {
		bufferList->mBuffers[i].mNumberChannels = kChannelCount;
		bufferList->mBuffers[i].mDataByteSize =
			kBufferFrameSize * kChannelCount * kBytesPerSample;
		bufferList->mBuffers[i].mData = nullptr;
	}

	*outDataSize = static_cast<UInt32>(bytes);
	return kAudioHardwareNoError;
}

AudioChannelLayout ChannelLayout() noexcept
{
	AudioChannelLayout layout {};
	layout.mChannelLayoutTag =
		kAudioChannelLayoutTag_DiscreteInOrder | kChannelCount;
	layout.mChannelBitmap = 0;
	layout.mNumberChannelDescriptions = 0;
	return layout;
}

bool IsCommonProperty(AudioObjectID objectId,
	const AudioObjectPropertyAddress *address) noexcept
{
	if (!IsKnownObject(objectId) || address == nullptr || !IsMainElement(address))
		return false;

	switch (address->mSelector) {
	case kAudioObjectPropertyBaseClass:
	case kAudioObjectPropertyClass:
	case kAudioObjectPropertyOwner:
	case kAudioObjectPropertyName:
	case kAudioObjectPropertyModelName:
	case kAudioObjectPropertyManufacturer:
	case kAudioObjectPropertyOwnedObjects:
		return IsGlobalScope(address)
			|| (objectId == kDeviceObjectId
				&& (address->mSelector == kAudioObjectPropertyOwnedObjects));
	case kAudioObjectPropertyIdentify:
		return objectId == kDeviceObjectId;
	default:
		return false;
	}
}

bool HasPlugInProperty(const AudioObjectPropertyAddress *address) noexcept
{
	if (!IsGlobalScope(address) || !IsMainElement(address))
		return false;

	switch (address->mSelector) {
	case kAudioPlugInPropertyBundleID:
	case kAudioPlugInPropertyDeviceList:
	case kAudioPlugInPropertyTranslateUIDToDevice:
	case kAudioPlugInPropertyBoxList:
	case kAudioPlugInPropertyTranslateUIDToBox:
	case kAudioPlugInPropertyClockDeviceList:
	case kAudioPlugInPropertyTranslateUIDToClockDevice:
		return true;
	default:
		return false;
	}
}

bool HasDeviceProperty(const AudioObjectPropertyAddress *address) noexcept
{
	if (!IsMainElement(address))
		return false;

	switch (address->mSelector) {
	case kAudioDevicePropertyPlugIn:
	case kAudioDevicePropertyDeviceUID:
	case kAudioDevicePropertyModelUID:
	case kAudioDevicePropertyTransportType:
	case kAudioDevicePropertyClockDomain:
	case kAudioDevicePropertyDeviceIsAlive:
	case kAudioDevicePropertyIsHidden:
	case kAudioDevicePropertyDeviceIsRunning:
	case kAudioDevicePropertyDeviceIsRunningSomewhere:
	case kAudioDevicePropertyDeviceCanBeDefaultDevice:
	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
	case kAudioDevicePropertyLatency:
	case kAudioDevicePropertyStreams:
	case kAudioObjectPropertyControlList:
	case kAudioDevicePropertySafetyOffset:
	case kAudioDevicePropertyZeroTimeStampPeriod:
	case kAudioDevicePropertyNominalSampleRate:
	case kAudioDevicePropertyActualSampleRate:
	case kAudioDevicePropertyAvailableNominalSampleRates:
	case kAudioDevicePropertyPreferredChannelsForStereo:
	case kAudioDevicePropertyPreferredChannelLayout:
	case kAudioDevicePropertyBufferFrameSize:
	case kAudioDevicePropertyBufferFrameSizeRange:
	case kAudioDevicePropertyUsesVariableBufferFrameSizes:
	case kAudioDevicePropertyIOCycleUsage:
	case kAudioDevicePropertyStreamConfiguration:
	case kAudioDevicePropertyHogMode:
		return true;
	default:
		return false;
	}
}

bool HasStreamProperty(const AudioObjectPropertyAddress *address) noexcept
{
	if (!IsGlobalScope(address) || !IsMainElement(address))
		return false;

	switch (address->mSelector) {
	case kAudioStreamPropertyIsActive:
	case kAudioStreamPropertyDirection:
	case kAudioStreamPropertyTerminalType:
	case kAudioStreamPropertyStartingChannel:
	case kAudioStreamPropertyLatency:
	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyAvailableVirtualFormats:
	case kAudioStreamPropertyPhysicalFormat:
	case kAudioStreamPropertyAvailablePhysicalFormats:
		return true;
	default:
		return false;
	}
}

UInt32 PropertyDataSize(AudioObjectID objectId, pid_t clientProcessId,
	const AudioObjectPropertyAddress *address, UInt32 qualifierDataSize,
	const void *qualifierData) noexcept
{
	(void)clientProcessId;

	if (!IsKnownObject(objectId) || address == nullptr)
		return 0;

	if (IsCommonProperty(objectId, address)) {
		switch (address->mSelector) {
		case kAudioObjectPropertyName:
		case kAudioObjectPropertyModelName:
		case kAudioObjectPropertyManufacturer:
			return sizeof(CFStringRef);
		case kAudioObjectPropertyOwnedObjects:
			return OwnedObjectList(objectId, address, qualifierDataSize,
				qualifierData, nullptr, 0)
				* static_cast<UInt32>(sizeof(AudioObjectID));
		default:
			return sizeof(UInt32);
		}
	}

	if (objectId == kAudioObjectPlugInObject && HasPlugInProperty(address)) {
		switch (address->mSelector) {
		case kAudioPlugInPropertyBundleID:
			return sizeof(CFStringRef);
		case kAudioPlugInPropertyDeviceList:
			return sizeof(AudioObjectID);
		case kAudioPlugInPropertyTranslateUIDToDevice:
		case kAudioPlugInPropertyTranslateUIDToBox:
		case kAudioPlugInPropertyTranslateUIDToClockDevice:
			return sizeof(AudioObjectID);
		default:
			return 0;
		}
	}

	if (objectId == kDeviceObjectId && HasDeviceProperty(address)) {
		switch (address->mSelector) {
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID:
			return sizeof(CFStringRef);
		case kAudioDevicePropertyStreams:
			return DeviceStreamList(address, nullptr, 0)
				* static_cast<UInt32>(sizeof(AudioObjectID));
		case kAudioObjectPropertyControlList:
			return 0;
		case kAudioDevicePropertyAvailableNominalSampleRates:
		case kAudioDevicePropertyBufferFrameSizeRange:
			return sizeof(AudioValueRange);
		case kAudioDevicePropertyPreferredChannelsForStereo:
			return 2 * sizeof(UInt32);
		case kAudioDevicePropertyPreferredChannelLayout:
			return sizeof(AudioChannelLayout);
		case kAudioDevicePropertyStreamConfiguration:
			return static_cast<UInt32>(AudioBufferListSize(
				StreamConfigurationBufferCount(address)));
		case kAudioDevicePropertyNominalSampleRate:
		case kAudioDevicePropertyActualSampleRate:
			return sizeof(Float64);
		case kAudioDevicePropertyIOCycleUsage:
			return sizeof(Float32);
		case kAudioDevicePropertyHogMode:
			return sizeof(pid_t);
		default:
			return sizeof(UInt32);
		}
	}

	if ((objectId == kOutputStreamObjectId || objectId == kInputStreamObjectId)
		&& HasStreamProperty(address)) {
		switch (address->mSelector) {
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			return sizeof(AudioStreamBasicDescription);
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			return sizeof(AudioStreamRangedDescription);
		default:
			return sizeof(UInt32);
		}
	}

	return 0;
}

HRESULT STDMETHODCALLTYPE QueryInterface(void *driver, REFIID uuid,
	LPVOID *outInterface) noexcept
{
	(void)driver;
	if (outInterface == nullptr)
		return E_POINTER;

	*outInterface = nullptr;
	if (UUIDEquals(uuid, kAudioServerPlugInDriverInterfaceUUID)
		|| UUIDEquals(uuid, IUnknownUUID)) {
		gRefCount.fetch_add(1);
		*outInterface = &gDriverInterfacePtr;
		return S_OK;
	}

	return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE AddRef(void *driver) noexcept
{
	(void)driver;
	return gRefCount.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE Release(void *driver) noexcept
{
	(void)driver;
	const UInt32 oldRefCount = gRefCount.fetch_sub(1);
	return oldRefCount == 0 ? 0 : oldRefCount - 1;
}

OSStatus STDMETHODCALLTYPE Initialize(AudioServerPlugInDriverRef driver,
	AudioServerPlugInHostRef host) noexcept
{
	(void)driver;
	gHost = host;
	(void)TimebaseInfo();
	gStartHostTime.store(mach_absolute_time(), std::memory_order_release);
	os_log(OS_LOG_DEFAULT,
		"AKAI EIE Pro HAL plugin initialized (USB playback transport)");
	return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE CreateDevice(AudioServerPlugInDriverRef driver,
	CFDictionaryRef description,
	const AudioServerPlugInClientInfo *clientInfo,
	AudioObjectID *outDeviceObjectId) noexcept
{
	(void)driver;
	(void)description;
	(void)clientInfo;
	if (outDeviceObjectId != nullptr)
		*outDeviceObjectId = kAudioObjectUnknown;
	return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE DestroyDevice(AudioServerPlugInDriverRef driver,
	AudioObjectID deviceObjectId) noexcept
{
	(void)driver;
	return deviceObjectId == kDeviceObjectId ? kAudioHardwareNoError
						 : kAudioHardwareBadObjectError;
}

OSStatus STDMETHODCALLTYPE AddDeviceClient(AudioServerPlugInDriverRef driver,
	AudioObjectID deviceObjectId,
	const AudioServerPlugInClientInfo *clientInfo) noexcept
{
	(void)driver;
	(void)clientInfo;
	return deviceObjectId == kDeviceObjectId ? kAudioHardwareNoError
						 : kAudioHardwareBadObjectError;
}

OSStatus STDMETHODCALLTYPE RemoveDeviceClient(AudioServerPlugInDriverRef driver,
	AudioObjectID deviceObjectId,
	const AudioServerPlugInClientInfo *clientInfo) noexcept
{
	(void)driver;
	(void)clientInfo;
	return deviceObjectId == kDeviceObjectId ? kAudioHardwareNoError
						 : kAudioHardwareBadObjectError;
}

OSStatus STDMETHODCALLTYPE PerformDeviceConfigurationChange(
	AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectId,
	UInt64 changeAction, void *changeInfo) noexcept
{
	(void)driver;
	(void)changeAction;
	(void)changeInfo;
	return deviceObjectId == kDeviceObjectId ? kAudioHardwareNoError
						 : kAudioHardwareBadObjectError;
}

OSStatus STDMETHODCALLTYPE AbortDeviceConfigurationChange(
	AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectId,
	UInt64 changeAction, void *changeInfo) noexcept
{
	(void)driver;
	(void)changeAction;
	(void)changeInfo;
	return deviceObjectId == kDeviceObjectId ? kAudioHardwareNoError
						 : kAudioHardwareBadObjectError;
}

Boolean STDMETHODCALLTYPE HasProperty(AudioServerPlugInDriverRef driver,
	AudioObjectID objectId, pid_t clientProcessId,
	const AudioObjectPropertyAddress *address) noexcept
{
	(void)driver;
	(void)clientProcessId;
	if (!IsKnownObject(objectId) || address == nullptr)
		return false;

	if (IsCommonProperty(objectId, address))
		return true;
	if (objectId == kAudioObjectPlugInObject)
		return HasPlugInProperty(address);
	if (objectId == kDeviceObjectId)
		return HasDeviceProperty(address);
	if (objectId == kOutputStreamObjectId || objectId == kInputStreamObjectId)
		return HasStreamProperty(address);

	return false;
}

OSStatus STDMETHODCALLTYPE IsPropertySettable(AudioServerPlugInDriverRef driver,
	AudioObjectID objectId, pid_t clientProcessId,
	const AudioObjectPropertyAddress *address, Boolean *outIsSettable) noexcept
{
	(void)driver;
	(void)clientProcessId;
	if (outIsSettable == nullptr)
		return kAudioHardwareBadPropertySizeError;
	if (!HasProperty(driver, objectId, clientProcessId, address))
		return kAudioHardwareUnknownPropertyError;

	*outIsSettable = false;
	return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE GetPropertyDataSize(
	AudioServerPlugInDriverRef driver, AudioObjectID objectId,
	pid_t clientProcessId, const AudioObjectPropertyAddress *address,
	UInt32 qualifierDataSize, const void *qualifierData,
	UInt32 *outDataSize) noexcept
{
	if (outDataSize == nullptr)
		return kAudioHardwareBadPropertySizeError;
	if (!HasProperty(driver, objectId, clientProcessId, address))
		return kAudioHardwareUnknownPropertyError;

	*outDataSize = PropertyDataSize(objectId, clientProcessId, address,
		qualifierDataSize, qualifierData);
	return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE GetPropertyData(AudioServerPlugInDriverRef driver,
	AudioObjectID objectId, pid_t clientProcessId,
	const AudioObjectPropertyAddress *address, UInt32 qualifierDataSize,
	const void *qualifierData, UInt32 inDataSize, UInt32 *outDataSize,
	void *outData) noexcept
{
	if (!HasProperty(driver, objectId, clientProcessId, address))
		return kAudioHardwareUnknownPropertyError;

	if (IsCommonProperty(objectId, address)) {
		switch (address->mSelector) {
		case kAudioObjectPropertyBaseClass:
			return CopyScalar<AudioClassID>(kAudioObjectClassID,
				inDataSize, outDataSize, outData);
		case kAudioObjectPropertyClass:
			return CopyScalar<AudioClassID>(ClassForObject(objectId),
				inDataSize, outDataSize, outData);
		case kAudioObjectPropertyOwner:
			return CopyScalar<AudioObjectID>(OwnerForObject(objectId),
				inDataSize, outDataSize, outData);
		case kAudioObjectPropertyName:
			return CopyCFString(NameForObject(objectId), inDataSize,
				outDataSize, outData);
		case kAudioObjectPropertyModelName:
			return CopyCFString(kDeviceName, inDataSize, outDataSize,
				outData);
		case kAudioObjectPropertyManufacturer:
			return CopyCFString(kManufacturerName, inDataSize,
				outDataSize, outData);
		case kAudioObjectPropertyOwnedObjects: {
			const UInt32 objectCount = OwnedObjectList(objectId, address,
				qualifierDataSize, qualifierData, nullptr, 0);
			const UInt32 bytes = objectCount * sizeof(AudioObjectID);
			if (outDataSize == nullptr || outData == nullptr
				|| inDataSize < bytes) {
				return kAudioHardwareBadPropertySizeError;
			}
			OwnedObjectList(objectId, address, qualifierDataSize,
				qualifierData, static_cast<AudioObjectID *>(outData),
				objectCount);
			*outDataSize = bytes;
			return kAudioHardwareNoError;
		}
		case kAudioObjectPropertyIdentify:
			return CopyScalar<UInt32>(0, inDataSize, outDataSize, outData);
		default:
			break;
		}
	}

	if (objectId == kAudioObjectPlugInObject) {
		switch (address->mSelector) {
		case kAudioPlugInPropertyBundleID:
			return CopyCFString(kBundleId, inDataSize, outDataSize, outData);
		case kAudioPlugInPropertyDeviceList:
			return CopyScalar<AudioObjectID>(kDeviceObjectId, inDataSize,
				outDataSize, outData);
		case kAudioPlugInPropertyTranslateUIDToDevice: {
			AudioObjectID result = kAudioObjectUnknown;
			if (qualifierDataSize == sizeof(CFStringRef)
				&& qualifierData != nullptr) {
				CFStringRef uid =
					*static_cast<CFStringRef const *>(qualifierData);
				CFStringRef expected = CFStringCreateWithCString(
					kCFAllocatorDefault, kDeviceUid,
					kCFStringEncodingUTF8);
				if (expected != nullptr) {
					if (CFStringCompare(uid, expected, 0)
						== kCFCompareEqualTo) {
						result = kDeviceObjectId;
					}
					CFRelease(expected);
				}
			}
			return CopyScalar<AudioObjectID>(result, inDataSize,
				outDataSize, outData);
		}
		case kAudioPlugInPropertyTranslateUIDToBox:
		case kAudioPlugInPropertyTranslateUIDToClockDevice:
			return CopyScalar<AudioObjectID>(kAudioObjectUnknown,
				inDataSize, outDataSize, outData);
		case kAudioPlugInPropertyBoxList:
		case kAudioPlugInPropertyClockDeviceList:
			if (outDataSize == nullptr)
				return kAudioHardwareBadPropertySizeError;
			*outDataSize = 0;
			return kAudioHardwareNoError;
		default:
			break;
		}
	}

	if (objectId == kDeviceObjectId) {
		switch (address->mSelector) {
		case kAudioDevicePropertyPlugIn:
			return CopyScalar<AudioObjectID>(kAudioObjectPlugInObject,
				inDataSize, outDataSize, outData);
		case kAudioDevicePropertyDeviceUID:
			return CopyCFString(kDeviceUid, inDataSize, outDataSize, outData);
		case kAudioDevicePropertyModelUID:
			return CopyCFString(kModelUid, inDataSize, outDataSize, outData);
		case kAudioDevicePropertyTransportType:
			return CopyScalar<UInt32>(kAudioDeviceTransportTypeUSB,
				inDataSize, outDataSize, outData);
		case kAudioDevicePropertyClockDomain:
			return CopyScalar<UInt32>(0, inDataSize, outDataSize, outData);
		case kAudioDevicePropertyDeviceIsAlive:
			return CopyScalar<UInt32>(1, inDataSize, outDataSize, outData);
		case kAudioDevicePropertyIsHidden:
			return CopyScalar<UInt32>(0, inDataSize, outDataSize, outData);
		case kAudioDevicePropertyDeviceIsRunning:
		case kAudioDevicePropertyDeviceIsRunningSomewhere:
			return CopyScalar<UInt32>(
				gRunningClientCount.load() > 0 ? 1u : 0u, inDataSize,
				outDataSize, outData);
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
			return CopyScalar<UInt32>(
				IsGlobalScope(address) || IsInputScope(address)
						|| IsOutputScope(address)
					? 1u
					: 0u,
				inDataSize, outDataSize, outData);
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
			return CopyScalar<UInt32>(
				IsGlobalScope(address) || IsOutputScope(address) ? 1u : 0u,
				inDataSize, outDataSize, outData);
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyUsesVariableBufferFrameSizes:
			return CopyScalar<UInt32>(0, inDataSize, outDataSize, outData);
		case kAudioDevicePropertyZeroTimeStampPeriod:
			return CopyScalar<UInt32>(kBufferFrameSize, inDataSize,
				outDataSize, outData);
		case kAudioDevicePropertyStreams: {
			const UInt32 streamCount = DeviceStreamList(address, nullptr, 0);
			const UInt32 bytes = streamCount * sizeof(AudioObjectID);
			if (outDataSize == nullptr || outData == nullptr
				|| inDataSize < bytes) {
				return kAudioHardwareBadPropertySizeError;
			}
			DeviceStreamList(address, static_cast<AudioObjectID *>(outData),
				streamCount);
			*outDataSize = bytes;
			return kAudioHardwareNoError;
		}
		case kAudioObjectPropertyControlList:
			if (outDataSize == nullptr)
				return kAudioHardwareBadPropertySizeError;
			*outDataSize = 0;
			return kAudioHardwareNoError;
		case kAudioDevicePropertyNominalSampleRate:
		case kAudioDevicePropertyActualSampleRate:
			return CopyScalar<Float64>(kSampleRate, inDataSize, outDataSize,
				outData);
		case kAudioDevicePropertyAvailableNominalSampleRates: {
			AudioValueRange range {kSampleRate, kSampleRate};
			return CopyScalar<AudioValueRange>(range, inDataSize,
				outDataSize, outData);
		}
		case kAudioDevicePropertyPreferredChannelsForStereo: {
			if (outDataSize == nullptr || outData == nullptr
				|| inDataSize < 2 * sizeof(UInt32)) {
				return kAudioHardwareBadPropertySizeError;
			}
			auto *channels = static_cast<UInt32 *>(outData);
			channels[0] = 1;
			channels[1] = 2;
			*outDataSize = 2 * sizeof(UInt32);
			return kAudioHardwareNoError;
		}
		case kAudioDevicePropertyPreferredChannelLayout: {
			const AudioChannelLayout layout = ChannelLayout();
			return CopyScalar<AudioChannelLayout>(layout, inDataSize,
				outDataSize, outData);
		}
		case kAudioDevicePropertyBufferFrameSize:
			return CopyScalar<UInt32>(kBufferFrameSize, inDataSize,
				outDataSize, outData);
		case kAudioDevicePropertyBufferFrameSizeRange: {
			AudioValueRange range {
				static_cast<Float64>(kBufferFrameSize),
				static_cast<Float64>(kBufferFrameSize),
			};
			return CopyScalar<AudioValueRange>(range, inDataSize,
				outDataSize, outData);
		}
		case kAudioDevicePropertyStreamConfiguration:
			return CopyStreamConfiguration(address, inDataSize, outDataSize,
				outData);
		case kAudioDevicePropertyIOCycleUsage:
			return CopyScalar<Float32>(0.0f, inDataSize, outDataSize,
				outData);
		case kAudioDevicePropertyHogMode:
			return CopyScalar<pid_t>(-1, inDataSize, outDataSize, outData);
		default:
			break;
		}
	}

	if (objectId == kOutputStreamObjectId || objectId == kInputStreamObjectId) {
		const bool isInput = objectId == kInputStreamObjectId;
		switch (address->mSelector) {
		case kAudioStreamPropertyIsActive:
			return CopyScalar<UInt32>(1, inDataSize, outDataSize, outData);
		case kAudioStreamPropertyDirection:
			return CopyScalar<UInt32>(isInput ? 1u : 0u, inDataSize,
				outDataSize, outData);
		case kAudioStreamPropertyTerminalType:
			return CopyScalar<UInt32>(
				isInput ? kAudioStreamTerminalTypeMicrophone
					: kAudioStreamTerminalTypeLine,
				inDataSize, outDataSize, outData);
		case kAudioStreamPropertyStartingChannel:
			return CopyScalar<UInt32>(1, inDataSize, outDataSize, outData);
		case kAudioStreamPropertyLatency:
			return CopyScalar<UInt32>(0, inDataSize, outDataSize, outData);
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat: {
			const AudioStreamBasicDescription format = StreamFormat();
			return CopyScalar<AudioStreamBasicDescription>(format, inDataSize,
				outDataSize, outData);
		}
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats: {
			const AudioStreamRangedDescription description =
				StreamRangedDescription();
			return CopyScalar<AudioStreamRangedDescription>(description,
				inDataSize, outDataSize, outData);
		}
		default:
			break;
		}
	}

	return kAudioHardwareUnknownPropertyError;
}

OSStatus STDMETHODCALLTYPE SetPropertyData(AudioServerPlugInDriverRef driver,
	AudioObjectID objectId, pid_t clientProcessId,
	const AudioObjectPropertyAddress *address, UInt32 qualifierDataSize,
	const void *qualifierData, UInt32 inDataSize, const void *inData) noexcept
{
	(void)qualifierDataSize;
	(void)qualifierData;
	(void)inDataSize;
	(void)inData;

	if (!HasProperty(driver, objectId, clientProcessId, address))
		return kAudioHardwareUnknownPropertyError;
	return kAudioHardwareIllegalOperationError;
}

OSStatus STDMETHODCALLTYPE StartIO(AudioServerPlugInDriverRef driver,
	AudioObjectID deviceObjectId, UInt32 clientId) noexcept
{
	(void)driver;
	if (deviceObjectId != kDeviceObjectId)
		return kAudioHardwareBadObjectError;

	const UInt32 oldClientCount = gRunningClientCount.fetch_add(1);
	if (oldClientCount == 0 && !gUsbTransport.Start()) {
		gRunningClientCount.fetch_sub(1);
		os_log_error(OS_LOG_DEFAULT,
			"AKAI EIE Pro HAL StartIO failed: USB transport not ready");
		return kAudioHardwareNotReadyError;
	}

	os_log(OS_LOG_DEFAULT,
		"AKAI EIE Pro HAL StartIO client %{public}u",
		clientId);
	return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE StopIO(AudioServerPlugInDriverRef driver,
	AudioObjectID deviceObjectId, UInt32 clientId) noexcept
{
	(void)driver;
	if (deviceObjectId != kDeviceObjectId)
		return kAudioHardwareBadObjectError;

	UInt32 oldCount = gRunningClientCount.load();
	while (oldCount > 0
		&& !gRunningClientCount.compare_exchange_weak(oldCount, oldCount - 1)) {
	}
	if (oldCount == 1)
		gUsbTransport.Stop();

	os_log(OS_LOG_DEFAULT,
		"AKAI EIE Pro HAL StopIO client %{public}u",
		clientId);
	return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE GetZeroTimeStamp(AudioServerPlugInDriverRef driver,
	AudioObjectID deviceObjectId, UInt32 clientId, Float64 *outSampleTime,
	UInt64 *outHostTime, UInt64 *outSeed) noexcept
{
	(void)driver;
	(void)clientId;
	if (deviceObjectId != kDeviceObjectId)
		return kAudioHardwareBadObjectError;
	if (outSampleTime == nullptr || outHostTime == nullptr || outSeed == nullptr)
		return kAudioHardwareBadPropertySizeError;

	const UInt64 startHostTime =
		gStartHostTime.load(std::memory_order_acquire);
	if (startHostTime == 0)
		return kAudioHardwareNotReadyError;

	const mach_timebase_info_data_t &timebase = TimebaseInfo();
	const UInt64 now = mach_absolute_time();
	const UInt64 elapsedTicks = now - startHostTime;
	const Float64 elapsedNanos =
		static_cast<Float64>(elapsedTicks) * timebase.numer / timebase.denom;
	const Float64 rawSampleTime =
		(elapsedNanos / 1000000000.0) * kSampleRate;
	const UInt64 cycleCount =
		static_cast<UInt64>(rawSampleTime) / kBufferFrameSize;
	const UInt64 cycleSampleTime = cycleCount * kBufferFrameSize;
	const Float64 cycleSeconds =
		static_cast<Float64>(cycleSampleTime) / kSampleRate;
	const UInt64 cycleNanos =
		static_cast<UInt64>(cycleSeconds * 1000000000.0);
	const UInt64 cycleTicks = static_cast<UInt64>(
		(static_cast<long double>(cycleNanos) * timebase.denom)
		/ timebase.numer);

	*outSampleTime = static_cast<Float64>(cycleSampleTime);
	*outHostTime = startHostTime + cycleTicks;
	*outSeed = kClockSeed;
	return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE WillDoIOOperation(AudioServerPlugInDriverRef driver,
	AudioObjectID deviceObjectId, UInt32 clientId, UInt32 operationId,
	Boolean *outWillDo, Boolean *outWillDoInPlace) noexcept
{
	(void)driver;
	(void)clientId;
	if (deviceObjectId != kDeviceObjectId)
		return kAudioHardwareBadObjectError;
	if (outWillDo == nullptr || outWillDoInPlace == nullptr)
		return kAudioHardwareBadPropertySizeError;

	*outWillDo = operationId == kAudioServerPlugInIOOperationReadInput
		|| operationId == kAudioServerPlugInIOOperationWriteMix;
	*outWillDoInPlace = true;
	return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE BeginIOOperation(AudioServerPlugInDriverRef driver,
	AudioObjectID deviceObjectId, UInt32 clientId, UInt32 operationId,
	UInt32 ioBufferFrameSize,
	const AudioServerPlugInIOCycleInfo *ioCycleInfo) noexcept
{
	(void)driver;
	(void)clientId;
	(void)operationId;
	(void)ioBufferFrameSize;
	(void)ioCycleInfo;
	return deviceObjectId == kDeviceObjectId ? kAudioHardwareNoError
						 : kAudioHardwareBadObjectError;
}

OSStatus STDMETHODCALLTYPE DoIOOperation(AudioServerPlugInDriverRef driver,
	AudioObjectID deviceObjectId, AudioObjectID streamObjectId, UInt32 clientId,
	UInt32 operationId, UInt32 ioBufferFrameSize,
	const AudioServerPlugInIOCycleInfo *ioCycleInfo, void *ioMainBuffer,
	void *ioSecondaryBuffer) noexcept
{
	(void)driver;
	(void)clientId;
	(void)ioCycleInfo;
	(void)ioSecondaryBuffer;

	if (deviceObjectId != kDeviceObjectId)
		return kAudioHardwareBadObjectError;

	if (operationId == kAudioServerPlugInIOOperationReadInput) {
		if (streamObjectId != kInputStreamObjectId)
			return kAudioHardwareBadObjectError;
		if (ioMainBuffer != nullptr) {
			std::memset(ioMainBuffer, 0,
				static_cast<std::size_t>(ioBufferFrameSize)
					* kChannelCount * kBytesPerSample);
		}
		return kAudioHardwareNoError;
	}

	if (operationId == kAudioServerPlugInIOOperationWriteMix) {
		if (streamObjectId != kOutputStreamObjectId)
			return kAudioHardwareBadObjectError;
		if (ioMainBuffer != nullptr) {
			gUsbTransport.EnqueueOutput(
				static_cast<const Float32 *>(ioMainBuffer),
				ioBufferFrameSize);
		}
		return kAudioHardwareNoError;
	}

	return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE EndIOOperation(AudioServerPlugInDriverRef driver,
	AudioObjectID deviceObjectId, UInt32 clientId, UInt32 operationId,
	UInt32 ioBufferFrameSize,
	const AudioServerPlugInIOCycleInfo *ioCycleInfo) noexcept
{
	(void)driver;
	(void)clientId;
	(void)operationId;
	(void)ioBufferFrameSize;
	(void)ioCycleInfo;
	return deviceObjectId == kDeviceObjectId ? kAudioHardwareNoError
						 : kAudioHardwareBadObjectError;
}

AudioServerPlugInDriverInterface gDriverInterface = {
	nullptr,
	QueryInterface,
	AddRef,
	Release,
	Initialize,
	CreateDevice,
	DestroyDevice,
	AddDeviceClient,
	RemoveDeviceClient,
	PerformDeviceConfigurationChange,
	AbortDeviceConfigurationChange,
	HasProperty,
	IsPropertySettable,
	GetPropertyDataSize,
	GetPropertyData,
	SetPropertyData,
	StartIO,
	StopIO,
	GetZeroTimeStamp,
	WillDoIOOperation,
	BeginIOOperation,
	DoIOOperation,
	EndIOOperation,
};

AudioServerPlugInDriverInterface *gDriverInterfacePtr = &gDriverInterface;

} // namespace

extern "C" __attribute__((visibility("default"))) void *AkaiEieProFactory(
	CFAllocatorRef allocator, CFUUIDRef requestedTypeUuid)
{
	(void)allocator;
	if (requestedTypeUuid == nullptr)
		return nullptr;

	if (CFEqual(requestedTypeUuid, kAudioServerPlugInTypeUUID)) {
		return &gDriverInterfacePtr;
	}

	return nullptr;
}
