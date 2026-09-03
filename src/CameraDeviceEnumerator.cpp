#include "CameraDeviceEnumerator.h"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <tuple>

namespace wpc {

namespace {

using Microsoft::WRL::ComPtr;

std::string wideToUtf8(const wchar_t* value) {
    if (value == nullptr || *value == L'\0') {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(length - 1));
    return result;
}

std::string subtypeName(const GUID& subtype) {
    if (subtype == MFVideoFormat_NV12) {
        return "NV12";
    }
    if (subtype == MFVideoFormat_YUY2) {
        return "YUY2";
    }
    if (subtype == MFVideoFormat_MJPG) {
        return "MJPG";
    }
    if (subtype == MFVideoFormat_RGB32) {
        return "BGRA32";
    }
    if (subtype == MFVideoFormat_I420) {
        return "I420";
    }
    if (subtype == MFVideoFormat_H264) {
        return "H264";
    }
    return "Other";
}

bool sameMode(const CameraMode& left, const CameraMode& right) {
    return std::tie(left.width,
                    left.height,
                    left.frameRateNumerator,
                    left.frameRateDenominator,
                    left.pixelFormat) ==
           std::tie(right.width,
                    right.height,
                    right.frameRateNumerator,
                    right.frameRateDenominator,
                    right.pixelFormat);
}

} // namespace

double CameraMode::framesPerSecond() const noexcept {
    return frameRateDenominator == 0
               ? 0.0
               : static_cast<double>(frameRateNumerator) /
                     static_cast<double>(frameRateDenominator);
}

std::string CameraMode::displayName() const {
    std::ostringstream stream;
    stream << width << 'x' << height << " @ " << std::fixed << std::setprecision(2)
           << framesPerSecond() << " FPS / " << pixelFormat;
    return stream.str();
}

std::vector<CameraDeviceInfo> CameraDeviceEnumerator::enumerate() {
    std::vector<CameraDeviceInfo> devices;
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        return devices;
    }
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        if (uninitializeCom) {
            CoUninitialize();
        }
        return devices;
    }

    ComPtr<IMFAttributes> attributes;
    IMFActivate** activations = nullptr;
    UINT32 activationCount = 0;
    if (SUCCEEDED(MFCreateAttributes(&attributes, 1)) &&
        SUCCEEDED(attributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)) &&
        SUCCEEDED(MFEnumDeviceSources(attributes.Get(), &activations, &activationCount))) {
        for (UINT32 deviceIndex = 0; deviceIndex < activationCount; ++deviceIndex) {
            CameraDeviceInfo device;
            device.captureIndex = static_cast<int>(deviceIndex);
            wchar_t* friendlyName = nullptr;
            UINT32 friendlyNameLength = 0;
            if (SUCCEEDED(activations[deviceIndex]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                    &friendlyName,
                    &friendlyNameLength))) {
                device.friendlyName = wideToUtf8(friendlyName);
                CoTaskMemFree(friendlyName);
            }

            wchar_t* symbolicLink = nullptr;
            UINT32 symbolicLinkLength = 0;
            if (SUCCEEDED(activations[deviceIndex]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                    &symbolicLink,
                    &symbolicLinkLength))) {
                device.symbolicLink.assign(symbolicLink);
                CoTaskMemFree(symbolicLink);
            }

            ComPtr<IMFMediaSource> source;
            ComPtr<IMFSourceReader> reader;
            if (SUCCEEDED(activations[deviceIndex]->ActivateObject(IID_PPV_ARGS(&source))) &&
                SUCCEEDED(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader))) {
                for (DWORD modeIndex = 0;; ++modeIndex) {
                    ComPtr<IMFMediaType> mediaType;
                    const HRESULT result = reader->GetNativeMediaType(
                        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                        modeIndex,
                        &mediaType);
                    if (result == MF_E_NO_MORE_TYPES) {
                        break;
                    }
                    if (FAILED(result)) {
                        continue;
                    }

                    GUID majorType = GUID_NULL;
                    GUID subtype = GUID_NULL;
                    UINT32 width = 0;
                    UINT32 height = 0;
                    UINT32 numerator = 0;
                    UINT32 denominator = 1;
                    if (FAILED(mediaType->GetGUID(MF_MT_MAJOR_TYPE, &majorType)) ||
                        majorType != MFMediaType_Video ||
                        FAILED(mediaType->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
                        FAILED(MFGetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
                        FAILED(MFGetAttributeRatio(
                            mediaType.Get(),
                            MF_MT_FRAME_RATE,
                            &numerator,
                            &denominator)) ||
                        width == 0 || height == 0 || numerator == 0 || denominator == 0) {
                        continue;
                    }
                    device.modes.push_back(
                        {width, height, numerator, denominator, subtypeName(subtype)});
                }
            }
            if (source) {
                source->Shutdown();
            }

            std::sort(device.modes.begin(), device.modes.end(), [](const CameraMode& left, const CameraMode& right) {
                const auto leftPixels = static_cast<std::uint64_t>(left.width) * left.height;
                const auto rightPixels = static_cast<std::uint64_t>(right.width) * right.height;
                if (leftPixels != rightPixels) {
                    return leftPixels > rightPixels;
                }
                if (std::abs(left.framesPerSecond() - right.framesPerSecond()) > 0.001) {
                    return left.framesPerSecond() > right.framesPerSecond();
                }
                return left.pixelFormat < right.pixelFormat;
            });
            device.modes.erase(
                std::unique(device.modes.begin(), device.modes.end(), sameMode),
                device.modes.end());
            devices.push_back(std::move(device));
        }
    }

    if (activations != nullptr) {
        for (UINT32 index = 0; index < activationCount; ++index) {
            activations[index]->Release();
        }
        CoTaskMemFree(activations);
    }
    MFShutdown();
    if (uninitializeCom) {
        CoUninitialize();
    }
    return devices;
}

} // namespace wpc
