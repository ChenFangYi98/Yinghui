#pragma once
#include <QByteArray>
#include <QCryptographicHash>
#include <QIODevice>
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace comp {
// Windows CNG uses the system's optimized SHA-256 implementation for large packs.
class FastSha256 {
  public:
    FastSha256() {
        DWORD bytes = 0, length = 0;
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
            BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&bytes),
                              sizeof(bytes), &length, 0) >= 0) {
            object_.resize(bytes);
            BCryptCreateHash(algorithm_, &hash_, reinterpret_cast<PUCHAR>(object_.data()), bytes, nullptr, 0,
                             0);
        }
    }
    FastSha256(const FastSha256&) = delete;
    ~FastSha256() {
        if (hash_)
            BCryptDestroyHash(hash_);
        if (algorithm_)
            BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    void addData(QByteArrayView data) {
        if (hash_)
            okay_ &= BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                    ULONG(data.size()), 0) >= 0;
        else
            fallback_.addData(data);
    }
    bool addData(QIODevice* file) {
        while (!file->atEnd()) {
            const auto block = file->read(4 * 1024 * 1024);
            if (block.isEmpty())
                return false;
            addData(block);
        }
        return okay_;
    }
    QByteArray result() {
        if (!hash_)
            return fallback_.result();
        QByteArray digest(32, '\0');
        return okay_ && BCryptFinishHash(hash_, reinterpret_cast<PUCHAR>(digest.data()), 32, 0) >= 0
                   ? digest
                   : QByteArray();
    }

  private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    QByteArray object_;
    QCryptographicHash fallback_{QCryptographicHash::Sha256};
    bool okay_ = true;
};
} // namespace comp
