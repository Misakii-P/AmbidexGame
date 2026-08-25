#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <3ds.h>
#include <citro2d.h>

extern const u8 topbg_t3x[];
extern const u8 topbg_t3x_end[];
extern const u8 botbg_t3x[];
extern const u8 botbg_t3x_end[];
extern const u8 abgamelogo_t3x[];
extern const u8 abgamelogo_t3x_end[];
extern const u8 warningmsg_t3x[];
extern const u8 warningmsg_t3x_end[];

#define SERVER_PORT 3002
#define MAX_PLAYERS 16
#define MAX_SLOTS   12
#define MAX_RESULTS 6
#define BUFFER_SIZE 4096
#define SOC_ALIGN   0x1000
#define SOC_BUFSIZE 0x80000

#define TOP_W 400.0f
#define BOT_W 320.0f

enum View {
    VIEW_IP,
    VIEW_CONNECTING,
    VIEW_SLOTS,
    VIEW_LOBBY,
    VIEW_SETUP,
    VIEW_VOTEREADY,
    VIEW_VOTE,
    VIEW_RESULTS,
    VIEW_STANDINGS,
    VIEW_ERROR
};

struct Player { int id; char name[64]; int points; };
struct Slot {
    int id;
    char type[8];
    char name[96];
    int connected;
    int voted;
};
struct Pairing { int s1, s2; };
struct GameResult {
    int s1, s2;
    char v1[8], v2[8];
    int c1, c2;
};

static enum View g_view = VIEW_IP;
static int g_fadePhase = 2;   /* 0 idle, 1 fade-out, 2 fade-in */
static int g_fadeAlpha = 255; /* black overlay alpha 0..255 */
static enum View g_fadeNext = VIEW_IP;

static void transitionTo(enum View v)
{
    if (g_fadePhase == 0 && v != g_view) {
        g_fadePhase = 1;
        g_fadeAlpha = 0;
    } else if (g_fadePhase == 2 && v != g_view) {
        g_fadePhase = 1;
    }
    g_fadeNext = v;
}

static int g_sock = -1;
static int g_connecting = 0;
static int g_connWait = 0;
static int g_haveState = 0;
static char g_errorMsg[160] = "";

static char g_phase[16] = "";
static int g_currentRound = 0;
static int g_mySlotId = -1;

static struct Player g_players[MAX_PLAYERS];
static int g_numPlayers = 0;
static struct Slot g_slots[MAX_SLOTS];
static int g_slotCount = 0;
static struct Pairing g_pairings[MAX_SLOTS / 2];
static int g_numPairings = 0;
static struct GameResult g_results[MAX_RESULTS];
static int g_numResults = 0;

static char g_banner[192] = "";
static int g_bannerTimer = 0;

static int g_octets[4] = {192, 168, 1, 100};
static int g_selectedSlot = 0;
static int g_voteChoice = 0;
static int g_votedLocal = 0;
static int g_voteReady = 0;
static int g_readyGlow = 0;
static char g_prevPhase[16] = "";
static int g_prevMySlot = -1;
static u32 g_frame = 0;

static char g_incoming[BUFFER_SIZE];
static int g_incomingLen = 0;

static u32 *g_socBuf = NULL;
static int g_socInited = 0;
static u32 g_socErr = 0;

static C3D_RenderTarget *s_top, *s_bot;
static C2D_SpriteSheet s_topSheet, s_botSheet, s_logoSheet, s_warnSheet;
static C2D_TextBuf s_textBuf;

#define MAX_WAVS 8

typedef struct {
    const char *name;
    s16 *pcm;
    u32 bytes;
    u32 rate;
    int channels;
} CachedWav;

static CachedWav s_cache[MAX_WAVS];
static ndspWaveBuf s_musicWb;
static ndspWaveBuf s_sfxWb;
static int s_musicIx = -1;
static int s_ndspOk = 0;
static char s_audioStat[48] = "SND: boot";

#define COL_TEXT  C2D_Color32(0xE0, 0xE8, 0xF0, 0xFF)
#define COL_DIM   C2D_Color32(0x8A, 0x96, 0xA8, 0xFF)
#define COL_SEL   C2D_Color32(0x00, 0xFF, 0x80, 0xFF)
#define COL_ERR   C2D_Color32(0xFF, 0x60, 0x60, 0xFF)
#define COL_ALLY  C2D_Color32(0x40, 0xE8, 0xA0, 0xFF)
#define COL_BETRY C2D_Color32(0xFF, 0x70, 0x70, 0xFF)
#define COL_ACC   C2D_Color32(0x30, 0xB8, 0xE8, 0xFF)
#define COL_PANEL C2D_Color32(0x08, 0x0C, 0x12, 0xC8)
#define COL_HL    C2D_Color32(0x00, 0xFF, 0x80, 0x38)
#define COL_LINE  C2D_Color32(0x30, 0xB8, 0xE8, 0x60)

static void audioFail(const char *why)
{
    snprintf(s_audioStat, sizeof(s_audioStat), "SND FAIL: %s", why);
}

typedef struct {
    const char *name;
    const u8 *data;
    u32 len;
} WavEntry;

extern const WavEntry wav_blobs[];

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static s16 *loadWavMem(const u8 *data, u32 len, u32 *outRate, int *outChannels, u32 *outBytes)
{
    const u8 *p = data;
    const u8 *e = data + len;
    u16 channels = 0, fmtCode = 0;
    u32 rate = 0;
    const u8 *dataPtr = NULL;
    u32 dataBytes = 0;
    s16 *pcm;

    if (len < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) {
        audioFail("riff");
        return NULL;
    }
    p += 12;

    while (p + 8 <= e && !dataPtr) {
        u32 csz = rd32(p + 4);
        if (!memcmp(p, "fmt ", 4)) {
            if (csz < 16 || p + 24 > e) break;
            fmtCode  = rd16(p + 8);
            channels = rd16(p + 10);
            rate     = rd32(p + 12);
        } else if (!memcmp(p, "data", 4)) {
            if (p + 8 + csz > e) csz = (u32)(e - p - 8);
            dataPtr   = p + 8;
            dataBytes = csz;
        }
        p += 8 + csz + (csz & 1);
    }

    if (!fmtCode || !channels || !rate || !dataPtr || !dataBytes ||
        channels > 2 || fmtCode != 1) {
        audioFail("fmt");
        return NULL;
    }

    pcm = (s16 *)linearAlloc(dataBytes);
    if (!pcm) { audioFail("mem"); return NULL; }
    memcpy(pcm, dataPtr, dataBytes);
    *outRate = rate;
    *outChannels = channels;
    *outBytes = dataBytes;
    return pcm;
}

static void audioInit(void)
{
    const WavEntry *w;
    int n = 0, ok = 0;
    u32 rate = 0, bytes = 0, total = 0;
    int channels = 0;

    for (w = wav_blobs; w->name && n < MAX_WAVS; w++, n++) {
        s16 *pcm = loadWavMem(w->data, w->len, &rate, &channels, &bytes);
        s_cache[n].name = w->name;
        s_cache[n].pcm = pcm;
        s_cache[n].bytes = pcm ? bytes : 0;
        s_cache[n].rate = pcm ? rate : 0;
        s_cache[n].channels = pcm ? channels : 0;
        if (pcm) { ok++; total += bytes; }
    }
    if (n > 0 && ok == n)
        snprintf(s_audioStat, sizeof(s_audioStat), "SND ok %d/%d %ukB",
                 ok, n, (unsigned)(total / 1024));
    else
        snprintf(s_audioStat, sizeof(s_audioStat), "SND FAIL %d/%d", ok, n);
}

static int cacheFind(const char *name)
{
    int i;
    for (i = 0; i < MAX_WAVS; i++) {
        if (s_cache[i].name && !strcmp(s_cache[i].name, name))
            return s_cache[i].pcm ? i : -1;
    }
    return -1;
}

static void channelSetup(int ch, u32 rate, int channels)
{
    float mix[12] = {0};
    ndspChnWaveBufClear(ch);
    ndspChnReset(ch);
    ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
    ndspChnSetRate(ch, (float)rate);
    ndspChnSetFormat(ch, channels==2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(ch, mix);
}

static void musicStop(void)
{
    if (!s_ndspOk) return;
    ndspChnWaveBufClear(0);
    s_musicIx = -1;
}

static void playMusic(const char *name)
{
    const CachedWav *c;
    int ix;

    if (!s_ndspOk) return;
    ix = cacheFind(name);
    if (ix < 0) { audioFail("miss"); return; }
    if (ix == s_musicIx) return;

    musicStop();
    c = &s_cache[ix];
    s_musicIx = ix;
    DSP_FlushDataCache(c->pcm, c->bytes);

    channelSetup(0, c->rate, c->channels);
    memset(&s_musicWb, 0, sizeof(s_musicWb));
    s_musicWb.data_vaddr = c->pcm;
    s_musicWb.nsamples   = c->bytes / (c->channels * 2);
    s_musicWb.status     = NDSP_WBUF_DONE;
    ndspChnWaveBufAdd(0, &s_musicWb);
}

static void sfxStop(void)
{
    if (!s_ndspOk) return;
    ndspChnWaveBufClear(1);
}

static void playSfx(const char *name)
{
    const CachedWav *c;
    int ix;

    if (!s_ndspOk) return;
    ix = cacheFind(name);
    if (ix < 0) return;
    c = &s_cache[ix];

    sfxStop();
    DSP_FlushDataCache(c->pcm, c->bytes);

    channelSetup(1, c->rate, c->channels);
    memset(&s_sfxWb, 0, sizeof(s_sfxWb));
    s_sfxWb.data_vaddr = c->pcm;
    s_sfxWb.nsamples   = c->bytes / (c->channels * 2);
    s_sfxWb.status     = NDSP_WBUF_DONE;
    ndspChnWaveBufAdd(1, &s_sfxWb);
}

static void audioShutdown(void)
{
    int i;
    if (s_ndspOk) {
        ndspChnWaveBufClear(0);
        ndspChnWaveBufClear(1);
    }
    for (i = 0; i < MAX_WAVS; i++) {
        if (s_cache[i].pcm) {
            linearFree(s_cache[i].pcm);
            s_cache[i].pcm = NULL;
        }
    }
    ndspExit();
}

static void drawTextRaw(float x, float y, float sc, u32 col, const C2D_Text *t)
{
    C2D_DrawText(t, C2D_WithColor, x, y, 0.5f, sc, sc, col);
}

static void drawText(float x, float y, float sc, u32 col, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    C2D_Text t;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    C2D_TextFontParse(&t, NULL, s_textBuf, buf);
    C2D_TextOptimize(&t);
    drawTextRaw(x, y, sc, col, &t);
}

static float textWidth(const char *s, float sc)
{
    C2D_Text t;
    float w = 0.0f;
    C2D_TextFontParse(&t, NULL, s_textBuf, s);
    C2D_TextGetDimensions(&t, sc, sc, &w, NULL);
    return w;
}

static void drawTextC(float cx, float y, float sc, u32 col, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    C2D_Text t;
    float w;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    C2D_TextFontParse(&t, NULL, s_textBuf, buf);
    C2D_TextOptimize(&t);
    C2D_TextGetDimensions(&t, sc, sc, &w, NULL);
    drawTextRaw(cx - w/2.0f, y, sc, col, &t);
}

static float wrapText(const char *s, float cx, float y, float maxX, float sc, u32 col)
{
    char line[512];
    size_t len = 0;
    const char *p = s;
    float lh = 30.0f * sc + 6.0f;
    line[0] = 0;
    while (*p) {
        const char *sp = strchr(p, ' ');
        size_t wlen = sp ? (size_t)(sp - p + 1) : strlen(p);
        if (wlen >= sizeof(line)-1) wlen = sizeof(line)-1;
        if (len + wlen >= sizeof(line)-1) break;
        memcpy(line+len, p, wlen);
        len += wlen;
        line[len] = 0;
        if (textWidth(line, sc) > maxX && len > wlen) {
            len -= wlen;
            line[len] = 0;
            drawTextC(cx, y, sc, col, "%s", line);
            y += lh;
            memmove(line, p, wlen);
            len = wlen;
            line[len] = 0;
        }
        p += wlen;
    }
    if (len > 0) {
        drawTextC(cx, y, sc, col, "%s", line);
        y += lh;
    }
    return y;
}

static void panel(float x, float y, float w, float h)
{
    C2D_DrawRectSolid(x, y, 0.20f, w, h, COL_PANEL);
    C2D_DrawRectSolid(x, y, 0.25f, w, 2.0f, COL_ACC);
}

static void header(float cx, const char *title, const char *sub)
{
    drawTextC(cx, 12.0f, 0.58f, COL_ACC, "%s", title);
    if (sub) drawTextC(cx, 36.0f, 0.42f, COL_DIM, "%s", sub);
    C2D_DrawRectSolid(cx - 80.0f, 33.0f, 0.35f, 160.0f, 1.5f, COL_LINE);
}

static void showBanner(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_banner, sizeof(g_banner), fmt, ap);
    va_end(ap);
    g_bannerTimer = 240;
    playSfx("wrong");
}

static const char *jsonFindValue(const char *json, const char *key)
{
    static char kbuf[64];
    char *p;
    snprintf(kbuf, sizeof(kbuf), "\"%s\"", key);
    p = strstr((char *)json, kbuf);
    if (!p) return NULL;
    p = strstr(p, ":");
    if (!p) return NULL;
    p++;
    while (*p==' '||*p=='\t'||*p=='\n'||*p=='\r') p++;
    return p;
}

static void parseInt(const char *json, const char *key, int def, int *out)
{
    const char *p = jsonFindValue(json, key);
    *out = def;
    if (!p) return;
    if (*p=='n'||*p=='t'||*p=='f') return;
    *out = (int)strtol(p, NULL, 10);
}

static void parseStr(const char *json, const char *key, char *out, int maxLen)
{
    const char *p = jsonFindValue(json, key);
    int i = 0;
    out[0] = 0;
    if (!p) return;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && i < maxLen-1) {
            if (*p == '\\' && *(p+1)) p++;
            out[i++] = *p++;
        }
        out[i] = 0;
        return;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != '\n' && i < maxLen-1) out[i++] = *p++;
    out[i] = 0;
    if (!strcmp(out, "null")) out[0] = 0;
}

static void parseBool(const char *json, const char *key, int *out)
{
    const char *p = jsonFindValue(json, key);
    *out = 0;
    if (!p) return;
    *out = (*p == 't');
}

static void pushIncoming(const char *data, int len)
{
    if (g_incomingLen + len >= (int)sizeof(g_incoming) - 1) return;
    memcpy(g_incoming + g_incomingLen, data, len);
    g_incomingLen += len;
    g_incoming[g_incomingLen] = 0;
}

static int hasJsonMessage(void)
{
    char *p = g_incoming;
    int depth = 0;
    while (*p) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) return (int)(p - g_incoming + 1); }
        p++;
    }
    return 0;
}

static void consumeJsonMessage(int len)
{
    if (len > g_incomingLen) len = g_incomingLen;
    memmove(g_incoming, g_incoming + len, g_incomingLen - len + 1);
    g_incomingLen -= len;
}

static void sendJson(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_sock >= 0) send(g_sock, buf, strlen(buf), 0);
}

static void parseState(const char *json)
{
    char *p, *end;

    parseStr(json, "phase", g_phase, sizeof(g_phase));
    parseInt(json, "currentRound", 0, &g_currentRound);
    parseInt(json, "mySlotId", -1, &g_mySlotId);

    p = strstr(json, "\"players\"");
    if (p && (p = strstr(p, "["))) {
        g_numPlayers = 0;
        p++;
        while (*p && *p != ']' && g_numPlayers < MAX_PLAYERS) {
            struct Player *pl;
            char block[512];
            int blen;
            while (*p && *p != '{') p++;
            if (!*p || *p != '{') break;
            end = strchr(p, '}');
            if (!end) break;
            blen = (int)(end - p + 1);
            if (blen > 511) blen = 511;
            memcpy(block, p, blen);
            block[blen] = 0;
            pl = &g_players[g_numPlayers++];
            memset(pl, 0, sizeof(*pl));
            parseInt(block, "id", 0, &pl->id);
            parseStr(block, "name", pl->name, sizeof(pl->name));
            parseInt(block, "points", 0, &pl->points);
            p = end + 1;
        }
    }

    p = strstr(json, "\"slots\"");
    if (p && (p = strstr(p, "["))) {
        g_slotCount = 0;
        p++;
        while (*p && *p != ']' && g_slotCount < MAX_SLOTS) {
            struct Slot *s;
            char block[512];
            int blen;
            while (*p && *p != '{') p++;
            if (!*p || *p != '{') break;
            end = strchr(p, '}');
            if (!end) break;
            blen = (int)(end - p + 1);
            if (blen > 511) blen = 511;
            memcpy(block, p, blen);
            block[blen] = 0;
            s = &g_slots[g_slotCount++];
            memset(s, 0, sizeof(*s));
            parseInt(block, "id", 0, &s->id);
            parseStr(block, "type", s->type, sizeof(s->type));
            parseStr(block, "name", s->name, sizeof(s->name));
            parseBool(block, "connected", &s->connected);
            parseBool(block, "voted", &s->voted);
            p = end + 1;
        }
    }

    p = strstr(json, "\"pairings\"");
    if (p && (p = strstr(p, "["))) {
        g_numPairings = 0;
        p++;
        while (*p && *p != ']' && g_numPairings < MAX_SLOTS/2) {
            struct Pairing *pr;
            char block[256];
            int blen;
            while (*p && *p != '{') p++;
            if (!*p || *p != '{') break;
            end = strchr(p, '}');
            if (!end) break;
            blen = (int)(end - p + 1);
            if (blen > 255) blen = 255;
            memcpy(block, p, blen);
            block[blen] = 0;
            pr = &g_pairings[g_numPairings++];
            parseInt(block, "slot1Id", 0, &pr->s1);
            parseInt(block, "slot2Id", 0, &pr->s2);
            p = end + 1;
        }
    }

    p = strstr(json, "\"allResults\"");
    if (p && (p = strstr(p, "["))) {
        g_numResults = 0;
        p++;
        while (*p && *p != ']' && g_numResults < MAX_RESULTS) {
            struct GameResult *r;
            char block[768];
            int blen;
            while (*p && *p != '{') p++;
            if (!*p || *p != '{') break;
            end = strchr(p, '}');
            if (!end) break;
            blen = (int)(end - p + 1);
            if (blen > 767) blen = 767;
            memcpy(block, p, blen);
            block[blen] = 0;
            r = &g_results[g_numResults++];
            memset(r, 0, sizeof(*r));
            parseInt(block, "slot1Id", 0, &r->s1);
            parseInt(block, "slot2Id", 0, &r->s2);
            parseStr(block, "slot1Vote", r->v1, sizeof(r->v1));
            parseStr(block, "slot2Vote", r->v2, sizeof(r->v2));
            parseInt(block, "slot1Change", 0, &r->c1);
            parseInt(block, "slot2Change", 0, &r->c2);
            p = end + 1;
        }
    }

    g_haveState = 1;
}

static struct Slot *findSlot(int id)
{
    int i;
    for (i = 0; i < g_slotCount; i++)
        if (g_slots[i].id == id) return &g_slots[i];
    return NULL;
}

static const char *slotName(int id)
{
    struct Slot *s = findSlot(id);
    return (s && s->name[0]) ? s->name : "-";
}

static int slotJoinable(struct Slot *s)
{
    return s && s->name[0] && !s->connected;
}

static int countVoted(void)
{
    int i, voted = 0;
    for (i = 0; i < g_numPairings; i++) {
        struct Slot *a = findSlot(g_pairings[i].s1);
        struct Slot *b = findSlot(g_pairings[i].s2);
        if (a && a->voted) voted++;
        if (b && b->voted) voted++;
    }
    return voted;
}

static void processMessage(const char *json)
{
    char msgType[32] = "";
    parseStr(json, "type", msgType, sizeof(msgType));

    if (!strcmp(msgType, "error")) {
        char m[160];
        parseStr(json, "message", m, sizeof(m));
        if (m[0]) showBanner("%s", m);
        return;
    }
    if (!strcmp(msgType, "pong")) return;

    parseState(json);
}

static void updateView(void)
{
    int joined = g_mySlotId > 0;
    int votingEntered, resultsEntered;

    if (strcmp(g_prevPhase, "voting") != 0 && !strcmp(g_phase, "voting")) {
        votingEntered = 1;
    } else {
        votingEntered = 0;
    }
    if (strcmp(g_prevPhase, "results") != 0 && !strcmp(g_phase, "results")) {
        resultsEntered = 1;
    } else {
        resultsEntered = 0;
    }

    if (g_haveState) {
        if (strcmp(g_phase, "voting")) g_voteReady = 0;
        if (!joined) {
            if (g_view != VIEW_ERROR && g_view != VIEW_IP)
                transitionTo(VIEW_SLOTS);
        } else {
            if (!strcmp(g_phase, "lobby"))          transitionTo(VIEW_LOBBY);
            else if (!strcmp(g_phase, "roundSetup")) transitionTo(VIEW_SETUP);
            else if (!strcmp(g_phase, "voting"))     transitionTo(g_voteReady ? VIEW_VOTE : VIEW_VOTEREADY);
            else if (!strcmp(g_phase, "results"))    transitionTo(VIEW_RESULTS);
            else if (!strcmp(g_phase, "roundEnd"))   transitionTo(VIEW_STANDINGS);
            else                                     transitionTo(VIEW_LOBBY);
        }
    }

    if (votingEntered) {
        g_voteChoice = 0;
        g_votedLocal = 0;
        playSfx("start");
    }
    if (resultsEntered) playSfx("resound");

    snprintf(g_prevPhase, sizeof(g_prevPhase), "%s", g_phase);
    g_prevMySlot = g_mySlotId;
}

static void socEnsure(void)
{
    int res;
    if (g_socInited) return;
    g_socBuf = (u32 *)memalign(SOC_ALIGN, SOC_BUFSIZE);
    if (!g_socBuf) { g_socErr = 1; return; }
    memset(g_socBuf, 0, SOC_BUFSIZE);
    res = socInit(g_socBuf, SOC_BUFSIZE);
    if (res != 0) {
        free(g_socBuf);
        g_socBuf = NULL;
        g_socErr = (u32)res;
        return;
    }
    g_socInited = 1;
}

static void disconnect(void)
{
    if (g_sock >= 0) { close(g_sock); g_sock = -1; }
    g_connecting = 0;
    g_connWait = 0;
    g_incomingLen = 0;
    g_incoming[0] = 0;
    g_haveState = 0;
    g_mySlotId = -1;
    g_votedLocal = 0;
    g_voteReady = 0;
    g_readyGlow = 0;
    g_selectedSlot = 0;
    g_phase[0] = 0;
    g_prevPhase[0] = 0;
    g_prevMySlot = -1;
}

static void leaveGame(void)
{
    disconnect();
    transitionTo(VIEW_IP);
}

static void connectToServer(void)
{
    char ip[64];
    int rc, flags;
    struct sockaddr_in addr;

    disconnect();
    socEnsure();
    if (!g_socInited) {
        if (g_socErr > 1)
            showBanner("SOC init failed (%08X)", g_socErr);
        else
            showBanner("SOC buffer alloc failed");
        transitionTo(VIEW_IP);
        return;
    }
    snprintf(ip, sizeof(ip), "%d.%d.%d.%d",
        g_octets[0], g_octets[1], g_octets[2], g_octets[3]);
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) {
        showBanner("Socket failed");
        transitionTo(VIEW_IP);
        return;
    }
    flags = fcntl(g_sock, F_GETFL, 0);
    fcntl(g_sock, F_SETFL, flags | O_NONBLOCK);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_aton(ip, &addr.sin_addr);
    rc = connect(g_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EAGAIN) {
        close(g_sock);
        g_sock = -1;
        showBanner("Connect failed (%s, err %d)", ip, (int)errno);
        transitionTo(VIEW_IP);
        return;
    }
    g_connecting = 1;
    g_connWait = 450;
    transitionTo(VIEW_CONNECTING);
}

static int ipPrompt(void)
{
    SwkbdState sw;
    char buf[16];
    unsigned a, b, c, d;
    char extra;

    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             g_octets[0], g_octets[1], g_octets[2], g_octets[3]);
    swkbdInit(&sw, SWKBD_TYPE_NORMAL, 2, 15);
    swkbdSetButton(&sw, SWKBD_BUTTON_LEFT, "Back", false);
    swkbdSetButton(&sw, SWKBD_BUTTON_RIGHT, "OK", true);
    swkbdSetHintText(&sw, "Server IP address");
    swkbdSetInitialText(&sw, buf);
    if (swkbdInputText(&sw, buf, sizeof(buf)) != SWKBD_BUTTON_RIGHT)
        return -1;
    if (sscanf(buf, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 ||
        a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    g_octets[0] = (int)a;
    g_octets[1] = (int)b;
    g_octets[2] = (int)c;
    g_octets[3] = (int)d;
    return 1;
}

static void tryRecv(void)
{
    char rbuf[2048];
    int n;

    if (g_sock < 0) return;

    if (g_connecting) {
        fd_set rf, wf;
        struct timeval tv;
        FD_ZERO(&rf);
        FD_SET(g_sock, &rf);
        FD_ZERO(&wf);
        FD_SET(g_sock, &wf);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        n = select((int)g_sock + 1, &rf, &wf, NULL, &tv);
        if (n <= 0) return;
        g_connecting = 0;
        g_connWait = 0;
        musicStop();
        playSfx("connected");
    }

    n = recv(g_sock, rbuf, sizeof(rbuf) - 1, 0);
    if (n > 0) {
        pushIncoming(rbuf, n);
        while ((n = hasJsonMessage()) > 0) {
            char msg[BUFFER_SIZE];
            int clen = n > (int)sizeof(msg) - 1 ? (int)sizeof(msg) - 1 : n;
            memcpy(msg, g_incoming, clen);
            msg[clen] = 0;
            consumeJsonMessage(n);
            processMessage(msg);
        }
    } else if (n == 0) {
        snprintf(g_errorMsg, sizeof(g_errorMsg), "Disconnected from host");
        disconnect();
        transitionTo(VIEW_ERROR);
    } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINPROGRESS) {
            snprintf(g_errorMsg, sizeof(g_errorMsg), "Network error (%d)", (int)errno);
            disconnect();
            transitionTo(VIEW_ERROR);
        }
    }
}

static void drawTopBg(void)
{
    if (s_topSheet)
        C2D_DrawImageAt(C2D_SpriteSheetGetImage(s_topSheet, 0), 0, 0, 0.1f, NULL, 1.0f, 1.0f);
}

static void drawBotBg(void)
{
    if (s_botSheet)
        C2D_DrawImageAt(C2D_SpriteSheetGetImage(s_botSheet, 0), 0, 0, 0.1f, NULL, 1.0f, 1.0f);
}

static void drawRoster(float cx, float y, int highlightMe)
{
    int i;
    float xL = cx - 130.0f, xR = cx + 130.0f;
    float rowH = 18.0f;
    int showPts = 0;
    for (i = 0; i < g_numPlayers && i < MAX_PLAYERS; i++)
        if (g_players[i].points) { showPts = 1; break; }
    if (highlightMe && showPts) {
        drawText(xL + 14.0f, y, 0.36f, COL_DIM, "PLAYER");
        drawText(xR - 34.0f, y, 0.36f, COL_DIM, "PTS");
        C2D_DrawRectSolid(xL, y + 13.0f, 0.15f, xR - xL, 1.0f, COL_LINE);
        y += 18.0f;
    }
    for (i = 0; i < g_numPlayers && i < MAX_PLAYERS; i++) {
        struct Player *pl = &g_players[i];
        struct Slot *sl = findSlot(pl->id);
        int me = highlightMe && pl->id == g_mySlotId;
        u32 nameCol = me ? COL_SEL : (sl && !sl->connected ? COL_DIM : COL_TEXT);
        char pts[16];
        float w;
        if ((i & 1) == 0)
            C2D_DrawRectSolid(xL, y - 3.0f, 0.12f, xR - xL, rowH - 2.0f,
                              C2D_Color32(0xFF, 0xFF, 0xFF, 0x0B));
        if (me)
            drawText(xL, y, 0.46f, COL_SEL, ">");
        drawText(xL + 12.0f, y, me ? 0.46f : 0.44f, nameCol, "%.18s", pl->name);
        if (highlightMe && showPts) {
            snprintf(pts, sizeof(pts), "%d", pl->points);
            w = textWidth(pts, 0.44f);
            drawText(xR - w, y, 0.44f, me ? COL_SEL : COL_DIM, "%s", pts);
        }
        y += rowH;
    }
}

static void drawTopIp(void)
{
    drawTopBg();
    if (s_logoSheet)
        C2D_DrawImageAt(C2D_SpriteSheetGetImage(s_logoSheet, 0), 0.0f, 0.0f, 0.15f,
                        NULL, 1.0f, 1.0f);
}

static void drawTopConnecting(void)
{
    char ip[64];
    static const char dots[4][4] = {"", ".", "..", "..."};
    snprintf(ip, sizeof(ip), "%d.%d.%d.%d",
        g_octets[0], g_octets[1], g_octets[2], g_octets[3]);
    drawTopBg();
    drawTextC(TOP_W/2, 88.0f, 0.62f, COL_ACC, "CONNECTING%s", dots[(g_frame/20) % 4]);
    drawTextC(TOP_W/2, 128.0f, 0.48f, COL_TEXT, "%s", ip);
    drawTextC(TOP_W/2, 216.0f, 0.34f, s_audioStat[4]=='o' ? COL_DIM : COL_ERR, "%s", s_audioStat);
}

static void drawTopSlots(void)
{
    drawTopBg();
    drawRoster(TOP_W/2, 40.0f, 0);
}

static void drawTopLobby(void)
{
    drawTopBg();
    header(TOP_W/2, "LOBBY", "Waiting for the host to start");
    drawRoster(TOP_W/2, 66.0f, 1);
}

static void drawTopSetup(void)
{
    int g;
    float y = 62.0f;
    drawTopBg();
    header(TOP_W/2, "ROUND SETUP", g_currentRound > 0 ? NULL : "First round");
    if (g_currentRound > 0) {
        drawTextC(TOP_W/2, 36.0f, 0.42f, COL_DIM, "Round %d", g_currentRound);
    }
    for (g = 0; g < 3; g++) {
        drawTextC(TOP_W/2, y, 0.45f, COL_ACC, "GROUP %d", g+1);
        y += 18.0f;
        drawTextC(TOP_W/2, y, 0.42f, COL_TEXT, "Pair: %.22s", slotName(g*2+1));
        y += 16.0f;
        drawTextC(TOP_W/2, y, 0.42f, COL_TEXT, "Solo: %.22s", slotName(g*2+2));
        y += 20.0f;
    }
}

static void drawTopReady(void)
{
    drawTopBg();
    if (s_warnSheet)
        C2D_DrawImageAt(C2D_SpriteSheetGetImage(s_warnSheet, 0), 0.0f, 0.0f, 0.15f,
                        NULL, 1.0f, 1.0f);
}

static void drawTopVote(void)
{
    int i, partnerId = -1;
    int total, voted;
    float y = 64.0f;

    drawTopBg();
    header(TOP_W/2, "VOTE", NULL);

    for (i = 0; i < g_numPairings; i++) {
        if (g_pairings[i].s1 == g_mySlotId) partnerId = g_pairings[i].s2;
        if (g_pairings[i].s2 == g_mySlotId) partnerId = g_pairings[i].s1;
    }
    if (partnerId > 0)
        drawTextC(TOP_W/2, y, 0.45f, COL_TEXT, "You: %.16s", slotName(g_mySlotId)), y += 18.0f,
        drawTextC(TOP_W/2, y, 0.45f, COL_TEXT, "Against: %.16s", slotName(partnerId));
    else
        drawTextC(TOP_W/2, y, 0.45f, COL_TEXT, "You: %.16s", slotName(g_mySlotId));
    y += 26.0f;

    voted = countVoted();
    total = g_numPairings * 2;
    drawTextC(TOP_W/2, y, 0.4f, COL_DIM, "Votes in: %d / %d", voted, total);
    y += 16.0f;
    C2D_DrawRectSolid(100.0f, y, 0.35f, 200.0f, 8.0f, C2D_Color32(0x28, 0x30, 0x3C, 0xC0));
    if (total > 0)
        C2D_DrawRectSolid(100.0f, y, 0.36f, 200.0f * ((float)voted / (float)total), 8.0f, COL_SEL);

    if (findSlot(g_mySlotId) && (findSlot(g_mySlotId)->voted || g_votedLocal))
        drawTextC(TOP_W/2, 196.0f, 0.5f, COL_SEL, "Vote locked in");
}

static void drawTopResults(void)
{
    int i;
    float y = 58.0f;
    drawTopBg();
    header(TOP_W/2, "RESULTS", NULL);
    if (g_currentRound > 0)
        drawTextC(TOP_W/2, 36.0f, 0.42f, COL_DIM, "Round %d", g_currentRound);
    for (i = 0; i < g_numResults; i++) {
        struct GameResult *r = &g_results[i];
        int mine = (r->s1 == g_mySlotId || r->s2 == g_mySlotId);
        float x = 30.0f;
        char b[40];
        u32 c1 = !strcmp(r->v1, "ally") ? COL_ALLY : COL_BETRY;
        u32 c2 = !strcmp(r->v2, "ally") ? COL_ALLY : COL_BETRY;
        if (mine) drawText(16.0f, y, 0.4f, COL_SEL, ">");
        snprintf(b, sizeof(b), "%.13s", slotName(r->s1));
        drawText(x, y, 0.4f, COL_TEXT, b);
        x += textWidth(b, 0.4f) + 3.0f;
        snprintf(b, sizeof(b), "%.6s %+d", r->v1[0] ? r->v1 : "?", r->c1);
        drawText(x, y, 0.4f, c1, b);
        x += textWidth(b, 0.4f) + 9.0f;
        drawText(x, y, 0.4f, COL_DIM, "vs");
        x += textWidth("vs", 0.4f) + 9.0f;
        snprintf(b, sizeof(b), "%.13s", slotName(r->s2));
        drawText(x, y, 0.4f, COL_TEXT, b);
        x += textWidth(b, 0.4f) + 3.0f;
        snprintf(b, sizeof(b), "%.6s %+d", r->v2[0] ? r->v2 : "?", r->c2);
        drawText(x, y, 0.4f, c2, b);
        y += 19.0f;
    }
}

static void drawTopStandings(void)
{
    int order[MAX_PLAYERS];
    int i, j;
    float y = 60.0f;
    for (i = 0; i < g_numPlayers; i++) order[i] = i;
    for (i = 1; i < g_numPlayers; i++) {
        j = i;
        while (j > 0 && g_players[order[j-1]].points < g_players[order[j]].points) {
            int t = order[j-1];
            order[j-1] = order[j];
            order[j] = t;
            j--;
        }
    }
    drawTopBg();
    header(TOP_W/2, "STANDINGS", g_currentRound > 0 ? NULL : NULL);
    for (i = 0; i < g_numPlayers && i < MAX_PLAYERS; i++) {
        struct Player *pl = &g_players[order[i]];
        int me = pl->id == g_mySlotId;
        if (me) drawTextC(TOP_W/2, y, 0.45f, COL_SEL, "%d. %s  %d", i+1, pl->name, pl->points);
        else    drawTextC(TOP_W/2, y, 0.45f, COL_TEXT, "%d. %s  %d", i+1, pl->name, pl->points);
        y += 17.0f;
    }
}

static void drawTopError(void)
{
    drawTopBg();
    header(TOP_W/2, "ERROR", NULL);
    wrapText(g_errorMsg, TOP_W/2, 78.0f, TOP_W - 40.0f, 0.48f, COL_ERR);
}

static void drawTop(void)
{
    switch (g_view) {
    case VIEW_IP:         drawTopIp(); break;
    case VIEW_CONNECTING: drawTopConnecting(); break;
    case VIEW_SLOTS:      drawTopSlots(); break;
    case VIEW_LOBBY:      drawTopLobby(); break;
    case VIEW_SETUP:      drawTopSetup(); break;
    case VIEW_VOTEREADY:  drawTopReady(); break;
    case VIEW_VOTE:       drawTopVote(); break;
    case VIEW_RESULTS:    drawTopResults(); break;
    case VIEW_STANDINGS:  drawTopStandings(); break;
    case VIEW_ERROR:      drawTopError(); break;
    }
}

#define IP_BOX_X   60.0f
#define IP_BOX_Y   78.0f
#define IP_BOX_W  200.0f
#define IP_BOX_H   34.0f

static void drawBottomIp(void)
{
    drawBotBg();
    panel(24.0f, 44.0f, BOT_W - 48.0f, 108.0f);
    drawTextC(BOT_W/2, 54.0f, 0.45f, COL_DIM, "SERVER IP");
    C2D_DrawRectSolid(IP_BOX_X, IP_BOX_Y, 0.30f, IP_BOX_W, IP_BOX_H,
                      C2D_Color32(0x28, 0x30, 0x3C, 0xE0));
    C2D_DrawRectSolid(IP_BOX_X + 2.0f, IP_BOX_Y + 2.0f, 0.31f,
                      IP_BOX_W - 4.0f, IP_BOX_H - 4.0f,
                      C2D_Color32(0x10, 0x16, 0x20, 0xE0));
    drawTextC(BOT_W/2, IP_BOX_Y + 9.0f, 0.55f, COL_TEXT, "%d.%d.%d.%d",
              g_octets[0], g_octets[1], g_octets[2], g_octets[3]);
    drawTextC(BOT_W/2, 214.0f, 0.38f, COL_DIM, "START: exit");
}

static void drawBottomConnecting(void)
{
    static const char dots[4][4] = {"", ".", "..", "..."};
    drawBotBg();
    panel(30.0f, 86.0f, BOT_W - 60.0f, 68.0f);
    drawTextC(BOT_W/2, 98.0f, 0.55f, COL_ACC, "CONNECTING%s", dots[(g_frame/20) % 4]);
    drawTextC(BOT_W/2, 126.0f, 0.4f, COL_DIM, "B: cancel");
}

#define SLOT_COLS 3
#define SLOT_ROWS 2
#define SLOT_CARD_W 82.0f
#define SLOT_CARD_H 72.0f
#define COL_CARD_FILL  C2D_Color32(0x07, 0x4C, 0x3D, 0xFF)
#define COL_CARD_EMPTY C2D_Color32(0x1C, 0x37, 0x31, 0xFF)

static const float slotCardX[SLOT_COLS] = { 17.0f, 120.0f, 222.0f };
static const float slotCardY[SLOT_ROWS] = { 64.0f, 150.0f };

static int slotCardAt(float tx, float ty)
{
    int c, r;
    for (r = 0; r < SLOT_ROWS; r++) {
        for (c = 0; c < SLOT_COLS; c++) {
            float x = slotCardX[c], y = slotCardY[r];
            if (tx >= x && tx < x + SLOT_CARD_W && ty >= y && ty < y + SLOT_CARD_H)
                return r * SLOT_COLS + c;
        }
    }
    return -1;
}

static void drawBottomSlots(void)
{
    int i;
    drawBotBg();
    drawTextC(BOT_W/2, 10.0f, 0.80f, COL_TEXT, "PICK YOUR SLOT");
    for (i = 0; i < g_slotCount && i < SLOT_COLS * SLOT_ROWS; i++) {
        struct Slot *s = &g_slots[i];
        float x = slotCardX[i % SLOT_COLS], y = slotCardY[i / SLOT_COLS];
        float cx = x + SLOT_CARD_W / 2.0f;
        int sel = (i == g_selectedSlot);
        int empty = !s->name[0];
        u32 fill = empty ? COL_CARD_EMPTY : COL_CARD_FILL;
        C2D_DrawRectSolid(x + 2.0f, y, 0.13f, SLOT_CARD_W - 4.0f, SLOT_CARD_H, fill);
        C2D_DrawRectSolid(x, y + 2.0f, 0.13f, SLOT_CARD_W, SLOT_CARD_H - 4.0f, fill);
        if (sel) {
            C2D_DrawRectSolid(x - 2.0f, y - 2.0f, 0.14f, SLOT_CARD_W + 4.0f, 2.0f, COL_SEL);
            C2D_DrawRectSolid(x - 2.0f, y + SLOT_CARD_H, 0.14f, SLOT_CARD_W + 4.0f, 2.0f, COL_SEL);
            C2D_DrawRectSolid(x - 2.0f, y, 0.14f, 2.0f, SLOT_CARD_H, COL_SEL);
            C2D_DrawRectSolid(x + SLOT_CARD_W, y, 0.14f, 2.0f, SLOT_CARD_H, COL_SEL);
        }
        if (empty) {
            drawTextC(cx, y + 26.0f, 0.40f, COL_DIM, "Empty slot");
        } else if (!strcmp(s->type, "pair")) {
            const char *amp = strstr(s->name, " & ");
            u32 nameCol = s->connected ? COL_DIM : COL_TEXT;
            if (amp) {
                char n2[48];
                size_t l1 = (size_t)(amp - s->name);
                if (l1 > 22) l1 = 22;
                drawTextC(cx, y + 10.0f, 0.42f, nameCol, "%.*s", (int)l1, s->name);
                snprintf(n2, sizeof(n2), "%s", amp + 3);
                drawTextC(cx, y + 28.0f, 0.42f, nameCol, "%.20s", n2);
            } else {
                drawTextC(cx, y + 19.0f, 0.42f, nameCol, "%.20s", s->name);
            }
            drawTextC(cx, y + 48.0f, 0.36f, COL_DIM, "Pair");
        } else {
            drawTextC(cx, y + 19.0f, 0.42f, s->connected ? COL_DIM : COL_TEXT,
                      "%.20s", s->name);
            drawTextC(cx, y + 48.0f, 0.36f, COL_DIM, "Solo");
        }
    }
}

static void drawBottomLobby(void)
{
    struct Slot *s = findSlot(g_mySlotId);
    drawBotBg();
    panel(20.0f, 82.0f, BOT_W - 40.0f, 74.0f);
    drawTextC(BOT_W/2, 94.0f, 0.52f, COL_ACC, "JOINED");
    if (s && s->name[0])
        drawTextC(BOT_W/2, 120.0f, 0.42f, COL_TEXT, "Playing as: %.20s", s->name);
    else
        drawTextC(BOT_W/2, 120.0f, 0.42f, COL_TEXT, "Waiting...");
}

static void drawBottomSetup(void)
{
    drawBotBg();
    panel(20.0f, 88.0f, BOT_W - 40.0f, 62.0f);
    drawTextC(BOT_W/2, 102.0f, 0.5f, COL_SEL, "ROUND %d", g_currentRound);
    drawTextC(BOT_W/2, 126.0f, 0.4f, COL_DIM, "Host is preparing the round...");
}

static void drawBottomVote(void)
{
    static const char *opts[2] = {"ALLY", "BETRAY"};
    const u32 ocols[2] = {COL_ALLY, COL_BETRY};
    int i;
    int voted = (findSlot(g_mySlotId) && findSlot(g_mySlotId)->voted) || g_votedLocal;

    drawBotBg();
    header(BOT_W/2, "CAST YOUR VOTE", NULL);
    for (i = 0; i < 2; i++) {
        float by = 62.0f + i * 62.0f;
        int sel = (i == g_voteChoice);
        u32 edge = sel ? ocols[i] : C2D_Color32(0x28, 0x30, 0x3C, 0xE0);
        u32 txt  = voted ? COL_DIM : (sel ? ocols[i] : COL_TEXT);
        C2D_DrawRectSolid(40.0f, by, 0.30f, BOT_W - 80.0f, 46.0f, edge);
        if (sel && !voted)
            C2D_DrawRectSolid(43.0f, by + 3.0f, 0.31f, BOT_W - 86.0f, 40.0f,
                C2D_Color32(0x00, 0x00, 0x00, 0xA0));
        drawTextC(BOT_W/2, by + 12.0f, 0.58f, txt, "%s%s", opts[i], sel ? " <" : "");
    }
    if (voted)
        drawTextC(BOT_W/2, 200.0f, 0.48f, COL_SEL, "Vote submitted!");
    else
        drawTextC(BOT_W/2, 200.0f, 0.4f, COL_DIM, "Up/Down: choose  A: confirm");
}

#define READY_BTN_W 220.0f
#define READY_BTN_H 70.0f
#define READY_BTN_X ((BOT_W - READY_BTN_W) / 2.0f)
#define READY_BTN_Y 85.0f
#define COL_READY      C2D_Color32(0x4B, 0xED, 0xD1, 0xFF)
#define COL_READY_WRAP C2D_Color32(0x4B, 0xED, 0xD1, 0x66)
#define COL_READY_GLOW C2D_Color32(0x4B, 0xED, 0xD1, 0x22)
#define COL_READY_RED  C2D_Color32(0xFF, 0x47, 0x57, 0xFF)
#define COL_BTN_FILL   C2D_Color32(0x0A, 0x3E, 0x32, 0xFF)

static void chamferRect(float x, float y, float w, float h, u32 col)
{
    C2D_DrawRectSolid(x + 2.0f, y, 0.30f, w - 4.0f, h, col);
    C2D_DrawRectSolid(x, y + 2.0f, 0.30f, w, h - 4.0f, col);
}

static void drawBottomReady(void)
{
    float cx = BOT_W / 2.0f;
    float bx = READY_BTN_X, by = READY_BTN_Y;
    u32 edge = COL_READY;
    u32 redA = 0;
    drawBotBg();
    if (g_readyGlow > 0) {
        int t = 72 - g_readyGlow;
        int inten = t < 20 ? t * 12 : 200 - (t - 20) * 4;
        if (inten < 0) inten = 0;
        if (inten > 200) inten = 200;
        redA = (u32)inten;
        edge = COL_READY_RED;
    }
    chamferRect(bx - 12.0f, by - 9.0f, READY_BTN_W + 24.0f, READY_BTN_H + 18.0f, COL_READY_GLOW);
    chamferRect(bx - 7.0f, by - 7.0f, READY_BTN_W + 14.0f, READY_BTN_H + 14.0f, COL_READY_WRAP);
    chamferRect(bx - 5.0f, by - 5.0f, READY_BTN_W + 10.0f, READY_BTN_H + 10.0f, COL_BTN_FILL);
    chamferRect(bx, by, READY_BTN_W, READY_BTN_H, edge);
    chamferRect(bx + 2.0f, by + 2.0f, READY_BTN_W - 4.0f, READY_BTN_H - 4.0f, COL_BTN_FILL);
    if (redA)
        C2D_DrawRectSolid(bx + 6.0f, by + 6.0f, 0.32f, READY_BTN_W - 12.0f, READY_BTN_H - 12.0f,
                          C2D_Color32(0xFF, 0x47, 0x57, redA));
    drawTextC(cx, by + 30.0f, 0.62f, COL_READY, "START");
}

static void drawBottomResults(void)
{
    int i, myChange = 0, found = 0;
    drawBotBg();
    for (i = 0; i < g_numResults; i++) {
        struct GameResult *r = &g_results[i];
        if (r->s1 == g_mySlotId) { myChange = r->c1; found = 1; }
        if (r->s2 == g_mySlotId) { myChange = r->c2; found = 1; }
    }
    if (found) {
        char buf[16];
        u32 col = myChange >= 0 ? COL_ALLY : COL_BETRY;
        snprintf(buf, sizeof(buf), "%+d", myChange);
        panel(60.0f, 66.0f, BOT_W - 120.0f, 92.0f);
        drawTextC(BOT_W/2, 76.0f, 0.42f, COL_DIM, "YOUR RESULT");
        drawTextC(BOT_W/2, 100.0f, 0.95f, col, "%s", buf);
    } else {
        panel(20.0f, 88.0f, BOT_W - 40.0f, 60.0f);
        drawTextC(BOT_W/2, 108.0f, 0.45f, COL_DIM, "Not paired this round");
    }
    drawTextC(BOT_W/2, 180.0f, 0.42f, COL_DIM, "See results on the top screen");
}

static void drawBottomStandings(void)
{
    int order[MAX_PLAYERS];
    int i, j;
    float y = 52.0f;
    for (i = 0; i < g_numPlayers; i++) order[i] = i;
    for (i = 1; i < g_numPlayers; i++) {
        j = i;
        while (j > 0 && g_players[order[j-1]].points < g_players[order[j]].points) {
            int t = order[j-1];
            order[j-1] = order[j];
            order[j] = t;
            j--;
        }
    }
    drawBotBg();
    header(BOT_W/2, "SCOREBOARD", NULL);
    for (i = 0; i < g_numPlayers && i < 9; i++) {
        struct Player *pl = &g_players[order[i]];
        int me = pl->id == g_mySlotId;
        drawText(34.0f, y, 0.42f, me ? COL_SEL : COL_TEXT, "%d.", i+1);
        drawText(56.0f, y, 0.42f, me ? COL_SEL : COL_TEXT, "%.16s", pl->name);
        drawText(BOT_W - 76.0f, y, 0.42f, me ? COL_SEL : COL_TEXT, "%d pts", pl->points);
        y += 18.0f;
    }
}

static void drawBottomError(void)
{
    drawBotBg();
    panel(20.0f, 82.0f, BOT_W - 40.0f, 76.0f);
    drawTextC(BOT_W/2, 94.0f, 0.52f, COL_ERR, "CONNECTION ERROR");
    drawTextC(BOT_W/2, 124.0f, 0.38f, COL_DIM, "%.40s", g_errorMsg);
    drawTextC(BOT_W/2, 210.0f, 0.45f, COL_TEXT, "A: retry   B: back");
}

static void drawBanner(void)
{
    if (g_bannerTimer <= 0) return;
    C2D_DrawRectSolid(0.0f, 0.0f, 0.40f, BOT_W, 26.0f, C2D_Color32(0x50, 0x10, 0x10, 0xD0));
    drawTextC(BOT_W/2, 5.0f, 0.4f, COL_ERR, "%.46s", g_banner);
}

static void drawBottom(void)
{
    switch (g_view) {
    case VIEW_IP:         drawBottomIp(); break;
    case VIEW_CONNECTING: drawBottomConnecting(); break;
    case VIEW_SLOTS:      drawBottomSlots(); break;
    case VIEW_LOBBY:      drawBottomLobby(); break;
    case VIEW_SETUP:      drawBottomSetup(); break;
    case VIEW_VOTEREADY:  drawBottomReady(); break;
    case VIEW_VOTE:       drawBottomVote(); break;
    case VIEW_RESULTS:    drawBottomResults(); break;
    case VIEW_STANDINGS:  drawBottomStandings(); break;
    case VIEW_ERROR:      drawBottomError(); break;
    }
    drawBanner();
}

static void moveSel(int dir)
{
    int tries;
    if (g_slotCount <= 0) return;
    for (tries = 0; tries < g_slotCount; tries++) {
        g_selectedSlot += dir;
        if (g_selectedSlot < 0) g_selectedSlot = g_slotCount - 1;
        if (g_selectedSlot >= g_slotCount) g_selectedSlot = 0;
        if (slotJoinable(&g_slots[g_selectedSlot])) break;
    }
    playSfx("selection");
}

static void handleInput(u32 down)
{
    switch (g_view) {
    case VIEW_IP: {
        int r = -1;
        if (down & KEY_A) {
            r = ipPrompt();
        } else if (down & KEY_TOUCH) {
            touchPosition tp;
            hidTouchRead(&tp);
            if (tp.px >= IP_BOX_X && tp.px < IP_BOX_X + IP_BOX_W &&
                tp.py >= IP_BOX_Y && tp.py < IP_BOX_Y + IP_BOX_H)
                r = ipPrompt();
        }
        if (r == 1) {
            connectToServer();
        } else if (r == 0) {
            showBanner("Invalid IP address");
            playSfx("wrong");
        }
        break;
    }

    case VIEW_CONNECTING:
        if (down & KEY_B) { disconnect(); transitionTo(VIEW_IP); }
        break;

    case VIEW_SLOTS:
        if (down & KEY_UP)    moveSel(-SLOT_COLS);
        if (down & KEY_DOWN)  moveSel(SLOT_COLS);
        if (down & KEY_LEFT)  moveSel(-1);
        if (down & KEY_RIGHT) moveSel(1);
        if ((down & KEY_A) && g_selectedSlot >= 0 && g_selectedSlot < g_slotCount &&
            slotJoinable(&g_slots[g_selectedSlot])) {
            sendJson("{\"type\":\"join-slot\",\"slotId\":%d}\n", g_slots[g_selectedSlot].id);
            g_votedLocal = 0;
            playSfx("start");
        }
        if (down & KEY_B) leaveGame();
        if (down & KEY_TOUCH) {
            touchPosition tp;
            hidTouchRead(&tp);
            int idx = slotCardAt((float)tp.px, (float)tp.py);
            if (idx >= 0 && idx < g_slotCount) {
                g_selectedSlot = idx;
                if (slotJoinable(&g_slots[idx])) {
                    sendJson("{\"type\":\"join-slot\",\"slotId\":%d}\n", g_slots[idx].id);
                    g_votedLocal = 0;
                    playSfx("start");
                } else {
                    playSfx("wrong");
                }
            }
        }
        break;

    case VIEW_LOBBY:
    case VIEW_SETUP:
    case VIEW_STANDINGS:
    case VIEW_RESULTS:
        if (down & KEY_B) leaveGame();
        break;

    case VIEW_VOTEREADY:
        if (down & KEY_B) leaveGame();
        if (g_readyGlow == 0 && (down & (KEY_A | KEY_TOUCH))) {
            int hit = (down & KEY_A) != 0;
            if (down & KEY_TOUCH) {
                touchPosition tp;
                hidTouchRead(&tp);
                hit = tp.px >= READY_BTN_X - 8.0f && tp.px < READY_BTN_X + READY_BTN_W + 8.0f &&
                      tp.py >= READY_BTN_Y - 8.0f && tp.py < READY_BTN_Y + READY_BTN_H + 8.0f;
            }
            if (hit) {
                g_readyGlow = 72;
                playSfx("start");
            }
        }
        break;

    case VIEW_VOTE:
        if (down & KEY_B) { leaveGame(); break; }
        if (!(findSlot(g_mySlotId) && findSlot(g_mySlotId)->voted) && !g_votedLocal) {
            if ((down & KEY_UP) && g_voteChoice != 0)   { g_voteChoice = 0; playSfx("selection"); }
            if ((down & KEY_DOWN) && g_voteChoice != 1) { g_voteChoice = 1; playSfx("selection"); }
            if (down & KEY_A) {
                sendJson("{\"type\":\"vote\",\"vote\":\"%s\"}\n",
                    g_voteChoice == 0 ? "ally" : "betray");
                g_votedLocal = 1;
                playSfx("vote");
            }
        }
        break;

    case VIEW_ERROR:
        if (down & KEY_A) connectToServer();
        if (down & KEY_B) transitionTo(VIEW_IP);
        break;
    }
}

int main(void)
{
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    s_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    s_topSheet = C2D_SpriteSheetLoadFromMem(topbg_t3x, (size_t)(topbg_t3x_end - topbg_t3x));
    s_botSheet = C2D_SpriteSheetLoadFromMem(botbg_t3x, (size_t)(botbg_t3x_end - botbg_t3x));
    s_logoSheet = C2D_SpriteSheetLoadFromMem(abgamelogo_t3x, (size_t)(abgamelogo_t3x_end - abgamelogo_t3x));
    s_warnSheet = C2D_SpriteSheetLoadFromMem(warningmsg_t3x, (size_t)(warningmsg_t3x_end - warningmsg_t3x));

    s_textBuf = C2D_TextBufNew(4096);

    if (ndspInit() == 0) {
        s_ndspOk = 1;
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspSetMasterVol(1.0f);
        audioInit();
        playMusic("title");
    } else {
        audioFail("ndspInit");
    }

    socEnsure();

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START) break;

        if (g_fadePhase != 1) handleInput(down);
        tryRecv();

        if (g_connecting) {
            g_connWait--;
            if (g_connWait <= 0) {
                snprintf(g_errorMsg, sizeof(g_errorMsg), "No response from host");
                disconnect();
                transitionTo(VIEW_ERROR);
            }
        }

        if (g_fadePhase == 1) {
            g_fadeAlpha += 18;
            if (g_fadeAlpha >= 255) {
                g_fadeAlpha = 255;
                g_view = g_fadeNext;
                g_fadePhase = 2;
            }
        } else if (g_fadePhase == 2) {
            g_fadeAlpha -= 18;
            if (g_fadeAlpha <= 0) {
                g_fadeAlpha = 0;
                g_fadePhase = 0;
            }
        }

        if (g_bannerTimer > 0) g_bannerTimer--;

        if (g_readyGlow > 0) {
            g_readyGlow--;
            if (g_readyGlow == 0 && g_view == VIEW_VOTEREADY) {
                g_voteReady = 1;
                transitionTo(VIEW_VOTE);
            }
        }

        updateView();

        g_frame++;

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(s_textBuf);

        C2D_TargetClear(s_top, C2D_Color32(0x00, 0x00, 0x00, 0xFF));
        C2D_SceneBegin(s_top);
        drawTop();
        if (g_fadeAlpha > 0)
            C2D_DrawRectSolid(0.0f, 0.0f, 0.90f, TOP_W, 240.0f,
                              C2D_Color32(0x00, 0x00, 0x00, (u32)g_fadeAlpha));

        C2D_TargetClear(s_bot, C2D_Color32(0x00, 0x00, 0x00, 0xFF));
        C2D_SceneBegin(s_bot);
        drawBottom();
        if (g_fadeAlpha > 0)
            C2D_DrawRectSolid(0.0f, 0.0f, 0.90f, BOT_W, 240.0f,
                              C2D_Color32(0x00, 0x00, 0x00, (u32)g_fadeAlpha));

        C3D_FrameEnd(0);
        gspWaitForVBlank();
    }

    disconnect();
    if (g_socInited) { socExit(); g_socInited = 0; }
    if (g_socBuf) { free(g_socBuf); g_socBuf = NULL; }
    audioShutdown();
    if (s_textBuf) C2D_TextBufDelete(s_textBuf);
    if (s_topSheet) C2D_SpriteSheetFree(s_topSheet);
    if (s_botSheet) C2D_SpriteSheetFree(s_botSheet);
    if (s_logoSheet) C2D_SpriteSheetFree(s_logoSheet);
    if (s_warnSheet) C2D_SpriteSheetFree(s_warnSheet);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
