#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdatomic.h>

#include "pd_api.h"
#include "minimp3.h"

// BBC WOrld Service
#define STREAM_URL  "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service"
#define ACCESS_HOST "stream.live.vc.bbcmedia.co.uk"
#define ACCESS_PORT 80

// Internal HTTP read buffer
#define STREAM_HTTP_READ_BUFFER 262144

// Single-producer/single-consumer MP3 byte ring between the main thread
// (HTTP pump producing) and the main thread again (decoder consuming).
// 128 KB ≈ 8 s @ 128 kbps. Power of 2 for cheap masking.
#define RING_SIZE 131072
#define RING_MASK (RING_SIZE - 1)

// Contiguous slice the decoder copies out of the byte ring before feeding
// minimp3. Comfortably larger than one frame (a 128 kbps frame is ~417 B)
// plus any ID3/junk minimp3 might skip past.
#define MP3_SCRATCH 4096

// Decoded-PCM ring between the main thread (decoder producing) and the
// audio thread (callback consuming). One slot = one mono int16. 16384
// frames ≈ 372 ms at 44.1 kHz — enough for ~7 main-loop ticks of jitter
// and for a worst-case single-frame resample burst.
#define PCM_FRAMES 16384
#define PCM_MASK   (PCM_FRAMES - 1)

// Playdate audio output rate. This is fixed, if the audio source has a
// different rate, it needs to be resampled on-the-fly
#define OUTPUT_HZ 44100

#define MAX_RESAMPLED_PER_FRAME 8192
#define PREBUFFER_BYTES 49152
#define PREBUFFER_TIMEOUT_MS 10000
#define STALL_TIMEOUT_MS 8000
#define RECONNECT_DELAY_MS 2000

// --- on-screen log ---------------------------------------------------------

#define LOG_LINES    14
#define LOG_LINE_LEN 80

static PlaydateAPI* pd = NULL;
static const struct playdate_http* http = NULL;

static char log_lines[LOG_LINES][LOG_LINE_LEN];
static int  log_head  = 0;
static int  log_count = 0;

static void logScreen(const char* fmt, ...)
{
    char buf[LOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (pd) pd->system->logToConsole("%s", buf);

    memcpy(log_lines[log_head], buf, sizeof(buf));
    log_head = (log_head + 1) % LOG_LINES;
    if (log_count < LOG_LINES) log_count++;
}

// --- stream connection / minimp3 state -------------------------------------

static HTTPConnection* stream_conn = NULL;
static SoundSource* mp3_source = NULL;

static mp3dec_t mp3dec;
static uint8_t  mp3_scratch[MP3_SCRATCH];
static int16_t  decode_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];

// PCM ring (mono).
// Producer = main thread (decodeIntoPCMRing);
// Consumer = audio thread (audioCallback).
//
// Mono because the Playdate device crashes when an addCallbackSource is
// registered stereo, and the speaker is mono anyway.
// Stereo MP3 frames get downmixed before they hit the ring.
static int16_t     pcm[PCM_FRAMES];
static atomic_uint pcm_write = 0;  // written by main thread
static atomic_uint pcm_read  = 0;  // written by audio thread

// Linear-interpolating resampler state. Q16.16 fixed point.
static int     rs_in_rate  = 0;
static int32_t rs_step_q16 = 0;
static int32_t rs_pos_q16  = 0;
static int16_t rs_prev     = 0;

static uint8_t ring[RING_SIZE];
static atomic_uint ring_write = 0;  // written by main thread (HTTP pump)
static atomic_uint ring_read  = 0;  // written by audio thread / decoder (main)

// Parsed-out pieces of STREAM_URL.
static char stream_host[128] = {0};
static int  stream_port      = 80;
static char stream_path[256] = {0};

static unsigned prebuffer_start_ms = 0;
static unsigned reconnect_at_ms    = 0;

typedef enum {
    kBoot,
    kRequestingAccess,
    kWaitingForAccess,
    kStartingStream,
    kPrebuffering,
    kPlaying,
    kReconnectWait,
    kFailed,
} State;

static State state = kBoot;
static char  fail_reason[96] = {0};

// Closing/releasing a connection from inside one of its own callbacks crashes
// the SDK. Callbacks set a pending action and the next update() tick performs
// the teardown on a clean stack.
typedef enum { kPendingNone, kPendingRetry } PendingAction;
static PendingAction pending = kPendingNone;
static char pending_reason[64] = {0};

static void ring_reset(void);
static void pcm_reset(void);
static int  parseHttpURL(const char* url, char* host, size_t hostlen,
                         int* port, char* path, size_t pathlen);

static void fail(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fail_reason, sizeof(fail_reason), fmt, ap);
    va_end(ap);
    logScreen("FAIL: %s", fail_reason);
    state = kFailed;
}

static void requestRetry(const char* reason)
{
    pending = kPendingRetry;
    strncpy(pending_reason, reason, sizeof(pending_reason) - 1);
    pending_reason[sizeof(pending_reason) - 1] = 0;
}

static void teardownPlayback(void)
{
    if (mp3_source)  { pd->sound->removeSource(mp3_source); mp3_source = NULL; }
    if (stream_conn) { http->close(stream_conn); http->release(stream_conn); stream_conn = NULL; }
    // removeSource has unhooked the callback, so the audio thread is no
    // longer observing pcm_*. Reset here so a fresh stream starts cleanly.
    pcm_reset();
    ring_reset();
}

static int resetStreamTarget(void)
{
    if (!parseHttpURL(STREAM_URL,
                      stream_host, sizeof(stream_host), &stream_port,
                      stream_path, sizeof(stream_path))) {
        fail("bad STREAM_URL");
        return 0;
    }
    return 1;
}

// --- ring buffer ------------------------------------------------------------

static inline unsigned ring_used(void) {
    unsigned w = atomic_load_explicit(&ring_write, memory_order_acquire);
    unsigned r = atomic_load_explicit(&ring_read,  memory_order_relaxed);
    return (w - r) & RING_MASK;
}

static inline unsigned ring_free(void) {
    return RING_SIZE - 1 - ring_used();
}

static void ring_reset(void) {
    atomic_store_explicit(&ring_write, 0, memory_order_relaxed);
    atomic_store_explicit(&ring_read,  0, memory_order_relaxed);
}

// Validate an MP3 frame header at the given ring position. Rejects the
// reserved bitrate/samplerate/layer/version codes so we don't lock onto random
// 0xFF bytes inside the audio stream.
static int looksLikeFrameHeader(unsigned pos)
{
    unsigned p1 = pos                & RING_MASK;
    unsigned p2 = (pos + 1)          & RING_MASK;
    unsigned p3 = (pos + 2)          & RING_MASK;
    uint8_t b1 = ring[p1];
    uint8_t b2 = ring[p2];
    uint8_t b3 = ring[p3];
    if (b1 != 0xFF) return 0;
    if ((b2 & 0xE0) != 0xE0) return 0;        // sync word top 11 bits
    if ((b2 & 0x18) == 0x08) return 0;        // mpeg version "01" reserved
    if ((b2 & 0x06) == 0x00) return 0;        // layer "00" reserved
    if ((b3 & 0xF0) == 0xF0) return 0;        // bitrate "1111" reserved
    if ((b3 & 0x0C) == 0x0C) return 0;        // samplerate "11" reserved
    return 1;
}

// Layer III frame length in bytes for the header at `pos`. Returns 0 if the
// header has reserved/free-format/invalid fields or isn't Layer III.
static int frameLengthAt(unsigned pos)
{
    uint8_t b2 = ring[(pos + 1) & RING_MASK];
    uint8_t b3 = ring[(pos + 2) & RING_MASK];
    int ver   = (b2 >> 3) & 0x03;  // 3=MPEG-1, 2=MPEG-2, 0=MPEG-2.5
    int layer = (b2 >> 1) & 0x03;  // 1=Layer III
    if (ver == 1 || layer != 1) return 0;

    static const int br_mpeg1[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const int br_mpeg2[16] = {0, 8,16,24,32,40,48,56, 64, 80, 96,112,128,144,160,0};
    static const int sr_mpeg1 [4] = {44100, 48000, 32000, 0};
    static const int sr_mpeg2 [4] = {22050, 24000, 16000, 0};
    static const int sr_mpeg25[4] = {11025, 12000,  8000, 0};

    int br = (ver == 3) ? br_mpeg1[(b3 >> 4) & 0x0F]
                        : br_mpeg2[(b3 >> 4) & 0x0F];
    int sr = (ver == 3) ? sr_mpeg1 [(b3 >> 2) & 0x03]
           : (ver == 2) ? sr_mpeg2 [(b3 >> 2) & 0x03]
                        : sr_mpeg25[(b3 >> 2) & 0x03];
    if (br == 0 || sr == 0) return 0;
    int padding = (b3 >> 1) & 0x01;
    int coeff = (ver == 3) ? 144 : 72;
    return (coeff * br * 1000) / sr + padding;
}

// Some streams prepend ID3v2 tags or HTML error bodies before the first audio
// frame. Scan the prebuffer for two consecutive valid headers at the expected
// stride — that's the canonical "this is really an MP3 stream" check — and
// advance ring_read past any preamble.
static int alignRingToFrameSync(void)
{
    unsigned r = atomic_load_explicit(&ring_read,  memory_order_relaxed);
    unsigned w = atomic_load_explicit(&ring_write, memory_order_acquire);
    unsigned avail = (w - r) & RING_MASK;
    if (avail < 4) return 0;

    for (unsigned i = 0; i + 3 < avail; i++) {
        if (!looksLikeFrameHeader(r + i)) continue;
        int flen = frameLengthAt(r + i);
        if (flen <= 0) continue;
        if (i + (unsigned)flen + 2 >= avail) continue;
        if (!looksLikeFrameHeader(r + i + flen)) continue;
        if (i > 0) {
            logScreen("aligned ring: skipped %u preamble bytes", i);
            atomic_store_explicit(&ring_read, (r + i) & RING_MASK,
                                  memory_order_release);
        }
        return 1;
    }
    logScreen("no MP3 sync found in %u-byte prebuffer", avail);
    return 0;
}

// Drain HTTP's internal buffer into the byte ring. Main thread only.
static void pumpHTTPIntoRing(void)
{
    if (!stream_conn) return;
    while (1) {
        size_t avail = http->getBytesAvailable(stream_conn);
        if (avail == 0) break;

        unsigned writable = ring_free();
        if (writable == 0) break;
        unsigned w = atomic_load_explicit(&ring_write, memory_order_relaxed);
        unsigned contiguous = RING_SIZE - w;
        unsigned chunk = writable < contiguous ? writable : contiguous;
        if ((size_t)chunk > avail) chunk = (unsigned)avail;
        int got = http->read(stream_conn, ring + w, chunk);
        if (got <= 0) break;
        atomic_store_explicit(&ring_write, (w + got) & RING_MASK, memory_order_release);
    }
}

// --- decoder (main thread) --------------------------------------------------

static unsigned ring_peek_contig(uint8_t* dst, unsigned want)
{
    unsigned r = atomic_load_explicit(&ring_read,  memory_order_relaxed);
    unsigned w = atomic_load_explicit(&ring_write, memory_order_acquire);
    unsigned avail = (w - r) & RING_MASK;
    unsigned n = want < avail ? want : avail;
    unsigned first = RING_SIZE - r;
    if (first >= n) {
        memcpy(dst, ring + r, n);
    } else {
        memcpy(dst, ring + r, first);
        memcpy(dst + first, ring, n - first);
    }
    return n;
}

static void ring_consume(unsigned n)
{
    unsigned r = atomic_load_explicit(&ring_read, memory_order_relaxed);
    atomic_store_explicit(&ring_read, (r + n) & RING_MASK, memory_order_release);
}

static inline unsigned pcm_used(void) {
    unsigned w = atomic_load_explicit(&pcm_write, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&pcm_read,  memory_order_acquire);
    return (w - r) & PCM_MASK;
}

static inline unsigned pcm_free(void) {
    return PCM_FRAMES - 1 - pcm_used();
}

static void pcm_reset(void)
{
    atomic_store_explicit(&pcm_write, 0, memory_order_relaxed);
    atomic_store_explicit(&pcm_read,  0, memory_order_relaxed);
}

static void rs_reset(void)
{
    rs_in_rate  = 0;
    rs_step_q16 = 0;
    rs_pos_q16  = 0;
    rs_prev     = 0;
}

// Push one decoded frame's worth of samples into the mono PCM ring, going
// through a stereo→mono downmix and then a linear-interp resampler to the
// Playdate's 44.1 kHz output. rs_prev keeps the last input sample around so
// the lerp at the left edge of each new frame is against the right value.
static void pcm_write_frame(const int16_t* src, int samples, int channels, int hz)
{
    if (hz != rs_in_rate) {
        logScreen("decoder: input=%d Hz, %d ch -> %d Hz", hz, channels, OUTPUT_HZ);
        rs_in_rate  = hz;
        // 64-bit intermediate: (hz << 16) overflows int32 for hz >= 32768.
        rs_step_q16 = (int32_t)(((int64_t)hz << 16) / OUTPUT_HZ);
    }

    static int16_t mono[MINIMP3_MAX_SAMPLES_PER_FRAME / 2];
    if (channels == 2) {
        for (int i = 0; i < samples; i++) {
            int32_t l = src[i * 2];
            int32_t r = src[i * 2 + 1];
            mono[i] = (int16_t)((l + r) >> 1);
        }
    } else {
        for (int i = 0; i < samples; i++) {
            mono[i] = src[i];
        }
    }

    int32_t pos  = rs_pos_q16;
    int32_t step = rs_step_q16;
    int32_t end  = (int32_t)samples << 16;

    unsigned w = atomic_load_explicit(&pcm_write, memory_order_relaxed);
    int written = 0;
    while (pos < end) {
        int idx  = pos >> 16;
        int frac = pos & 0xFFFF;
        int16_t a = (idx == 0) ? rs_prev : mono[idx - 1];
        int16_t b = mono[idx];
        int32_t s = a + (int32_t)(((int64_t)(b - a) * frac) >> 16);
        pcm[(w + written) & PCM_MASK] = (int16_t)s;
        written++;
        pos += step;
    }
    atomic_store_explicit(&pcm_write, (w + written) & PCM_MASK,
                          memory_order_release);

    rs_prev    = mono[samples - 1];
    rs_pos_q16 = pos - end;
}

// Decode up to `max_frames` from the byte ring into the PCM ring. Bounded so
// a glut of MP3 data doesn't pin the main loop for too long.
static void decodeIntoPCMRing(int max_frames)
{
    for (int n = 0; n < max_frames; n++) {
        if (pcm_free() < MAX_RESAMPLED_PER_FRAME) break;

        unsigned got = ring_peek_contig(mp3_scratch, MP3_SCRATCH);
        if (got < 4) break;

        mp3dec_frame_info_t info = {0};
        int samples = mp3dec_decode_frame(&mp3dec, mp3_scratch, (int)got,
                                          decode_buf, &info);
        if (info.frame_bytes > 0) {
            ring_consume((unsigned)info.frame_bytes);
        } else {
            break;
        }
        if (samples > 0) {
            pcm_write_frame(decode_buf, samples, info.channels, info.hz);
        }
    }
}

// --- audio callback (audio thread) ------------------------------------------

static int audioCallback(void* ctx, int16_t* left, int16_t* right, int len)
{
    (void)ctx;
    (void)right;

    unsigned r = atomic_load_explicit(&pcm_read,  memory_order_relaxed);
    unsigned w = atomic_load_explicit(&pcm_write, memory_order_acquire);
    unsigned avail = (w - r) & PCM_MASK;
    unsigned want  = (unsigned)len < avail ? (unsigned)len : avail;

    unsigned first = PCM_FRAMES - r;
    if (first > want) first = want;
    memcpy(left, pcm + r, first * sizeof(int16_t));
    if (first < want) {
        memcpy(left + first, pcm, (want - first) * sizeof(int16_t));
    }

    for (unsigned i = want; i < (unsigned)len; i++) {
        left[i] = 0;
    }

    atomic_store_explicit(&pcm_read, (r + want) & PCM_MASK,
                          memory_order_release);
    return 1;
}

// --- URL parsing ------------------------------------------------------------

static int parseHttpURL(const char* url, char* host, size_t hostlen,
                        int* port, char* path, size_t pathlen)
{
    static const char prefix[] = "http://";
    if (strncmp(url, prefix, sizeof(prefix) - 1) != 0) return 0;
    const char* p = url + sizeof(prefix) - 1;

    const char* host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/') host_end++;
    size_t hl = host_end - p;
    if (hl == 0 || hl >= hostlen) return 0;
    memcpy(host, p, hl);
    host[hl] = 0;

    *port = 80;
    if (*host_end == ':') {
        host_end++;
        char* end;
        long parsed = strtol(host_end, &end, 10);
        if (end == host_end || parsed <= 0 || parsed > 65535) return 0;
        *port = (int)parsed;
        host_end = end;
    }

    if (*host_end == 0) {
        if (pathlen < 2) return 0;
        path[0] = '/'; path[1] = 0;
    } else {
        size_t pl = strlen(host_end);
        if (pl >= pathlen) return 0;
        memcpy(path, host_end, pl + 1);
    }
    return 1;
}

// --- access reply -----------------------------------------------------------

static void onAccessReply(bool allowed, void* userdata)
{
    (void)userdata;
    logScreen("access reply: allowed=%d", allowed);
    if (allowed) {
        state = kStartingStream;
    } else {
        fail("network access denied by user");
    }
}

// --- stream connection ------------------------------------------------------

static int stream_status = 0;

static void onStreamHeadersRead(HTTPConnection* c)
{
    int status = http->getResponseStatus(c);
    stream_status = status;
    logScreen("headers complete: status=%d", status);
}

static void onStreamRequestComplete(HTTPConnection* c)
{
    if (stream_status == 200 || stream_status == 0) return;

    char body[256];
    size_t blen = 0;
    while (http->getBytesAvailable(c) > 0 && blen < sizeof(body) - 1) {
        size_t room = sizeof(body) - 1 - blen;
        int got = http->read(c, (uint8_t*)body + blen, (unsigned)room);
        if (got <= 0) break;
        blen += got;
    }
    body[blen] = 0;
    if (blen > 0) logScreen("body: %.60s", body);

    char why[64];
    snprintf(why, sizeof(why), "status %d", stream_status);
    requestRetry(why);
}

static void onStreamConnectionClosed(HTTPConnection* c)
{
    PDNetErr err = http->getError(c);
    logScreen("stream closed (err=%d)", err);
    if (state == kPrebuffering) {
        requestRetry("closed before playback");
    }
}

static void startStreamRequest(void)
{
    stream_status = 0;
    logScreen("connecting %s:%d", stream_host, stream_port);
    stream_conn = http->newConnection(stream_host, stream_port, false);
    if (!stream_conn) { fail("newConnection failed"); return; }

    http->setKeepAlive(stream_conn, false);
    http->setConnectTimeout(stream_conn, 10000);
    http->setReadTimeout(stream_conn, 15000);
    http->setReadBufferSize(stream_conn, STREAM_HTTP_READ_BUFFER);
    http->setHeadersReadCallback(stream_conn, onStreamHeadersRead);
    http->setRequestCompleteCallback(stream_conn, onStreamRequestComplete);
    http->setConnectionClosedCallback(stream_conn, onStreamConnectionClosed);

    static const char headers[] =
        "User-Agent: minimp3demo/0.1\r\n";
    PDNetErr err = http->get(stream_conn, stream_path, headers, sizeof(headers) - 1);
    if (err != NET_OK) { fail("http->get err=%d", err); return; }

    prebuffer_start_ms = pd->system->getCurrentTimeMilliseconds();
    state = kPrebuffering;
}

// Initialize minimp3 and register the audio source. The first audio callback
// after this returns silence (zero-fill) until decodeIntoPCMRing has produced
// enough samples. Caller has already aligned the byte ring to a Layer III
// sync. Mono: the device hard-faults if the source is stereo.
static int startStreamPlayer(void)
{
    mp3dec_init(&mp3dec);
    pcm_reset();
    rs_reset();
    mp3_source = pd->sound->channel->addCallbackSource(
        pd->sound->getDefaultChannel(), audioCallback, NULL, 0);
    return mp3_source != NULL;
}

static void startPlayback(void)
{
    if (!startStreamPlayer()) { fail("addSource failed"); return; }
    state = kPlaying;
}

// --- update -----------------------------------------------------------------

static int update(void* ctx)
{
    (void)ctx;

    if (pending == kPendingRetry) {
        pending = kPendingNone;
        logScreen("retry: %s", pending_reason);
        teardownPlayback();
        reconnect_at_ms = pd->system->getCurrentTimeMilliseconds()
                        + RECONNECT_DELAY_MS;
        state = kReconnectWait;
    }

    switch (state) {
        case kBoot:
            state = kRequestingAccess;
            break;

        case kRequestingAccess: {
            enum accessReply r = http->requestAccess(
                ACCESS_HOST, ACCESS_PORT, false,
                "stream BBC World Service", onAccessReply, NULL);
            logScreen("requestAccess: reply=%d", r);
            if      (r == kAccessAllow) state = kStartingStream;
            else if (r == kAccessDeny)  fail("network access denied");
            else                        state = kWaitingForAccess;
            break;
        }

        case kWaitingForAccess:
            // onAccessReply advances the state.
            break;

        case kStartingStream:
            startStreamRequest();
            break;

        case kPrebuffering: {
            pumpHTTPIntoRing();
            unsigned used = ring_used();
            if (used >= PREBUFFER_BYTES) {
                if (!alignRingToFrameSync()) {
                    requestRetry("no MP3 frame sync");
                    break;
                }
                logScreen("prebuffered %u bytes, playing", used);
                startPlayback();
            } else {
                unsigned now = pd->system->getCurrentTimeMilliseconds();
                if (now - prebuffer_start_ms > PREBUFFER_TIMEOUT_MS) {
                    char why[48];
                    snprintf(why, sizeof(why), "buffer timeout (%u B)", used);
                    requestRetry(why);
                }
            }
            break;
        }

        case kPlaying: {
            pumpHTTPIntoRing();
            // 16 frames @ ~26 ms each ≈ 420 ms, covering one 50 ms tick with
            // generous headroom.
            decodeIntoPCMRing(16);

            // Watchdog over the audio thread's read cursor.
            static unsigned last_pcm_read_seen  = 0;
            static unsigned last_pcm_advance_ms = 0;
            unsigned now = pd->system->getCurrentTimeMilliseconds();
            unsigned r   = atomic_load_explicit(&pcm_read, memory_order_acquire);
            if (r != last_pcm_read_seen) {
                last_pcm_read_seen  = r;
                last_pcm_advance_ms = now;
            } else if (last_pcm_advance_ms == 0) {
                last_pcm_advance_ms = now;
            }

            if (now - last_pcm_advance_ms > STALL_TIMEOUT_MS) {
                logScreen("audio stalled %u ms", now - last_pcm_advance_ms);
                last_pcm_advance_ms = 0;
                last_pcm_read_seen  = 0;
                requestRetry("stalled");
            }
            break;
        }

        case kReconnectWait: {
            unsigned now = pd->system->getCurrentTimeMilliseconds();
            if ((int)(reconnect_at_ms - now) <= 0) {
                state = kStartingStream;
            }
            break;
        }

        case kFailed:
            break;
    }

    // --- render -------------------------------------------------------------
    pd->graphics->clear(kColorWhite);

    int y = 4;
    for (int i = 0; i < log_count; i++) {
        int idx = (log_head - log_count + i + LOG_LINES) % LOG_LINES;
        const char* s = log_lines[idx];
        pd->graphics->drawText(s, strlen(s), kUTF8Encoding, 4, y);
        y += 16;
    }

    return 1;
}

// --- entry ------------------------------------------------------------------

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg)
{
    (void)arg;

    if (event == kEventInit) {
        pd   = playdate;
        http = pd->network->http;

        logScreen("minimp3 demo booting");
        if (!resetStreamTarget()) return 0;

        pd->system->setAutoLockDisabled(1);
        pd->display->setRefreshRate(20);
        pd->system->setUpdateCallback(update, NULL);
    } else if (event == kEventTerminate) {
        if (mp3_source)  { pd->sound->removeSource(mp3_source); mp3_source = NULL; }
        if (stream_conn) { http->close(stream_conn); http->release(stream_conn); stream_conn = NULL; }
    }

    return 0;
}
