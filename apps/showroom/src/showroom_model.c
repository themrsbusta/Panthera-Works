#include "showroom_model.h"

#include <string.h>

void sh_model_init(ShModel *model)
{
    if (model == NULL) return;
    memset(model, 0, sizeof(*model));
    model->count = 1;
    strcpy(model->slides[0].title, "Untitled Presentation");
}

int sh_model_insert(ShModel *model, unsigned position)
{
    unsigned i;
    if (model == NULL || position > model->count || model->count >= SH_MAX_SLIDES) return 0;
    for (i = model->count; i > position; --i) model->slides[i] = model->slides[i - 1];
    memset(&model->slides[position], 0, sizeof(model->slides[position]));
    strcpy(model->slides[position].title, "New Slide");
    ++model->count;
    model->current = position;
    return 1;
}

int sh_model_remove(ShModel *model, unsigned position)
{
    unsigned i;
    if (model == NULL || position >= model->count || model->count <= 1) return 0;
    for (i = position; i + 1 < model->count; ++i) model->slides[i] = model->slides[i + 1];
    memset(&model->slides[model->count - 1], 0, sizeof(model->slides[0]));
    --model->count;
    if (model->current >= model->count) model->current = model->count - 1;
    return 1;
}

static int set_text(char *destination, unsigned capacity, const char *text)
{
    unsigned length;
    if (destination == NULL || text == NULL) return 0;
    length = (unsigned)strlen(text);
    if (length > capacity) return 0;
    memcpy(destination, text, length + 1);
    return 1;
}

int sh_model_set_title(ShModel *model, unsigned position, const char *text)
{
    if (model == NULL || position >= model->count) return 0;
    return set_text(model->slides[position].title, SH_TITLE_BYTES, text);
}

int sh_model_set_body(ShModel *model, unsigned position, const char *text)
{
    if (model == NULL || position >= model->count) return 0;
    return set_text(model->slides[position].body, SH_BODY_BYTES, text);
}

const ShSlide *sh_model_get(const ShModel *model, unsigned position)
{
    if (model == NULL || position >= model->count) return NULL;
    return &model->slides[position];
}
