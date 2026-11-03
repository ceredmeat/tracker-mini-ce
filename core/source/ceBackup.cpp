// tracker-mini-ce: Auto Backup (see ceBackup.h).
//
// The job owns one heap block (about 10 KB) from start to end; if it can't be had, that
// backup is skipped. Work is done only while the file manager is idle, two 2 KB
// read + write pairs per main-loop pass, so playback and the screen keep going. Files are
// closed whenever the file manager gets busy; the file being copied then starts again.

#include "ceBackup.h"

#ifdef PLATFORM_IMX

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Platform.h"
#include "TrackerImpl.h"
#include "ceModsExtra.h"
#include "fileManager.h"
#include "mtStructs.h"

namespace ceBackup
{
namespace
{
constexpr char kBackupsRoot[] = "Backups";
constexpr int kPath = 256;
constexpr int kStackDepth = 8;
constexpr int kRelLen = 128;
constexpr int kNamesBuf = 6144;
constexpr int kMaxNames = 300;
constexpr int kMaxNameLen = 64;
constexpr int kCopyBuf = 2048;
constexpr int kChunksPerPass = 2;
constexpr int kMaxFolderNumbers = 64;

const uint8_t kKeepCounts[8] = { 5, 1, 2, 3, 10, 20, 50, 0 };

enum class State : uint8_t
{
    kIdle,
    kPending,
    kNextDir,
    kCopy,
    kInfo,
    kPrune,
};

struct Job
{
    char srcRoot[kPath];
    char dstRoot[kPath];
    char projectRoot[kPath]; // Backups/<name>
    char projectName[PROJECT_NAME_SIZE + 1];
    char rel[kStackDepth][kRelLen]; // directory stack, relative to the roots
    int depth;
    char curDir[kRelLen];
    char names[kNamesBuf];
    char* list[kMaxNames];
    int count;
    int index;
    uint8_t buf[kCopyBuf];
    char path[kPath];
    char path2[kPath];
    FileSystem::FileHandle in;
    FileSystem::FileHandle out;
    bool open;
    bool samples;
    uint8_t keep;
    uint16_t number;
    uint32_t files;
    uint32_t bytes;
};

State state = State::kIdle;
Job* job = nullptr;

FileSystem* fs()
{
    return TrackerImpl::getInstance()->getPlatform()->getFileSystem();
}

bool fileManagerIdle()
{
    // the "...End" states mean the last operation is over (the screen clears them later)
    uint8_t s = newFileManager.getStatus();
    return (s == fmIdle || s >= fmLoadEnd) && newFileManager.getCurrentOperation() == fmNoOperation;
}

void closeFiles()
{
    if (job != nullptr && job->open)
    {
        fs()->closeFile(job->in);
        fs()->closeFile(job->out);
        job->open = false;
    }
}

void finish()
{
    closeFiles();
    free(job);
    job = nullptr;
    state = State::kIdle;
}

void join(char* out, const char* a, const char* b, const char* c = nullptr)
{
    if (c != nullptr && c[0] != 0)
    {
        snprintf(out, kPath, b[0] ? "%s/%s/%s" : "%s%s/%s", a, b, c);
    }
    else
    {
        snprintf(out, kPath, b[0] ? "%s/%s" : "%s%s", a, b);
    }
}

bool isNumber(const char* s)
{
    if (s[0] == 0) return false;
    for (const char* p = s; *p; p++)
        if (*p < '0' || *p > '9') return false;
    return true;
}

// the numbered backup folders in Backups/<name>, sorted (listDirContents sorts by name and
// the numbers are four digits)
int listNumbers(uint16_t* out)
{
    int n = kMaxNames;
    fs()->listDirContents(job->projectRoot,
                          FileSystem::kFolders,
                          nullptr,
                          job->names,
                          kNamesBuf,
                          kMaxNameLen,
                          n,
                          job->list);
    int k = 0;
    for (int i = 0; i < n && k < kMaxFolderNumbers; i++)
    {
        const char* name = job->list[i] + 1; // folders start with '/'
        if (isNumber(name)) out[k++] = (uint16_t)atoi(name);
    }
    // sort (names are usually already in order)
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && out[j - 1] > out[j]; j--)
        {
            uint16_t t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return k;
}

void start()
{
    uint8_t keepSetting = ceExtra::at(ceExtra::kBackupKeep);
    uint8_t keep = kKeepCounts[keepSetting & 7];
    if (keep == 0)
    {
        state = State::kIdle;
        return;
    }
    const char* name = newFileManager.getCurrentProjectName();
    const char* path = newFileManager.getCurrentProjectPath();
    if (name == nullptr || name[0] == 0 || path == nullptr)
    {
        state = State::kIdle;
        return;
    }

    if (job == nullptr) job = (Job*)malloc(sizeof(Job));
    if (job == nullptr)
    {
        state = State::kIdle; // not enough memory now: skip this backup
        return;
    }
    memset(job, 0, sizeof(Job));
    job->keep = keep;
    job->samples = ceExtra::at(ceExtra::kBackupSamples) == 0;
    strncpy(job->projectName, name, PROJECT_NAME_SIZE);
    snprintf(job->srcRoot, kPath, "%s/%s", path, name);
    snprintf(job->projectRoot, kPath, "%s/%s", kBackupsRoot, name);

    FileSystem* f = fs();
    if (!f->exists(job->srcRoot))
    {
        finish();
        return;
    }
    f->mkdir(0, job->projectRoot);

    uint16_t numbers[kMaxFolderNumbers];
    int n = listNumbers(numbers);
    job->number = n > 0 ? numbers[n - 1] + 1 : 1;
    if (job->number > 9999) job->number = 1;
    snprintf(job->dstRoot, kPath, "%s/%04u", job->projectRoot, job->number);
    if (f->exists(job->dstRoot)) f->removeDirWithFiles(job->dstRoot);
    if (!f->mkdir(0, job->dstRoot) && !f->exists(job->dstRoot))
    {
        finish();
        return;
    }

    job->depth = 1;
    job->rel[0][0] = 0;
    state = State::kNextDir;
}

void nextDir()
{
    FileSystem* f = fs();
    if (job->depth == 0)
    {
        state = State::kInfo;
        return;
    }
    job->depth--;
    strncpy(job->curDir, job->rel[job->depth], kRelLen);
    job->curDir[kRelLen - 1] = 0;

    if (job->curDir[0] != 0)
    {
        join(job->path, job->dstRoot, job->curDir);
        f->mkdir(0, job->path);
    }

    // sub-folders go on the stack (not export/, and samples/ only if wanted)
    join(job->path, job->srcRoot, job->curDir);
    int n = kMaxNames;
    f->listDirContents(job->path,
                       FileSystem::kFolders,
                       nullptr,
                       job->names,
                       kNamesBuf,
                       kMaxNameLen,
                       n,
                       job->list);
    for (int i = 0; i < n; i++)
    {
        const char* name = job->list[i] + 1;
        if (name[0] == '.' ) continue;
        if (job->curDir[0] == 0)
        {
            if (strcmp(name, "export") == 0) continue;
            if (!job->samples && strcmp(name, "samples") == 0) continue;
        }
        if (job->depth >= kStackDepth) break;
        char* slot = job->rel[job->depth];
        if (job->curDir[0] != 0)
            snprintf(slot, kRelLen, "%s/%s", job->curDir, name);
        else
            snprintf(slot, kRelLen, "%s", name);
        job->depth++;
    }

    // then this folder's files
    job->count = kMaxNames;
    f->listDirContents(job->path,
                       FileSystem::kFiles,
                       nullptr,
                       job->names,
                       kNamesBuf,
                       kMaxNameLen,
                       job->count,
                       job->list);
    job->index = 0;
    job->open = false;
    state = State::kCopy;
}

void copyStep()
{
    FileSystem* f = fs();
    for (int chunk = 0; chunk < kChunksPerPass; chunk++)
    {
        if (!job->open)
        {
            // skip the Mac's "._" files
            while (job->index < job->count && strncmp(job->list[job->index], "._", 2) == 0)
                job->index++;
            if (job->index >= job->count)
            {
                state = State::kNextDir;
                return;
            }
            const char* name = job->list[job->index];
            join(job->path, job->srcRoot, job->curDir, name);
            join(job->path2, job->dstRoot, job->curDir, name);
            auto [okIn, in] = f->openFile(job->path, FileSystem::kRead);
            if (!okIn)
            {
                job->index++;
                continue;
            }
            auto [okOut, out] = f->openFile(job->path2, FileSystem::kWriteCreateNew);
            if (!okOut)
            {
                f->closeFile(in);
                job->index++;
                continue;
            }
            job->in = in;
            job->out = out;
            job->open = true;
        }

        size_t got = f->readFile(job->buf, kCopyBuf, job->in);
        if (got > 0)
        {
            if (f->writeFile(job->buf, got, job->out) != got)
            {
                // card full or a write error: stop this backup, remove what was made
                closeFiles();
                f->removeDirWithFiles(job->dstRoot);
                finish();
                return;
            }
            job->bytes += got;
        }
        if (got < (size_t)kCopyBuf)
        {
            closeFiles();
            job->files++;
            job->index++;
        }
    }
}

void writeInfo()
{
    FileSystem* f = fs();
    snprintf(job->path, kPath, "%s/_backup.txt", job->dstRoot);
    auto [ok, h] = f->openFile(job->path, FileSystem::kWriteCreateNew);
    if (ok)
    {
        int len = snprintf((char*)job->buf,
                           kCopyBuf,
                           "Auto Backup %04u of \"%s\"\r\nFrom: %s\r\nFirmware: %s\r\n"
                           "Files: %lu, %lu bytes%s\r\n"
                           "To restore: copy this folder into Projects, rename it, open it.\r\n",
                           job->number,
                           job->projectName,
                           job->srcRoot,
                           FW_EDITION_NAME,
                           (unsigned long)job->files,
                           (unsigned long)job->bytes,
                           job->samples ? "" : " (without the samples folder)");
        f->writeFile(job->buf, len, h);
        f->closeFile(h);
    }
    state = State::kPrune;
}

void pruneStep()
{
    uint16_t numbers[kMaxFolderNumbers];
    int n = listNumbers(numbers);
    if (n > job->keep)
    {
        // one folder per pass: the oldest
        snprintf(job->path, kPath, "%s/%04u", job->projectRoot, numbers[0]);
        if (numbers[0] != job->number) fs()->removeDirWithFiles(job->path);
        if (n - 1 > job->keep) return;
    }
    finish();
}
} // namespace

void projectSaved()
{
    // a save during a backup: that copy is incomplete, start again
    if (job != nullptr && state != State::kIdle && state != State::kPending &&
        state != State::kPrune)
    {
        closeFiles();
        fs()->removeDirWithFiles(job->dstRoot);
    }
    state = State::kPending;
}

void update()
{
    if (state == State::kIdle) return;
    if (!fileManagerIdle())
    {
        closeFiles(); // the file being copied starts again when the file manager is free
        if (job != nullptr && state == State::kCopy) job->open = false;
        return;
    }
    if (job != nullptr && state != State::kPending &&
        strncmp(job->projectName, newFileManager.getCurrentProjectName(), PROJECT_NAME_SIZE) != 0)
    {
        // another project was opened: drop the unfinished copy
        if (state != State::kPrune) fs()->removeDirWithFiles(job->dstRoot);
        finish();
        return;
    }

    switch (state)
    {
        case State::kPending: start(); break;
        case State::kNextDir: nextDir(); break;
        case State::kCopy: copyStep(); break;
        case State::kInfo: writeInfo(); break;
        case State::kPrune: pruneStep(); break;
        default: state = State::kIdle; break;
    }
}
} // namespace ceBackup

#else

namespace ceBackup
{
void projectSaved() {}
void update() {}
} // namespace ceBackup

#endif
