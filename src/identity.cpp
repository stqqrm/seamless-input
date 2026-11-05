#include "identity.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <vector>

extern "C" {
#include "monocypher/monocypher.h"
}

namespace fs = std::filesystem;

namespace {

const char* kHex = "0123456789abcdef";

std::string ToHex(const std::uint8_t* p, std::size_t n)
{
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) { s += kHex[p[i] >> 4]; s += kHex[p[i] & 15]; }
    return s;
}

int Nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool FromHex(const std::string& s, std::uint8_t* out, std::size_t n)
{
    if (s.size() != n * 2) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const int a = Nibble(s[2 * i]), b = Nibble(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (std::uint8_t)((a << 4) | b);
    }
    return true;
}

void RandomBytes(std::uint8_t* out, std::size_t n)
{
    std::random_device rd;   // OS entropy (getrandom / BCrypt / rand_s)
    for (std::size_t i = 0; i < n; ++i) out[i] = (std::uint8_t)(rd() & 0xff);
}

} // namespace

std::string Identity::RandomHex(std::size_t bytes)
{
    std::vector<std::uint8_t> v(bytes);
    RandomBytes(v.data(), v.size());
    return ToHex(v.data(), v.size());
}

std::string Identity::HashHex(const std::string& data, std::size_t bytes)
{
    if (bytes == 0 || bytes > 64) bytes = 16;
    std::vector<std::uint8_t> h(bytes);
    crypto_blake2b(h.data(), h.size(), reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    return ToHex(h.data(), h.size());
}

bool Identity::IsValidPublicHex(const std::string& s)
{
    std::uint8_t tmp[32];
    return FromHex(s, tmp, 32);
}

std::string Identity::Fingerprint(const std::string& pubHex)
{
    const std::string h = HashHex(pubHex, 8);   // 16 hex chars
    std::string f;
    for (std::size_t i = 0; i < h.size(); i += 4) {
        if (i) f += '-';
        f += h.substr(i, 4);
    }
    return f;
}

Identity Identity::LoadOrCreate(const fs::path& dir, std::string* err)
{
    Identity id;
    std::uint8_t seed[32] = {};
    bool have = false;

    std::error_code ec;
    const fs::path file = dir / "identity.key";

    {
        std::ifstream in(file);
        std::string line;
        if (in && std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            have = FromHex(line, seed, 32);
            if (!have && err) *err = "identity.key is damaged, creating a new identity";
        }
    }

    bool created = false;
    if (!have) {
        RandomBytes(seed, 32);
        created = true;
    }

    std::uint8_t seedCopy[32];
    std::copy(seed, seed + 32, seedCopy);
    std::uint8_t pub[32];
    crypto_eddsa_key_pair(id.secret_, pub, seed);   // wipes 'seed'
    id.pubHex_ = ToHex(pub, 32);

    if (created) {
        fs::create_directories(dir, ec);
        const fs::path tmp = dir / "identity.key.tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (out) {
                // Restrict first, write second: the seed is never readable by others.
                fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write,
                                fs::perm_options::replace, ec);
                out << ToHex(seedCopy, 32) << "\n";
            }
            if (!out) {
                if (err) *err = "could not write " + file.string();
            }
        }
        fs::rename(tmp, file, ec);
        if (ec && err && err->empty()) *err = "could not save " + file.string() + ": " + ec.message();
    }
    crypto_wipe(seedCopy, sizeof seedCopy);
    return id;
}

std::string Identity::Sign(const std::string& msg) const
{
    std::uint8_t sig[64];
    crypto_eddsa_sign(sig, secret_, reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size());
    return ToHex(sig, 64);
}

bool Identity::Verify(const std::string& pubHex, const std::string& msg, const std::string& sigHex)
{
    std::uint8_t pub[32], sig[64];
    if (!FromHex(pubHex, pub, 32) || !FromHex(sigHex, sig, 64)) return false;
    return crypto_eddsa_check(sig, pub, reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size()) == 0;
}
