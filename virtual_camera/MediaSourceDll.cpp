#include "MediaSource.h"

#include "VirtualCameraIds.h"

#include <Windows.h>

#include <atomic>
#include <new>
#include <string>

namespace wpc::virtual_camera {

std::atomic<long> ObjectCount{0};
std::atomic<long> ServerLockCount{0};
HMODULE Module = nullptr;

} // namespace wpc::virtual_camera

namespace {

HRESULT registerComServer() {
    wchar_t modulePath[32768]{};
    if (GetModuleFileNameW(wpc::virtual_camera::Module, modulePath, _countof(modulePath)) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    const std::wstring keyPath =
        std::wstring(L"Software\\Classes\\CLSID\\") +
        wpc::virtual_camera_ids::MediaSourceClsidString + L"\\InprocServer32";
    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        keyPath.c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    if (result != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(result);
    }
    const DWORD pathBytes = static_cast<DWORD>((wcslen(modulePath) + 1) * sizeof(wchar_t));
    result = RegSetValueExW(
        key,
        nullptr,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(modulePath),
        pathBytes);
    if (result == ERROR_SUCCESS) {
        constexpr wchar_t threadingModel[] = L"Both";
        result = RegSetValueExW(
            key,
            L"ThreadingModel",
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(threadingModel),
            sizeof(threadingModel));
    }
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(result);
}

HRESULT unregisterComServer() {
    const std::wstring keyPath =
        std::wstring(L"Software\\Classes\\CLSID\\") +
        wpc::virtual_camera_ids::MediaSourceClsidString;
    const LONG result = RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath.c_str());
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND
               ? S_OK
               : HRESULT_FROM_WIN32(result);
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        wpc::virtual_camera::Module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow() {
    return wpc::virtual_camera::ObjectCount.load() == 0 &&
                   wpc::virtual_camera::ServerLockCount.load() == 0
               ? S_OK
               : S_FALSE;
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(
    REFCLSID classId,
    REFIID interfaceId,
    void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (classId != wpc::virtual_camera_ids::MediaSourceClsid) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* factory = new (std::nothrow) wpc::virtual_camera::ClassFactory();
    if (factory == nullptr) {
        return E_OUTOFMEMORY;
    }
    const HRESULT result = factory->QueryInterface(interfaceId, object);
    factory->Release();
    return result;
}

extern "C" HRESULT STDAPICALLTYPE DllRegisterServer() {
    return registerComServer();
}

extern "C" HRESULT STDAPICALLTYPE DllUnregisterServer() {
    return unregisterComServer();
}
