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

#define WM_APP_UPDATE_GUI (WM_APP + 1)
#ifdef _DEBUG
#define DbgPrint(format, ...)                                                                                             \
    do                                                                                                                    \
    {                                                                                                                     \
        TCHAR buffer[512];                                                                                                \
        _stprintf_s(buffer, _T("[AudioPluginHost][%hs:%d] ") _T(format) _T("\n"), __FUNCTION__, __LINE__, ##__VA_ARGS__); \
        OutputDebugString(buffer);                                                                                        \
    } while (0)
#else
#define DbgPrint(format, ...)
#endif

// =================================================================
// 定数・グローバル変数
// =================================================================
const TCHAR *PIPE_NAME_BASE = _T("\\\\.\\pipe\\AviUtlAudioBridge");
const TCHAR *SHARED_MEM_NAME_BASE = _T("Local\\AviUtlAudioSharedAudio");
const TCHAR *EVENT_CLIENT_READY_NAME_BASE = _T("Local\\AviUtlAudioClientReady");
const TCHAR *EVENT_HOST_DONE_NAME_BASE = _T("Local\\AviUtlAudioHostDone");
const int MAX_BLOCK_SIZE = 2048;
const int STATE_B64_MAX_LEN = 65536;
const int MAX_RESTART_ATTEMPTS = 3;
const int CRASH_LOOP_THRESHOLD_MS = 60000;

// =================================================================
// 構造体・クラス定義
// =================================================================
#pragma pack(push, 1)
struct AudioSharedData
{
    double sampleRate;
    int32_t numSamples;
    int32_t numChannels;
};
struct Exdata
{
    TCHAR plugin_path[MAX_PATH];
    char state_b64[STATE_B64_MAX_LEN];
};
#pragma pack(pop)
const int SHARED_MEM_TOTAL_SIZE = sizeof(AudioSharedData) + (4 * MAX_BLOCK_SIZE * sizeof(float));

class HostState;
bool SendCommandToHost(HostState &state, const char *command, char *response, DWORD responseSize);
bool IsHostAlive(HostState &state);

class HostState
{
public:
    TCHAR loaded_plugin_path[MAX_PATH] = {0};
    uint64_t unique_id = 0;
    std::atomic<bool> host_running = false;
    std::atomic<bool> gui_visible = false;
    std::atomic<bool> crashed_notified = false;
    std::atomic<bool> temporarily_disabled = false;
    std::atomic<int> restart_attempts = 0;
    std::atomic<ULONGLONG> last_crash_time = 0;
    PROCESS_INFORMATION pi = {};
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    HANDLE hShm = NULL;
    void *pSharedMem = nullptr;
    HANDLE hEventClientReady = NULL;
    HANDLE hEventHostDone = NULL;

    HostState()
    {
        uint64_t tick = GetTickCount64();
        uint32_t pid = GetCurrentProcessId();
        static std::atomic<uint32_t> counter = 0;
        unique_id = (tick << 32) | (static_cast<uint64_t>(pid & 0xFFFF)) << 16 | (counter++ & 0xFFFF);
    }
    ~HostState()
    {
        if (!host_running)
            return;
        DbgPrint(_T("~HostState: Cleaning up for unique_id %llu, ProcessID %lu"), unique_id, pi.dwProcessId);
        if (IsHostAlive(*this))
        {
            char response[64] = {};
            SendCommandToHost(*this, "exit\n", response, sizeof(response));
        }
        if (pi.hProcess)
        {
            DWORD waitResult = WaitForSingleObject(pi.hProcess, 2000);
            if (waitResult == WAIT_TIMEOUT)
            {
                DbgPrint(_T("Host process %lu did not exit in time, terminating it."), pi.dwProcessId);
                TerminateProcess(pi.hProcess, 1);
            }
            else
            {
                DbgPrint(_T("Host process %lu exited gracefully."), pi.dwProcessId);
            }
        }
        CleanupResources();
    }

    void CleanupForRestart()
    {
        DbgPrint(_T("Cleaning up resources for restart (unique_id %llu)"), unique_id);
        CleanupResources();
        host_running = false;
        gui_visible = false;
    }

private:
    void CleanupResources()
    {
        if (pi.hProcess)
            CloseHandle(pi.hProcess);
        if (pi.hThread)
            CloseHandle(pi.hThread);
        if (hPipe != INVALID_HANDLE_VALUE)
            CloseHandle(hPipe);
        if (pSharedMem)
            UnmapViewOfFile(pSharedMem);
        if (hShm)
            CloseHandle(hShm);
        if (hEventClientReady)
            CloseHandle(hEventClientReady);
        if (hEventHostDone)
            CloseHandle(hEventHostDone);

        pi = {};
        hPipe = INVALID_HANDLE_VALUE;
        pSharedMem = nullptr;
        hShm = NULL;
        hEventClientReady = NULL;
        hEventHostDone = NULL;
    }
};
std::mutex g_states_mutex;
std::unordered_map<uint32_t, std::unique_ptr<HostState>> g_host_states;

bool IsHostAlive(HostState &state)
{
    if (!state.host_running || state.pi.hProcess == NULL)
    {
        return false;
    }
    DWORD exitCode;
    if (GetExitCodeProcess(state.pi.hProcess, &exitCode))
    {
        return exitCode == STILL_ACTIVE;
    }
    return false;
}

// =================================================================
// 拡張編集プラグイン定義
// =================================================================
#define PLUGIN_VERSION "v0.2.0"
#define PLUGIN_AUTHOR "BOOK25"
#define FILTER_NAME "Audio Plugin Host"
#define FILTER_INFO_FMT(name, ver, author) (name " " ver " by " author)
constexpr char filter_name[] = FILTER_NAME;
constexpr char info[] = FILTER_INFO_FMT(FILTER_NAME, PLUGIN_VERSION, PLUGIN_AUTHOR);
namespace idx_check
{
    enum id : int
    {
        select_plugin,
        toggle_gui,
        count
    };
}
const char *check_names[] = {"プラグインを選択", "プラグインGUIを表示"};
const int32_t check_default[] = {-1, -1};
const Exdata exdata_def = {_T(""), ""};
BOOL func_proc(ExEdit::Filter *efp, ExEdit::FilterProcInfo *efpip);
BOOL func_init(ExEdit::Filter *efp);
BOOL func_exit(ExEdit::Filter *efp);
BOOL func_WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, AviUtl::EditHandle *editp, ExEdit::Filter *efp);
int32_t func_window_init(HINSTANCE hinstance, HWND hwnd, int y, int base_id, int sw_param, ExEdit::Filter *efp);
bool LaunchHostProcess(ExEdit::Filter *efp, ExEdit::FilterProcInfo *efpip, HostState &state);
consteval ExEdit::Filter filter_template(ExEdit::Filter::Flag flag)
{
    return {
        .flag = flag,
        .name = const_cast<char *>(filter_name),
        .check_n = idx_check::count,
        .check_name = const_cast<char **>(check_names),
        .check_default = const_cast<int *>(check_default),
        .func_proc = &func_proc,
        .func_init = &func_init,
        .func_exit = &func_exit,
        .func_WndProc = &func_WndProc,
        .exdata_size = sizeof(Exdata),
        .information = const_cast<char *>(info),
        .func_window_init = &func_window_init,
        .exdata_def = const_cast<Exdata *>(&exdata_def),
        .exdata_use = nullptr,
    };
}
inline constinit auto filter = filter_template(ExEdit::Filter::Flag::Audio);
inline constinit auto effect = filter_template(ExEdit::Filter::Flag::Audio | ExEdit::Filter::Flag::Effect | ExEdit::Filter::Flag::Unaddable);

// =================================================================
// フィルター関数実装
// =================================================================
BOOL SaveStateIfGuiVisible(ExEdit::Filter *efp)
{
    uint32_t object_id = static_cast<uint32_t>(efp->processing);
    auto *exdata = reinterpret_cast<Exdata *>(efp->exdata_ptr);
    std::lock_guard<std::mutex> lock(g_states_mutex);
    auto it = g_host_states.find(object_id);
    if (it == g_host_states.end() || !it->second->host_running || !it->second->gui_visible)
    {
        return FALSE;
    }
    auto &state = *it->second;
    if (!IsHostAlive(state))
    {
        DbgPrint(_T("SaveStateIfGuiVisible: Host is not alive. Cannot save state."));
        return FALSE;
    }
    DbgPrint(_T("Saving state for object %u because GUI is open."), object_id);
    std::vector<char> state_response(STATE_B64_MAX_LEN + 100);
    if (SendCommandToHost(state, "get_state\n", state_response.data(), (DWORD)state_response.size()))
    {
        if (strncmp(state_response.data(), "OK ", 3) == 0)
        {
            efp->exfunc->set_undo(efp->processing, 0);
            char *state_ptr = state_response.data() + 3;
            size_t len = strlen(state_ptr);
            if (len > 0 && state_ptr[len - 1] == '\n')
            {
                state_ptr[len - 1] = '\0';
            }
            strncpy_s(exdata->state_b64, sizeof(exdata->state_b64), state_ptr, _TRUNCATE);
            DbgPrint(_T("State saved for object %u. Length: %zu"), object_id, strlen(exdata->state_b64));
            return TRUE;
        }
        else
        {
            DbgPrint(_T("Failed to get state: %hs"), state_response.data());
        }
    }
    else
    {
        DbgPrint(_T("SendCommandToHost for get_state failed for object %u."), object_id);
    }
    return FALSE;
}

BOOL func_proc(ExEdit::Filter *efp, ExEdit::FilterProcInfo *efpip)
{
    auto *exdata = reinterpret_cast<Exdata *>(efp->exdata_ptr);
    uint32_t object_id = static_cast<uint32_t>(efp->processing);

    if (_tcslen(exdata->plugin_path) == 0 || efpip->audio_n == 0)
    {
        std::lock_guard<std::mutex> lock(g_states_mutex);
        if (g_host_states.count(object_id))
        {
            DbgPrint(_T("Plugin path is empty, cleaning up leftover host for object %u."), object_id);
            g_host_states.erase(object_id);
        }
        return TRUE;
    }

    std::lock_guard<std::mutex> lock(g_states_mutex);
    auto it = g_host_states.find(object_id);
    if (it != g_host_states.end())
    {
        if (_tcscmp(it->second->loaded_plugin_path, exdata->plugin_path) != 0)
        {
            DbgPrint(_T("Plugin path mismatch for object %u. Old: '%s', New: '%s'. Re-launching host."),
                     object_id, it->second->loaded_plugin_path, exdata->plugin_path);
            g_host_states.erase(it);
        }
    }
    if (g_host_states.find(object_id) == g_host_states.end())
    {
        g_host_states[object_id] = std::make_unique<HostState>();
    }
    auto &state = *g_host_states[object_id];
    if (state.temporarily_disabled)
    {
        return TRUE;
    }
    if (state.host_running && !IsHostAlive(state))
    {
        DbgPrint(_T("Host process for object %u has terminated unexpectedly. Handling crash..."), object_id);

        ULONGLONG current_time = GetTickCount64();
        if (current_time - state.last_crash_time < CRASH_LOOP_THRESHOLD_MS)
        {
            state.restart_attempts++;
        }
        else
        {
            state.restart_attempts = 1;
        }
        state.last_crash_time = current_time;

        DbgPrint(_T("Crash detected. Attempt count: %d"), (int)state.restart_attempts);

        if (state.restart_attempts >= MAX_RESTART_ATTEMPTS)
        {
            DbgPrint(_T("Too many restart attempts. Disabling host for object %u."), object_id);
            MessageBox(efp->exedit_fp->hwnd,
                       _T("オーディオ処理ホストの起動に繰り返し失敗しました。\n")
                       _T("このオブジェクトに対する処理を一時的に無効化します。\n")
                       _T("プラグインの選択や設定を確認後、再度プラグインを選択し直してください。"),
                       FILTER_NAME, MB_OK | MB_ICONERROR);

            state.temporarily_disabled = true;
            state.CleanupForRestart();
            PostMessage(efp->exedit_fp->hwnd, WM_APP_UPDATE_GUI, 0, 0);
            return TRUE;
        }
        else
        {
            if (!state.crashed_notified)
            {
                MessageBox(efp->exedit_fp->hwnd, _T("オーディオ処理ホストが予期せず終了しました。\n自動的に再起動を試みます。"), FILTER_NAME, MB_OK | MB_ICONWARNING);
                state.crashed_notified = true;
            }
            state.CleanupForRestart();
            return TRUE;
        }
    }
    if (!state.host_running)
    {
        _tcscpy_s(state.loaded_plugin_path, MAX_PATH, exdata->plugin_path);
        if (!LaunchHostProcess(efp, efpip, state))
        {
            DbgPrint(_T("func_proc: Host launch failed for obj %u. Bypassing."), object_id);
            state.host_running = true;
            state.pi.hProcess = INVALID_HANDLE_VALUE;
            return TRUE;
        }
    }
    if (!state.pSharedMem)
        return TRUE;

    short *audio_in = (efp == &effect) ? efpip->audio_temp : efpip->audio_p;
    short *audio_out = (efp == &effect) ? efpip->audio_data : efpip->audio_p;
    if (efp != &effect)
    {
        memcpy(efpip->audio_temp, audio_in, efpip->audio_n * efpip->audio_ch * sizeof(short));
        audio_in = efpip->audio_temp;
    }
    int total_samples = efpip->audio_n;
    auto *shared_data = static_cast<AudioSharedData *>(state.pSharedMem);
    auto *shared_buffer = reinterpret_cast<float *>(static_cast<char *>(state.pSharedMem) + sizeof(AudioSharedData));

    for (int samples_processed = 0; samples_processed < total_samples;)
    {
        int samples_to_process = std::min(total_samples - samples_processed, MAX_BLOCK_SIZE);
        float *in_l = shared_buffer;
        float *in_r = shared_buffer + MAX_BLOCK_SIZE;
        for (int i = 0; i < samples_to_process; ++i)
        {
            if (efpip->audio_ch == 2)
            {
                in_l[i] = static_cast<float>(audio_in[(samples_processed + i) * 2]) / 32768.0f;
                in_r[i] = static_cast<float>(audio_in[(samples_processed + i) * 2 + 1]) / 32768.0f;
            }
            else
            {
                in_l[i] = static_cast<float>(audio_in[samples_processed + i]) / 32768.0f;
                in_r[i] = in_l[i];
            }
        }
        shared_data->sampleRate = efpip->audio_rate;
        shared_data->numSamples = samples_to_process;
        shared_data->numChannels = efpip->audio_ch;
        ResetEvent(state.hEventHostDone);
        SetEvent(state.hEventClientReady);
        DWORD waitResult = WaitForSingleObject(state.hEventHostDone, 500);

        if (waitResult == WAIT_OBJECT_0)
        {
            float *out_l = shared_buffer + 2 * MAX_BLOCK_SIZE;
            float *out_r = shared_buffer + 3 * MAX_BLOCK_SIZE;
            for (int i = 0; i < samples_to_process; ++i)
            {
                float sample_l = std::clamp(out_l[i], -1.0f, 1.0f);
                float sample_r = std::clamp(out_r[i], -1.0f, 1.0f);
                if (efpip->audio_ch == 2)
                {
                    audio_out[(samples_processed + i) * 2] = static_cast<short>(sample_l * 32767.0f);
                    audio_out[(samples_processed + i) * 2 + 1] = static_cast<short>(sample_r * 32767.0f);
                }
                else
                {
                    audio_out[samples_processed + i] = static_cast<short>(((sample_l + sample_r) * 0.5f) * 32767.0f);
                }
            }
        }
        else
        {
            DbgPrint(_T("Host processing timed out/failed (result: %lu). Bypassing."), waitResult);
            if (!IsHostAlive(state))
            {
                DbgPrint(_T("Host appears to have terminated during processing. Crash will be handled on the next frame."));
                return TRUE;
            }
            memcpy(audio_out + samples_processed * efpip->audio_ch, audio_in + samples_processed * efpip->audio_ch, samples_to_process * efpip->audio_ch * sizeof(short));
            break;
        }
        samples_processed += samples_to_process;
    }

    return TRUE;
}

BOOL func_init(ExEdit::Filter *efp) { return TRUE; }
BOOL func_exit(ExEdit::Filter *efp)
{
    DbgPrint(_T("Filter exiting. Cleaning up all host processes."));
    std::lock_guard<std::mutex> lock(g_states_mutex);
    g_host_states.clear();
    return TRUE;
}
BOOL func_WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, AviUtl::EditHandle *editp, ExEdit::Filter *efp)
{
    if (message == WM_APP_UPDATE_GUI)
    {
        DbgPrint(_T("Received WM_APP_UPDATE_GUI. Calling filter_window_update."));
        efp->exedit_fp->exfunc->filter_window_update(efp->exedit_fp);
        return TRUE;
    }
    if (message == AviUtl::FilterPlugin::WindowMessage::SaveStart)
    {
        DbgPrint(_T("WM_EXTENDEDFILTER_SAVE_START received. Checking if state needs to be saved."));
        SaveStateIfGuiVisible(efp);
        return TRUE;
    }
    if (message != ExEdit::ExtendedFilter::Message::WM_EXTENDEDFILTER_COMMAND)
        return FALSE;
    uint32_t object_id = static_cast<uint32_t>(efp->processing);
    auto *exdata = reinterpret_cast<Exdata *>(efp->exdata_ptr);
    if (LOWORD(wparam) != ExEdit::ExtendedFilter::CommandId::EXTENDEDFILTER_PUSH_BUTTON)
        return FALSE;
    bool needs_update = false;
    switch (HIWORD(wparam))
    {
    case idx_check::select_plugin:
    {
        DbgPrint(_T("Button 'select_plugin' clicked."));
        TCHAR aviutl_dir[MAX_PATH];
        GetModuleFileName(NULL, aviutl_dir, MAX_PATH);
        TCHAR *last_slash = _tcsrchr(aviutl_dir, _T('\\'));
        if (!last_slash)
            break;
        *(last_slash + 1) = _T('\0');
        TCHAR ini_path[MAX_PATH];
        _stprintf_s(ini_path, _T("%s%s\\%s"), aviutl_dir, _T("audio_exe"), _T("audio_plugin_link.ini"));
        TCHAR final_filter[2048] = {0};
        TCHAR *p = final_filter;
        const TCHAR *p_end = final_filter + (sizeof(final_filter) / sizeof(TCHAR)) - 2;
        TCHAR combined_exts[512] = {0};
        if (GetFileAttributes(ini_path) != INVALID_FILE_ATTRIBUTES)
        {
            TCHAR all_keys[2048];
            DWORD bytes_read = GetPrivateProfileString(_T("Mappings"), NULL, _T(""), all_keys, sizeof(all_keys) / sizeof(TCHAR), ini_path);
            if (bytes_read > 0)
            {
                const TCHAR *current_key = all_keys;
                while (*current_key)
                {
                    if (_tcslen(combined_exts) > 0)
                    {
                        _tcscat_s(combined_exts, _T(";"));
                    }
                    _tcscat_s(combined_exts, _T("*"));
                    _tcscat_s(combined_exts, current_key);
                    current_key += _tcslen(current_key) + 1;
                }
                int len = _stprintf_s(p, p_end - p, _T("Audio Plugins (%s)"), combined_exts);
                p += len + 1;
                len = _stprintf_s(p, p_end - p, _T("%s"), combined_exts);
                p += len + 1;
                current_key = all_keys;
                while (*current_key)
                {
                    TCHAR ext_upper[32];
                    _tcscpy_s(ext_upper, current_key + 1);
                    _tcsupr_s(ext_upper);
                    len = _stprintf_s(p, p_end - p, _T("%s Plugins (*%s)"), ext_upper, current_key);
                    p += len + 1;
                    len = _stprintf_s(p, p_end - p, _T("*%s"), current_key);
                    p += len + 1;
                    current_key += _tcslen(current_key) + 1;
                }
            }
        }
        int len = _stprintf_s(p, p_end - p, _T("Executable Host (*.exe)"));
        p += len + 1;
        len = _stprintf_s(p, p_end - p, _T("*.exe"));
        p += len + 1;
        len = _stprintf_s(p, p_end - p, _T("All Files (*.*)"));
        p += len + 1;
        len = _stprintf_s(p, p_end - p, _T("*.*"));
        p += len + 1;
        *p = _T('\0');
        TCHAR szFile[MAX_PATH] = {0};
        OPENFILENAME ofn = {0};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = efp->exedit_fp->hwnd;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile) / sizeof(TCHAR);
        ofn.lpstrFilter = final_filter;
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileName(&ofn))
        {
            DbgPrint(_T("File selected: %s"), szFile);
            efp->exfunc->set_undo(efp->processing, 0);
            {
                std::lock_guard<std::mutex> lock(g_states_mutex);
                g_host_states.erase(object_id);
            }
            _tcscpy_s(exdata->plugin_path, MAX_PATH, szFile);
            exdata->state_b64[0] = '\0';
            needs_update = true;
        }
        else
        {
            if (CommDlgExtendedError() != 0)
            {
                DbgPrint(_T("GetOpenFileName failed with error: %ld"), CommDlgExtendedError());
            }
            else
            {
                DbgPrint(_T("GetOpenFileName cancelled by user."));
            }
        }
        break;
    }
    case idx_check::toggle_gui:
    {
        DbgPrint(_T("Button 'toggle_gui' clicked."));
        std::unique_lock<std::mutex> lock(g_states_mutex);
        auto it = g_host_states.find(object_id);
        if (it == g_host_states.end() || !it->second->host_running)
        {
            lock.unlock();
            MessageBox(efp->exedit_fp->hwnd, _T("ホストが起動していません。\n編集中に一度再生すると起動します。"), _T("情報"), MB_OK | MB_ICONINFORMATION);
            return TRUE;
        }
        auto &state = *it->second;
        if (state.temporarily_disabled)
        {
            lock.unlock();
            MessageBox(efp->exedit_fp->hwnd, _T("ホストは繰り返しクラッシュしたため無効化されています。\nプラグインを再選択してください。"), _T("エラー"), MB_OK | MB_ICONERROR);
            return TRUE;
        }
        if (!IsHostAlive(state))
        {
            lock.unlock();
            MessageBox(efp->exedit_fp->hwnd, _T("ホストプロセスが応答しません。クラッシュした可能性があります。\n再生を再開すると、ホストの再起動が試みられます。"), _T("エラー"), MB_OK | MB_ICONERROR);
            return TRUE;
        }
        bool is_hiding = state.gui_visible;
        const char *cmd_str = is_hiding ? "hide_gui\n" : "show_gui\n";
        char response[256];
        if (SendCommandToHost(state, cmd_str, response, sizeof(response)))
        {
            if (is_hiding)
            {
                DbgPrint(_T("GUI hidden. Getting state to save."));
                lock.unlock();
                SaveStateIfGuiVisible(efp);
                lock.lock();
            }
            state.gui_visible = !is_hiding;
            needs_update = true;
        }
        else
        {
            TCHAR error_msg[256];
            if (!IsHostAlive(state))
            {
                _tcscpy_s(error_msg, _T("ホストプロセスが応答しません。クラッシュした可能性があります。\n再生を再開すると、ホストの再起動が試みられます。"));
            }
            else
            {
                _tcscpy_s(error_msg, _T("GUIコマンドの送信に失敗しました。ホストがフリーズしている可能性があります。"));
            }
            lock.unlock();
            MessageBox(efp->exedit_fp->hwnd, error_msg, _T("エラー"), MB_OK | MB_ICONERROR);
        }
        break;
    }
    }
    if (needs_update)
    {
        DbgPrint(_T("Requesting filter update."));
        efp->exedit_fp->exfunc->filter_window_update(efp->exedit_fp);
    }
    return TRUE;
}
int32_t func_window_init(HINSTANCE, HWND, int, int, int, ExEdit::Filter *efp)
{
    DbgPrint(_T("func_window_init called for update."));
    auto *exdata = reinterpret_cast<Exdata *>(efp->exdata_ptr);
    uint32_t object_id = static_cast<uint32_t>(efp->processing);
    HWND hStaticPath = efp->exfunc->get_hwnd(efp->processing, 5, idx_check::select_plugin);
    if (hStaticPath)
    {
        TCHAR display_path[MAX_PATH];
        if (_tcslen(exdata->plugin_path) == 0)
        {
            _tcscpy_s(display_path, _T("（プラグイン未選択）"));
        }
        else
        {
            const TCHAR *filename = _tcsrchr(exdata->plugin_path, _T('\\'));
            _tcscpy_s(display_path, filename ? filename + 1 : exdata->plugin_path);
        }
        SetWindowText(hStaticPath, display_path);
    }
    bool gui_is_visible = false;
    bool is_disabled = false;
    {
        std::lock_guard<std::mutex> lock(g_states_mutex);
        if (g_host_states.count(object_id))
        {
            auto &state = g_host_states.at(object_id);
            gui_is_visible = state->gui_visible;
            is_disabled = state->temporarily_disabled;
        }
    }
    HWND hBtnGui = efp->exfunc->get_hwnd(efp->processing, 4, idx_check::toggle_gui);
    if (hBtnGui)
    {
        if (is_disabled)
        {
            SetWindowTextA(hBtnGui, "無効化中 (再選択して下さい)");
            EnableWindow(hBtnGui, FALSE);
        }
        else
        {
            SetWindowTextA(hBtnGui, gui_is_visible ? "プラグインGUIを非表示" : "プラグインGUIを表示");
            EnableWindow(hBtnGui, TRUE);
        }
    }
    return 0;
}
bool ConnectIPC(HostState &state)
{
    TCHAR name[MAX_PATH];
    _stprintf_s(name, _T("%s_%llu"), PIPE_NAME_BASE, state.unique_id);
    for (int i = 0; i < 50; ++i)
    {
        state.hPipe = CreateFile(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (state.hPipe != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() == ERROR_PIPE_BUSY)
            WaitNamedPipe(name, 1000);
        else
            Sleep(100);
    }
    if (state.hPipe == INVALID_HANDLE_VALUE)
    {
        DbgPrint(_T("Pipe open timed out."));
        return false;
    }
    _stprintf_s(name, _T("%s_%llu"), SHARED_MEM_NAME_BASE, state.unique_id);
    state.hShm = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!state.hShm)
    {
        DbgPrint(_T("OpenFileMapping failed: %lu"), GetLastError());
        return false;
    }
    state.pSharedMem = MapViewOfFile(state.hShm, FILE_MAP_ALL_ACCESS, 0, 0, SHARED_MEM_TOTAL_SIZE);
    if (!state.pSharedMem)
    {
        DbgPrint(_T("MapViewOfFile failed: %lu"), GetLastError());
        return false;
    }
    _stprintf_s(name, _T("%s_%llu"), EVENT_CLIENT_READY_NAME_BASE, state.unique_id);
    state.hEventClientReady = OpenEvent(EVENT_ALL_ACCESS, FALSE, name);
    _stprintf_s(name, _T("%s_%llu"), EVENT_HOST_DONE_NAME_BASE, state.unique_id);
    state.hEventHostDone = OpenEvent(EVENT_ALL_ACCESS, FALSE, name);
    if (!state.hEventClientReady || !state.hEventHostDone)
    {
        DbgPrint(_T("OpenEvent failed: %lu"), GetLastError());
        return false;
    }
    return true;
}

bool LaunchHostProcess(ExEdit::Filter *efp, ExEdit::FilterProcInfo *efpip, HostState &state)
{
    auto *exdata = reinterpret_cast<Exdata *>(efp->exdata_ptr);
    TCHAR msg[MAX_PATH + 256];
    TCHAR host_path[MAX_PATH];
    bool is_standalone_exe = false;

    const TCHAR *extension = _tcsrchr(exdata->plugin_path, _T('.'));
    if (!extension)
    {
        _stprintf_s(msg, _T("プラグインパスに拡張子が含まれていません。\nパス: %s"), exdata->plugin_path);
        MessageBox(efp->exedit_fp->hwnd, msg, _T("設定エラー"), MB_OK | MB_ICONERROR);
        return false;
    }

    if (_tcsicmp(extension, _T(".exe")) == 0)
    {
        is_standalone_exe = true;
        _tcscpy_s(host_path, MAX_PATH, exdata->plugin_path);
        DbgPrint(_T("Standalone executable host selected: %s"), host_path);
    }
    else
    {
        TCHAR aviutl_dir[MAX_PATH];
        GetModuleFileName(NULL, aviutl_dir, MAX_PATH);
        TCHAR *last_slash = _tcsrchr(aviutl_dir, _T('\\'));
        if (!last_slash)
            return false;
        *(last_slash + 1) = _T('\0');
        TCHAR audio_exe_dir[MAX_PATH];
        _stprintf_s(audio_exe_dir, _T("%s%s"), aviutl_dir, _T("audio_exe"));
        TCHAR ini_path[MAX_PATH];
        _stprintf_s(ini_path, _T("%s\\%s"), audio_exe_dir, _T("audio_plugin_link.ini"));
        if (GetFileAttributes(ini_path) == INVALID_FILE_ATTRIBUTES)
        {
            _stprintf_s(msg, _T("設定ファイルが見つかりません。\nパス: %s"), ini_path);
            MessageBox(efp->exedit_fp->hwnd, msg, _T("設定エラー"), MB_OK | MB_ICONERROR);
            return false;
        }
        TCHAR host_exe_name[MAX_PATH];
        GetPrivateProfileString(_T("Mappings"), extension, _T(""), host_exe_name, MAX_PATH, ini_path);
        if (_tcslen(host_exe_name) == 0)
        {
            _stprintf_s(msg, _T("設定ファイルに拡張子 '%s' の定義がありません。\n\n%s の [Mappings] セクションに\n%s=ホスト名.exe\nのように追記してください。"), extension, ini_path, extension);
            MessageBox(efp->exedit_fp->hwnd, msg, _T("設定エラー"), MB_OK | MB_ICONERROR);
            return false;
        }
        _stprintf_s(host_path, _T("%s\\%s"), audio_exe_dir, host_exe_name);
    }
    if (GetFileAttributes(host_path) == INVALID_FILE_ATTRIBUTES)
    {
        _stprintf_s(msg, _T("指定されたホストプログラムが見つかりません。\nパス: %s"), host_path);
        MessageBox(efp->exedit_fp->hwnd, msg, _T("起動エラー"), MB_OK | MB_ICONERROR);
        return false;
    }

    DbgPrint(_T("Attempting to launch host from: %s"), host_path);
    TCHAR cmd_line[MAX_PATH * 4];
    _stprintf_s(cmd_line, _T("\"%s\" -uid %llu -pipe \"%s\" -shm \"%s\" -event_ready \"%s\" -event_done \"%s\""), host_path, state.unique_id, PIPE_NAME_BASE, SHARED_MEM_NAME_BASE, EVENT_CLIENT_READY_NAME_BASE, EVENT_HOST_DONE_NAME_BASE);
    DbgPrint(_T("Launching host with command line: %s"), cmd_line);

    STARTUPINFO si = {sizeof(si)};
    if (!CreateProcess(NULL, cmd_line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &state.pi))
    {
        DbgPrint(_T("CreateProcess failed: %lu."), GetLastError());
        return false;
    }

    state.host_running = true;
    if (!ConnectIPC(state))
    {
        DbgPrint(_T("ConnectIPC failed."));
        CloseHandle(state.pi.hProcess);
        CloseHandle(state.pi.hThread);
        state.pi = {};
        return false;
    }

    char response[256];
    if (is_standalone_exe)
    {
        std::vector<char> cmd_buffer;
        if (strlen(exdata->state_b64) > 0)
        {
            size_t state_len = strlen(exdata->state_b64);
            cmd_buffer.resize(state_len + 256);
            sprintf_s(cmd_buffer.data(), cmd_buffer.size(), "init_with_state %f %d %s\n", (double)efpip->audio_rate, MAX_BLOCK_SIZE, exdata->state_b64);
        }
        else
        {
            cmd_buffer.resize(256);
            sprintf_s(cmd_buffer.data(), cmd_buffer.size(), "init %f %d\n", (double)efpip->audio_rate, MAX_BLOCK_SIZE);
        }
        if (!SendCommandToHost(state, cmd_buffer.data(), response, sizeof(response)) || strncmp(response, "OK", 2) != 0)
        {
            DbgPrint(_T("Failed to initialize standalone host. Response: %hs"), response);
            return false;
        }
    }
    else
    {
        char plugin_path_mb[MAX_PATH];
#ifdef UNICODE
        WideCharToMultiByte(CP_UTF8, 0, exdata->plugin_path, -1, plugin_path_mb, MAX_PATH, NULL, NULL);
#else
        strcpy_s(plugin_path_mb, MAX_PATH, exdata->plugin_path);
#endif
        std::vector<char> cmd_buffer;
        if (strlen(exdata->state_b64) > 0)
        {
            size_t state_len = strlen(exdata->state_b64);
            cmd_buffer.resize(state_len + 1024);
            sprintf_s(cmd_buffer.data(), cmd_buffer.size(), "load_and_set_state \"%s\" %f %d %s\n", plugin_path_mb, (double)efpip->audio_rate, MAX_BLOCK_SIZE, exdata->state_b64);
        }
        else
        {
            cmd_buffer.resize(1024);
            sprintf_s(cmd_buffer.data(), cmd_buffer.size(), "load_plugin \"%s\" %f %d\n", plugin_path_mb, (double)efpip->audio_rate, MAX_BLOCK_SIZE);
        }
        if (!SendCommandToHost(state, cmd_buffer.data(), response, sizeof(response)) || strncmp(response, "OK", 2) != 0)
        {
            DbgPrint(_T("Failed to configure plugin. Response: %hs"), response);
            return false;
        }
    }

    DbgPrint(_T("Host launched and initialized successfully."));
    return true;
}
bool SendCommandToHost(HostState &state, const char *command, char *response, DWORD responseSize)
{
    if (!state.host_running || state.hPipe == INVALID_HANDLE_VALUE)
        return false;
    response[0] = '\0';
    DWORD bytesWritten;
    if (!WriteFile(state.hPipe, command, (DWORD)strlen(command), &bytesWritten, NULL))
        return false;
    DWORD bytesRead;
    if (!ReadFile(state.hPipe, response, responseSize - 1, &bytesRead, NULL))
    {
        DbgPrint(_T("ReadFile from pipe failed. Error: %lu"), GetLastError());
        return false;
    }
    response[bytesRead] = '\0';
    return true;
}
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD fdwReason, LPVOID)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hinst);
    return TRUE;
}
EXTERN_C __declspec(dllexport) ExEdit::Filter *const *__stdcall GetFilterTableList()
{
    constexpr static ExEdit::Filter *filter_list[] = {&filter, &effect, nullptr};
    return filter_list;
}