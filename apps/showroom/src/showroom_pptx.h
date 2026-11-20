#ifndef PANTHERA_SHOWROOM_PPTX_H
#define PANTHERA_SHOWROOM_PPTX_H

#include "showroom_model.h"

enum {
    SH_PPTX_OK = 0,
    SH_PPTX_BUILD_ERROR = 1,
    SH_PPTX_WRITE_ERROR = 2,
    SH_PPTX_BACKUP_ERROR = 3,
    SH_PPTX_COMMIT_COPY_ERROR = 4,
    SH_PPTX_RECOVERY_REQUIRED = 5,
    SH_PPTX_RECOVERY_ERROR = 6,
    SH_PPTX_OPEN_ERROR = 7,
    SH_PPTX_OPEN_INVALID = 8,
    SH_PPTX_OPEN_LIMIT = 9,
    SH_PPTX_OPEN_NOT_FOUND = 10
};

enum {
    SH_PPTX_RECOVER_NONE = 0,
    SH_PPTX_RECOVER_COMMIT_KEPT = 1,
    SH_PPTX_RECOVER_BACKUP_RESTORED = 2,
    SH_PPTX_RECOVER_FIRST_SAVE_REMOVED = 3,
    SH_PPTX_RECOVER_ERROR = -1
};

/* Save SHOWROOM.PPTX in the current directory; preserve the previous package
 * as SHOWROOM.BAK and use SHOWROOM.TMP while constructing the new archive.
 */
int sh_pptx_save(const ShModel *model);

/* Resolve an interrupted copy transaction before presenting a document. */
int sh_pptx_recover(void);

/* Import ordered slide text from a bounded PPTX package. The destination model
 * is replaced only after the complete presentation has been parsed. */
int sh_pptx_load(const char *path, ShModel *model);


#endif
