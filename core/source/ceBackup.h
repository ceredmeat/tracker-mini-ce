#ifndef CE_BACKUP_H_
#define CE_BACKUP_H_

// tracker-mini-ce: Auto Backup. After every successful project save, a dated copy of the
// project folder goes to Backups/<project name>/NNNN on the card (without export/), copied
// a little at a time from the main loop while the file manager is idle. Config -> Project
// Settings -> Auto backup sets how many copies are kept (or Off), Backup samples whether
// the samples folder is copied.

namespace ceBackup
{
void projectSaved(); // called when a project save has finished
void update();       // main loop
} // namespace ceBackup

#endif
