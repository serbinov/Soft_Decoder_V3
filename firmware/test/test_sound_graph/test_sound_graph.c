#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
/* Pre-include every header sound.c uses so the white-box `#define static`
 * below only affects the component translation unit, not header inline code. */
#include "audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "motor.h"
#include "settings.h"
#include "storage.h"
#include "sound.h"
#include "../../components/sound/src/sound_graph.c"
#include "../../components/sound/src/sound_graph_runner.c"
#define static
#include "../../components/sound/src/sound.c"
#undef static
#include "../../test_libs/teststubs/stubs.c"

/* Minimal audio/settings/motor/storage stand-ins for the sound.c white-box
 * tests. The semantic runner tests use their own sg_io mock below. */
static audio_completion_t s_audio_completion;
static audio_voice_state_t s_audio_voice_state;
static unsigned s_audio_play_calls;
static unsigned s_audio_release_calls;

esp_err_t audio_voice_play_generation(uint8_t voice, const char *path, bool loop,
                                      uint8_t volume, audio_voice_handle_t *out)
{
    (void)path; (void)loop; (void)volume;
    ++s_audio_play_calls;
    s_audio_completion = AUDIO_COMPLETION_NONE;
    s_audio_voice_state = AUDIO_VOICE_PENDING;
    if (out != NULL) { *out = (audio_voice_handle_t){voice, s_audio_play_calls}; }
    return ESP_OK;
}
esp_err_t audio_voice_alloc_owned(audio_voice_handle_t *out)
{
    if (out == NULL) { return ESP_ERR_INVALID_ARG; }
    *out = (audio_voice_handle_t){0, ++s_audio_play_calls};
    return ESP_OK;
}
esp_err_t audio_voice_play_owned(audio_voice_handle_t *handle, const char *path,
                                 bool loop, uint8_t volume)
{
    (void)path; (void)loop; (void)volume;
    s_audio_completion = AUDIO_COMPLETION_NONE;
    s_audio_voice_state = AUDIO_VOICE_PENDING;
    if (handle != NULL) { handle->generation = ++s_audio_play_calls; }
    return ESP_OK;
}
void audio_voice_release_owned(audio_voice_handle_t handle)
{
    (void)handle;
    ++s_audio_release_calls;
}
esp_err_t audio_voice_set_rate_owned(audio_voice_handle_t handle, uint16_t permille)
{
    (void)handle; (void)permille;
    return ESP_OK;
}
audio_voice_state_t audio_voice_get_state(audio_voice_handle_t handle)
{
    (void)handle;
    return s_audio_voice_state;
}
audio_completion_t audio_voice_completion(audio_voice_handle_t handle)
{
    (void)handle;
    return s_audio_completion;
}
esp_err_t audio_voice_set_volume_live(uint8_t voice, uint8_t volume)
{
    (void)voice; (void)volume;
    return ESP_OK;
}
esp_err_t settings_load(settings_config_t *cfg)
{
    if (cfg != NULL) { memset(cfg, 0, sizeof(*cfg)); }
    return ESP_OK;
}
void motor_get_applied_speed(uint8_t *speed, bool *forward)
{
    if (speed != NULL) { *speed = 0; }
    if (forward != NULL) { *forward = true; }
}
const char *storage_get_root(void) { return "."; }

static sg_graph_t *g;
static sg_runner_t *r;
static sg_diagnostic_t diagnostic;
static unsigned plays, releases;
static sg_audio_state_t audio_state[25];
static bool fail_play;
static bool no_voice_once;
static const char *minimal =
    "{\"format\":\"sound-graph\",\"schemaVersion\":1,\"id\":\"test\",\"name\":\"Test\","
    "\"engine\":{\"entry\":\"off\",\"fn\":1},\"hysteresis\":2,"
    "\"states\":[{\"id\":\"off\",\"name\":\"Off\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000}],"
    "\"transitions\":[],\"effects\":[],\"assets\":[],\"editor\":{\"positions\":{},\"viewport\":{\"x\":-1.5,\"zoom\":1e0}}}";

static esp_err_t play(void *ctx, bool engine, const sg_state_t *state, sg_handle_t *h)
{
    (void)ctx; (void)state; if (fail_play) { return ESP_FAIL; }
    if (no_voice_once) { no_voice_once = false; return ESP_ERR_NO_MEM; }
    h->voice = engine ? 18 : (uint8_t)(plays % 18); h->generation = ++plays;
    audio_state[h->voice] = SG_AUDIO_PENDING; return ESP_OK;
}
static void release(void *ctx, sg_handle_t h) { (void)ctx; (void)h; ++releases; }
static sg_audio_state_t poll(void *ctx, sg_handle_t h) { (void)ctx; return audio_state[h.voice]; }
static void init_runner(void)
{
    TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    sg_io_t io = {play,release,poll,NULL}; sg_runner_init(r,g,&io);
}
void setUp(void)
{
    sound_graph_deactivate();
    memset(s_fn_state, 0, sizeof(s_fn_state));
    s_audio_completion = AUDIO_COMPLETION_NONE;
    s_audio_voice_state = AUDIO_VOICE_FINISHED;
    s_audio_play_calls = s_audio_release_calls = 0;
    g = calloc(1,sizeof(*g)); r = calloc(1,sizeof(*r));
    TEST_ASSERT_NOT_NULL(g); TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(ESP_OK,sg_parse(minimal,strlen(minimal),g,&diagnostic));
    plays = releases = 0; fail_play = false; no_voice_once = false; memset(audio_state,0,sizeof(audio_state));
}
void tearDown(void) { sound_graph_deactivate(); free(r); free(g); }
static unsigned state(const char *id, const char *file, bool loop)
{
    unsigned i = g->state_count++; sg_state_t *s = &g->states[i];
    snprintf(s->id,sizeof(s->id),"%s",id); snprintf(s->name,sizeof(s->name),"%s",id);
    snprintf(s->file,sizeof(s->file),"%s",file); s->loop = loop; s->volume = 75; s->rate = 1200; return i;
}
static unsigned edge(const char *src, const char *dst, unsigned priority, sg_condition_type_t type, unsigned fn, sg_timing_t timing)
{
    unsigned i = g->transition_count++; sg_transition_t *e = &g->transitions[i];
    snprintf(e->id,sizeof(e->id),"e%u",i); snprintf(e->source,sizeof(e->source),"%s",src); snprintf(e->target,sizeof(e->target),"%s",dst);
    e->priority = (uint8_t)priority; e->timing = timing; e->condition.type = type;
    e->condition.has_fn = type <= SG_FN_OFF; e->condition.fn = (uint8_t)fn; return i;
}
static void reject_editor(const char *editor)
{
    char *json = malloc(SG_MAX_JSON + 1); TEST_ASSERT_NOT_NULL(json);
    const char *at = strstr(minimal,"\"editor\":"); size_t prefix = (size_t)(at-minimal);
    memcpy(json,minimal,prefix); snprintf(json+prefix,SG_MAX_JSON-prefix,"\"editor\":%s}",editor);
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_parse(json,strlen(json),g,&diagnostic)); free(json);
}
static void accept_editor(const char *editor)
{
    char *json = malloc(SG_MAX_JSON + 1); TEST_ASSERT_NOT_NULL(json);
    const char *at = strstr(minimal,"\"editor\":"); size_t prefix = (size_t)(at-minimal);
    memcpy(json,minimal,prefix); snprintf(json+prefix,SG_MAX_JSON-prefix,"\"editor\":%s}",editor);
    TEST_ASSERT_EQUAL(ESP_OK,sg_parse(json,strlen(json),g,&diagnostic)); free(json);
}
static void reject_json(const char *json, size_t len)
{
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_parse(json, len, g, &diagnostic));
}
static void reset_model(void)
{
    memset(g, 0, sizeof(*g));
    TEST_ASSERT_EQUAL(ESP_OK, sg_parse(minimal, strlen(minimal), g, &diagnostic));
}
static void add_asset(const char *file, const char *crc, uint32_t size)
{
    sg_asset_t *a = &g->assets[g->asset_count++];
    snprintf(a->file, sizeof(a->file), "%s", file);
    snprintf(a->crc32, sizeof(a->crc32), "%s", crc);
    a->size = size; a->sampleRate = 8000; a->channels = 1; a->bits = 16;
}
static void add_effect(const char *id, const char *entry, uint8_t fn)
{
    sg_effect_t *e = &g->effects[g->effect_count++];
    snprintf(e->id, sizeof(e->id), "%s", id);
    snprintf(e->entry, sizeof(e->entry), "%s", entry);
    e->fn = fn;
}
static void test_parse_valid_token_edges(void)
{
    /* Uppercase/lowercase hex, surrogate pair, number forms, literals/arrays. */
    accept_editor("{\"n\":-1.5e+2,\"m\":1E-2,\"u\":\"\\u00E9\\u00e9\\u20AC\\ud83d\\ude00\","
                  "\"l\":[true,false,null],\"o\":{\"k\":1.0}}");
    /* Raw 2/3/4-byte UTF-8 and all JSON escapes. */
    accept_editor("{\"s\":\"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\","
                  "\"e\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"}");
}
static void test_parse_rejects_malformed_token_edges(void)
{
    const char *bad[] = {
        "", "{", "[]", "{1:2}", "{\"a\" 1}", "{\"a\":1} x", "{\"a\":1,}",
        "{\"a\":-}", "{\"a\":1.}", "{\"a\":1e}", "{\"a\":1e+}", "{\"a\":+1}",
        "{\"a\":tru}", "{\"a\":fals}", "{\"a\":nul}",
        "{\"a\":\"\\q\"}", "{\"a\":\"\\u12\"}", "{\"a\":\"\\uZZZZ\"}",
        "{\"a\":\"\\ud800\"}", "{\"a\":\"\\ud800\\u0041\"}", "{\"a\":\"\\udc00\"}",
        "{\"a\":\"\xC0\xAF\"}", "{\"a\":\"\xE2\x28\xA1\"}", "{\"a\":\"\xF0\x80\x80\xAF\"}",
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        reject_json(bad[i], strlen(bad[i]));
    }
    const char ctrl[] = { '{','"','a','"',':','"',1,'"','}',0 };
    reject_json(ctrl, sizeof(ctrl) - 1);
}
static void test_validator_error_matrix(void)
{
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(NULL, &diagnostic));

    reset_model(); g->state_count = 0;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); g->state_count = SG_MAX_STATES + 1;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); g->transition_count = SG_MAX_TRANSITIONS + 1;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); g->effect_count = SG_MAX_EFFECTS + 1;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); g->asset_count = SG_MAX_ASSETS + 1;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); g->variable_count = SG_MAX_VARIABLES + 1;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));

    reset_model(); snprintf(g->id, sizeof(g->id), " bad");
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); snprintf(g->name, sizeof(g->name), "\xC0");
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); g->engine_fn = 29;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); g->hysteresis = 33;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));

    reset_model(); g->states[0].file[0] = 'x';
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic)); /* engine entry must be silent */

    reset_model(); state("run", "run.wav", true); g->states[1].volume = 101;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); state("run", "run.wav", true); g->states[1].rate = 499;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); state("run", "run.wav", true); g->states[1].id[0] = ' ';
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); state("run", "run.wav", true); state("run", "x.wav", false);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic)); /* duplicate ID */
    reset_model(); state("run", "../x.wav", false);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic)); /* bad basename */

    reset_model(); g->variable_count = 2;
    snprintf(g->variables[0].name, sizeof(g->variables[0].name), "v");
    snprintf(g->variables[1].name, sizeof(g->variables[1].name), "v");
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); g->variable_count = 1; g->variables[0].kind = SG_VAR_ENGINE;
    g->variables[0].init = 1;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); g->variable_count = 1; snprintf(g->variables[0].name, sizeof(g->variables[0].name), "v");
    g->variables[0].kind = (sg_var_kind_t)(SG_VAR_ENGINE + 1);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));

    reset_model(); add_asset("run.wav", "00000000", 1);
    TEST_ASSERT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); add_asset("run.wav", "0000000G", 1);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); add_asset("run.wav", "00000000", 0);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); add_asset("run.wav", "00000000", 1); add_asset("run.wav", "00000001", 1);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));

    reset_model(); add_effect("fx", "off", 1);
    TEST_ASSERT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); add_effect("fx", "off", 29);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); add_effect(" bad", "off", 1);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); state("run", "run.wav", true); add_effect("fx", "run", 1);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); add_effect("fx", "off", 1); add_effect("fx", "off", 2);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));

    reset_model(); state("run", "run.wav", true); edge("off", "run", 1, SG_ENGINE_ON, 0, SG_IMMEDIATE);
    TEST_ASSERT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); state("run", "run.wav", true); edge("off", "run", 1, SG_ENGINE_ON, 0, (sg_timing_t)99);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); state("run", "run.wav", true); edge("off", "run", 1, (sg_condition_type_t)99, 0, SG_IMMEDIATE);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); state("run", "run.wav", true); edge("off", "run", 1, SG_ENGINE_ON, 0, SG_IMMEDIATE);
    g->transitions[0].condition.has_fn = true;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); state("run", "run.wav", true); edge("off", "run", 1, SG_SPEED, 0, SG_IMMEDIATE);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic)); /* missing range */
    reset_model(); state("run", "run.wav", true); edge("off", "run", 1, SG_RANDOM, 0, SG_IMMEDIATE);
    g->transitions[0].condition.has_min = true; g->transitions[0].condition.min = 101;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); state("run", "run.wav", true); edge("off", "run", 1, SG_TIMEOUT, 0, SG_IMMEDIATE);
    g->transitions[0].condition.has_max = true; g->transitions[0].condition.max = 2;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); state("run", "run.wav", true); edge("off", "run", 1, SG_SPEED, 0, SG_IMMEDIATE);
    g->transitions[0].condition.has_min = g->transitions[0].condition.has_max = true;
    g->transitions[0].condition.min = 20; g->transitions[0].condition.max = 10;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); state("run", "run.wav", true); edge("off", "run", 1, SG_VAR_CMP, 0, SG_IMMEDIATE);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic)); /* missing variable */
    reset_model(); state("run", "run.wav", true); edge("off", "run", 1, SG_ENGINE_ON, 0, SG_IMMEDIATE);
    g->transitions[0].condition.has_var = true;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic)); /* unexpected variable */
    reset_model(); state("run", "run.wav", true); edge("off", "run", 1, SG_ENGINE_ON, 0, SG_IMMEDIATE);
    strcpy(g->transitions[0].id, " bad");
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic));
    reset_model(); state("run", "run.wav", true); edge("off", "run", 1, SG_ENGINE_ON, 0, SG_IMMEDIATE);
    edge("off", "run", 1, SG_ENGINE_OFF, 0, SG_IMMEDIATE);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic)); /* duplicate source priority */
    reset_model(); state("run", "run.wav", true); edge("off", "run", 1, SG_ENGINE_ON, 0, SG_IMMEDIATE);
    edge("off", "run", 2, SG_ENGINE_OFF, 0, SG_IMMEDIATE); strcpy(g->transitions[1].id, "e0");
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic)); /* duplicate ID */

    reset_model(); state("a", "", false); state("b", "", false);
    edge("a", "b", 1, SG_ENGINE_ON, 0, SG_IMMEDIATE);
    edge("b", "a", 2, SG_ENGINE_ON, 0, SG_IMMEDIATE);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic)); /* silent cycle */
    reset_model(); state("island", "", false);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_validate(g, &diagnostic)); /* unreachable */

    reset_model(); state("run", "run.wav", true);
    g->state_count = SG_MAX_STATES + 1;
    TEST_ASSERT_FALSE(sg_file_used(g, "run.wav"));
    TEST_ASSERT_FALSE(sg_file_used(NULL, "run.wav"));
    TEST_ASSERT_FALSE(sg_file_used(g, ""));
}

static void test_parse_minimal_and_draft(void)
{
    TEST_ASSERT_EQUAL_STRING("test",g->id); TEST_ASSERT_EQUAL_UINT8(0,g->engine_entry_index);
    state("run","run.wav",true); edge("off","run",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    TEST_ASSERT_TRUE(sg_file_used(g,"run.wav")); TEST_ASSERT_FALSE(sg_file_used(g,"other.wav"));
}
static void test_parse_engine_no_fkey_and_hold(void)
{
    const char *json =
        "{\"format\":\"sound-graph\",\"schemaVersion\":1,\"id\":\"test\",\"name\":\"Test\","
        "\"engine\":{\"entry\":\"off\",\"fn\":255,\"hold\":1},\"hysteresis\":2,\"observers\":1,"
        "\"states\":[{\"id\":\"off\",\"name\":\"Off\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000}],"
        "\"transitions\":[],\"effects\":[],\"assets\":[],\"editor\":{\"positions\":{},\"viewport\":{\"x\":0,\"zoom\":1}}}";
    TEST_ASSERT_EQUAL(ESP_OK,sg_parse(json,strlen(json),g,&diagnostic));
    TEST_ASSERT_EQUAL_UINT8(SG_NONE,g->engine_fn);
    TEST_ASSERT_TRUE(g->engine_hold);
    TEST_ASSERT_TRUE(g->observers);
    TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
}
static void test_parse_editor_boolean_and_effect_metadata(void)
{
    const char *json =
        "{\"format\":\"sound-graph\",\"schemaVersion\":1,\"id\":\"test\",\"name\":\"Test\","
        "\"engine\":{\"entry\":\"off\",\"fn\":255,\"hold\":false},\"hysteresis\":2,\"observers\":false,"
        "\"states\":[{\"id\":\"off\",\"name\":\"Off\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000}],"
        "\"transitions\":[],\"effects\":[{\"id\":\"horn\",\"entry\":\"off\",\"fn\":2,\"hasFn\":true,\"label\":\"Horn\"}],"
        "\"assets\":[],\"editor\":{\"positions\":{},\"viewport\":{\"x\":0,\"zoom\":1}}}";
    TEST_ASSERT_EQUAL(ESP_OK,sg_parse(json,strlen(json),g,&diagnostic));
    TEST_ASSERT_FALSE(g->engine_hold);
    TEST_ASSERT_FALSE(g->observers);
    TEST_ASSERT_EQUAL_UINT8(1,g->effect_count);
    TEST_ASSERT_EQUAL_STRING("horn",g->effects[0].id);
    TEST_ASSERT_EQUAL_UINT8(2,g->effects[0].fn);
    TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
}
static void test_duplicate_keys_and_malformed_json(void)
{
    reject_editor("{\"x\":1,\"x\":2}"); reject_editor("{\"x\":1,\"\\u0078\":2}");
    reject_editor("{\"x\":NaN}"); reject_editor("{\"x\":1e309}"); reject_editor("{\"x\":01}");
    reject_editor("{\"x\":1,}"); reject_editor("{\"x\":true false}"); reject_editor("{\"x\":\"bad\\q\"}");
    reject_editor("{\"x\":\"\\u0000\"}"); reject_editor("{\"x\":\"\\ud800\"}");
    reject_editor("{\"x\":\"\\udc00\"}"); reject_editor("{\"x\":\"\xc0\xaf\"}");
}
static void test_utf8_and_unicode_escapes(void)
{
    const char *from = strstr(minimal,"\"Test\""); char json[1024]; size_t n = (size_t)(from-minimal);
    memcpy(json,minimal,n); snprintf(json+n,sizeof(json)-n,"\"\\u0410\\ud83d\\ude00\"%s",from+6);
    TEST_ASSERT_EQUAL(ESP_OK,sg_parse(json,strlen(json),g,&diagnostic));
    TEST_ASSERT_EQUAL_STRING("\xd0\x90\xf0\x9f\x98\x80",g->name);
}
static void test_parser_size_depth_and_string_bounds(void)
{
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_parse(minimal,SG_MAX_JSON+1,g,&diagnostic));
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_parse(NULL,1,g,&diagnostic));
    reject_editor("{\"x\":[[[[[[[[[[[[[[[[[[[[0]]]]]]]]]]]]]]]]]]]]}");
    char large[1100]; memset(large,'a',sizeof(large)); large[0]='{'; large[1]='"'; large[1095]='"'; large[1096]=':'; large[1097]='0'; large[1098]='}'; large[1099]=0; reject_editor(large);
}
static void test_exact_body_limit_and_token_limit(void)
{
    char *json = malloc(SG_MAX_JSON + 1); TEST_ASSERT_NOT_NULL(json);
    size_t size = strlen(minimal); memcpy(json,minimal,size); memset(json+size,' ',SG_MAX_JSON-size);
    TEST_ASSERT_EQUAL(ESP_OK,sg_parse(json,SG_MAX_JSON,g,&diagnostic));
    char *editor = malloc(2 * SG_TOKEN_LIMIT + 16); TEST_ASSERT_NOT_NULL(editor);
    size_t pos = 0; memcpy(editor,"{\"x\":[",6); pos = 6;
    for (unsigned i = 0; i < SG_TOKEN_LIMIT; ++i) { editor[pos++] = '0'; editor[pos++] = ','; }
    editor[pos-1] = ']'; editor[pos++] = '}'; editor[pos] = 0;
    reject_editor(editor); free(editor); free(json);
}
static void test_ids_filenames_and_direct_model_bounds(void)
{
    TEST_ASSERT_TRUE(sg_id_valid("A_0-z")); TEST_ASSERT_FALSE(sg_id_valid("../x"));
    TEST_ASSERT_FALSE(sg_id_valid("")); TEST_ASSERT_TRUE(sg_filename_valid("run.wav"));
    TEST_ASSERT_FALSE(sg_filename_valid("../run.wav")); TEST_ASSERT_FALSE(sg_filename_valid("x.mp3"));
    TEST_ASSERT_TRUE(sg_filename_valid("notch_2-loop1.1.wav"));
    TEST_ASSERT_TRUE(sg_filename_valid("t\xc3\xbcre-auf_fuhrerstand.wav"));
    TEST_ASSERT_FALSE(sg_filename_valid(".hidden.wav"));
    TEST_ASSERT_FALSE(sg_filename_valid("a..b.wav"));
    memset(g->id,'x',sizeof(g->id)); TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    snprintf(g->id,sizeof(g->id),"test"); g->state_count = 58; TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
}
static void test_validator_references_ranges_priorities(void)
{
    state("run","run.wav",true); unsigned e = edge("off","missing",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic)); TEST_ASSERT_EQUAL_STRING("e0",diagnostic.id);
    snprintf(g->transitions[e].target,SG_ID_CAP,"run"); TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    edge("off","run",1,SG_ENGINE_OFF,0,SG_IMMEDIATE); TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    g->transition_count = 1; g->transitions[0].condition.type = SG_SPEED;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    g->transitions[0].condition.has_min = g->transitions[0].condition.has_max = true;
    g->transitions[0].condition.min = 20; g->transitions[0].condition.max = 10;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
}
static void test_runner_no_autostart_and_toggle_edges(void)
{
    state("run","run.wav",true); edge("off","run",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    init_runner(); sg_runner_tick(r,100,0,true,20); TEST_ASSERT_EQUAL(0,plays);
    sg_runner_function(r,1,true); sg_runner_function(r,1,true); TEST_ASSERT_TRUE(r->engine_on);
    sg_runner_tick(r,0,0,true,20); TEST_ASSERT_EQUAL(1,plays); TEST_ASSERT_EQUAL_UINT8(1,r->channels[0].state);
    sg_runner_reset(r); sg_runner_function(r,1,true); sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL(1,plays); TEST_ASSERT_FALSE(r->armed);
    sg_runner_function(r,1,false); sg_runner_function(r,1,true); sg_runner_tick(r,0,0,true,20); TEST_ASSERT_EQUAL(2,plays);
}
static void test_engine_without_fkey_runs_continuously(void)
{
    state("run","run.wav",true); edge("off","run",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    g->engine_fn = SG_NONE;
    init_runner();
    TEST_ASSERT_TRUE(r->engine_on);
    sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL(1,plays); TEST_ASSERT_EQUAL_UINT8(1,r->channels[0].state);
}
static void test_prime_levels_restores_held_engine_state(void)
{
    state("run","run.wav",true); edge("off","run",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    init_runner();
    TEST_ASSERT_FALSE(r->engine_on);
    TEST_ASSERT_FALSE(r->armed);
    sg_runner_prime_levels(r,UINT32_C(1) << 1);
    TEST_ASSERT_TRUE(r->engine_on);
    TEST_ASSERT_TRUE(r->armed);
    TEST_ASSERT_TRUE((r->fn_levels & (UINT32_C(1) << 1)) != 0);
    sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL(1,plays); TEST_ASSERT_EQUAL_UINT8(1,r->channels[0].state);
}

static void test_sound_graph_prime_after_commit_starts_engine(void)
{
    state("run","run.wav",true); edge("off","run",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    sound_graph_prepared_t *prepared = NULL;
    TEST_ASSERT_EQUAL(ESP_OK,sound_graph_prepare(g,"test",1,&prepared));
    sound_graph_commit(prepared);
    TEST_ASSERT_NOT_NULL(s_graph_runner);
    TEST_ASSERT_FALSE(s_graph_runner->engine_on);
    sound_graph_prime_functions(UINT32_C(1) << 1);
    TEST_ASSERT_TRUE(s_graph_runner->engine_on);
    TEST_ASSERT_TRUE(s_graph_runner->armed);
}

static void test_status_masks_handle_channel_32(void)
{
    reset_model();
    g->effect_count = SG_MAX_EFFECTS;
    for (unsigned i = 0; i < SG_MAX_EFFECTS; ++i) {
        snprintf(g->effects[i].id,sizeof(g->effects[i].id),"fx%u",i);
        snprintf(g->effects[i].entry,sizeof(g->effects[i].entry),"off");
        g->effects[i].fn = (uint8_t)(i % 29U);
    }
    sound_graph_prepared_t *prepared = NULL;
    TEST_ASSERT_EQUAL(ESP_OK,sound_graph_prepare(g,"test",1,&prepared));
    sound_graph_commit(prepared);
    TEST_ASSERT_NOT_NULL(s_graph_runner);
    s_graph_runner->channels[31].failed = true;
    s_graph_runner->channels[31].retry_play = true;
    s_graph_runner->channels[32].failed = true;
    s_graph_runner->channels[32].retry_play = true;
    s_graph_runner->channels[32].state = 7;

    sound_graph_status_t status;
    sound_graph_status_get(&status);
    TEST_ASSERT_TRUE((status.failed_channels & (UINT32_C(1) << 31)) != 0);
    TEST_ASSERT_TRUE((status.retry_channels & (UINT32_C(1) << 31)) != 0);
    TEST_ASSERT_TRUE(status.failed_channel32);
    TEST_ASSERT_TRUE(status.retry_channel32);
    TEST_ASSERT_EQUAL_UINT8(7,status.states[32]);
}

static void test_audio_idle_retries_without_permanent_failure(void)
{
    state("run","run.wav",true); edge("off","run",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    init_runner(); sg_runner_power(r,true);
    sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL(1,plays);
    audio_state[18] = SG_AUDIO_IDLE;
    sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_FALSE(r->channels[0].failed);
    TEST_ASSERT_TRUE(r->channels[0].retry_play);
    TEST_ASSERT_FALSE(r->channels[0].playing);
    TEST_ASSERT_EQUAL(1,releases);
    sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL(2,plays);
    TEST_ASSERT_FALSE(r->channels[0].failed);
    TEST_ASSERT_TRUE(r->channels[0].playing);
}

static void test_graph_audio_poll_maps_stopped_to_idle(void)
{
    sg_handle_t handle = {0,1};
    s_audio_voice_state = AUDIO_VOICE_FINISHED;
    s_audio_completion = AUDIO_COMPLETION_STOPPED;
    TEST_ASSERT_EQUAL(SG_AUDIO_IDLE,graph_audio_poll(NULL,handle));
    s_audio_completion = AUDIO_COMPLETION_STALE;
    TEST_ASSERT_EQUAL(SG_AUDIO_IDLE,graph_audio_poll(NULL,handle));
    s_audio_completion = AUDIO_COMPLETION_ERROR;
    TEST_ASSERT_EQUAL(SG_AUDIO_FAILED,graph_audio_poll(NULL,handle));
    s_audio_completion = AUDIO_COMPLETION_EOF;
    TEST_ASSERT_EQUAL(SG_AUDIO_DONE,graph_audio_poll(NULL,handle));
    s_audio_completion = AUDIO_COMPLETION_NONE;
    s_audio_voice_state = AUDIO_VOICE_PLAYING;
    TEST_ASSERT_EQUAL(SG_AUDIO_PLAYING,graph_audio_poll(NULL,handle));
    s_audio_voice_state = AUDIO_VOICE_PENDING;
    TEST_ASSERT_EQUAL(SG_AUDIO_PENDING,graph_audio_poll(NULL,handle));
    /* A voice finished without an error (e.g. inhibit) is idle, not failed. */
    s_audio_voice_state = AUDIO_VOICE_FINISHED;
    TEST_ASSERT_EQUAL(SG_AUDIO_IDLE,graph_audio_poll(NULL,handle));
}

static void test_all_function_keys_drive_effect_channels(void)
{
    unsigned play_state[29], off_state[29];
    memset(g,0,sizeof(*g));
    snprintf(g->id,sizeof(g->id),"allfn");
    snprintf(g->name,sizeof(g->name),"All F");
    g->hysteresis = 2;
    g->engine_fn = SG_NONE;
    state("eng_off","",false);
    snprintf(g->engine_entry,sizeof(g->engine_entry),"eng_off");
    for (unsigned fn = 0; fn < 29; ++fn) {
        char off[16], play[16], file[16];
        snprintf(off,sizeof(off),"e%u_off",fn);
        snprintf(play,sizeof(play),"e%u_play",fn);
        snprintf(file,sizeof(file),"f%u.wav",fn);
        off_state[fn] = state(off,"",false);
        play_state[fn] = state(play,file,false);
        snprintf(g->effects[fn].id,sizeof(g->effects[fn].id),"fx%u",fn);
        snprintf(g->effects[fn].entry,sizeof(g->effects[fn].entry),"%s",off);
        g->effects[fn].fn = (uint8_t)fn;
        g->effect_count = fn + 1;
        edge(off,play,1,SG_FN_ON,fn,SG_IMMEDIATE);
        edge(play,off,1,SG_FN_OFF,fn,SG_IMMEDIATE);
    }
    init_runner();
    sg_runner_tick(r,0,0,true,20);
    for (unsigned fn = 0; fn < 29; ++fn) {
        sg_runner_function(r,(uint8_t)fn,true);
        sg_runner_tick(r,0,0,true,20);
        TEST_ASSERT_EQUAL_UINT8(play_state[fn],r->channels[fn + 1].state);
        sg_runner_function(r,(uint8_t)fn,false);
        sg_runner_tick(r,0,0,true,20);
        TEST_ASSERT_EQUAL_UINT8(off_state[fn],r->channels[fn + 1].state);
    }
}
static void test_state_enter_fires_once_after_entry(void)
{
    state("wait","",false); state("run","run.wav",false);
    edge("off","wait",1,SG_SAMPLE_DONE,0,SG_IMMEDIATE);
    edge("wait","run",2,SG_STATE_ENTER,0,SG_IMMEDIATE);
    init_runner(); r->armed = true;
    sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL_UINT8(1,r->channels[0].state); TEST_ASSERT_EQUAL(0,plays);
    sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL_UINT8(2,r->channels[0].state); TEST_ASSERT_EQUAL(1,plays);
    sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL_UINT8(2,r->channels[0].state);
}
static void test_observers_arm_from_boot(void)
{
    g->observers = true; init_runner();
    TEST_ASSERT_TRUE(r->armed);
}
static void test_short_edges_priority_and_one_transition(void)
{
    state("low","low.wav",false); state("high","high.wav",false); state("next","next.wav",false);
    edge("off","low",1,SG_FN_PRESS,2,SG_IMMEDIATE); edge("off","high",9,SG_FN_PRESS,2,SG_IMMEDIATE);
    edge("high","next",1,SG_FN_OFF,2,SG_IMMEDIATE);
    init_runner(); sg_runner_function(r,2,true); sg_runner_function(r,2,false); sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL_UINT8(2,r->channels[0].state); TEST_ASSERT_EQUAL(1,plays);
    sg_runner_tick(r,0,0,true,20); TEST_ASSERT_EQUAL_UINT8(3,r->channels[0].state); TEST_ASSERT_EQUAL(2,plays);
}
static void test_engine_endless_loop_changes_band_immediately(void)
{
    unsigned idle = state("idle","idle.wav",true);
    unsigned run = state("run","run.wav",true);
    edge("off","idle",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    unsigned e = edge("idle","run",1,SG_SPEED,0,SG_AFTER_SAMPLE);
    g->transitions[e].condition.has_min = true; g->transitions[e].condition.min = 1;
    g->transitions[e].condition.has_max = true; g->transitions[e].condition.max = 255;
    init_runner(); sg_runner_power(r,true);
    sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL_UINT8(idle,r->channels[0].state);
    TEST_ASSERT_TRUE(r->channels[0].playing);
    /* The engine loop has no sample end: a speed-band change must not wait for
     * sample_done, otherwise the engine freezes on the idle loop. */
    sg_runner_tick(r,100,0,true,20);
    TEST_ASSERT_EQUAL_UINT8(run,r->channels[0].state);
    TEST_ASSERT_EQUAL(2,plays);
}

static void test_pending_boundary_rechecks_condition_and_loop_replays(void)
{
    state("run","run.wav",true); state("end","end.wav",false);
    edge("off","run",1,SG_ENGINE_ON,0,SG_IMMEDIATE); edge("run","end",1,SG_FN_ON,2,SG_AFTER_SAMPLE);
    init_runner(); sg_runner_power(r,true); sg_runner_tick(r,0,0,true,20); TEST_ASSERT_EQUAL(1,plays);
    sg_runner_tick(r,0,0,true,20); TEST_ASSERT_EQUAL(1,plays); TEST_ASSERT_FALSE(r->channels[0].done);
    audio_state[18] = SG_AUDIO_DONE; sg_runner_tick(r,0,0,true,20); TEST_ASSERT_EQUAL(2,plays);
    sg_runner_function(r,2,true); sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL(2,plays); TEST_ASSERT_NOT_EQUAL(SG_NONE,r->channels[0].pending);
    sg_runner_function(r,2,false); audio_state[18] = SG_AUDIO_DONE; sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL(3,plays); TEST_ASSERT_EQUAL_UINT8(1,r->channels[0].state);
    sg_runner_function(r,2,true); audio_state[18] = SG_AUDIO_DONE; sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL(4,plays); TEST_ASSERT_EQUAL_UINT8(2,r->channels[0].state);
}
static void test_finite_loop_repeats_then_sample_done(void)
{
    /* A looping state with loops=N replays N times, then signals sample_done. */
    unsigned a = state("loop","loop.wav",true); g->states[a].loops = 3;
    state("end","end.wav",false);
    edge("off","loop",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    edge("loop","end",1,SG_SAMPLE_DONE,0,SG_IMMEDIATE);
    init_runner(); sg_runner_power(r,true); sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL(1,plays); TEST_ASSERT_EQUAL_UINT8(1,r->channels[0].state);
    audio_state[18] = SG_AUDIO_DONE; sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL(2,plays); TEST_ASSERT_EQUAL_UINT8(1,r->channels[0].state); TEST_ASSERT_FALSE(r->channels[0].done);
    audio_state[18] = SG_AUDIO_DONE; sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL(3,plays); TEST_ASSERT_EQUAL_UINT8(1,r->channels[0].state);
    audio_state[18] = SG_AUDIO_DONE; sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL(4,plays); TEST_ASSERT_EQUAL_UINT8(2,r->channels[0].state);
}
static void test_loop_timeout_covers_whole_loop_not_one_sample(void)
{
    /* A timeout loop must not restart its timer when the sample ends. */
    state("loop","loop.wav",true); state("end","end.wav",false);
    edge("off","loop",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    unsigned e = edge("loop","end",1,SG_SAMPLE_DONE,0,SG_IMMEDIATE);
    g->transitions[e].condition.type = SG_TIMEOUT;
    g->transitions[e].condition.has_min = true;
    g->transitions[e].condition.min = 2; /* 100 ms */
    init_runner(); sg_runner_power(r,true); sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL_UINT8(1,r->channels[0].state);
    for (unsigned i = 0; i < 5; ++i) { audio_state[18] = SG_AUDIO_DONE; sg_runner_tick(r,0,0,true,20); }
    TEST_ASSERT_EQUAL_UINT8(2,r->channels[0].state);
}
static void test_no_free_voice_is_retried_not_failed(void)
{
    state("run","run.wav",true); edge("off","run",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    init_runner(); no_voice_once = true; sg_runner_power(r,true);
    sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_FALSE(r->channels[0].failed); TEST_ASSERT_EQUAL(0,plays);
    sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_FALSE(r->channels[0].failed); TEST_ASSERT_EQUAL(1,plays);
}
static void test_parse_state_loops_field(void)
{
    const char *json = "{\"format\":\"sound-graph\",\"schemaVersion\":1,\"id\":\"t\",\"name\":\"T\",\"engine\":{\"entry\":\"off\",\"fn\":1},\"hysteresis\":2,"
        "\"states\":[{\"id\":\"off\",\"name\":\"off\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000},"
        "{\"id\":\"l\",\"name\":\"l\",\"file\":\"a.wav\",\"loop\":true,\"volume\":80,\"rate\":1000,\"loops\":4}],"
        "\"transitions\":[{\"id\":\"e0\",\"source\":\"off\",\"target\":\"l\",\"priority\":1,\"timing\":\"immediate\",\"condition\":{\"type\":\"engine_on\"}}],"
        "\"effects\":[],\"assets\":[],\"editor\":{\"positions\":{},\"viewport\":{\"x\":-1.5,\"zoom\":1e0}}}";
    TEST_ASSERT_EQUAL(ESP_OK, sg_parse(json,(size_t)strlen(json),g,&diagnostic));
    TEST_ASSERT_EQUAL_UINT8(4, g->states[1].loops);
}
static void test_sample_done_silent_next_tick_and_audible_once(void)
{
    state("run","run.wav",false); edge("off","run",1,SG_SAMPLE_DONE,0,SG_IMMEDIATE);
    edge("run","off",1,SG_SAMPLE_DONE,0,SG_IMMEDIATE); init_runner();
    r->armed = true; sg_runner_tick(r,0,0,true,20); TEST_ASSERT_EQUAL_UINT8(1,r->channels[0].state);
    sg_runner_tick(r,0,0,true,20); TEST_ASSERT_EQUAL_UINT8(1,r->channels[0].state);
    audio_state[18] = SG_AUDIO_DONE; sg_runner_tick(r,0,0,true,20); TEST_ASSERT_EQUAL_UINT8(0,r->channels[0].state);
    TEST_ASSERT_EQUAL(1,plays);
}
static void test_async_and_admission_failure_stop_channel(void)
{
    state("run","run.wav",true); edge("off","run",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    init_runner(); sg_runner_power(r,true); sg_runner_tick(r,0,0,true,20);
    audio_state[18] = SG_AUDIO_FAILED; sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_TRUE(r->channels[0].failed); TEST_ASSERT_FALSE(r->channels[0].playing);
    sg_runner_tick(r,0,0,true,20); TEST_ASSERT_EQUAL(1,plays);
    sg_runner_reset(r); fail_play = true; sg_runner_power(r,true); sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_TRUE(r->channels[0].failed); TEST_ASSERT_EQUAL(1,plays);
}
static void test_effect_channels_do_not_interrupt_engine(void)
{
    state("run","run.wav",true); state("fxoff","",false); state("horn","horn.wav",false);
    edge("off","run",1,SG_ENGINE_ON,0,SG_IMMEDIATE); edge("fxoff","horn",1,SG_FN_PRESS,2,SG_IMMEDIATE);
    g->effect_count = 1; snprintf(g->effects[0].id,SG_ID_CAP,"horn_channel"); snprintf(g->effects[0].entry,SG_ID_CAP,"fxoff"); g->effects[0].fn = 2;
    init_runner(); sg_runner_function(r,1,true); sg_runner_function(r,2,true); sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL(2,plays); TEST_ASSERT_EQUAL_UINT8(18,r->channels[0].handle.voice);
    TEST_ASSERT_TRUE(r->channels[1].handle.voice < 18); TEST_ASSERT_EQUAL(0,releases);
    sg_runner_reset(r); TEST_ASSERT_EQUAL(2,releases);
}
static void test_filtered_accel_and_decel_ranges(void)
{
    state("run","run.wav",false); unsigned e = edge("off","run",1,SG_DECEL,0,SG_IMMEDIATE);
    g->transitions[e].condition.has_min = true; g->transitions[e].condition.min = 3;
    init_runner(); r->armed = true; sg_runner_tick(r,0,-2,true,20); TEST_ASSERT_EQUAL(0,plays);
    sg_runner_tick(r,0,-3,true,20); TEST_ASSERT_EQUAL(1,plays);
}
static void test_silent_cycles_and_temporal_events(void)
{
    state("next","",false); edge("off","next",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    edge("next","off",1,SG_ENGINE_OFF,0,SG_IMMEDIATE);
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    g->transitions[1].condition.type = SG_SAMPLE_DONE; TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    g->transitions[1].condition.type = SG_FN_PRESS; g->transitions[1].condition.has_fn = true; g->transitions[1].condition.fn = 2;
    TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
}
static void test_speed_hysteresis_rejects_threshold_jitter(void)
{
    state("run","run.wav",true); unsigned up = edge("off","run",1,SG_SPEED,0,SG_IMMEDIATE);
    unsigned down = edge("run","off",1,SG_SPEED,0,SG_IMMEDIATE);
    g->transitions[up].condition.has_min = true; g->transitions[up].condition.min = 100;
    g->transitions[down].condition.has_max = true; g->transitions[down].condition.max = 100;
    init_runner(); r->armed = true;
    sg_runner_tick(r,101,0,true,20); TEST_ASSERT_EQUAL(0,plays);
    sg_runner_tick(r,102,0,true,20); TEST_ASSERT_EQUAL(1,plays);
    sg_runner_tick(r,100,0,true,20); TEST_ASSERT_EQUAL_UINT8(1,r->channels[0].state);
    sg_runner_tick(r,99,0,true,20); TEST_ASSERT_EQUAL_UINT8(1,r->channels[0].state);
    sg_runner_tick(r,98,0,true,20); TEST_ASSERT_EQUAL_UINT8(0,r->channels[0].state);
}
static void test_state_and_asset_capacity(void)
{
    g->state_count = 0;
    for (unsigned i = 0; i < SG_MAX_STATES; ++i) {
        char id[32], file[64]; snprintf(id,sizeof(id),"s%u",i);
        snprintf(file,sizeof(file),"f%u.wav",i); state(id,i < SG_MAX_SILENT_STATES ? "" : file,false);
        if (i > 0) { char src[32]; snprintf(src,sizeof(src),"s%u",i-1); edge(src,id,1,SG_SAMPLE_DONE,0,SG_IMMEDIATE); }
    }
    snprintf(g->engine_entry,SG_ID_CAP,"s0"); TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    for (unsigned i = 1; i < SG_MAX_STATES; ++i) {
        char file[64]; snprintf(file,sizeof(file),"f%u.wav",i);
        snprintf(g->states[i].file,SG_FILE_CAP,"%s",i <= SG_MAX_SOUND_STATES + 1 ? file : "");
    }
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    for (unsigned i = 0; i < SG_MAX_STATES; ++i) {
        char file[64]; snprintf(file,sizeof(file),"f%u.wav",i);
        snprintf(g->states[i].file,SG_FILE_CAP,"%s",i <= SG_MAX_SILENT_STATES ? "" : file);
    }
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    for (unsigned i = 0; i < SG_MAX_STATES; ++i) {
        char file[64]; snprintf(file,sizeof(file),"f%u.wav",i);
        snprintf(g->states[i].file,SG_FILE_CAP,"%s",i < SG_MAX_SILENT_STATES ? "" : file);
    }
    g->asset_count = SG_MAX_ASSETS + 1; TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
}
static void test_pending_cannot_mask_current_immediate_and_event_is_not_latched(void)
{
    state("run","run.wav",true); state("end","end.wav",false);
    edge("off","run",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    edge("run","end",20,SG_FN_PRESS,2,SG_AFTER_SAMPLE);
    edge("run","off",1,SG_ENGINE_OFF,0,SG_IMMEDIATE);
    init_runner(); sg_runner_power(r,true); sg_runner_tick(r,0,0,true,20);
    sg_runner_function(r,2,true); sg_runner_tick(r,0,0,true,20); TEST_ASSERT_NOT_EQUAL(SG_NONE,r->channels[0].pending);
    audio_state[18] = SG_AUDIO_DONE; sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL_UINT8(1,r->channels[0].state); TEST_ASSERT_EQUAL(2,plays);
    sg_runner_function(r,2,false); sg_runner_function(r,2,true); sg_runner_power(r,false); sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL_UINT8(0,r->channels[0].state); TEST_ASSERT_EQUAL(2,plays);
}
static void test_reachability_union_and_shared_states(void)
{
    state("orphan","",false); TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    TEST_ASSERT_EQUAL_STRING("orphan",diagnostic.id);
    g->effect_count = 1; snprintf(g->effects[0].id,SG_ID_CAP,"effect"); snprintf(g->effects[0].entry,SG_ID_CAP,"orphan");
    g->effects[0].fn = 2; TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    edge("off","orphan",1,SG_SAMPLE_DONE,0,SG_IMMEDIATE); TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
}
static void test_direction_condition(void)
{
    state("run","run.wav",true); unsigned e = edge("off","run",1,SG_DIR_FWD,0,SG_IMMEDIATE);
    init_runner(); r->armed = true;
    sg_runner_tick(r,0,0,false,20); TEST_ASSERT_EQUAL(0,plays);
    sg_runner_tick(r,0,0,true,20); TEST_ASSERT_EQUAL(1,plays);
    sg_runner_reset(r); g->transitions[e].condition.type = SG_DIR_REV; r->armed = true;
    sg_runner_tick(r,0,0,true,20); TEST_ASSERT_EQUAL(1,plays);
    sg_runner_tick(r,0,0,false,20); TEST_ASSERT_EQUAL(2,plays);
}
static void test_random_selects_among_equal_priority_options(void)
{
    state("r1","r1.wav",false); state("r2","r2.wav",false); state("ready","",false);
    edge("off","ready",1,SG_FN_PRESS,2,SG_IMMEDIATE);
    unsigned a = edge("ready","r1",5,SG_RANDOM,0,SG_IMMEDIATE);
    unsigned b = edge("ready","r2",5,SG_RANDOM,0,SG_IMMEDIATE);
    g->transitions[a].condition.has_min = true; g->transitions[a].condition.min = 100;
    g->transitions[b].condition.has_min = true; g->transitions[b].condition.min = 100;
    edge("r1","off",1,SG_SAMPLE_DONE,0,SG_IMMEDIATE);
    edge("r2","off",1,SG_SAMPLE_DONE,0,SG_IMMEDIATE);
    init_runner();
    unsigned c1 = 0, c2 = 0;
    for (unsigned t = 0; t < 200; ++t) {
        sg_runner_reset(r);
        sg_runner_function(r,2,false); sg_runner_function(r,2,true);
        sg_runner_tick(r,0,0,true,20); sg_runner_tick(r,0,0,true,20);
        if (r->channels[0].state == 1U) { ++c1; } else if (r->channels[0].state == 2U) { ++c2; }
    }
    TEST_ASSERT_TRUE(c1 > 0 && c2 > 0);
}
static void test_random_requires_min_only(void)
{
    state("r","r.wav",false); unsigned e = edge("off","r",1,SG_RANDOM,0,SG_IMMEDIATE);
    g->transitions[e].condition.has_max = true; g->transitions[e].condition.max = 50;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    g->transitions[e].condition.has_max = false; g->transitions[e].condition.has_min = true;
    g->transitions[e].condition.min = 101; TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    g->transitions[e].condition.min = 50; TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
}
static void test_parse_direction_and_random_json(void)
{
    const char *json =
      "{\"format\":\"sound-graph\",\"schemaVersion\":1,\"id\":\"t\",\"name\":\"T\","
      "\"engine\":{\"entry\":\"off\",\"fn\":1},\"hysteresis\":2,"
      "\"states\":[{\"id\":\"off\",\"name\":\"Off\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000},"
      "{\"id\":\"rev\",\"name\":\"Rev\",\"file\":\"r.wav\",\"loop\":false,\"volume\":100,\"rate\":1000},"
      "{\"id\":\"ready\",\"name\":\"Ready\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000},"
      "{\"id\":\"b1\",\"name\":\"B1\",\"file\":\"b.wav\",\"loop\":false,\"volume\":100,\"rate\":1000},"
      "{\"id\":\"b2\",\"name\":\"B2\",\"file\":\"c.wav\",\"loop\":false,\"volume\":100,\"rate\":1000},"
      "{\"id\":\"wait\",\"name\":\"Wait\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000}],"
      "\"transitions\":["
      "{\"id\":\"e1\",\"source\":\"off\",\"target\":\"rev\",\"priority\":1,\"timing\":\"immediate\",\"condition\":{\"type\":\"dir_rev\"}},"
      "{\"id\":\"e2\",\"source\":\"rev\",\"target\":\"ready\",\"priority\":1,\"timing\":\"immediate\",\"condition\":{\"type\":\"fn_press\",\"fn\":2}},"
      "{\"id\":\"e3\",\"source\":\"ready\",\"target\":\"b1\",\"priority\":5,\"timing\":\"immediate\",\"condition\":{\"type\":\"random\",\"min\":100}},"
      "{\"id\":\"e4\",\"source\":\"ready\",\"target\":\"b2\",\"priority\":5,\"timing\":\"immediate\",\"condition\":{\"type\":\"random\",\"min\":100}},"
      "{\"id\":\"e5\",\"source\":\"b1\",\"target\":\"off\",\"priority\":1,\"timing\":\"immediate\",\"condition\":{\"type\":\"sample_done\"}},"
      "{\"id\":\"e6\",\"source\":\"b2\",\"target\":\"off\",\"priority\":1,\"timing\":\"immediate\",\"condition\":{\"type\":\"sample_done\"}},"
      "{\"id\":\"e7\",\"source\":\"ready\",\"target\":\"wait\",\"priority\":6,\"timing\":\"immediate\",\"condition\":{\"type\":\"timeout\",\"min\":2}},"
      "{\"id\":\"e8\",\"source\":\"wait\",\"target\":\"off\",\"priority\":1,\"timing\":\"immediate\",\"condition\":{\"type\":\"sample_done\"}}],"
      "\"effects\":[],\"assets\":[],\"editor\":{\"positions\":{},\"viewport\":{\"x\":0,\"zoom\":1}}}";
    TEST_ASSERT_EQUAL(ESP_OK,sg_parse(json,strlen(json),g,&diagnostic));
    TEST_ASSERT_EQUAL_UINT8(SG_DIR_REV,g->transitions[0].condition.type);
    TEST_ASSERT_EQUAL_UINT8(SG_RANDOM,g->transitions[2].condition.type);
    TEST_ASSERT_EQUAL_UINT8(SG_TIMEOUT,g->transitions[6].condition.type);
}
static void test_timeout_delay(void)
{
    state("run","run.wav",false); state("wait","",false);
    edge("off","wait",1,SG_FN_PRESS,2,SG_IMMEDIATE);
    unsigned e = edge("wait","run",1,SG_TIMEOUT,0,SG_IMMEDIATE);
    g->transitions[e].condition.has_min = true; g->transitions[e].condition.min = 2; /* 100 ms */
    init_runner(); r->armed = true;
    sg_runner_function(r,2,true); sg_runner_tick(r,0,0,true,20); /* off -> wait */
    sg_runner_tick(r,0,0,true,20); sg_runner_tick(r,0,0,true,20); sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL(0,plays); /* 60 ms */
    sg_runner_tick(r,0,0,true,20); TEST_ASSERT_EQUAL(0,plays); /* 80 ms */
    sg_runner_tick(r,0,0,true,20); TEST_ASSERT_EQUAL(1,plays); /* 100 ms */
}
static char *read_fixture(const char *file, size_t *length)
{
    char path[1024]; snprintf(path,sizeof(path),"%s",__FILE__);
    char *a = strrchr(path,'/'), *b = strrchr(path,'\\'); char *last = a;
    if (b && (!a || b > a)) { last = b; } TEST_ASSERT_NOT_NULL(last); last[1] = 0;
    size_t used = strlen(path); snprintf(path+used,sizeof(path)-used,"fixtures/%s",file);
    FILE *f = fopen(path,"rb"); TEST_ASSERT_NOT_NULL_MESSAGE(f,path);
    TEST_ASSERT_EQUAL(0,fseek(f,0,SEEK_END)); long size = ftell(f);
    TEST_ASSERT_TRUE(size > 0 && (unsigned long)size <= SG_MAX_JSON); rewind(f);
    char *json = malloc((size_t)size+1); TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL((size_t)size,fread(json,1,(size_t)size,f)); fclose(f);
    json[size] = 0; *length = (size_t)size; return json;
}
static void test_shared_frontend_fixture_verdicts(void)
{
    size_t len; char *registry = read_fixture("verdicts.json",&len);
    sg_parser_t p = {0}; p.json = registry; p.len = len; p.capacity = SG_TOKEN_LIMIT;
    p.tokens = calloc(1,SG_TOKEN_LIMIT * sizeof(*p.tokens) + 2 * (SG_STRING_LIMIT + 1)); TEST_ASSERT_NOT_NULL(p.tokens);
    p.scratch[0] = (char *)(p.tokens + SG_TOKEN_LIMIT); p.scratch[1] = p.scratch[0] + SG_STRING_LIMIT + 1;
    TEST_ASSERT_EQUAL(0,sg_value(&p,0)); TEST_ASSERT_EQUAL('[',p.tokens[0].type);
    unsigned cases = 0;
    for (int t = 1; t < p.tokens[0].next; t = p.tokens[t].next) {
        char file[128]; bool valid;
        TEST_ASSERT_TRUE(sg_decode(&p,sg_get(&p,t,"file"),file,sizeof(file)));
        TEST_ASSERT_TRUE(sg_boolean(&p,sg_get(&p,t,"valid"),&valid));
        size_t size; char *json = read_fixture(file,&size);
        bool accepted = sg_parse(json,size,g,&diagnostic) == ESP_OK;
        TEST_ASSERT_EQUAL_MESSAGE(valid,accepted,file); free(json); ++cases;
    }
    TEST_ASSERT_TRUE(cases >= 7); free(p.tokens); free(registry);
}
static void test_parse_variables_var_cmp_and_set(void)
{
    const char *json =
      "{\"format\":\"sound-graph\",\"schemaVersion\":1,\"id\":\"t\",\"name\":\"T\","
      "\"engine\":{\"entry\":\"off\",\"fn\":1},\"hysteresis\":2,"
      "\"variables\":[{\"name\":\"flag\",\"init\":3},{\"name\":\"spd\",\"kind\":\"speed\"}],"
      "\"states\":[{\"id\":\"off\",\"name\":\"off\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000},"
      "{\"id\":\"set\",\"name\":\"set\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000,\"setVar\":0,\"setValue\":7},"
      "{\"id\":\"go\",\"name\":\"go\",\"file\":\"g.wav\",\"loop\":false,\"volume\":100,\"rate\":1000},"
      "{\"id\":\"end\",\"name\":\"end\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000}],"
      "\"transitions\":["
      "{\"id\":\"e1\",\"source\":\"off\",\"target\":\"set\",\"priority\":1,\"timing\":\"immediate\",\"condition\":{\"type\":\"var_cmp\",\"var\":1,\"op\":4,\"value\":10}},"
      "{\"id\":\"e2\",\"source\":\"set\",\"target\":\"go\",\"priority\":1,\"timing\":\"immediate\",\"condition\":{\"type\":\"var_cmp\",\"var\":0,\"op\":0,\"var2\":1}},"
      "{\"id\":\"e3\",\"source\":\"go\",\"target\":\"end\",\"priority\":1,\"timing\":\"immediate\",\"condition\":{\"type\":\"sample_done\"}}],"
      "\"effects\":[],\"assets\":[],\"editor\":{\"positions\":{},\"viewport\":{\"x\":0,\"zoom\":1}}}";
    TEST_ASSERT_EQUAL(ESP_OK,sg_parse(json,strlen(json),g,&diagnostic));
    TEST_ASSERT_EQUAL_UINT8(2,g->variable_count);
    TEST_ASSERT_EQUAL_UINT8(SG_VAR_USER,g->variables[0].kind);
    TEST_ASSERT_EQUAL_INT32(3,g->variables[0].init);
    TEST_ASSERT_EQUAL_UINT8(SG_VAR_SPEED,g->variables[1].kind);
    TEST_ASSERT_EQUAL_INT32(0,g->variables[1].init);
    TEST_ASSERT_TRUE(g->states[1].has_set);
    TEST_ASSERT_EQUAL_UINT8(0,g->states[1].set_var);
    TEST_ASSERT_EQUAL_INT16(7,g->states[1].set_value);
    TEST_ASSERT_EQUAL_UINT8(SG_VAR_CMP,g->transitions[0].condition.type);
    TEST_ASSERT_EQUAL_UINT8(4,g->transitions[0].condition.op);
    TEST_ASSERT_EQUAL_INT16(10,g->transitions[0].condition.value);
    TEST_ASSERT_TRUE(g->transitions[1].condition.has_var2);
    TEST_ASSERT_EQUAL_UINT8(1,g->transitions[1].condition.var2);
}
static void test_parse_set_without_value_defaults(void)
{
    const char *json =
      "{\"format\":\"sound-graph\",\"schemaVersion\":1,\"id\":\"t\",\"name\":\"T\","
      "\"engine\":{\"entry\":\"off\",\"fn\":1},\"hysteresis\":2,\"variables\":[{\"name\":\"v\"}],"
      "\"states\":[{\"id\":\"off\",\"name\":\"off\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000,\"setVar\":0}],"
      "\"transitions\":[],\"effects\":[],\"assets\":[],\"editor\":{\"x\":1}}";
    TEST_ASSERT_EQUAL(ESP_OK,sg_parse(json,strlen(json),g,&diagnostic));
    TEST_ASSERT_TRUE(g->states[0].has_set);
    TEST_ASSERT_EQUAL_INT16(0,g->states[0].set_value);
}
static void test_variable_model_validation(void)
{
    snprintf(g->variables[0].name,SG_VAR_NAME_CAP,"v"); g->variable_count = 1;
    g->variables[0].kind = SG_VAR_SPEED; g->variables[0].init = 1;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));   /* builtin with init */
    g->variables[0].init = 0; TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    g->variables[0].kind = 9; TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    g->variables[0].kind = SG_VAR_USER;
    g->variable_count = 2; g->variables[1] = g->variables[0];
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));   /* duplicate name */
    g->variable_count = 1; g->variables[0].name[0] = 0;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));   /* empty name */
    g->variable_count = 0;
}
static void test_set_variable_validation(void)
{
    unsigned i = state("set","",false); edge("off","set",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    g->states[i].has_set = true; g->states[i].set_var = 0;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));   /* no variables declared */
    g->variable_count = 1; snprintf(g->variables[0].name,SG_VAR_NAME_CAP,"v");
    g->variables[0].kind = SG_VAR_SPEED;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));   /* builtin cannot be set */
    g->variables[0].kind = SG_VAR_USER; TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    g->states[i].set_var = 1; TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    g->states[i].has_set = false; TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    g->variable_count = 0;
}
static void test_var_cmp_validation(void)
{
    g->variable_count = 1; snprintf(g->variables[0].name,SG_VAR_NAME_CAP,"v");
    state("go","g.wav",false); unsigned e = edge("off","go",1,SG_VAR_CMP,0,SG_IMMEDIATE);
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));   /* missing variable */
    g->transitions[e].condition.has_var = true; g->transitions[e].condition.var = 1;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));   /* out-of-range variable */
    g->transitions[e].condition.var = 0; g->transitions[e].condition.op = 6;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));   /* bad operator */
    g->transitions[e].condition.op = 0; TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    g->transitions[e].condition.has_var2 = true; g->transitions[e].condition.var2 = 5;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));   /* out-of-range second variable */
    g->transitions[e].condition.var2 = 0; TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    g->transitions[e].condition.type = SG_ENGINE_ON;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));   /* stray variable fields */
    g->transitions[e].condition.has_var = false; g->transitions[e].condition.has_var2 = false;
    TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    g->variable_count = 0;
}
static unsigned var_tick(uint8_t var, uint8_t op, int32_t value, bool has2, uint8_t var2,
                         uint8_t speed, bool fwd, bool power)
{
    sg_condition_t *c = &g->transitions[0].condition;
    c->type = SG_VAR_CMP; c->has_fn = c->has_min = c->has_max = false;
    c->has_var = true; c->var = var; c->op = op; c->value = (int16_t)value; c->has_var2 = has2; c->var2 = var2;
    unsigned before = plays;
    sg_runner_reset(r); r->armed = true; if (power) { sg_runner_power(r,true); }
    sg_runner_tick(r,speed,0,fwd,20);
    return plays - before;
}
static void test_runner_var_cmp_builtins_and_operators(void)
{
    g->variable_count = 5;
    snprintf(g->variables[0].name,SG_VAR_NAME_CAP,"flag"); g->variables[0].kind = SG_VAR_USER;
    snprintf(g->variables[1].name,SG_VAR_NAME_CAP,"spd"); g->variables[1].kind = SG_VAR_SPEED;
    snprintf(g->variables[2].name,SG_VAR_NAME_CAP,"fwd"); g->variables[2].kind = SG_VAR_FORWARD;
    snprintf(g->variables[3].name,SG_VAR_NAME_CAP,"mov"); g->variables[3].kind = SG_VAR_MOVING;
    snprintf(g->variables[4].name,SG_VAR_NAME_CAP,"eng"); g->variables[4].kind = SG_VAR_ENGINE;
    state("run","run.wav",false);
    unsigned e = edge("off","run",1,SG_VAR_CMP,0,SG_IMMEDIATE);
    g->transitions[e].condition.has_var = true; g->transitions[e].condition.var = 1;
    init_runner();
    TEST_ASSERT_EQUAL_UINT(1,var_tick(1,SG_CMP_GT,50,false,0,60,true,false));
    TEST_ASSERT_EQUAL_UINT(0,var_tick(1,SG_CMP_GT,50,false,0,50,true,false));
    TEST_ASSERT_EQUAL_UINT(1,var_tick(1,SG_CMP_GE,50,false,0,50,true,false));
    TEST_ASSERT_EQUAL_UINT(1,var_tick(1,SG_CMP_LT,50,false,0,40,true,false));
    TEST_ASSERT_EQUAL_UINT(1,var_tick(1,SG_CMP_LE,50,false,0,50,true,false));
    TEST_ASSERT_EQUAL_UINT(1,var_tick(1,SG_CMP_EQ,50,false,0,50,true,false));
    TEST_ASSERT_EQUAL_UINT(1,var_tick(1,SG_CMP_NE,50,false,0,49,true,false));
    TEST_ASSERT_EQUAL_UINT(1,var_tick(2,SG_CMP_EQ,1,false,0,0,true,false));   /* forward */
    TEST_ASSERT_EQUAL_UINT(0,var_tick(2,SG_CMP_EQ,1,false,0,0,false,false));
    TEST_ASSERT_EQUAL_UINT(1,var_tick(3,SG_CMP_EQ,1,false,0,10,true,false));  /* moving */
    TEST_ASSERT_EQUAL_UINT(0,var_tick(3,SG_CMP_EQ,1,false,0,0,true,false));
    TEST_ASSERT_EQUAL_UINT(1,var_tick(4,SG_CMP_EQ,1,false,0,0,true,true));    /* engine */
    TEST_ASSERT_EQUAL_UINT(1,var_tick(0,SG_CMP_EQ,0,false,0,0,true,false));   /* user variable */
    TEST_ASSERT_EQUAL_UINT(1,var_tick(1,SG_CMP_EQ,0,true,1,7,true,false));    /* variable vs variable */
    TEST_ASSERT_EQUAL_UINT(0,var_tick(9,SG_CMP_EQ,1,false,0,0,true,false));   /* out of range */
}
static void test_variable_json_rejections(void)
{
    const char *head = "{\"format\":\"sound-graph\",\"schemaVersion\":1,\"id\":\"t\",\"name\":\"T\",\"engine\":{\"entry\":\"off\",\"fn\":1},\"hysteresis\":2,";
    const char *states = "\"states\":[{\"id\":\"off\",\"name\":\"off\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000}],";
    const char *tail = "\"transitions\":[],\"effects\":[],\"assets\":[],\"editor\":{\"x\":1}}";
    char buf[512];
    #define REJ(frag) do { snprintf(buf,sizeof(buf),"%s" frag "%s%s",head,states,tail); TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_parse(buf,strlen(buf),g,&diagnostic)); } while(0)
    REJ("\"variables\":{\"x\":1},");
    REJ("\"variables\":[{\"name\":\"v\",\"kind\":\"bogus\"}],");
    REJ("\"variables\":[{\"name\":\"v\",\"init\":1.5}],");
    REJ("\"variables\":[{\"name\":\"v\",\"init\":1e10}],");
    #undef REJ
    int n = snprintf(buf,sizeof(buf),"%s\"variables\":[",head);
    for (unsigned i = 0; i <= SG_MAX_VARIABLES; ++i) { n += snprintf(buf+n,sizeof(buf)-n,"%s{\"name\":\"v%u\"}", i ? "," : "", i); }
    n += snprintf(buf+n,sizeof(buf)-n,"],%s%s",states,tail);
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_parse(buf,strlen(buf),g,&diagnostic));
}
static void test_condition_json_rejections(void)
{
    const char *head = "{\"format\":\"sound-graph\",\"schemaVersion\":1,\"id\":\"t\",\"name\":\"T\",\"engine\":{\"entry\":\"off\",\"fn\":1},\"hysteresis\":2,\"variables\":[{\"name\":\"v\"}],";
    const char *states = "\"states\":[{\"id\":\"off\",\"name\":\"off\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000},{\"id\":\"go\",\"name\":\"go\",\"file\":\"g.wav\",\"loop\":false,\"volume\":100,\"rate\":1000}],";
    char buf[768];
    #define REJT(trans) do { snprintf(buf,sizeof(buf),"%s%s\"transitions\":[" trans "],\"effects\":[],\"assets\":[],\"editor\":{\"x\":1}}",head,states); TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_parse(buf,strlen(buf),g,&diagnostic)); } while(0)
    REJT("{\"id\":\"e\",\"source\":\"off\",\"target\":\"go\",\"priority\":1,\"timing\":\"immediate\",\"condition\":{\"type\":\"var_cmp\",\"var\":0,\"op\":0,\"value\":40000}}");
    REJT("{\"id\":\"e\",\"source\":\"off\",\"target\":\"go\",\"priority\":1,\"timing\":\"immediate\",\"condition\":{\"type\":\"var_cmp\",\"var\":16,\"op\":0}}");
    REJT("{\"id\":\"e\",\"source\":\"off\",\"target\":\"go\",\"priority\":1,\"timing\":\"immediate\",\"condition\":{\"type\":\"var_cmp\",\"var\":0,\"op\":6}}");
    REJT("{\"id\":\"e\",\"source\":\"off\",\"target\":\"go\",\"priority\":1,\"timing\":\"immediate\",\"condition\":{\"type\":\"var_cmp\",\"var\":0,\"op\":0,\"var2\":16}}");
    #undef REJT
    snprintf(buf,sizeof(buf),"%s\"states\":[{\"id\":\"off\",\"name\":\"off\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000,\"setVar\":0,\"setValue\":40000},{\"id\":\"go\",\"name\":\"go\",\"file\":\"g.wav\",\"loop\":false,\"volume\":100,\"rate\":1000}],\"transitions\":[],\"effects\":[],\"assets\":[],\"editor\":{\"x\":1}}",head);
    TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_parse(buf,strlen(buf),g,&diagnostic));
}
int main(void)
{
    UNITY_BEGIN(); RUN_TEST(test_parse_minimal_and_draft); RUN_TEST(test_parse_engine_no_fkey_and_hold); RUN_TEST(test_parse_editor_boolean_and_effect_metadata); RUN_TEST(test_duplicate_keys_and_malformed_json);
    RUN_TEST(test_parse_valid_token_edges); RUN_TEST(test_parse_rejects_malformed_token_edges); RUN_TEST(test_validator_error_matrix);
    RUN_TEST(test_utf8_and_unicode_escapes); RUN_TEST(test_parser_size_depth_and_string_bounds);
    RUN_TEST(test_exact_body_limit_and_token_limit);
    RUN_TEST(test_ids_filenames_and_direct_model_bounds); RUN_TEST(test_validator_references_ranges_priorities);
    RUN_TEST(test_runner_no_autostart_and_toggle_edges); RUN_TEST(test_engine_without_fkey_runs_continuously); RUN_TEST(test_prime_levels_restores_held_engine_state);
    RUN_TEST(test_sound_graph_prime_after_commit_starts_engine); RUN_TEST(test_status_masks_handle_channel_32);
    RUN_TEST(test_audio_idle_retries_without_permanent_failure); RUN_TEST(test_graph_audio_poll_maps_stopped_to_idle);
    RUN_TEST(test_all_function_keys_drive_effect_channels);
    RUN_TEST(test_state_enter_fires_once_after_entry); RUN_TEST(test_observers_arm_from_boot);
    RUN_TEST(test_short_edges_priority_and_one_transition);
    RUN_TEST(test_engine_endless_loop_changes_band_immediately); RUN_TEST(test_pending_boundary_rechecks_condition_and_loop_replays); RUN_TEST(test_finite_loop_repeats_then_sample_done); RUN_TEST(test_loop_timeout_covers_whole_loop_not_one_sample); RUN_TEST(test_no_free_voice_is_retried_not_failed); RUN_TEST(test_parse_state_loops_field); RUN_TEST(test_sample_done_silent_next_tick_and_audible_once);
    RUN_TEST(test_async_and_admission_failure_stop_channel); RUN_TEST(test_effect_channels_do_not_interrupt_engine);
    RUN_TEST(test_filtered_accel_and_decel_ranges); RUN_TEST(test_silent_cycles_and_temporal_events);
    RUN_TEST(test_speed_hysteresis_rejects_threshold_jitter); RUN_TEST(test_state_and_asset_capacity);
    RUN_TEST(test_pending_cannot_mask_current_immediate_and_event_is_not_latched);
    RUN_TEST(test_reachability_union_and_shared_states); RUN_TEST(test_shared_frontend_fixture_verdicts);
    RUN_TEST(test_direction_condition); RUN_TEST(test_random_selects_among_equal_priority_options);
    RUN_TEST(test_random_requires_min_only); RUN_TEST(test_parse_direction_and_random_json);
    RUN_TEST(test_timeout_delay);
    RUN_TEST(test_parse_variables_var_cmp_and_set); RUN_TEST(test_parse_set_without_value_defaults);
    RUN_TEST(test_variable_model_validation); RUN_TEST(test_set_variable_validation);
    RUN_TEST(test_var_cmp_validation); RUN_TEST(test_runner_var_cmp_builtins_and_operators);
    RUN_TEST(test_variable_json_rejections); RUN_TEST(test_condition_json_rejections);
    return UNITY_END();
}
