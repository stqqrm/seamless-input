#include "config.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>

#include "identity.hpp"

namespace fs = std::filesystem;

long long NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

const SavedPeer* SavedConfig::FindPeer(const std::string& key) const
{
    for (const auto& p : peers) if (p.key == key) return &p;
    return nullptr;
}

SavedPeer& SavedConfig::PeerRef(const std::string& key)
{
    for (auto& p : peers) if (p.key == key) return p;
    peers.push_back({});
    peers.back().key = key;
    return peers.back();
}

std::vector<std::string> ConfigStore::Normalize(std::vector<std::string> keys)
{
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

std::string ConfigStore::SetId(const std::vector<std::string>& keys)
{
    std::string all = "si-set-v1";
    for (const auto& k : Normalize(keys)) all += "|" + k;
    return Identity::HashHex(all, 12);
}

fs::path ConfigStore::PathFor(const std::vector<std::string>& members) const
{
    return dir_ / ("config-" + SetId(members) + ".cfg");
}

// ---- file format: one "key value..." line each, easy to read and to edit by hand
//   seamless-input-config 1
//   label <text>
//   rev <ms>
//   member <public key>            (one per PC of the set, this PC included)
//   audio.send 0|1                 (legacy, read only)
//   audio.source 0|1               (legacy, read only: which source the old "send" meant)
//   audio.mute 0|1                 (master)
//   audio.volume 0..100            (master)
//   input.enabled 0|1
//   peer <public key> <ox> <oy> <name...>
//   peeraudio <public key> <send 0|1> <mute 0|1> <volume 0..100> [<mic 0|1>]
//                     send = share my (system) audio, mic = share my microphone
bool ConfigStore::Save(const SavedConfig& in, std::string* err) const
{
    SavedConfig cfg = in;
    cfg.members = Normalize(cfg.members);
    std::error_code ec;
    fs::create_directories(dir_, ec);

    const fs::path file = PathFor(cfg.members);
    const fs::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) { if (err) *err = "cannot write " + tmp.string(); return false; }
        out << "seamless-input-config 1\n";
        out << "label " << cfg.label << "\n";
        out << "rev " << cfg.rev << "\n";
        for (const auto& m : cfg.members) out << "member " << m << "\n";
        out << "audio.mute " << (cfg.audio.mute ? 1 : 0) << "\n";
        out << "audio.volume " << cfg.audio.volume << "\n";
        out << "input.enabled " << (cfg.inputEnabled ? 1 : 0) << "\n";
        for (const auto& p : cfg.peers)
            if (p.hasPos) out << "peer " << p.key << " " << std::llround(p.ox) << " " << std::llround(p.oy)
                << " " << p.name << "\n";
        for (const auto& p : cfg.peers)
            out << "peeraudio " << p.key << " " << (p.send ? 1 : 0) << " " << (p.mute ? 1 : 0)
                << " " << std::clamp(p.volume, 0, 100) << " " << (p.mic ? 1 : 0) << "\n";
        out.flush();
        if (!out) { if (err) *err = "write failed: " + tmp.string(); return false; }
    }
    fs::rename(tmp, file, ec);          // atomic replace: never a half-written config
    if (ec) { if (err) *err = "cannot replace " + file.string() + ": " + ec.message(); return false; }
    return true;
}

bool ConfigStore::Load(const std::vector<std::string>& wanted, SavedConfig& out) const
{
    const std::vector<std::string> want = Normalize(wanted);
    std::ifstream in(PathFor(want));
    if (!in) return false;

    std::string line;
    if (!std::getline(in, line) || line.rfind("seamless-input-config 1", 0) != 0) return false;

    SavedConfig cfg;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream is(line);
        std::string k;
        if (!(is >> k)) continue;

        if (k == "label")              { std::getline(is >> std::ws, cfg.label); }
        else if (k == "rev")           { is >> cfg.rev; }
        else if (k == "member")        { std::string m; if (is >> m && Identity::IsValidPublicHex(m)) cfg.members.push_back(m); }
        else if (k == "audio.send")    { int v = 0; is >> v; cfg.audio.send = v != 0; }
        else if (k == "audio.source")  { int v = 0; is >> v; cfg.audio.source = v == 1 ? 1 : 0; }
        else if (k == "audio.mute")    { int v = 0; is >> v; cfg.audio.mute = v != 0; }
        else if (k == "audio.volume")  { int v = 100; is >> v; cfg.audio.volume = std::clamp(v, 0, 100); }
        else if (k == "input.enabled") { int v = 1; is >> v; cfg.inputEnabled = v != 0; }
        else if (k == "peer") {
            SavedPeer p;
            double x = 0, y = 0;
            if (is >> p.key >> x >> y && Identity::IsValidPublicHex(p.key)) {
                std::getline(is >> std::ws, p.name);
                p.ox = x; p.oy = y;
                p.hasPos = true;
                SavedPeer& dst = cfg.PeerRef(p.key);
                dst.name = std::move(p.name);
                dst.ox = x; dst.oy = y; dst.hasPos = true;
            }
        }
        else if (k == "peeraudio") {
            std::string key;
            int snd = 0, mut = 0, vol = 100, mic = 0;
            if (is >> key >> snd >> mut >> vol && Identity::IsValidPublicHex(key)) {
                SavedPeer& p = cfg.PeerRef(key);
                p.hasAudio = true;
                p.send = snd != 0;
                p.mute = mut != 0;
                p.volume = std::clamp(vol, 0, 100);
                if (is >> mic) { p.mic = mic != 0; p.hasMic = true; }
            }
        }
    }

    // Old files had one "send" switch per device and one source for all of them:
    // when that source was the microphone, the switch means "share my microphone" now.
    for (auto& p : cfg.peers) {
        if (!p.hasAudio || p.hasMic) continue;
        if (cfg.audio.source == 0) { p.mic = p.send; p.send = false; }
        p.hasMic = true;
    }

    // A file only counts when it really describes THIS set (guards against renamed/copied files).
    if (Normalize(cfg.members) != want) return false;
    cfg.members = want;
    out = std::move(cfg);
    return true;
}

// ---- trust.txt:  seamless-input-trust 1 / "allow <key> <name>" / "deny <key> <name>"
void TrustStore::Load()
{
    entries_.clear();
    std::ifstream in(file_);
    std::string line;
    if (!in || !std::getline(in, line) || line.rfind("seamless-input-trust 1", 0) != 0) return;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream is(line);
        std::string verb, key;
        if (!(is >> verb >> key) || !Identity::IsValidPublicHex(key)) continue;
        Entry e;
        if (verb == "allow") e.state = State::Allowed;
        else if (verb == "deny") e.state = State::Denied;
        else continue;
        std::getline(is >> std::ws, e.name);
        entries_[key] = std::move(e);
    }
}

TrustStore::State TrustStore::Get(const std::string& key) const
{
    auto it = entries_.find(key);
    return it == entries_.end() ? State::Unknown : it->second.state;
}

bool TrustStore::Set(const std::string& key, State s, const std::string& name, std::string* err)
{
    if (s == State::Unknown) entries_.erase(key);
    else entries_[key] = {s, name};
    return Save(err);
}

bool TrustStore::Save(std::string* err) const
{
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    const fs::path tmp = file_.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) { if (err) *err = "cannot write " + tmp.string(); return false; }
        // owner-only: this file decides who may type on this PC
        fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
        out << "seamless-input-trust 1\n";
        for (const auto& kv : entries_)
            out << (kv.second.state == State::Allowed ? "allow " : "deny ") << kv.first << " " << kv.second.name << "\n";
        out.flush();
        if (!out) { if (err) *err = "write failed: " + tmp.string(); return false; }
    }
    fs::rename(tmp, file_, ec);
    if (ec) { if (err) *err = "cannot replace " + file_.string() + ": " + ec.message(); return false; }
    return true;
}
