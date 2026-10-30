/*
 * g_preload.c — preload files as script save data.
 *
 * Retail writes the strings passed to Preload between PreloadGenClear and
 * PreloadGenEnd into a "preload file" under CustomMapData, and Preloader
 * runs such a file back. Maps (Reforged Lua maps above all) use this pair as
 * their only disk storage: a FileIO library splits its data into Preload
 * strings that close the generated JASS function and open a
 * //!beginusercode block of Lua, which Preloader then runs. This keeps the
 * retail file layout so the same script reads and writes the same files.
 *
 * Ability tooltips set with BlzSetAbilityTooltip are kept here too: FileIO
 * hands the loaded payload back through an ability tooltip.
 */

#include "g_local.h"

#ifdef _WIN32
#include <direct.h>
#define preload_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define preload_mkdir(path) mkdir(path, 0775)
#endif

#define PRELOAD_MAX_BYTES (1u << 20)
#define PRELOAD_MAX_TOOLTIPS 64

static struct {
    bool active;
    char *text;
    size_t len, cap;
} preload_gen;

static struct {
    int32_t code, level;
    char *text;
} preload_tooltips[PRELOAD_MAX_TOOLTIPS];

static void preload_append(cstring_t s) {
    size_t n = strlen(s);
    if (preload_gen.len + n + 1 > PRELOAD_MAX_BYTES) return;
    if (preload_gen.len + n + 1 > preload_gen.cap) {
        size_t cap = MAX(preload_gen.cap * 2, preload_gen.len + n + 1024);
        char *grown = gi.MemAlloc((long)cap);
        if (preload_gen.text) memcpy(grown, preload_gen.text, preload_gen.len);
        SAFE_DELETE(preload_gen.text, gi.MemFree);
        preload_gen.text = grown;
        preload_gen.cap = cap;
    }
    memcpy(preload_gen.text + preload_gen.len, s, n + 1);
    preload_gen.len += n;
}

/* Map file names are relative ("Folder\\file.pld"); anything that climbs out
 * of CustomMapData or names a drive is refused. */
static bool preload_path(cstring_t filename, string_t out, uint32_t size) {
    char rel[MAX_PATHLEN];
    if (!filename || !*filename || strstr(filename, "..") || strchr(filename, ':') ||
        filename[0] == '/' || filename[0] == '\\') return false;
    snprintf(rel, sizeof(rel), "CustomMapData/%s", filename);
    for (char *c = rel; *c; c++) if (*c == '\\') *c = '/';
    gi.UserPath(rel, out, size);
    return true;
}

static void preload_mkdirs(string_t path) {
    for (char *c = path + 1; *c; c++) {
        if (*c != '/' && *c != '\\') continue;
        char saved = *c;
        *c = '\0';
        preload_mkdir(path);
        *c = saved;
    }
}

void G_PreloadGenClear(void) {
    preload_gen.len = 0;
    if (preload_gen.text) preload_gen.text[0] = '\0';
    preload_gen.active = true;
}

void G_PreloadRecord(cstring_t line) {
    if (!preload_gen.active || !line) return;
    preload_append("\tcall Preload( \"");
    preload_append(line);
    preload_append("\" )\n");
}

void G_PreloadGenEnd(cstring_t filename) {
    char path[MAX_PATHLEN];
    FILE *file;
    if (!preload_gen.active) return;
    preload_gen.active = false;
    if (!preload_path(filename, path, sizeof(path))) {
        fprintf(stderr, "PreloadGenEnd: refused file name %s\n", filename ? filename : "(null)");
        return;
    }
    preload_mkdirs(path);
    file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "PreloadGenEnd: cannot write %s\n", path);
        return;
    }
    fputs("function PreloadFiles takes nothing returns nothing\n\n\tcall PreloadStart()\n", file);
    if (preload_gen.text) fwrite(preload_gen.text, 1, preload_gen.len, file);
    fputs("\tcall PreloadEnd( 0.0 )\n\nendfunction\n\n", file);
    fclose(file);
}

/* Runs the //!beginusercode Lua block of a preload file. Files without one
 * only list assets to preload, which this engine does not model. */
void G_PreloadRun(cstring_t filename) {
    char path[MAX_PATHLEN];
    FILE *file;
    long size;
    char *text, *begin, *end;
    static cstring_t const open_tag = "//!beginusercode";
    static cstring_t const close_tag = "//!endusercode";

    if (!preload_path(filename, path, sizeof(path)) || !(file = fopen(path, "rb"))) return;
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0 || size > (long)PRELOAD_MAX_BYTES) {
        fclose(file);
        return;
    }
    text = gi.MemAlloc(size + 1);
    size = (long)fread(text, 1, (size_t)size, file);
    fclose(file);
    text[size] = '\0';
    for (begin = strstr(text, open_tag); begin; begin = strstr(end, open_tag)) {
        begin += strlen(open_tag);
        end = strstr(begin, close_tag);
        if (!end) break;
        G_LuaRunChunk(begin, (size_t)(end - begin), filename);
        end += strlen(close_tag);
    }
    gi.MemFree(text);
}

cstring_t G_AbilityTooltipOverride(int32_t code, int32_t level) {
    FOR_LOOP(i, PRELOAD_MAX_TOOLTIPS) {
        if (preload_tooltips[i].text && preload_tooltips[i].code == code && preload_tooltips[i].level == level)
            return preload_tooltips[i].text;
    }
    return NULL;
}

void G_SetAbilityTooltipOverride(int32_t code, int32_t level, cstring_t text) {
    int32_t slot = -1;
    size_t n;
    FOR_LOOP(i, PRELOAD_MAX_TOOLTIPS) {
        if (preload_tooltips[i].text && preload_tooltips[i].code == code && preload_tooltips[i].level == level) {
            slot = (int32_t)i;
            break;
        }
        if (!preload_tooltips[i].text && slot < 0) slot = (int32_t)i;
    }
    if (slot < 0) return;
    SAFE_DELETE(preload_tooltips[slot].text, gi.MemFree);
    text = text ? text : "";
    n = strlen(text);
    preload_tooltips[slot].text = gi.MemAlloc((long)n + 1);
    memcpy(preload_tooltips[slot].text, text, n + 1);
    preload_tooltips[slot].code = code;
    preload_tooltips[slot].level = level;
}

/* Tooltip changes belong to one level, like every other runtime object edit. */
void G_PreloadReset(void) {
    FOR_LOOP(i, PRELOAD_MAX_TOOLTIPS) SAFE_DELETE(preload_tooltips[i].text, gi.MemFree);
    preload_gen.active = false;
    preload_gen.len = 0;
}
