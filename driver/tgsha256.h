/* tgsha256.h - CNG SHA-256 小工具（服务与询问程序共用）
   家长密码只存哈希，明文不落盘。 */
#ifndef TGSHA256_H
#define TGSHA256_H
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

static BOOL TgSha256Hex(const BYTE *data, DWORD len, WCHAR *outHex, DWORD outChars)
{
    BCRYPT_ALG_HANDLE  alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    DWORD objLen = 0, cb = 0, hashLen = 0;
    PBYTE obj = NULL, digest = NULL;
    BOOL  ok = FALSE;
    DWORD i;
    static const WCHAR hexdig[] = L"0123456789abcdef";

    if (outChars < 65) return FALSE;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)))
        return FALSE;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                                          (PUCHAR)&objLen, sizeof(objLen), &cb, 0)))
        goto done;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
                                          (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0)))
        goto done;
    obj = (PBYTE)LocalAlloc(LMEM_FIXED, objLen);
    digest = (PBYTE)LocalAlloc(LMEM_FIXED, hashLen);
    if (obj == NULL || digest == NULL) goto done;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, obj, objLen, NULL, 0, 0)))
        goto done;
    if (!BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)data, len, 0)))
        goto done;
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, hashLen, 0)))
        goto done;
    for (i = 0; i < hashLen && (i * 2 + 1) < outChars; i++) {
        outHex[i * 2]     = hexdig[(digest[i] >> 4) & 0xF];
        outHex[i * 2 + 1] = hexdig[digest[i] & 0xF];
    }
    outHex[i * 2] = 0;
    ok = TRUE;
done:
    if (hash) BCryptDestroyHash(hash);
    if (obj) LocalFree(obj);
    if (digest) LocalFree(digest);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* 把 UTF-16 密码按 UTF-8 编码后求哈希（保证不同代码页一致） */
static BOOL TgSha256HexOfPassword(const WCHAR *pw, WCHAR *outHex, DWORD outChars)
{
    char utf8[512];
    int  n = WideCharToMultiByte(CP_UTF8, 0, pw, -1, utf8, (int)sizeof(utf8) - 1, NULL, NULL);
    if (n <= 0) return FALSE;
    if (n > 0) n -= 1;      /* 去掉结尾的 0，不参与哈希 */
    return TgSha256Hex((const BYTE *)utf8, (DWORD)n, outHex, outChars);
}
#endif
