/*
 * OLE client/server test suite
 *
 * Copyright 2013 Dmitry Timoshkov
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS
#define CONST_VTABLE

#include <windows.h>
#include <exdisp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <assert.h>
#include "wine/test.h"

#include <initguid.h>
DEFINE_GUID(CLSID_WineTestObject, 0xdeadbeef,0xdead,0xbeef,0xde,0xad,0xbe,0xef,0xde,0xad,0xbe,0xef);
#ifndef CLSID_IdentityUnmarshal
DEFINE_GUID(CLSID_IdentityUnmarshal,0x0000001b,0x0000,0x0000,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46);
#endif
DEFINE_GUID(CLSID_UnknownUnmarshal,0x4c1e39e1,0xe3e3,0x4296,0xaa,0x86,0xec,0x93,0x8d,0x89,0x6e,0x92);

struct winetest_info
{
    LONG child_failures;
};

static const struct
{
    const GUID *guid;
    const char *name;
} guid_name[] =
{
#define GUID_NAME(guid) \
    { &IID_##guid, #guid }
    GUID_NAME(IUnknown),
    GUID_NAME(IClassFactory),
    GUID_NAME(IOleObject),
    GUID_NAME(IMarshal),
    GUID_NAME(IStdMarshalInfo),
    GUID_NAME(IExternalConnection),
    GUID_NAME(IRunnableObject),
    { &CLSID_IdentityUnmarshal, "CLSID_IdentityUnmarshal" },
    { &CLSID_UnknownUnmarshal, "CLSID_UnknownUnmarshal" },
#undef GUID_NAME
};

static const char *debugstr_guid(const GUID *guid)
{
    static char buf[50];
    int i;

    if (!guid) return "(null)";

    for (i = 0; i < sizeof(guid_name)/sizeof(guid_name[0]); i++)
    {
        if (IsEqualIID(guid, guid_name[i].guid))
            return guid_name[i].name;
    }

    sprintf(buf, "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
            guid->Data1, guid->Data2, guid->Data3, guid->Data4[0],
            guid->Data4[1], guid->Data4[2], guid->Data4[3], guid->Data4[4],
            guid->Data4[5], guid->Data4[6], guid->Data4[7]);
    return buf;
}

/******************************* OLE server *******************************/
typedef struct
{
    IUnknown IUnknown_iface;
    LONG ref;
} UnknownImpl;

static inline UnknownImpl *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, UnknownImpl, IUnknown_iface);
}

static HRESULT WINAPI UnknownImpl_QueryInterface(IUnknown *iface,
    REFIID iid, void **ppv)
{
    UnknownImpl *This = impl_from_IUnknown(iface);

    trace("server: unknown_QueryInterface: %p,%s,%p\n", iface, debugstr_guid(iid), ppv);

    if (!ppv) return E_INVALIDARG;

    if (IsEqualIID(&IID_IUnknown, iid))
    {
        *ppv = &This->IUnknown_iface;
        IUnknown_AddRef(&This->IUnknown_iface);
        return S_OK;
    }

    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI UnknownImpl_AddRef(IUnknown *iface)
{
    UnknownImpl *This = impl_from_IUnknown(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    trace("server: unknown_AddRef: %p, ref %u\n", iface, ref);
    return ref;
}

static ULONG WINAPI UnknownImpl_Release(IUnknown *iface)
{
    UnknownImpl *This = impl_from_IUnknown(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    trace("server: unknown_Release: %p, ref %u\n", iface, ref);
    if (ref == 0) HeapFree(GetProcessHeap(), 0, This);
    return ref;
}

static const IUnknownVtbl UnknownImpl_Vtbl =
{
    UnknownImpl_QueryInterface,
    UnknownImpl_AddRef,
    UnknownImpl_Release,
};

typedef struct
{
    IClassFactory IClassFactory_iface;
    LONG ref;
} ClassFactoryImpl;

static inline ClassFactoryImpl *impl_from_IClassFactory(IClassFactory *iface)
{
    return CONTAINING_RECORD(iface, ClassFactoryImpl, IClassFactory_iface);
}

static HRESULT WINAPI ClassFactoryImpl_QueryInterface(IClassFactory *iface,
    REFIID iid, void **ppv)
{
    ClassFactoryImpl *This = impl_from_IClassFactory(iface);

    trace("server: factory_QueryInterface: %p,%s,%p\n", iface, debugstr_guid(iid), ppv);

    if (!ppv) return E_INVALIDARG;

    if (IsEqualIID(&IID_IUnknown, iid) ||
        IsEqualIID(&IID_IClassFactory, iid))
    {
        IClassFactory_AddRef(&This->IClassFactory_iface);
        *ppv = &This->IClassFactory_iface;
        return S_OK;
    }

    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI ClassFactoryImpl_AddRef(IClassFactory *iface)
{
    ClassFactoryImpl *This = impl_from_IClassFactory(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    trace("server: factory_AddRef: %p, ref %u\n", iface, ref);
    return ref;
}

static ULONG WINAPI ClassFactoryImpl_Release(IClassFactory *iface)
{
    ClassFactoryImpl *This = impl_from_IClassFactory(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    trace("server: factory_Release: %p, ref %u\n", iface, ref);
    return ref;
}

static HRESULT WINAPI ClassFactoryImpl_CreateInstance(IClassFactory *iface,
    IUnknown *punkouter, REFIID iid, void **ppv)
{
    UnknownImpl *unknown;
    HRESULT hr;

    trace("server: factory_CreateInstance: %p,%s,%p\n", iface, debugstr_guid(iid), ppv);

    if (punkouter) return CLASS_E_NOAGGREGATION;

    unknown = HeapAlloc(GetProcessHeap(), 0, sizeof(*unknown));
    if (!unknown) return E_OUTOFMEMORY;

    unknown->IUnknown_iface.lpVtbl = &UnknownImpl_Vtbl;
    unknown->ref = 1;

    hr = IUnknown_QueryInterface(&unknown->IUnknown_iface, iid, ppv);
    IUnknown_Release(&unknown->IUnknown_iface);

    return hr;
}

static HRESULT WINAPI ClassFactoryImpl_LockServer(IClassFactory *iface, BOOL lock)
{
    static LONG server_locks;
    ULONG ref = lock ? InterlockedIncrement(&server_locks) : InterlockedDecrement(&server_locks);

    trace("server: factory_LockServer: %p,%d, ref %u\n", iface, lock, ref);
    return S_OK;
}

static const IClassFactoryVtbl ClassFactoryImpl_Vtbl =
{
    ClassFactoryImpl_QueryInterface,
    ClassFactoryImpl_AddRef,
    ClassFactoryImpl_Release,
    ClassFactoryImpl_CreateInstance,
    ClassFactoryImpl_LockServer
};

static ClassFactoryImpl factory = { { &ClassFactoryImpl_Vtbl }, 0 };

static void ole_server(void)
{
    HRESULT hr;
    DWORD key;

    trace("server: starting %u\n", GetCurrentProcessId());

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == S_OK)
    {
        trace("server: registering class object\n");
        hr = CoRegisterClassObject(&CLSID_WineTestObject, (IUnknown *)&factory,
                                   CLSCTX_SERVER, REGCLS_MULTIPLEUSE, &key);
        if (hr == S_OK)
        {
            HANDLE done_event, init_done_event;

            done_event = OpenEvent(SYNCHRONIZE, FALSE, "ole_server_done_event");
            ok(done_event != 0, "server: OpenEvent error %d\n", GetLastError());
            init_done_event = OpenEvent(EVENT_MODIFY_STATE, FALSE, "ole_server_init_done_event");
            ok(init_done_event != 0, "server: OpenEvent error %d\n", GetLastError());

            SetEvent(init_done_event);

            trace("server: waiting for requests\n");
            WaitForSingleObject(done_event, INFINITE);

            CloseHandle(done_event);
            CloseHandle(init_done_event);
            if (0)
            {
                /* calling CoRevokeClassObject terminates process under Win7 */
                trace("call CoRevokeClassObject\n");
                CoRevokeClassObject(key);
                trace("ret CoRevokeClassObject\n");
            }
        }
        trace("server: call CoUninitialize\n");
        CoUninitialize();
        trace("server: ret CoUninitialize\n");
    }

    trace("server: exiting %u\n", GetCurrentProcessId());
}

/******************************* OLE client *******************************/
static BOOL register_server(const char *server)
{
    static const WCHAR clsidW[] = {'C','L','S','I','D','\\',0};
    DWORD ret;
    HKEY root;
    WCHAR buf[39 + 6];
    char server_path[MAX_PATH];

    lstrcpy(server_path, server);
    lstrcat(server_path, " ole_server");

    lstrcpyW(buf, clsidW);
    StringFromGUID2(&CLSID_WineTestObject, buf + 6, 39);

    ret = RegCreateKeyExW(HKEY_CLASSES_ROOT, buf, 0, NULL, 0,
                          KEY_READ | KEY_WRITE | KEY_CREATE_SUB_KEY, NULL, &root, NULL);
    if (ret == ERROR_SUCCESS)
    {
        ret = RegSetValue(root, "LocalServer32", REG_SZ, server_path, strlen(server_path));
        ok(ret == ERROR_SUCCESS, "RegSetValue error %u\n", ret);
        RegCloseKey(root);
    }

    return ret == ERROR_SUCCESS;
}

static void unregister_server(void)
{
    static const WCHAR clsidW[] = {'C','L','S','I','D','\\',0};
    DWORD ret;
    HKEY root;
    WCHAR buf[39 + 6];

    lstrcpyW(buf, clsidW);
    StringFromGUID2(&CLSID_WineTestObject, buf + 6, 39);

    ret = RegCreateKeyExW(HKEY_CLASSES_ROOT, buf, 0, NULL, 0,
                          DELETE, NULL, &root, NULL);
    if (ret == ERROR_SUCCESS)
    {
        ret = RegDeleteKey(root, "LocalServer32");
        ok(ret == ERROR_SUCCESS, "RegDeleteKey error %u\n", ret);
        ret = RegDeleteKey(root, "");
        ok(ret == ERROR_SUCCESS, "RegDeleteKey error %u\n", ret);
        RegCloseKey(root);
    }
}

static HANDLE start_server(const char *argv0)
{
    PROCESS_INFORMATION pi;
    STARTUPINFO si;
    SECURITY_ATTRIBUTES sa;
    char cmdline[MAX_PATH * 2];
    BOOL ret;

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput =  GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = si.hStdOutput;

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    sprintf(cmdline, "\"%s\" ole_server -server", argv0);
    ret = CreateProcess(argv0, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    ok(ret, "CreateProcess(%s) error %d\n", cmdline, GetLastError());
    if (!ret) return 0;

    CloseHandle(pi.hThread);
    return pi.hProcess;
}

START_TEST(ole_server)
{
    CLSID clsid = CLSID_WineTestObject;
    HRESULT hr;
    IClassFactory *factory;
    IUnknown *unknown;
    IOleObject *oleobj;
    DWORD ret;
    HANDLE mapping, done_event, init_done_event, process;
    struct winetest_info *info;
    int argc;
    char **argv;

    mapping = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, "winetest_ole_server");
    ok(mapping != 0, "CreateFileMapping failed\n");
    info = MapViewOfFile(mapping, FILE_MAP_READ|FILE_MAP_WRITE, 0, 0, 4096);

    argc = winetest_get_mainargs(&argv);

    done_event = CreateEvent(NULL, TRUE, FALSE, "ole_server_done_event");
    ok(done_event != 0, "CreateEvent error %d\n", GetLastError());
    init_done_event = CreateEvent(NULL, TRUE, FALSE, "ole_server_init_done_event");
    ok(init_done_event != 0, "CreateEvent error %d\n", GetLastError());

    if (argc > 2)
    {
        if (!lstrcmpi(argv[2], "-Embedding"))
        {
            trace("server: Refusing to be run by ole32\n");
            return;
        }

        if (!lstrcmpi(argv[2], "-server"))
        {
            info->child_failures = 0;
            ole_server();
            info->child_failures = winetest_get_failures();
            return;
        }

        trace("server: Unknown parameter: %s\n", argv[2]);
        return;
    }

    if (!register_server(argv[0]))
    {
        win_skip("not enough permissions to create a server CLSID key\n");
        return;
    }

    if (!(process = start_server(argv[0])))
    {
        unregister_server();
        return;
    }
    WaitForSingleObject(init_done_event, 5000);

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ok(hr == S_OK, "OleInitialize error %#x\n", hr);

    hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_HANDLER, &IID_IUnknown, (void **)&unknown);
    ok(hr == REGDB_E_CLASSNOTREG, "expected REGDB_E_CLASSNOTREG, got %#x\n", hr);

    /* server supports IID_IUnknown */
    trace("call CoCreateInstance(&IID_IUnknown)\n");
    hr = CoCreateInstance(&clsid, NULL, CLSCTX_LOCAL_SERVER, &IID_IUnknown, (void **)&unknown);
    trace("ret CoCreateInstance(&IID_IUnknown)\n");
    ok(hr == S_OK, "CoCreateInstance(IID_IUnknown) error %#x\n", hr);

    trace("call IUnknown_QueryInterface(&IID_IRunnableObject)\n");
    hr = IUnknown_QueryInterface(unknown, &IID_IRunnableObject, (void **)&oleobj);
    trace("ret IUnknown_QueryInterface(&IID_IRunnableObject)\n");
    ok(hr == E_NOINTERFACE, "expected E_NOINTERFACE, got %#x\n", hr);

    trace("call OleRun\n");
    hr = OleRun(unknown);
    trace("ret OleRun\n");
    ok(hr == S_OK, "OleRun error %#x\n", hr);

    trace("call IUnknown_QueryInterface(&IID_IOleObject)\n");
    hr = IUnknown_QueryInterface(unknown, &IID_IOleObject, (void **)&oleobj);
    trace("ret IUnknown_QueryInterface(&IID_IOleObject)\n");
    ok(hr == E_NOINTERFACE, "expected E_NOINTERFACE, got %#x\n", hr);

    trace("call IUnknown_Release\n");
    ret = IUnknown_Release(unknown);
    trace("ret IUnknown_Release\n");
    ok(!ret, "expected ref 0, got %u\n", ret);

    trace("call CoGetClassObject(&IID_IClassFactory)\n");
    hr = CoGetClassObject(&clsid, CLSCTX_LOCAL_SERVER, NULL, &IID_IClassFactory, (void **)&factory);
    trace("ret CoGetClassObject(&IID_IClassFactory)\n");
    ok(hr == S_OK, "CoGetClassObject error %#x\n", hr);

    trace("call IClassFactory_QueryInterface(&IID_IOleObject)\n");
    hr = IClassFactory_QueryInterface(factory, &IID_IOleObject, (void **)&oleobj);
    trace("ret IClassFactory_QueryInterface(&IID_IOleObject)\n");
    ok(hr == E_NOINTERFACE, "expected E_NOINTERFACE, got %#x\n", hr);

    trace("call IClassFactory_CreateInstance(&IID_IOleObject)\n");
    hr = IClassFactory_CreateInstance(factory, NULL, &IID_IOleObject, (void **)&oleobj);
    trace("ret IClassFactory_CreateInstance(&IID_IOleObject)\n");
    ok(hr == E_NOINTERFACE, "expected E_NOINTERFACE, got %#x\n", hr);

    trace("call IClassFactory_Release\n");
    ret = IClassFactory_Release(factory);
    trace("ret IClassFactory_Release\n");
    ok(!ret, "expected ref 0, got %u\n", ret);

    trace("signalling termination\n");
    SetEvent(done_event);
    ret = WaitForSingleObject(process, 10000);
    ok(ret == WAIT_OBJECT_0, "server failed to terminate\n");

    OleUninitialize();

    unregister_server();

    if (info->child_failures)
    {
        trace("%d failures in child process\n", info->child_failures);
        winetest_add_failures(info->child_failures);
    }
}
