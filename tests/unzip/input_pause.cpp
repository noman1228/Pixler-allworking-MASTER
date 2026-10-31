#include "shims/ESPixelStick.h"
#include <atomic>
#include <cassert>
#include <mutex>
#include <new>
#include <thread>
#include "DeferredConfigReload.hpp"

#define SUPPORT_FPP
#define INPUT_STATE_GUARD std::lock_guard<std::recursive_mutex> stateGuard(InputStateMutex)
static constexpr unsigned LOAD_CONFIG_DELAY = 1;
static bool IsBooting = false;
static const String InputRebootReason;
static std::atomic<unsigned> openHandles{0}, closedHandles{0}, playerDestructions{0}, starts{0};
static unsigned invalidHeapDeletions = 0;
static unsigned checks = 0;
static void require(bool success, const char* message) { ++checks; if (!success) throw std::runtime_error(message); }
struct { unsigned BlankDelay = 0; } config;

struct c_ExternalInput {
    enum class InputValue_t { Pressed };
    unsigned polls = 0;
    void Poll() { ++polls; }
};
class c_InputCommon;
class c_InputMgr {
public:
    enum e_InputType { InputType_DDP, InputType_FPP };
    enum e_InputChannelIds { InputPrimaryChannelId, InputSecondaryChannelId };
    struct Driver { c_InputCommon* InputDriver = nullptr; bool DriverInUse = false; } InputChannelDrivers[2];
    bool PauseProcessing = false, TestModePaused = false, FileExtractionPaused = false, configInProgress = false, RebootNeeded = false;
    std::recursive_mutex InputStateMutex;
    DeferredConfigReload ConfigReload;
    String ConfigFileName = "input.json";
    c_ExternalInput ExternalInput;
    unsigned configLoads = 0;
    void SetFileExtractionPaused(bool);
    void SetOperationalState(bool);
    void SetTestModePaused(bool);
    void ApplyOperationalState();
    void Process();
    void PollConfig();
    void ProcessButtonActions(c_ExternalInput::InputValue_t);
    void LoadConfig();
    void ProcessJsonConfig(JsonObject&) { ++configLoads; }
    void CreateNewConfig() {}
    bool BlankTimerHasExpired(e_InputChannelIds) { return true; }
    void RestartBlankTimer(e_InputChannelIds) {}
};
struct {
    unsigned loads = 0;
    bool LoadFlashFile(const String&, std::function<void(JsonDocument&)> callback) {
        ++loads;
        JsonDocument document;
        document.to<JsonObject>();
        callback(document);
        return true;
    }
} FileMgr;
struct { void ClearBuffer() {} } OutputMgr;

class c_InputCommon {
public:
    bool active = true;
    unsigned processed = 0, buttons = 0;
    virtual ~c_InputCommon() = default;
    virtual c_InputMgr::e_InputType GetInputType() { return c_InputMgr::InputType_DDP; }
    virtual void SetOperationalState(bool value) { active = value; }
    virtual void Process() { if (active) ++processed; }
    virtual void ProcessButtonActions(c_ExternalInput::InputValue_t) { if (active) ++buttons; }
    virtual void SetBlankTimerIsRunning(bool) {}
    c_InputMgr::e_InputChannelIds GetInputChannelId() { return c_InputMgr::InputSecondaryChannelId; }
};

class c_InputFPPRemotePlayItem {
public:
    bool paused = false;
    virtual ~c_InputFPPRemotePlayItem() = default;
    virtual void Stop() = 0;
    virtual bool Poll() = 0;
    virtual bool IsIdle() = 0;
    // Production playlist children live in their parent's placement-new buffer.
    // Trap ordinary delete without freeing that interior address on the host.
    static void operator delete(void*) noexcept { ++invalidHeapDeletions; }
    void ClearFileNames() {}
    bool InputIsPaused() const { return paused; }
    void SetOperationalState(bool active) { paused = !active; }
};

// The independent FSM seam owns an SD-reader token. The actual production
// Stop/Poll and destructor must reach its stopping transition to release it.
class c_InputFPPRemotePlayFile : public c_InputFPPRemotePlayItem {
public:
    struct State {
        bool open = true, stopping = false;
        State() { ++openHandles; }
        void Stop() { stopping = true; }
        bool Poll() {
            if (open && stopping) { open = false; --openHandles; ++closedHandles; }
            return false;
        }
    } state;
    State* pCurrentFsmState = &state;
    ~c_InputFPPRemotePlayFile();
    void Stop() override;
    bool Poll() override;
    bool IsIdle() override { return !state.open; }
};

class c_InputFPPRemote : public c_InputCommon {
    friend class c_InputMgr;
public:
    c_InputFPPRemotePlayItem* pInputFPPRemotePlayItem = nullptr;
    struct { alignas(16) byte Player[256]{}; bool InUse = false; } PlayerInfo;
    bool Stopping = false, remote = false;
    c_InputMgr::e_InputType GetInputType() override { return c_InputMgr::InputType_FPP; }
    bool AllowedToPlayRemoteFile() { return remote && pInputFPPRemotePlayItem != nullptr; }
    void SetOperationalState(bool value) override {
        active = value;
        if (pInputFPPRemotePlayItem) pInputFPPRemotePlayItem->SetOperationalState(value);
    }
    void Process() override {
        if (!active) return;
        ++processed;
        if (!pInputFPPRemotePlayItem) {
            pInputFPPRemotePlayItem = new(PlayerInfo.Player) c_InputFPPRemotePlayFile();
            PlayerInfo.InUse = true;
            ++starts;
        }
    }
    void StopPlaying();
    void FppStopRemoteFilePlay();
    bool PlayingFile();
};

struct c_InputFPPRemotePlayList;
struct PlaylistIdleState { void Init(c_InputFPPRemotePlayList*); };
struct c_InputFPPRemotePlayList {
    alignas(16) byte InputFPPRemotePlayItem[256]{};
    c_InputFPPRemotePlayItem* pInputFPPRemotePlayItem = nullptr;
    PlaylistIdleState fsm_PlayList_state_Idle_imp;
    bool idle = false;
};
void PlaylistIdleState::Init(c_InputFPPRemotePlayList* parent) { parent->idle = true; }
struct fsm_PlayList_state_PlayingFile {
    c_InputFPPRemotePlayList* pInputFPPRemotePlayList;
    void Stop();
};

#include "player_methods.inc"
#include "input_methods.inc"

static void exerciseReasons(bool priorUploadPause, bool priorTestPause, bool remote)
{
    c_InputMgr manager;
    c_InputCommon networkInput;
    c_InputFPPRemote player;
    player.remote = remote;
    manager.InputChannelDrivers[0] = {&networkInput, true};
    manager.InputChannelDrivers[1] = {&player, true};
    manager.Process();
    require(openHandles == 1, "Configured sequence was not opened by the setup");
    if (priorUploadPause) manager.SetOperationalState(false);
    if (priorTestPause) manager.SetTestModePaused(true);
    manager.SetFileExtractionPaused(true);
    require(openHandles == 0 && player.pInputFPPRemotePlayItem == nullptr, "Extraction pause did not close configured sequence handle");
    require(!networkInput.active && !player.active, "Extraction pause did not suspend input drivers");
    auto before = starts.load();
    manager.Process();
    manager.ProcessButtonActions(c_ExternalInput::InputValue_t::Pressed);
    require(starts == before && networkInput.buttons == 0, "Input processing restarted playback during extraction");
    manager.ConfigReload.Schedule(millis() - 2000u);
    auto configs = manager.configLoads;
    manager.LoadConfig();
    manager.PollConfig();
    require(manager.ConfigReload.IsDue(millis(), 0) && manager.configLoads == configs, "Extraction performed a pending config reload");
    manager.SetOperationalState(true);
    manager.SetTestModePaused(false);
    require(!networkInput.active && !player.active, "Upload/test completion bypassed extraction pause");
    manager.SetFileExtractionPaused(true);
    require(openHandles == 0, "Repeated pause reopened or retained a player");
    manager.SetOperationalState(!priorUploadPause);
    manager.SetTestModePaused(priorTestPause);
    manager.SetFileExtractionPaused(false);
    require(player.active == (!priorUploadPause && !priorTestPause), "Extraction resume ignored another active pause reason");
    manager.PollConfig();
    manager.Process();
    require(openHandles == ((!priorUploadPause && !priorTestPause) ? 1U : 0U), "Playback resumed under another pause or failed to reopen after extraction");
    manager.SetOperationalState(true);
    manager.SetTestModePaused(false);
    manager.PollConfig();
    manager.Process();
    require(openHandles == 1 && !manager.ConfigReload.IsDue(millis(), 0), "Playback/config reload did not resume after all pause reasons cleared");
    manager.SetFileExtractionPaused(true);
    require(openHandles == 0, "Final extraction pause leaked a playback handle");
}

int main()
{
    try {
        for (bool upload : {false, true})
            for (bool test : {false, true})
                for (bool remote : {false, true}) exerciseReasons(upload, test, remote);
        require(closedHandles >= 16, "Production file teardown did not execute for the reason matrix");
        c_InputFPPRemotePlayList playlist;
        playlist.pInputFPPRemotePlayItem = new(playlist.InputFPPRemotePlayItem) c_InputFPPRemotePlayFile();
        fsm_PlayList_state_PlayingFile playlistState{&playlist};
        playlistState.Stop();
        require(openHandles == 0 && playlist.idle, "Stopping playlist did not close its active sequence");
        require(invalidHeapDeletions == 0, "Playlist teardown passed placement-new child storage to delete");
        require(playlist.pInputFPPRemotePlayItem == nullptr, "Stopped playlist retained a dangling child pointer");
        std::cout << "PASS: independent extraction/input/upload/test pause reasons and actual FPP teardown (" << checks << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
