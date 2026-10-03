#include "sound_graph.h"

#include <string.h>

static void sgr_release(sg_runner_t *r, sg_channel_t *c)
{
    if (c->playing && r->io.release) { r->io.release(r->io.ctx,c->handle); }
    c->playing = false;
    c->handle = (sg_handle_t){SG_NONE,0};
}
static void sgr_enter(sg_runner_t *r, unsigned channel, uint8_t state)
{
    sg_channel_t *c = &r->channels[channel]; sgr_release(r,c);
    c->state = state; c->pending = SG_NONE; c->done = false;
    memset(c->speed_match,0,sizeof(c->speed_match));
    const sg_state_t *s = &r->graph->states[state];
    if (!s->file[0]) { return; }
    if (!r->io.play || r->io.play(r->io.ctx,channel == 0,s,&c->handle) != ESP_OK) {
        c->failed = true; return;
    }
    c->playing = true;
}
void sg_runner_init(sg_runner_t *r, const sg_graph_t *g, const sg_io_t *io)
{
    memset(r,0,sizeof(*r)); r->graph = g; if (io) { r->io = *io; }
    sg_runner_reset(r);
}
void sg_runner_reset(sg_runner_t *r)
{
    for (unsigned i = 0; i <= SG_MAX_EFFECTS; ++i) {
        sgr_release(r,&r->channels[i]); memset(&r->channels[i],0,sizeof(r->channels[i]));
        r->channels[i].handle.voice = SG_NONE; r->channels[i].pending = SG_NONE;
        r->channels[i].state = SG_NONE;
    }
    if (r->graph) {
        r->channels[0].state = r->graph->engine_entry_index;
        for (unsigned i = 0; i < r->graph->effect_count; ++i) { r->channels[i+1].state = r->graph->effects[i].entry_index; }
    }
    r->engine_on = false; r->armed = false; r->fn_press = 0; r->fn_release = 0;
}
void sg_runner_function(sg_runner_t *r, uint8_t fn, bool on)
{
    if (fn > 28 || !r->graph) { return; } uint32_t bit = UINT32_C(1) << fn;
    if (((r->fn_levels & bit) != 0) == on) { return; }
    if (on) { r->fn_levels |= bit; r->fn_press |= bit; }
    else { r->fn_levels &= ~bit; r->fn_release |= bit; }
    r->armed = true;
    if (on && fn == r->graph->engine_fn) { r->engine_on = !r->engine_on; }
}
void sg_runner_power(sg_runner_t *r, bool on)
{
    r->engine_on = on; r->armed = true;
}
static bool sgr_condition(sg_runner_t *r, sg_channel_t *ch, unsigned idx, uint8_t speed, int32_t accel)
{
    const sg_condition_t *c = &r->graph->transitions[idx].condition;
    uint32_t bit = c->fn <= 28 ? UINT32_C(1) << c->fn : 0;
    switch (c->type) {
        case SG_FN_PRESS: return (r->fn_press & bit) != 0;
        case SG_FN_RELEASE: return (r->fn_release & bit) != 0;
        case SG_FN_ON: return (r->fn_levels & bit) != 0;
        case SG_FN_OFF: return (r->fn_levels & bit) == 0;
        case SG_ENGINE_ON: return r->engine_on;
        case SG_ENGINE_OFF: return !r->engine_on;
        case SG_SAMPLE_DONE: return ch->done;
        default: break;
    }
    int64_t value = c->type == SG_SPEED ? speed : c->type == SG_ACCEL ? accel : -(int64_t)accel;
    int32_t lo = c->has_min ? c->min : 0, hi = c->has_max ? c->max : 255;
    if (c->type == SG_SPEED && ch->speed_match[idx]) {
        lo -= r->graph->hysteresis; hi += r->graph->hysteresis;
    } else if (c->type == SG_SPEED) {
        if (c->has_min && lo > 0) { lo += r->graph->hysteresis; }
        if (c->has_max && hi < 255) { hi -= r->graph->hysteresis; }
        if (lo > 255) { lo = 255; } if (hi < 0) { hi = 0; }
    }
    bool match = value >= lo && value <= hi;
    if (c->type == SG_SPEED) { ch->speed_match[idx] = match; }
    return match;
}
void sg_runner_tick(sg_runner_t *r, uint8_t speed, int32_t accel)
{
    if (!r || !r->graph || !r->armed) { return; }
    for (unsigned i = 0; i <= r->graph->effect_count; ++i) {
        sg_channel_t *ch = &r->channels[i]; if (ch->failed) { continue; }
        if (!r->graph->states[ch->state].file[0]) { ch->done = true; }
        if (ch->playing && r->io.poll) {
            sg_audio_state_t audio = r->io.poll(r->io.ctx,ch->handle);
            if (audio == SG_AUDIO_FAILED) { sgr_release(r,ch); ch->failed = true; ch->pending = SG_NONE; continue; }
            if (audio == SG_AUDIO_DONE) { sgr_release(r,ch); ch->done = true; }
        }
        int best = -1; ch->pending = SG_NONE;
        for (unsigned j = 0; j < r->graph->transition_count; ++j) {
            const sg_transition_t *e = &r->graph->transitions[j];
            if (e->source_index != ch->state || !sgr_condition(r,ch,j,speed,accel)) { continue; }
            if (e->timing == SG_AFTER_SAMPLE && ch->playing) {
                if (ch->pending == SG_NONE || e->priority > r->graph->transitions[ch->pending].priority) { ch->pending = (uint8_t)j; }
                continue;
            }
            if (best < 0 || e->priority > r->graph->transitions[best].priority) { best = (int)j; }
        }
        if (best >= 0) {
            const sg_transition_t *e = &r->graph->transitions[best];
            sgr_enter(r,i,e->target_index); continue;
        }
        if (ch->done && r->graph->states[ch->state].loop) { sgr_enter(r,i,ch->state); }
        else { ch->done = false; }
    }
    r->fn_press = 0; r->fn_release = 0;
}
