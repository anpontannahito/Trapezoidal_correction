#pragma once

#include <guiddef.h>

namespace wpc::virtual_camera_ids {

// {9A5E0A9A-5108-47AC-8AF1-6B8244F18C73}
inline constexpr GUID MediaSourceClsid = {
    0x9a5e0a9a,
    0x5108,
    0x47ac,
    {0x8a, 0xf1, 0x6b, 0x82, 0x44, 0xf1, 0x8c, 0x73}};

inline constexpr wchar_t MediaSourceClsidString[] =
    L"{9A5E0A9A-5108-47AC-8AF1-6B8244F18C73}";
inline constexpr wchar_t FriendlyName[] = L"Perspective Camera";
inline constexpr wchar_t MediaSourceDllName[] = L"PerspectiveCameraMediaSource.dll";

} // namespace wpc::virtual_camera_ids

