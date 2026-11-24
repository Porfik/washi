#include "secret.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mupdf/fitz.h>

#include "../platform/platform.h"
#include "log.h"

#define PREFIX "enc1:"
#define TAG_LEN 16

static unsigned char enc_key[32], mac_key[32];
static int ready;

static void sha256(const void *a, size_t an, const void *b, size_t bn, unsigned char out[32])
{
    fz_sha256 st;
    fz_sha256_init(&st);
    fz_sha256_update(&st, a, an);
    if (b)
        fz_sha256_update(&st, b, bn);
    fz_sha256_final(&st, out);
}

static void hmac(const unsigned char key[32], const unsigned char *msg, size_t n, unsigned char out[32])
{
    unsigned char ipad[64], opad[64], inner[32];
    memset(ipad, 0x36, sizeof ipad);
    memset(opad, 0x5c, sizeof opad);
    for (int i = 0; i < 32; i++) {
        ipad[i] ^= key[i];
        opad[i] ^= key[i];
    }
    sha256(ipad, sizeof ipad, msg, n, inner);
    sha256(opad, sizeof opad, inner, sizeof inner, out);
}

static int random_bytes(unsigned char *buf, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t got = 0;
    while (fd >= 0 && got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    if (fd >= 0)
        close(fd);
    return got == n ? 0 : -1;
}

/* Запасной ключ: случайный, в файле папки данных (создаётся при первом обращении). */
static int key_file(char *buf, size_t len)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/secret.key", pf_data_dir());
    FILE *f = fopen(path, "r");
    if (f) {
        int ok = fgets(buf, (int)len, f) != NULL && strlen(buf) >= 32;
        fclose(f);
        if (ok)
            return 0;
    }
    unsigned char r[32];
    if (random_bytes(r, sizeof r) != 0 || len < 65)
        return -1;
    for (int i = 0; i < 32; i++)
        snprintf(buf + 2 * i, 3, "%02x", r[i]);
    f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "%s\n", buf);
    fclose(f);
    return 0;
}

void secret_init(void)
{
    if (ready)
        return;
    char dev[1024] = "";
    const char *how = "ключ и серийный номер устройства";
    if (pf_device_secret(dev, sizeof dev) != 0) {
        how = "файл secret.key";
        if (key_file(dev, sizeof dev) != 0) {
            log_msg("пароли: нет ни ключа устройства, ни secret.key — шифровать нечем");
            return;
        }
    }
    unsigned char base[32];
    /* Не менять (и при переименовании проекта тоже): от соли зависят пароли, уже сохранённые на
     * устройствах. Строка — из первого имени проекта. */
    static const char salt[] = "openbook-secret-v1|";
    sha256(salt, sizeof salt - 1, dev, strlen(dev), base);
    sha256(base, sizeof base, "enc", 3, enc_key);
    sha256(base, sizeof base, "mac", 3, mac_key);
    memset(dev, 0, sizeof dev);
    log_msg("пароли: ключ — %s, отпечаток %02x%02x", how, base[0], base[1]);
    memset(base, 0, sizeof base);
    ready = 1;
}

/* ---- base64url без «=» ---- */

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64_len(size_t n)
{
    return (n * 4 + 2) / 3;
}

static void b64_encode(const unsigned char *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = b64[v >> 18 & 63];
        out[o++] = b64[v >> 12 & 63];
        if (i + 1 < n)
            out[o++] = b64[v >> 6 & 63];
        if (i + 2 < n)
            out[o++] = b64[v & 63];
    }
    out[o] = 0;
}

/* Длина результата или -1. */
static long b64_decode(const char *in, unsigned char *out, size_t cap)
{
    uint32_t v = 0;
    int bits = 0;
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        const char *c = strchr(b64, *p);
        if (!c)
            return -1;
        v = v << 6 | (uint32_t)(c - b64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap)
                return -1;
            out[o++] = (unsigned char)(v >> bits);
        }
    }
    return (long)o;
}

size_t secret_sealed_len(size_t plain_len)
{
    size_t ct = (plain_len / 16 + 1) * 16;
    return strlen(PREFIX) + b64_len(16 + ct + TAG_LEN);
}

int secret_is_sealed(const char *stored)
{
    return stored && !strncmp(stored, PREFIX, strlen(PREFIX));
}

int secret_seal(const char *plain, char *out, size_t len)
{
    if (len)
        out[0] = 0;
    if (!plain || !*plain)
        return 0;
    secret_init();
    size_t n = strlen(plain);
    if (!ready || n > 1024 || secret_sealed_len(n) + 1 > len)
        return -1;
    unsigned char buf[16 + 1040 + TAG_LEN], iv[16];
    if (random_bytes(buf, 16) != 0) {
        log_msg("пароли: нет /dev/urandom");
        return -1;
    }
    size_t ct = (n / 16 + 1) * 16;
    unsigned char *body = buf + 16;
    memcpy(body, plain, n);
    memset(body + n, (int)(ct - n), ct - n); /* PKCS#7 */
    fz_aes aes;
    fz_aes_setkey_enc(&aes, enc_key, 256);
    memcpy(iv, buf, 16);
    fz_aes_crypt_cbc(&aes, FZ_AES_ENCRYPT, ct, iv, body, body);
    unsigned char tag[32];
    hmac(mac_key, buf, 16 + ct, tag);
    memcpy(buf + 16 + ct, tag, TAG_LEN);
    memcpy(out, PREFIX, strlen(PREFIX));
    b64_encode(buf, 16 + ct + TAG_LEN, out + strlen(PREFIX));
    memset(buf, 0, sizeof buf);
    memset(&aes, 0, sizeof aes);
    return 0;
}

int secret_open(const char *stored, char *out, size_t len)
{
    if (!len)
        return -1;
    out[0] = 0;
    if (!stored || !*stored)
        return 0;
    if (!secret_is_sealed(stored)) {
        snprintf(out, len, "%s", stored);
        return 0;
    }
    secret_init();
    unsigned char buf[16 + 1040 + TAG_LEN], iv[16], tag[32];
    long n = b64_decode(stored + strlen(PREFIX), buf, sizeof buf);
    if (!ready || n < 16 + 16 + TAG_LEN || (n - 16 - TAG_LEN) % 16)
        return -1;
    size_t ct = (size_t)n - 16 - TAG_LEN;
    hmac(mac_key, buf, 16 + ct, tag);
    unsigned diff = 0;
    for (int i = 0; i < TAG_LEN; i++)
        diff |= tag[i] ^ buf[16 + ct + i];
    if (diff) {
        log_msg("пароли: не расшифровалось (другое устройство или файл испорчен)");
        return -1;
    }
    fz_aes aes;
    fz_aes_setkey_dec(&aes, enc_key, 256);
    memcpy(iv, buf, 16);
    fz_aes_crypt_cbc(&aes, FZ_AES_DECRYPT, ct, iv, buf + 16, buf + 16);
    memset(&aes, 0, sizeof aes);
    unsigned pad = buf[16 + ct - 1];
    if (pad < 1 || pad > 16 || pad > ct || ct - pad + 1 > len)
        return -1;
    memcpy(out, buf + 16, ct - pad);
    out[ct - pad] = 0;
    memset(buf, 0, sizeof buf);
    return 0;
}
