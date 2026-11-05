#pragma once
#include <filesystem>
#include <string>

// Long-term identity of this PC: an Ed25519 key pair created on first start and
// kept in <data dir>/identity.key (the private seed never leaves this PC).
//
// The PUBLIC key is the PC's name everywhere else: configs are looked up by the
// set of public keys that are connected (see config.hpp), and the TCP handshake
// proves that the other side really owns the key it claims (see network.cpp).
class Identity {
public:
    // Loads <dir>/identity.key or creates it. On failure the identity is still
    // usable for this run (in-memory key) and *err says why it could not be saved.
    static Identity LoadOrCreate(const std::filesystem::path& dir, std::string* err);

    const std::string& PublicHex() const { return pubHex_; }      // 64 hex chars
    std::string Fingerprint() const { return Fingerprint(pubHex_); }

    std::string Sign(const std::string& msg) const;               // 128 hex chars

    static bool Verify(const std::string& pubHex, const std::string& msg, const std::string& sigHex);
    static bool IsValidPublicHex(const std::string& s);
    static std::string Fingerprint(const std::string& pubHex);    // "1a2b-3c4d-5e6f-7a8b"
    static std::string RandomHex(std::size_t bytes);              // OS random, hex encoded
    static std::string HashHex(const std::string& data, std::size_t bytes = 16);   // BLAKE2b

private:
    Identity() = default;
    unsigned char secret_[64] = {};
    std::string pubHex_;
};
