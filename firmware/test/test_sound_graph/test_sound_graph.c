#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../components/sound/src/sound_graph.c"
#include "../../components/sound/src/sound_graph_runner.c"

static sg_graph_t *g;
static sg_runner_t *r;
static sg_diagnostic_t diagnostic;
static unsigned plays, releases;
static sg_audio_state_t audio_state[25];
static bool fail_play;
static const char *minimal =
    "{\"format\":\"sound-graph\",\"schemaVersion\":1,\"id\":\"test\",\"name\":\"Test\","
    "\"engine\":{\"entry\":\"off\",\"fn\":1},\"hysteresis\":2,"
    "\"states\":[{\"id\":\"off\",\"name\":\"Off\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000}],"
    "\"transitions\":[],\"effects\":[],\"assets\":[],\"editor\":{\"positions\":{},\"viewport\":{\"x\":-1.5,\"zoom\":1e0}}}";

static esp_err_t play(void *ctx, bool engine, const sg_state_t *state, sg_handle_t *h)
{
    (void)ctx; (void)state; if (fail_play) { return ESP_FAIL; }
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
    g = calloc(1,sizeof(*g)); r = calloc(1,sizeof(*r));
    TEST_ASSERT_NOT_NULL(g); TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(ESP_OK,sg_parse(minimal,strlen(minimal),g,&diagnostic));
    plays = releases = 0; fail_play = false; memset(audio_state,0,sizeof(audio_state));
}
void tearDown(void) { free(r); free(g); }
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
static void test_parse_minimal_and_draft(void)
{
    TEST_ASSERT_EQUAL_STRING("test",g->id); TEST_ASSERT_EQUAL_UINT8(0,g->engine_entry_index);
    state("run","run.wav",true); edge("off","run",1,SG_ENGINE_ON,0,SG_IMMEDIATE);
    TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    TEST_ASSERT_TRUE(sg_file_used(g,"run.wav")); TEST_ASSERT_FALSE(sg_file_used(g,"other.wav"));
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
static void test_short_edges_priority_and_one_transition(void)
{
    state("low","low.wav",false); state("high","high.wav",false); state("next","next.wav",false);
    edge("off","low",1,SG_FN_PRESS,2,SG_IMMEDIATE); edge("off","high",9,SG_FN_PRESS,2,SG_IMMEDIATE);
    edge("high","next",1,SG_FN_OFF,2,SG_IMMEDIATE);
    init_runner(); sg_runner_function(r,2,true); sg_runner_function(r,2,false); sg_runner_tick(r,0,0,true,20);
    TEST_ASSERT_EQUAL_UINT8(2,r->channels[0].state); TEST_ASSERT_EQUAL(1,plays);
    sg_runner_tick(r,0,0,true,20); TEST_ASSERT_EQUAL_UINT8(3,r->channels[0].state); TEST_ASSERT_EQUAL(2,plays);
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
        snprintf(file,sizeof(file),"f%u.wav",i); state(id,i < 26 ? "" : file,false);
        if (i > 0) { char src[32]; snprintf(src,sizeof(src),"s%u",i-1); edge(src,id,1,SG_SAMPLE_DONE,0,SG_IMMEDIATE); }
    }
    snprintf(g->engine_entry,SG_ID_CAP,"s0"); TEST_ASSERT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    snprintf(g->states[25].file,SG_FILE_CAP,"extra.wav"); TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    g->states[25].file[0] = 0; g->states[26].file[0] = 0; TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
    snprintf(g->states[26].file,SG_FILE_CAP,"f26.wav"); g->asset_count = 32; TEST_ASSERT_NOT_EQUAL(ESP_OK,sg_validate(g,&diagnostic));
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
    size_t used = strlen(path); snprintf(path+used,sizeof(path)-used,"../../sound_editor/test/fixtures/%s",file);
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
int main(void)
{
    UNITY_BEGIN(); RUN_TEST(test_parse_minimal_and_draft); RUN_TEST(test_duplicate_keys_and_malformed_json);
    RUN_TEST(test_utf8_and_unicode_escapes); RUN_TEST(test_parser_size_depth_and_string_bounds);
    RUN_TEST(test_exact_body_limit_and_token_limit);
    RUN_TEST(test_ids_filenames_and_direct_model_bounds); RUN_TEST(test_validator_references_ranges_priorities);
    RUN_TEST(test_runner_no_autostart_and_toggle_edges); RUN_TEST(test_short_edges_priority_and_one_transition);
    RUN_TEST(test_pending_boundary_rechecks_condition_and_loop_replays); RUN_TEST(test_sample_done_silent_next_tick_and_audible_once);
    RUN_TEST(test_async_and_admission_failure_stop_channel); RUN_TEST(test_effect_channels_do_not_interrupt_engine);
    RUN_TEST(test_filtered_accel_and_decel_ranges); RUN_TEST(test_silent_cycles_and_temporal_events);
    RUN_TEST(test_speed_hysteresis_rejects_threshold_jitter); RUN_TEST(test_state_and_asset_capacity);
    RUN_TEST(test_pending_cannot_mask_current_immediate_and_event_is_not_latched);
    RUN_TEST(test_reachability_union_and_shared_states); RUN_TEST(test_shared_frontend_fixture_verdicts);
    RUN_TEST(test_direction_condition); RUN_TEST(test_random_selects_among_equal_priority_options);
    RUN_TEST(test_random_requires_min_only); RUN_TEST(test_parse_direction_and_random_json);
    RUN_TEST(test_timeout_delay); return UNITY_END();
}
