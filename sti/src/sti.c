/* sti.dll replacement for running SilverFast under Wine.
 *
 * Wine's own sti.dll returns an empty device list, so SilverFast never sees
 * a scanner. This implementation reads the still-image devices that the
 * installer registered under the Windows "Image" device class, and forwards
 * IStiDevice raw I/O to silverfast-usbd (see transport.c).
 *
 * Details SilverFast depends on (found while reverse engineering, see
 * docs/how-it-works.md):
 *   - STI_DEVICE_INFORMATIONW.dwSize must be 0x138 (the x64 struct size)
 *   - DeviceType carries the device type in the high word and the subtype in
 *     the low word, e.g. 0x00010001 for a flatbed scanner
 *   - the plain "StiCreateInstance" export must exist (looked up by name)
 *   - all scanner traffic uses RawWriteData/RawReadData between
 *     LockDevice/UnLockDevice
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <objbase.h>
#include <sti.h>
#include <stdlib.h>

#include "log.h"
#include "transport.h"

#define IMAGE_CLASS_GUID L"{6BDD1FC6-810F-11D0-BEC7-08002BE2092F}"
#define IMAGE_CLASS_KEY  L"SYSTEM\\CurrentControlSet\\Control\\Class\\" IMAGE_CLASS_GUID

_Static_assert(sizeof(void *) != 8 || sizeof(STI_DEVICE_INFORMATIONW) == 0x138,
               "SilverFast checks dwSize == 0x138 on x64");

static HINSTANCE sti_instance;
static LONG object_count;

/* ------------------------------------------------------------------------ */
/* Registry-backed device database                                          */
/* ------------------------------------------------------------------------ */

struct device_record {
    WCHAR  internal_name[STI_MAX_INTERNAL_NAME_LENGTH];
    WCHAR  key_path[MAX_PATH];
    DWORD  type, subtype, caps, hw_config;
    WCHAR  vendor[128], description[128], port[MAX_PATH], local_name[128];
};

static void read_sz(HKEY key, const WCHAR *name, WCHAR *out, DWORD chars)
{
    DWORD size = chars * sizeof(WCHAR), type;
    out[0] = 0;
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)out, &size) || type != REG_SZ) out[0] = 0;
    out[chars - 1] = 0;
}

static DWORD read_dword(HKEY key, const WCHAR *name, DWORD def)
{
    DWORD value, size = sizeof(value), type;
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&value, &size) || type != REG_DWORD) return def;
    return value;
}

/* Fills up to max records; returns the number of still-image devices found. */
static DWORD enum_devices(struct device_record *recs, DWORD max)
{
    HKEY cls, dev;
    WCHAR sub[16], subclass[32];
    DWORD i, count = 0, len;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, IMAGE_CLASS_KEY, 0, KEY_READ, &cls)) return 0;
    for (i = 0; len = ARRAYSIZE(sub), !RegEnumKeyExW(cls, i, sub, &len, NULL, NULL, NULL, NULL); i++)
    {
        struct device_record *r;

        if (RegOpenKeyExW(cls, sub, 0, KEY_READ, &dev)) continue;
        read_sz(dev, L"SubClass", subclass, ARRAYSIZE(subclass));
        if (lstrcmpiW(subclass, L"StillImage") || count >= max) { RegCloseKey(dev); continue; }

        r = &recs[count++];
        memset(r, 0, sizeof(*r));
        wsprintfW(r->internal_name, L"%s\\%s", IMAGE_CLASS_GUID, sub);
        wsprintfW(r->key_path, L"%s\\%s", IMAGE_CLASS_KEY, sub);
        r->type      = read_dword(dev, L"DeviceType", StiDeviceTypeScanner);
        r->subtype   = read_dword(dev, L"DeviceSubType", 1);
        r->caps      = read_dword(dev, L"Capabilities", 0);
        r->hw_config = read_dword(dev, L"HardwareConfig", STI_HW_CONFIG_USB);
        read_sz(dev, L"Vendor", r->vendor, ARRAYSIZE(r->vendor));
        read_sz(dev, L"FriendlyName", r->description, ARRAYSIZE(r->description));
        if (!r->description[0]) read_sz(dev, L"DriverDesc", r->description, ARRAYSIZE(r->description));
        read_sz(dev, L"CreateFileName", r->port, ARRAYSIZE(r->port));
        lstrcpyW(r->local_name, r->description);
        RegCloseKey(dev);
    }
    RegCloseKey(cls);
    return count;
}

static BOOL find_device(const WCHAR *name, struct device_record *out)
{
    struct device_record recs[16];
    DWORD i, n = enum_devices(recs, ARRAYSIZE(recs));

    for (i = 0; i < n; i++)
        if (name && !lstrcmpiW(recs[i].internal_name, name)) { *out = recs[i]; return TRUE; }
    return FALSE;
}

static DWORD str_bytes(const WCHAR *s) { return (lstrlenW(s) + 1) * sizeof(WCHAR); }

/* Builds STI_DEVICE_INFORMATIONW entries plus their strings in one LocalAlloc
 * block, which is how callers expect to free them (LocalFree). */
static STI_DEVICE_INFORMATIONW *build_info(const struct device_record *recs, DWORD count)
{
    static const WCHAR empty[] = L"";
    DWORD i, size = count * sizeof(STI_DEVICE_INFORMATIONW);
    STI_DEVICE_INFORMATIONW *info;
    BYTE *strings;

    for (i = 0; i < count; i++)
        size += str_bytes(recs[i].vendor) + str_bytes(recs[i].description) + str_bytes(recs[i].port)
              + str_bytes(empty) + str_bytes(recs[i].local_name);
    if (!(info = LocalAlloc(LPTR, size ? size : 1))) return NULL;
    strings = (BYTE *)(info + count);

#define PUT(field, src) do { DWORD b = str_bytes(src); memcpy(strings, src, b); \
                             info[i].field = (WCHAR *)strings; strings += b; } while (0)
    for (i = 0; i < count; i++)
    {
        info[i].dwSize = sizeof(STI_DEVICE_INFORMATIONW);
        info[i].DeviceType = MAKELONG(recs[i].subtype, recs[i].type);
        lstrcpynW(info[i].szDeviceInternalName, recs[i].internal_name, STI_MAX_INTERNAL_NAME_LENGTH);
        info[i].DeviceCapabilities.dwGeneric = recs[i].caps;
        info[i].dwHardwareConfiguration = recs[i].hw_config;
        PUT(pszVendorDescription, recs[i].vendor);
        PUT(pszDeviceDescription, recs[i].description);
        PUT(pszPortName, recs[i].port);
        PUT(pszPropProvider, empty);
        PUT(pszLocalName, recs[i].local_name);
    }
#undef PUT
    return info;
}

/* Looks a value up on the device key, then in its DeviceData subkey. */
static LONG query_device_value(const struct device_record *rec, const WCHAR *value, DWORD *type, BYTE *data, DWORD *size)
{
    HKEY key, data_key;
    LONG ret;

    if ((ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE, rec->key_path, 0, KEY_READ, &key))) return ret;
    ret = RegQueryValueExW(key, value, NULL, type, data, size);
    if (ret == ERROR_FILE_NOT_FOUND && !RegOpenKeyExW(key, L"DeviceData", 0, KEY_READ, &data_key))
    {
        ret = RegQueryValueExW(data_key, value, NULL, type, data, size);
        RegCloseKey(data_key);
    }
    RegCloseKey(key);
    return ret;
}

/* ------------------------------------------------------------------------ */
/* IStiDevice                                                               */
/* ------------------------------------------------------------------------ */

struct sti_device {
    IStiDevice IStiDevice_iface;
    LONG       ref;
    struct device_record rec;
    HANDLE     mutex;      /* cross-process device lock, like the STI service provides */
    BOOL       locked;
    DWORD      last_error;
};

static struct sti_device *impl_from_IStiDevice(IStiDevice *iface)
{
    return CONTAINING_RECORD(iface, struct sti_device, IStiDevice_iface);
}

static HRESULT WINAPI device_QueryInterface(IStiDevice *iface, REFIID riid, void **out)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IStiDevice))
    {
        *out = iface;
        IStiDevice_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI device_AddRef(IStiDevice *iface)
{
    return InterlockedIncrement(&impl_from_IStiDevice(iface)->ref);
}

static ULONG WINAPI device_Release(IStiDevice *iface)
{
    struct sti_device *dev = impl_from_IStiDevice(iface);
    ULONG ref = InterlockedDecrement(&dev->ref);

    if (!ref)
    {
        if (dev->locked) ReleaseMutex(dev->mutex);
        CloseHandle(dev->mutex);
        free(dev);
        InterlockedDecrement(&object_count);
    }
    return ref;
}

static HRESULT WINAPI device_Initialize(IStiDevice *iface, HINSTANCE inst, LPCWSTR name, DWORD version, DWORD mode)
{
    return S_OK;
}

static HRESULT WINAPI device_GetCapabilities(IStiDevice *iface, PSTI_DEV_CAPS caps)
{
    caps->dwGeneric = impl_from_IStiDevice(iface)->rec.caps;
    return S_OK;
}

static HRESULT WINAPI device_GetStatus(IStiDevice *iface, PSTI_DEVICE_STATUS status)
{
    HRESULT hr = transport_connect();

    status->dwOnlineState = SUCCEEDED(hr) ? STI_ONLINESTATE_OPERATIONAL : STI_ONLINESTATE_OFFLINE;
    status->dwHardwareStatusCode = 0;
    status->dwEventHandlingState = 0;
    status->dwPollingInterval = 0;
    return S_OK;
}

static HRESULT WINAPI device_DeviceReset(IStiDevice *iface)
{
    return S_OK;
}

static HRESULT WINAPI device_Diagnostic(IStiDevice *iface, LPSTI_DIAG diag)
{
    HRESULT hr = transport_connect();
    diag->sErrorInfo.dwGenericError = hr;
    diag->sErrorInfo.dwVendorError = 0;
    return S_OK;
}

static HRESULT WINAPI device_Escape(IStiDevice *iface, STI_RAW_CONTROL_CODE code, LPVOID in, DWORD in_size,
                                   LPVOID out, DWORD out_size, LPDWORD actual)
{
    sfl_log("Escape(0x%lx) is not supported", code);
    return STIERR_UNSUPPORTED;
}

static HRESULT WINAPI device_GetLastError(IStiDevice *iface, LPDWORD err)
{
    *err = impl_from_IStiDevice(iface)->last_error;
    return S_OK;
}

static HRESULT WINAPI device_LockDevice(IStiDevice *iface, DWORD timeout)
{
    struct sti_device *dev = impl_from_IStiDevice(iface);
    DWORD wait;

    if (dev->locked) return S_OK;
    wait = WaitForSingleObject(dev->mutex, timeout);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return STIERR_DEVICE_LOCKED;
    dev->locked = TRUE;
    return S_OK;
}

static HRESULT WINAPI device_UnLockDevice(IStiDevice *iface)
{
    struct sti_device *dev = impl_from_IStiDevice(iface);

    if (!dev->locked) return STIERR_NEEDS_LOCK;
    dev->locked = FALSE;
    ReleaseMutex(dev->mutex);
    return S_OK;
}

/* Raw I/O is synchronous; an OVERLAPPED, if given, is completed before returning. */
static void complete_overlapped(LPOVERLAPPED ov, HRESULT hr, DWORD bytes)
{
    if (!ov) return;
    ov->Internal = SUCCEEDED(hr) ? 0 : (ULONG_PTR)hr;
    ov->InternalHigh = bytes;
    if (ov->hEvent) SetEvent(ov->hEvent);
}

static HRESULT raw_read(struct sti_device *dev, LPVOID buf, LPDWORD len, LPOVERLAPPED ov)
{
    HRESULT hr = transport_read(buf, len);
    if (FAILED(hr)) dev->last_error = hr;
    complete_overlapped(ov, hr, *len);
    return hr;
}

static HRESULT raw_write(struct sti_device *dev, LPVOID buf, DWORD len, LPOVERLAPPED ov)
{
    HRESULT hr = transport_write(buf, len);
    if (FAILED(hr)) dev->last_error = hr;
    complete_overlapped(ov, hr, SUCCEEDED(hr) ? len : 0);
    return hr;
}

static HRESULT WINAPI device_RawReadData(IStiDevice *iface, LPVOID buf, LPDWORD len, LPOVERLAPPED ov)
{
    return raw_read(impl_from_IStiDevice(iface), buf, len, ov);
}

static HRESULT WINAPI device_RawWriteData(IStiDevice *iface, LPVOID buf, DWORD len, LPOVERLAPPED ov)
{
    return raw_write(impl_from_IStiDevice(iface), buf, len, ov);
}

static HRESULT WINAPI device_RawReadCommand(IStiDevice *iface, LPVOID buf, LPDWORD len, LPOVERLAPPED ov)
{
    return raw_read(impl_from_IStiDevice(iface), buf, len, ov);
}

static HRESULT WINAPI device_RawWriteCommand(IStiDevice *iface, LPVOID buf, DWORD len, LPOVERLAPPED ov)
{
    return raw_write(impl_from_IStiDevice(iface), buf, len, ov);
}

static HRESULT WINAPI device_Subscribe(IStiDevice *iface, LPSTISUBSCRIBE sub)
{
    return STIERR_UNSUPPORTED;
}

static HRESULT WINAPI device_GetLastNotificationData(IStiDevice *iface, LPSTINOTIFY notify)
{
    return STIERR_NOEVENTS;
}

static HRESULT WINAPI device_UnSubscribe(IStiDevice *iface)
{
    return S_OK;
}

static HRESULT WINAPI device_GetLastErrorInfo(IStiDevice *iface, STI_ERROR_INFO *info)
{
    memset(info, 0, sizeof(*info));
    info->dwGenericError = impl_from_IStiDevice(iface)->last_error;
    return S_OK;
}

static IStiDeviceVtbl device_vtbl = {
    device_QueryInterface, device_AddRef, device_Release, device_Initialize, device_GetCapabilities,
    device_GetStatus, device_DeviceReset, device_Diagnostic, device_Escape, device_GetLastError,
    device_LockDevice, device_UnLockDevice, device_RawReadData, device_RawWriteData,
    device_RawReadCommand, device_RawWriteCommand, device_Subscribe, device_GetLastNotificationData,
    device_UnSubscribe, device_GetLastErrorInfo,
};

static HRESULT create_device(const WCHAR *name, IStiDevice **out)
{
    struct sti_device *dev;
    WCHAR mutex_name[STI_MAX_INTERNAL_NAME_LENGTH + 16], *p;

    *out = NULL;
    if (!(dev = calloc(1, sizeof(*dev)))) return E_OUTOFMEMORY;
    if (!find_device(name, &dev->rec)) { free(dev); return STIERR_INVALID_DEVICE_NAME; }

    wsprintfW(mutex_name, L"sfl-sti-%s", dev->rec.internal_name);
    for (p = mutex_name; *p; p++) if (*p == '\\') *p = '_';
    dev->mutex = CreateMutexW(NULL, FALSE, mutex_name);
    dev->IStiDevice_iface.lpVtbl = &device_vtbl;
    dev->ref = 1;
    InterlockedIncrement(&object_count);
    sfl_log("CreateDevice %ls", dev->rec.internal_name);
    *out = &dev->IStiDevice_iface;
    return S_OK;
}

/* ------------------------------------------------------------------------ */
/* IStillImageW                                                             */
/* ------------------------------------------------------------------------ */

struct still_image {
    IStillImageW IStillImageW_iface;
    LONG ref;
};

static struct still_image *impl_from_IStillImageW(IStillImageW *iface)
{
    return CONTAINING_RECORD(iface, struct still_image, IStillImageW_iface);
}

static HRESULT WINAPI sti_QueryInterface(IStillImageW *iface, REFIID riid, void **out)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IStillImageW))
    {
        *out = iface;
        iface->lpVtbl->AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI sti_AddRef(IStillImageW *iface)
{
    return InterlockedIncrement(&impl_from_IStillImageW(iface)->ref);
}

static ULONG WINAPI sti_Release(IStillImageW *iface)
{
    struct still_image *sti = impl_from_IStillImageW(iface);
    ULONG ref = InterlockedDecrement(&sti->ref);

    if (!ref)
    {
        free(sti);
        InterlockedDecrement(&object_count);
    }
    return ref;
}

static HRESULT WINAPI sti_Initialize(IStillImageW *iface, HINSTANCE inst, DWORD version)
{
    return S_OK;
}

static HRESULT WINAPI sti_GetDeviceList(IStillImageW *iface, DWORD type, DWORD flags, DWORD *count, LPVOID *buffer)
{
    struct device_record recs[16], *matching = recs;
    DWORD i, n = enum_devices(recs, ARRAYSIZE(recs)), kept = 0;

    for (i = 0; i < n; i++)
        if (type == StiDeviceTypeDefault || recs[i].type == type) matching[kept++] = recs[i];
    if (!(*buffer = build_info(matching, kept))) return E_OUTOFMEMORY;
    *count = kept;
    return S_OK;
}

static HRESULT WINAPI sti_GetDeviceInfo(IStillImageW *iface, LPWSTR name, LPVOID *buffer)
{
    struct device_record rec;

    *buffer = NULL;
    if (!find_device(name, &rec)) return STIERR_INVALID_DEVICE_NAME;
    return (*buffer = build_info(&rec, 1)) ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI sti_CreateDevice(IStillImageW *iface, LPWSTR name, DWORD mode, PSTIDEVICE *out, LPUNKNOWN outer)
{
    if (outer) return CLASS_E_NOAGGREGATION;
    return create_device(name, out);
}

static HRESULT WINAPI sti_GetDeviceValue(IStillImageW *iface, LPWSTR name, LPWSTR value, LPDWORD type, LPBYTE data, LPDWORD size)
{
    struct device_record rec;
    LONG ret;

    if (!find_device(name, &rec)) return STIERR_INVALID_DEVICE_NAME;
    ret = query_device_value(&rec, value, type, data, size);
    return ret ? HRESULT_FROM_WIN32(ret) : S_OK;
}

static HRESULT WINAPI sti_SetDeviceValue(IStillImageW *iface, LPWSTR name, LPWSTR value, DWORD type, LPBYTE data, DWORD size)
{
    struct device_record rec;
    HKEY key;
    LONG ret;

    if (!find_device(name, &rec)) return STIERR_INVALID_DEVICE_NAME;
    if ((ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE, rec.key_path, 0, KEY_SET_VALUE, &key))) return HRESULT_FROM_WIN32(ret);
    ret = RegSetValueExW(key, value, 0, type, data, size);
    RegCloseKey(key);
    return ret ? HRESULT_FROM_WIN32(ret) : S_OK;
}

static HRESULT WINAPI sti_GetSTILaunchInformation(IStillImageW *iface, LPWSTR name, DWORD *event, LPWSTR event_name)
{
    return STIERR_UNSUPPORTED;
}

static HRESULT WINAPI sti_RegisterLaunchApplication(IStillImageW *iface, LPWSTR app, LPWSTR cmdline)
{
    return S_OK;
}

static HRESULT WINAPI sti_UnregisterLaunchApplication(IStillImageW *iface, LPWSTR app)
{
    return S_OK;
}

static HRESULT WINAPI sti_EnableHwNotifications(IStillImageW *iface, LPCWSTR name, WINBOOL enable)
{
    return S_OK;
}

static HRESULT WINAPI sti_GetHwNotificationState(IStillImageW *iface, LPCWSTR name, WINBOOL *state)
{
    *state = FALSE;
    return S_OK;
}

static HRESULT WINAPI sti_RefreshDeviceBus(IStillImageW *iface, LPCWSTR name)
{
    return S_OK;
}

static HRESULT WINAPI sti_LaunchApplicationForDevice(IStillImageW *iface, LPWSTR name, LPWSTR app, LPSTINOTIFY notify)
{
    return STIERR_UNSUPPORTED;
}

static HRESULT WINAPI sti_SetupDeviceParameters(IStillImageW *iface, PSTI_DEVICE_INFORMATIONW info)
{
    return S_OK;
}

static HRESULT WINAPI sti_WriteToErrorLog(IStillImageW *iface, DWORD type, LPCWSTR message)
{
    sfl_log("app: %ls", message);
    return S_OK;
}

static IStillImageWVtbl sti_vtbl = {
    sti_QueryInterface, sti_AddRef, sti_Release, sti_Initialize, sti_GetDeviceList, sti_GetDeviceInfo,
    sti_CreateDevice, sti_GetDeviceValue, sti_SetDeviceValue, sti_GetSTILaunchInformation,
    sti_RegisterLaunchApplication, sti_UnregisterLaunchApplication, sti_EnableHwNotifications,
    sti_GetHwNotificationState, sti_RefreshDeviceBus, sti_LaunchApplicationForDevice,
    sti_SetupDeviceParameters, sti_WriteToErrorLog,
};

/* ------------------------------------------------------------------------ */
/* Exports                                                                  */
/* ------------------------------------------------------------------------ */

#undef StiCreateInstance

HRESULT WINAPI StiCreateInstanceW(HINSTANCE inst, DWORD version, IStillImageW **out, LPUNKNOWN outer)
{
    struct still_image *sti;

    if (!out) return E_POINTER;
    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!(sti = calloc(1, sizeof(*sti)))) return E_OUTOFMEMORY;
    sti->IStillImageW_iface.lpVtbl = &sti_vtbl;
    sti->ref = 1;
    InterlockedIncrement(&object_count);
    *out = &sti->IStillImageW_iface;
    return S_OK;
}

/* SilverFast resolves this undecorated name with GetProcAddress. */
HRESULT WINAPI StiCreateInstance(HINSTANCE inst, DWORD version, IStillImageW **out, LPUNKNOWN outer)
{
    return StiCreateInstanceW(inst, version, out, outer);
}

HRESULT WINAPI StiCreateInstanceA(HINSTANCE inst, DWORD version, IStillImageA **out, LPUNKNOWN outer)
{
    sfl_log("StiCreateInstanceA is not supported");
    if (out) *out = NULL;
    return STIERR_UNSUPPORTED;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return object_count ? S_FALSE : S_OK;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    *out = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllRegisterServer(void)
{
    return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        sti_instance = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
