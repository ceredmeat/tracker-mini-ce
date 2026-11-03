// tracker-mini-ce: the Looper screen, projects and WAV files.
// A project is a folder /Looper/Projects/<name> with track1.wav ... track4.wav and
// looper.cfg (the settings). A folder with only WAVs loads too, so any WAV can be put in a
// track by naming it trackN.wav in a folder there.

#include "looper.h"

#ifdef CE_LOOPER

#include <stdio.h>
#include <string.h>

#include "AudioLooper.h"
#include "TrackerImpl.h"
#include "cr_section_macros.h"

using namespace lp;

static constexpr char kRoot[] = "/Looper";
static constexpr char kProjects[] = "/Looper/Projects";
static constexpr char kExport[] = "/Looper/Export";
static constexpr uint32_t kCfgMagic = 0x4C4F4F50;   // "LOOP"
static constexpr uint16_t kCfgVersion = 1;

static FileSystem* fs() { return TrackerImpl::getInstance()->getPlatform()->getFileSystem(); }

struct SavedTrack
{
    uint32_t len;
    int64_t anchor;
    int64_t off;
    uint8_t ch, mode, muted, volume;
    uint8_t send[kFxSlots];
    int8_t djFilter;
    uint8_t stopOpt, fixedBars, startOpt, afterRec, tempoOpt, channels, playing;
    GrainParams grain;
    uint8_t perfKnob[kPerfFx];
};

struct SavedProject
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t barFrames;
    float loopBpm, tempoPct;
    Config cfg;
    SavedTrack track[kTracks];
    uint8_t fxType[kFxSlots];
    uint8_t fxKnob[kFxSlots][kFxKnobs];
    uint8_t fxReturn[kFxSlots], fxMute[kFxSlots], chain[kFxSlots];
};

static void putLE32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void putLE16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static uint32_t getLE32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t getLE16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bool writeWav(const char* path, int t)
{
    const Track& tr = engine.track[t];
    if (tr.len == 0) return true;
    FileSystem* f = fs();
    if (f->exists(path)) f->remove(path);
    auto [ok, h] = f->openFile(path, FileSystem::kWriteCreateNew);
    if (!ok) return false;
    uint8_t hdr[44];
    uint32_t dataBytes = tr.len * tr.ch * 2;
    memcpy(hdr, "RIFF", 4);
    putLE32(hdr + 4, 36 + dataBytes);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    putLE32(hdr + 16, 16);
    putLE16(hdr + 20, 1);
    putLE16(hdr + 22, tr.ch);
    putLE32(hdr + 24, 44100);
    putLE32(hdr + 28, 44100 * tr.ch * 2);
    putLE16(hdr + 32, (uint16_t)(tr.ch * 2));
    putLE16(hdr + 34, 16);
    memcpy(hdr + 36, "data", 4);
    putLE32(hdr + 40, dataBytes);
    bool good = f->writeFile(hdr, 44, h) == 44;
    const uint8_t* src = (const uint8_t*)(tr.mem + (size_t)tr.start * tr.ch);
    uint32_t left = dataBytes;
    while (good && left)
    {
        uint32_t n = left > 32768 ? 32768 : left;
        // copy through the scratch so the card writes from ordinary RAM blocks
        memcpy(AudioLooper::uiScratch(), src, n);
        good = f->writeFile(AudioLooper::uiScratch(), n, h) == n;
        src += n;
        left -= n;
    }
    f->closeFile(h);
    return good;
}

// reads a 16-bit PCM WAV into an empty track; returns false if it is not one
bool cLooper::loadWav(int t, const char* path)
{
    FileSystem* f = fs();
    auto [ok, h] = f->openFile(path, FileSystem::kRead);
    if (!ok) return false;
    uint8_t* buf = AudioLooper::uiScratch();
    bool good = f->readFile(buf, 12, h) == 12 && memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WAVE", 4) == 0;
    uint16_t ch = 0, bits = 0, fmt = 0;
    uint32_t dataBytes = 0;
    while (good)
    {
        if (f->readFile(buf, 8, h) != 8)
        {
            good = false;
            break;
        }
        uint32_t sz = getLE32(buf + 4);
        if (memcmp(buf, "fmt ", 4) == 0)
        {
            uint32_t n = sz < 64 ? sz : 64;
            if (f->readFile(buf + 8, n, h) != n)
            {
                good = false;
                break;
            }
            fmt = getLE16(buf + 8);
            ch = getLE16(buf + 10);
            bits = getLE16(buf + 22);
            if (sz > n) f->seekFileCur((int)(sz - n), h);
            if (sz & 1) f->seekFileCur(1, h);
        }
        else if (memcmp(buf, "data", 4) == 0)
        {
            dataBytes = sz;
            break;
        }
        else
        {
            f->seekFileCur((int)(sz + (sz & 1)), h);
        }
    }
    if (!good || fmt != 1 || bits != 16 || ch < 1 || ch > 2 || dataBytes == 0)
    {
        f->closeFile(h);
        return false;
    }
    uint32_t frames = dataBytes / (ch * 2);
    uint32_t maxFrames = engine.track[t].memSamples / ch;
    if (frames > maxFrames) frames = maxFrames;
    int16_t* dst = engine.beginLoad(t, (uint8_t)ch, frames);
    if (!dst)
    {
        f->closeFile(h);
        return false;
    }
    uint8_t* d = (uint8_t*)dst;
    uint32_t left = frames * ch * 2;
    while (left)
    {
        uint32_t n = left > 32768 ? 32768 : left;
        uint32_t got = f->readFile(buf, n, h);
        memcpy(d, buf, got);
        d += got;
        left -= got;
        if (got < n) break;
    }
    f->closeFile(h);
    uint32_t loaded = frames - left / (ch * 2);
    engine.endLoad(t, loaded, !engine.hasLoops());
    return loaded > 0;
}

void cLooper::newProjectName()
{
    // the first free "Loop NN"
    for (int i = 1; i < 100; i++)
    {
        char path[64];
        snprintf(projectName, sizeof(projectName), "Loop %02d", i);
        snprintf(path, sizeof(path), "%s/%s", kProjects, projectName);
        if (!fs()->exists(path)) return;
    }
}

static __NOINIT(EXTERNAL_RAM) char s_projectList[16][24];

void cLooper::scanProjects()
{
    projectList = s_projectList;
    projectCount = 0;
    FileSystem* f = fs();
    if (!f->exists(kProjects)) return;
    static __NOINIT(EXTERNAL_RAM) char names[16 * 24];
    static __NOINIT(EXTERNAL_RAM) char* list[16];
    int count = 0;
    f->listDirContents(kProjects, FileSystem::kFolders, [](const char*) { return true; }, names, sizeof(names), 23, count,
                       list);
    if (count > 16) count = 16;
    for (int i = 0; i < count; i++)
    {
        snprintf(projectList[i], sizeof(projectList[i]), "%s", list[i]);
    }
    projectCount = (uint8_t)count;
    if (fileRow >= projectCount) fileRow = projectCount ? projectCount - 1 : 0;
}

bool cLooper::saveProject()
{
    FileSystem* f = fs();
    char path[96];
    f->mkdir(0, kRoot);
    f->mkdir(0, kProjects);
    snprintf(path, sizeof(path), "%s/%s", kProjects, projectName);
    f->mkdir(0, path);
    bool good = true;
    for (int t = 0; t < kTracks; t++)
    {
        snprintf(path, sizeof(path), "%s/%s/track%d.wav", kProjects, projectName, t + 1);
        if (engine.track[t].len)
            good = writeWav(path, t) && good;
        else if (f->exists(path))
            f->remove(path);
    }
    SavedProject sp{};
    sp.magic = kCfgMagic;
    sp.version = kCfgVersion;
    sp.size = sizeof(sp);
    sp.barFrames = engine.barFrames();
    sp.loopBpm = engine.loopBpm();
    sp.tempoPct = engine.tempoPct();
    sp.cfg = engine.cfg;
    for (int t = 0; t < kTracks; t++)
    {
        const Track& tr = engine.track[t];
        SavedTrack& s = sp.track[t];
        s.len = tr.len;
        s.anchor = tr.anchor;
        s.off = tr.off;
        s.ch = tr.ch;
        s.mode = tr.mode;
        s.muted = tr.muted;
        s.volume = tr.volume;
        memcpy(s.send, tr.send, sizeof(s.send));
        s.djFilter = tr.djFilter;
        s.stopOpt = tr.stopOpt;
        s.fixedBars = tr.fixedBars;
        s.startOpt = tr.startOpt;
        s.afterRec = tr.afterRec;
        s.tempoOpt = tr.tempoOpt;
        s.channels = tr.channels;
        s.playing = tr.state != TrackState::kStopped;
        s.grain = tr.grain;
        memcpy(s.perfKnob, tr.perfKnob, sizeof(s.perfKnob));
    }
    for (int s = 0; s < kFxSlots; s++)
    {
        sp.fxType[s] = (uint8_t)engine.fx[s].type();
        memcpy(sp.fxKnob[s], engine.fx[s].knob, kFxKnobs);
        sp.fxReturn[s] = engine.fxReturn[s];
        sp.fxMute[s] = engine.fxMute[s];
        sp.chain[s] = engine.chainSend[s];
    }
    snprintf(path, sizeof(path), "%s/%s/looper.cfg", kProjects, projectName);
    if (f->exists(path)) f->remove(path);
    auto [ok, h] = f->openFile(path, FileSystem::kWriteCreateNew);
    if (!ok) return false;
    good = f->writeFile(&sp, sizeof(sp), h) == sizeof(sp) && good;
    f->closeFile(h);
    return good;
}

bool cLooper::loadProject(const char* name)
{
    FileSystem* f = fs();
    char path[96];
    engine.clearAll();
    // let the audio side take the clear
    uint32_t b = engine.blockCount();
    for (uint32_t spin = 0; spin < 20000000u && engine.audioRunning && engine.blockCount() - b < 2; spin++)
    {
    }

    SavedProject sp;
    bool haveCfg = false;
    snprintf(path, sizeof(path), "%s/%s/looper.cfg", kProjects, name);
    if (f->exists(path))
    {
        auto [ok, h] = f->openFile(path, FileSystem::kRead);
        if (ok)
        {
            haveCfg = f->readFile(&sp, sizeof(sp), h) == sizeof(sp) && sp.magic == kCfgMagic && sp.version == kCfgVersion;
            f->closeFile(h);
        }
    }
    if (haveCfg)
    {
        engine.cfg = sp.cfg;
        for (int s = 0; s < kFxSlots; s++)
        {
            engine.fx[s].setType(sp.fxType[s]);
            memcpy(engine.fx[s].knob, sp.fxKnob[s], kFxKnobs);
            engine.fxReturn[s] = sp.fxReturn[s];
            engine.fxMute[s] = sp.fxMute[s];
            engine.chainSend[s] = sp.chain[s];
        }
    }
    bool any = false;
    for (int t = 0; t < kTracks; t++)
    {
        snprintf(path, sizeof(path), "%s/%s/track%d.wav", kProjects, name, t + 1);
        if (!f->exists(path)) continue;
        if (!loadWav(t, path)) continue;
        any = true;
        Track& tr = engine.track[t];
        if (haveCfg)
        {
            const SavedTrack& s = sp.track[t];
            if (s.len && s.len <= tr.len) tr.len = s.len;
            tr.anchor = s.anchor;
            tr.off = s.off;
            tr.mode = s.mode;
            tr.muted = s.muted;
            tr.volume = s.volume;
            memcpy(tr.send, s.send, sizeof(tr.send));
            tr.djFilter = s.djFilter;
            tr.stopOpt = s.stopOpt;
            tr.fixedBars = s.fixedBars;
            tr.startOpt = s.startOpt;
            tr.afterRec = s.afterRec;
            tr.tempoOpt = s.tempoOpt;
            tr.grain = s.grain;
            memcpy(tr.perfKnob, s.perfKnob, sizeof(tr.perfKnob));
            tr.state = s.playing ? TrackState::kPlaying : TrackState::kStopped;
        }
        else
            tr.state = TrackState::kPlaying;
        tr.version++;
    }
    if (haveCfg) engine.restoreClock(sp.barFrames, sp.loopBpm, sp.tempoPct);
    snprintf(projectName, sizeof(projectName), "%s", name);
    for (int t = 0; t < kTracks; t++)
        laneValid[t] = false;
    return any;
}

bool cLooper::exportTracks()
{
    FileSystem* f = fs();
    f->mkdir(0, kRoot);
    f->mkdir(0, kExport);
    bool good = true, any = false;
    for (int t = 0; t < kTracks; t++)
    {
        if (!engine.track[t].len) continue;
        char path[96];
        snprintf(path, sizeof(path), "%s/%s_track%d.wav", kExport, projectName, t + 1);
        good = writeWav(path, t) && good;
        any = true;
    }
    return any && good;
}

#endif
