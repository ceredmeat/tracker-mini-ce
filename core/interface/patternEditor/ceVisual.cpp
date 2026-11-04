// tracker-mini-ce: Visual (see ceVisual.h). Our own code, written for this fork after the
// user's earlier stock-firmware Visual (sv1-sv4).
//
// The sequencer is read (not changed): each time a track moves to a new step with a note,
// that note becomes an event (track, pitch, velocity). Each style turns events into a few
// shapes; one frame every 30 ms, at most ~1500 display-list words.

#include "ceVisual.h"

#include <math.h>
#include <string.h>

#include <new>

#include "FT812.h"
#include "display.h"
#include "elapsedMillis.h"
#include "mtSequencer.h"
#include "mtStructs.h"
#include "patternEditor/patternEditor.h"

extern Sequencer sequencer;

namespace ceVis
{
namespace
{
constexpr int kTracks = kSampleTracks; // 8
constexpr int kMaxP = 64;
constexpr float kPi = 3.14159265f;

const uint32_t kTrackColor[8] = { 0xFF5A5A, 0xFFA53C, 0xF5E050, 0x6EE06E,
                                  0x46D2D2, 0x5A8CFF, 0xB46EFF, 0xFF6EC8 };
constexpr uint32_t kMuted = 0x505050;

bool on = false;
uint8_t styleIdx = 1;
elapsedMillis frameTimer;
uint32_t frame = 0;

int16_t lastPos[kTracks];
bool wasPlaying = false;
uint32_t rng = 0x12345678u;

struct Particle
{
    float x, y, vx, vy, size, grow, life, decay;
    uint8_t track;
    bool alive;
};
// the particles live on the heap only while Visual is on (2.3 KB of on-chip RAM otherwise)
Particle* p = nullptr;
float level[kTracks];   // Bars, Wave, Orbit flash
float orbitAngle[kTracks];
float energy = 0.0f;
int curX = 0, curY = 28, curW = 800, curH = 395; // the canvas (spawning uses its size)

float rnd01()
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (rng & 0xFFFFFF) / 16777216.0f;
}

bool muted(int t)
{
    return mtProject.values.trackMute[t] != 0;
}

uint32_t trackColor(int t, bool bright = false)
{
    if (muted(t)) return kMuted;
    uint32_t c = kTrackColor[t & 7];
    if (!bright) return c;
    // the cursor track: towards white
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r = (r + 255) / 2;
    g = (g + 255) / 2;
    b = (b + 255) / 2;
    return (r << 16) | (g << 8) | b;
}

int cursorTrack()
{
    return patternEditor.trackerPattern.actualTrack;
}

Particle* spawn()
{
    static Particle dummy;
    if (p == nullptr) return &dummy;
    for (int i = 0; i < kMaxP; i++)
        if (!p[i].alive) return &p[i];
    // reuse the oldest-looking one
    int best = 0;
    for (int i = 1; i < kMaxP; i++)
        if (p[i].life < p[best].life) best = i;
    return &p[best];
}

void addEvent(int t, int note, int vel)
{
    float v = vel / 127.0f;
    level[t] = fmaxf(level[t], 0.35f + 0.65f * v);
    energy = fminf(1.5f, energy + 0.25f * v);
    float pc = (note % 12) / 12.0f;
    float oct = (note / 12) / 10.0f;

    Particle* q;
    switch (styleIdx)
    {
        case 1: // Rain: a drop from the top, column by track, nudged by pitch
            q = spawn();
            *q = { curW * (t + 0.5f) / kTracks + (pc - 0.5f) * curW / kTracks * 0.8f, 0.0f,
                   0.0f, 5.0f + 6.0f * v, 4.0f + 4.0f * v, 0.0f, 1.0f, 0.012f, (uint8_t)t, true };
            break;
        case 2: // Ripples: a ring at (pitch, octave)
            q = spawn();
            *q = { curW * (0.08f + 0.84f * pc), curH * (0.9f - 0.8f * oct), 0, 0,
                   6.0f, 3.0f + 3.0f * v, 1.0f, 0.02f, (uint8_t)t, true };
            break;
        case 4: // Scroll: a mark at the right edge, height by pitch
            q = spawn();
            *q = { (float)curW, curH * (1.0f - (note - 24) / 84.0f), -4.0f, 0, 6.0f + 6.0f * v,
                   0, 1.0f, 0.004f, (uint8_t)t, true };
            break;
        case 5: // Burst: sparks from the middle
            for (int k = 0; k < 5; k++)
            {
                q = spawn();
                float a = rnd01() * 2.0f * kPi;
                float sp = (4.0f + 10.0f * rnd01()) * (0.6f + v);
                *q = { curW * 0.5f, curH * 0.5f, cosf(a) * sp, sinf(a) * sp, 10.0f + 10.0f * v,
                       -0.15f, 1.0f, 0.025f, (uint8_t)t, true };
            }
            break;
        case 7: // Tunnel: a square growing out of the middle
            q = spawn();
            *q = { curW * 0.5f, curH * 0.5f, 0, 0, 8.0f, 1.07f + 0.03f * v, 1.0f, 0.0f,
                   (uint8_t)t, true };
            break;
        default: break; // Bars, Wave, Orbit use the levels
    }
}

void readSequencer()
{
    const Sequencer::strPlayer* pl = sequencer.ptrPlayer;
    bool playing = pl->isPlay;
    if (playing && !wasPlaying)
        for (int t = 0; t < kTracks; t++) lastPos[t] = -1;
    wasPlaying = playing;
    if (!playing) return;

    Sequencer::strPattern* pat = sequencer.getPatternToUI();
    for (int t = 0; t < kTracks; t++)
    {
        int pos = pl->track[t].actual_pos;
        if (pos == lastPos[t]) continue;
        lastPos[t] = (int16_t)pos;
        if (pos < 0 || pos > Sequencer::MAXSTEP || muted(t)) continue;
        const auto& st = pat->track[t].step[pos];
        if (st.note >= 0) addEvent(t, st.note, st.volume < 0 ? 100 : st.volume);
    }
}

void step()
{
    frame++;
    if (p == nullptr) return;
    for (int t = 0; t < kTracks; t++)
    {
        level[t] *= 0.88f;
        orbitAngle[t] += (0.010f + 0.004f * t) * (1.0f + 2.0f * level[t]);
    }
    energy *= 0.94f;
    for (int i = 0; i < kMaxP; i++)
    {
        Particle& q = p[i];
        if (!q.alive) continue;
        q.x += q.vx;
        q.y += q.vy;
        if (styleIdx == 7)
            q.size *= q.grow;
        else
            q.size += q.grow;
        if (q.size < 1.0f) q.size = 1.0f;
        q.life -= q.decay;
        if (q.life <= 0.0f || q.y > curH + 40 || q.x < -40 || q.x > curW + 60 ||
            (styleIdx == 7 && q.size > curW * 1.2f))
            q.alive = false;
    }
}

inline void colorA(uint32_t c, float a)
{
    API_COLOR(c);
    API_COLOR_A((uint8_t)(255.0f * fmaxf(0.0f, fminf(1.0f, a))));
}
} // namespace

bool isOn() { return on; }

void setOn(bool o)
{
    if (o && p == nullptr) p = new (std::nothrow) Particle[kMaxP];
    if (!o && p != nullptr)
    {
        delete[] p;
        p = nullptr;
    }
    on = o && p != nullptr;
    if (p != nullptr) memset(p, 0, sizeof(Particle) * kMaxP);
    memset(level, 0, sizeof(level));
    for (int t = 0; t < kTracks; t++) lastPos[t] = -1;
    wasPlaying = false;
}

uint8_t style() { return styleIdx; }

void changeStyle(int dir)
{
    styleIdx = (uint8_t)((styleIdx + kStyleCount + (dir > 0 ? 1 : -1)) % kStyleCount);
    if (p != nullptr) memset(p, 0, sizeof(Particle) * kMaxP);
}

const char* styleName(uint8_t s)
{
    static const char* const names[kStyleCount] = { "Bars",   "Rain",  "Ripples", "Wave",
                                                    "Scroll", "Burst", "Orbit",   "Tunnel" };
    return names[s % kStyleCount];
}

bool tick()
{
    if (!on) return false;
    if (frameTimer < 30) return false;
    frameTimer = 0;
    readSequencer();
    step();
    return true;
}

void paint(int, int x, int y, int w, int h)
{
    if (p == nullptr) return;
    curX = x;
    curY = y;
    curW = w;
    curH = h;
    int cur = cursorTrack();

    // black over the grid
    API_COLOR(0x000000);
    API_BEGIN(RECTS);
    API_VERTEX2F(x, y);
    API_VERTEX2F(x + w, y + h);
    API_END();

    switch (styleIdx)
    {
        case 0: // Bars
        {
            API_BEGIN(RECTS);
            int bw = w / kTracks;
            for (int t = 0; t < kTracks; t++)
            {
                int bh = (int)(level[t] * (h - 30));
                API_COLOR(trackColor(t, t == cur));
                API_VERTEX2F(x + t * bw + 8, y + h - 20 - bh);
                API_VERTEX2F(x + (t + 1) * bw - 8, y + h - 20);
            }
            API_END();
            break;
        }
        case 3: // Wave: one line per track, amplitude by its level
        {
            API_LINE_WIDTH(16 * 2);
            for (int t = 0; t < kTracks; t++)
            {
                if (level[t] < 0.03f && t != cur) continue;
                colorA(trackColor(t, t == cur), 0.4f + level[t]);
                API_BEGIN(LINE_STRIP);
                float amp = (h * 0.4f) * level[t] + 2.0f;
                float k = (1.5f + t * 0.7f) * 2.0f * kPi / w;
                for (int i = 0; i <= 40; i++)
                {
                    int px = i * w / 40;
                    float yy = h * 0.5f + amp * sinf(px * k + frame * (0.08f + 0.02f * t));
                    API_VERTEX2F(x + px, y + (int)yy);
                }
                API_END();
            }
            break;
        }
        case 6: // Orbit
        {
            float cx = w * 0.5f, cy = h * 0.5f;
            API_BEGIN(FTPOINTS);
            for (int t = 0; t < kTracks; t++)
            {
                float r = (0.12f + 0.05f * t) * h * 1.6f;
                API_COLOR(trackColor(t, t == cur));
                API_POINT_SIZE((uint16_t)(16 * (5 + 14 * level[t])));
                API_VERTEX2F(x + (int)(cx + r * cosf(orbitAngle[t])), y + (int)(cy + 0.6f * r * sinf(orbitAngle[t])));
            }
            API_END();
            break;
        }
        default: // particle styles
        {
            if (styleIdx == 7)
            {
                API_LINE_WIDTH(16 * 3);
                for (int i = 0; i < kMaxP; i++)
                {
                    const Particle& q = p[i];
                    if (!q.alive) continue;
                    float hs = q.size, vs = q.size * 0.6f;
                    colorA(trackColor(q.track, q.track == cur), 1.2f - q.size / (w * 1.2f));
                    API_BEGIN(LINE_STRIP);
                    API_VERTEX2F(x + (int)(q.x - hs), y + (int)(q.y - vs));
                    API_VERTEX2F(x + (int)(q.x + hs), y + (int)(q.y - vs));
                    API_VERTEX2F(x + (int)(q.x + hs), y + (int)(q.y + vs));
                    API_VERTEX2F(x + (int)(q.x - hs), y + (int)(q.y + vs));
                    API_VERTEX2F(x + (int)(q.x - hs), y + (int)(q.y - vs));
                    API_END();
                }
            }
            else if (styleIdx == 1 || styleIdx == 4)
            {
                API_BEGIN(RECTS);
                for (int i = 0; i < kMaxP; i++)
                {
                    const Particle& q = p[i];
                    if (!q.alive) continue;
                    colorA(trackColor(q.track, q.track == cur), q.life);
                    if (styleIdx == 1) // a drop with a tail
                    {
                        API_VERTEX2F(x + (int)q.x - (int)(q.size / 2), y + (int)q.y - 24);
                        API_VERTEX2F(x + (int)q.x + (int)(q.size / 2), y + (int)q.y);
                    }
                    else
                    {
                        API_VERTEX2F(x + (int)q.x, y + (int)q.y - (int)(q.size / 2));
                        API_VERTEX2F(x + (int)q.x + 14, y + (int)q.y + (int)(q.size / 2));
                    }
                }
                API_END();
            }
            else // Ripples, Burst: discs
            {
                API_BEGIN(FTPOINTS);
                for (int i = 0; i < kMaxP; i++)
                {
                    const Particle& q = p[i];
                    if (!q.alive) continue;
                    colorA(trackColor(q.track, q.track == cur), styleIdx == 2 ? q.life * 0.6f : q.life);
                    API_POINT_SIZE((uint16_t)(16.0f * fminf(q.size, 500.0f)));
                    API_VERTEX2F(x + (int)q.x, y + (int)q.y);
                }
                API_END();
                if (styleIdx == 2)
                {
                    // hollow the ripples: a smaller black disc inside each
                    API_COLOR(0x000000);
                    API_COLOR_A(255);
                    API_BEGIN(FTPOINTS);
                    for (int i = 0; i < kMaxP; i++)
                    {
                        const Particle& q = p[i];
                        if (!q.alive || q.size < 10.0f) continue;
                        API_POINT_SIZE((uint16_t)(16.0f * (q.size - 4.0f)));
                        API_VERTEX2F(x + (int)q.x, y + (int)q.y);
                    }
                    API_END();
                }
            }
            break;
        }
    }
    API_COLOR_A(255);

    // the step dots (16 per bar) along the bottom
    int pos = sequencer.ptrPlayer->isPlay ? sequencer.ptrPlayer->track[cur >= 0 ? cur : 0].actual_pos : -1;
    API_BEGIN(FTPOINTS);
    for (int i = 0; i < 16; i++)
    {
        bool now = pos >= 0 && (pos % 16) == i;
        API_COLOR(now ? 0xFFFFFF : 0x3A3A3A);
        API_POINT_SIZE(16 * (now ? 5 : 3));
        API_VERTEX2F(x + w / 2 - 15 * 16 / 2 + i * 15, y + h - 8);
    }
    API_END();

    // the style's name, top left
    API_COLOR(0x6A6A6A);
    API_CMD_TEXT(x + 8, y + 6, fonts[0].handle, 0, styleName(styleIdx));
}
} // namespace ceVis
