#include "MediaSource.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfvirtualcamera.h>
#include <propvarutil.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace wpc::virtual_camera {

namespace {

using Microsoft::WRL::ComPtr;

HRESULT pointerError(const void* pointer) {
    return pointer == nullptr ? E_POINTER : S_OK;
}

} // namespace

MediaStream::MediaStream(
    IMFMediaSource* source,
    IMFStreamDescriptor* descriptor,
    IMFMediaType* mediaType,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t frameRateNumerator,
    const std::uint32_t frameRateDenominator,
    const std::uint32_t dataBytes)
    : source_(source),
      descriptor_(descriptor),
      mediaType_(mediaType),
      width_(width),
      height_(height),
      frameRateNumerator_(frameRateNumerator),
      frameRateDenominator_(frameRateDenominator),
      dataBytes_(dataBytes),
      packedFrame_(dataBytes),
      sampleDuration_(static_cast<LONGLONG>(10'000'000ULL * frameRateDenominator /
                                            std::max(1U, frameRateNumerator))) {
    ++ObjectCount;
}

MediaStream::~MediaStream() {
    shutdown();
    --ObjectCount;
}

HRESULT MediaStream::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    HRESULT result = MFCreateAttributes(&attributes_, 4);
    if (FAILED(result) ||
        FAILED(result = attributes_->SetGUID(
                   MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE)) ||
        FAILED(result = attributes_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0)) ||
        FAILED(result = attributes_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1)) ||
        FAILED(result = attributes_->SetUINT32(
                   MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES,
                   MFFrameSourceTypes_Color))) {
        return result;
    }
    return MFCreateEventQueue(&eventQueue_);
}

HRESULT MediaStream::start(IMFMediaType* mediaType) {
    std::lock_guard<std::mutex> lock(mutex_);
    const HRESULT stateResult = checkStateLocked();
    if (FAILED(stateResult)) {
        return stateResult;
    }
    if (mediaType == nullptr) {
        return E_INVALIDARG;
    }
    mediaType_ = mediaType;
    const HRESULT allocatorResult = initializeAllocatorLocked();
    if (FAILED(allocatorResult)) {
        return allocatorResult;
    }
    started_ = true;
    streamState_ = MF_STREAM_STATE_RUNNING;
    nextSampleTime_ = MFGetSystemTime();
    const HRESULT result = eventQueue_->QueueEventParamVar(
        MEStreamStarted, GUID_NULL, S_OK, nullptr);
    if (SUCCEEDED(result)) {
        shared_frame::reportFailure(400, S_OK);
    }
    return result;
}

HRESULT MediaStream::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    const HRESULT stateResult = checkStateLocked();
    if (FAILED(stateResult)) {
        return stateResult;
    }
    started_ = false;
    streamState_ = MF_STREAM_STATE_STOPPED;
    PROPVARIANT value;
    PropVariantInit(&value);
    value.vt = VT_I8;
    value.hVal.QuadPart = MFGetSystemTime();
    return eventQueue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, &value);
}

HRESULT MediaStream::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) {
        return S_OK;
    }
    shutdown_ = true;
    started_ = false;
    frameReader_.close();
    if (eventQueue_) {
        eventQueue_->Shutdown();
        eventQueue_.Reset();
    }
    descriptor_.Reset();
    attributes_.Reset();
    sampleAllocator_.Reset();
    mediaType_.Reset();
    allocatorInitialized_ = false;
    source_.Reset();
    return S_OK;
}

HRESULT MediaStream::getAttributes(IMFAttributes** attributes) {
    if (attributes == nullptr) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (FAILED(checkStateLocked())) {
        *attributes = nullptr;
        return MF_E_SHUTDOWN;
    }
    *attributes = attributes_.Get();
    (*attributes)->AddRef();
    return S_OK;
}

HRESULT MediaStream::setSampleAllocator(IMFVideoSampleAllocator* allocator) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (FAILED(checkStateLocked())) {
        return MF_E_SHUTDOWN;
    }
    sampleAllocator_ = allocator;
    allocatorInitialized_ = false;
    return started_ ? initializeAllocatorLocked() : S_OK;
}

HRESULT MediaStream::QueryInterface(REFIID interfaceId, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (interfaceId == IID_IUnknown || interfaceId == __uuidof(IMFMediaEventGenerator) ||
        interfaceId == __uuidof(IMFMediaStream) || interfaceId == __uuidof(IMFMediaStream2)) {
        *object = static_cast<IMFMediaStream2*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG MediaStream::AddRef() {
    return ++references_;
}

ULONG MediaStream::Release() {
    const ULONG references = --references_;
    if (references == 0) {
        delete this;
    }
    return references;
}

HRESULT MediaStream::GetEvent(const DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (FAILED(checkStateLocked())) {
            return MF_E_SHUTDOWN;
        }
        queue = eventQueue_;
    }
    return queue->GetEvent(flags, event);
}

HRESULT MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard<std::mutex> lock(mutex_);
    return FAILED(checkStateLocked()) ? MF_E_SHUTDOWN : eventQueue_->BeginGetEvent(callback, state);
}

HRESULT MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard<std::mutex> lock(mutex_);
    return FAILED(checkStateLocked()) ? MF_E_SHUTDOWN : eventQueue_->EndGetEvent(result, event);
}

HRESULT MediaStream::QueueEvent(
    const MediaEventType eventType,
    REFGUID extendedType,
    const HRESULT status,
    const PROPVARIANT* value) {
    std::lock_guard<std::mutex> lock(mutex_);
    return FAILED(checkStateLocked())
               ? MF_E_SHUTDOWN
               : eventQueue_->QueueEventParamVar(eventType, extendedType, status, value);
}

HRESULT MediaStream::GetMediaSource(IMFMediaSource** mediaSource) {
    if (mediaSource == nullptr) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (FAILED(checkStateLocked()) || !source_) {
        *mediaSource = nullptr;
        return MF_E_SHUTDOWN;
    }
    return source_.CopyTo(mediaSource);
}

HRESULT MediaStream::GetStreamDescriptor(IMFStreamDescriptor** streamDescriptor) {
    if (streamDescriptor == nullptr) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (FAILED(checkStateLocked())) {
        *streamDescriptor = nullptr;
        return MF_E_SHUTDOWN;
    }
    *streamDescriptor = descriptor_.Get();
    (*streamDescriptor)->AddRef();
    return S_OK;
}

HRESULT MediaStream::RequestSample(IUnknown* token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!firstSampleDiagnosticCompleted_) {
        shared_frame::reportFailure(409, S_OK);
    }
    const HRESULT stateResult = checkStateLocked();
    if (FAILED(stateResult)) {
        shared_frame::reportFailure(410, stateResult);
        return stateResult;
    }
    if (streamState_ != MF_STREAM_STATE_RUNNING) {
        shared_frame::reportFailure(411, MF_E_INVALIDREQUEST);
        return MF_E_INVALIDREQUEST;
    }

    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = S_OK;
    if (sampleAllocator_) {
        if (FAILED(result = sampleAllocator_->AllocateSample(&sample)) ||
            FAILED(result = sample->GetBufferByIndex(0, &buffer))) {
            shared_frame::reportFailure(412, result);
            return result;
        }
    } else {
        if (FAILED(result = MFCreateMemoryBuffer(dataBytes_, &buffer)) ||
            FAILED(result = MFCreateSample(&sample)) ||
            FAILED(result = sample->AddBuffer(buffer.Get()))) {
            shared_frame::reportFailure(413, result);
            return result;
        }
    }
    ComPtr<IMF2DBuffer2> buffer2D;
    if (SUCCEEDED(buffer.As(&buffer2D))) {
        BYTE* scanline0 = nullptr;
        BYTE* bufferStart = nullptr;
        LONG pitch = 0;
        DWORD bufferLength = 0;
        result = buffer2D->Lock2DSize(
            MF2DBuffer_LockFlags_Write,
            &scanline0,
            &pitch,
            &bufferStart,
            &bufferLength);
        if (FAILED(result)) {
            shared_frame::reportFailure(414, result);
            return result;
        }
        const std::uint32_t rowCount = height_ + height_ / 2U;
        const auto bufferAddress = reinterpret_cast<std::uintptr_t>(bufferStart);
        const auto scanlineAddress = reinterpret_cast<std::uintptr_t>(scanline0);
        const std::uint64_t scanlineOffset =
            scanlineAddress >= bufferAddress ? scanlineAddress - bufferAddress : UINT64_MAX;
        const std::uint64_t requiredBytes = pitch > 0
            ? static_cast<std::uint64_t>(pitch) * (rowCount - 1U) + width_
            : UINT64_MAX;
        const bool layoutValid = scanline0 != nullptr && bufferStart != nullptr &&
                                 pitch >= static_cast<LONG>(width_) &&
                                 scanlineOffset <= bufferLength &&
                                 requiredBytes <= bufferLength - scanlineOffset;
        if (layoutValid && pitch == static_cast<LONG>(width_)) {
            if (!frameReader_.copyLatest(scanline0, dataBytes_)) {
                fillBlackFrame(scanline0);
            }
        } else if (layoutValid) {
            if (!frameReader_.copyLatest(packedFrame_.data(), dataBytes_)) {
                fillBlackFrame(packedFrame_.data());
            }
            for (std::uint32_t row = 0; row < rowCount; ++row) {
                std::memcpy(
                    scanline0 + static_cast<std::ptrdiff_t>(row) * pitch,
                    packedFrame_.data() + static_cast<std::size_t>(row) * width_,
                    width_);
            }
        }
        const HRESULT unlockResult = buffer2D->Unlock2D();
        if (!layoutValid || FAILED(unlockResult)) {
            result = !layoutValid ? E_UNEXPECTED : unlockResult;
            shared_frame::reportFailure(415, result);
            return result;
        }
    } else {
        BYTE* destination = nullptr;
        DWORD maximumLength = 0;
        result = buffer->Lock(&destination, &maximumLength, nullptr);
        if (FAILED(result)) {
            shared_frame::reportFailure(414, result);
            return result;
        }
        const bool copied = maximumLength >= dataBytes_ &&
                            frameReader_.copyLatest(destination, dataBytes_);
        if (!copied && maximumLength >= dataBytes_) {
            fillBlackFrame(destination);
        }
        const HRESULT unlockResult = buffer->Unlock();
        if (maximumLength < dataBytes_ || FAILED(unlockResult)) {
            result = maximumLength < dataBytes_ ? E_UNEXPECTED : unlockResult;
            shared_frame::reportFailure(415, result);
            return result;
        }
        if (!sampleAllocator_) {
            result = buffer->SetCurrentLength(dataBytes_);
            if (FAILED(result)) {
                shared_frame::reportFailure(416, result);
                return result;
            }
        }
    }

    if (FAILED(result = sample->SetSampleTime(nextSampleTime_)) ||
        FAILED(result = sample->SetSampleDuration(sampleDuration_))) {
        shared_frame::reportFailure(417, result);
        return result;
    }
    nextSampleTime_ += sampleDuration_;
    if (token != nullptr) {
        result = sample->SetUnknown(MFSampleExtension_Token, token);
        if (FAILED(result)) {
            shared_frame::reportFailure(418, result);
            return result;
        }
    }
    result = eventQueue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    if (FAILED(result)) {
        shared_frame::reportFailure(418, result);
    } else if (!firstSampleDiagnosticCompleted_) {
        shared_frame::reportFailure(419, S_OK);
        firstSampleDiagnosticCompleted_ = true;
    }
    return result;
}

HRESULT MediaStream::SetStreamState(const MF_STREAM_STATE value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (FAILED(checkStateLocked())) {
        return MF_E_SHUTDOWN;
    }
    if (streamState_ == value) {
        return S_OK;
    }
    switch (value) {
    case MF_STREAM_STATE_RUNNING: {
        const HRESULT allocatorResult = initializeAllocatorLocked();
        if (FAILED(allocatorResult)) {
            return allocatorResult;
        }
        started_ = true;
        nextSampleTime_ = MFGetSystemTime();
        break;
    }
    case MF_STREAM_STATE_PAUSED:
        if (streamState_ != MF_STREAM_STATE_RUNNING) {
            return MF_E_INVALID_STATE_TRANSITION;
        }
        break;
    case MF_STREAM_STATE_STOPPED:
        started_ = false;
        break;
    default:
        return MF_E_INVALID_STATE_TRANSITION;
    }
    streamState_ = value;
    return S_OK;
}

HRESULT MediaStream::GetStreamState(MF_STREAM_STATE* value) {
    if (value == nullptr) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (FAILED(checkStateLocked())) {
        return MF_E_SHUTDOWN;
    }
    *value = streamState_;
    return S_OK;
}

HRESULT MediaStream::checkStateLocked() const {
    return shutdown_ || !eventQueue_ ? MF_E_SHUTDOWN : S_OK;
}

HRESULT MediaStream::initializeAllocatorLocked() {
    if (!sampleAllocator_ || allocatorInitialized_) {
        return S_OK;
    }
    if (!mediaType_) {
        return MF_E_NOT_INITIALIZED;
    }
    const HRESULT result = sampleAllocator_->InitializeSampleAllocator(3, mediaType_.Get());
    if (SUCCEEDED(result)) {
        allocatorInitialized_ = true;
        shared_frame::reportFailure(402, S_OK);
    } else {
        shared_frame::reportFailure(401, result);
    }
    return result;
}

void MediaStream::fillBlackFrame(std::uint8_t* destination) const {
    const std::size_t yBytes = static_cast<std::size_t>(width_) * height_;
    std::fill(destination, destination + yBytes, static_cast<std::uint8_t>(16));
    std::fill(destination + yBytes, destination + dataBytes_, static_cast<std::uint8_t>(128));
}

MediaSource::MediaSource() {
    ++ObjectCount;
}

MediaSource::~MediaSource() {
    Shutdown();
    --ObjectCount;
}

HRESULT MediaSource::initialize(IMFAttributes* activationAttributes) {
    std::lock_guard<std::mutex> lock(mutex_);
    HRESULT result = MFCreateEventQueue(&eventQueue_);
    if (FAILED(result) || FAILED(result = MFCreateAttributes(&sourceAttributes_, 8))) {
        return result;
    }
    if (activationAttributes != nullptr &&
        FAILED(result = activationAttributes->CopyAllItems(sourceAttributes_.Get()))) {
        return result;
    }
    sourceAttributes_->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_CATEGORY,
        KSCATEGORY_VIDEO_CAMERA);
    ComPtr<IMFSensorProfileCollection> profileCollection;
    ComPtr<IMFSensorProfile> profile;
    if (FAILED(result = MFCreateSensorProfileCollection(&profileCollection)) ||
        FAILED(result = MFCreateSensorProfile(
                   KSCAMERAPROFILE_Legacy, 0, nullptr, &profile)) ||
        FAILED(result = profile->AddProfileFilter(
                   0, L"((RES==;FRT<=30,1;SUT==))")) ||
        FAILED(result = profileCollection->AddProfile(profile.Get()))) {
        return result;
    }
    profile.Reset();
    if (FAILED(result = MFCreateSensorProfile(
                   KSCAMERAPROFILE_HighFrameRate, 0, nullptr, &profile)) ||
        FAILED(result = profile->AddProfileFilter(
                   0, L"((RES==;FRT>=60,1;SUT==))")) ||
        FAILED(result = profileCollection->AddProfile(profile.Get())) ||
        FAILED(result = sourceAttributes_->SetUnknown(
                   MF_DEVICEMFT_SENSORPROFILE_COLLECTION,
                   profileCollection.Get()))) {
        return result;
    }

    SharedFrameReader formatReader;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    std::uint32_t frameRateNumerator = 30;
    std::uint32_t frameRateDenominator = 1;
    std::uint32_t dataBytes = width * height * 3 / 2;
    (void)formatReader.readFormat(
        width, height, frameRateNumerator, frameRateDenominator, dataBytes);

    ComPtr<IMFMediaType> mediaType;
    if (FAILED(result = MFCreateMediaType(&mediaType)) ||
        FAILED(result = mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(result = mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) ||
        FAILED(result = MFSetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, width, height)) ||
        FAILED(result = MFSetAttributeRatio(
            mediaType.Get(),
            MF_MT_FRAME_RATE,
            frameRateNumerator,
            frameRateDenominator)) ||
        FAILED(result = MFSetAttributeRatio(mediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
        FAILED(result = mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
        FAILED(result = mediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE)) ||
        FAILED(result = mediaType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE)) ||
        FAILED(result = mediaType->SetUINT32(MF_MT_SAMPLE_SIZE, dataBytes)) ||
        FAILED(result = mediaType->SetUINT32(MF_MT_DEFAULT_STRIDE, width))) {
        return result;
    }
    IMFMediaType* mediaTypes[] = {mediaType.Get()};
    if (FAILED(result = MFCreateStreamDescriptor(0, 1, mediaTypes, &streamDescriptor_))) {
        return result;
    }
    ComPtr<IMFAttributes> streamAttributes;
    if (FAILED(result = streamDescriptor_.As(&streamAttributes)) ||
        FAILED(result = streamAttributes->SetGUID(
                   MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE)) ||
        FAILED(result = streamAttributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0)) ||
        FAILED(result = streamAttributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1)) ||
        FAILED(result = streamAttributes->SetUINT32(
                   MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES,
                   MFFrameSourceTypes_Color))) {
        return result;
    }
    ComPtr<IMFMediaTypeHandler> mediaTypeHandler;
    if (FAILED(result = streamDescriptor_->GetMediaTypeHandler(&mediaTypeHandler)) ||
        FAILED(result = mediaTypeHandler->SetCurrentMediaType(mediaType.Get()))) {
        return result;
    }
    auto* stream = new (std::nothrow) MediaStream(
        static_cast<IMFMediaSource*>(this),
        streamDescriptor_.Get(),
        mediaType.Get(),
        width,
        height,
        frameRateNumerator,
        frameRateDenominator,
        dataBytes);
    if (stream == nullptr) {
        return E_OUTOFMEMORY;
    }
    stream_.Attach(stream);
    if (FAILED(result = stream_->initialize())) {
        return result;
    }
    IMFStreamDescriptor* descriptors[] = {streamDescriptor_.Get()};
    if (FAILED(result = MFCreatePresentationDescriptor(1, descriptors, &presentationDescriptor_))) {
        return result;
    }
    return S_OK;
}

HRESULT MediaSource::QueryInterface(REFIID interfaceId, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (interfaceId == IID_IUnknown || interfaceId == __uuidof(IMFMediaEventGenerator) ||
        interfaceId == __uuidof(IMFMediaSource)) {
        *object = static_cast<IMFMediaSource*>(this);
    } else if (interfaceId == __uuidof(IMFMediaSourceEx)) {
        *object = static_cast<IMFMediaSourceEx*>(this);
    } else if (interfaceId == __uuidof(IMFGetService)) {
        *object = static_cast<IMFGetService*>(this);
    } else if (interfaceId == __uuidof(IKsControl)) {
        *object = static_cast<IKsControl*>(this);
    } else if (interfaceId == __uuidof(IMFSampleAllocatorControl)) {
        *object = static_cast<IMFSampleAllocatorControl*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG MediaSource::AddRef() {
    return ++references_;
}

ULONG MediaSource::Release() {
    const ULONG references = --references_;
    if (references == 0) {
        delete this;
    }
    return references;
}

HRESULT MediaSource::GetEvent(const DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (FAILED(checkStateLocked())) {
            return MF_E_SHUTDOWN;
        }
        queue = eventQueue_;
    }
    return queue->GetEvent(flags, event);
}

HRESULT MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard<std::mutex> lock(mutex_);
    return FAILED(checkStateLocked()) ? MF_E_SHUTDOWN : eventQueue_->BeginGetEvent(callback, state);
}

HRESULT MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard<std::mutex> lock(mutex_);
    return FAILED(checkStateLocked()) ? MF_E_SHUTDOWN : eventQueue_->EndGetEvent(result, event);
}

HRESULT MediaSource::QueueEvent(
    const MediaEventType eventType,
    REFGUID extendedType,
    const HRESULT status,
    const PROPVARIANT* value) {
    std::lock_guard<std::mutex> lock(mutex_);
    return FAILED(checkStateLocked())
               ? MF_E_SHUTDOWN
               : eventQueue_->QueueEventParamVar(eventType, extendedType, status, value);
}

HRESULT MediaSource::GetCharacteristics(DWORD* characteristics) {
    if (characteristics == nullptr) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (FAILED(checkStateLocked())) {
        return MF_E_SHUTDOWN;
    }
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

HRESULT MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** presentationDescriptor) {
    if (presentationDescriptor == nullptr) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return FAILED(checkStateLocked())
               ? MF_E_SHUTDOWN
               : presentationDescriptor_->Clone(presentationDescriptor);
}

HRESULT MediaSource::Start(
    IMFPresentationDescriptor* presentationDescriptor,
    const GUID* timeFormat,
    const PROPVARIANT* startPosition) {
    shared_frame::reportFailure(500, S_OK);
    if (presentationDescriptor == nullptr || startPosition == nullptr) {
        shared_frame::reportFailure(501, E_INVALIDARG);
        return E_INVALIDARG;
    }
    if (timeFormat != nullptr && *timeFormat != GUID_NULL) {
        shared_frame::reportFailure(502, MF_E_UNSUPPORTED_TIME_FORMAT);
        return MF_E_UNSUPPORTED_TIME_FORMAT;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (FAILED(checkStateLocked())) {
        return MF_E_SHUTDOWN;
    }

    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> selectedDescriptor;
    HRESULT result = presentationDescriptor->GetStreamDescriptorByIndex(
        0, &selected, &selectedDescriptor);
    if (FAILED(result)) {
        shared_frame::reportFailure(503, result);
        return result;
    }
    BOOL wasSelected = FALSE;
    ComPtr<IMFStreamDescriptor> internalDescriptor;
    if (FAILED(result = presentationDescriptor_->GetStreamDescriptorByIndex(
                   0, &wasSelected, &internalDescriptor))) {
        shared_frame::reportFailure(504, result);
        return result;
    }
    if (selected) {
        ComPtr<IMFMediaTypeHandler> mediaTypeHandler;
        ComPtr<IMFMediaType> selectedMediaType;
        if (FAILED(result = selectedDescriptor->GetMediaTypeHandler(&mediaTypeHandler)) ||
            FAILED(result = mediaTypeHandler->GetCurrentMediaType(&selectedMediaType))) {
            shared_frame::reportFailure(505, result);
            return result;
        }
        if (FAILED(result = presentationDescriptor_->SelectStream(0))) {
            shared_frame::reportFailure(506, result);
            return result;
        }
        result = eventQueue_->QueueEventParamUnk(
            wasSelected ? MEUpdatedStream : MENewStream,
            GUID_NULL,
            S_OK,
            stream_.Get());
        if (FAILED(result)) {
            shared_frame::reportFailure(507, result);
            return result;
        }
        result = stream_->start(selectedMediaType.Get());
        if (FAILED(result)) {
            shared_frame::reportFailure(508, result);
            return result;
        }
    } else if (wasSelected) {
        if (FAILED(result = presentationDescriptor_->DeselectStream(0)) ||
            FAILED(result = stream_->stop())) {
            shared_frame::reportFailure(508, result);
            return result;
        }
    }
    PROPVARIANT startTime;
    PropVariantInit(&startTime);
    startTime.vt = VT_I8;
    startTime.hVal.QuadPart = MFGetSystemTime();
    result = eventQueue_->QueueEventParamVar(
        MESourceStarted, GUID_NULL, S_OK, &startTime);
    if (FAILED(result)) {
        shared_frame::reportFailure(509, result);
        return result;
    }
    shared_frame::reportFailure(510, S_OK);
    state_ = State::Started;
    return S_OK;
}

HRESULT MediaSource::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (FAILED(checkStateLocked())) {
        return MF_E_SHUTDOWN;
    }
    if (state_ != State::Started) {
        return MF_E_INVALID_STATE_TRANSITION;
    }
    HRESULT result = stream_->stop();
    if (FAILED(result)) {
        return result;
    }
    result = presentationDescriptor_->DeselectStream(0);
    if (FAILED(result)) {
        return result;
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    value.vt = VT_I8;
    value.hVal.QuadPart = MFGetSystemTime();
    result = eventQueue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &value);
    if (SUCCEEDED(result)) {
        state_ = State::Stopped;
    }
    return result;
}

HRESULT MediaSource::Pause() {
    return MF_E_INVALID_STATE_TRANSITION;
}

HRESULT MediaSource::Shutdown() {
    ComPtr<MediaStream> stream;
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Shutdown) {
            return S_OK;
        }
        state_ = State::Shutdown;
        stream = stream_;
        queue = eventQueue_;
        stream_.Reset();
        streamDescriptor_.Reset();
        presentationDescriptor_.Reset();
        sourceAttributes_.Reset();
        eventQueue_.Reset();
    }
    if (stream) {
        stream->shutdown();
    }
    if (queue) {
        queue->Shutdown();
    }
    return S_OK;
}

HRESULT MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
    if (attributes == nullptr) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (FAILED(checkStateLocked())) {
        *attributes = nullptr;
        return MF_E_SHUTDOWN;
    }
    *attributes = sourceAttributes_.Get();
    (*attributes)->AddRef();
    return S_OK;
}

HRESULT MediaSource::GetStreamAttributes(const DWORD streamIdentifier, IMFAttributes** attributes) {
    if (attributes == nullptr) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (FAILED(checkStateLocked())) {
        *attributes = nullptr;
        return MF_E_SHUTDOWN;
    }
    if (streamIdentifier != 0) {
        *attributes = nullptr;
        return MF_E_INVALIDSTREAMNUMBER;
    }
    return stream_->getAttributes(attributes);
}

HRESULT MediaSource::SetD3DManager(IUnknown*) {
    return E_NOTIMPL;
}

HRESULT MediaSource::GetService(REFGUID, REFIID, LPVOID* object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

HRESULT MediaSource::KsProperty(
    PKSPROPERTY,
    const ULONG,
    PVOID,
    const ULONG,
    ULONG* bytesReturned) {
    if (bytesReturned != nullptr) {
        *bytesReturned = 0;
    }
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

HRESULT MediaSource::KsMethod(
    PKSMETHOD,
    const ULONG,
    PVOID,
    const ULONG,
    ULONG* bytesReturned) {
    if (bytesReturned != nullptr) {
        *bytesReturned = 0;
    }
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

HRESULT MediaSource::KsEvent(
    PKSEVENT,
    const ULONG,
    PVOID,
    const ULONG,
    ULONG* bytesReturned) {
    if (bytesReturned != nullptr) {
        *bytesReturned = 0;
    }
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

HRESULT MediaSource::SetDefaultAllocator(
    const DWORD outputStreamId,
    IUnknown* allocator) {
    if (outputStreamId != 0 || allocator == nullptr) {
        return E_INVALIDARG;
    }
    ComPtr<IMFVideoSampleAllocator> videoAllocator;
    const HRESULT result = allocator->QueryInterface(IID_PPV_ARGS(&videoAllocator));
    if (FAILED(result)) {
        return result;
    }
    ComPtr<MediaStream> stream;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (FAILED(checkStateLocked()) || !stream_) {
            return MF_E_SHUTDOWN;
        }
        stream = stream_;
    }
    const HRESULT allocatorResult = stream->setSampleAllocator(videoAllocator.Get());
    if (SUCCEEDED(allocatorResult)) {
        shared_frame::reportFailure(609, S_OK);
    } else {
        shared_frame::reportFailure(601, allocatorResult);
    }
    return allocatorResult;
}

HRESULT MediaSource::GetAllocatorUsage(
    const DWORD outputStreamId,
    DWORD* inputStreamId,
    MFSampleAllocatorUsage* usage) {
    if (outputStreamId != 0) {
        return MF_E_INVALIDSTREAMNUMBER;
    }
    if (inputStreamId == nullptr || usage == nullptr) {
        return E_POINTER;
    }
    *inputStreamId = 0;
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

HRESULT MediaSource::checkStateLocked() const {
    return state_ == State::Shutdown || !eventQueue_ ? MF_E_SHUTDOWN : S_OK;
}

MediaSourceActivate::MediaSourceActivate() {
    ++ObjectCount;
    if (SUCCEEDED(MFCreateAttributes(&attributes_, 16))) {
        attributes_->SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1);
    }
}

MediaSourceActivate::~MediaSourceActivate() {
    --ObjectCount;
}

HRESULT MediaSourceActivate::QueryInterface(REFIID interfaceId, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (interfaceId == IID_IUnknown || interfaceId == __uuidof(IMFAttributes) ||
        interfaceId == __uuidof(IMFActivate)) {
        *object = static_cast<IMFActivate*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG MediaSourceActivate::AddRef() { return ++references_; }
ULONG MediaSourceActivate::Release() {
    const ULONG references = --references_;
    if (references == 0) { delete this; }
    return references;
}

#define FORWARD_ATTRIBUTES(method, ...) return attributes_ ? attributes_->method(__VA_ARGS__) : E_UNEXPECTED
HRESULT MediaSourceActivate::GetItem(REFGUID key, PROPVARIANT* value) { FORWARD_ATTRIBUTES(GetItem, key, value); }
HRESULT MediaSourceActivate::GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) { FORWARD_ATTRIBUTES(GetItemType, key, type); }
HRESULT MediaSourceActivate::CompareItem(REFGUID key, REFPROPVARIANT value, BOOL* result) { FORWARD_ATTRIBUTES(CompareItem, key, value, result); }
HRESULT MediaSourceActivate::Compare(IMFAttributes* attributes, MF_ATTRIBUTES_MATCH_TYPE matchType, BOOL* result) { FORWARD_ATTRIBUTES(Compare, attributes, matchType, result); }
HRESULT MediaSourceActivate::GetUINT32(REFGUID key, UINT32* value) { FORWARD_ATTRIBUTES(GetUINT32, key, value); }
HRESULT MediaSourceActivate::GetUINT64(REFGUID key, UINT64* value) { FORWARD_ATTRIBUTES(GetUINT64, key, value); }
HRESULT MediaSourceActivate::GetDouble(REFGUID key, double* value) { FORWARD_ATTRIBUTES(GetDouble, key, value); }
HRESULT MediaSourceActivate::GetGUID(REFGUID key, GUID* value) { FORWARD_ATTRIBUTES(GetGUID, key, value); }
HRESULT MediaSourceActivate::GetStringLength(REFGUID key, UINT32* length) { FORWARD_ATTRIBUTES(GetStringLength, key, length); }
HRESULT MediaSourceActivate::GetString(REFGUID key, LPWSTR value, UINT32 size, UINT32* length) { FORWARD_ATTRIBUTES(GetString, key, value, size, length); }
HRESULT MediaSourceActivate::GetAllocatedString(REFGUID key, LPWSTR* value, UINT32* length) { FORWARD_ATTRIBUTES(GetAllocatedString, key, value, length); }
HRESULT MediaSourceActivate::GetBlobSize(REFGUID key, UINT32* size) { FORWARD_ATTRIBUTES(GetBlobSize, key, size); }
HRESULT MediaSourceActivate::GetBlob(REFGUID key, UINT8* buffer, UINT32 bufferSize, UINT32* blobSize) { FORWARD_ATTRIBUTES(GetBlob, key, buffer, bufferSize, blobSize); }
HRESULT MediaSourceActivate::GetAllocatedBlob(REFGUID key, UINT8** buffer, UINT32* size) { FORWARD_ATTRIBUTES(GetAllocatedBlob, key, buffer, size); }
HRESULT MediaSourceActivate::GetUnknown(REFGUID key, REFIID interfaceId, LPVOID* object) { FORWARD_ATTRIBUTES(GetUnknown, key, interfaceId, object); }
HRESULT MediaSourceActivate::SetItem(REFGUID key, REFPROPVARIANT value) { FORWARD_ATTRIBUTES(SetItem, key, value); }
HRESULT MediaSourceActivate::DeleteItem(REFGUID key) { FORWARD_ATTRIBUTES(DeleteItem, key); }
HRESULT MediaSourceActivate::DeleteAllItems() { FORWARD_ATTRIBUTES(DeleteAllItems); }
HRESULT MediaSourceActivate::SetUINT32(REFGUID key, UINT32 value) { FORWARD_ATTRIBUTES(SetUINT32, key, value); }
HRESULT MediaSourceActivate::SetUINT64(REFGUID key, UINT64 value) { FORWARD_ATTRIBUTES(SetUINT64, key, value); }
HRESULT MediaSourceActivate::SetDouble(REFGUID key, double value) { FORWARD_ATTRIBUTES(SetDouble, key, value); }
HRESULT MediaSourceActivate::SetGUID(REFGUID key, REFGUID value) { FORWARD_ATTRIBUTES(SetGUID, key, value); }
HRESULT MediaSourceActivate::SetString(REFGUID key, LPCWSTR value) { FORWARD_ATTRIBUTES(SetString, key, value); }
HRESULT MediaSourceActivate::SetBlob(REFGUID key, const UINT8* buffer, UINT32 size) { FORWARD_ATTRIBUTES(SetBlob, key, buffer, size); }
HRESULT MediaSourceActivate::SetUnknown(REFGUID key, IUnknown* object) { FORWARD_ATTRIBUTES(SetUnknown, key, object); }
HRESULT MediaSourceActivate::LockStore() { FORWARD_ATTRIBUTES(LockStore); }
HRESULT MediaSourceActivate::UnlockStore() { FORWARD_ATTRIBUTES(UnlockStore); }
HRESULT MediaSourceActivate::GetCount(UINT32* count) { FORWARD_ATTRIBUTES(GetCount, count); }
HRESULT MediaSourceActivate::GetItemByIndex(UINT32 index, GUID* key, PROPVARIANT* value) { FORWARD_ATTRIBUTES(GetItemByIndex, index, key, value); }
HRESULT MediaSourceActivate::CopyAllItems(IMFAttributes* destination) { FORWARD_ATTRIBUTES(CopyAllItems, destination); }
#undef FORWARD_ATTRIBUTES

HRESULT MediaSourceActivate::ActivateObject(REFIID interfaceId, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    ComPtr<MediaSource> source;
    source.Attach(new (std::nothrow) MediaSource());
    if (!source) {
        return E_OUTOFMEMORY;
    }
    const HRESULT initializeResult = source->initialize(attributes_.Get());
    if (FAILED(initializeResult)) {
        return initializeResult;
    }
    activatedSource_.Swap(source);
    return activatedSource_->QueryInterface(interfaceId, object);
}

HRESULT MediaSourceActivate::ShutdownObject() {
    return S_OK;
}

HRESULT MediaSourceActivate::DetachObject() {
    std::lock_guard<std::mutex> lock(mutex_);
    activatedSource_.Reset();
    return S_OK;
}

ClassFactory::ClassFactory() { ++ObjectCount; }
ClassFactory::~ClassFactory() { --ObjectCount; }
HRESULT ClassFactory::QueryInterface(REFIID interfaceId, void** object) {
    if (object == nullptr) { return E_POINTER; }
    *object = nullptr;
    if (interfaceId == IID_IUnknown || interfaceId == IID_IClassFactory) {
        *object = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}
ULONG ClassFactory::AddRef() { return ++references_; }
ULONG ClassFactory::Release() {
    const ULONG references = --references_;
    if (references == 0) { delete this; }
    return references;
}
HRESULT ClassFactory::CreateInstance(IUnknown* outer, REFIID interfaceId, void** object) {
    if (outer != nullptr) { return CLASS_E_NOAGGREGATION; }
    auto* activation = new (std::nothrow) MediaSourceActivate();
    if (activation == nullptr) { return E_OUTOFMEMORY; }
    const HRESULT result = activation->QueryInterface(interfaceId, object);
    activation->Release();
    return result;
}
HRESULT ClassFactory::LockServer(const BOOL lock) {
    if (lock) { ++ServerLockCount; } else { --ServerLockCount; }
    return S_OK;
}

} // namespace wpc::virtual_camera
