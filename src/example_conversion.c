// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Example conversion provider: the smallest complete DisplayXR
 *         conversion-provider plug-in (ADR-053).
 *
 * One capability, DEPTH_MONO (one image -> depth), on the CPU, on both lanes
 * (stream + job). Its "model" is a vertical ramp — near at the bottom, far at
 * the top, a floor-to-horizon prior — so every path of the contract can be
 * exercised with no inference runtime, no GPU and no vendor SDK.
 *
 * VENDOR TODO: everything marked `VENDOR TODO` is where your real provider
 * differs. Keep the shape: probe cheap, describe from memory, warm on the
 * runtime's thread, convert into the runtime's images, report honest units.
 *
 * Contract (normative): docs/specs/runtime/conversion-provider-discovery.md in
 * the runtime repository; walk-through: docs/guides/vendor-plugin-onboarding.md
 * section 9. ABI: include/xrt/xrt_conversion_plugin.h (vendored, see README).
 *
 * Threading: the runtime makes ONE [S] call at a time per provider, so the
 * stream table and the model need no lock against each other. The [C] slots
 * (get_provider_status, describe_cap, get_generation, get_state,
 * stream_get_output_desc) may run concurrently with an [S] call, so the
 * readiness fields they read sit behind a small mutex.
 */

#include "xrt/xrt_conversion_plugin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif


/*
 *
 * Identity. VENDOR TODO: your id is also your registry subkey / manifest id.
 *
 */

#define EX_ID "example-conversion"
#define EX_CAP_DEPTH 1u     //!< our only cap_id (provider-chosen, non-zero, stable)
#define EX_MAX_STREAMS 4u   //!< declared in the capability; the runtime enforces it
#define EX_WARM_STEP_MS 10u //!< warm() polls is_cancelled at least this often (contract: <= 250 ms)

/*! xrt_plugin_platform_state values (xrt_plugin.h), restated so this file
 *  needs only the conversion header. */
#define EX_PLATFORM_READY 1u
#define EX_PLATFORM_ABSENT 2u
#define EX_PLATFORM_INCOMPATIBLE 5u

//! Whole-field struct_size coverage, as the header's XRT_CONV_COVERS.
#define COVERS(PTR, TYPE, FIELD) XRT_CONV_COVERS(PTR, TYPE, FIELD)
//! Write FIELD only when the caller's struct_size covers it.
#define SET(PTR, TYPE, FIELD, VALUE)                                                                                   \
	do {                                                                                                           \
		if (COVERS(PTR, TYPE, FIELD)) {                                                                        \
			(PTR)->FIELD = (VALUE);                                                                        \
		}                                                                                                      \
	} while (0)


/*
 *
 * Tiny portability layer: a mutex and a sleep. Nothing else is OS-specific.
 *
 */

#ifdef _WIN32
typedef SRWLOCK ex_mutex;
static void
ex_mutex_init(ex_mutex *m)
{
	InitializeSRWLock(m);
}
static void
ex_lock(ex_mutex *m)
{
	AcquireSRWLockExclusive(m);
}
static void
ex_unlock(ex_mutex *m)
{
	ReleaseSRWLockExclusive(m);
}
static void
ex_mutex_destroy(ex_mutex *m)
{
	(void)m;
}
static void
ex_sleep_ms(uint32_t ms)
{
	Sleep(ms);
}
#else
typedef pthread_mutex_t ex_mutex;
static void
ex_mutex_init(ex_mutex *m)
{
	pthread_mutex_init(m, NULL);
}
static void
ex_lock(ex_mutex *m)
{
	pthread_mutex_lock(m);
}
static void
ex_unlock(ex_mutex *m)
{
	pthread_mutex_unlock(m);
}
static void
ex_mutex_destroy(ex_mutex *m)
{
	pthread_mutex_destroy(m);
}
static void
ex_sleep_ms(uint32_t ms)
{
	struct timespec ts;
	ts.tv_sec = (time_t)(ms / 1000u);
	ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
	nanosleep(&ts, NULL);
}
#endif


/*
 *
 * State.
 *
 */

/*! What the (simulated) dependency check found. VENDOR TODO: replace with
 *  the real check of your inference runtime / driver / licence. */
enum ex_dependency
{
	EX_DEP_PRESENT = 0,
	EX_DEP_ABSENT = 1,  //!< not installed -> ABSENT (falls through to the next provider)
	EX_DEP_TOO_OLD = 2, //!< installed, below our floor -> FAILED(VERSION), never FAILED(MODEL)
};

struct ex_stream
{
	bool used;
	uint64_t id;
	struct xrt_conv_stream_info info;
	uint64_t frames;
};

struct ex_provider
{
	/* Guarded by `lock`: read by the [C] slots while an [S] slot runs. */
	ex_mutex lock;
	uint32_t state;          //!< XRT_CONV_STATE_*
	uint32_t failure_reason; //!< XRT_CONV_FAIL_*
	uint32_t retryable;
	float progress;
	char detail[XRT_CONV_DETAIL_MAX];

	/* Settings, read once in probe() (never per frame). */
	enum ex_dependency dependency;
	bool ramp_horizontal;
	uint32_t warm_ms;

	/* [S]-only (one call in flight per provider). */
	struct ex_stream streams[EX_MAX_STREAMS];
	uint64_t next_stream_id;
};

static const struct xrt_conversion_plugin_host_iface *g_host = NULL;


/*
 *
 * Helpers.
 *
 */

static void
copy_str(char *dst, size_t cap, const char *src)
{
	if (cap == 0) {
		return;
	}
	size_t n = strlen(src);
	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
}

/*! A setting through the host: DXR_CONV_EXAMPLE_CONVERSION_<KEY> in the
 *  hosting process's environment, else Settings\<KEY> in the registry row
 *  (Windows) or the manifest's "settings" object (POSIX). Never getenv(). */
static bool
setting(const char *key, char *buf, uint32_t size)
{
	if (g_host == NULL || !XRT_CONV_COVERS(g_host, struct xrt_conversion_plugin_host_iface, get_setting) ||
	    g_host->get_setting == NULL) {
		return false;
	}
	return g_host->get_setting(key, buf, size);
}

static void
notify(uint32_t cap_id)
{
	if (g_host != NULL && XRT_CONV_COVERS(g_host, struct xrt_conversion_plugin_host_iface, notify_changed) &&
	    g_host->notify_changed != NULL) {
		g_host->notify_changed(cap_id);
	}
}

static void
set_state(
    struct ex_provider *p, uint32_t state, uint32_t reason, uint32_t retryable, float progress, const char *detail)
{
	ex_lock(&p->lock);
	const bool changed = p->state != state || p->failure_reason != reason;
	p->state = state;
	p->failure_reason = reason;
	p->retryable = retryable;
	p->progress = progress;
	copy_str(p->detail, sizeof(p->detail), detail);
	ex_unlock(&p->lock);
	if (changed) {
		notify(EX_CAP_DEPTH);
	}
}

static uint32_t
get_state_locked(struct ex_provider *p)
{
	ex_lock(&p->lock);
	const uint32_t s = p->state;
	ex_unlock(&p->lock);
	return s;
}

static bool
never_cancelled(void *ctx)
{
	(void)ctx;
	return false;
}

static bool
is_colour_format(uint32_t f)
{
	return f == XRT_CONV_FMT_RGBA8_UNORM || f == XRT_CONV_FMT_RGBA8_SRGB || f == XRT_CONV_FMT_BGRA8_UNORM ||
	       f == XRT_CONV_FMT_BGRA8_SRGB;
}

/*!
 * The "model". VENDOR TODO: run your inference here. @p src is the RGBA/BGRA
 * input (unused by the ramp), @p dst an R32F target of @p w x @p h with row
 * stride @p dst_stride bytes. Returns the decoded range of what it wrote.
 */
static void
run_model(const struct ex_provider *p,
          const uint8_t *src,
          uint32_t src_stride,
          uint32_t w,
          uint32_t h,
          float *dst,
          uint32_t dst_stride,
          float *out_min,
          float *out_max)
{
	(void)src;
	(void)src_stride;
	float lo = INFINITY, hi = -INFINITY;
	for (uint32_t y = 0; y < h; y++) {
		float *row = (float *)((uint8_t *)dst + (size_t)y * dst_stride);
		for (uint32_t x = 0; x < w; x++) {
			// AFFINE depth, larger = farther: 1 at the top (horizon), 0 at the bottom (floor).
			const float t =
			    p->ramp_horizontal ? ((float)x + 0.5f) / (float)w : ((float)y + 0.5f) / (float)h;
			const float v = 1.0f - t;
			row[x] = v;
			lo = v < lo ? v : lo;
			hi = v > hi ? v : hi;
		}
	}
	*out_min = lo;
	*out_max = hi;
}

/*! Honest units for what run_model() produced (frozen struct: fill every field). */
static void
fill_depth_info(struct xrt_conv_depth_info *di, uint32_t w, uint32_t h, float lo, float hi, uint64_t source_time_ns)
{
	memset(di, 0, sizeof(*di));
	di->kind = XRT_CONV_DEPTH_AFFINE;                 // unknown scale + shift, larger = farther
	di->measure = XRT_CONV_MEASURE_PLANAR;            //
	di->intrinsics_source = XRT_CONV_INTRINSICS_NONE; // we know none: leave fx..cy at 0, never invent
	di->value_scale = 1.0f;
	di->value_shift = 0.0f;
	di->invalid_sample = NAN; // no "no data" sample value
	di->range_min = lo;       // this frame's extent BEFORE any normalisation
	di->range_max = hi;
	di->range_p05 = lo + 0.05f * (hi - lo);
	di->range_p95 = lo + 0.95f * (hi - lo);
	di->source_width = w;
	di->source_height = h;
	di->same_inference = 1;
	di->source_time_ns = source_time_ns;
}


/*
 *
 * Provider iface: status, probe, describe, state.
 *
 */

static bool
ex_get_provider_status(struct xrt_conv_provider_status *out)
{
	if (out == NULL) {
		return false;
	}
	// VENDOR TODO: report whether your inference runtime / driver is installed
	// (ADR-045 shape). Answer from memory or a cheap check, <= 100 ms.
	SET(out, struct xrt_conv_provider_status, state, EX_PLATFORM_READY);
	SET(out, struct xrt_conv_provider_status, flags, 0u);
	if (COVERS(out, struct xrt_conv_provider_status, hint)) {
		copy_str(out->hint, sizeof(out->hint), "template provider: CPU ramp, no inference runtime");
	}
	return true;
}

static xrt_result_t
ex_probe(struct xrt_conversion_plugin_instance **out_inst)
{
	if (out_inst == NULL) {
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	char buf[64];
	// ENABLE=0 declines cleanly (the runtime then lists the row as declined).
	if (setting("ENABLE", buf, sizeof(buf)) && strtoul(buf, NULL, 10) == 0) {
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	struct ex_provider *p = (struct ex_provider *)calloc(1, sizeof(*p));
	if (p == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	ex_mutex_init(&p->lock);

	// Settings: read here, once. No model load, no network in probe().
	if (setting("RAMP_DIRECTION", buf, sizeof(buf))) {
		p->ramp_horizontal = strcmp(buf, "horizontal") == 0;
	}
	if (setting("WARM_MS", buf, sizeof(buf))) {
		p->warm_ms = (uint32_t)strtoul(buf, NULL, 10);
	}
	// VENDOR TODO: replace DEPENDENCY with the real check (is your inference
	// runtime installed, which version, does the GPU qualify?).
	if (setting("DEPENDENCY", buf, sizeof(buf))) {
		if (strcmp(buf, "absent") == 0) {
			p->dependency = EX_DEP_ABSENT;
		} else if (strcmp(buf, "too-old") == 0) {
			p->dependency = EX_DEP_TOO_OLD;
		}
	}

	switch (p->dependency) {
	case EX_DEP_ABSENT:
		set_state(p, XRT_CONV_STATE_ABSENT, XRT_CONV_FAIL_NONE, 0, -1.0f,
		          "example: inference runtime not installed");
		break;
	case EX_DEP_TOO_OLD:
		// The version gate: present but older than our floor is VERSION, so the
		// dashboard says "too old", not "missing". Retryable: an upgrade fixes it.
		set_state(p, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_VERSION, 1, -1.0f,
		          "example: inference runtime 0.9 is older than the supported 1.0");
		break;
	default: set_state(p, XRT_CONV_STATE_INSTALLED, XRT_CONV_FAIL_NONE, 0, -1.0f, "example: present, cold"); break;
	}
	*out_inst = (struct xrt_conversion_plugin_instance *)p;
	return XRT_SUCCESS;
}

static void
fill_cap(const struct ex_provider *p, struct xrt_conv_capability *o)
{
#define C(FIELD, VALUE) SET(o, struct xrt_conv_capability, FIELD, VALUE)
#define S(FIELD, VALUE)                                                                                                \
	do {                                                                                                           \
		if (COVERS(o, struct xrt_conv_capability, FIELD)) {                                                    \
			copy_str(o->FIELD, sizeof(o->FIELD), (VALUE));                                                 \
		}                                                                                                      \
	} while (0)
	C(cap_id, EX_CAP_DEPTH);
	C(task, XRT_CONV_TASK_DEPTH_MONO);
	C(lanes, XRT_CONV_LANE_STREAM | XRT_CONV_LANE_JOB);
	C(input_kinds, XRT_CONV_INPUT_MONO);
	C(outputs, XRT_CONV_OUT_DEPTH);
	C(flags, XRT_CONV_CAP_DETERMINISTIC);
	// CPU only: the runtime copies any GPU input to CPU memory for us.
	// VENDOR TODO: add XRT_CONV_MEM_WIN32_NT_HANDLE (+ fence sync) once you
	// import GPU images on your own device.
	C(import_memory_kinds, XRT_CONV_MEM_CPU);
	C(import_sync_kinds, 1u << XRT_CONV_SYNC_NONE);
	C(input_formats, (1u << XRT_CONV_FMT_RGBA8_UNORM) | (1u << XRT_CONV_FMT_RGBA8_SRGB) |
	                     (1u << XRT_CONV_FMT_BGRA8_UNORM) | (1u << XRT_CONV_FMT_BGRA8_SRGB));
	C(depth_output_format, XRT_CONV_FMT_R32_FLOAT);
	C(min_views, 0u);
	C(max_views, 0u);
	C(max_streams, EX_MAX_STREAMS);
	C(max_concurrent, 1u);
	C(max_input_edge, 0u);
	C(preferred_input_edge, 0u);
	C(depth_kind, XRT_CONV_DEPTH_AFFINE);
	C(depth_measure, XRT_CONV_MEASURE_PLANAR);
	C(intrinsics_source, XRT_CONV_INTRINSICS_NONE);
	C(splat_formats, 0u);
	// VENDOR TODO: the licence class covers your model AND every dependency
	// AND the inference runtime you load. RESEARCH_ONLY is selected only after
	// the user opts in.
	C(licence_class, XRT_CONV_LICENCE_PERMISSIVE);
	C(commercial_ok, 1u);
	S(licence_spdx, "BSL-1.0");
	S(model_id, "example/ramp");
	S(model_version, "1");
	S(backend, "cpu-reference");
	S(display_name, "Example depth (ramp)");
	C(device_luid, 0ull); // CPU
	C(est_warmup_ms, p->warm_ms);
	C(est_latency_ms, 1u);
	C(est_vram_mb, 0u);
	C(est_ram_mb, 1u);
#undef C
#undef S
}

static xrt_result_t
ex_describe_cap(struct xrt_conversion_plugin_instance *inst,
                uint32_t index,
                struct xrt_conv_capability *out,
                uint32_t *out_count)
{
	struct ex_provider *p = (struct ex_provider *)inst;
	if (out_count != NULL) {
		*out_count = 1;
	}
	if (out == NULL) {
		return out_count != NULL ? XRT_SUCCESS : XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (p == NULL || index >= 1) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	fill_cap(p, out);
	return XRT_SUCCESS;
}

static uint64_t
ex_get_generation(struct xrt_conversion_plugin_instance *inst)
{
	(void)inst;
	return 1; // the capability set never changes; readiness goes through get_state + notify_changed
}

static xrt_result_t
ex_get_state(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id, struct xrt_conv_state *out)
{
	struct ex_provider *p = (struct ex_provider *)inst;
	if (p == NULL || out == NULL || cap_id != EX_CAP_DEPTH) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	ex_lock(&p->lock);
	SET(out, struct xrt_conv_state, state, p->state);
	SET(out, struct xrt_conv_state, failure_reason, p->failure_reason);
	SET(out, struct xrt_conv_state, retryable, p->retryable);
	SET(out, struct xrt_conv_state, progress, p->progress);
	SET(out, struct xrt_conv_state, eta_ns, 0ull);
	if (COVERS(out, struct xrt_conv_state, version)) {
		copy_str(out->version, sizeof(out->version), "ramp-1");
	}
	if (COVERS(out, struct xrt_conv_state, detail)) {
		copy_str(out->detail, sizeof(out->detail), p->detail);
	}
	ex_unlock(&p->lock);
	return XRT_SUCCESS;
}


/*
 *
 * warm / evict.
 *
 */

static xrt_result_t
ex_warm(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id, bool (*is_cancelled)(void *ctx), void *cancel_ctx)
{
	struct ex_provider *p = (struct ex_provider *)inst;
	if (p == NULL || cap_id != EX_CAP_DEPTH) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (is_cancelled == NULL) {
		is_cancelled = never_cancelled;
	}
	const uint32_t s = get_state_locked(p);
	if (s == XRT_CONV_STATE_READY) {
		return XRT_SUCCESS;
	}
	if (s == XRT_CONV_STATE_ABSENT || s == XRT_CONV_STATE_FAILED) {
		// Terminal for this dependency state; the runtime falls through to the
		// next provider. Re-check the dependency here if it can appear later.
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	// VENDOR TODO: load your model here. It may take seconds: publish progress
	// through get_state and poll is_cancelled at least every 250 ms.
	set_state(p, XRT_CONV_STATE_WARMING, XRT_CONV_FAIL_NONE, 0, 0.0f, "example: warming");
	for (uint32_t t = 0; t < p->warm_ms; t += EX_WARM_STEP_MS) {
		if (is_cancelled(cancel_ctx)) {
			set_state(p, XRT_CONV_STATE_INSTALLED, XRT_CONV_FAIL_NONE, 0, -1.0f, "example: warm cancelled");
			return XRT_SUCCESS;
		}
		ex_sleep_ms(EX_WARM_STEP_MS);
		ex_lock(&p->lock);
		p->progress = (float)(t + EX_WARM_STEP_MS) / (float)p->warm_ms;
		if (p->progress > 1.0f) {
			p->progress = 1.0f;
		}
		ex_unlock(&p->lock);
	}
	set_state(p, XRT_CONV_STATE_READY, XRT_CONV_FAIL_NONE, 0, 1.0f, "example: ready");
	return XRT_SUCCESS;
}

static xrt_result_t
ex_evict(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id)
{
	struct ex_provider *p = (struct ex_provider *)inst;
	if (p == NULL || cap_id != EX_CAP_DEPTH) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	// VENDOR TODO: free the model. Only called with no live stream / job.
	if (get_state_locked(p) == XRT_CONV_STATE_READY) {
		set_state(p, XRT_CONV_STATE_EVICTED, XRT_CONV_FAIL_NONE, 0, -1.0f, "example: evicted");
	}
	return XRT_SUCCESS;
}


/*
 *
 * Stream lane.
 *
 */

static struct ex_stream *
stream_find(struct ex_provider *p, uint64_t id)
{
	for (uint32_t i = 0; i < EX_MAX_STREAMS; i++) {
		if (p->streams[i].used && p->streams[i].id == id) {
			return &p->streams[i];
		}
	}
	return NULL;
}

static xrt_result_t
ex_stream_create(struct xrt_conversion_plugin_instance *inst,
                 uint32_t cap_id,
                 const struct xrt_conv_stream_info *info,
                 uint64_t *out_stream)
{
	struct ex_provider *p = (struct ex_provider *)inst;
	if (p == NULL || info == NULL || out_stream == NULL || cap_id != EX_CAP_DEPTH ||
	    !COVERS(info, struct xrt_conv_stream_info, input_kind)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (get_state_locked(p) != XRT_CONV_STATE_READY) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED; // the runtime creates streams only on a READY cap
	}
	if ((info->input_kind & XRT_CONV_INPUT_MONO) == 0) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	for (uint32_t i = 0; i < EX_MAX_STREAMS; i++) {
		struct ex_stream *s = &p->streams[i];
		if (s->used) {
			continue;
		}
		memset(s, 0, sizeof(*s));
		// Copy only what the caller's struct covers; the rest stays zero.
		const size_t n = info->struct_size < sizeof(s->info) ? info->struct_size : sizeof(s->info);
		memcpy(&s->info, info, n);
		s->info.struct_size = (uint32_t)sizeof(s->info);
		s->used = true;
		s->id = ++p->next_stream_id;
		*out_stream = s->id;
		return XRT_SUCCESS;
	}
	return XRT_ERROR_CLIENT_LIMIT_REACHED;
}

static xrt_result_t
ex_stream_get_output_desc(struct xrt_conversion_plugin_instance *inst,
                          uint64_t stream,
                          uint32_t input_width,
                          uint32_t input_height,
                          struct xrt_conv_output_desc *out)
{
	(void)stream; // pure: the answer depends only on the input extent here
	if (inst == NULL || out == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	// VENDOR TODO: a model with a fixed inference resolution returns that here;
	// the runtime allocates exactly what you describe.
	SET(out, struct xrt_conv_output_desc, depth_width, input_width);
	SET(out, struct xrt_conv_output_desc, depth_height, input_height);
	SET(out, struct xrt_conv_output_desc, depth_format, XRT_CONV_FMT_R32_FLOAT);
	return XRT_SUCCESS;
}

static xrt_result_t
ex_stream_convert(struct xrt_conversion_plugin_instance *inst,
                  uint64_t stream,
                  const struct xrt_conv_frame_in *in,
                  const struct xrt_conv_params *params,
                  struct xrt_conv_frame_out *out)
{
	struct ex_provider *p = (struct ex_provider *)inst;
	if (p == NULL || in == NULL || out == NULL || !COVERS(in, struct xrt_conv_frame_in, images) ||
	    !COVERS(out, struct xrt_conv_frame_out, depth)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct ex_stream *s = stream_find(p, stream);
	if (s == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (get_state_locked(p) != XRT_CONV_STATE_READY) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // the runtime keeps the previous result
	}

	// The first COLOR image is ours.
	const struct xrt_conv_image *color = NULL;
	for (uint32_t i = 0; i < in->image_count && i < XRT_CONV_MAX_FRAME_IMAGES; i++) {
		const uint32_t role = COVERS(in, struct xrt_conv_frame_in, image_role) ? in->image_role[i] : 0u;
		if (role == XRT_CONV_IMG_COLOR) {
			color = &in->images[i];
			break;
		}
	}
	if (color == NULL || color->memory_kind != XRT_CONV_MEM_CPU || color->cpu_ptr == NULL ||
	    !is_colour_format(color->format) || color->width == 0 || color->height == 0) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	const struct xrt_conv_image *dst = &out->depth;
	if (dst->memory_kind != XRT_CONV_MEM_CPU || dst->cpu_ptr == NULL || dst->format != XRT_CONV_FMT_R32_FLOAT ||
	    dst->width == 0 || dst->height == 0) {
		return XRT_ERROR_OUTPUT_UNSUPPORTED;
	}
	const uint32_t src_stride = color->row_stride != 0 ? color->row_stride : color->width * 4u;
	const uint32_t dst_stride = dst->row_stride != 0 ? dst->row_stride : dst->width * 4u;

	// Honour a discontinuity: drop temporal state BEFORE this frame (we have none).
	uint32_t frame_flags = 0;
	uint64_t source_time_ns = 0;
	if (params != NULL) {
		frame_flags = COVERS(params, struct xrt_conv_params, frame_flags) ? params->frame_flags : 0u;
		source_time_ns = COVERS(params, struct xrt_conv_params, source_time_ns) ? params->source_time_ns : 0u;
	}
	if ((frame_flags & XRT_CONV_FRAME_RESET) != 0) {
		s->frames = 0;
	}

	float lo = 0.0f, hi = 0.0f;
	run_model(p, (const uint8_t *)color->cpu_ptr, src_stride, dst->width, dst->height, (float *)dst->cpu_ptr,
	          dst_stride, &lo, &hi);
	// CPU target, sync NONE: the write is complete when we return.

	SET(out, struct xrt_conv_frame_out, written, XRT_CONV_OUT_DEPTH);
	if (COVERS(out, struct xrt_conv_frame_out, depth_info)) {
		fill_depth_info(&out->depth_info, color->width, color->height, lo, hi, source_time_ns);
		out->depth_info.provider_frame_id = s->frames;
	}
	s->frames++;
	return XRT_SUCCESS;
}

static void
ex_stream_destroy(struct xrt_conversion_plugin_instance *inst, uint64_t stream)
{
	struct ex_provider *p = (struct ex_provider *)inst;
	if (p == NULL) {
		return;
	}
	struct ex_stream *s = stream_find(p, stream);
	if (s != NULL) {
		memset(s, 0, sizeof(*s));
	}
}


/*
 *
 * Job lane: one image in, one RAW_IMAGE depth blob out through a runtime sink.
 *
 */

static xrt_result_t
ex_job_execute(struct xrt_conversion_plugin_instance *inst,
               uint32_t cap_id,
               const struct xrt_conv_job_in *in,
               const struct xrt_conv_params *params,
               const struct xrt_conv_job_control *ctl,
               struct xrt_conv_job_out *out)
{
	struct ex_provider *p = (struct ex_provider *)inst;
	if (p == NULL || in == NULL || ctl == NULL || cap_id != EX_CAP_DEPTH ||
	    !COVERS(in, struct xrt_conv_job_in, images) || !COVERS(ctl, struct xrt_conv_job_control, is_cancelled)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (get_state_locked(p) != XRT_CONV_STATE_READY) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	const struct xrt_conv_image *color = in->image_count > 0 ? &in->images[0] : NULL;
	if (color == NULL || color->memory_kind != XRT_CONV_MEM_CPU || color->cpu_ptr == NULL ||
	    !is_colour_format(color->format) || color->width == 0 || color->height == 0) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	struct xrt_conv_blob_sink *sink = NULL;
	for (uint32_t i = 0; i < ctl->sink_count && ctl->sinks != NULL; i++) {
		if (ctl->sinks[i].role == XRT_CONV_OUT_DEPTH) {
			sink = &ctl->sinks[i];
			break;
		}
	}
	if (sink == NULL || sink->begin == NULL || sink->write == NULL) {
		return XRT_ERROR_OUTPUT_UNSUPPORTED;
	}

	const uint32_t w = color->width, h = color->height;
	const uint32_t src_stride = color->row_stride != 0 ? color->row_stride : w * 4u;
	float *img = (float *)malloc((size_t)w * h * sizeof(float));
	if (img == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	if (ctl->progress != NULL) {
		ctl->progress(ctl->ctx, 0.0f, "infer");
	}
	if (ctl->is_cancelled != NULL && ctl->is_cancelled(ctl->ctx)) {
		free(img);
		return XRT_SUCCESS; // cancelled: nothing written, the runtime reports it
	}
	float lo = 0.0f, hi = 0.0f;
	run_model(p, (const uint8_t *)color->cpu_ptr, src_stride, w, h, img, w * 4u, &lo, &hi);
	if (ctl->progress != NULL) {
		ctl->progress(ctl->ctx, 0.9f, "write");
	}
	const uint64_t bytes = (uint64_t)w * h * sizeof(float);
	const bool ok = sink->begin(sink->ctx, XRT_CONV_BLOB_RAW_IMAGE, bytes) && sink->write(sink->ctx, img, bytes);
	free(img);
	if (!ok) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	const uint64_t source_time_ns =
	    params != NULL && COVERS(params, struct xrt_conv_params, source_time_ns) ? params->source_time_ns : 0u;
	if (out != NULL) {
		SET(out, struct xrt_conv_job_out, written, XRT_CONV_OUT_DEPTH);
		if (COVERS(out, struct xrt_conv_job_out, depth_info)) {
			fill_depth_info(&out->depth_info, w, h, lo, hi, source_time_ns);
		}
	}
	if (ctl->progress != NULL) {
		ctl->progress(ctl->ctx, 1.0f, "done");
	}
	return XRT_SUCCESS;
}

static void
ex_destroy(struct xrt_conversion_plugin_instance *inst)
{
	// MAY NEVER BE CALLED (process exit, host kill): hold nothing that must be flushed.
	struct ex_provider *p = (struct ex_provider *)inst;
	if (p == NULL) {
		return;
	}
	ex_mutex_destroy(&p->lock);
	free(p);
}


/*
 *
 * Negotiate: the only export.
 *
 */

static struct xrt_conversion_plugin_iface g_iface = {
    .struct_size = (uint32_t)sizeof(struct xrt_conversion_plugin_iface),
    .id = EX_ID,
    .display_name = "Example Conversion Provider (template)",
    .vendor = "Example",
    .version = "0.1.0",
    .get_provider_status = ex_get_provider_status,
    .probe = ex_probe,
    .describe_cap = ex_describe_cap,
    .get_generation = ex_get_generation,
    .get_state = ex_get_state,
    .warm = ex_warm,
    .evict = ex_evict,
    .stream_create = ex_stream_create,
    .stream_get_output_desc = ex_stream_get_output_desc,
    .stream_convert = ex_stream_convert,
    .stream_destroy = ex_stream_destroy,
    .job_execute = ex_job_execute,
    .destroy = ex_destroy,
};

XRT_CONVERSION_PLUGIN_EXPORT xrt_result_t
xrtConversionPluginNegotiate(uint32_t runtime_api_version,
                             const struct xrt_conversion_plugin_host_iface *host,
                             struct xrt_conversion_plugin_iface **out_iface,
                             uint32_t *out_plugin_api_version)
{
	if (out_iface == NULL || out_plugin_api_version == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	*out_plugin_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	if (runtime_api_version != XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT) {
		// The loader rejects a different major itself; answer honestly anyway.
		*out_iface = NULL;
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	g_host = host; // valid until process end
	*out_iface = &g_iface;
	return XRT_SUCCESS;
}
