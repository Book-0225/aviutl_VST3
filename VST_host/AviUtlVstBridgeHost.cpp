#define _CRT_SECURE_NO_WARNINGS

// --- VST SDK Headers ---
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
#include "public.sdk/source/common/memorystream.h"

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
#include <objbase.h>
#include <sstream>

#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "ole32.lib")

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace VST3::Hosting;

#define MAX_STATE_DATA_LEN 66000
#ifdef _DEBUG
#define DbgPrint(format, ...)                                                                               \
    do                                                                                                      \
    {                                                                                                       \
        TCHAR *tszDbgBuffer = new TCHAR[MAX_STATE_DATA_LEN];                                                \
        if (tszDbgBuffer)                                                                                   \
        {                                                                                                   \
            _stprintf_s(tszDbgBuffer, MAX_STATE_DATA_LEN, _T("[VstHost] ") format _T("\n"), ##__VA_ARGS__); \
            OutputDebugString(tszDbgBuffer);                                                                \
            _tprintf(tszDbgBuffer);                                                                         \
            delete[] tszDbgBuffer;                                                                          \
        }                                                                                                   \
    } while (0)
#else
#define DbgPrint(format, ...)
#endif
const TCHAR *PIPE_NAME_BASE = TEXT("\\\\.\\pipe\\AviUtlVstBridge");
const TCHAR *SHARED_MEM_NAME_BASE = TEXT("Local\\AviUtlVstSharedAudio");
const TCHAR *EVENT_CLIENT_READY_NAME_BASE = TEXT("Local\\AviUtlVstClientReady");
const TCHAR *EVENT_HOST_DONE_NAME_BASE = TEXT("Local\\AviUtlVstHostDone");
const int MAX_BLOCK_SIZE = 2048;
#pragma pack(push, 1)
struct AudioSharedData
{
    double sampleRate;
    int32_t numSamples;
    int32_t numChannels;
};
#pragma pack(pop)
const int FLOAT_SIZE = sizeof(float);
const int BUFFER_BYTES = MAX_BLOCK_SIZE * FLOAT_SIZE;
const int SHARED_MEM_TOTAL_SIZE = sizeof(AudioSharedData) + (4 * BUFFER_BYTES);
std::string base64_encode(const BYTE *data, DWORD data_len)
{
    if (data == nullptr || data_len == 0)
        return "";
    DWORD b64_len = 0;
    if (!CryptBinaryToStringA(data, data_len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &b64_len))
        return "";
    if (b64_len == 0)
        return "";
    std::string b64_str(b64_len, '\0');
    if (!CryptBinaryToStringA(data, data_len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &b64_str[0], &b64_len))
        return "";
    b64_str.resize(b64_len - 1);
    return b64_str;
}
std::vector<BYTE> base64_decode(const std::string &b64_str)
{
    if (b64_str.empty())
        return {};
    DWORD bin_len = 0;
    if (!CryptStringToBinaryA(b64_str.c_str(), (DWORD)b64_str.length(), CRYPT_STRING_BASE64, NULL, &bin_len, NULL, NULL))
        return {};
    if (bin_len == 0)
        return {};
    std::vector<BYTE> bin_data(bin_len);
    if (!CryptStringToBinaryA(b64_str.c_str(), (DWORD)b64_str.length(), CRYPT_STRING_BASE64, bin_data.data(), &bin_len, NULL, NULL))
        return {};
    return bin_data;
}

class WindowController : public IPlugFrame
{
public:
    WindowController(IPlugView *view, HWND parent) : plugView(view), parentWindow(parent), m_refCount(1) {}
    ~WindowController() {}
    void connect()
    {
        if (plugView)
            plugView->setFrame(this);
    }
    void disconnect()
    {
        if (plugView)
            plugView->setFrame(nullptr);
    }
    tresult PLUGIN_API queryInterface(const TUID _iid, void **obj) override
    {
        if (FUnknownPrivate::iidEqual(_iid, IPlugFrame::iid) || FUnknownPrivate::iidEqual(_iid, FUnknown::iid))
        {
            *obj = this;
            addRef();
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++m_refCount; }
    uint32 PLUGIN_API release() override
    {
        if (--m_refCount == 0)
        {
            delete this;
            return 0;
        }
        return m_refCount;
    }
    tresult PLUGIN_API resizeView(IPlugView *view, ViewRect *newSize) override
    {
        if (view == plugView && newSize && parentWindow)
        {
            RECT clientRect = {0, 0, newSize->right - newSize->left, newSize->bottom - newSize->top};
            DWORD style = GetWindowLong(parentWindow, GWL_STYLE);
            DWORD exStyle = GetWindowLong(parentWindow, GWL_EXSTYLE);
            AdjustWindowRectEx(&clientRect, style, FALSE, exStyle);
            int windowWidth = clientRect.right - clientRect.left;
            int windowHeight = clientRect.bottom - clientRect.top;
            SetWindowPos(parentWindow, NULL, 0, 0, windowWidth, windowHeight, SWP_NOMOVE | SWP_NOZORDER);
        }
        return kResultTrue;
    }

private:
    IPlugView *plugView;
    HWND parentWindow;
    std::atomic<uint32> m_refCount;
};

class VstHost : public IHostApplication, public IComponentHandler, public IComponentHandler2
{
public:
    VstHost(HINSTANCE hInstance, uint64_t unique_id);
    ~VstHost();
    tresult PLUGIN_API queryInterface(const TUID _iid, void **obj) override;
    uint32 PLUGIN_API addRef() override;
    uint32 PLUGIN_API release() override;
    tresult PLUGIN_API getName(String128 name) override;
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void **obj) override;
    tresult PLUGIN_API beginEdit(ParamID id) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue valueNormalized) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID id) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override
    {
        DbgPrint(_T("restartComponent(0x%X) called."), flags);
        return kResultOk;
    }
    tresult PLUGIN_API setDirty(TBool state) override { return kResultOk; }
    tresult PLUGIN_API requestOpenEditor(FIDString name = nullptr) override
    {
        if (m_hMainThreadMsgWindow)
            PostMessage(m_hMainThreadMsgWindow, WM_APP_SHOW_GUI, 0, 0);
        return kResultOk;
    }
    tresult PLUGIN_API startGroupEdit() override { return kResultOk; }
    tresult PLUGIN_API finishGroupEdit() override { return kResultOk; }
    bool Initialize();
    void Cleanup();
    void RunMessageLoop();
    void RequestStop();

private:
    static DWORD WINAPI PipeThreadProc(LPVOID p)
    {
        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr))
        {
            ((VstHost *)p)->HandlePipeCommands();
            CoUninitialize();
        }
        return 0;
    }
    static DWORD WINAPI AudioThreadProc(LPVOID p)
    {
        ((VstHost *)p)->HandleAudioProcessing();
        return 0;
    }
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MainThreadMsgWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    void HandlePipeCommands();
    void HandleAudioProcessing();
    void ProcessQueuedCommands();
    void ShowGui();
    void HideGui();
    void OnGuiClose();
    bool InitIPC();
    std::string ProcessCommand(const std::string &full_cmd);
    bool LoadPlugin(const std::string &path, double sampleRate, int32 blockSize);
    void ReleasePlugin();
    void ProcessAudioBlock();
    std::atomic<uint32> m_refCount;
    uint64_t m_uniqueId;
    HINSTANCE m_hInstance;
    std::atomic<bool> m_mainLoopRunning, m_threadsRunning;
    HANDLE m_hPipeThread = NULL, m_hAudioThread = NULL;
    HANDLE m_hPipe = INVALID_HANDLE_VALUE, m_hShm = NULL;
    void *m_pSharedMem = nullptr;
    AudioSharedData *m_pAudioData = nullptr;
    HANDLE m_hEventClientReady = NULL, m_hEventHostDone = NULL;
    std::mutex m_commandMutex, m_syncMutex;
    std::vector<std::string> m_commandQueue;
    std::condition_variable m_syncCv;
    std::string m_syncCommand, m_syncResult;
    bool m_syncSuccess = false;
    Module::Ptr m_module;
    PlugProvider *m_plugProvider = nullptr;
    IComponent *m_component = nullptr;
    IEditController *m_controller = nullptr;
    IAudioProcessor *m_processor = nullptr;
    std::atomic<bool> m_isPluginReady;
    HWND m_hGuiWindow = NULL, m_hMainThreadMsgWindow = NULL;
    FUnknownPtr<IPlugView> m_plugView;
    WindowController *m_windowController = nullptr;
    static const UINT WM_APP_SHOW_GUI = WM_APP + 1;
    static const UINT WM_APP_HIDE_GUI = WM_APP + 2;
};

VstHost *g_pVstHost = nullptr;

VstHost::VstHost(HINSTANCE hInstance, uint64_t unique_id) : m_refCount(1), m_uniqueId(unique_id), m_hInstance(hInstance), m_mainLoopRunning(false), m_threadsRunning(false), m_isPluginReady(false) {}
VstHost::~VstHost() { Cleanup(); }
tresult PLUGIN_API VstHost::queryInterface(const TUID _iid, void **obj)
{
    if (FUnknownPrivate::iidEqual(_iid, IHostApplication::iid))
    {
        *obj = static_cast<IHostApplication *>(this);
        addRef();
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual(_iid, IComponentHandler::iid))
    {
        *obj = static_cast<IComponentHandler *>(this);
        addRef();
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual(_iid, IComponentHandler2::iid))
    {
        *obj = static_cast<IComponentHandler2 *>(this);
        addRef();
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual(_iid, FUnknown::iid))
    {
        *obj = static_cast<IHostApplication *>(this);
        addRef();
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}
uint32 PLUGIN_API VstHost::addRef() { return ++m_refCount; }
uint32 PLUGIN_API VstHost::release()
{
    if (--m_refCount == 0)
    {
        delete this;
        return 0;
    }
    return m_refCount;
}
tresult PLUGIN_API VstHost::getName(String128 name)
{
    Steinberg::str8ToStr16(name, "AviUtl VST3 Host Bridge", 128);
    return kResultOk;
}
tresult PLUGIN_API VstHost::createInstance(TUID cid, TUID iid, void **obj)
{
    FUnknownPtr<IMessage> message;
    if (FUnknownPrivate::iidEqual(cid, IMessage::iid))
    {
        message = owned(new HostMessage);
        if (message)
        {
            return message->queryInterface(iid, obj);
        }
    }
    *obj = nullptr;
    return kNoInterface;
}
bool VstHost::Initialize()
{
    if (!InitIPC())
    {
        DbgPrint(_T("Initialize: InitIPC FAILED."));
        return false;
    }
    m_threadsRunning = true;
    m_hPipeThread = CreateThread(NULL, 0, PipeThreadProc, this, 0, NULL);
    m_hAudioThread = CreateThread(NULL, 0, AudioThreadProc, this, 0, NULL);
    if (!m_hPipeThread || !m_hAudioThread)
    {
        DbgPrint(_T("Initialize: Failed to create worker threads."));
        return false;
    }
    return true;
}
void VstHost::RequestStop() { m_mainLoopRunning = false; }
void VstHost::RunMessageLoop()
{
    WNDCLASS wc = {};
    wc.lpfnWndProc = VstHost::MainThreadMsgWndProc;
    wc.hInstance = m_hInstance;
    wc.lpszClassName = TEXT("VstHostMsgWindowClass");
    RegisterClass(&wc);
    m_hMainThreadMsgWindow = CreateWindow(wc.lpszClassName, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, m_hInstance, this);
    MSG msg;
    m_mainLoopRunning = true;
    while (m_mainLoopRunning && GetMessage(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}
void VstHost::Cleanup()
{
    if (!m_threadsRunning.exchange(false))
        return;
    m_mainLoopRunning = false;
    m_syncCv.notify_all();
    if (m_hPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hPipe);
        m_hPipe = INVALID_HANDLE_VALUE;
    }
    if (m_hEventClientReady)
        SetEvent(m_hEventClientReady);
    if (m_hMainThreadMsgWindow)
        PostMessage(m_hMainThreadMsgWindow, WM_QUIT, 0, 0);
    if (m_hPipeThread)
    {
        WaitForSingleObject(m_hPipeThread, 2000);
        CloseHandle(m_hPipeThread);
        m_hPipeThread = NULL;
    }
    if (m_hAudioThread)
    {
        WaitForSingleObject(m_hAudioThread, 2000);
        CloseHandle(m_hAudioThread);
        m_hAudioThread = NULL;
    }
    ReleasePlugin();
    if (m_pSharedMem)
    {
        UnmapViewOfFile(m_pSharedMem);
        m_pSharedMem = nullptr;
    }
    if (m_hShm)
    {
        CloseHandle(m_hShm);
        m_hShm = NULL;
    }
    if (m_hEventClientReady)
    {
        CloseHandle(m_hEventClientReady);
        m_hEventClientReady = NULL;
    }
    if (m_hEventHostDone)
    {
        CloseHandle(m_hEventHostDone);
        m_hEventHostDone = NULL;
    }
    if (m_hMainThreadMsgWindow)
    {
        DestroyWindow(m_hMainThreadMsgWindow);
        m_hMainThreadMsgWindow = NULL;
    }
}
void VstHost::HandlePipeCommands()
{
    char buffer[MAX_STATE_DATA_LEN];
    DWORD bytesRead;
    while (m_threadsRunning)
    {
        BOOL connected = ConnectNamedPipe(m_hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected)
        {
            if (!m_threadsRunning || m_hPipe == INVALID_HANDLE_VALUE)
                break;
            Sleep(100);
            continue;
        }
        while (m_threadsRunning)
        {
            BOOL success = ReadFile(m_hPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL);
            if (!success || bytesRead == 0)
                break;
            buffer[bytesRead] = '\0';
            std::string cmd(buffer);
            std::string response = ProcessCommand(cmd);
            DWORD bytesWritten;
            WriteFile(m_hPipe, response.c_str(), (DWORD)response.length(), &bytesWritten, NULL);
            if (cmd.rfind("exit", 0) == 0)
                break;
        }
        if (m_hPipe != INVALID_HANDLE_VALUE)
            DisconnectNamedPipe(m_hPipe);
    }
}
void VstHost::HandleAudioProcessing()
{
    while (m_threadsRunning)
    {
        if (WaitForSingleObject(m_hEventClientReady, 1000) != WAIT_OBJECT_0)
            continue;
        if (!m_threadsRunning)
            break;
        ResetEvent(m_hEventClientReady);
        ProcessAudioBlock();
        SetEvent(m_hEventHostDone);
    }
}
LRESULT CALLBACK VstHost::MainThreadMsgWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VstHost *h;
    if (msg == WM_CREATE)
    {
        h = (VstHost *)((CREATESTRUCT *)lp)->lpCreateParams;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)h);
    }
    else
    {
        h = (VstHost *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    }
    if (h)
    {
        switch (msg)
        {
        case WM_APP_SHOW_GUI:
            h->ShowGui();
            return 0;
        case WM_APP_HIDE_GUI:
            h->HideGui();
            return 0;
        case WM_APP:
            h->ProcessQueuedCommands();
            return 0;
        }
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}
std::string VstHost::ProcessCommand(const std::string &full_cmd)
{
    std::string cmd = full_cmd;
    while (!cmd.empty() && isspace(cmd.back()))
        cmd.pop_back();
    if (cmd == "exit")
    {
        RequestStop();
        return "OK: Exit requested.\n";
    }
    if (cmd == "get_state")
    {
        std::string result;
        bool success = false;
        {
            std::unique_lock<std::mutex> lock(m_syncMutex);
            m_syncCommand = cmd;
            if (m_hMainThreadMsgWindow)
                PostMessage(m_hMainThreadMsgWindow, WM_APP, 0, 0);
            m_syncCv.wait(lock, [this]
                          { return m_syncCommand.empty() || !m_mainLoopRunning; });
            if (m_mainLoopRunning)
            {
                success = m_syncSuccess;
                result = m_syncResult;
            }
        }
        if (success)
            return "OK " + result + "\n";
        else
            return "FAIL " + result + "\n";
    }
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        m_commandQueue.push_back(full_cmd);
    }
    if (m_hMainThreadMsgWindow)
        PostMessage(m_hMainThreadMsgWindow, WM_APP, 0, 0);
    return "OK\n";
}
void VstHost::ProcessQueuedCommands()
{
    // 同期コマンド処理 (get_state)
    {
        std::unique_lock<std::mutex> lock(m_syncMutex);
        if (!m_syncCommand.empty())
        {
            if (m_syncCommand == "get_state")
            {
                if (m_plugProvider && m_plugProvider->getComponent() && m_plugProvider->getController())
                {
                    MemoryStream cStream, tStream;
                    m_plugProvider->getComponent()->getState(&cStream);
                    m_plugProvider->getController()->getState(&tStream);
                    if (cStream.getSize() > 0 || tStream.getSize() > 0)
                    {
                        MemoryStream fStream;
                        int32 b;
                        int64 cs = cStream.getSize(), ts = tStream.getSize();
                        fStream.write(&cs, sizeof(cs), &b);
                        if (cs > 0)
                            fStream.write(cStream.getData(), (int32)cs, &b);
                        fStream.write(&ts, sizeof(ts), &b);
                        if (ts > 0)
                            fStream.write(tStream.getData(), (int32)ts, &b);
                        m_syncResult = "VST3_DUAL:" + base64_encode((const BYTE *)fStream.getData(), (DWORD)fStream.getSize());
                        m_syncSuccess = true;
                    }
                    else
                    {
                        m_syncResult = "EMPTY";
                        m_syncSuccess = true;
                    }
                }
                else
                {
                    m_syncResult = "NoPlugin";
                    m_syncSuccess = false;
                }
            }
            m_syncCommand.clear();
            lock.unlock();
            m_syncCv.notify_one();
        }
    }

    // 非同期コマンド処理
    std::vector<std::string> commandsToProcess;
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        if (m_commandQueue.empty())
            return;
        commandsToProcess.swap(m_commandQueue);
    }

    for (const auto &cmd_raw : commandsToProcess)
    {
        std::string cmd = cmd_raw;
        while (!cmd.empty() && isspace((unsigned char)cmd.back()))
        {
            cmd.pop_back();
        }

        if (cmd.rfind("load_plugin ", 0) == 0)
        {
            // --- try-catch ブロックを分割 ---

            std::string path;
            double sr = 44100.0;
            int32 bs = 1024;

            try
            {
                std::string args_str = cmd.substr(12);

                // 1. パスを抽出
                if (!args_str.empty() && args_str.front() == '"')
                {
                    size_t end_quote = args_str.find('"', 1);
                    if (end_quote != std::string::npos)
                    {
                        path = args_str.substr(1, end_quote - 1);
                        args_str = args_str.substr(end_quote + 1);
                    }
                    else
                    {
                        path = args_str.substr(1);
                        args_str.clear();
                    }
                }
                else
                {
                    size_t first_space = args_str.find(' ');
                    if (first_space != std::string::npos)
                    {
                        path = args_str.substr(0, first_space);
                        args_str = args_str.substr(first_space);
                    }
                    else
                    {
                        path = args_str;
                        args_str.clear();
                    }
                }

                // 2. stringstreamでサンプルレートとブロックサイズを安全にパース
                if (!args_str.empty())
                {
                    std::stringstream ss(args_str);
                    ss >> sr >> bs;
                    if (ss.fail() && !ss.eof())
                    {
                        DbgPrint(_T("Warning: stringstream failed to parse all arguments from '%hs'"), args_str.c_str());
                    }
                }
            }
            catch (const std::exception &e)
            {
                // ここで捕捉されたら、引数パース自体が問題
                DbgPrint(_T("MainThread: Exception during argument parsing for load_plugin: %hs"), e.what());
                continue; // 次のコマンドへ
            }

            if (!path.empty())
            {
                DbgPrint(_T("Attempting to load: '%hs', SR: %f, BS: %d"), path.c_str(), sr, bs);
                try
                {
                    LoadPlugin(path, sr, bs);
                }
                catch (const std::exception &e)
                {
                    // ここで捕捉されたら、LoadPlugin関数内が問題
                    DbgPrint(_T("MainThread: Exception inside LoadPlugin function: %hs"), e.what());
                }
            }
        }
        else if (cmd.rfind("set_state ", 0) == 0)
        {
            if (m_plugProvider && m_plugProvider->getComponent() && m_plugProvider->getController())
            {
                std::string data = cmd.substr(10);
                if (data.rfind("VST3_DUAL:", 0) == 0)
                {
                    auto state = base64_decode(data.substr(10));
                    if (!state.empty())
                    {
                        MemoryStream stream(state.data(), state.size());
                        int32 br;
                        int64 cs = 0, ts = 0;
                        stream.read(&cs, sizeof(cs), &br);
                        if (cs > 0)
                        {
                            std::vector<BYTE> d(cs);
                            stream.read(d.data(), (int32)cs, &br);
                            MemoryStream s(d.data(), cs);
                            m_plugProvider->getComponent()->setState(&s);
                        }
                        stream.read(&ts, sizeof(ts), &br);
                        if (ts > 0)
                        {
                            std::vector<BYTE> d(ts);
                            stream.read(d.data(), (int32)ts, &br);
                            MemoryStream s(d.data(), ts);
                            m_plugProvider->getController()->setState(&s);
                        }
                    }
                }
                restartComponent(kParamValuesChanged | kReloadComponent);
            }
        }
        else if (cmd == "show_gui")
        {
            ShowGui();
        }
        else if (cmd == "hide_gui")
        {
            HideGui();
        }
    }
}
// この関数をVstHostクラスの既存のLoadPluginと置き換えてください
bool VstHost::LoadPlugin(const std::string &path, double sampleRate, int32 blockSize)
{
    DbgPrint(_T("LoadPlugin (Corrected): Loading plugin on main thread: %hs"), path.c_str());
    ReleasePlugin(); // 以前のプラグインを安全に解放

    std::string error;
    m_module = Module::create(path, error);
    if (!m_module)
    {
        DbgPrint(_T("LoadPlugin (Corrected): Could not create Module. Error: %hs"), error.c_str());
        return false;
    }

    auto factory = m_module->getFactory();
    ClassInfo audioProcessorClass;
    bool found = false;
    for (auto &classInfo : factory.classInfos())
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
        DbgPrint(_T("LoadPlugin (Corrected): No VST3 Audio Module Class found."));
        m_module.reset();
        return false;
    }

    // PlugProviderはComponentとControllerを内部で作成・初期化・接続します
    // コンストラクタの第3引数がtrueなので、initialize()が自動で呼ばれます
    m_plugProvider = new PlugProvider(factory, audioProcessorClass, true);
    if (!m_plugProvider)
    {
        DbgPrint(_T("LoadPlugin (Corrected): PlugProvider creation failed."));
        m_module.reset(); // m_plugProviderが失敗したらモジュールも解放
        return false;
    }

    // PlugProviderからComponentとControllerへのポインタを取得
    m_component = m_plugProvider->getComponent();
    m_controller = m_plugProvider->getController();

    if (!m_component || !m_controller)
    {
        DbgPrint(_T("LoadPlugin (Corrected): Failed to get Component/Controller from PlugProvider."));
        ReleasePlugin(); // 不完全な状態なので全て解放
        return false;
    }

    // ControllerにHostのComponentHandlerをセット
    m_controller->setComponentHandler(this);

    // IAudioProcessor インターフェースを取得してキャッシュする
    if (m_component->queryInterface(IAudioProcessor::iid, (void **)&m_processor) != kResultOk || !m_processor)
    {
        DbgPrint(_T("LoadPlugin (Corrected): Failed to get IAudioProcessor."));
        ReleasePlugin();
        return false;
    }

    // --- オーディオ処理のセットアップ ---
    m_processor->setProcessing(false); // 念のため一旦停止

    ProcessSetup setup{kRealtime, kSample32, (int32_t)blockSize, sampleRate};
    if (m_processor->setupProcessing(setup) != kResultOk)
    {
        DbgPrint(_T("LoadPlugin (Corrected): setupProcessing failed."));
        ReleasePlugin();
        return false;
    }

    // メインのステレオ入出力バスを有効化
    int32 numIn = m_component->getBusCount(kAudio, kInput);
    int32 numOut = m_component->getBusCount(kAudio, kOutput);
    if (numIn > 0)
        m_component->activateBus(kAudio, kInput, 0, true);
    if (numOut > 0)
        m_component->activateBus(kAudio, kOutput, 0, true);

    // プラグインのアクティブ化
    if (m_component->setActive(true) != kResultOk)
    {
        DbgPrint(_T("LoadPlugin (Corrected): setActive(true) failed."));
        ReleasePlugin();
        return false;
    }

    // オーディオ処理を開始
    if (m_processor->setProcessing(true) != kResultOk)
    {
        DbgPrint(_T("LoadPlugin (Corrected): setProcessing(true) failed."));
        // 処理開始に失敗しても、プラグイン自体はロードされたままにする場合もあるが、ここでは解放する
        ReleasePlugin();
        return false;
    }

    m_isPluginReady = true;

    String128 name;
    Steinberg::str8ToStr16(name, audioProcessorClass.name().c_str(), 128);
    DbgPrint(_T("LoadPlugin (Corrected): Plugin loaded and setup: %s. Ready for processing."), (wchar_t *)name);
    return true;
}

// この関数をVstHostクラスの既存のReleasePluginと置き換えてください
void VstHost::ReleasePlugin()
{
    DbgPrint(_T("ReleasePlugin (Corrected): Releasing current plugin..."));
    m_isPluginReady = false;

    // GUIが開いている場合は、まずそれを閉じる
    HideGui(); // HideGuiは内部でOnGuiCloseを呼び、viewとcontrollerを解放する

    if (m_component)
    {
        // アクティブなプラグインを安全に非アクティブ化する
        if (m_processor)
        {
            m_processor->setProcessing(false);
        }
        m_component->setActive(false);
    }

    // IAudioProcessorインターフェースを解放
    if (m_processor)
    {
        m_processor->release();
        m_processor = nullptr;
    }

    // PlugProviderがComponentとControllerの所有権を持つため、
    // m_componentとm_controllerは単なるポインタ。nullptrに設定するだけで良い。
    m_component = nullptr;
    m_controller = nullptr;

    // PlugProviderを解放すると、内部でComponentとControllerが解放される
    if (m_plugProvider)
    {
        delete m_plugProvider;
        m_plugProvider = nullptr;
    }

    // 最後にモジュールを解放
    m_module.reset();
    DbgPrint(_T("ReleasePlugin (Corrected): Plugin released."));
}

void VstHost::ProcessAudioBlock()
{
    // m_isPluginReadyのチェックと、キャッシュされたm_processorを使用
    if (!m_isPluginReady || !m_processor || !m_component || !m_pAudioData || m_pAudioData->numSamples <= 0)
        return;

    // --- ここから先のロジックはほぼ同じ ---
    ProcessData data;
    data.numSamples = m_pAudioData->numSamples;
    data.symbolicSampleSize = kSample32;

    float *pSharedAudio = (float *)((char *)m_pSharedMem + sizeof(AudioSharedData));

    std::vector<AudioBusBuffers> inBuf, outBuf;
    std::vector<std::vector<float *>> inPtrs, outPtrs;

    int32 numIn = m_component->getBusCount(kAudio, kInput);
    int32 numOut = m_component->getBusCount(kAudio, kOutput);
    data.numInputs = numIn;
    data.numOutputs = numOut;

    if (numIn > 0)
    {
        inBuf.resize(numIn);
        inPtrs.resize(numIn);
        for (int32 i = 0; i < numIn; ++i)
        {
            BusInfo bi;
            m_component->getBusInfo(kAudio, kInput, i, bi);
            inBuf[i].numChannels = bi.channelCount;
            inPtrs[i].resize(bi.channelCount, nullptr); // チャンネル数でリサイズ
            if (i == 0)                                 // Main Stereo Bus
            {
                if (bi.channelCount > 0)
                    inPtrs[i][0] = pSharedAudio;
                if (bi.channelCount > 1)
                    inPtrs[i][1] = pSharedAudio + MAX_BLOCK_SIZE;
            }
            inBuf[i].channelBuffers32 = inPtrs[i].data();
        }
        data.inputs = inBuf.data();
    }

    if (numOut > 0)
    {
        outBuf.resize(numOut);
        outPtrs.resize(numOut);
        for (int32 i = 0; i < numOut; ++i)
        {
            BusInfo bi;
            m_component->getBusInfo(kAudio, kOutput, i, bi);
            outBuf[i].numChannels = bi.channelCount;
            outPtrs[i].resize(bi.channelCount, nullptr); // チャンネル数でリサイズ
            if (i == 0)                                  // Main Stereo Bus
            {
                if (bi.channelCount > 0)
                    outPtrs[i][0] = pSharedAudio + 2 * MAX_BLOCK_SIZE;
                if (bi.channelCount > 1)
                    outPtrs[i][1] = pSharedAudio + 3 * MAX_BLOCK_SIZE;
            }
            outBuf[i].channelBuffers32 = outPtrs[i].data();
        }
        data.outputs = outBuf.data();
    }

    // キャッシュしたm_processorでprocessを呼び出す
    if (m_processor->process(data) != kResultOk)
    {
        DbgPrint(_T("ProcessAudioBlock: Error."));
    }
}
void VstHost::ShowGui()
{
    if (!m_plugProvider)
    {
        DbgPrint(_T("ShowGui: Plugin not loaded."));
        return;
    }
    IEditController *controller = m_plugProvider->getController();
    if (!controller)
    {
        DbgPrint(_T("ShowGui: Controller not available."));
        return;
    }
    if (m_hGuiWindow && IsWindow(m_hGuiWindow))
    {
        ShowWindow(m_hGuiWindow, SW_SHOW);
        SetForegroundWindow(m_hGuiWindow);
        return;
    }
    m_plugView = owned(controller->createView(ViewType::kEditor));
    if (!m_plugView)
    {
        DbgPrint(_T("ShowGui: Failed to create plug-in view."));
        return;
    }
    ViewRect sz;
    if (m_plugView->getSize(&sz) != kResultOk)
    {
        sz = {0, 0, 800, 600};
    }
    RECT wr = {0, 0, sz.right - sz.left, sz.bottom - sz.top};
    AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_APPWINDOW);
    WNDCLASS wc = {};
    wc.lpfnWndProc = VstHost::WndProc;
    wc.hInstance = m_hInstance;
    wc.lpszClassName = TEXT("VstHostGuiWindowClass");
    RegisterClass(&wc);
    m_hGuiWindow = CreateWindowEx(WS_EX_APPWINDOW, wc.lpszClassName, TEXT("VST3 Plugin"), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, m_hInstance, this);
    if (!m_hGuiWindow)
    {
        m_plugView.reset();
        return;
    }
    m_windowController = new WindowController(m_plugView, m_hGuiWindow);
    m_windowController->connect();
    if (m_plugView->attached(m_hGuiWindow, kPlatformTypeHWND) != kResultOk)
    {
        m_windowController->disconnect();
        m_windowController->release();
        m_windowController = nullptr;
        DestroyWindow(m_hGuiWindow);
        m_plugView.reset();
        return;
    }
    ShowWindow(m_hGuiWindow, SW_SHOW);
    UpdateWindow(m_hGuiWindow);
    m_windowController->resizeView(m_plugView, &sz);
}
void VstHost::HideGui()
{
    if (m_hGuiWindow)
        DestroyWindow(m_hGuiWindow);
}
void VstHost::OnGuiClose()
{
    if (m_windowController)
    {
        m_windowController->disconnect();
        m_windowController->release();
        m_windowController = nullptr;
    }
    if (m_plugView)
    {
        m_plugView->removed();
        m_plugView.reset();
    }
    m_hGuiWindow = NULL;
}
LRESULT CALLBACK VstHost::WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VstHost *h;
    if (msg == WM_CREATE)
    {
        h = (VstHost *)((CREATESTRUCT *)lp)->lpCreateParams;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)h);
    }
    else
    {
        h = (VstHost *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    }
    if (h)
    {
        if (msg == WM_DESTROY)
        {
            h->OnGuiClose();
            SetWindowLongPtr(hWnd, GWLP_USERDATA, NULL);
        }
        else if (msg == WM_CLOSE)
        {
            return 0;
        }
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}
bool VstHost::InitIPC()
{
    TCHAR p[MAX_PATH], s[MAX_PATH], er[MAX_PATH], ed[MAX_PATH];
    _stprintf_s(p, _T("%s_%llu"), PIPE_NAME_BASE, m_uniqueId);
    _stprintf_s(s, _T("%s_%llu"), SHARED_MEM_NAME_BASE, m_uniqueId);
    _stprintf_s(er, _T("%s_%llu"), EVENT_CLIENT_READY_NAME_BASE, m_uniqueId);
    _stprintf_s(ed, _T("%s_%llu"), EVENT_HOST_DONE_NAME_BASE, m_uniqueId);
    m_hPipe = CreateNamedPipe(p, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, MAX_STATE_DATA_LEN, MAX_STATE_DATA_LEN, 0, NULL);
    if (m_hPipe == INVALID_HANDLE_VALUE)
        return false;
    m_hShm = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, SHARED_MEM_TOTAL_SIZE, s);
    if (!m_hShm)
        return false;
    m_pSharedMem = MapViewOfFile(m_hShm, FILE_MAP_ALL_ACCESS, 0, 0, SHARED_MEM_TOTAL_SIZE);
    if (!m_pSharedMem)
        return false;
    m_pAudioData = (AudioSharedData *)m_pSharedMem;
    m_hEventClientReady = CreateEvent(NULL, TRUE, FALSE, er);
    m_hEventHostDone = CreateEvent(NULL, FALSE, FALSE, ed);
    if (!m_hEventClientReady || !m_hEventHostDone)
        return false;
    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
#ifdef _DEBUG
    AllocConsole();
    FILE *c;
    freopen_s(&c, "CONOUT$", "w", stdout);
    freopen_s(&c, "CONOUT$", "w", stderr);
#endif
    setlocale(LC_ALL, "C");
    DbgPrint(_T("Locale set to 'C'."));
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
        return 1;
    uint64_t uid = 0;
    try
    {
        if (lpCmdLine && *lpCmdLine)
            uid = std::stoull(lpCmdLine);
    }
    catch (...)
    {
        uid = GetCurrentProcessId();
    }
    g_pVstHost = new VstHost(hInstance, uid);
    PluginContextFactory::instance().setPluginContext(static_cast<IHostApplication *>(g_pVstHost));
    if (g_pVstHost->Initialize())
    {
        g_pVstHost->RunMessageLoop();
    }
    if (g_pVstHost)
    {
        g_pVstHost->release();
        g_pVstHost = nullptr;
    }
    PluginContextFactory::instance().setPluginContext(nullptr);
    CoUninitialize();
#ifdef _DEBUG
    if (c)
        fclose(c);
    FreeConsole();
#endif
    return 0;
}