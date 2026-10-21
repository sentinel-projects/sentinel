// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/platform/DpapiEncryptedSettingsStore.h"

#include <QtGlobal>

#if defined(Q_OS_WIN)
// clang-format off
#include <qt_windows.h>
#include <wincrypt.h>
#include <dpapi.h>
// clang-format on
#elif defined(Q_OS_MACOS) || defined(__APPLE__)
#include <CommonCrypto/CommonCrypto.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace sentinel::core {

namespace {

constexpr QLatin1String encryptedPrefix("$dpapi$");

bool isProbablySecret(const QString& key) {
    const auto lower = key.toLower();
    return lower.contains(QStringLiteral("apikey")) || lower.contains(QStringLiteral("secret")) ||
           lower.contains(QStringLiteral("token")) || lower.contains(QStringLiteral("password"));
}

} // namespace

DpapiEncryptedSettingsStore::DpapiEncryptedSettingsStore(std::unique_ptr<ISettingsStore> inner,
                                                         KeySource keySource)
    : inner_(std::move(inner)), keySource_(std::move(keySource)) {}

bool DpapiEncryptedSettingsStore::isSecretKey(const QString& key) {
    return isProbablySecret(key);
}

QString DpapiEncryptedSettingsStore::value(const QString& key, const QString& defaultValue) const {
    const auto raw = inner_->value(key, QString());
    if (raw.isEmpty()) {
        return defaultValue;
    }

    if (!isSecretKey(key) || !raw.startsWith(encryptedPrefix)) {
        return raw;
    }

    const auto cipherText = QByteArray::fromBase64(raw.mid(encryptedPrefix.size()).toLatin1());
    const auto decrypted = decrypt(cipherText);
    return decrypted.isEmpty() ? defaultValue : decrypted;
}

void DpapiEncryptedSettingsStore::setValue(QString key, QString value) {
    if (isSecretKey(key) && !value.isEmpty()) {
        const auto cipherData = encrypt(value);
        const auto encoded = encryptedPrefix + QString::fromLatin1(cipherData.toBase64());
        inner_->setValue(key, encoded);
    } else {
        inner_->setValue(key, value);
    }
}

void DpapiEncryptedSettingsStore::remove(const QString& key) {
    inner_->remove(key);
}

bool DpapiEncryptedSettingsStore::applyChanges(const QMap<QString, QString>& values,
                                               const QStringList& removals) {
    QMap<QString, QString> encoded;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        encoded.insert(it.key(),
                       isSecretKey(it.key()) && !it.value().isEmpty()
                           ? encryptedPrefix + QString::fromLatin1(encrypt(it.value()).toBase64())
                           : it.value());
    }
    return inner_ && inner_->applyChanges(encoded, removals);
}

QString DpapiEncryptedSettingsStore::errorCode() const {
    return inner_ ? inner_->errorCode() : QStringLiteral("StoreUnavailable");
}

#if defined(Q_OS_WIN)

QByteArray DpapiEncryptedSettingsStore::encrypt(const QString& plainText) {
    const QByteArray utf8 = plainText.toUtf8();
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(utf8.constData()));
    in.cbData = static_cast<DWORD>(utf8.size());

    DATA_BLOB out;
    if (!CryptProtectData(&in, L"Sentinel Settings", nullptr, nullptr, nullptr, 0, &out)) {
        return {};
    }

    QByteArray result(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return result;
}

QString DpapiEncryptedSettingsStore::decrypt(const QByteArray& cipherData) const {
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(cipherData.constData()));
    in.cbData = static_cast<DWORD>(cipherData.size());

    DATA_BLOB out;
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
        return {};
    }

    const QByteArray utf8(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return QString::fromUtf8(utf8);
}

#elif defined(Q_OS_MACOS) || defined(__APPLE__)

static QByteArray getOrCreateMacKeychainKey(bool create = true) {
    static const char* kService = "dev.sentinel.Sentinel";
    static const char* kAccount = "master_encryption_key";

    CFStringRef serviceStr =
        CFStringCreateWithCString(kCFAllocatorDefault, kService, kCFStringEncodingUTF8);
    CFStringRef accountStr =
        CFStringCreateWithCString(kCFAllocatorDefault, kAccount, kCFStringEncodingUTF8);

    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData,
                          kSecMatchLimit};
    const void* values[] = {kSecClassGenericPassword, serviceStr, accountStr, kCFBooleanTrue,
                            kSecMatchLimitOne};

    CFDictionaryRef query =
        CFDictionaryCreate(kCFAllocatorDefault, keys, values, 5, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);

    CFTypeRef dataTypeRef = NULL;
    OSStatus status = SecItemCopyMatching(query, &dataTypeRef);
    CFRelease(query);

    if (status == errSecSuccess && dataTypeRef != NULL) {
        CFDataRef dataRef = (CFDataRef)dataTypeRef;
        QByteArray key(reinterpret_cast<const char*>(CFDataGetBytePtr(dataRef)),
                       static_cast<int>(CFDataGetLength(dataRef)));
        CFRelease(dataTypeRef);
        CFRelease(serviceStr);
        CFRelease(accountStr);
        return key;
    }

    // Decryption is a read path, including settings inspection during maintenance.
    if (!create) {
        CFRelease(serviceStr);
        CFRelease(accountStr);
        return {};
    }

    QByteArray newKey(32, 0);
    if (SecRandomCopyBytes(kSecRandomDefault, newKey.size(),
                           reinterpret_cast<uint8_t*>(newKey.data())) != errSecSuccess) {
        CFRelease(serviceStr);
        CFRelease(accountStr);
        return QByteArray();
    }

    CFDataRef keyData = CFDataCreate(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(newKey.constData()), newKey.size());
    const void* addKeys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData,
                             kSecAttrAccessible};
    const void* addValues[] = {kSecClassGenericPassword, serviceStr, accountStr, keyData,
                               kSecAttrAccessibleAfterFirstUnlock};

    CFDictionaryRef addQuery =
        CFDictionaryCreate(kCFAllocatorDefault, addKeys, addValues, 5,
                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    SecItemAdd(addQuery, NULL);

    CFRelease(addQuery);
    CFRelease(keyData);
    CFRelease(serviceStr);
    CFRelease(accountStr);

    return newKey;
}

QByteArray DpapiEncryptedSettingsStore::encrypt(const QString& plainText) {
    const QByteArray key = keySource_ ? keySource_() : getOrCreateMacKeychainKey();
    if (key.isEmpty()) {
        return plainText.toUtf8();
    }

    const QByteArray utf8 = plainText.toUtf8();
    QByteArray iv(kCCBlockSizeAES128, 0);
    (void)SecRandomCopyBytes(kSecRandomDefault, iv.size(), reinterpret_cast<uint8_t*>(iv.data()));

    size_t outLen = 0;
    QByteArray cipherText(utf8.size() + kCCBlockSizeAES128, 0);

    CCCryptorStatus status = CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding,
                                     key.constData(), key.size(), iv.constData(), utf8.constData(),
                                     utf8.size(), cipherText.data(), cipherText.size(), &outLen);

    if (status != kCCSuccess) {
        return plainText.toUtf8();
    }

    cipherText.resize(static_cast<int>(outLen));
    return iv + cipherText;
}

QString DpapiEncryptedSettingsStore::decrypt(const QByteArray& cipherData) const {
    const QByteArray key = keySource_ ? keySource_() : getOrCreateMacKeychainKey(false);
    if (key.isEmpty() || cipherData.size() <= kCCBlockSizeAES128) {
        return QString::fromUtf8(cipherData);
    }

    const QByteArray iv = cipherData.left(kCCBlockSizeAES128);
    const QByteArray encryptedPayload = cipherData.mid(kCCBlockSizeAES128);

    size_t outLen = 0;
    QByteArray plainText(encryptedPayload.size() + kCCBlockSizeAES128, 0);

    CCCryptorStatus status =
        CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key.constData(), key.size(),
                iv.constData(), encryptedPayload.constData(), encryptedPayload.size(),
                plainText.data(), plainText.size(), &outLen);

    if (status != kCCSuccess) {
        return QString::fromUtf8(cipherData);
    }

    plainText.resize(static_cast<int>(outLen));
    return QString::fromUtf8(plainText);
}

#else

QByteArray DpapiEncryptedSettingsStore::encrypt(const QString& plainText) {
    return plainText.toUtf8();
}

QString DpapiEncryptedSettingsStore::decrypt(const QByteArray& cipherData) const {
    return QString::fromUtf8(cipherData);
}

#endif

} // namespace sentinel::core
