#pragma once

#include "SharedFrameReader.h"

#include <Windows.h>
#include <mfidl.h>
#include <ks.h>
#include <ksproxy.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace wpc::virtual_camera {

extern std::atomic<long> ObjectCount;
extern std::atomic<long> ServerLockCount;

class MediaSource;

class MediaStream final : public IMFMediaStream2 {
public:
    MediaStream(
        IMFMediaSource* source,
        IMFStreamDescriptor* descriptor,
        IMFMediaType* mediaType,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t frameRateNumerator,
        std::uint32_t frameRateDenominator,
        std::uint32_t dataBytes);

    HRESULT initialize();
    HRESULT start(IMFMediaType* mediaType);
    HRESULT stop();
    HRESULT shutdown();
    HRESULT getAttributes(IMFAttributes** attributes);
    HRESULT setSampleAllocator(IMFVideoSampleAllocator* allocator);

    STDMETHODIMP QueryInterface(REFIID interfaceId, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(
        MediaEventType eventType,
        REFGUID extendedType,
        HRESULT status,
        const PROPVARIANT* value) override;

    STDMETHODIMP GetMediaSource(IMFMediaSource** mediaSource) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** streamDescriptor) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;
    STDMETHODIMP SetStreamState(MF_STREAM_STATE value) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* value) override;

private:
    ~MediaStream();
    HRESULT checkStateLocked() const;
    HRESULT initializeAllocatorLocked();
    void fillBlackFrame(std::uint8_t* destination) const;

    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    Microsoft::WRL::ComPtr<IMFMediaSource> source_;
    Microsoft::WRL::ComPtr<IMFStreamDescriptor> descriptor_;
    Microsoft::WRL::ComPtr<IMFMediaType> mediaType_;
    Microsoft::WRL::ComPtr<IMFMediaEventQueue> eventQueue_;
    Microsoft::WRL::ComPtr<IMFAttributes> attributes_;
    Microsoft::WRL::ComPtr<IMFVideoSampleAllocator> sampleAllocator_;
    SharedFrameReader frameReader_;
    std::vector<std::uint8_t> packedFrame_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t frameRateNumerator_;
    std::uint32_t frameRateDenominator_;
    std::uint32_t dataBytes_;
    LONGLONG sampleDuration_;
    LONGLONG nextSampleTime_ = 0;
    bool started_ = false;
    bool shutdown_ = false;
    bool allocatorInitialized_ = false;
    bool firstSampleDiagnosticCompleted_ = false;
    MF_STREAM_STATE streamState_ = MF_STREAM_STATE_STOPPED;
};

class MediaSource final : public IMFMediaSourceEx,
                          public IMFGetService,
                          public IKsControl,
                          public IMFSampleAllocatorControl {
public:
    MediaSource();
    HRESULT initialize(IMFAttributes* activationAttributes);

    STDMETHODIMP QueryInterface(REFIID interfaceId, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(
        MediaEventType eventType,
        REFGUID extendedType,
        HRESULT status,
        const PROPVARIANT* value) override;

    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** presentationDescriptor) override;
    STDMETHODIMP Start(
        IMFPresentationDescriptor* presentationDescriptor,
        const GUID* timeFormat,
        const PROPVARIANT* startPosition) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;

    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD streamIdentifier, IMFAttributes** attributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;

    STDMETHODIMP GetService(REFGUID service, REFIID interfaceId, LPVOID* object) override;

    STDMETHODIMP KsProperty(
        PKSPROPERTY property,
        ULONG propertyLength,
        PVOID propertyData,
        ULONG dataLength,
        ULONG* bytesReturned) override;
    STDMETHODIMP KsMethod(
        PKSMETHOD method,
        ULONG methodLength,
        PVOID methodData,
        ULONG dataLength,
        ULONG* bytesReturned) override;
    STDMETHODIMP KsEvent(
        PKSEVENT event,
        ULONG eventLength,
        PVOID eventData,
        ULONG dataLength,
        ULONG* bytesReturned) override;

    STDMETHODIMP SetDefaultAllocator(DWORD outputStreamId, IUnknown* allocator) override;
    STDMETHODIMP GetAllocatorUsage(
        DWORD outputStreamId,
        DWORD* inputStreamId,
        MFSampleAllocatorUsage* usage) override;

private:
    ~MediaSource();
    HRESULT checkStateLocked() const;

    enum class State {
        Stopped,
        Started,
        Shutdown
    };

    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    State state_ = State::Stopped;
    Microsoft::WRL::ComPtr<IMFMediaEventQueue> eventQueue_;
    Microsoft::WRL::ComPtr<IMFAttributes> sourceAttributes_;
    Microsoft::WRL::ComPtr<IMFPresentationDescriptor> presentationDescriptor_;
    Microsoft::WRL::ComPtr<IMFStreamDescriptor> streamDescriptor_;
    Microsoft::WRL::ComPtr<MediaStream> stream_;
};

class MediaSourceActivate final : public IMFActivate {
public:
    MediaSourceActivate();

    STDMETHODIMP QueryInterface(REFIID interfaceId, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetItem(REFGUID key, PROPVARIANT* value) override;
    STDMETHODIMP GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) override;
    STDMETHODIMP CompareItem(REFGUID key, REFPROPVARIANT value, BOOL* result) override;
    STDMETHODIMP Compare(IMFAttributes* attributes, MF_ATTRIBUTES_MATCH_TYPE matchType, BOOL* result) override;
    STDMETHODIMP GetUINT32(REFGUID key, UINT32* value) override;
    STDMETHODIMP GetUINT64(REFGUID key, UINT64* value) override;
    STDMETHODIMP GetDouble(REFGUID key, double* value) override;
    STDMETHODIMP GetGUID(REFGUID key, GUID* value) override;
    STDMETHODIMP GetStringLength(REFGUID key, UINT32* length) override;
    STDMETHODIMP GetString(REFGUID key, LPWSTR value, UINT32 size, UINT32* length) override;
    STDMETHODIMP GetAllocatedString(REFGUID key, LPWSTR* value, UINT32* length) override;
    STDMETHODIMP GetBlobSize(REFGUID key, UINT32* size) override;
    STDMETHODIMP GetBlob(REFGUID key, UINT8* buffer, UINT32 bufferSize, UINT32* blobSize) override;
    STDMETHODIMP GetAllocatedBlob(REFGUID key, UINT8** buffer, UINT32* size) override;
    STDMETHODIMP GetUnknown(REFGUID key, REFIID interfaceId, LPVOID* object) override;
    STDMETHODIMP SetItem(REFGUID key, REFPROPVARIANT value) override;
    STDMETHODIMP DeleteItem(REFGUID key) override;
    STDMETHODIMP DeleteAllItems() override;
    STDMETHODIMP SetUINT32(REFGUID key, UINT32 value) override;
    STDMETHODIMP SetUINT64(REFGUID key, UINT64 value) override;
    STDMETHODIMP SetDouble(REFGUID key, double value) override;
    STDMETHODIMP SetGUID(REFGUID key, REFGUID value) override;
    STDMETHODIMP SetString(REFGUID key, LPCWSTR value) override;
    STDMETHODIMP SetBlob(REFGUID key, const UINT8* buffer, UINT32 size) override;
    STDMETHODIMP SetUnknown(REFGUID key, IUnknown* object) override;
    STDMETHODIMP LockStore() override;
    STDMETHODIMP UnlockStore() override;
    STDMETHODIMP GetCount(UINT32* count) override;
    STDMETHODIMP GetItemByIndex(UINT32 index, GUID* key, PROPVARIANT* value) override;
    STDMETHODIMP CopyAllItems(IMFAttributes* destination) override;

    STDMETHODIMP ActivateObject(REFIID interfaceId, void** object) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

private:
    ~MediaSourceActivate();

    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    Microsoft::WRL::ComPtr<IMFAttributes> attributes_;
    Microsoft::WRL::ComPtr<MediaSource> activatedSource_;
};

class ClassFactory final : public IClassFactory {
public:
    ClassFactory();

    STDMETHODIMP QueryInterface(REFIID interfaceId, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID interfaceId, void** object) override;
    STDMETHODIMP LockServer(BOOL lock) override;

private:
    ~ClassFactory();
    std::atomic<ULONG> references_{1};
};

} // namespace wpc::virtual_camera
