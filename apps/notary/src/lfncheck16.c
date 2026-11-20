/*
 * Panthera LFN bootstrap check.
 * Small real-mode helper so DOSLFN's INT 21h API can be queried with
 * conventional-mode segment:offset pointers before the protected-mode editor
 * starts. This is an original probe of the documented 71A0h interface.
 */
#include <dos.h>
#include <i86.h>
#include <stdio.h>
#include <string.h>

static char g_root[4];
static unsigned char g_volume_info[32];

static int drive_has_lfn(unsigned char drive)
{
    union REGS in_regs;
    union REGS out_regs;
    struct SREGS segment_regs;

    g_root[0] = (char)drive;
    g_root[1] = ':';
    g_root[2] = '\\';
    g_root[3] = 0;
    memset(g_volume_info, 0, sizeof(g_volume_info));
    memset(&in_regs, 0, sizeof(in_regs));
    memset(&out_regs, 0, sizeof(out_regs));
    memset(&segment_regs, 0, sizeof(segment_regs));

    in_regs.w.ax = 0x71a0;
    in_regs.w.cx = sizeof(g_volume_info);
    in_regs.w.dx = FP_OFF(g_root);
    in_regs.w.di = FP_OFF(g_volume_info);
    in_regs.w.cflag = 1;
    segment_regs.ds = FP_SEG(g_root);
    segment_regs.es = FP_SEG(g_volume_info);
    intdosx(&in_regs, &out_regs, &segment_regs);
    return ((out_regs.w.cflag & 1) == 0 && (out_regs.w.bx & 0x4000) != 0);
}

int main(void)
{
    unsigned char drive;
    for (drive = 'C'; drive <= 'Z'; ++drive) {
        if (drive_has_lfn(drive)) {
            printf("LFN_API_PASS %c:\r\n", drive);
            return 0;
        }
    }
    for (drive = 'A'; drive <= 'B'; ++drive) {
        if (drive_has_lfn(drive)) {
            printf("LFN_API_PASS %c:\r\n", drive);
            return 0;
        }
    }
    puts("LFN_API_UNAVAILABLE");
    return 1;
}
