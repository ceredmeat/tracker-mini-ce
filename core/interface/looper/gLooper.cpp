// tracker-mini-ce: the Looper screen, drawing. See looper.h.

#include "looper.h"

#ifdef CE_LOOPER

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "AudioLooper.h"
#include "FT812.h"
#include "display.h"

using namespace lp;

// layout (800 x 480)
static constexpr int kLaneH = 106;
static constexpr int kWaveX = 14;
static constexpr int kWaveW = 772;            // multiple of 4 (L2 bitmap)
static constexpr int kWaveTop = 12;
static constexpr int kWaveH = 88;
static constexpr int kKeysY = 424;
static constexpr uint32_t kLaneStride = kWaveW / 4;
static constexpr uint32_t kLaneBytes = kLaneStride * kWaveH;
static constexpr uint32_t kLaneRamG = 683000;   // past the controls' display lists (150000 + 65 x 8192)
static constexpr int kEdH = 300;
static constexpr int kEdY = 60;
static constexpr uint32_t kEdBytes = kLaneStride * kEdH;
static constexpr uint32_t kEdRamG = 751000;
static constexpr uint8_t kBitmapHandle = 14;

// colours
static constexpr uint32_t kGrey = 0x8C8C8C, kWhite = 0xE8E8E8, kDim = 0x4A4A4A, kYellow = 0xE2D25E;
static constexpr uint32_t kRed = 0xE04848, kBeatLine = 0x1E1E1E, kBarLine = 0x3A3A3A, kBarNum = 0x5E5E5E;
static constexpr uint32_t kWaveGrey = 0x9E9E9E;

static uint8_t* scratch() { return AudioLooper::uiScratch(); }

static inline int font(int i) { return fonts[i].handle; }   // 0 R14, 1 R17, 2 B14, 3 B17, 4 R12

static void text(int x, int y, int f, uint16_t opt, const char* s, uint32_t color)
{
    API_COLOR(color);
    API_CMD_TEXT(x, y, font(f), opt, s);
}

static void rect(int x0, int y0, int x1, int y1, uint32_t color)
{
    API_COLOR(color);
    API_BEGIN(RECTS);
    API_VERTEX2F(x0, y0);
    API_VERTEX2F(x1, y1);
    API_END();
}

static void vline(int x, int y0, int y1, uint32_t color)
{
    API_COLOR(color);
    API_BEGIN(LINES);
    API_VERTEX2F(x, y0);
    API_VERTEX2F(x, y1);
    API_END();
}

static void drawBitmap(uint32_t addr, int x, int y, int w, int h, uint32_t tint)
{
    API_BITMAP_HANDLE(kBitmapHandle);
    API_CMD_SETBITMAP(addr, L2, w, h);
    API_COLOR(tint);
    API_BEGIN(BITMAPS);
    API_VERTEX2F(x, y);
    API_END();
}

static uint32_t laneTint(int t, bool selected)
{
    const Track& tr = engine.track[t];
    if (tr.state == TrackState::kRecording || tr.state == TrackState::kOverdub || tr.state == TrackState::kReplace)
        return selected ? 0xE87060 : 0xB85A50;
    if (tr.muted) return selected ? 0x6A6230 : 0x3A3A3A;
    if (tr.state == TrackState::kStopped) return selected ? 0x8A8040 : 0x555555;
    return selected ? kYellow : kWaveGrey;
}

// span shown in a lane, in frames
static uint32_t laneSpanOf(int t)
{
    const Track& tr = engine.track[t];
    if (tr.state == TrackState::kRecording)
    {
        uint32_t bar = (uint32_t)(engine.barLen() >> kFrac);
        uint32_t rec = tr.recFramesTotal;
        uint32_t span = bar ? ((rec / bar) + 1) * bar : rec;
        return span ? span : 1;
    }
    return tr.len;
}

//------------------------------------------------------------------------------
// waveform bitmaps (L2: four levels, tinted when drawn)

void cLooper::renderLane(int t, uint8_t* buf, int w, int h, uint32_t span)
{
    const Track& tr = engine.track[t];
    int stride = w / 4;
    memset(buf, 0, (size_t)stride * h);
    uint32_t have = (tr.state == TrackState::kRecording) ? tr.recFramesTotal : tr.len;
    if (span == 0 || have == 0) return;
    int half = h / 2;
    for (int x = 0; x < w; x++)
    {
        uint32_t f0 = (uint32_t)((uint64_t)x * span / w);
        uint32_t f1 = (uint32_t)((uint64_t)(x + 1) * span / w);
        if (f0 >= have) break;
        if (f1 > have) f1 = have;
        if (f1 <= f0) f1 = f0 + 1;
        uint32_t n = f1 - f0;
        uint32_t step = n > 96 ? n / 96 : 1;
        int pk[2] = {0, 0};
        int64_t sum[2] = {0, 0};
        uint32_t cnt = 0;
        for (uint32_t f = f0; f < f1; f += step)
        {
            for (int c = 0; c < 2; c++)
            {
                int v = engine.sample(t, f, tr.ch == 2 ? c : 0);
                if (v < 0) v = -v;
                if (v > pk[c]) pk[c] = v;
                sum[c] += v;
            }
            cnt++;
        }
        for (int c = 0; c < 2; c++)
        {
            int hp = pk[c] * half / 30000;
            int hm = (int)(sum[c] / (cnt ? cnt : 1)) * half * 14 / 10 / 30000;
            if (hp > half) hp = half;
            if (hm > hp) hm = hp;
            if (hp == 0 && pk[c] > 40) hp = 1;
            for (int r = 0; r < hp; r++)
            {
                int row = c == 0 ? half - 1 - r : half + r;
                uint8_t lv = r < hm ? 3 : 2;
                uint8_t& b = buf[row * stride + (x >> 2)];
                b |= (uint8_t)(lv << (6 - 2 * (x & 3)));
            }
        }
    }
}

static void writeRamG(const uint8_t* data, uint32_t size, uint32_t addr)
{
    DisplayDriver* d = display.getDisplayDriver();
    while (size)
    {
        uint32_t n = size > 32768 ? 32768 : size;
        d->writeDataRAMG(data, n, addr);
        data += n;
        addr += n;
        size -= n;
    }
}

void cLooper::uploadLanes()
{
    uint32_t ms = now();
    for (int i = 0; i < kTracks; i++)
    {
        int t = (laneNext + i) % kTracks;
        const Track& tr = engine.track[t];
        uint32_t span = laneSpanOf(t);
        bool live = tr.state == TrackState::kRecording || tr.state == TrackState::kOverdub ||
                    tr.state == TrackState::kReplace;
        bool stale = !laneValid[t] || tr.version != laneVersion[t] || span != laneSpan[t];
        if (!stale) continue;
        if (live && laneValid[t] && ms - laneDrawnMs[t] < 150) continue;
        renderLane(t, scratch(), kWaveW, kWaveH, span);
        writeRamG(scratch(), kLaneBytes, kLaneRamG + t * kLaneBytes);
        laneVersion[t] = tr.version;
        laneSpan[t] = span;
        laneDrawnMs[t] = ms;
        laneValid[t] = true;
        laneNext = (uint8_t)((t + 1) % kTracks);
        return;   // one lane per frame keeps the screen moving
    }
}

void cLooper::uploadEditor()
{
    const Track& tr = engine.track[edTrack];
    if (tr.version == edVersion) return;
    renderLane(edTrack, scratch(), kWaveW, kEdH, tr.len);
    writeRamG(scratch(), kEdBytes, kEdRamG);
    edVersion = tr.version;
}

//------------------------------------------------------------------------------
// lanes

static void barLines(int x0, int w, int yTop, int yBot, uint32_t span, pos_t anchor, bool numbers)
{
    if (span == 0) return;
    pos_t bar = engine.barLen(), beat = engine.beatLen();
    if (bar <= 0) return;
    float pxPerFrame = (float)w / span;
    float beatPx = (float)(beat >> kFrac) * pxPerFrame;
    // first bar line at or after the loop start
    pos_t a = anchor % bar;
    if (a < 0) a += bar;
    pos_t firstBar = a ? bar - a : 0;
    int barNo = (int)((anchor + firstBar) / bar);
    API_LINE_WIDTH(16);
    if (beatPx >= 6.0f && !looper.perform)
    {
        API_COLOR(kBeatLine);
        API_BEGIN(LINES);
        pos_t b0 = anchor % beat;
        if (b0 < 0) b0 += beat;
        pos_t first = b0 ? beat - b0 : 0;
        int count = 0;
        for (pos_t p = first; (p >> kFrac) < span && count < 48; p += beat, count++)
        {
            int x = x0 + (int)((p >> kFrac) * pxPerFrame);
            API_VERTEX2F(x, yTop);
            API_VERTEX2F(x, yBot);
        }
        API_END();
    }
    float barPx = (float)(bar >> kFrac) * pxPerFrame;
    if (barPx < 3.0f) return;
    API_COLOR(kBarLine);
    API_BEGIN(LINES);
    int count = 0;
    for (pos_t p = firstBar; (p >> kFrac) < span && count < 32; p += bar, count++)
    {
        int x = x0 + (int)((p >> kFrac) * pxPerFrame);
        API_VERTEX2F(x, yTop);
        API_VERTEX2F(x, yBot);
    }
    API_END();
    if (!numbers || barPx < 40.0f) return;
    count = 0;
    char s[12];
    for (pos_t p = firstBar; (p >> kFrac) < span && count < 16; p += bar, count++)
    {
        int n = barNo + count + 1;
        if (p == 0 && n == 1) continue;
        int x = x0 + (int)((p >> kFrac) * pxPerFrame);
        snprintf(s, sizeof(s), "%d", n);
        text(x + 4, yTop - 1, 4, 0, s, kBarNum);
    }
}

static void paintLane(int t, int x, int y, int w, int h)
{
    (void)w;
    (void)h;
    cLooper& L = looper;
    const Track& tr = engine.track[t];
    bool selected = t == L.sel;
    int wy = y + kWaveTop;
    int cy = wy + kWaveH / 2;
    uint32_t span = laneSpanOf(t);
    uint32_t ms = L.now();

    // tag: track number, state dot
    char s[24];
    snprintf(s, sizeof(s), "%d", t + 1);
    text(x + 4, cy - 8, 4, 0, s, selected ? kYellow : kDim);

    if (tr.state == TrackState::kEmpty)
    {
        rect(x + kWaveX, cy, x + kWaveX + kWaveW, cy + 1, selected ? 0x3A3620 : 0x202020);
        text(x + kWaveX + 4, y + 1, 4, 0, "Empty", kDim);
        return;
    }
    if (tr.state == TrackState::kArmed)
    {
        // waits for sound: the input level against the threshold
        float lvl = engine.inputLevel();
        float thr = dbToGain((float)engine.cfg.thresholdDb);
        int lw = (int)(clampf(lvl, 0, 1) * kWaveW), tx = (int)(thr * kWaveW);
        rect(x + kWaveX, cy - 3, x + kWaveX + lw, cy + 3, 0x6A6A6A);
        rect(x + kWaveX + tx, cy - 10, x + kWaveX + tx + 2, cy + 10, kRed);
        API_POINT_SIZE(16 * 6);
        API_COLOR(kRed);
        API_BEGIN(FTPOINTS);
        API_VERTEX2F(x + 7, y + 6);
        API_END();
        API_POINT_SIZE(16 * 4);
        API_COLOR(0x000000);
        API_BEGIN(FTPOINTS);
        API_VERTEX2F(x + 7, y + 6);
        API_END();
        snprintf(s, sizeof(s), "Armed  %d dB", engine.cfg.thresholdDb);
        text(x + kWaveX + 4, y + 1, 4, 0, s, kGrey);
        return;
    }
    if (tr.state == TrackState::kWaiting)
    {
        int beat = engine.countInBeat();
        if (beat > 0)
            snprintf(s, sizeof(s), "Count-in  %d", beat);
        else
            snprintf(s, sizeof(s), "Waiting for the bar");
        text(x + kWaveX + 4, y + 1, 4, 0, s, kYellow);
        rect(x + kWaveX, cy, x + kWaveX + kWaveW, cy + 1, 0x303030);
        return;
    }

    barLines(x + kWaveX, kWaveW, wy, wy + kWaveH, span, tr.anchor, true);

    if (L.laneValid[t]) drawBitmap(kLaneRamG + t * kLaneBytes, x + kWaveX, wy, kWaveW, kWaveH, laneTint(t, selected));

    // length / state text
    float secs = (float)(tr.state == TrackState::kRecording ? tr.recFramesTotal : tr.len) / kSampleRate;
    const char* st = "";
    switch (tr.state)
    {
        case TrackState::kRecording: st = "  Rec"; break;
        case TrackState::kOverdub: st = "  Overdub"; break;
        case TrackState::kReplace: st = "  Replace"; break;
        case TrackState::kStopped: st = "  Stopped"; break;
        default: break;
    }
    snprintf(s, sizeof(s), "%.2f s%s%s", secs, st, tr.muted ? "  Muted" : "");
    text(x + kWaveX + 4, y + 1, 4, 0, s, tr.state == TrackState::kRecording ? kRed : kGrey);
    if (tr.mode != kModeForward)
    {
        text(x + kWaveX + kWaveW - 4, y + 1, 4, OPT_RIGHTX, modeName(tr.mode), kBarNum);
    }

    // recording dot, pulsing
    if (tr.state == TrackState::kRecording || tr.state == TrackState::kOverdub || tr.state == TrackState::kReplace)
    {
        float ph = (ms % 800) / 800.0f;
        API_COLOR_A((uint8_t)(120 + 135 * sin01(ph)));
        API_POINT_SIZE(16 * 5);
        API_COLOR(kRed);
        API_BEGIN(FTPOINTS);
        API_VERTEX2F(x + 7, y + 6);
        API_END();
        API_COLOR_A(255);
        // every fourth bar of a take: the end of the four bars coming
        if (tr.state == TrackState::kRecording)
        {
            uint32_t bar = (uint32_t)(engine.barLen() >> kFrac);
            if (bar)
            {
                uint32_t inGroup = tr.recFramesTotal % (bar * 4);
                if (inGroup > bar * 3)
                {
                    float k = (float)(inGroup - bar * 3) / bar;
                    API_COLOR_A((uint8_t)(255 * k));
                    rect(x + kWaveX + kWaveW - 2, wy, x + kWaveX + kWaveW, wy + kWaveH, kWhite);
                    API_COLOR_A(255);
                }
            }
        }
    }

    // playhead with its trail
    if (span && (engine.running() || tr.state == TrackState::kRecording) && tr.state != TrackState::kStopped)
    {
        float pos = tr.state == TrackState::kRecording ? (float)tr.recFramesTotal : engine.playhead(t);
        int px = x + kWaveX + (int)(pos * kWaveW / span);
        float stepF = engine.playheadStep(t);
        int dir = stepF < 0 ? 1 : -1;   // trail on the side it came from
        float spd = fabsf(stepF);
        int len = (int)(spd * 10.0f);
        if (len > 30) len = 30;
        for (int k = 5; k >= 1; k--)
        {
            API_COLOR_A((uint8_t)(150 - k * 25));
            int tx = px + dir * k * len / 5;
            rect(tx, wy + 4, tx + 2, wy + kWaveH - 4, kWhite);
        }
        API_COLOR_A(255);
        rect(px - 3, wy - 2, px + 3, wy + kWaveH + 2, kWhite);
        rect(px - 1, wy, px + 1, wy + kWaveH, 0x000000);
    }

    // grains: where they read, as bright as they sound
    if (tr.mode == kModeGranular && engine.running() && span)
    {
        API_POINT_SIZE(16 * 3);
        API_BEGIN(FTPOINTS);
        for (int g = 0; g < kMaxGrains; g++)
        {
            float gp = tr.drawGrainPos[g];
            if (gp < 0) continue;
            float a = tr.drawGrainAmp[g];
            API_COLOR_A((uint8_t)(60 + 195 * a));
            API_COLOR(0xFFFFFF);
            int gx = x + kWaveX + (int)(gp * kWaveW / span);
            int gy = cy - (int)(a * (kWaveH / 2 - 6)) * ((g & 1) ? 1 : -1);
            API_VERTEX2F(gx, gy);
        }
        API_END();
        API_COLOR_A(255);
    }

    // performance mode: the eight effect cells under the waveform (pad columns 5-12)
    if (L.perform)
    {
        int cw = kWaveW / 8;
        uint8_t act = tr.perfHeld | tr.perfLatched;
        for (int f = 0; f < kPerfFx; f++)
        {
            int cx = x + kWaveX + f * cw;
            bool on = (act >> f) & 1;
            API_COLOR_A(on ? 170 : 60);
            rect(cx + 2, y + kLaneH - 18, cx + cw - 2, y + kLaneH - 3, on ? kYellow : 0x404040);
            API_COLOR_A(255);
            text(cx + cw / 2, y + kLaneH - 11, 4, OPT_CENTER, perfName(f), on ? 0x000000 : kGrey);
            if ((tr.perfLatched >> f) & 1)
            {
                API_POINT_SIZE(16 * 2);
                API_COLOR(on ? 0x000000 : kYellow);
                API_BEGIN(FTPOINTS);
                API_VERTEX2F(cx + cw - 8, y + kLaneH - 11);
                API_END();
            }
        }
    }
}

static void paintLaneCb(int id, int x, int y, int w, int h) { paintLane(id, x, y, w, h); }

//------------------------------------------------------------------------------
// key row

static const char* const kStopNames[3] = {"Free", "On bar", "On beat"};
static const char* const kStartNames[3] = {"Now", "Count-in", "On sound"};
static const char* const kAfterNames[3] = {"Play", "Overdub", "Stop"};
static const char* const kTempoNames[2] = {"Tape", "Stretch"};
static const char* const kChannelNames[4] = {"Stereo", "Mono L", "Mono R", "Mono sum"};
static const char* const kClickNames[4] = {"Beep", "Wood", "Hi-hat", "Cowbell"};
static const char* const kFadeNames[4] = {"Off", "1 beat", "1 bar", "2 bars"};
static const char* const kSnapNames[4] = {"Bar", "Beat", "1/16", "Free"};
static const char* const kSpeedNames[3] = {"1/2", "1x", "2x"};
static const char* const kDirNames[3] = {"Forward", "Reverse", "Ping-pong"};

enum KeyFlags : uint8_t
{
    kKfLit = 1,       // name lit (active / selected)
    kKfLine = 2,      // underline
    kKfRed = 4,
};

static void perfKnobText(int f, uint8_t v, char* out, int n)
{
    static const char* const sweep[5] = {"1/4 bar", "1/2 bar", "1 bar", "2 bars", "4 bars"};
    static const char* const tape[4] = {"1 beat", "2 beats", "1 bar", "2 bars"};
    switch (f)
    {
        case kPfRepeat1:
        case kPfRepeat2: snprintf(out, n, "%s", repeatName(v)); break;
        case kPfLowPass: snprintf(out, n, "%d Hz", (int)(80.0f * powf(100.0f, v * 0.01f))); break;
        case kPfHighPass: snprintf(out, n, "%d Hz", (int)(100.0f * powf(80.0f, v * 0.01f))); break;
        case kPfSweepDown:
        case kPfSweepUp: snprintf(out, n, "%s", sweep[clampi(v, 0, 4)]); break;
        case kPfTapeStop: snprintf(out, n, "%s", tape[clampi(v, 0, 3)]); break;
        default: snprintf(out, n, "%d", v); break;
    }
}

void cLooper::keyText(int k, const char*& name, char* v, int vn, uint8_t& fl)
{
    name = "";
    v[0] = 0;
    fl = 0;
    Track& t = engine.track[sel];
    switch (page)
    {
        case kHome:
            if (perform && bar == kBarMain)
            {
                name = perfName(k);
                perfKnobText(k, t.perfKnob[k], v, vn);
                if (((t.perfHeld | t.perfLatched) >> k) & 1) fl = kKfLit | kKfLine;
                return;
            }
            if (bar == kBarModes)
            {
                int m = modePage == 0 ? (k < 7 ? k : -1) : (k < 4 ? kModeHalfTime + k : -1);
                if (m >= 0)
                {
                    name = "MODE";
                    snprintf(v, vn, "%s", modeName(m));
                    if (t.mode == m) fl = kKfLit | kKfLine;
                }
                else if (k == 7)
                {
                    name = "";
                    snprintf(v, vn, "%s", modePage == 0 ? "More" : "Back");
                }
                return;
            }
            if (bar == kBarGrain)
            {
                GrainParams& g = t.grain;
                static const int8_t knobOf[8] = {0, 1, 2, 3, -1, 4, 5, 6};
                switch (k)
                {
                    case 0: name = "SPREAD"; snprintf(v, vn, "%d %%", g.spread); break;
                    case 1: name = "SIZE"; snprintf(v, vn, "%d ms", (int)(20.0f * powf(25.0f, g.size * 0.01f))); break;
                    case 2:
                        name = "DENSITY";
                        if (g.density > 47 && g.density < 53)
                            snprintf(v, vn, "None");
                        else
                            snprintf(v, vn, "%s %d", g.density < 50 ? "Even" : "Rand", g.density < 50 ? (50 - g.density) * 2 : (g.density - 50) * 2);
                        break;
                    case 3: name = "PITCH"; snprintf(v, vn, "%+d st", g.pitch); break;
                    case 4: name = "GRAINS"; snprintf(v, vn, "Close"); break;
                    case 5: name = "SPEED"; snprintf(v, vn, "%s", kSpeedNames[g.speed]); break;
                    case 6: name = "DIRECTION"; snprintf(v, vn, "%s", kDirNames[g.direction]); break;
                    case 7: name = "TEXTURE"; snprintf(v, vn, "%d %%", g.texture); break;
                }
                if (knobOf[k] >= 0 && knobOf[k] == grainKnob) fl = kKfLit | kKfLine;
                return;
            }
            if (!shift)
            {
                switch (k)
                {
                    case 0:
                    {
                        name = "REC";
                        const char* a = "Record";
                        switch (t.state)
                        {
                            case TrackState::kArmed:
                            case TrackState::kWaiting: a = "Cancel"; break;
                            case TrackState::kRecording: a = "Stop"; fl = kKfRed | kKfLit; break;
                            case TrackState::kPlaying:
                            case TrackState::kStopped: a = "Overdub"; break;
                            case TrackState::kOverdub:
                            case TrackState::kReplace: a = "Play"; fl = kKfRed | kKfLit; break;
                            default: break;
                        }
                        snprintf(v, vn, "%s", a);
                        break;
                    }
                    case 1:
                        name = "PLAY";
                        if (t.len == 0)
                            snprintf(v, vn, "-");
                        else if (t.state == TrackState::kStopped)
                            snprintf(v, vn, "Play");
                        else
                        {
                            snprintf(v, vn, "Stop");
                            fl = kKfLit | kKfLine;
                        }
                        break;
                    case 2:
                        name = "MUTE";
                        snprintf(v, vn, "%s", t.muted ? "On" : "Off");
                        if (t.muted) fl = kKfLit;
                        break;
                    case 3:
                        name = "TEMPO";
                        if (engine.hasLoops() && engine.tempoPct() != 100.0f)
                            snprintf(v, vn, "%d bpm %d%%", (int)(engine.currentBpm() + 0.5f), (int)(engine.tempoPct() + 0.5f));
                        else
                            snprintf(v, vn, "%d bpm", (int)(engine.currentBpm() + 0.5f));
                        break;
                    case 4: name = "MODE"; snprintf(v, vn, "%s", modeName(t.mode)); break;
                    case 5: name = "INPUT"; snprintf(v, vn, "Line %d", engine.cfg.inputGain); break;
                    case 6: name = "VOLUME"; snprintf(v, vn, "%d", t.volume); break;
                    case 7:
                        name = "UNDO";
                        snprintf(v, vn, "%s", !t.undoAvail ? "-" : (t.undone ? "Redo" : "Undo"));
                        break;
                }
            }
            else
            {
                switch (k)
                {
                    case 0:
                        name = "REPLACE";
                        snprintf(v, vn, "%s", t.state == TrackState::kReplace ? "On" : "Off");
                        if (t.state == TrackState::kReplace) fl = kKfRed | kKfLit;
                        break;
                    case 1: name = "ALL"; snprintf(v, vn, "Play / stop"); break;
                    case 2: name = "SOLO"; snprintf(v, vn, "Track %d", sel + 1); break;
                    case 3:
                        name = "CLICK";
                        snprintf(v, vn, "%s %d", engine.cfg.metronome ? "On" : "Off", engine.cfg.clickLevel);
                        if (engine.cfg.metronome) fl = kKfLit;
                        break;
                    case 4:
                        name = "STOP";
                        if (t.fixedBars)
                            snprintf(v, vn, "%d bar%s", t.fixedBars, t.fixedBars > 1 ? "s" : "");
                        else
                            snprintf(v, vn, "%s", kStopNames[t.stopOpt]);
                        break;
                    case 5: name = "START"; snprintf(v, vn, "%s", kStartNames[t.startOpt]); break;
                    case 6: name = "AFTER REC"; snprintf(v, vn, "%s", kAfterNames[t.afterRec]); break;
                    case 7: name = "TEMPO"; snprintf(v, vn, "%s", kTempoNames[t.tempoOpt]); break;
                }
            }
            return;

        case kMixer:
            if (k < 4)
            {
                name = shift ? "MUTE" : "TRACK";
                snprintf(v, vn, "Track %d", k + 1);
                if (shift && engine.track[k].muted) fl = kKfLit | kKfRed;
            }
            else if (k < 6)
            {
                name = shift ? "MUTE" : (k == 4 ? "FX 1" : "FX 2");
                snprintf(v, vn, "%s", kFxDesc[engine.fx[k - 4].type()].name);
                if (shift && engine.fxMute[k - 4]) fl = kKfLit | kKfRed;
            }
            else if (k == 6)
            {
                name = shift ? "MUTE" : "MASTER";
                snprintf(v, vn, "Level");
                if (shift && engine.cfg.masterMute) fl = kKfLit | kKfRed;
            }
            else
            {
                name = "HOME";
                snprintf(v, vn, "Loops");
            }
            if (!shift && k == mixStrip) fl = kKfLit | kKfLine;
            return;

        case kFx:
        {
            FxSlot& f = engine.fx[fxSlot];
            const FxDesc& d = kFxDesc[f.type()];
            if (bar == kBarFxType)
            {
                if (k == 7)
                {
                    snprintf(v, vn, "%s", fxTypePage ? "Back" : "More");
                    return;
                }
                int ty = fxTypePage * 7 + k;
                if (ty < kFxTypes)
                {
                    name = "TYPE";
                    snprintf(v, vn, "%s", kFxDesc[ty].name);
                    if (ty == f.type()) fl = kKfLit | kKfLine;
                }
                return;
            }
            switch (k)
            {
                case 0: name = "TYPE"; snprintf(v, vn, "%s", d.name); break;
                case 1:
                case 2:
                case 3:
                case 4:
                {
                    int base = (fxKnob < d.knobs ? fxKnob / 4 : 0) * 4;
                    if (shift) base = (base + 4 < d.knobs) ? base + 4 : 0;
                    int kn = base + k - 1;
                    if (kn < d.knobs)
                    {
                        name = d.knobName[kn];
                        fxKnobText(f.type(), kn, f.knob[kn], v, vn);
                        if (kn == fxKnob) fl = kKfLit | kKfLine;
                    }
                    break;
                }
                case 5:
                    name = "RETURN";
                    snprintf(v, vn, "%d", engine.fxReturn[fxSlot]);
                    if (fxKnob == kFxReturnKnob) fl = kKfLit | kKfLine;
                    break;
                case 6:
                    name = fxSlot ? "FX 1" : "FX 2";
                    snprintf(v, vn, "%s", kFxDesc[engine.fx[fxSlot ^ 1].type()].name);
                    break;
                case 7: name = "HOME"; snprintf(v, vn, "Loops"); break;
            }
            return;
        }

        case kInput:
            if (k == 0) { name = "CARD"; snprintf(v, vn, "Input"); if (inCol == 0) fl = kKfLit | kKfLine; }
            if (k == 1) { name = "CARD"; snprintf(v, vn, "Tempo"); if (inCol == 1) fl = kKfLit | kKfLine; }
            if (k == 7) { name = "HOME"; snprintf(v, vn, "Loops"); }
            return;
        case kConfig:
            if (k == 7) { name = "HOME"; snprintf(v, vn, "Loops"); }
            return;
        case kFile:
            switch (k)
            {
                case 0: name = "SAVE"; snprintf(v, vn, "Project"); break;
                case 1: name = "LOAD"; snprintf(v, vn, "Selected"); break;
                case 2: name = "EXPORT"; snprintf(v, vn, "WAVs"); break;
                case 3: name = "NEW"; snprintf(v, vn, "Name"); break;
                case 7: name = "HOME"; snprintf(v, vn, "Loops"); break;
            }
            return;
        case kEditor:
        {
            uint32_t bar = (uint32_t)(engine.barLen() >> kFrac);
            uint32_t beat = (uint32_t)(engine.beatLen() >> kFrac);
            auto bbs = [&](uint32_t f, char* o, int n) {
                if (!bar || !beat) { snprintf(o, n, "%u", (unsigned)f); return; }
                uint32_t b = f / bar, r = f % bar, bt = r / beat, sx = (r % beat) * 4 / beat;
                snprintf(o, n, "%u.%u.%u", (unsigned)(b + 1), (unsigned)(bt + 1), (unsigned)(sx + 1));
            };
            switch (k)
            {
                case 0: name = "START"; bbs(edA, v, vn); if (edMarker == 0) fl = kKfLit | kKfLine; break;
                case 1: name = "END"; bbs(edB, v, vn); if (edMarker == 1) fl = kKfLit | kKfLine; break;
                case 2: name = "SNAP"; snprintf(v, vn, "%s", kSnapNames[edSnap]); break;
                case 3: name = "TRIM"; snprintf(v, vn, "Keep"); break;
                case 4: name = "CUT"; snprintf(v, vn, "%s", edConfirm == 4 ? "Sure?" : "Remove"); if (edConfirm == 4) fl = kKfRed | kKfLit; break;
                case 5: name = "ERASE"; snprintf(v, vn, "%s", edConfirm == 5 ? "Sure?" : "Silence"); if (edConfirm == 5) fl = kKfRed | kKfLit; break;
                case 6: name = "UNDO"; snprintf(v, vn, "Trim"); break;
                case 7: name = "HOME"; snprintf(v, vn, "Loops"); break;
            }
            return;
        }
        case kManual:
            if (k == 0) { name = "PAGE"; snprintf(v, vn, "Prev"); }
            if (k == 1) { name = "PAGE"; snprintf(v, vn, "Next"); }
            if (k == 7) { name = "HOME"; snprintf(v, vn, "Loops"); }
            return;
    }
}

static void paintKeys(int id, int x, int y, int w, int h)
{
    (void)id;
    (void)w;
    (void)h;
    cLooper& L = looper;
    rect(x, y, x + 800, y + 56, 0x000000);
    char v[24];
    for (int k = 0; k < 8; k++)
    {
        const char* name;
        uint8_t fl;
        L.keyText(k, name, v, sizeof(v), fl);
        int cx = x + k * 100 + 50;
        uint32_t nc = (fl & kKfRed) ? kRed : ((fl & kKfLit) ? (L.page == cLooper::kHome && !L.perform ? kWhite : kYellow) : kGrey);
        if (name[0]) text(cx, y + 19, 0, OPT_CENTER, name, nc);
        if (v[0]) text(cx, y + 37, 0, OPT_CENTER, v, (fl & kKfLit) ? kYellow : kWhite);
        if (fl & kKfLine) rect(cx - 36, y + 51, cx + 36, y + 53, (fl & kKfRed) ? kRed : 0xB0B0B0);
    }
    // the volume key shows the level as a thin bar (home)
    if (L.page == cLooper::kHome && !L.shift && !L.perform && L.bar == cLooper::kBarMain)
    {
        const Track& t = engine.track[L.sel];
        int cx = x + 6 * 100 + 50;
        rect(cx - 36, y + 51, cx + 36, y + 53, 0x303030);
        rect(cx - 36, y + 51, cx - 36 + t.volume * 72 / 100, y + 53, 0x9A9A9A);
    }
}

//------------------------------------------------------------------------------
// overlay: knobs, messages

static void bigKnob(const char* title, const char* value, float frac)
{
    int x0 = 250, y0 = 130, x1 = 550, y1 = 290;
    API_COLOR_A(235);
    rect(x0, y0, x1, y1, 0x101010);
    API_COLOR_A(255);
    API_LINE_WIDTH(16);
    API_COLOR(0x5A5A5A);
    API_BEGIN(LINE_STRIP);
    API_VERTEX2F(x0, y0);
    API_VERTEX2F(x1, y0);
    API_VERTEX2F(x1, y1);
    API_VERTEX2F(x0, y1);
    API_VERTEX2F(x0, y0);
    API_END();
    text(400, y0 + 26, 0, OPT_CENTER, title, kGrey);
    text(400, y0 + 70, 3, OPT_CENTER, value, kWhite);
    // arc of dots
    frac = clampf(frac, 0, 1);
    API_POINT_SIZE(16 * 3);
    API_BEGIN(FTPOINTS);
    for (int i = 0; i <= 24; i++)
    {
        float a = (float)i / 24;
        float ang = kPi * (1.25f - 1.5f * a);
        int px = 400 + (int)(cosf(ang) * 46);
        int py = y0 + 118 - (int)(sinf(ang) * 30);
        API_COLOR(a <= frac ? kYellow : 0x333333);
        API_VERTEX2F(px, py);
    }
    API_END();
}

static void paintOverlay(int id, int x, int y, int w, int h)
{
    (void)id;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    cLooper& L = looper;
    char v[32];
    const Track& t = engine.track[L.sel];
    switch (L.overlay)
    {
        case cLooper::kOvTempo:
            if (engine.hasLoops())
            {
                snprintf(v, sizeof(v), "%.1f bpm  %d %%", engine.currentBpm(), (int)(engine.tempoPct() + 0.5f));
                bigKnob("TEMPO (varispeed)", v, (engine.tempoPct() - 50) / 150.0f);
            }
            else
            {
                snprintf(v, sizeof(v), "%d bpm", (int)(engine.currentBpm() + 0.5f));
                bigKnob("TEMPO", v, (engine.currentBpm() - 40) / 200.0f);
            }
            break;
        case cLooper::kOvVolume:
            snprintf(v, sizeof(v), "%d", t.volume);
            bigKnob("VOLUME", v, t.volume / 100.0f);
            break;
        case cLooper::kOvGain:
            snprintf(v, sizeof(v), "%d", engine.cfg.inputGain);
            bigKnob("INPUT GAIN", v, engine.cfg.inputGain / 100.0f);
            break;
        case cLooper::kOvThreshold:
            snprintf(v, sizeof(v), "%d dB", engine.cfg.thresholdDb);
            bigKnob("THRESHOLD", v, (engine.cfg.thresholdDb + 60) / 60.0f);
            break;
        case cLooper::kOvLength:
            if (t.fixedBars)
                snprintf(v, sizeof(v), "%d bar%s", t.fixedBars, t.fixedBars > 1 ? "s" : "");
            else
                snprintf(v, sizeof(v), "Off");
            bigKnob("LENGTH", v, t.fixedBars / 16.0f);
            break;
        case cLooper::kOvPerf:
            if (L.perfKnobFx >= 0)
            {
                perfKnobText(L.perfKnobFx, t.perfKnob[L.perfKnobFx], v, sizeof(v));
                bigKnob(perfName(L.perfKnobFx), v, t.perfKnob[L.perfKnobFx] / (L.perfKnobFx < 2 ? 5.0f : (L.perfKnobFx >= 4 && L.perfKnobFx <= 5 ? 4.0f : (L.perfKnobFx == 7 ? 3.0f : 100.0f))));
            }
            break;
        default: break;
    }
    if (L.messageUntil)
    {
        API_COLOR_A(235);
        rect(200, 196, 600, 236, 0x181818);
        API_COLOR_A(255);
        text(400, 216, 0, OPT_CENTER, L.message, kWhite);
    }
    if (L.showStats)
    {
        snprintf(v, sizeof(v), "blocks %lu", (unsigned long)engine.blockCount());
        text(796, 2, 4, OPT_RIGHTX, v, 0x60A060);
    }
}

//------------------------------------------------------------------------------
// pages (drawn on the background control)

static void meterBar(int x, int y0, int y1, float lvl)
{
    rect(x, y0, x + 3, y1, 0x262626);
    int h = (int)(clampf(lvl, 0, 1) * (y1 - y0));
    rect(x, y1 - h, x + 3, y1, lvl > 0.95f ? kRed : 0xA0A0A0);
}

static void smallBar(int cx, int y, float frac, bool sel, bool bipolar)
{
    rect(cx - 33, y, cx + 33, y + 3, 0x2A2A2A);
    if (bipolar)
    {
        int m = cx, e = cx + (int)(frac * 33);
        rect(m < e ? m : e, y, m < e ? e : m, y + 3, sel ? kYellow : 0x8A8A8A);
        rect(cx - 1, y - 1, cx + 1, y + 4, 0x6A6A6A);
    }
    else
        rect(cx - 33, y, cx - 33 + (int)(clampf(frac, 0, 1) * 66), y + 3, sel ? kYellow : 0x8A8A8A);
}

static void paintMixer()
{
    cLooper& L = looper;
    char v[24];
    for (int s = 0; s < 7; s++)
    {
        int cx = s * 100 + 50;
        bool selS = s == L.mixStrip;
        const char* title;
        bool muted;
        if (s < 4)
        {
            snprintf(v, sizeof(v), "Track %d", s + 1);
            title = v;
            muted = engine.track[s].muted;
        }
        else if (s < 6)
        {
            title = s == 4 ? "FX 1" : "FX 2";
            muted = engine.fxMute[s - 4];
        }
        else
        {
            title = "Master";
            muted = engine.cfg.masterMute;
        }
        text(cx, 22, 0, OPT_CENTER, muted ? "Muted" : title, muted ? kRed : (selS ? kYellow : kGrey));

        for (int r = 0; r < 3; r++)
        {
            int y = 50 + r * 58;
            bool selC = selS && L.mixRow == r;
            const char* nm = nullptr;
            float frac = 0;
            bool bip = false;
            v[0] = 0;
            if (s < 4)
            {
                const Track& t = engine.track[s];
                if (r < 2)
                {
                    static char nbuf[2][20];
                    snprintf(nbuf[r], sizeof(nbuf[r]), "To %s", kFxDesc[engine.fx[r].type()].name);
                    nm = nbuf[r];
                    snprintf(v, sizeof(v), "%d", t.send[r]);
                    frac = t.send[r] / 100.0f;
                }
                else
                {
                    nm = "Filter";
                    if (t.djFilter == 0)
                        snprintf(v, sizeof(v), "Off");
                    else
                        snprintf(v, sizeof(v), "%s %d", t.djFilter < 0 ? "LP" : "HP", t.djFilter < 0 ? -t.djFilter : t.djFilter);
                    frac = t.djFilter / 50.0f;
                    bip = true;
                }
            }
            else if (s < 6)
            {
                int sl = s - 4;
                const FxSlot& f = engine.fx[sl];
                if (r == 0)
                {
                    nm = kFxDesc[f.type()].knobName[0];
                    fxKnobText(f.type(), 0, f.knob[0], v, sizeof(v));
                    frac = f.knob[0] / 100.0f;
                }
                else if (r == 1)
                {
                    static char cbuf[2][20];
                    snprintf(cbuf[sl], sizeof(cbuf[sl]), "To %s", kFxDesc[engine.fx[sl ^ 1].type()].name);
                    nm = cbuf[sl];
                    bool other = sl == 1 && engine.chainSend[0] > 0 && engine.chainSend[1] > 0;
                    if (other)
                        snprintf(v, sizeof(v), "off");
                    else
                        snprintf(v, sizeof(v), "%d", engine.chainSend[sl]);
                    frac = engine.chainSend[sl] / 100.0f;
                }
            }
            if (!nm) continue;
            text(cx, y, 0, OPT_CENTER, nm, selC ? kYellow : kGrey);
            text(cx, y + 18, 0, OPT_CENTER, v, selC ? kYellow : kWhite);
            smallBar(cx, y + 32, frac, selC, bip);
        }

        // fader
        int lvl;
        float meter;
        const char* fname = "Level";
        if (s < 4)
        {
            lvl = engine.track[s].volume;
            meter = engine.track[s].meter;
        }
        else if (s < 6)
        {
            lvl = engine.fxReturn[s - 4];
            meter = engine.returnMeter(s - 4);
            fname = "Return";
        }
        else
        {
            lvl = engine.cfg.master;
            meter = engine.masterMeter(0);
        }
        bool selF = selS && L.mixRow == 3;
        text(cx, 234, 0, OPT_CENTER, fname, selF ? kYellow : kGrey);
        int ft = 254, fb = 392;
        rect(cx - 2, ft, cx + 2, fb, 0x3A3A3A);
        int ky = fb - lvl * (fb - ft) / 100;
        rect(cx - 2, ky, cx + 2, fb, selF ? kYellow : 0x6A6A6A);
        rect(cx - 14, ky - 4, cx + 14, ky + 4, selF ? kYellow : 0xB0B0B0);
        if (s == 6)
        {
            meterBar(cx - 30, ft, fb, engine.masterMeter(0));
            meterBar(cx + 28, ft, fb, engine.masterMeter(1));
        }
        else
            meterBar(cx + 28, ft, fb, meter);
        snprintf(v, sizeof(v), "%d", lvl);
        text(cx, 408, 0, OPT_CENTER, v, selF ? kYellow : kGrey);
    }
}

static void paintFx()
{
    cLooper& L = looper;
    const FxSlot& f = engine.fx[L.fxSlot];
    char v[96];
    snprintf(v, sizeof(v), "FX %d  %s", L.fxSlot + 1, kFxDesc[f.type()].name);
    text(20, 14, 2, 0, v, kWhite);
    int x0 = 40, x1 = 760, y0 = 60, y1 = 330;
    rect(x0, y1, x1, y1 + 1, 0x303030);
    float barF = (float)(engine.barLen() >> kFrac) / (engine.tempoPct() * 0.01f);
    API_LINE_WIDTH(16);
    switch (f.type())
    {
        case kFxDelay:
        case kFxPingPong:
        case kFxTape:
        case kFxPrime:
        case kFxReverse:
        {
            // the echoes on the beat grid: one bar wide
            int beats = engine.cfg.beatsPerBar;
            for (int b = 0; b <= beats * 2; b++)
            {
                int bx = x0 + (x1 - x0) * b / (beats * 2);
                vline(bx, y0, y1, b % beats == 0 ? kBarLine : kBeatLine);
            }
            float d = kDelayTimeBars[delayTimeIndex(f.knob[0])] * barF;
            float fb = f.knob[1] * 0.0098f;
            float amp = 1.0f;
            for (int e = 1; e < 24; e++)
            {
                float tt = e * d;
                if (f.type() == kFxPrime && (e & 1) == 0) tt = (e / 2) * d * 1.5f;
                int ex = x0 + (int)(tt / (barF * 2) * (x1 - x0));
                if (ex > x1) break;
                int eh = (int)(amp * (y1 - y0 - 20));
                bool right = f.type() == kFxPingPong && (e & 1) == 0;
                rect(ex - 3, y1 - eh, ex + 3, y1, right ? 0x8AA0C8 : kYellow);
                amp *= (f.type() == kFxPrime) ? sqrtf(fb) : fb;
                if (amp < 0.02f) break;
            }
            break;
        }
        case kFxReverb:
        case kFxShimmer:
        {
            float T = 0.3f * powf(30.0f, f.knob[0] * 0.01f);
            API_COLOR(kYellow);
            API_BEGIN(LINE_STRIP);
            for (int i = 0; i <= 60; i++)
            {
                float tt = i / 60.0f * 10.0f;
                float a = powf(10.0f, -3.0f * tt / T);
                API_VERTEX2F(x0 + (x1 - x0) * i / 60, y1 - (int)(a * (y1 - y0)));
            }
            API_END();
            snprintf(v, sizeof(v), "decay %.1f s", T);
            text(x1, y0 - 20, 0, OPT_RIGHTX, v, kGrey);
            break;
        }
        case kFxChorus:
        case kFxFlanger:
        case kFxPhaser:
        {
            API_COLOR(0x6A6A6A);
            API_BEGIN(LINE_STRIP);
            for (int i = 0; i <= 64; i++)
            {
                float ph = i / 64.0f * 2;
                API_VERTEX2F(x0 + (x1 - x0) * i / 64, (y0 + y1) / 2 - (int)(sin01(ph) * (y1 - y0) * 0.4f * f.knob[1] / 100.0f));
            }
            API_END();
            float ph = f.lfo;
            int px = x0 + (int)(ph / 2 * (x1 - x0));
            int py = (y0 + y1) / 2 - (int)(sin01(ph) * (y1 - y0) * 0.4f * f.knob[1] / 100.0f);
            API_POINT_SIZE(16 * 7);
            API_COLOR(kYellow);
            API_BEGIN(FTPOINTS);
            API_VERTEX2F(px, py);
            API_VERTEX2F(px + (x1 - x0) / 2, (y0 + y1) / 2 - (int)(sin01(ph + 1) * (y1 - y0) * 0.4f * f.knob[1] / 100.0f));
            API_END();
            break;
        }
        case kFxClouds:
        {
            static const char* const modes[4] = {"Granular", "Stretch", "Looping delay", "Spectral"};
            static const char* const q[3] = {"Stereo 16-bit, 1.5 s", "Stereo 8-bit, 3 s", "Mono 16-bit, 2 s"};
            int cy = (y0 + y1) / 2;
            rect(x0, cy - 30, x1, cy + 30, f.frozen() ? 0x30405A : 0x202020);
            snprintf(v, sizeof(v), "%s   %s", modes[f.knob[10] & 3], q[f.knob[11] % 3]);
            text(x0, y0 - 20, 0, 0, v, kGrey);
            text(x1, y0 - 20, 0, OPT_RIGHTX, f.frozen() ? "Frozen" : "Recording", f.frozen() ? 0x8AA0C8 : kGrey);
            // where the grains read: Position back from now, Size wide
            int px = x1 - (int)(f.knob[0] * 0.01f * (x1 - x0) * 0.8f);
            int w = 4 + (int)(f.knob[1] * 0.01f * (x1 - x0) * 0.15f);
            API_COLOR_A(150);
            rect(px - w, cy - 26, px, cy + 26, kYellow);
            API_COLOR_A(255);
            text(x0, y1 + 16, 4, 0, "Mutable Instruments Clouds (MIT). Blend 100 = all wet, as a send.", kDim);
            break;
        }
        case kFxGrains:
        {
            // the record buffer with its grains
            rect(x0, (y0 + y1) / 2 - 30, x1, (y0 + y1) / 2 + 30, f.frozen() ? 0x30405A : 0x202020);
            text(x0, y0 - 20, 0, 0, f.frozen() ? "Frozen" : "Recording", f.frozen() ? 0x8AA0C8 : kGrey);
            API_POINT_SIZE(16 * 5);
            API_COLOR(kYellow);
            API_BEGIN(FTPOINTS);
            for (int g = 0; g < f.visCount; g++)
                API_VERTEX2F(x0 + (int)(f.vis[g] * (x1 - x0)), (y0 + y1) / 2 + ((g * 37) % 50) - 25);
            API_END();
            break;
        }
        case kFxLofi:
        {
            int bits = 16 - f.knob[0] * 14 / 100;
            int steps = 1 << (bits > 6 ? 6 : bits);
            int hold = 1 + f.knob[1] * 31 / 100;
            API_COLOR(kYellow);
            API_BEGIN(LINE_STRIP);
            int n = 96;
            float last = 0;
            for (int i = 0; i <= n; i++)
            {
                if (i % (1 + hold / 4) == 0) last = floorf(sinf(kTwoPi * i / n * 2) * steps / 2) / (steps / 2);
                API_VERTEX2F(x0 + (x1 - x0) * i / n, (y0 + y1) / 2 - (int)(last * (y1 - y0) * 0.4f));
            }
            API_END();
            break;
        }
    }
    // knobs as bars over their keys
    const FxDesc& d = kFxDesc[f.type()];
    int base = (L.fxKnob < d.knobs ? L.fxKnob / 4 : 0) * 4;
    if (L.shift) base = (base + 4 < d.knobs) ? base + 4 : 0;
    for (int k = 0; k < 4; k++)
    {
        int kn = base + k;
        if (kn >= d.knobs) continue;
        int cx = (k + 1) * 100 + 50;
        smallBar(cx, 400, f.knob[kn] / (float)FxSlot::knobMax(f.type(), kn), kn == L.fxKnob, false);
    }
    smallBar(550, 400, engine.fxReturn[L.fxSlot] / 100.0f, L.fxKnob == kFxReturnKnob, false);
    if (d.knobs > 4)
    {
        snprintf(v, sizeof(v), "Knobs %d-%d of %d   Left / Right: all knobs   Shift: next page", base + 1,
                 base + 4 < d.knobs ? base + 4 : d.knobs, d.knobs);
        text(20, 380, 4, 0, v, kDim);
    }
}

static void listRow(int x, int y, int w, const char* name, const char* value, bool selected)
{
    if (selected) rect(x - 6, y - 11, x + w + 6, y + 13, 0x26240F);
    text(x, y, 0, OPT_CENTERY, name, selected ? kYellow : kGrey);
    text(x + w, y, 0, OPT_CENTERY | OPT_RIGHTX, value, selected ? kYellow : kWhite);
}

static void paintInput()
{
    cLooper& L = looper;
    const Config& c = engine.cfg;
    const Track& t = engine.track[L.sel];
    char v[32];
    text(40, 24, 2, 0, "Input", L.inCol == 0 ? kYellow : kGrey);
    text(430, 24, 2, 0, "Tempo", L.inCol == 1 ? kYellow : kGrey);
    int y = 70;
    snprintf(v, sizeof(v), "%d", c.inputGain);
    listRow(40, y, 320, "Gain", v, L.inCol == 0 && L.inRow == 0);
    snprintf(v, sizeof(v), "%s  (track %d)", kChannelNames[t.channels], L.sel + 1);
    listRow(40, y + 34, 320, "Channels", v, L.inCol == 0 && L.inRow == 1);
    listRow(40, y + 68, 320, "Monitoring", c.monitor ? "On (through the sends)" : "Off", L.inCol == 0 && L.inRow == 2);
    snprintf(v, sizeof(v), "%d %%", c.feedback);
    listRow(40, y + 102, 320, "Overdub feedback", v, L.inCol == 0 && L.inRow == 3);
    // level against the threshold
    float lvl = engine.inputLevel();
    rect(40, y + 150, 360, y + 158, 0x202020);
    rect(40, y + 150, 40 + (int)(clampf(lvl, 0, 1) * 320), y + 158, lvl > 0.95f ? kRed : 0x9A9A9A);
    int tx = 40 + (int)(dbToGain((float)c.thresholdDb) * 320);
    rect(tx, y + 146, tx + 2, y + 162, kRed);
    text(40, y + 176, 4, 0, "Input level (red: the on-sound threshold)", kDim);

    const char* rows[10] = {"Tempo", "Beats / bar", "Metronome", "Click level", "Click sound",
                            "Accent", "Click", "Count-in", "Threshold", "Stop fade"};
    for (int r = 0; r < 10; r++)
    {
        switch (r)
        {
            case 0:
                if (engine.hasLoops())
                    snprintf(v, sizeof(v), "%.1f bpm (%d %%)", engine.currentBpm(), (int)(engine.tempoPct() + 0.5f));
                else
                    snprintf(v, sizeof(v), "%.1f bpm", engine.currentBpm());
                break;
            case 1: snprintf(v, sizeof(v), "%d", c.beatsPerBar); break;
            case 2: snprintf(v, sizeof(v), "%s", c.metronome ? "On" : "Off"); break;
            case 3: snprintf(v, sizeof(v), "%d", c.clickLevel); break;
            case 4: snprintf(v, sizeof(v), "%s", kClickNames[c.clickSound]); break;
            case 5: snprintf(v, sizeof(v), "%s", c.accent ? "On" : "Off"); break;
            case 6: snprintf(v, sizeof(v), "%s", c.clickOnlyRec ? "While recording" : "Always"); break;
            case 7:
                if (c.countInBars)
                    snprintf(v, sizeof(v), "%d bar%s", c.countInBars, c.countInBars > 1 ? "s" : "");
                else
                    snprintf(v, sizeof(v), "Off");
                break;
            case 8: snprintf(v, sizeof(v), "%d dB", c.thresholdDb); break;
            case 9: snprintf(v, sizeof(v), "%s", kFadeNames[c.stopFade]); break;
        }
        listRow(430, 70 + r * 34, 330, rows[r], v, L.inCol == 1 && L.inRow == r);
    }
}

static void paintConfig()
{
    cLooper& L = looper;
    char v[96];
    text(40, 24, 2, 0, "Settings", kWhite);
    snprintf(v, sizeof(v), "%.1f ms", engine.cfg.latency * 1000.0f / kSampleRate);
    listRow(40, 70, 500, "Recording latency", v, L.cfgRow == 0);
    listRow(40, 104, 500, "Show the stats", L.showStats ? "On" : "Off", L.cfgRow == 1);
    const Track& t = engine.track[0];
    float st = (float)(t.memSamples / 2) / kSampleRate;
    snprintf(v, sizeof(v), "Recording time per track: %.1f s stereo, %.1f s mono", st, st * 2);
    text(40, 170, 0, 0, v, kGrey);
    text(40, 200, 0, 0, "Undo keeps loops up to half of that.", kDim);
    text(40, 240, 0, 0, "Recording latency moves takes and overdubs back by the codec's", kDim);
    text(40, 262, 0, 0, "round trip, so layers land where they were played.", kDim);
}

static void paintFile()
{
    cLooper& L = looper;
    char v[200];
    snprintf(v, sizeof(v), "Project: %s", L.projectName);
    text(40, 24, 2, 0, v, kWhite);
    text(40, 52, 4, 0, "Projects are saved in /Looper/Projects (every loop as a WAV plus the settings).", kDim);
    if (L.projectCount == 0) text(40, 100, 0, 0, "No saved projects yet.", kGrey);
    for (int i = 0; i < L.projectCount && i < 9; i++)
    {
        int idx = (L.fileRow >= 9) ? L.fileRow - 8 + i : i;
        if (idx >= L.projectCount) break;
        listRow(40, 100 + i * 32, 400, L.projectList[idx], idx == L.fileRow ? "<" : "", idx == L.fileRow);
    }
    text(480, 100, 0, 0, "Load WAVs into tracks:", kGrey);
    text(480, 124, 4, 0, "make a folder in /Looper/Projects and put", kDim);
    text(480, 142, 4, 0, "track1.wav ... track4.wav in it (16-bit,", kDim);
    text(480, 160, 4, 0, "44.1 kHz, mono or stereo); load it with F2.", kDim);
}

static void paintEditor()
{
    cLooper& L = looper;
    const Track& t = engine.track[L.edTrack];
    char v[96];
    snprintf(v, sizeof(v), "Track %d", L.edTrack + 1);
    text(20, 14, 2, 0, v, kWhite);
    if (t.len == 0)
    {
        text(400, 200, 1, OPT_CENTER, "Empty", kGrey);
        return;
    }
    snprintf(v, sizeof(v), "%.2f s   selection %.2f s", t.len / kSampleRate, (L.edB - L.edA) / kSampleRate);
    text(780, 14, 0, OPT_RIGHTX, v, kGrey);
    int x0 = kWaveX;
    barLines(x0, kWaveW, kEdY, kEdY + kEdH, t.len, t.anchor, true);
    if (L.edVersion == t.version)
    {
        drawBitmap(kEdRamG, x0, kEdY, kWaveW, kEdH, 0x5A5A5A);
        int ax = x0 + (int)((uint64_t)L.edA * kWaveW / t.len);
        int bx = x0 + (int)((uint64_t)L.edB * kWaveW / t.len);
        API_SAVE_CONTEXT();
        API_SCISSOR_XY(ax, kEdY);
        API_SCISSOR_SIZE(bx - ax > 0 ? bx - ax : 1, kEdH);
        drawBitmap(kEdRamG, x0, kEdY, kWaveW, kEdH, kYellow);
        API_RESTORE_CONTEXT();
        rect(ax - 1, kEdY - 8, ax + 1, kEdY + kEdH + 8, L.edMarker == 0 ? kWhite : 0x8A8A8A);
        rect(bx - 1, kEdY - 8, bx + 1, kEdY + kEdH + 8, L.edMarker == 1 ? kWhite : 0x8A8A8A);
    }
    // the playhead
    if (engine.running() && t.state != TrackState::kStopped)
    {
        int px = x0 + (int)(engine.playhead(L.edTrack) * kWaveW / t.len);
        rect(px - 1, kEdY, px + 1, kEdY + kEdH, 0xFFFFFF);
    }
    snprintf(v, sizeof(v), "Clipboard %.2f s   Copy: copy   Shift+Copy: paste   Insert: insert", engine.clipLen() / kSampleRate);
    text(20, 392, 4, 0, v, kDim);
}

static const char* const kManual[8][9] = {
    {"Looper", "Four stereo tracks on one bar clock. The first take sets the bar;",
     "every later take, mode and effect follows it, so nothing drifts.", "",
     "Pads: one row per track. Column 1 record / overdub (Shift: replace),",
     "2 stop / play, 3 mute, 4 clear, 5-12 jump to that eighth of the loop.",
     "Play: transport (Shift: metronome). Rec: the selected track.", "Insert: undo (Shift: redo). Delete: clear (Shift: all).",
     "Copy: duplicate the track into the next one."},
    {"The screen keys", "REC, PLAY, MUTE act on the selected track (Up / Down).",
     "Hold TEMPO and turn: the bar before a loop, varispeed after it.", "Tap MODE: the mode bar. On a Granular track hold it: the grains.",
     "Tap INPUT: the input page; hold and turn: input gain.", "Tap VOLUME: the mixer strip; hold and turn: volume.",
     "UNDO takes back the last overdub, again brings it back.", "Shift: replace, play / stop all, solo, click, and the track's",
     "Stop, Start, After rec and Tempo options."},
    {"Playback modes", "Forward, Reverse, Ping-pong: these can overdub.",
     "Half speed, Double, Half rev, Double rev: tape style.", "Half time: half tempo, same pitch.",
     "Octave up / down: same tempo, other pitch.", "Granular: grains around the playhead. Spread, Size, Density,",
     "Pitch, Speed (1/2, 1, 2x), Direction and Texture (Clouds' grains;", "Density: below the middle even, above random, middle silent).",
     "Every mode reads from the bar clock: a track stays in time."},
    {"Performance mode", "Instr (or Perform, Fn2) toggles it; hold it to use it while held.",
     "Pad columns 5-12: repeat, repeat, low-pass, high-pass,", "sweep down, sweep up, vibrato, tape stop. Hold to play,",
     "Shift + pad to latch. Hold a pad and turn: that track's knob.", "The screen keys play an effect on every track at once;",
     "hold one and turn for its knob. Column 4: freeze Grains/Clouds", "(row 1 FX 1, row 2 FX 2).", ""},
    {"Mixer and effects", "Master (Fn4): seven strips. Sends to both effects,",
     "a DJ filter (down: low-pass, up: high-pass) and the level.", "FX strips: the first knob, the send into the other slot",
     "(chain), the return. Shift + key: mute. Again: its page.", "Fx / Vol keys: the FX 1 / FX 2 page. TYPE picks one of",
     "13: Delay, Ping-pong, Reverb, Chorus, Flanger, Grains, Phaser,", "Shimmer, Tape echo, Reverse, Prime time, Lo-fi and Clouds",
     "(Mutable Instruments'). Left / Right: every knob; encoder turns it."},
    {"Loop editor", "Note: the selected track with start and end markers.",
     "START / END pick a marker, SNAP its steps (bar, beat, 1/16, free).", "TRIM keeps the selection (instant, UNDO puts it back).",
     "CUT removes it and closes up, ERASE silences it (press twice).", "Copy copies the selection, Shift + Copy pastes it at the start",
     "marker, Insert inserts it there.", "", ""},
    {"Track options (Shift layer)", "Stop: free, on the bar or on the beat; hold and turn: a fixed",
     "length in bars. Start: now, count-in or on sound (hold and turn:", "the threshold). After rec: play, overdub or stop.",
     "Tempo: Tape (pitch follows) or Stretch (keeps its pitch).", "Input page: channels per track, monitoring, overdub feedback,",
     "and the tempo card: metronome, click, count-in, stop fade.", "Encoder push: tap tempo.", ""},
    {"Leaving", "Menu opens the Mini's module menu: the tracker carries on",
     "and the loops keep playing under it.", "Projects: File (Shift + Fn3). Settings: Config (Shift + Fn4).", "This manual: Song (Shift + Fn1). Input: Sample Play (Fn3).",
     "Pattern (Fn1): back to the loops. (Fn keys as bound in Config.)", "", "", ""},
};

static void paintManual()
{
    cLooper& L = looper;
    int p = clampi(L.manualPage, 0, 7);
    char v[16];
    snprintf(v, sizeof(v), "%d / 8", p + 1);
    text(760, 24, 0, OPT_RIGHTX, v, kDim);
    text(40, 24, 3, 0, kManual[p][0], kWhite);
    for (int i = 1; i < 9; i++)
        if (kManual[p][i][0]) text(40, 60 + i * 34, 1, 0, kManual[p][i], kGrey);
}

static void paintBg(int id, int x, int y, int w, int h)
{
    (void)id;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    rect(0, 0, 800, 480, 0x000000);
    switch (looper.page)
    {
        case cLooper::kMixer: paintMixer(); break;
        case cLooper::kFx: paintFx(); break;
        case cLooper::kInput: paintInput(); break;
        case cLooper::kConfig: paintConfig(); break;
        case cLooper::kFile: paintFile(); break;
        case cLooper::kEditor: paintEditor(); break;
        case cLooper::kManual: paintManual(); break;
        default: break;
    }
}

//------------------------------------------------------------------------------

void cLooper::initDisplayControls()
{
    strControlProperties prop;

    // created first = drawn last (on top)
    overlayPainter = {paintOverlay, 0};
    prop.x = 0;
    prop.y = 0;
    prop.w = 800;
    prop.h = 480;
    prop.style = 0;
    prop.data = &overlayPainter;
    overlayCtl = display.createControl<cLooperView>(&prop);

    keysPainter = {paintKeys, 0};
    prop.y = kKeysY;
    prop.h = 480 - kKeysY;
    prop.data = &keysPainter;
    keysCtl = display.createControl<cLooperView>(&prop);

    for (int t = 0; t < kTracks; t++)
    {
        lanePainter[t] = {paintLaneCb, t};
        prop.y = t * kLaneH;
        prop.h = kLaneH;
        prop.data = &lanePainter[t];
        laneCtl[t] = display.createControl<cLooperView>(&prop);
    }

    bgPainter = {paintBg, 0};
    prop.y = 0;
    prop.h = 480;
    prop.data = &bgPainter;
    bgCtl = display.createControl<cLooperView>(&prop);
}

void cLooper::destroyDisplayControls()
{
    display.destroyControl(overlayCtl);
    display.destroyControl(keysCtl);
    for (int t = 0; t < kTracks; t++)
    {
        display.destroyControl(laneCtl[t]);
        laneCtl[t] = -1;
    }
    display.destroyControl(bgCtl);
    overlayCtl = keysCtl = bgCtl = -1;
}

#endif
