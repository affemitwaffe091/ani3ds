/*
 * http/https protocol for the 3DS player: all I/O is delegated to the
 * application (pbnet_* functions, implemented with libcurl + mbedtls there),
 * so libavformat needs no sockets or TLS of its own.
 */
#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "url.h"

int     pbnet_open(const char *url, void **handle, int64_t *size);
int     pbnet_read(void *handle, unsigned char *buf, int size);
int64_t pbnet_seek(void *handle, int64_t pos);
void    pbnet_close(void *handle);

typedef struct PBNetContext {
    void *h;
    int64_t size;
    int64_t pos;
} PBNetContext;

static int pbnet_url_open(URLContext *uc, const char *url, int flags)
{
    PBNetContext *c = uc->priv_data;
    if (flags & AVIO_FLAG_WRITE)
        return AVERROR(ENOSYS);
    c->size = -1;
    c->pos = 0;
    int r = pbnet_open(url, &c->h, &c->size);
    if (r < 0)
        return r;
    uc->is_streamed = c->size < 0;
    return 0;
}

static int pbnet_url_read(URLContext *uc, unsigned char *buf, int size)
{
    PBNetContext *c = uc->priv_data;
    int r = pbnet_read(c->h, buf, size);
    if (r == 0)
        return AVERROR_EOF;
    if (r > 0)
        c->pos += r;
    return r;
}

static int64_t pbnet_url_seek(URLContext *uc, int64_t pos, int whence)
{
    PBNetContext *c = uc->priv_data;
    if (whence == AVSEEK_SIZE)
        return c->size >= 0 ? c->size : AVERROR(ENOSYS);
    if (whence == SEEK_CUR)
        pos += c->pos;
    else if (whence == SEEK_END) {
        if (c->size < 0)
            return AVERROR(ENOSYS);
        pos += c->size;
    } else if (whence != SEEK_SET)
        return AVERROR(EINVAL);
    if (pos < 0)
        return AVERROR(EINVAL);
    int64_t r = pbnet_seek(c->h, pos);
    if (r < 0)
        return r;
    c->pos = r;
    return r;
}

static int pbnet_url_close(URLContext *uc)
{
    PBNetContext *c = uc->priv_data;
    if (c->h)
        pbnet_close(c->h);
    c->h = NULL;
    return 0;
}

const URLProtocol ff_pbnet_http_protocol = {
    .name           = "http",
    .url_open       = pbnet_url_open,
    .url_read       = pbnet_url_read,
    .url_seek       = pbnet_url_seek,
    .url_close      = pbnet_url_close,
    .priv_data_size = sizeof(PBNetContext),
    .default_whitelist = "http,https,tls,tcp,crypto,data",
};

const URLProtocol ff_pbnet_https_protocol = {
    .name           = "https",
    .url_open       = pbnet_url_open,
    .url_read       = pbnet_url_read,
    .url_seek       = pbnet_url_seek,
    .url_close      = pbnet_url_close,
    .priv_data_size = sizeof(PBNetContext),
    .default_whitelist = "http,https,tls,tcp,crypto,data",
};
