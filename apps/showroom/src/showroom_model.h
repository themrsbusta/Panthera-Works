#ifndef PANTHERA_SHOWROOM_MODEL_H
#define PANTHERA_SHOWROOM_MODEL_H

#define SH_MAX_SLIDES 99
#define SH_TITLE_BYTES 120
#define SH_BODY_BYTES 440

typedef struct {
    char title[SH_TITLE_BYTES + 1];
    char body[SH_BODY_BYTES + 1];
} ShSlide;

typedef struct {
    ShSlide slides[SH_MAX_SLIDES];
    unsigned count;
    unsigned current;
} ShModel;

void sh_model_init(ShModel *model);
int sh_model_insert(ShModel *model, unsigned position);
int sh_model_remove(ShModel *model, unsigned position);
int sh_model_set_title(ShModel *model, unsigned position, const char *text);
int sh_model_set_body(ShModel *model, unsigned position, const char *text);
const ShSlide *sh_model_get(const ShModel *model, unsigned position);

#endif
