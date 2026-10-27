/* Exercises sti.dll the way SilverFast does, against tests/mock_usbd.py.
 * Run under Wine: wine sti_selftest.exe path\to\sti.dll
 * The test registers a fake device in the prefix registry and removes it
 * again at the end. */
#define COBJMACROS
#include <windows.h>
#include <sti.h>
#include <stdio.h>
#include <string.h>

#define CLASS_KEY L"SYSTEM\\CurrentControlSet\\Control\\Class\\{6BDD1FC6-810F-11D0-BEC7-08002BE2092F}\\9999"
#define DEV_NAME  L"{6BDD1FC6-810F-11D0-BEC7-08002BE2092F}\\9999"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

typedef HRESULT (WINAPI *create_fn)(HINSTANCE, DWORD, IStillImageW **, LPUNKNOWN);

static void set_sz(HKEY k, const WCHAR *n, const WCHAR *v) { RegSetValueExW(k, n, 0, REG_SZ, (const BYTE *)v, (lstrlenW(v) + 1) * 2); }
static void set_dw(HKEY k, const WCHAR *n, DWORD v) { RegSetValueExW(k, n, 0, REG_DWORD, (const BYTE *)&v, 4); }

static void register_device(void)
{
    HKEY k, d;
    RegCreateKeyExW(HKEY_LOCAL_MACHINE, CLASS_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL);
    set_sz(k, L"SubClass", L"StillImage");
    set_sz(k, L"FriendlyName", L"Test Scanner");
    set_sz(k, L"Vendor", L"TEST");
    set_sz(k, L"CreateFileName", L"\\\\.\\Usbscan9");
    set_dw(k, L"DeviceType", 1);
    set_dw(k, L"DeviceSubType", 1);
    set_dw(k, L"Capabilities", 0x13);
    RegCreateKeyExW(k, L"DeviceData", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &d, NULL);
    set_sz(d, L"ExtCmdModule", L"test.dll");
    RegCloseKey(d);
    RegCloseKey(k);
}

static void unregister_device(void)
{
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, CLASS_KEY);
}

static STI_DEVICE_INFORMATIONW *find_test_device(STI_DEVICE_INFORMATIONW *list, DWORD n)
{
    DWORD i;
    for (i = 0; i < n; i++) if (!lstrcmpW(list[i].szDeviceInternalName, DEV_NAME)) return &list[i];
    return NULL;
}

int main(int argc, char **argv)
{
    HMODULE mod;
    create_fn create, create_plain;
    IStillImageW *sti = NULL;
    IStiDevice *dev = NULL;
    STI_DEVICE_INFORMATIONW *list = NULL, *info;
    DWORD n = 0, type, size, got;
    WCHAR value[64];
    char buf[64];
    HRESULT hr;

    if (argc < 2) { printf("usage: sti_selftest.exe sti.dll\n"); return 2; }
    if (!(mod = LoadLibraryA(argv[1]))) { printf("cannot load %s\n", argv[1]); return 2; }
    create = (create_fn)(void *)GetProcAddress(mod, "StiCreateInstanceW");
    create_plain = (create_fn)(void *)GetProcAddress(mod, "StiCreateInstance");
    CHECK(create && create_plain);
    if (!create || !create_plain) return 1;

    register_device();

    hr = create_plain(GetModuleHandleW(NULL), STI_VERSION, &sti, NULL);
    CHECK(hr == S_OK && sti);

    hr = sti->lpVtbl->GetDeviceList(sti, 0, 0, &n, (void **)&list);
    CHECK(hr == S_OK && n >= 1);
    info = find_test_device(list, n);
    CHECK(info != NULL);
    if (info)
    {
        CHECK(info->dwSize == 0x138);
        CHECK(info->DeviceType == 0x00010001);
        CHECK(!lstrcmpW(info->pszDeviceDescription, L"Test Scanner"));
        CHECK(!lstrcmpW(info->pszVendorDescription, L"TEST"));
        CHECK(!lstrcmpW(info->pszPortName, L"\\\\.\\Usbscan9"));
    }
    LocalFree(list);

    size = sizeof(value);
    hr = sti->lpVtbl->GetDeviceValue(sti, DEV_NAME, L"ExtCmdModule", &type, (BYTE *)value, &size);
    CHECK(hr == S_OK && type == REG_SZ && !lstrcmpW(value, L"test.dll"));

    hr = sti->lpVtbl->CreateDevice(sti, (LPWSTR)L"{6BDD1FC6-810F-11D0-BEC7-08002BE2092F}\\nope", 3, &dev, NULL);
    CHECK(FAILED(hr) && !dev);

    hr = sti->lpVtbl->CreateDevice(sti, (LPWSTR)DEV_NAME, 3, &dev, NULL);
    CHECK(hr == S_OK && dev);
    if (dev)
    {
        CHECK(IStiDevice_LockDevice(dev, 2000) == S_OK);
        CHECK(IStiDevice_RawWriteData(dev, "\x1b\x03hello", 7, NULL) == S_OK);
        got = 7;
        hr = IStiDevice_RawReadData(dev, buf, &got, NULL);
        CHECK(hr == S_OK && got == 7 && !memcmp(buf, "\x1b\x03hello", 7));
        got = 4; /* mock has nothing queued: a zero-length read is a valid answer */
        hr = IStiDevice_RawReadData(dev, buf, &got, NULL);
        CHECK(hr == S_OK && got == 0);
        CHECK(IStiDevice_UnLockDevice(dev) == S_OK);
        CHECK(IStiDevice_Release(dev) == 0);
    }
    CHECK(sti->lpVtbl->Release(sti) == 0);

    unregister_device();
    printf(failures ? "sti_selftest: %d failure(s)\n" : "sti_selftest: ok\n", failures);
    return failures ? 1 : 0;
}
