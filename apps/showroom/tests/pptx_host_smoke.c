#include "../src/showroom_model.h"
#include "../src/showroom_pptx.h"

#include <stdio.h>
#include <string.h>

static int copy_file(const char *source_path, const char *destination_path)
{
    unsigned char buffer[4096];
    FILE *source = fopen(source_path, "rb");
    FILE *destination;
    int ok = 1;
    if (source == NULL) return 0;
    destination = fopen(destination_path, "wb");
    if (destination == NULL) { fclose(source); return 0; }
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), source);
        if (count != 0 && fwrite(buffer, 1, count, destination) != count) { ok = 0; break; }
        if (count < sizeof(buffer)) {
            if (ferror(source)) ok = 0;
            break;
        }
    }
    if (fclose(source) != 0) ok = 0;
    if (fclose(destination) != 0) ok = 0;
    return ok;
}

static int files_equal(const char *left_path, const char *right_path)
{
    unsigned char left[4096], right[4096];
    FILE *a = fopen(left_path, "rb"), *b;
    int equal = 1;
    if (a == NULL) return 0;
    b = fopen(right_path, "rb");
    if (b == NULL) { fclose(a); return 0; }
    for (;;) {
        size_t an = fread(left, 1, sizeof(left), a), bn = fread(right, 1, sizeof(right), b);
        if (an != bn || (an && memcmp(left, right, an) != 0)) { equal = 0; break; }
        if (an < sizeof(left)) { if (ferror(a) || ferror(b)) equal = 0; break; }
    }
    if (fclose(a) != 0 || fclose(b) != 0) equal = 0;
    return equal;
}

static int write_journal(int had_current)
{
    unsigned char record[10] = {'S','H','P','T','X','J','0','1',0,0xff};
    FILE *file = fopen("SHOWROOM.JRN", "wb");
    record[8] = had_current ? 1 : 0;
    record[9] = had_current ? 0xfe : 0xff;
    if (file == NULL) return 0;
    if (fwrite(record, 1, sizeof(record), file) != sizeof(record)) { fclose(file); return 0; }
    return fclose(file) == 0;
}

static int append_commit_marker(void)
{
    unsigned char marker = 0xa5;
    FILE *file = fopen("SHOWROOM.JRN", "ab");
    if (file == NULL) return 0;
    if (fwrite(&marker, 1, 1, file) != 1) { fclose(file); return 0; }
    return fclose(file) == 0;
}

static int simulate_interrupted_copy(void)
{
    FILE *file;
    if (!copy_file("EXPECTED-CURRENT.PPTX", "SHOWROOM.TMP")) return 0;
    file = fopen("SHOWROOM.PPTX", "wb");
    if (file == NULL) return 0;
    if (fwrite("partial interrupted package", 1, 28, file) != 28 || fclose(file) != 0) return 0;
    if (!write_journal(1)) return 0;
    if (sh_pptx_recover() != SH_PPTX_RECOVER_BACKUP_RESTORED) return 0;
    if (!files_equal("SHOWROOM.PPTX", "SHOWROOM.BAK")) return 0;
    if (!copy_file("SHOWROOM.PPTX", "ROLLBACK.PPTX")) return 0;
    return sh_pptx_recover() == SH_PPTX_RECOVER_NONE;
}

static int simulate_completed_copy_before_cleanup(void)
{
    if (!copy_file("EXPECTED-CURRENT.PPTX", "SHOWROOM.TMP") ||
        !copy_file("EXPECTED-CURRENT.PPTX", "SHOWROOM.PPTX") ||
        !write_journal(1) || !append_commit_marker()) return 0;
    if (sh_pptx_recover() != SH_PPTX_RECOVER_COMMIT_KEPT) return 0;
    if (!files_equal("SHOWROOM.PPTX", "EXPECTED-CURRENT.PPTX")) return 0;
    if (!copy_file("SHOWROOM.PPTX", "RECOVERED.PPTX")) return 0;
    return sh_pptx_recover() == SH_PPTX_RECOVER_NONE;
}

static int file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    fclose(file);
    return 1;
}

static int simulate_incomplete_journal_before_commit(void)
{
    static const unsigned char partial[] = {'S','H','P','T'};
    FILE *file;
    if (!copy_file("EXPECTED-CURRENT.PPTX", "SHOWROOM.PPTX")) return 0;
    file = fopen("SHOWROOM.TMP", "wb");
    if (file == NULL) return 0;
    if (fwrite("discarded staged file", 1, 21, file) != 21 || fclose(file) != 0) return 0;
    file = fopen("SHOWROOM.JRN", "wb");
    if (file == NULL) return 0;
    if (fwrite(partial, 1, sizeof(partial), file) != sizeof(partial) || fclose(file) != 0) return 0;
    if (sh_pptx_recover() != SH_PPTX_RECOVER_NONE) return 0;
    return files_equal("SHOWROOM.PPTX", "EXPECTED-CURRENT.PPTX") && !file_exists("SHOWROOM.JRN");
}

static int simulate_interrupted_first_save(void)
{
    FILE *file;
    if (!copy_file("EXPECTED-CURRENT.PPTX", "SHOWROOM.TMP")) return 0;
    file = fopen("SHOWROOM.PPTX", "wb");
    if (file == NULL) return 0;
    if (fwrite("partial first save", 1, 18, file) != 18 || fclose(file) != 0) return 0;
    if (!write_journal(0)) return 0;
    if (sh_pptx_recover() != SH_PPTX_RECOVER_FIRST_SAVE_REMOVED) return 0;
    return !file_exists("SHOWROOM.PPTX") && sh_pptx_recover() == SH_PPTX_RECOVER_NONE;
}

static int check_import_fixture(const char *path)
{
    ShModel model;
    ShModel reopened;
    int result;
    sh_model_init(&model);
    result = sh_pptx_load(path, &model);
    if (result != SH_PPTX_OK) return 20 + result;
    if (model.count != 2 || strcmp(model.slides[0].title, "Caf\202 <Intro> &") != 0 ||
        strcmp(model.slides[0].body, "Agenda\nLine 2") != 0 ||
        strcmp(model.slides[1].title, "Close") != 0 ||
        strcmp(model.slides[1].body, "Done") != 0) return 40;
    if (!sh_model_set_title(&model, 1, "Imported & Saved") ||
        sh_pptx_save(&model) != SH_PPTX_OK) return 41;
    sh_model_init(&reopened);
    if (sh_pptx_load("SHOWROOM.PPTX", &reopened) != SH_PPTX_OK ||
        reopened.count != 2 || strcmp(reopened.slides[0].title, "Caf\202 <Intro> &") != 0 ||
        strcmp(reopened.slides[0].body, "Agenda\nLine 2") != 0 ||
        strcmp(reopened.slides[1].title, "Imported & Saved") != 0 ||
        strcmp(reopened.slides[1].body, "Done") != 0) return 42;
    puts("PASS: compressed PPTX import, relationship-based custom paths, slide order, UTF-8/CP437/XML text, and import-edit-export-reopen round trip.");
    return 0;
}

int main(int argc, char **argv)
{
    ShModel model;
    ShModel imported;
    FILE *bad;
    if (argc == 3 && strcmp(argv[1], "--import-check") == 0)
        return check_import_fixture(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--reject-check") == 0) {
        sh_model_init(&imported);
        strcpy(imported.slides[0].title, "Keep current");
        if (sh_pptx_load(argv[2], &imported) != SH_PPTX_OPEN_INVALID ||
            strcmp(imported.slides[0].title, "Keep current") != 0) return 41;
        puts("PASS: invalid/traversal package rejected without replacing the current model.");
        return 0;
    }
    sh_model_init(&model);
    if (!sh_model_set_title(&model, 0, "Caf\202 & <Title>")) return 1;
    if (!sh_model_set_body(&model, 0, "Line 1\nLine 2 & <tag>")) return 2;
    if (sh_pptx_save(&model) != SH_PPTX_OK) return 3;
    if (!sh_model_insert(&model, 1)) return 4;
    if (!sh_model_set_title(&model, 1, "Second slide")) return 5;
    if (!sh_model_set_body(&model, 1, "Backup should contain one slide.")) return 6;
    if (sh_pptx_save(&model) != SH_PPTX_OK) return 7;
    if (!copy_file("SHOWROOM.PPTX", "EXPECTED-CURRENT.PPTX") ||
        !copy_file("SHOWROOM.BAK", "EXPECTED-BACKUP.PPTX")) return 8;
    sh_model_init(&imported);
    if (sh_pptx_load("EXPECTED-CURRENT.PPTX", &imported) != SH_PPTX_OK ||
        imported.count != 2 || strcmp(imported.slides[0].title, "Caf\202 & <Title>") != 0 ||
        strcmp(imported.slides[0].body, "Line 1\nLine 2 & <tag>") != 0 ||
        strcmp(imported.slides[1].title, "Second slide") != 0) return 13;
    sh_model_init(&imported);
    bad = fopen("TRUNCATED.PPTX", "wb");
    if (bad == NULL || fwrite("PK\003\004partial", 1, 11, bad) != 11 || fclose(bad) != 0) return 15;
    if (sh_pptx_load("TRUNCATED.PPTX", &imported) != SH_PPTX_OPEN_INVALID ||
        imported.count != 1 || strcmp(imported.slides[0].title, "Untitled Presentation") != 0) return 14;
    puts("PASS: own PPTX export reopens in slide order and invalid archives leave the model intact.");
    if (!simulate_interrupted_copy()) return 9;
    if (!simulate_completed_copy_before_cleanup()) return 10;
    if (!simulate_incomplete_journal_before_commit()) return 11;
    if (!simulate_interrupted_first_save()) return 12;
    return 0;
}
