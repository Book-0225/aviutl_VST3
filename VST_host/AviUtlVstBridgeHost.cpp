#define _CRT_SECURE_NO_WARNINGS

// --- VST SDK Headers ---
#include "public.sdk\source\common\memorystream.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/base/ustring.h"

// --- Standard/Windows Headers ---
#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <stdexcept>
#include <mutex>
#include <condition_variable>
#include <tchar.h>
#include <cstdio>
#include <wincrypt.h>
#include <guiddef.h>
#include <objbase.h>

#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "ole32.lib")

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace VST3::Hosting;

#define MAX_STATE_DATA_LEN 66000 

// --- デバッグログ用マクロ ---
#ifdef _DEBUG
#define DbgPrint(format, ...) do { \
    TCHAR* tszDbgBuffer = new TCHAR[MAX_STATE_DATA_LEN]; \
    if (tszDbgBuffer) { \
        _stprintf_s(tszDbgBuffer, MAX_STATE_DATA_LEN, _T("[VstHost] ") format _T("\n"), ##__VA_ARGS__); \
        OutputDebugString(tszDbgBuffer); \
        _tprintf(tszDbgBuffer); \
        delete[] tszDbgBuffer; \
    } \
} while(0)
#else
#define DbgPrint(format, ...)
#endif
std::wstring TUIDToString(const TUID tuid) {
    wchar_t wstr[40] = { 0 };
    if (StringFromGUID2(*reinterpret_cast<const GUID*>(tuid), wstr, 40) > 0) {
        return std::wstring(wstr);
    }
    return L"";
}

const TCHAR* PIPE_NAME_BASE = TEXT("\\\\.\\pipe\\AviUtlVstBridge");
const TCHAR* SHARED_MEM_NAME_BASE = TEXT("Local\\AviUtlVstSharedAudio");
const TCHAR* EVENT_CLIENT_READY_NAME_BASE = TEXT("Local\\AviUtlVstClientReady");
const TCHAR* EVENT_HOST_DONE_NAME_BASE = TEXT("Local\\AviUtlVstHostDone");
const int MAX_BLOCK_SIZE = 2048;
#pragma pack(push, 1)
struct AudioSharedData {
    double sampleRate;
    int32_t numSamples;
    int32_t numChannels;
};
#pragma pack(pop)
const int FLOAT_SIZE = sizeof(float);
const int BUFFER_BYTES = MAX_BLOCK_SIZE * FLOAT_SIZE;
const int SHARED_MEM_TOTAL_SIZE = sizeof(AudioSharedData) + (4 * BUFFER_BYTES);
class VstHost;
VstHost* g_pVstHost = nullptr;
std::string base64_encode(const BYTE* data, DWORD data_len) {
    if (data == nullptr || data_len == 0) return "";
    DWORD b64_len = 0;
    if (!CryptBinaryToStringA(data, data_len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &b64_len)) {
        return "";
    }
    if (b64_len == 0) return "";

    std::string b64_str(b64_len, '\0');
    if (!CryptBinaryToStringA(data, data_len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &b64_str[0], &b64_len)) {
        return "";
    }
    b64_str.resize(b64_len - 1);
    return b64_str;
}
std::vector<BYTE> base64_decode(const std::string& b64_str) {
    if (b64_str.empty()) return {};
    DWORD bin_len = 0;
    if (!CryptStringToBinaryA(b64_str.c_str(), (DWORD)b64_str.length(), CRYPT_STRING_BASE64, NULL, &bin_len, NULL, NULL)) {
        return {};
    }
    if (bin_len == 0) return {};

    std::vector<BYTE> bin_data(bin_len);
    if (!CryptStringToBinaryA(b64_str.c_str(), (DWORD)b64_str.length(), CRYPT_STRING_BASE64, bin_data.data(), &bin_len, NULL, NULL)) {
        return {};
    }
    return bin_data;
}
class WindowController : public IPlugFrame {
public:
    WindowController(IPlugView* view, HWND parent) : plugView(view), parentWindow(parent) {}
    void connect() { if (plugView) plugView->setFrame(this); }
    void disconnect() { if (plugView) plugView->setFrame(nullptr); }
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, IPlugFrame::iid) || FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
            *obj = this; addRef(); return kResultTrue;
        }
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override {
        if (view == plugView && newSize && parentWindow) {
            RECT clientRect = { 0, 0, newSize->right - newSize->left, newSize->bottom - newSize->top };
            DWORD style = GetWindowLong(parentWindow, GWL_STYLE);
            DWORD exStyle = GetWindowLong(parentWindow, GWL_EXSTYLE);
            BOOL hasMenu = (GetMenu(parentWindow) != NULL);
            AdjustWindowRectEx(&clientRect, style, hasMenu, exStyle);
            int windowWidth = clientRect.right - clientRect.left;
            int windowHeight = clientRect.bottom - clientRect.top;
            SetWindowPos(parentWindow, NULL, 0, 0, windowWidth, windowHeight, SWP_NOMOVE | SWP_NOZORDER);
        }
        return kResultTrue;
    }
private:
    IPlugView* plugView;
    HWND parentWindow;
};

class VstHost : public virtual IHostApplication,
    public virtual IComponentHandler,
    public virtual IComponentHandler2
{
public:
    VstHost(HINSTANCE hInstance, uint64_t unique_id) :
        m_refCount(1),
        m_hInstance(hInstance), m_uniqueId(unique_id),
        m_hPipe(INVALID_HANDLE_VALUE), m_hShm(NULL),
        m_pSharedMem(nullptr), m_pAudioData(nullptr), m_hEventClientReady(NULL),
        m_hEventHostDone(NULL), m_hPipeThread(NULL), m_hAudioThread(NULL),
        m_hGuiWindow(NULL), m_plugProvider(nullptr), m_component(nullptr),
        m_processor(nullptr), m_controller(nullptr), m_plugView(nullptr),
        m_windowController(nullptr), m_mainLoopRunning(false), m_threadsRunning(false),
        m_isPluginReady(false) {
        DbgPrint(_T("VstHost constructor called for ID %llu."), m_uniqueId);
    }

protected:
    ~VstHost() {
        DbgPrint(_T("VstHost destructor called for ID %llu."), m_uniqueId);
    }

public:
    // --- FUnknown (COM)の実装 ---
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        std::wstring iid_wstr = TUIDToString(_iid);
#ifdef _UNICODE
        DbgPrint(_T("queryInterface: Plugin requested IID: %s"), iid_wstr.c_str());
#else
        char iid_str_mb[40] = { 0 };
        WideCharToMultiByte(CP_ACP, 0, iid_wstr.c_str(), -1, iid_str_mb, sizeof(iid_str_mb), NULL, NULL);
        DbgPrint(_T("queryInterface: Plugin requested IID: %s"), iid_str_mb);
#endif

        if (FUnknownPrivate::iidEqual(_iid, IHostApplication::iid)) {
            DbgPrint(_T("queryInterface: Providing IHostApplication."));
            *obj = static_cast<IHostApplication*>(this);
            addRef();
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IComponentHandler::iid)) {
            DbgPrint(_T("queryInterface: Providing IComponentHandler."));
            *obj = static_cast<IComponentHandler*>(this);
            addRef();
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IComponentHandler2::iid)) {
            DbgPrint(_T("queryInterface: Providing IComponentHandler2."));
            *obj = static_cast<IComponentHandler2*>(this);
            addRef();
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
            DbgPrint(_T("queryInterface: Providing IUnknown (via IHostApplication)."));
            *obj = static_cast<IHostApplication*>(this);
            addRef();
            return kResultOk;
        }

        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override {
        return ++m_refCount;
    }

    uint32 PLUGIN_API release() override {
        uint32 new_ref = --m_refCount;
        if (new_ref == 0) {
            delete this;
            return 0;
        }
        return new_ref;
    }
    tresult PLUGIN_API getName(String128 name) override {
        Steinberg::str8ToStr16(name, "AviUtl VST3 Host Bridge", 128);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void** obj) override {
        *obj = nullptr;
        return kNotImplemented;
    }
    tresult PLUGIN_API beginEdit(ParamID id) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue valueNormalized) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID id) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        DbgPrint(_T("IComponentHandler: restartComponent(0x%X) called by plugin."), flags);
        return kResultOk;
    }
    tresult PLUGIN_API setDirty(TBool state) override { return kResultOk; }
    tresult PLUGIN_API requestOpenEditor(FIDString name = nullptr) override { ShowGui(); return kResultOk; }
    tresult PLUGIN_API startGroupEdit() override { return kResultOk; }
    tresult PLUGIN_API finishGroupEdit() override { return kResultOk; }

    bool Initialize() {
        if (!InitIPC()) {
            DbgPrint(_T("Initialize: InitIPC FAILED."));
            return false;
        }

        m_threadsRunning = true;
        m_hPipeThread = CreateThread(NULL, 0, PipeThreadProc, this, 0, NULL);
        m_hAudioThread = CreateThread(NULL, 0, AudioThreadProc, this, 0, NULL);

        if (!m_hPipeThread || !m_hAudioThread) {
            DbgPrint(_T("Initialize: Failed to create worker threads."));
            return false;
        }
        DbgPrint(_T("Initialize: Host initialized successfully. Waiting for client commands..."));
        return true;
    }

    void RequestStop() {
        DbgPrint(_T("RequestStop: Setting main loop and worker threads to exit."));
        m_threadsRunning = false;
        m_mainLoopRunning = false;
        m_syncCv.notify_all();
    }

    void RunMessageLoop() {
        MSG msg;
        m_mainLoopRunning = true;
        while (m_mainLoopRunning) {
            ProcessQueuedCommands();

            if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    RequestStop();
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            else {
                Sleep(10);
            }
        }
        DbgPrint(_T("RunMessageLoop: Exited."));
    }

    void Cleanup() {
        if (!m_threadsRunning.exchange(false)) return;
        m_mainLoopRunning = false;
        m_syncCv.notify_all();
        DbgPrint(_T("Cleanup: Starting cleanup process..."));
        if (m_hPipe != INVALID_HANDLE_VALUE) {
            DbgPrint(_T("Cleanup: Closing pipe handle to unblock thread..."));
            CloseHandle(m_hPipe);
            m_hPipe = INVALID_HANDLE_VALUE;
        }
        if (m_hEventClientReady) SetEvent(m_hEventClientReady);
        if (m_hPipeThread) {
            WaitForSingleObject(m_hPipeThread, 2000);
            CloseHandle(m_hPipeThread);
            m_hPipeThread = NULL;
        }
        if (m_hAudioThread) {
            WaitForSingleObject(m_hAudioThread, 2000);
            CloseHandle(m_hAudioThread);
            m_hAudioThread = NULL;
        }
        ReleasePlugin();
        if (m_pSharedMem) { UnmapViewOfFile(m_pSharedMem); m_pSharedMem = nullptr; }
        if (m_hShm) { CloseHandle(m_hShm); m_hShm = NULL; }
        if (m_hEventClientReady) { CloseHandle(m_hEventClientReady); m_hEventClientReady = NULL; }
        if (m_hEventHostDone) { CloseHandle(m_hEventHostDone); m_hEventHostDone = NULL; }
        DbgPrint(_T("Cleanup: Host cleaned up."));
    }

    void HandlePipeCommands() {
        char buffer[40 * 1024];
        DWORD bytesRead;
        while (m_threadsRunning) {
            DbgPrint(_T("PipeThread: Waiting for client to connect on pipe..."));
            BOOL connected = ConnectNamedPipe(m_hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (!connected && m_hPipe != INVALID_HANDLE_VALUE) {
                if (m_threadsRunning) Sleep(100);
                continue;
            }
            if (!m_threadsRunning || m_hPipe == INVALID_HANDLE_VALUE) break;
            DbgPrint(_T("PipeThread: Client connected."));
            while (m_threadsRunning) {
                BOOL success = ReadFile(m_hPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL);
                if (!success || bytesRead == 0) {
                    if (GetLastError() != ERROR_BROKEN_PIPE && GetLastError() != ERROR_INVALID_HANDLE) {
                        DbgPrint(_T("PipeThread: ReadFile failed with error %lu."), GetLastError());
                    }
                    break;
                }
                buffer[bytesRead] = '\0';
                std::string cmd(buffer);
                std::string response = ProcessCommand(cmd);
                DWORD bytesWritten;
                WriteFile(m_hPipe, response.c_str(), (DWORD)response.length(), &bytesWritten, NULL);
                if (cmd.rfind("exit", 0) == 0) {
                    break;
                }
            }
            if (m_hPipe != INVALID_HANDLE_VALUE) DisconnectNamedPipe(m_hPipe);
            DbgPrint(_T("PipeThread: Client disconnected."));
        }
        DbgPrint(_T("PipeThread: Exiting."));
    }

    void HandleAudioProcessing() {
        while (m_threadsRunning) {
            if (WaitForSingleObject(m_hEventClientReady, 1000) != WAIT_OBJECT_0) continue;
            if (!m_threadsRunning) break;
            ResetEvent(m_hEventClientReady);
            if (m_isPluginReady && m_processor && m_pAudioData) {
                ProcessAudioBlock();
            }
            SetEvent(m_hEventHostDone);
        }
        DbgPrint(_T("AudioThread: Exiting."));
    }

    void ShowGui() {
        DbgPrint(_T("ShowGui: Request received."));
        if (!m_controller || !m_component) { DbgPrint(_T("ShowGui: Aborted. Plugin not loaded yet.")); return; }
        if (m_hGuiWindow && IsWindow(m_hGuiWindow)) { ShowWindow(m_hGuiWindow, SW_SHOW); SetForegroundWindow(m_hGuiWindow); return; }
        m_plugView = m_controller->createView(ViewType::kEditor);
        if (!m_plugView) { DbgPrint(_T("ShowGui: Failed to create plug-in view.")); return; }
        ViewRect plugClientSize;
        if (m_plugView->getSize(&plugClientSize) != kResultOk) { plugClientSize = { 0, 0, 800, 600 }; }
        RECT windowRect = { 0, 0, plugClientSize.right - plugClientSize.left, plugClientSize.bottom - plugClientSize.top };
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        DWORD exStyle = WS_EX_APPWINDOW;
        AdjustWindowRectEx(&windowRect, style, FALSE, exStyle);
        WNDCLASS wc = {};
        wc.lpfnWndProc = VstHost::WndProc;
        wc.hInstance = m_hInstance;
        wc.lpszClassName = TEXT("VstHostWindowClass");
        RegisterClass(&wc);
        m_hGuiWindow = CreateWindowEx(exStyle, wc.lpszClassName, TEXT("VST3 Plugin"), style,
            CW_USEDEFAULT, CW_USEDEFAULT, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
            NULL, NULL, m_hInstance, this);
        if (!m_hGuiWindow) { DbgPrint(_T("ShowGui: CreateWindowEx failed. Error: %lu"), GetLastError()); m_plugView->release(); m_plugView = nullptr; return; }
        m_windowController = new WindowController(m_plugView, m_hGuiWindow);
        m_windowController->connect();
        ShowWindow(m_hGuiWindow, SW_SHOW);
        UpdateWindow(m_hGuiWindow);
        if (m_plugView->attached(m_hGuiWindow, kPlatformTypeHWND) != kResultOk) { DbgPrint(_T("ShowGui: plugView->attached failed.")); DestroyWindow(m_hGuiWindow); return; }
        ViewRect currentSize;
        if (m_plugView->getSize(&currentSize) == kResultOk) { m_windowController->resizeView(m_plugView, &currentSize); }
        if (m_controller) {
            DbgPrint(_T("ShowGui: Calling restartComponent(kParamValuesChanged) to force GUI update."));
            restartComponent(kParamValuesChanged);
        }
        DbgPrint(_T("ShowGui: GUI shown successfully."));
    }

    void HideGui() {
        DbgPrint(_T("HideGui: Request received."));
        if (m_hGuiWindow) DestroyWindow(m_hGuiWindow);
    }

    void OnGuiClose() {
        DbgPrint(_T("OnGuiClose: GUI window is being destroyed."));
        if (m_plugView) {
            if (m_windowController) { m_windowController->disconnect(); delete m_windowController; m_windowController = nullptr; }
            m_plugView->removed();
            m_plugView->release();
            m_plugView = nullptr;
        }
        m_hGuiWindow = NULL;
        DbgPrint(_T("OnGuiClose: GUI resources released."));
    }

    bool LoadPlugin(const std::string& path, double sampleRate, int32 blockSize);
    void ReleasePlugin();
    void ProcessAudioBlock();

private:
    static DWORD WINAPI PipeThreadProc(LPVOID p) { HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); if (SUCCEEDED(hr)) { ((VstHost*)p)->HandlePipeCommands(); CoUninitialize(); } return 0; }
    static DWORD WINAPI AudioThreadProc(LPVOID p) { ((VstHost*)p)->HandleAudioProcessing(); return 0; }

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
        VstHost* host = (message == WM_CREATE) ?
            (VstHost*)((CREATESTRUCT*)lParam)->lpCreateParams :
            (VstHost*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (host) {
            switch (message) {
            case WM_CREATE:
                SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)host);
                {
                    HMENU hMenu = GetSystemMenu(hWnd, FALSE);
                    if (hMenu != NULL) { EnableMenuItem(hMenu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED); }
                }
                return 0;
            case WM_CLOSE:
                return 0;
            case WM_DESTROY:
                host->OnGuiClose();
                SetWindowLongPtr(hWnd, GWLP_USERDATA, NULL);
                break;
            }
        }
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    bool InitIPC() {
        TCHAR pipe_name[MAX_PATH], shm_name[MAX_PATH], event_ready_name[MAX_PATH], event_done_name[MAX_PATH];
        _stprintf_s(pipe_name, _T("%s_%llu"), PIPE_NAME_BASE, m_uniqueId);
        _stprintf_s(shm_name, _T("%s_%llu"), SHARED_MEM_NAME_BASE, m_uniqueId);
        _stprintf_s(event_ready_name, _T("%s_%llu"), EVENT_CLIENT_READY_NAME_BASE, m_uniqueId);
        _stprintf_s(event_done_name, _T("%s_%llu"), EVENT_HOST_DONE_NAME_BASE, m_uniqueId);
        DbgPrint(_T("InitIPC: Creating Pipe: %s"), pipe_name);
        m_hPipe = CreateNamedPipe(pipe_name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 40 * 1024, 40 * 1024, 0, NULL);
        if (m_hPipe == INVALID_HANDLE_VALUE) { DbgPrint(_T("InitIPC: CreateNamedPipe FAILED. Error: %lu"), GetLastError()); return false; }
        DbgPrint(_T("InitIPC: Creating Shared Memory: %s"), shm_name);
        m_hShm = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, SHARED_MEM_TOTAL_SIZE, shm_name);
        if (!m_hShm) { DbgPrint(_T("InitIPC: CreateFileMapping FAILED. Error: %lu"), GetLastError()); return false; }
        m_pSharedMem = MapViewOfFile(m_hShm, FILE_MAP_ALL_ACCESS, 0, 0, SHARED_MEM_TOTAL_SIZE);
        if (!m_pSharedMem) { DbgPrint(_T("InitIPC: MapViewOfFile FAILED. Error: %lu"), GetLastError()); return false; }
        m_pAudioData = (AudioSharedData*)m_pSharedMem;
        DbgPrint(_T("InitIPC: Creating Events: %s, %s"), event_ready_name, event_done_name);
        m_hEventClientReady = CreateEvent(NULL, TRUE, FALSE, event_ready_name);
        m_hEventHostDone = CreateEvent(NULL, FALSE, FALSE, event_done_name);
        if (!m_hEventClientReady || !m_hEventHostDone) { DbgPrint(_T("InitIPC: CreateEvent FAILED. Error: %lu"), GetLastError()); return false; }
        DbgPrint(_T("InitIPC: IPC initialized successfully for ID %llu."), m_uniqueId);
        return true;
    }

    std::string ProcessCommand(const std::string& full_cmd) {
        std::string cmd = full_cmd;
        if (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();
        if (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();
        if (cmd == "exit") {
            DbgPrint(_T("ProcessCommand: Processing 'exit' immediately..."));
            RequestStop();
            return "OK: Exit requested.\n";
        }
        if (cmd == "get_state") {
            std::string result_state;
            bool success = false;
            {
                std::unique_lock<std::mutex> lock(m_syncMutex);
                m_syncCommand = cmd;
                m_syncCv.wait(lock, [this] { return m_syncCommand.empty() || !m_mainLoopRunning; });
                if (m_mainLoopRunning) {
                    success = m_syncSuccess;
                    result_state = m_syncResult;
                }
            }
            if (success) {
                if (result_state == "EMPTY") { return "OK EMPTY\n"; }
                return "OK " + result_state + "\n";
            }
            else {
                return "FAIL " + result_state + "\n";
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            m_commandQueue.push_back(full_cmd);
        }
        return "OK\n";
    }

    void ProcessQueuedCommands() {
        {
            std::unique_lock<std::mutex> lock(m_syncMutex);
            if (!m_syncCommand.empty()) {
                DbgPrint(_T("MainThread: Processing sync command: '%hs'"), m_syncCommand.c_str());
                if (m_syncCommand == "get_state") {
                    if (m_component && m_controller) {
                        MemoryStream componentStream; MemoryStream controllerStream;
                        tresult componentResult = m_component->getState(&componentStream);
                        tresult controllerResult = m_controller->getState(&controllerStream);
                        int64 componentSize = (componentResult == kResultOk) ? componentStream.getSize() : 0;
                        int64 controllerSize = (controllerResult == kResultOk) ? controllerStream.getSize() : 0;
                        DbgPrint(_T("MainThread: Component::getState result: 0x%X (Size: %lld), Controller::getState result: 0x%X (Size: %lld)"), componentResult, componentSize, controllerResult, controllerSize);
                        if (componentSize > 0 || controllerSize > 0) {
                            MemoryStream finalStream; int32 bytesWritten = 0;
                            finalStream.write(&componentSize, sizeof(componentSize), &bytesWritten);
                            if (componentSize > 0) {
                                componentStream.seek(0, IBStream::kIBSeekSet, nullptr);
                                finalStream.write(componentStream.getData(), (int32)componentSize, &bytesWritten);
                            }
                            finalStream.write(&controllerSize, sizeof(controllerSize), &bytesWritten);
                            if (controllerSize > 0) {
                                controllerStream.seek(0, IBStream::kIBSeekSet, nullptr);
                                finalStream.write(controllerStream.getData(), (int32)controllerSize, &bytesWritten);
                            }
                            m_syncResult = "VST3_DUAL:" + base64_encode((const BYTE*)finalStream.getData(), (DWORD)finalStream.getSize());
                            m_syncSuccess = true;
                            DbgPrint(_T("MainThread: Combined state saved successfully (VST3_DUAL). Total size: %lld"), finalStream.getSize());
                        }
                        else {
                            DbgPrint(_T("MainThread: Both states are empty. Falling back to manual parameter scraping."));
                            std::string manual_state; int32 paramCount = m_controller->getParameterCount();
                            DbgPrint(_T("MainThread: Found %d parameters."), paramCount);
                            for (int32 i = 0; i < paramCount; ++i) {
                                ParameterInfo info = {};
                                if (m_controller->getParameterInfo(i, info) == kResultOk) {
                                    if (info.flags & ParameterInfo::kIsReadOnly) continue;
                                    ParamValue normalizedValue = m_controller->getParamNormalized(info.id);
                                    char buffer[256]; sprintf_s(buffer, "%X=%.17g;", info.id, normalizedValue);
                                    manual_state += buffer;
                                }
                            }
                            if (!manual_state.empty()) {
                                m_syncResult = "MANUAL:" + base64_encode((const BYTE*)manual_state.c_str(), (DWORD)manual_state.length());
                                m_syncSuccess = true;
                                DbgPrint(_T("MainThread: Manual state scraping successful. State size: %zu"), manual_state.length());
                            }
                            else {
                                m_syncResult = "EMPTY"; m_syncSuccess = true;
                                DbgPrint(_T("MainThread: Manual state scraping resulted in empty state."));
                            }
                        }
                    }
                    else {
                        m_syncSuccess = false; m_syncResult = "NoComponentOrController";
                        DbgPrint(_T("MainThread: get_state failed, no component or controller."));
                    }
                }
                m_syncCommand.clear();
                lock.unlock();
                m_syncCv.notify_one();
            }
        }
        std::vector<std::string> commandsToProcess;
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            if (m_commandQueue.empty()) return;
            commandsToProcess.swap(m_commandQueue);
        }
        for (const auto& cmd_raw : commandsToProcess) {
            std::string cmd = cmd_raw;
            if (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();
            if (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();
            DbgPrint(_T("MainThread: Processing queued command: '%hs'"), cmd.c_str());
            const std::string loadCmd = "load_plugin "; const std::string setStateCmd = "set_state ";
            if (cmd.rfind(loadCmd, 0) == 0) {
                try {
                    std::string args_str = cmd.substr(loadCmd.length()); std::string path;
                    double sampleRate = 44100.0; int32 blockSize = 1024;
                    if (args_str.front() == '"') {
                        size_t end_quote_pos = args_str.find('"', 1);
                        if (end_quote_pos != std::string::npos) {
                            path = args_str.substr(1, end_quote_pos - 1);
                            args_str = args_str.substr(end_quote_pos + 1);
                        }
                    }
                    args_str.erase(0, args_str.find_first_not_of(" \t\n\r"));
                    size_t space_pos = args_str.find(' ');
                    if (space_pos != std::string::npos) {
                        sampleRate = std::stod(args_str.substr(0, space_pos));
                        blockSize = std::stoi(args_str.substr(space_pos + 1));
                    }
                    if (!path.empty()) LoadPlugin(path, sampleRate, blockSize);
                }
                catch (const std::exception& e) { DbgPrint(_T("MainThread: Exception parsing load_plugin: %hs"), e.what()); }
            }
            else if (cmd.rfind(setStateCmd, 0) == 0) {
                if (m_controller && m_component) {
                    std::string full_data = cmd.substr(setStateCmd.length()); int32 bytesRead = 0;
                    if (full_data.rfind("VST3_DUAL:", 0) == 0) {
                        DbgPrint(_T("MainThread: Restoring state using combined DUAL state."));
                        std::vector<BYTE> state_data = base64_decode(full_data.substr(10));
                        if (!state_data.empty()) {
                            MemoryStream finalStream(state_data.data(), state_data.size()); tresult finalResult = kResultOk;
                            int64 componentSize = 0;
                            finalStream.read(&componentSize, sizeof(componentSize), &bytesRead);
                            DbgPrint(_T("MainThread: Reading component state, expected size: %lld"), componentSize);
                            if (componentSize > 0) {
                                std::vector<BYTE> componentData(componentSize);
                                if (finalStream.read(componentData.data(), (int32)componentSize, &bytesRead) == kResultOk && bytesRead == componentSize) {
                                    MemoryStream componentStream(componentData.data(), componentSize);
                                    if (m_component->setState(&componentStream) != kResultOk) {
                                        finalResult = kResultFalse; DbgPrint(_T("MainThread: Component setState FAILED."));
                                    }
                                }
                            }
                            int64 controllerSize = 0;
                            finalStream.read(&controllerSize, sizeof(controllerSize), &bytesRead);
                            DbgPrint(_T("MainThread: Reading controller state, expected size: %lld"), controllerSize);
                            if (controllerSize > 0) {
                                std::vector<BYTE> controllerData(controllerSize);
                                if (finalStream.read(controllerData.data(), (int32)controllerSize, &bytesRead) == kResultOk && bytesRead == controllerSize) {
                                    MemoryStream controllerStream(controllerData.data(), controllerSize);
                                    if (m_controller->setState(&controllerStream) != kResultOk) {
                                        finalResult = kResultFalse; DbgPrint(_T("MainThread: Controller setState FAILED."));
                                    }
                                }
                            }
                            if (finalResult == kResultOk) DbgPrint(_T("MainThread: DUAL setState success."));
                            else DbgPrint(_T("MainThread: DUAL setState finished with one or more errors."));
                        }
                    }
                    else if (full_data.rfind("VST3:", 0) == 0) {
                        DbgPrint(_T("MainThread: Restoring state using official setState (legacy controller-only)."));
                        std::vector<BYTE> state_data = base64_decode(full_data.substr(5));
                        if (!state_data.empty()) {
                            MemoryStream stream(state_data.data(), state_data.size());
                            if (m_controller->setState(&stream) == kResultOk) DbgPrint(_T("MainThread: setState (Official) success, size=%zu"), state_data.size());
                            else DbgPrint(_T("MainThread: setState (Official) failed."));
                        }
                    }
                    else if (full_data.rfind("MANUAL:", 0) == 0) {
                        DbgPrint(_T("MainThread: Restoring state using manual parameter setting."));
                        std::vector<BYTE> decoded_bytes = base64_decode(full_data.substr(7));
                        if (!decoded_bytes.empty()) {
                            std::string manual_state(decoded_bytes.begin(), decoded_bytes.end());
                            size_t start_pos = 0;
                            while (start_pos < manual_state.length()) {
                                size_t eq_pos = manual_state.find('=', start_pos); size_t sc_pos = manual_state.find(';', start_pos);
                                if (eq_pos == std::string::npos || sc_pos == std::string::npos) break;
                                try {
                                    std::string id_str = manual_state.substr(start_pos, eq_pos - start_pos);
                                    std::string val_str = manual_state.substr(eq_pos + 1, sc_pos - (eq_pos + 1));
                                    ParamID id = std::stoul(id_str, nullptr, 16); ParamValue value = std::stod(val_str);
                                    m_controller->setParamNormalized(id, value);
                                }
                                catch (const std::exception& e) { DbgPrint(_T("MainThread: Error parsing manual state chunk: %hs"), e.what()); }
                                start_pos = sc_pos + 1;
                            }
                        }
                    }
                    else { DbgPrint(_T("MainThread: setState failed, unknown format.")); }
                    restartComponent(kParamValuesChanged | kReloadComponent);
                }
                else { DbgPrint(_T("MainThread: setState failed, no controller or component.")); }
            }
            else if (cmd == "show_gui") {
                ShowGui();
            }
            else if (cmd == "hide_gui") {
                HideGui();
            }
        }
    }

private:
    std::atomic<uint32> m_refCount;
    uint64_t m_uniqueId;
    HINSTANCE m_hInstance;
    HWND m_hGuiWindow;
    std::atomic<bool> m_mainLoopRunning, m_threadsRunning;
    std::atomic<bool> m_isPluginReady;
    HANDLE m_hPipe, m_hShm, m_hEventClientReady, m_hEventHostDone, m_hPipeThread, m_hAudioThread;
    void* m_pSharedMem;
    AudioSharedData* m_pAudioData;
    Module::Ptr m_module;
    PlugProvider* m_plugProvider;
    IComponent* m_component;
    IAudioProcessor* m_processor;
    IEditController* m_controller;
    IPlugView* m_plugView;
    WindowController* m_windowController;
    std::mutex m_commandMutex;
    std::vector<std::string> m_commandQueue;
    std::mutex m_syncMutex;
    std::condition_variable m_syncCv;
    std::string m_syncCommand;
    std::string m_syncResult;
    bool m_syncSuccess;
};

bool VstHost::LoadPlugin(const std::string& path, double sampleRate, int32 blockSize)
{
    DbgPrint(_T("LoadPlugin: Loading plugin on main thread: %hs"), path.c_str());
    ReleasePlugin();

    std::string error;
    m_module = Module::create(path, error);
    if (!m_module)
    {
        DbgPrint(_T("LoadPlugin: Could not create Module. Error: %hs"), error.c_str());
        return false;
    }

    auto factory = m_module->getFactory();
    ClassInfo audioProcessorClass;
    bool found = false;
    for (const auto& classInfo : factory.classInfos())
    {
        if (classInfo.category() == kVstAudioEffectClass)
        {
            audioProcessorClass = classInfo;
            found = true;
            break;
        }
    }

    if (!found)
    {
        DbgPrint(_T("LoadPlugin: No VST3 Audio Module Class found."));
        m_module.reset();
        return false;
    }
    DbgPrint(_T("LoadPlugin: Creating and initializing PlugProvider..."));
    m_plugProvider = new PlugProvider(factory, audioProcessorClass, true);
    m_component = m_plugProvider->getComponent();
    m_controller = m_plugProvider->getController();

    if (!m_component || !m_controller) {
        DbgPrint(_T("LoadPlugin: Failed to get Component/Controller from PlugProvider. Initialization likely failed."));
        ReleasePlugin();
        return false;
    }

    DbgPrint(_T("LoadPlugin: PlugProvider initialization seems successful."));
    m_controller->setComponentHandler(this);

    m_component->queryInterface(IAudioProcessor::iid, (void**)&m_processor);
    if (!m_processor) {
        DbgPrint(_T("LoadPlugin: Failed to get IAudioProcessor."));
        ReleasePlugin();
        return false;
    }

    m_processor->setProcessing(false);
    ProcessSetup setup{ kRealtime, kSample32, blockSize, sampleRate };
    if (m_processor->setupProcessing(setup) != kResultOk) {
        DbgPrint(_T("LoadPlugin: setupProcessing failed."));
        ReleasePlugin();
        return false;
    }

    int32 numIn = m_component->getBusCount(kAudio, kInput);
    int32 numOut = m_component->getBusCount(kAudio, kOutput);
    if (numIn > 0) m_component->activateBus(kAudio, kInput, 0, true);
    if (numOut > 0) m_component->activateBus(kAudio, kOutput, 0, true);

    m_component->setActive(true);
    m_processor->setProcessing(true);

    m_isPluginReady = true;

    String128 name;
    Steinberg::str8ToStr16(name, audioProcessorClass.name().c_str(), 128);
    DbgPrint(_T("LoadPlugin: Plugin loaded and setup: %s. Ready for processing."), (wchar_t*)name);
    return true;
}

void VstHost::ReleasePlugin()
{
    DbgPrint(_T("ReleasePlugin: Releasing current plugin..."));
    m_isPluginReady = false;
    if (m_hGuiWindow && IsWindow(m_hGuiWindow)) DestroyWindow(m_hGuiWindow);
    if (m_plugProvider)
    {
        delete m_plugProvider;
        m_plugProvider = nullptr;
    }
    m_processor = nullptr;
    m_component = nullptr;
    m_controller = nullptr;
    m_module.reset();
    DbgPrint(_T("ReleasePlugin: Plugin released."));
}
void VstHost::ProcessAudioBlock() {
    if (!m_processor || !m_pAudioData || m_pAudioData->numSamples <= 0 || !m_component) return;
    ProcessData data;
    data.numSamples = m_pAudioData->numSamples;
    data.symbolicSampleSize = kSample32;
    float* pSharedAudio = (float*)((char*)m_pSharedMem + sizeof(AudioSharedData));
    std::vector<AudioBusBuffers> inBuf, outBuf;
    std::vector<std::vector<float*>> inPtrs, outPtrs;
    int32 numInBuses = m_component->getBusCount(kAudio, kInput);
    int32 numOutBuses = m_component->getBusCount(kAudio, kOutput);
    data.numInputs = numInBuses; data.numOutputs = numOutBuses;
    data.inputs = nullptr; data.outputs = nullptr;
    if (numInBuses > 0) {
        inBuf.resize(numInBuses); inPtrs.resize(numInBuses);
        for (int32 i = 0; i < numInBuses; ++i) {
            BusInfo busInfo; m_component->getBusInfo(kAudio, kInput, i, busInfo);
            inBuf[i].numChannels = busInfo.channelCount; inPtrs[i].resize(busInfo.channelCount, nullptr);
            if (i == 0) { if (busInfo.channelCount > 0) inPtrs[i][0] = pSharedAudio; if (busInfo.channelCount > 1) inPtrs[i][1] = pSharedAudio + MAX_BLOCK_SIZE; }
            inBuf[i].channelBuffers32 = inPtrs[i].data();
        }
        data.inputs = inBuf.data();
    }
    if (numOutBuses > 0) {
        outBuf.resize(numOutBuses); outPtrs.resize(numOutBuses);
        for (int32 i = 0; i < numOutBuses; ++i) {
            BusInfo busInfo; m_component->getBusInfo(kAudio, kOutput, i, busInfo);
            outBuf[i].numChannels = busInfo.channelCount; outPtrs[i].resize(busInfo.channelCount, nullptr);
            if (i == 0) { if (busInfo.channelCount > 0) outPtrs[i][0] = pSharedAudio + 2 * MAX_BLOCK_SIZE; if (busInfo.channelCount > 1) outPtrs[i][1] = pSharedAudio + 3 * MAX_BLOCK_SIZE; }
            outBuf[i].channelBuffers32 = outPtrs[i].data();
        }
        data.outputs = outBuf.data();
    }
    if (m_processor->process(data) != kResultOk) { DbgPrint(_T("ProcessAudioBlock: Error during audio processing.")); }
}


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
#ifdef _DEBUG
    AllocConsole();
    FILE* conout;
    freopen_s(&conout, "CONOUT$", "w", stdout);
    freopen_s(&conout, "CONOUT$", "w", stderr);
#endif

    DbgPrint(_T("WinMain: VST Host Application Starting..."));
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) { DbgPrint(_T("WinMain: Failed to initialize COM library.")); return 1; }
    uint64_t unique_id = 0;
    try {
        if (lpCmdLine && *lpCmdLine) {
            unique_id = std::stoull(lpCmdLine);
        }
    }
    catch (...) {
        unique_id = GetCurrentProcessId();
    }
    DbgPrint(_T("WinMain: Parsed unique_id: %llu"), unique_id);
    g_pVstHost = new VstHost(hInstance, unique_id);
    PluginContextFactory::instance().setPluginContext(static_cast<IHostApplication*>(g_pVstHost));

    if (g_pVstHost->Initialize()) {
        DbgPrint(_T("WinMain: Entering message loop..."));
        g_pVstHost->RunMessageLoop();
    }
    else {
        DbgPrint(_T("WinMain: VstHost initialization FAILED."));
    }

    DbgPrint(_T("WinMain: VST Host Application Exiting..."));
    if (g_pVstHost) {
        g_pVstHost->Cleanup();
        g_pVstHost->release();
        g_pVstHost = nullptr;
    }
    PluginContextFactory::instance().setPluginContext(nullptr);

#ifdef _DEBUG
    if (conout) fclose(conout);
    FreeConsole();
#endif
    CoUninitialize();
    return 0;
}