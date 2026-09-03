#include "VirtualCameraIds.h"

#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <iostream>

namespace {

using Microsoft::WRL::ComPtr;
using GetClassObjectFunction = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

int fail(const char* operation, const HRESULT result) {
    std::cerr << operation << " failed: 0x" << std::hex
              << static_cast<unsigned long>(result) << '\n';
    return 1;
}

} // namespace

int wmain(const int argumentCount, wchar_t* arguments[]) {
    if (argumentCount != 2) {
        std::cerr << "Media source DLL path is required.\n";
        return 2;
    }
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        return fail("CoInitializeEx", comResult);
    }
    HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(result)) {
        if (uninitializeCom) {
            CoUninitialize();
        }
        return fail("MFStartup", result);
    }

    HMODULE module = LoadLibraryW(arguments[1]);
    if (module == nullptr) {
        result = HRESULT_FROM_WIN32(GetLastError());
        MFShutdown();
        if (uninitializeCom) {
            CoUninitialize();
        }
        return fail("LoadLibrary", result);
    }
    const auto getClassObject = reinterpret_cast<GetClassObjectFunction>(
        GetProcAddress(module, "DllGetClassObject"));
    if (getClassObject == nullptr) {
        result = HRESULT_FROM_WIN32(GetLastError());
        FreeLibrary(module);
        MFShutdown();
        if (uninitializeCom) {
            CoUninitialize();
        }
        return fail("GetProcAddress", result);
    }

    ComPtr<IClassFactory> factory;
    ComPtr<IMFActivate> activation;
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSampleAllocatorControl> allocatorControl;
    ComPtr<IMFVideoSampleAllocator> sampleAllocator;
    ComPtr<IMFPresentationDescriptor> presentationDescriptor;
    DWORD streamCount = 0;
    DWORD characteristics = 0;
    ComPtr<IMFMediaStream> mediaStream;
    const char* operation = "media source setup";
    result = getClassObject(
        wpc::virtual_camera_ids::MediaSourceClsid,
        IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) {
        result = factory->CreateInstance(nullptr, IID_PPV_ARGS(&activation));
    }
    if (SUCCEEDED(result)) {
        result = activation->ActivateObject(IID_PPV_ARGS(&source));
    }
    if (SUCCEEDED(result)) {
        result = source.As(&allocatorControl);
    }
    if (SUCCEEDED(result)) {
        result = MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&sampleAllocator));
    }
    if (SUCCEEDED(result)) {
        result = allocatorControl->SetDefaultAllocator(0, sampleAllocator.Get());
    }
    if (SUCCEEDED(result)) {
        result = source->GetCharacteristics(&characteristics);
    }
    if (SUCCEEDED(result)) {
        result = source->CreatePresentationDescriptor(&presentationDescriptor);
    }
    if (SUCCEEDED(result)) {
        result = presentationDescriptor->GetStreamDescriptorCount(&streamCount);
    }
    if (SUCCEEDED(result)) {
        result = presentationDescriptor->SelectStream(0);
    }
    PROPVARIANT startPosition;
    PropVariantInit(&startPosition);
    if (SUCCEEDED(result)) {
        operation = "media source Start";
        result = source->Start(presentationDescriptor.Get(), &GUID_NULL, &startPosition);
    }
    ComPtr<IMFMediaEvent> sourceEvent;
    if (SUCCEEDED(result)) {
        result = source->GetEvent(0, &sourceEvent);
    }
    PROPVARIANT eventValue;
    PropVariantInit(&eventValue);
    if (SUCCEEDED(result)) {
        MediaEventType eventType = MEUnknown;
        result = sourceEvent->GetType(&eventType);
        if (SUCCEEDED(result) && eventType != MENewStream) {
            result = E_UNEXPECTED;
        }
        if (SUCCEEDED(result)) {
            result = sourceEvent->GetValue(&eventValue);
        }
        if (SUCCEEDED(result) && eventValue.vt == VT_UNKNOWN) {
            result = eventValue.punkVal->QueryInterface(IID_PPV_ARGS(&mediaStream));
        } else if (SUCCEEDED(result)) {
            result = E_UNEXPECTED;
        }
    }
    PropVariantClear(&eventValue);
    sourceEvent.Reset();
    if (SUCCEEDED(result)) {
        result = source->GetEvent(0, &sourceEvent);
    }
    if (SUCCEEDED(result)) {
        MediaEventType eventType = MEUnknown;
        result = sourceEvent->GetType(&eventType);
        if (SUCCEEDED(result) && eventType != MESourceStarted) {
            result = E_UNEXPECTED;
        }
    }

    ComPtr<IMFMediaEvent> streamEvent;
    if (SUCCEEDED(result)) {
        result = mediaStream->GetEvent(0, &streamEvent);
    }
    if (SUCCEEDED(result)) {
        MediaEventType eventType = MEUnknown;
        result = streamEvent->GetType(&eventType);
        if (SUCCEEDED(result) && eventType != MEStreamStarted) {
            result = E_UNEXPECTED;
        }
    }
    if (SUCCEEDED(result)) {
        operation = "provided allocator RequestSample";
        result = mediaStream->RequestSample(nullptr);
    }
    streamEvent.Reset();
    if (SUCCEEDED(result)) {
        result = mediaStream->GetEvent(0, &streamEvent);
    }
    ComPtr<IMFSample> sample;
    if (SUCCEEDED(result)) {
        MediaEventType eventType = MEUnknown;
        result = streamEvent->GetType(&eventType);
        PropVariantInit(&eventValue);
        if (SUCCEEDED(result) && eventType != MEMediaSample) {
            result = E_UNEXPECTED;
        }
        if (SUCCEEDED(result)) {
            result = streamEvent->GetValue(&eventValue);
        }
        if (SUCCEEDED(result) && eventValue.vt == VT_UNKNOWN) {
            result = eventValue.punkVal->QueryInterface(IID_PPV_ARGS(&sample));
        } else if (SUCCEEDED(result)) {
            result = E_UNEXPECTED;
        }
        PropVariantClear(&eventValue);
    }
    if (SUCCEEDED(result)) {
        operation = "provided allocator sample buffer validation";
        ComPtr<IMFMediaBuffer> sampleBuffer;
        DWORD sampleBytes = 0;
        result = sample->ConvertToContiguousBuffer(&sampleBuffer);
        if (SUCCEEDED(result)) {
            result = sampleBuffer->GetCurrentLength(&sampleBytes);
        }
        if (SUCCEEDED(result) && sampleBytes == 0) {
            result = sampleBuffer->GetMaxLength(&sampleBytes);
        }
        if (SUCCEEDED(result) && sampleBytes == 0) {
            result = E_UNEXPECTED;
        }
    }
    if (SUCCEEDED(result)) {
        operation = "media source Stop";
        result = source->Stop();
    }
    if (source) {
        const HRESULT shutdownResult = source->Shutdown();
        if (SUCCEEDED(result) && FAILED(shutdownResult)) {
            result = shutdownResult;
        }
    }
    presentationDescriptor.Reset();
    sample.Reset();
    mediaStream.Reset();
    source.Reset();
    allocatorControl.Reset();
    sampleAllocator.Reset();

    ComPtr<IMFSourceReader> sourceReader;
    if (SUCCEEDED(result)) {
        operation = "source reader ActivateObject";
        result = activation->ActivateObject(IID_PPV_ARGS(&source));
    }
    if (SUCCEEDED(result)) {
        result = source.As(&allocatorControl);
    }
    if (SUCCEEDED(result)) {
        result = MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&sampleAllocator));
    }
    if (SUCCEEDED(result)) {
        result = allocatorControl->SetDefaultAllocator(0, sampleAllocator.Get());
    }
    if (SUCCEEDED(result)) {
        operation = "source reader creation";
        result = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &sourceReader);
    }
    const DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFMediaType> nativeType;
    if (SUCCEEDED(result)) {
        result = sourceReader->GetNativeMediaType(videoStream, 0, &nativeType);
    }
    if (SUCCEEDED(result)) {
        result = sourceReader->SetStreamSelection(
            static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    }
    if (SUCCEEDED(result)) {
        result = sourceReader->SetCurrentMediaType(videoStream, nullptr, nativeType.Get());
    }
    if (SUCCEEDED(result)) {
        result = sourceReader->SetStreamSelection(videoStream, TRUE);
    }
    DWORD actualStream = 0;
    DWORD streamFlags = 0;
    LONGLONG timestamp = 0;
    if (SUCCEEDED(result)) {
        operation = "source reader ReadSample";
        result = sourceReader->ReadSample(
            videoStream,
            0,
            &actualStream,
            &streamFlags,
            &timestamp,
            &sample);
    }
    if (SUCCEEDED(result) && !sample) {
        result = E_UNEXPECTED;
    }
    sourceReader.Reset();
    if (source) {
        const HRESULT shutdownResult = source->Shutdown();
        if (SUCCEEDED(result) && FAILED(shutdownResult)) {
            result = shutdownResult;
        }
    }
    if (activation) {
        activation->DetachObject();
    }
    sample.Reset();
    nativeType.Reset();
    sampleAllocator.Reset();
    allocatorControl.Reset();
    source.Reset();
    activation.Reset();
    factory.Reset();
    FreeLibrary(module);
    MFShutdown();
    if (uninitializeCom) {
        CoUninitialize();
    }

    if (FAILED(result)) {
        return fail(operation, result);
    }
    if (streamCount != 1 || (characteristics & MFMEDIASOURCE_IS_LIVE) == 0) {
        std::cerr << "Unexpected media source metadata.\n";
        return 1;
    }
    std::cout << "Media source activation succeeded.\n";
    return 0;
}
