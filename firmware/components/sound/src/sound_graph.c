#include "sound_graph.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SG_TOKEN_LIMIT 8192U
#define SG_DEPTH_LIMIT 16U
#define SG_STRING_LIMIT 1024U
typedef struct { uint32_t start; uint16_t len, next, count; char type; } sg_token_t;
typedef struct {
    const char *json;
    size_t len, pos, decoded;
    uint16_t used, capacity;
    sg_token_t *tokens;
    sg_diagnostic_t *diag;
    char *scratch[2];
} sg_parser_t;

static esp_err_t sg_error(sg_diagnostic_t *d, size_t at, const char *msg)
{
    if (d) {
        d->offset = at; snprintf(d->message, sizeof(d->message), "%s", msg);
        snprintf(d->code,sizeof(d->code),"validation");
    }
    return ESP_ERR_INVALID_ARG;
}
static int sg_hex(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}
static bool sg_utf8(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        uint32_t cp = s[i++]; unsigned extra = 0;
        if (cp < 128) { continue; }
        if (cp >= 0xc2 && cp <= 0xdf) { cp &= 31; extra = 1; }
        else if (cp >= 0xe0 && cp <= 0xef) { cp &= 15; extra = 2; }
        else if (cp >= 0xf0 && cp <= 0xf4) { cp &= 7; extra = 3; }
        else { return false; }
        if (i + extra > n) { return false; }
        for (unsigned j = 0; j < extra; ++j) {
            unsigned c = s[i++]; if ((c & 0xc0) != 0x80) { return false; }
            cp = (cp << 6) | (c & 63);
        }
        if ((extra == 1 && cp < 128) || (extra == 2 && cp < 2048) ||
            (extra == 3 && cp < 65536) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) { return false; }
    }
    return true;
}
/* Decode only bounded strings, including surrogate pairs; reject embedded NUL. */
static bool sg_decode(const sg_parser_t *p, int t, char *out, size_t cap)
{
    if (t < 0 || p->tokens[t].type != 's') { return false; }
    const sg_token_t *v = &p->tokens[t]; size_t end = v->start + v->len, n = 0;
    for (size_t i = v->start; i < end;) {
        unsigned char c = (unsigned char)p->json[i++]; uint32_t cp = c;
        if (c == '\\') {
            if (i == end) { return false; }
            c = (unsigned char)p->json[i++];
            if (c == 'u') {
                cp = 0;
                if (i + 4 > end) { return false; }
                for (int j = 0; j < 4; ++j) { int h = sg_hex(p->json[i++]); if (h < 0) { return false; } cp = cp * 16 + (unsigned)h; }
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (i + 6 > end || p->json[i++] != '\\' || p->json[i++] != 'u') { return false; }
                    uint32_t low = 0;
                    for (int j = 0; j < 4; ++j) { int h = sg_hex(p->json[i++]); if (h < 0) { return false; } low = low * 16 + (unsigned)h; }
                    if (low < 0xdc00 || low > 0xdfff) { return false; }
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                } else if (cp >= 0xdc00 && cp <= 0xdfff) { return false; }
            } else {
                const char *esc = "\"\\/bfnrt", *values = "\"\\/\b\f\n\r\t";
                const char *e = strchr(esc, c); if (!e) { return false; } cp = (unsigned char)values[e - esc];
            }
        } else if (c < 32) { return false; }
        if (!cp) { return false; }
        unsigned char bytes[4]; size_t k;
        if (cp < 128 || c != 'u') { bytes[0] = (unsigned char)cp; k = 1; }
        else if (cp < 2048) { bytes[0] = 0xc0 | (cp >> 6); bytes[1] = 0x80 | (cp & 63); k = 2; }
        else if (cp < 65536) { bytes[0] = 0xe0 | (cp >> 12); bytes[1] = 0x80 | ((cp >> 6) & 63); bytes[2] = 0x80 | (cp & 63); k = 3; }
        else { bytes[0] = 0xf0 | (cp >> 18); bytes[1] = 0x80 | ((cp >> 12) & 63); bytes[2] = 0x80 | ((cp >> 6) & 63); bytes[3] = 0x80 | (cp & 63); k = 4; }
        if (n + k >= cap) { return false; } memcpy(out + n, bytes, k); n += k;
    }
    out[n] = 0; return sg_utf8((const unsigned char *)out, n);
}
static void sg_ws(sg_parser_t *p)
{
    while (p->pos < p->len && strchr(" \t\r\n", p->json[p->pos])) { ++p->pos; }
}
static int sg_value(sg_parser_t *p, unsigned depth)
{
    sg_ws(p);
    if (depth > SG_DEPTH_LIMIT || p->pos >= p->len || p->used >= p->capacity) { return -1; }
    int t = p->used++; sg_token_t *v = &p->tokens[t]; v->start = (uint32_t)p->pos;
    char c = p->json[p->pos++]; v->type = c;
    if (c == '{' || c == '[') {
        sg_ws(p); char close = c == '{' ? '}' : ']';
        if (p->pos < p->len && p->json[p->pos] == close) { ++p->pos; }
        else for (;;) {
            int key = -1;
            if (c == '{') {
                key = sg_value(p, depth + 1); char *a = p->scratch[0], *b = p->scratch[1];
                if (key < 0 || !sg_decode(p, key, a, SG_STRING_LIMIT + 1)) { return -1; }
                for (int j = t + 1; j < key; j = p->tokens[j + 1].next) {
                    if (!sg_decode(p, j, b, SG_STRING_LIMIT + 1) || !strcmp(a, b)) { return -1; }
                }
                sg_ws(p); if (p->pos == p->len || p->json[p->pos++] != ':') { return -1; }
            }
            if (sg_value(p, depth + 1) < 0) { return -1; } ++v->count;
            sg_ws(p); if (p->pos == p->len) { return -1; }
            char sep = p->json[p->pos++]; if (sep == close) { break; } if (sep != ',') { return -1; }
        }
    } else if (c == '"') {
        v->type = 's'; v->start = (uint32_t)p->pos;
        bool escape = false, closed = false;
        while (p->pos < p->len) {
            char ch = p->json[p->pos++];
            if (ch == '"' && !escape) { closed = true; break; }
            if (ch == '\\' && !escape) { escape = true; } else { escape = false; }
        }
        size_t raw = p->pos - v->start - 1;
        if (!closed || raw > SG_STRING_LIMIT * 6U) { return -1; } v->len = (uint16_t)raw;
        char *decoded = p->scratch[0]; if (!sg_decode(p, t, decoded, SG_STRING_LIMIT + 1)) { return -1; }
        p->decoded += strlen(decoded); if (p->decoded > SG_MAX_JSON) { return -1; }
    } else if (c == 't' || c == 'f' || c == 'n') {
        const char *word = c == 't' ? "true" : c == 'f' ? "false" : "null";
        size_t n = strlen(word); if (v->start + n > p->len || memcmp(p->json + v->start, word, n)) { return -1; } p->pos = v->start + n;
    } else {
        v->type = 'd'; size_t i = v->start;
        if (p->json[i] == '-') { ++i; } if (i == p->len) { return -1; }
        if (p->json[i] == '0') { ++i; } else {
            if (p->json[i] < '1' || p->json[i] > '9') { return -1; }
            while (i < p->len && p->json[i] >= '0' && p->json[i] <= '9') { ++i; }
        }
        if (i < p->len && p->json[i] == '.') {
            size_t first = ++i; while (i < p->len && p->json[i] >= '0' && p->json[i] <= '9') { ++i; } if (i == first) { return -1; }
        }
        if (i < p->len && (p->json[i] == 'e' || p->json[i] == 'E')) {
            ++i; if (i < p->len && (p->json[i] == '+' || p->json[i] == '-')) { ++i; }
            size_t first = i; while (i < p->len && p->json[i] >= '0' && p->json[i] <= '9') { ++i; } if (i == first) { return -1; }
        }
        char num[128]; size_t n = i - v->start; if (n >= sizeof(num)) { return -1; }
        memcpy(num, p->json + v->start, n); num[n] = 0; if (!isfinite(strtod(num, NULL))) { return -1; }
        p->pos = i;
    }
    if (v->type != 's' && v->type != '{' && v->type != '[') { v->len = (uint16_t)(p->pos - v->start); }
    v->next = p->used; return t;
}
static int sg_get(const sg_parser_t *p, int obj, const char *key)
{
    if (obj < 0 || p->tokens[obj].type != '{') { return -1; }
    for (int i = obj + 1; i < p->tokens[obj].next; i = p->tokens[i + 1].next) {
        char name[SG_STRING_LIMIT + 1]; if (sg_decode(p, i, name, sizeof(name)) && !strcmp(name, key)) { return i + 1; }
    }
    return -1;
}
static bool sg_keys(const sg_parser_t *p, int obj, const char *allowed)
{
    if (obj < 0 || p->tokens[obj].type != '{') { return false; }
    for (int i = obj + 1; i < p->tokens[obj].next; i = p->tokens[i + 1].next) {
        char key[SG_STRING_LIMIT + 1], needle[SG_STRING_LIMIT + 3];
        if (!sg_decode(p, i, key, sizeof(key)) || strchr(key,'|')) { return false; }
        snprintf(needle, sizeof(needle), "|%s|", key); if (!strstr(allowed, needle)) { return false; }
    }
    return true;
}
static bool sg_uint(const sg_parser_t *p, int t, uint32_t max, uint32_t *out)
{
    if (t < 0 || p->tokens[t].type != 'd') { return false; }
    const sg_token_t *v = &p->tokens[t]; char text[128];
    if (v->len >= sizeof(text)) { return false; }
    memcpy(text,p->json + v->start,v->len); text[v->len] = 0;
    double value = strtod(text,NULL);
    if (!isfinite(value) || value < 0 || value > max) { return false; }
    uint32_t n = (uint32_t)value; if (value != (double)n) { return false; }
    *out = n; return true;
}
static bool sg_boolean(const sg_parser_t *p, int t, bool *out)
{
    if (t < 0 || (p->tokens[t].type != 't' && p->tokens[t].type != 'f')) { return false; } *out = p->tokens[t].type == 't'; return true;
}
static int sg_enum(const char *s, const char *const *names, int count)
{
    for (int i = 0; i < count; ++i) { if (!strcmp(s, names[i])) { return i; } } return -1;
}
esp_err_t sg_parse(const char *json, size_t len, sg_graph_t *out, sg_diagnostic_t *diag)
{
    if (diag) { memset(diag, 0, sizeof(*diag)); }
    if (!json || !out || !len || len > SG_MAX_JSON || memchr(json, 0, len)) { return sg_error(diag, 0, "invalid JSON size/input"); }
    sg_parser_t p = {0}; p.json = json; p.len = len; p.diag = diag;
    p.capacity = (uint16_t)(len / 2 + 1 < SG_TOKEN_LIMIT ? len / 2 + 1 : SG_TOKEN_LIMIT);
    p.tokens = calloc(1, p.capacity * sizeof(*p.tokens) + 2 * (SG_STRING_LIMIT + 1)); if (!p.tokens) { return ESP_ERR_NO_MEM; }
    p.scratch[0] = (char *)(p.tokens + p.capacity); p.scratch[1] = p.scratch[0] + SG_STRING_LIMIT + 1;
    esp_err_t result = ESP_ERR_INVALID_ARG; uint32_t n; char text[64]; int obj, arr;
    if (sg_value(&p, 0) != 0) { goto fail; } sg_ws(&p); if (p.pos != len) { goto fail; }
    memset(out, 0, sizeof(*out));
#define SG_STR(o,k,d) do { if (!sg_decode(&p, sg_get(&p,o,k), d, sizeof(d))) { goto fail; } } while (0)
#define SG_NUM(o,k,d,m) do { if (!sg_uint(&p, sg_get(&p,o,k), m, &n)) { goto fail; } d = n; } while (0)
    if (!sg_keys(&p, 0, "|format||schemaVersion||id||name||engine||hysteresis||states||transitions||effects||assets||editor|")) { goto fail; }
    SG_STR(0, "format", text); if (strcmp(text, "sound-graph")) { goto fail; }
    SG_NUM(0, "schemaVersion", n, 1); if (n != 1) { goto fail; }
    SG_STR(0,"id",out->id); SG_STR(0,"name",out->name); SG_NUM(0,"hysteresis",out->hysteresis,32);
    obj = sg_get(&p,0,"engine"); if (!sg_keys(&p,obj,"|entry||fn|")) { goto fail; }
    SG_STR(obj,"entry",out->engine_entry); SG_NUM(obj,"fn",out->engine_fn,28);
#define SG_ARRAY(k,max,dest) do { arr = sg_get(&p,0,k); if (arr < 0 || p.tokens[arr].type != '[' || p.tokens[arr].count > max) { goto fail; } dest = (uint8_t)p.tokens[arr].count; } while (0)
    SG_ARRAY("states",SG_MAX_STATES,out->state_count); obj = arr + 1;
    for (unsigned i = 0; i < out->state_count; ++i, obj = p.tokens[obj].next) {
        sg_state_t *s = &out->states[i]; if (!sg_keys(&p,obj,"|id||name||file||loop||volume||rate|")) { goto fail; }
        SG_STR(obj,"id",s->id); SG_STR(obj,"name",s->name); SG_STR(obj,"file",s->file);
        if (!sg_boolean(&p,sg_get(&p,obj,"loop"),&s->loop)) { goto fail; }
        SG_NUM(obj,"volume",s->volume,100); SG_NUM(obj,"rate",s->rate,3000);
    }
    SG_ARRAY("transitions",SG_MAX_TRANSITIONS,out->transition_count); obj = arr + 1;
    for (unsigned i = 0; i < out->transition_count; ++i, obj = p.tokens[obj].next) {
        sg_transition_t *e = &out->transitions[i]; if (!sg_keys(&p,obj,"|id||source||target||priority||timing||condition|")) { goto fail; }
        SG_STR(obj,"id",e->id); SG_STR(obj,"source",e->source); SG_STR(obj,"target",e->target); SG_NUM(obj,"priority",e->priority,255);
        SG_STR(obj,"timing",text); const char *const timing[] = {"immediate","after_sample"}; int v = sg_enum(text,timing,2); if (v < 0) { goto fail; } e->timing = (sg_timing_t)v;
        int cond = sg_get(&p,obj,"condition"); if (!sg_keys(&p,cond,"|type||fn||min||max|")) { goto fail; }
        SG_STR(cond,"type",text); const char *const types[] = {"fn_press","fn_release","fn_on","fn_off","engine_on","engine_off","speed","accel","decel","sample_done"};
        v = sg_enum(text,types,10); if (v < 0) { goto fail; } e->condition.type = (sg_condition_type_t)v;
        int f = sg_get(&p,cond,"fn"), lo = sg_get(&p,cond,"min"), hi = sg_get(&p,cond,"max");
        e->condition.has_fn = f >= 0; e->condition.has_min = lo >= 0; e->condition.has_max = hi >= 0;
        if (f >= 0) { SG_NUM(cond,"fn",e->condition.fn,28); }
        if (lo >= 0) { SG_NUM(cond,"min",e->condition.min,255); }
        if (hi >= 0) { SG_NUM(cond,"max",e->condition.max,255); }
    }
    SG_ARRAY("effects",SG_MAX_EFFECTS,out->effect_count); obj = arr + 1;
    for (unsigned i = 0; i < out->effect_count; ++i, obj = p.tokens[obj].next) {
        sg_effect_t *e = &out->effects[i]; if (!sg_keys(&p,obj,"|id||entry||fn|")) { goto fail; }
        SG_STR(obj,"id",e->id); SG_STR(obj,"entry",e->entry); SG_NUM(obj,"fn",e->fn,28);
    }
    SG_ARRAY("assets",SG_MAX_ASSETS,out->asset_count); obj = arr + 1;
    for (unsigned i = 0; i < out->asset_count; ++i, obj = p.tokens[obj].next) {
        sg_asset_t *a = &out->assets[i]; if (!sg_keys(&p,obj,"|file||size||crc32||sampleRate||channels||bits||durationMs|")) { goto fail; }
        SG_STR(obj,"file",a->file); SG_STR(obj,"crc32",a->crc32);
        SG_NUM(obj,"size",a->size,UINT32_MAX); SG_NUM(obj,"sampleRate",a->sampleRate,UINT32_MAX);
        SG_NUM(obj,"channels",a->channels,UINT32_MAX); SG_NUM(obj,"bits",a->bits,UINT32_MAX); SG_NUM(obj,"durationMs",a->durationMs,UINT32_MAX);
    }
    obj = sg_get(&p,0,"editor"); if (obj < 0 || p.tokens[obj].type != '{') { goto fail; }
    result = sg_validate(out,diag); free(p.tokens); return result;
fail:
    result = sg_error(diag,p.pos,"invalid, duplicate, over-limit or missing JSON field"); free(p.tokens); memset(out,0,sizeof(*out)); return result;
#undef SG_STR
#undef SG_NUM
#undef SG_ARRAY
}
bool sg_id_valid(const char *id)
{
    if (!id) { return false; } size_t n = 0;
    while (n < SG_ID_CAP && id[n]) {
        char c = id[n++]; if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) { return false; }
    }
    return n > 0 && n < SG_ID_CAP;
}
bool sg_filename_valid(const char *file)
{
    if (!file) { return false; } size_t n = 0;
    while (n < SG_FILE_CAP && file[n]) { ++n; }
    if (n < 5 || n >= SG_FILE_CAP || strcmp(file + n - 4,".wav")) { return false; }
    for (size_t i = 0; i < n - 4; ++i) {
        char c = file[i]; if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) { return false; }
    }
    return true;
}
static bool sg_name_valid(const char *s)
{
    const char *end = memchr(s,0,SG_NAME_CAP); return end && sg_utf8((const unsigned char *)s,(size_t)(end-s));
}
static int sg_state_index(const sg_graph_t *g, const char *id)
{
    if (!sg_id_valid(id)) { return -1; }
    for (unsigned i = 0; i < g->state_count; ++i) { if (!strcmp(g->states[i].id,id)) { return (int)i; } } return -1;
}
esp_err_t sg_validate(sg_graph_t *g, sg_diagnostic_t *diag)
{
#define SG_CHECK(c,m) do { if (!(c)) { return sg_error(diag,0,m); } } while (0)
    if (diag) { memset(diag,0,sizeof(*diag)); }
    SG_CHECK(g,"null graph");
    SG_CHECK(g->state_count > 0 && g->state_count <= SG_MAX_STATES && g->transition_count <= SG_MAX_TRANSITIONS && g->effect_count <= SG_MAX_EFFECTS && g->asset_count <= SG_MAX_ASSETS,"graph capacity");
    SG_CHECK(sg_id_valid(g->id) && sg_name_valid(g->name) && g->engine_fn <= 28 && g->hysteresis <= 32,"invalid graph metadata");
    unsigned audible = 0, silent = 0;
    for (unsigned i = 0; i < g->state_count; ++i) {
        if (diag) { snprintf(diag->id,sizeof(diag->id),"%.31s",g->states[i].id); snprintf(diag->field,sizeof(diag->field),"states"); }
        sg_state_t *s = &g->states[i]; SG_CHECK(sg_id_valid(s->id) && sg_name_valid(s->name) && s->volume <= 100 && s->rate >= 500 && s->rate <= 3000,"invalid state");
        if (s->file[0]) { SG_CHECK(sg_filename_valid(s->file),"invalid WAV basename"); ++audible; } else { ++silent; }
        for (unsigned j = 0; j < i; ++j) { SG_CHECK(strcmp(s->id,g->states[j].id),"duplicate state ID"); }
    }
    SG_CHECK(audible <= 31 && silent <= 26,"audible/silent state capacity");
    int entry = sg_state_index(g,g->engine_entry); SG_CHECK(entry >= 0 && !g->states[entry].file[0],"engine entry must be silent"); g->engine_entry_index = (uint8_t)entry;
    for (unsigned i = 0; i < g->asset_count; ++i) {
        if (diag) { diag->id[0] = 0; snprintf(diag->field,sizeof(diag->field),"assets"); }
        sg_asset_t *a = &g->assets[i]; SG_CHECK(sg_filename_valid(a->file) && memchr(a->crc32,0,sizeof(a->crc32)) && strlen(a->crc32) == 8,"invalid asset metadata");
        for (unsigned j = 0; j < 8; ++j) { SG_CHECK(sg_hex(a->crc32[j]) >= 0,"invalid CRC32"); }
        SG_CHECK(a->size > 0 && a->sampleRate > 0 && a->channels > 0 && a->bits > 0,"invalid asset dimensions");
        for (unsigned j = 0; j < i; ++j) { SG_CHECK(strcmp(a->file,g->assets[j].file),"duplicate asset"); }
    }
    for (unsigned i = 0; i < g->effect_count; ++i) {
        if (diag) { snprintf(diag->id,sizeof(diag->id),"%.31s",g->effects[i].id); snprintf(diag->field,sizeof(diag->field),"effects"); }
        sg_effect_t *e = &g->effects[i]; SG_CHECK(sg_id_valid(e->id) && e->fn <= 28,"invalid effect");
        entry = sg_state_index(g,e->entry); SG_CHECK(entry >= 0 && !g->states[entry].file[0],"effect entry must be silent"); e->entry_index = (uint8_t)entry;
        for (unsigned j = 0; j < i; ++j) { SG_CHECK(strcmp(e->id,g->effects[j].id),"duplicate effect ID"); }
    }
    for (unsigned i = 0; i < g->transition_count; ++i) {
        if (diag) { snprintf(diag->id,sizeof(diag->id),"%.31s",g->transitions[i].id); snprintf(diag->field,sizeof(diag->field),"transitions"); }
        sg_transition_t *e = &g->transitions[i]; sg_condition_t *c = &e->condition;
        SG_CHECK(sg_id_valid(e->id) && (e->timing == SG_IMMEDIATE || e->timing == SG_AFTER_SAMPLE),"invalid transition");
        int src = sg_state_index(g,e->source), dst = sg_state_index(g,e->target); SG_CHECK(src >= 0 && dst >= 0,"dangling transition"); e->source_index = (uint8_t)src; e->target_index = (uint8_t)dst;
        SG_CHECK(c->type >= SG_FN_PRESS && c->type <= SG_SAMPLE_DONE,"unknown condition");
        bool fn = c->type <= SG_FN_OFF, range = c->type >= SG_SPEED && c->type <= SG_DECEL;
        SG_CHECK(c->has_fn == fn && (!fn || c->fn <= 28),"condition fn mismatch");
        SG_CHECK(range || (!c->has_min && !c->has_max),"unexpected condition range");
        SG_CHECK(!range || (c->has_min || c->has_max),"missing condition range");
        SG_CHECK(!c->has_min || !c->has_max || c->min <= c->max,"inverted condition range");
        for (unsigned j = 0; j < i; ++j) {
            SG_CHECK(strcmp(e->id,g->transitions[j].id),"duplicate transition ID");
            SG_CHECK(src != g->transitions[j].source_index || e->priority != g->transitions[j].priority,"duplicate source priority");
        }
    }
    /* A silent state supplies no audio boundary. Persistent immediate guards
     * must not form a cycle; edge events and next-tick sample_done are temporal. */
    uint64_t immediate[SG_MAX_STATES] = {0};
    for (unsigned i = 0; i < g->transition_count; ++i) {
        const sg_transition_t *e = &g->transitions[i];
        if (e->timing == SG_IMMEDIATE && e->condition.type >= SG_FN_ON &&
            e->condition.type != SG_SAMPLE_DONE && !g->states[e->source_index].file[0] && !g->states[e->target_index].file[0]) {
            immediate[e->source_index] |= UINT64_C(1) << e->target_index;
        }
    }
    for (unsigned k = 0; k < g->state_count; ++k) {
        for (unsigned i = 0; i < g->state_count; ++i) {
            if (immediate[i] & (UINT64_C(1) << k)) { immediate[i] |= immediate[k]; }
        }
    }
    for (unsigned i = 0; i < g->state_count; ++i) {
        if (diag) { snprintf(diag->id,sizeof(diag->id),"%s",g->states[i].id); snprintf(diag->field,sizeof(diag->field),"transitions"); }
        SG_CHECK(!(immediate[i] & (UINT64_C(1) << i)),"immediate silent cycle");
    }
    uint64_t reached = UINT64_C(1) << g->engine_entry_index;
    for (unsigned i = 0; i < g->effect_count; ++i) { reached |= UINT64_C(1) << g->effects[i].entry_index; }
    for (unsigned pass = 0; pass < g->state_count; ++pass) {
        uint64_t before = reached;
        for (unsigned i = 0; i < g->transition_count; ++i) {
            const sg_transition_t *e = &g->transitions[i];
            if (reached & (UINT64_C(1) << e->source_index)) { reached |= UINT64_C(1) << e->target_index; }
        }
        if (before == reached) { break; }
    }
    for (unsigned i = 0; i < g->state_count; ++i) {
        if (diag) { snprintf(diag->id,sizeof(diag->id),"%s",g->states[i].id); snprintf(diag->field,sizeof(diag->field),"states"); }
        SG_CHECK(reached & (UINT64_C(1) << i),"unreachable state");
    }
    if (diag) { memset(diag,0,sizeof(*diag)); } return ESP_OK;
#undef SG_CHECK
}
bool sg_file_used(const sg_graph_t *g, const char *file)
{
    if (!g || !file || !file[0] || g->state_count > SG_MAX_STATES) { return false; }
    for (unsigned i = 0; i < g->state_count; ++i) {
        if (memchr(g->states[i].file,0,SG_FILE_CAP) && !strcmp(g->states[i].file,file)) { return true; }
    }
    return false;
}
