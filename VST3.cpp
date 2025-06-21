#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <tchar.h>
#include <string>
#include <vector>
#include <atomic>
#include <cstdio>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <commdlg.h>
using byte = int8_t;
#include <exedit.hpp>

#ifdef _DEBUG
#define DbgPrint(format, ...) do { \
    TCHAR buffer[512]; \
    _stprintf_s(buffer, _T("[VstPlugin][%hs:%d] ") _T(format) _T("\n"), __FUNCTION__, __LINE__, ##__VA_ARGS__); \
    OutputDebugString(buffer); \
} while(0)
#else
#define DbgPrint(format, ...)
#endif

// =================================================================
// 定数・グローバル変数
// =================================================================
const TCHAR* PIPE_NAME_BASE = _T("\\\\.\\pipe\\AviUtlVstBridge");
const TCHAR* SHARED_MEM_NAME_BASE = _T("Local\\AviUtlVstSharedAudio");
const TCHAR* EVENT_CLIENT_READY_NAME_BASE = _T("Local\\AviUtlVstClientReady");
const TCHAR* EVENT_HOST_DONE_NAME_BASE = _T("Local\\AviUtlVstHostDone");
const int MAX_BLOCK_SIZE = 2048;
const int STATE_B64_MAX_LEN = 65536;

// =================================================================
// 構造体・クラス定義
// =================================================================
#pragma pack(push, 1)
struct AudioSharedData {
    double  sampleRate;
    int32_t numSamples;
    int32_t numChannels;
};
struct Exdata {
    TCHAR vst_path[MAX_PATH];
    char state_b64[STATE_B64_MAX_LEN];
};
#pragma pack(pop)
const int SHARED_MEM_TOTAL_SIZE = sizeof(AudioSharedData) + (4 * MAX_BLOCK_SIZE * sizeof(float));

class HostState;
bool SendCommandToHost(HostState& state, const char* command, char* response, DWORD responseSize);

class HostState {
public:
    uint64_t unique_id = 0;
    std::atomic<bool> host_running = false;
    std::atomic<bool> gui_visible = false;
    PROCESS_INFORMATION pi = {};
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    HANDLE hShm = NULL; void* pSharedMem = nullptr;
    HANDLE hEventClientReady = NULL; HANDLE hEventHostDone = NULL;

    HostState() {
        uint64_t tick = GetTickCount64();
        uint32_t pid = GetCurrentProcessId();
        static std::atomic<uint32_t> counter = 0;
        unique_id = (tick << 32) | (static_cast<uint64_t>(pid & 0xFFFF)) << 16 | (counter++ & 0xFFFF);
    }
    ~HostState() {
        if (!host_running) return;
        DbgPrint(_T("~HostState: Cleaning up for unique_id %llu, ProcessID %lu"), unique_id, pi.dwProcessId);
        char response[64] = {};
        SendCommandToHost(*this, "exit\n", response, sizeof(response));
        if (pi.hProcess) {
            DWORD waitResult = WaitForSingleObject(pi.hProcess, 2000);
            if (waitResult == WAIT_TIMEOUT) {
                DbgPrint(_T("Host process %lu did not exit in time, terminating it."), pi.dwProcessId);
                TerminateProcess(pi.hProcess, 1);
            }
            else {
                DbgPrint(_T("Host process %lu exited gracefully."), pi.dwProcessId);
            }
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        if (hPipe != INVALID_HANDLE_VALUE) CloseHandle(hPipe);
        if (pSharedMem) UnmapViewOfFile(pSharedMem);
        if (hShm) CloseHandle(hShm);
        if (hEventClientReady) CloseHandle(hEventClientReady);
        if (hEventHostDone) CloseHandle(hEventHostDone);
        host_running = false;
    }
};
std::mutex g_states_mutex;
std::unordered_map<uint32_t, std::unique_ptr<HostState>> g_host_states;

// =================================================================
// 拡張編集プラグイン定義
// =================================================================
#define PLUGIN_VERSION "v0.0.1"
#define PLUGIN_AUTHOR "BOOK25"
#define FILTER_NAME "VST3 Host"
#define FILTER_INFO_FMT(name, ver, author) (name " " ver " by " author)
constexpr char filter_name[] = FILTER_NAME;
constexpr char info[] = FILTER_INFO_FMT(FILTER_NAME, PLUGIN_VERSION, PLUGIN_AUTHOR);
namespace idx_check { enum id : int { select_vst, toggle_gui, count }; }
const char* check_names[] = { "VST3プラグインを選択", "プラグインGUIを表示" };
const int32_t check_default[] = { -1, -1 };
const Exdata exdata_def = { _T(""), "" };
BOOL func_proc(ExEdit::Filter* efp, ExEdit::FilterProcInfo* efpip);
BOOL func_init(ExEdit::Filter* efp);
BOOL func_exit(ExEdit::Filter* efp);
BOOL func_WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, AviUtl::EditHandle* editp, ExEdit::Filter* efp);
int32_t func_window_init(HINSTANCE hinstance, HWND hwnd, int y, int base_id, int sw_param, ExEdit::Filter* efp);
bool LaunchHostProcess(ExEdit::Filter* efp, ExEdit::FilterProcInfo* efpip, HostState& state);
consteval ExEdit::Filter filter_template(ExEdit::Filter::Flag flag) {
    return { .flag = flag, .name = const_cast<char*>(filter_name), .check_n = idx_check::count, .check_name = const_cast<char**>(check_names), .check_default = const_cast<int*>(check_default), .func_proc = &func_proc, .func_init = &func_init, .func_exit = &func_exit, .func_WndProc = &func_WndProc, .exdata_size = sizeof(Exdata), .information = const_cast<char*>(info), .func_window_init = &func_window_init, .exdata_def = const_cast<Exdata*>(&exdata_def), .exdata_use = nullptr, };
}
inline constinit auto filter = filter_template(ExEdit::Filter::Flag::Audio);
inline constinit auto effect = filter_template(ExEdit::Filter::Flag::Audio | ExEdit::Filter::Flag::Effect | ExEdit::Filter::Flag::Unaddable);

// =================================================================
// フィルター関数実装 (変更なし)
// =================================================================
BOOL SaveStateIfGuiVisible(ExEdit::Filter* efp) {
    uint32_t object_id = static_cast<uint32_t>(efp->processing);
    auto* exdata = reinterpret_cast<Exdata*>(efp->exdata_ptr);

    std::lock_guard<std::mutex> lock(g_states_mutex);
    auto it = g_host_states.find(object_id);
    if (it == g_host_states.end() || !it->second->host_running || !it->second->gui_visible) {
        return FALSE;
    }

    auto& state = *it->second;
    DbgPrint(_T("Saving state for object %u because GUI is open."), object_id);
    std::vector<char> state_response(STATE_B64_MAX_LEN + 100);
    if (SendCommandToHost(state, "get_state\n", state_response.data(), (DWORD)state_response.size())) {
        if (strncmp(state_response.data(), "OK ", 3) == 0) {
            efp->exfunc->set_undo(efp->processing, 0);
            char* state_ptr = state_response.data() + 3;
            size_t len = strlen(state_ptr);
            if (len > 0 && state_ptr[len - 1] == '\n') {
                state_ptr[len - 1] = '\0';
            }
            strncpy_s(exdata->state_b64, sizeof(exdata->state_b64), state_ptr, _TRUNCATE);
            DbgPrint(_T("State saved for object %u. Length: %zu"), object_id, strlen(exdata->state_b64));
            return TRUE;
        }
        else {
            DbgPrint(_T("Failed to get state: %hs"), state_response.data());
        }
    }
    else {
        DbgPrint(_T("SendCommandToHost for get_state failed for object %u."), object_id);
    }
    return FALSE;
}
BOOL func_proc(ExEdit::Filter* efp, ExEdit::FilterProcInfo* efpip) {
    auto* exdata = reinterpret_cast<Exdata*>(efp->exdata_ptr);
    uint32_t object_id = static_cast<uint32_t>(efp->processing);

    if (_tcslen(exdata->vst_path) == 0 || efpip->audio_n == 0) return TRUE;

    std::lock_guard<std::mutex> lock(g_states_mutex);
    if (g_host_states.find(object_id) == g_host_states.end()) {
        g_host_states[object_id] = std::make_unique<HostState>();
    }
    auto& state = *g_host_states[object_id];

    if (!state.host_running) {
        if (!LaunchHostProcess(efp, efpip, state)) {
            DbgPrint(_T("func_proc: Host launch failed for obj %u. Bypassing."), object_id);
            g_host_states.erase(object_id); return TRUE;
        }
    }
    if (!state.pSharedMem) return TRUE;
    short* audio_in = (efp == &effect) ? efpip->audio_temp : efpip->audio_p;
    short* audio_out = (efp == &effect) ? efpip->audio_data : efpip->audio_p;
    if (efp != &effect) {
        memcpy(efpip->audio_temp, audio_in, efpip->audio_n * efpip->audio_ch * sizeof(short));
        audio_in = efpip->audio_temp;
    }
    int total_samples = efpip->audio_n;
    auto* shared_data = static_cast<AudioSharedData*>(state.pSharedMem);
    auto* shared_buffer = reinterpret_cast<float*>(static_cast<char*>(state.pSharedMem) + sizeof(AudioSharedData));

    for (int samples_processed = 0; samples_processed < total_samples; ) {
        int samples_to_process = std::min(total_samples - samples_processed, MAX_BLOCK_SIZE);
        float* in_l = shared_buffer; float* in_r = shared_buffer + MAX_BLOCK_SIZE;
        for (int i = 0; i < samples_to_process; ++i) {
            if (efpip->audio_ch == 2) {
                in_l[i] = static_cast<float>(audio_in[(samples_processed + i) * 2]) / 32768.0f;
                in_r[i] = static_cast<float>(audio_in[(samples_processed + i) * 2 + 1]) / 32768.0f;
            }
            else {
                in_l[i] = static_cast<float>(audio_in[samples_processed + i]) / 32768.0f; in_r[i] = in_l[i];
            }
        }
        shared_data->sampleRate = efpip->audio_rate;
        shared_data->numSamples = samples_to_process;
        shared_data->numChannels = efpip->audio_ch;
        ResetEvent(state.hEventHostDone); SetEvent(state.hEventClientReady);
        DWORD waitResult = WaitForSingleObject(state.hEventHostDone, 500);

        if (waitResult == WAIT_OBJECT_0) {
            float* out_l = shared_buffer + 2 * MAX_BLOCK_SIZE; float* out_r = shared_buffer + 3 * MAX_BLOCK_SIZE;
            for (int i = 0; i < samples_to_process; ++i) {
                float sample_l = std::clamp(out_l[i], -1.0f, 1.0f); float sample_r = std::clamp(out_r[i], -1.0f, 1.0f);
                if (efpip->audio_ch == 2) {
                    audio_out[(samples_processed + i) * 2] = static_cast<short>(sample_l * 32767.0f);
                    audio_out[(samples_processed + i) * 2 + 1] = static_cast<short>(sample_r * 32767.0f);
                }
                else {
                    audio_out[samples_processed + i] = static_cast<short>(((sample_l + sample_r) * 0.5f) * 32767.0f);
                }
            }
        }
        else {
            DbgPrint(_T("Host processing timed out/failed (result: %lu). Bypassing."), waitResult);
            if (waitResult != WAIT_TIMEOUT) {
                DbgPrint(_T("Host process likely terminated. Cleaning up."));
                g_host_states.erase(object_id); return TRUE;
            }
            memcpy(audio_out + samples_processed * efpip->audio_ch, audio_in + samples_processed * efpip->audio_ch, samples_to_process * efpip->audio_ch * sizeof(short));
        }
        samples_processed += samples_to_process;
    }
    return TRUE;
}
BOOL func_init(ExEdit::Filter* efp) { return TRUE; }
BOOL func_exit(ExEdit::Filter* efp) { DbgPrint(_T("Filter exiting. Cleaning up all host processes.")); std::lock_guard<std::mutex> lock(g_states_mutex); g_host_states.clear(); return TRUE; }

BOOL func_WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, AviUtl::EditHandle* editp, ExEdit::Filter* efp) {
    if (message == AviUtl::FilterPlugin::WindowMessage::SaveStart) {
        DbgPrint(_T("WM_EXTENDEDFILTER_SAVE_START received. Checking if state needs to be saved."));
        SaveStateIfGuiVisible(efp);
        return TRUE;
    }

    if (message != ExEdit::ExtendedFilter::Message::WM_EXTENDEDFILTER_COMMAND) return FALSE;

    uint32_t object_id = static_cast<uint32_t>(efp->processing);
    auto* exdata = reinterpret_cast<Exdata*>(efp->exdata_ptr);
    if (LOWORD(wparam) != ExEdit::ExtendedFilter::CommandId::EXTENDEDFILTER_PUSH_BUTTON) return FALSE;

    bool needs_update = false;

    switch (HIWORD(wparam)) {
    case idx_check::select_vst: {
        DbgPrint(_T("Button 'select_vst' clicked."));
        TCHAR szFile[MAX_PATH] = { 0 };
        OPENFILENAME ofn = { 0 };
        ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = efp->exedit_fp->hwnd;
        ofn.lpstrFile = szFile; ofn.nMaxFile = sizeof(szFile) / sizeof(TCHAR);
        ofn.lpstrFilter = _T("VST3 Plugin (*.vst3)\0*.vst3\0All Files (*.*)\0*.*\0");
        ofn.nFilterIndex = 1; ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (GetOpenFileName(&ofn)) {
            DbgPrint(_T("File selected: %s"), szFile);
            efp->exfunc->set_undo(efp->processing, 0);
            {
                std::lock_guard<std::mutex> lock(g_states_mutex);
                g_host_states.erase(object_id);
            }
            _tcscpy_s(exdata->vst_path, MAX_PATH, szFile);
            exdata->state_b64[0] = '\0';
            needs_update = true;
        }
        else {
            if (CommDlgExtendedError() != 0) {
                DbgPrint(_T("GetOpenFileName failed."));
            }
        }
        break;
    }
    case idx_check::toggle_gui: {
        DbgPrint(_T("Button 'toggle_gui' clicked."));
        std::unique_lock<std::mutex> lock(g_states_mutex);
        auto it = g_host_states.find(object_id);

        if (it == g_host_states.end() || !it->second->host_running) {
            lock.unlock();
            MessageBox(efp->exedit_fp->hwnd, _T("ホストが起動していません。\n編集中に一度再生すると起動します。"), _T("情報"), MB_OK | MB_ICONINFORMATION);
            return TRUE;
        }

        auto& state = *it->second;
        bool is_hiding = state.gui_visible;
        const char* cmd_str = is_hiding ? "hide_gui\n" : "show_gui\n";
        char response[256];

        if (SendCommandToHost(state, cmd_str, response, sizeof(response))) {
            if (is_hiding) {
                DbgPrint(_T("GUI hidden. Getting state to save."));
                lock.unlock();
                if (SaveStateIfGuiVisible(efp)) {
                }
                lock.lock();
            }
            state.gui_visible = !is_hiding;
            needs_update = true;
        }
        else {
            MessageBox(efp->exedit_fp->hwnd, _T("GUIコマンドの送信に失敗しました。"), _T("エラー"), MB_OK | MB_ICONERROR);
        }
        break;
    }
    }

    if (needs_update) {
        DbgPrint(_T("Requesting filter update."));
        efp->exedit_fp->exfunc->filter_window_update(efp->exedit_fp);
    }

    return TRUE;
}
int32_t func_window_init(HINSTANCE, HWND hwnd, int, int, int sw_param, ExEdit::Filter* efp) {

    DbgPrint(_T("func_window_init called for update."));

    auto* exdata = reinterpret_cast<Exdata*>(efp->exdata_ptr);
    uint32_t object_id = static_cast<uint32_t>(efp->processing);
    HWND hStaticPath = efp->exfunc->get_hwnd(efp->processing, 5, idx_check::select_vst);
    if (hStaticPath) {
        TCHAR display_path[MAX_PATH];
        if (_tcslen(exdata->vst_path) == 0) {
            _tcscpy_s(display_path, _T("（VST3プラグイン未選択）"));
        }
        else {
            const TCHAR* filename = _tcsrchr(exdata->vst_path, _T('\\'));
            _tcscpy_s(display_path, filename ? filename + 1 : exdata->vst_path);
        }
        SetWindowText(hStaticPath, display_path);
    }
    bool gui_is_visible = false;
    {
        std::lock_guard<std::mutex> lock(g_states_mutex);
        if (g_host_states.count(object_id)) {
            gui_is_visible = g_host_states.at(object_id)->gui_visible;
        }
    }
    HWND hBtnGui = efp->exfunc->get_hwnd(efp->processing, 4, idx_check::toggle_gui);
    if (hBtnGui) {
        SetWindowTextA(hBtnGui, gui_is_visible ? "プラグインGUIを非表示" : "プラグインGUIを表示");
    }

    return 0;
}
bool ConnectIPC(HostState& state) {
    TCHAR name[MAX_PATH];
    _stprintf_s(name, _T("%s_%llu"), PIPE_NAME_BASE, state.unique_id);
    for (int i = 0; i < 50; ++i) {
        state.hPipe = CreateFile(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (state.hPipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipe(name, 1000);
        else Sleep(100);
    }
    if (state.hPipe == INVALID_HANDLE_VALUE) { DbgPrint(_T("Pipe open timed out.")); return false; }
    _stprintf_s(name, _T("%s_%llu"), SHARED_MEM_NAME_BASE, state.unique_id);
    state.hShm = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!state.hShm) { DbgPrint(_T("OpenFileMapping failed: %lu"), GetLastError()); return false; }
    state.pSharedMem = MapViewOfFile(state.hShm, FILE_MAP_ALL_ACCESS, 0, 0, SHARED_MEM_TOTAL_SIZE);
    if (!state.pSharedMem) { DbgPrint(_T("MapViewOfFile failed: %lu"), GetLastError()); return false; }
    _stprintf_s(name, _T("%s_%llu"), EVENT_CLIENT_READY_NAME_BASE, state.unique_id);
    state.hEventClientReady = OpenEvent(EVENT_ALL_ACCESS, FALSE, name);
    _stprintf_s(name, _T("%s_%llu"), EVENT_HOST_DONE_NAME_BASE, state.unique_id);
    state.hEventHostDone = OpenEvent(EVENT_ALL_ACCESS, FALSE, name);
    if (!state.hEventClientReady || !state.hEventHostDone) { DbgPrint(_T("OpenEvent failed: %lu"), GetLastError()); return false; }
    return true;
}
bool LaunchHostProcess(ExEdit::Filter* efp, ExEdit::FilterProcInfo* efpip, HostState& state) {
    auto* exdata = reinterpret_cast<Exdata*>(efp->exdata_ptr);
    TCHAR host_path[MAX_PATH];
    GetModuleFileName(efp->exedit_fp->dll_hinst, host_path, MAX_PATH);
    TCHAR* last_slash = _tcsrchr(host_path, _T('\\'));
    if (!last_slash) return false;
    *(last_slash + 1) = _T('\0');
    _tcscat_s(host_path, MAX_PATH, _T("VstHost.exe"));
    DbgPrint(_T("Attempting to launch host from: %s"), host_path);

    TCHAR cmd_line[MAX_PATH * 2];
    _stprintf_s(cmd_line, _T("\"%s\" %llu"), host_path, state.unique_id);
    STARTUPINFO si = { sizeof(si) };
    if (!CreateProcess(NULL, cmd_line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &state.pi)) {
        DbgPrint(_T("CreateProcess failed: %lu."), GetLastError()); return false;
    }
    state.host_running = true;
    if (!ConnectIPC(state)) { DbgPrint(_T("ConnectIPC failed.")); return false; }

    char vst_path_mb[MAX_PATH];
#ifdef UNICODE
    WideCharToMultiByte(CP_UTF8, 0, exdata->vst_path, -1, vst_path_mb, MAX_PATH, NULL, NULL);
#else
    strcpy_s(vst_path_mb, MAX_PATH, exdata->vst_path);
#endif
    char cmd_buffer[1024];
    char response[256];
    sprintf_s(cmd_buffer, "load_plugin \"%s\" %f %d\n", vst_path_mb, (double)efpip->audio_rate, MAX_BLOCK_SIZE);
    if (!SendCommandToHost(state, cmd_buffer, response, sizeof(response)) || strncmp(response, "OK", 2) != 0) {
        DbgPrint(_T("Failed to load plugin. Response: %hs"), response); return false;
    }
    if (strlen(exdata->state_b64) > 0) {
        DbgPrint(_T("Restoring saved state..."));
        std::vector<char> set_cmd(strlen(exdata->state_b64) + 32);
        sprintf_s(set_cmd.data(), set_cmd.size(), "set_state %s\n", exdata->state_b64);
        if (!SendCommandToHost(state, set_cmd.data(), response, sizeof(response)) || strncmp(response, "OK", 2) != 0) {
            DbgPrint(_T("Failed to restore state. Response: %hs"), response);
        }
        else { DbgPrint(_T("State restored successfully.")); }
    }
    DbgPrint(_T("Host launched and plugin configured successfully."));
    return true;
}
bool SendCommandToHost(HostState& state, const char* command, char* response, DWORD responseSize) {
    if (!state.host_running || state.hPipe == INVALID_HANDLE_VALUE) return false;
    response[0] = '\0';
    DWORD bytesWritten;
    if (!WriteFile(state.hPipe, command, (DWORD)strlen(command), &bytesWritten, NULL)) return false;
    DWORD bytesRead;
    if (!ReadFile(state.hPipe, response, responseSize - 1, &bytesRead, NULL)) return false;
    response[bytesRead] = '\0';
    return true;
}
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hinst);
    return TRUE;
}
EXTERN_C __declspec(dllexport) ExEdit::Filter* const* __stdcall GetFilterTableList() {
    constexpr static ExEdit::Filter* filter_list[] = { &filter, &effect, nullptr };
    return filter_list;
}