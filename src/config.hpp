#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// One saved configuration. A configuration belongs to an exact SET of PCs,
// identified by their public keys (see identity.hpp):
//
//     {A, B}     -> config-C        {A, B, C} -> config-D
//
// Each PC keeps its own copy in its own frame of reference (positions are
// "where is that peer relative to ME"), so the file can be copied around freely
// but is only ever picked when exactly this set of machines is connected.
struct SavedPeer {
    std::string key;       // public key, 64 hex chars
    std::string name;      // host name when last seen (display only)
    double ox = 0, oy = 0; // top-left of the peer's desktop relative to this PC's top-left
    bool hasPos = false;   // false: only the audio settings are known, no layout position yet

    // per-device audio (saved with the set, like the position)
    bool hasAudio = false; // false: the file had no "peeraudio" line for this peer
    bool send = false;     // share my audio (system audio) with this device
    bool mic = false;      // share my microphone with this device
    bool hasMic = false;   // false: line from before the microphone switch existed (migrated on load)
    bool mute = false;     // mute what this device sends me
    int  volume = 100;     // 0..100 slider units (what I hear from this device)
};

struct SavedAudio {
    bool send = false;     // legacy (one switch for all devices); only used to migrate old files
    int  source = 0;       // legacy (one source for all devices: 0 microphone, 1 system audio); only used to migrate old files
    bool mute = false;     // master mute of everything incoming
    int  volume = 100;     // master volume, 0..100 (slider units)
};

struct SavedConfig {
    std::vector<std::string> members;   // all public keys of the set INCLUDING this PC, sorted
    std::string label;                  // e.g. "desktop + laptop", display only
    long long rev = 0;                  // unix ms of the last change
    SavedAudio audio;
    bool inputEnabled = true;           // "Share keyboard & mouse"
    std::vector<SavedPeer> peers;       // layout of the other PCs

    const SavedPeer* FindPeer(const std::string& key) const;
    SavedPeer& PeerRef(const std::string& key);   // creates when missing
};

class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path dir) : dir_(std::move(dir)) {}

    // Sorted, de-duplicated copy of 'keys'.
    static std::vector<std::string> Normalize(std::vector<std::string> keys);
    // Stable id of a set (hash of the normalized keys); also the file name.
    static std::string SetId(const std::vector<std::string>& keys);

    // false if there is no (valid) config for exactly this set.
    bool Load(const std::vector<std::string>& members, SavedConfig& out) const;
    bool Save(const SavedConfig& cfg, std::string* err = nullptr) const;

    const std::filesystem::path& Dir() const { return dir_; }

private:
    std::filesystem::path PathFor(const std::vector<std::string>& members) const;
    std::filesystem::path dir_;
};

long long NowMs();

// Which PCs (by public key) may control the keyboard/mouse of THIS PC. This is a
// property of the peer, not of a set, so it lives in its own file (trust.txt) and
// survives restarts and set changes. Edit or delete the file to revoke.
class TrustStore {
public:
    enum class State { Unknown, Allowed, Denied };

    explicit TrustStore(std::filesystem::path file) : file_(std::move(file)) {}

    void Load();
    State Get(const std::string& key) const;
    // (key, name) of every Allowed entry
    std::vector<std::pair<std::string, std::string>> AllowedList() const;
    // Stores the decision and writes the file right away (decisions are rare).
    bool Set(const std::string& key, State s, const std::string& name, std::string* err = nullptr);

private:
    struct Entry { State state = State::Unknown; std::string name; };
    bool Save(std::string* err) const;
    std::filesystem::path file_;
    std::map<std::string, Entry> entries_;
};
