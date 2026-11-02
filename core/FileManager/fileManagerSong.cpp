#include "fileManagerDefs.h"
#include "mtStructs.h"

#include "core/interfacePopups.h"

#include "fileManager.h"

// uint8_t patternToLoad = 0;

//__NOINIT(EXTERNAL_RAM) uint8_t undo_Bank[1024*1024];
__NOINIT(EXTERNAL_RAM) Sequencer::strPattern songTrackCopy[2];

void setPatternBitmask(int patternIdx, int trackIdx, bool value);

AUDIO_CORE_RAM uint8_t cFileManager::getNextSongPattern()
{
    strSong* song = &mtProject.song;
    return song->playlist[song->playlistPos + 1] > 0 ? song->playlist[song->playlistPos + 1]
                                                     : song->playlist[0];
}
uint8_t cFileManager::getSongPattern(uint8_t pos)
{
    strSong* song = &mtProject.song;
    return song->playlist[pos];
}

std::optional<int> cFileManager::getPatternPosInSong(uint8_t patternToSearch)
{
    strSong* song = &mtProject.song;
    int startFromPos = song->playlistPos;

    for (int tryIdx = 0; tryIdx < SONG_MAX; tryIdx++)
    {
        int songPosition = (startFromPos + tryIdx) % SONG_MAX;
        if (song->playlist[songPosition] == patternToSearch)
        {
            return songPosition;
        }
    }

    return std::nullopt;
}

void cFileManager::setSongPos(uint8_t pos)
{
    strSong* song = &mtProject.song;
    song->playlistPos = pos;
}

bool cFileManager::switchNextPatternInSong()
{
    strSong* song = &mtProject.song;
    if (song->playlist[++(song->playlistPos)] != PLAYLIST_EMPTY_SLOT)
    {
        return 1;
    }
    else
    {
        song->playlistPos = 0;
        return 0;
    }

    return 0;
}

void cFileManager::copySongTracks(char* currentProjectPath,
                                  uint8_t srcPatternIdx,
                                  uint8_t destPaternIdx,
                                  uint8_t trackStartSrc,
                                  uint8_t trackStartDest,
                                  uint8_t tracksNum)
{
    bool status = false;
    char currentPath[PATCH_SIZE];
    Sequencer::strPattern* srcPattern = &songTrackCopy[0];
    Sequencer::strPattern* destPattern = &songTrackCopy[1];

    snprintf(currentPath,
             sizeof(currentPath),
             "%s/patterns/pattern_%02d.mtp",
             currentProjectPath,
             srcPatternIdx);
    status = readPatternFile(currentPath, srcPattern);

    if (!status)
    {
        return;
    }

    newFileManager.storeSongUndoRevision(destPaternIdx);

    snprintf(currentPath,
             sizeof(currentPath),
             "%s/patterns/pattern_%02d.mtp",
             currentProjectPath,
             destPaternIdx);
    status = readPatternFile(currentPath, destPattern);
    if (status == false)
    {
        sequencer.clearPattern(destPattern);
    }

    while ((trackStartSrc + tracksNum) > kAllTracks || (trackStartDest + tracksNum) > kAllTracks)
    {
        tracksNum--;

        if (tracksNum == 0)
        {
            return;
        }
    }

    for (int trackIdx = 0; trackIdx < tracksNum; trackIdx++)
    {
        if (&destPattern->track[trackStartDest + trackIdx] ==
            &srcPattern->track[trackStartSrc + trackIdx]) // memcpy restrict protection
        {
            continue;
        }

        int srcTrackIdx = trackStartSrc + trackIdx;
        int destTrackIdx = trackStartDest + trackIdx;
        Sequencer::strPattern::strTrack* srcTrack = &srcPattern->track[srcTrackIdx];
        Sequencer::strPattern::strTrack* destTrack = &destPattern->track[destTrackIdx];

        for (int stepIdx = 0; stepIdx <= Sequencer::MAXSTEP; stepIdx++)
        {
            sequencer.copyStep(
              &srcTrack->step[stepIdx], &destTrack->step[stepIdx], !isSampleTrack(destTrackIdx));
        }
    }

    destPattern->track[0].length =
      std::max(srcPattern->track[0].length, destPattern->track[0].length);

    if (tracksNum == kAllTracks)
    {
        destPattern->track[0].length = srcPattern->track[0].length;
    }

    sequencer.updateTrackLengths(destPattern);

    if (updatePatternBitmask(destPaternIdx, destPattern))
    {
        setProjectStructChanged();
    }

    writePatternFile(currentPath, destPattern);
}

void cFileManager::deleteTracks(char* currentProjectPath,
                                uint8_t srcPatternIdx,
                                uint8_t trackStartSrc,
                                uint8_t tracksNum)
{
    bool status = false;
    char currentPath[PATCH_SIZE];
    Sequencer::strPattern* srcPattern = &songTrackCopy[0];

    snprintf(currentPath,
             sizeof(currentPath),
             "%s/patterns/pattern_%02d.mtp",
             currentProjectPath,
             srcPatternIdx);
    status = readPatternFile(currentPath, srcPattern);

    if (status)
    {
        storeSongUndoRevision(srcPatternIdx);

        for (uint8_t track = 0; track < tracksNum; track++)
        {
            sequencer.clearSingleTrack(&srcPattern->track[track + trackStartSrc]);
        }

        if (updatePatternBitmask(srcPatternIdx, srcPattern))
        {
            setProjectStructChanged();
        }

        writePatternFile(currentPath, srcPattern);
    }
}

void cFileManager::updateActualPatternBitmask()
{
    assert(mtProject.values.actualPattern > 0);

    if (updatePatternBitmask(mtProject.values.actualPattern, sequencer.getPatternToSaveToFile()))
    {
        setProjectStructChanged();
    }
}

// Color Song: one 4-bit colour per track per pattern, 0 = none.
__NOINIT(EXTERNAL_RAM) static uint8_t songColors[PATTERN_INDEX_MAX + 1][kAllTracks / 2];
static uint8_t songColorsValid[(PATTERN_INDEX_MAX + 1 + 7) / 8]; // zeroed at boot

static uint8_t instrumentColor(uint8_t instrument)
{
    if (isMidiInstrument(instrument))
        return 9 + instrument % 3; // MIDI: blues
    if (isSynthInstrument(instrument))
        return 12 + instrument % 4; // synths: purples and pinks
    return 1 + instrument % 8; // samples: eight hues
}

void cFileManager::computeSongColors(uint8_t patternIdx, Sequencer::strPattern* pattern)
{
    uint8_t length = pattern->track[0].length;
    for (int t = 0; t < kAllTracks; t++)
    {
        uint8_t color = 0;
        for (int s = 0; s <= length; s++)
        {
            if (pattern->track[t].step[s].note >= 0)
            {
                color = instrumentColor(pattern->track[t].step[s].instrument);
                break;
            }
        }
        uint8_t& pair = songColors[patternIdx][t / 2];
        pair = (t & 1) ? ((pair & 0x0F) | (color << 4)) : ((pair & 0xF0) | color);
    }
    songColorsValid[patternIdx / 8] |= (1 << (patternIdx % 8));
}

void cFileManager::invalidateSongColors()
{
    memset(songColorsValid, 0, sizeof(songColorsValid));
}

uint8_t cFileManager::getSongColor(uint8_t patternIdx, uint8_t track)
{
    if (!(songColorsValid[patternIdx / 8] & (1 << (patternIdx % 8))) || track >= kAllTracks)
        return 0;
    uint8_t pair = songColors[patternIdx][track / 2];
    return (track & 1) ? (pair >> 4) : (pair & 0x0F);
}

void cFileManager::scanSongColors()
{
    if (!sequencer.isStop() || getStatus() != fmIdle)
        return;

    for (int pos = 0; pos < SONG_MAX; pos++)
    {
        uint8_t idx = mtProject.song.playlist[pos];
        if (idx == PLAYLIST_EMPTY_SLOT)
            break;
        if (songColorsValid[idx / 8] & (1 << (idx % 8)))
            continue;

        if (idx == mtProject.values.actualPattern)
        {
            computeSongColors(idx, sequencer.getPatternToUI());
            continue;
        }

        char patternPath[PATCH_SIZE]{ 0 };
        snprintf(patternPath, PATCH_SIZE, cWorkspacePatternFileFormat, idx);
        if (!fileSystem->exists(patternPath))
        {
            songColors[idx][0] = 0;
            memset(songColors[idx], 0, sizeof(songColors[idx]));
            songColorsValid[idx / 8] |= (1 << (idx % 8));
            continue;
        }
        if (readPatternFile(patternPath, sequencer.getPatternToLoadFromFile()))
        {
            computeSongColors(idx, sequencer.getPatternToLoadFromFile());
        }
    }
}

bool cFileManager::updatePatternBitmask(uint8_t patternIdx, Sequencer::strPattern* sourcePattern)
{
    assert(patternIdx > 0);

    computeSongColors(patternIdx, sourcePattern);

    uint8_t bitmaskIdx = patternIdx - 1;

    uint16_t dataBefore = mtProject.values.allPatternsBitmask[bitmaskIdx] |
                          (mtProject.values.allPatternsBitmaskMidi[bitmaskIdx] << 8);

    uint8_t length = sourcePattern->track[0].length;

    for (uint8_t trackNum = 0; trackNum < kAllTracks; trackNum++)
    {
        bool isAnyActive = false;

        for (uint8_t stepIdx = 0; stepIdx <= length; stepIdx++)
        {
            Sequencer::strPattern::strTrack::strStep* step =
              &((Sequencer::strPattern*)sourcePattern)->track[trackNum].step[stepIdx];

            if (step->note != Sequencer::STEP_NOTE_EMPTY || step->hasAnyFx() ||
                step->instrument >= MIDI_INSTRUMENTS_START_IDX)
            {
                isAnyActive = true;
                break;
            }
        }

        setPatternBitmask(bitmaskIdx, trackNum, isAnyActive);
    }

    uint16_t dataAfter = mtProject.values.allPatternsBitmask[bitmaskIdx] |
                         (mtProject.values.allPatternsBitmaskMidi[bitmaskIdx] << 8);

    if (dataBefore == dataAfter)
    {
        return false;
    }

    return true;
}

inline void setPatternBitmask(int bitmaskIdx, int trackIdx, bool value)
{
    assert(bitmaskIdx <= PATTERN_INDEX_MAX);
    assert(trackIdx >= 0);

    if (trackIdx < kSampleTracks)
    {
        if (value)
        {
            mtProject.values.allPatternsBitmask[bitmaskIdx] |= (1 << trackIdx);
        }
        else
        {
            mtProject.values.allPatternsBitmask[bitmaskIdx] &= ~(1 << trackIdx);
        }
    }
    else if (trackIdx < kSampleTracks + kMidiTracks)
    {
        trackIdx = trackIdx - kSampleTracks;

        if (value)
        {
            mtProject.values.allPatternsBitmaskMidi[bitmaskIdx] |= (1 << trackIdx);
        }
        else
        {
            mtProject.values.allPatternsBitmaskMidi[bitmaskIdx] &= ~(1 << trackIdx);
        }
    }
}
