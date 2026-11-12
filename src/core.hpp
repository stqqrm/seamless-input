#pragma once
// Core: everything seamless-input does that is not drawing a window.
// Network + keyboard/mouse + audio engines, the peer lifecycle, the layout of the connected
// PCs, saved configs and permissions. It runs inside the daemon; the GUI only mirrors its state.
//
// Threading: all logic runs on ONE thread (the Core loop). Callbacks of the engines (network,
// input, audio threads) Post() onto it. Public Cmd*/Get* methods must be called on that thread
// (the IPC server runs there); other threads use Post().
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "audio.hpp"
#include "inputudp.hpp"
#include "clipshare.hpp"
#include "config.hpp"
#include "identity.hpp"
#include "input/input.hpp"
#include "layout.hpp"
#include "localinfo.hpp"
#include "network.hpp"
#include "overlay.hpp"
#include "state.hpp"

class Core {
public:
    explicit Core(std::filesystem::path dataDir);
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    // Starts the loop thread and the engines. Engines that cannot start (no audio device,
    // no input permission) are reported in the log; the daemon keeps running without them.
    bool Start(std::string* err);
    void Stop();

    boost::asio::io_context& Context() { return ctx_; }
    void Post(std::function<void()> f) { boost::asio::post(ctx_, std::move(f)); }

    // ---- notifications (Core thread)
    std::function<void()> onChanged;                        // state changed; coalesce on the receiving side
    std::function<void(const std::string& line)> onLog;     // one finished log line (with time)
    std::function<void()> onQuit;                           // a client asked the daemon to stop

    // ---- state (Core thread)
    state::Snapshot GetSnapshot() const;
    state::Stats GetStats() const;
    std::vector<std::string> LogHistory() const { return {log_.begin(), log_.end()}; }
    void SetClientCount(int n) { clients_ = n; }

    // ---- commands (Core thread)
    void CmdScan();
    void CmdSelect(const std::string& ip);
    void CmdGroup(const std::string& password);
    void CmdMove(int id, double x, double y);               // the user dragged a PC in the layout
    void CmdPeerAudio(int id, bool share, bool mic, bool mute, int vol);
    void CmdMaster(bool mute, int vol);
    void CmdControl(int id, bool allow);                    // may this PC control ours?
    void CmdClipLimit(std::uint64_t bytes);                 // largest clipboard (text / files) that is copied to another PC
    void CmdRate(int id, int hz);                           // keyboard/mouse updates per second sent to this PC
    void CmdClipboard(int id, bool share);                  // send my clipboard to this PC whenever I copy something
    void CmdPopupTheme(const std::vector<int>& rgb21);      // the window's palette for the progress popup (7 colours x RGB)
    void CmdClipTelemetry(bool on);                         // progress popup while the clipboard is uploaded / downloaded
    bool ClipTelemetryEnabled() const { return clipTelemetry_; }
    void CmdMonitorsHint(const std::vector<InputRect>& mons);
    void CmdClearCache();                                   // forget remembered permissions and the monitor cache
    void CmdRegenKeys();                                    // new identity key pair, then restart the daemon
    void CmdQuit() { if (onQuit) onQuit(); }

private:
    using Clock = std::chrono::steady_clock;
    struct DiscRec { state::Beacon b; Clock::time_point seen; };
    struct ScanRec { state::Scanned s; Clock::time_point seen; };

    void Log(const std::string& msg);                          // Core thread only
    void PostLog(const std::string& msg) { Post([this, msg] { Log(msg); }); }   // any thread
    void Changed() { if (onChanged) onChanged(); }
    void Tick();
    void ScheduleTick();

    void LoadMonitors();
    // This PC's monitors changed (plugged, unplugged, rotated, resolution): follow it everywhere.
    void ApplyLocalMonitors(const std::vector<InputRect>& mons, const char* source, bool persist);
    void PollMonitors();                                        // the OS is asked once a second (Windows, X11)
    void ReplacePeersAfterMonitorChange();
    std::string ScreensLine() const;
    std::string VerLine() const;                                // version handshake line
    void OnVersion(int id, const std::string& line);
    std::string ClipBlocker(const state::Peer& p) const;        // empty: the peer can receive our clipboard
    void StartScan();

    // network events (already on the Core thread)
    void OnPeerReady(int id, std::uint32_t inst, const std::string& key, const std::string& name,
                     const std::string& addr, bool incoming, const PeerAudioKeys& audioKeys);
    void OnPeerLost(int id);
    void OnMessage(int id, const std::string& line);
    void SendControlState(int id);      // tell a peer whether it may control this PC (CTRL line)

    // clipboard sharing (Core thread)
    void OnLocalClipboardChanged();                                // the user copied something here: send it to every PC we share with
    void SaveClipSettings();
    // The telemetry popup (overlay.hpp) is drawn by this daemon; it is refreshed 10 times a second.
    void ScheduleTelemetry();
    void UpdatePopup();
    // The watcher thread polls the clipboard's change token; changes are posted to the Core thread.
    void StartClipWatch();
    void StopClipWatch();
    void ClipWatchLoop();

    // layout + saved configs
    void BroadcastRelative(int movedId);
    void SendPos(int id, double x, double y);
    void UpdateCfgFromState();
    void TouchCfg();
    void MarkCfgDirty();
    void RecordPeerAudio(const state::Peer& p, bool force = false);
    void RecordPeerPos(int id, double x, double y);
    void FlushConfig();
    void ApplyStoredLayout(int id);
    void PushPeerAudio(const state::Peer& p);
    void ApplyConfigSettings();
    void OnSetChanged();

    // audio capture
    void RefreshCapture();
    void UpdateCapture(AudioSource src, bool want, bool& capturing, const char* what);

    std::filesystem::path dir_;
    std::string host_;
    std::vector<LocalAddress> local_;

    std::vector<InputRect> mons_;
    std::string monsSource_ = "default";      // system | saved | window | default
    bool preferHint_ = false;                 // Wayland: the window's monitor list beats XWayland's

    std::unique_ptr<Identity> ident_;
    std::unique_ptr<ConfigStore> store_;
    std::unique_ptr<TrustStore> trust_;       // peers (by public key) that may / may not control this PC
    std::filesystem::path groupFile_;
    std::string groupPwd_;
    std::string identNote_;

    boost::asio::io_context ctx_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_;
    std::unique_ptr<boost::asio::steady_timer> tickTimer_;
    std::thread loop_;
    std::atomic<bool> running_{false};

    std::unique_ptr<Network> net_;
    std::unique_ptr<AudioEngine> audio_;
    std::unique_ptr<InputUdp> inUdp_;       // pointer motion and wheel over UDP (the rest of the input stays on TCP)
    std::unique_ptr<InputEngine> input_;
    layout::Model layout_;

    std::map<int, state::Peer> peers_;
    std::map<std::string, DiscRec> disc_;
    std::map<std::string, ScanRec> scan_;
    bool scanning_ = false;
    int clients_ = 0;
    int ticks_ = 0, scanTick_ = 0;

    void RefreshInputStatus(bool started, const std::string& msg);   // Core thread
    bool canInject_ = true;                       // this PC can be controlled
    std::string inputNote_;                       // why not / what is limited
    std::unique_ptr<ClipShare> clip_;             // clipboard text / files shared with peers
    bool clipTelemetry_ = false;                  // progress popup on (saved in clipboard.txt)
    std::unique_ptr<overlay::Popup> popup_;       // created at the first transfer
    std::unique_ptr<boost::asio::steady_timer> telTimer_;
    bool popupShown_ = false;
    bool popupErrorLogged_ = false;
    std::thread clipWatch_;
    std::atomic<bool> clipWatchRun_{false};
    std::atomic<bool> clipWatchActive_{false};    // somebody to share with: poll
    std::atomic<bool> clipWatchRebase_{false};    // we just wrote the clipboard ourselves: take the new token as the baseline
    std::vector<InputRect> pendingMons_;          // a monitor change seen once: applied when the next poll agrees

    bool masterMute_ = false;
    int masterVol_ = 100;
    bool sysCapturing_ = false, micCapturing_ = false;
    InputEngine::Mode inputMode_ = InputEngine::Mode::Local;
    int inputPeer_ = -1;

    SavedConfig cfg_;                         // config of the currently connected set of PCs
    bool haveCfg_ = false;                    // false while no other PC is connected
    bool cfgDirty_ = false;
    bool loading_ = false;
    Clock::time_point dirtyAt_;

    std::deque<std::string> log_;
};
