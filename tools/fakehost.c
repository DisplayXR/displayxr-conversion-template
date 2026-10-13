// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A fake host for conversion providers: drives a provider binary
 *         through the call sequence the DisplayXR runtime uses, with no
 *         runtime installed, and checks the contract on the way.
 *
 *     example-conversion-fakehost <provider binary> [--set KEY=VALUE]...
 *                                 [--expect ready|absent|version] [--warm-timeout-ms N]
 *
 * Sequence: load the binary by absolute path and resolve the ONE symbol
 * (xrtConversionPluginNegotiate) -> negotiate against a stub host iface ->
 * get_provider_status -> probe -> describe_cap (count, every index at the full
 * struct_size, and once at a truncated struct_size with a guard band) ->
 * validate each descriptor the way `displayxr-cli selftest` does -> get_state
 * -> warm (with a cancel callback) -> for each DEPTH_* capability: a CPU stream
 * (create, output desc, convert a test image, check written / depth_info /
 * values, destroy) and, when it declares the job lane, one job into a byte
 * sink -> evict -> destroy.
 *
 * Settings: `get_setting(KEY)` answers DXR_CONV_<ID>_<KEY> from this process's
 * environment first, then the --set table — the runtime's order (environment,
 * then the registry row / manifest).
 *
 * Exit code: 0 = every check passed, 1 = a check failed, 2 = usage / load error.
 * This is a development aid, not the runtime: passing it does not prove the
 * provider works in the satellite host (see the README's "Testing").
 */

#include "xrt/xrt_conversion_plugin.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <time.h>
#endif


/*
 *
 * Reporting.
 *
 */

static int g_failures = 0;

static void
check(bool ok, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("[%s] ", ok ? "PASS" : "FAIL");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	if (!ok) {
		g_failures++;
	}
}

static void
info(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("       ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
}


/*
 *
 * Portability: load a module by absolute path, sleep, temp dir.
 *
 */

static void *
load_module(const char *path, char *err, size_t err_size)
{
#ifdef _WIN32
	// The runtime loads by absolute path with backslashes and searches the
	// provider's own directory for its dependencies.
	wchar_t wpath[MAX_PATH], wfull[MAX_PATH];
	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH) == 0 ||
	    GetFullPathNameW(wpath, MAX_PATH, wfull, NULL) == 0) {
		snprintf(err, err_size, "bad path");
		return NULL;
	}
	for (wchar_t *c = wfull; *c != 0; c++) {
		if (*c == L'/') {
			*c = L'\\';
		}
	}
	HMODULE m = LoadLibraryExW(wfull, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (m == NULL) {
		snprintf(err, err_size, "LoadLibraryExW failed (error %lu)", (unsigned long)GetLastError());
	}
	return (void *)m;
#else
	char full[PATH_MAX];
	if (realpath(path, full) == NULL) {
		snprintf(err, err_size, "bad path");
		return NULL;
	}
	void *m = dlopen(full, RTLD_NOW | RTLD_LOCAL);
	if (m == NULL) {
		snprintf(err, err_size, "dlopen failed: %s", dlerror());
	}
	return m;
#endif
}

static void *
find_symbol(void *module, const char *name)
{
#ifdef _WIN32
	return (void *)GetProcAddress((HMODULE)module, name);
#else
	return dlsym(module, name);
#endif
}

static void
sleep_ms(uint32_t ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
	nanosleep(&ts, NULL);
#endif
}


/*
 *
 * The stub host iface.
 *
 */

#define MAX_SETTINGS 32

static struct
{
	char key[64];
	char value[256];
} g_settings[MAX_SETTINGS];
static int g_setting_count = 0;
static char g_env_prefix[96] = "";
static int g_notifications = 0;

static bool
host_get_path(uint32_t kind, char *buf, uint32_t buf_size)
{
	if (buf == NULL || buf_size == 0 || kind != XRT_CONV_PATH_SCRATCH) {
		return false;
	}
#ifdef _WIN32
	DWORD n = GetTempPathA(buf_size, buf);
	return n > 0 && n < buf_size;
#else
	const char *t = getenv("TMPDIR");
	snprintf(buf, buf_size, "%s", t != NULL ? t : "/tmp");
	return true;
#endif
}

static bool
host_get_setting(const char *key, char *buf, uint32_t buf_size)
{
	if (key == NULL || buf == NULL || buf_size == 0) {
		return false;
	}
	char env_name[192];
	snprintf(env_name, sizeof(env_name), "%s%s", g_env_prefix, key);
	const char *e = getenv(env_name);
	if (e != NULL) {
		snprintf(buf, buf_size, "%s", e);
		return true;
	}
	for (int i = 0; i < g_setting_count; i++) {
		if (strcmp(g_settings[i].key, key) == 0) {
			snprintf(buf, buf_size, "%s", g_settings[i].value);
			return true;
		}
	}
	return false;
}

static bool
host_get_adapter_hint(struct xrt_conv_adapter_hint *inout)
{
	if (inout == NULL) {
		return false;
	}
	inout->flags = 0;
	inout->preferred_luid = 0; // no GPU preference: the fake host is CPU only
	inout->avoid_luid = 0;
	return true;
}

static bool
host_model_resolve(const char *ref, struct xrt_conv_model_location *out)
{
	(void)ref;
	(void)out;
	return false; // no model store in the fake host
}

static bool
host_model_request(const char *ref)
{
	(void)ref;
	return false;
}

static void
host_notify_changed(uint32_t cap_id)
{
	(void)cap_id;
	g_notifications++;
}

static struct xrt_conversion_plugin_host_iface g_host = {
    .struct_size = (uint32_t)sizeof(struct xrt_conversion_plugin_host_iface),
    .host_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT,
    .host_flags = 2u, // bit1: headless / CI
    .get_path = host_get_path,
    .get_setting = host_get_setting,
    .get_adapter_hint = host_get_adapter_hint,
    .model_resolve = host_model_resolve,
    .model_request = host_model_request,
    .notify_changed = host_notify_changed,
};

static bool
never_cancelled(void *ctx)
{
	(void)ctx;
	return false;
}


/*
 *
 * Descriptor validation (the selftest's rules).
 *
 */

static const char *
task_name(uint32_t t)
{
	static const char *names[] = {"none",       "depth-mono",       "depth-video", "disparity-stereo",
	                              "views-mono", "views-from-depth", "splats-mono", "splats-stereo"};
	return t < sizeof(names) / sizeof(names[0]) ? names[t] : "?";
}

static const char *
state_name(uint32_t s)
{
	static const char *names[] = {"UNKNOWN", "READY",    "WARMING",   "ABSENT",      "INCOMPATIBLE", "UNLICENSED",
	                              "FAILED",  "DISABLED", "INSTALLED", "DOWNLOADING", "EVICTED"};
	return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

static void
validate_cap(const struct xrt_conv_capability *c, uint32_t index)
{
	check(c->cap_id != 0, "cap %u: cap_id is non-zero (%u)", index, c->cap_id);
	check(c->task >= XRT_CONV_TASK_DEPTH_MONO && c->task <= XRT_CONV_TASK_SPLATS_STEREO,
	      "cap %u: task is a v1 task (%u = %s)", index, c->task, task_name(c->task));
	check(c->lanes != 0 && (c->lanes & ~(XRT_CONV_LANE_STREAM | XRT_CONV_LANE_JOB)) == 0,
	      "cap %u: lanes set and known (0x%x)", index, c->lanes);
	check(c->min_views <= c->max_views && c->max_views <= XRT_CONV_MAX_VIEWS,
	      "cap %u: min_views %u <= max_views %u <= %d", index, c->min_views, c->max_views, XRT_CONV_MAX_VIEWS);
	if (c->task == XRT_CONV_TASK_VIEWS_MONO || c->task == XRT_CONV_TASK_VIEWS_FROM_DEPTH) {
		check((c->flags & XRT_CONV_CAP_EXPLICIT_VIEWPOINTS) != 0, "cap %u: VIEWS_* sets EXPLICIT_VIEWPOINTS",
		      index);
	}
	check(c->licence_class <= XRT_CONV_LICENCE_RESEARCH_ONLY, "cap %u: licence class known (%u)", index,
	      c->licence_class);
	check((c->import_memory_kinds & XRT_CONV_MEM_CPU) != 0, "cap %u: imports XRT_CONV_MEM_CPU", index);
	if ((c->outputs & XRT_CONV_OUT_DEPTH) != 0) {
		check(c->depth_kind != XRT_CONV_DEPTH_UNKNOWN, "cap %u: a depth output declares its depth kind", index);
	}
	info("cap %u: id %u, %s, model '%s' v%s, backend '%s', licence %s (%u), commercial_ok %u", index, c->cap_id,
	     task_name(c->task), c->model_id, c->model_version, c->backend, c->licence_spdx, c->licence_class,
	     c->commercial_ok);
}


/*
 *
 * Lanes.
 *
 */

struct sink_ctx
{
	uint32_t format;
	uint64_t bytes;
	bool begun;
};

static bool
sink_begin(void *ctx, uint32_t blob_format, uint64_t size_hint)
{
	struct sink_ctx *s = (struct sink_ctx *)ctx;
	(void)size_hint;
	s->format = blob_format;
	s->begun = true;
	return true;
}

static bool
sink_write(void *ctx, const void *bytes, uint64_t count)
{
	struct sink_ctx *s = (struct sink_ctx *)ctx;
	(void)bytes;
	s->bytes += count;
	return true;
}

static void
job_progress(void *ctx, float fraction, const char *stage)
{
	(void)ctx;
	info("job progress %.2f (%s)", fraction, stage != NULL ? stage : "");
}

static bool
job_is_cancelled(void *ctx)
{
	(void)ctx;
	return false;
}

//! A deterministic RGBA8 test image (a colour gradient).
static uint8_t *
make_image(uint32_t w, uint32_t h)
{
	uint8_t *img = (uint8_t *)malloc((size_t)w * h * 4u);
	if (img == NULL) {
		return NULL;
	}
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			uint8_t *px = img + ((size_t)y * w + x) * 4u;
			px[0] = (uint8_t)(255u * x / (w - 1));
			px[1] = (uint8_t)(255u * y / (h - 1));
			px[2] = 128;
			px[3] = 255;
		}
	}
	return img;
}

static void
cpu_image(struct xrt_conv_image *img, void *ptr, uint32_t w, uint32_t h, uint32_t format, uint32_t bpp, uint64_t key)
{
	memset(img, 0, sizeof(*img));
	img->memory_kind = XRT_CONV_MEM_CPU;
	img->import_key = key;
	img->cpu_ptr = ptr;
	img->byte_size = (uint64_t)w * h * bpp;
	img->width = w;
	img->height = h;
	img->format = format;
	img->color_space = bpp == 4 ? XRT_CONV_CS_SRGB : XRT_CONV_CS_DATA;
	img->row_stride = w * bpp;
	img->sync.kind = XRT_CONV_SYNC_NONE;
}

static void
run_stream(struct xrt_conversion_plugin_iface *iface,
           struct xrt_conversion_plugin_instance *inst,
           const struct xrt_conv_capability *cap)
{
	const uint32_t W = 64, H = 48;
	struct xrt_conv_stream_info si;
	memset(&si, 0, sizeof(si));
	si.struct_size = (uint32_t)sizeof(si);
	si.input_kind = XRT_CONV_INPUT_MONO;
	si.outputs = XRT_CONV_OUT_DEPTH;
	si.content_hint = cap->task == XRT_CONV_TASK_DEPTH_VIDEO ? XRT_CONV_CONTENT_VIDEO : XRT_CONV_CONTENT_PHOTO;
	si.max_input_width = W;
	si.max_input_height = H;
	uint64_t stream = 0;
	xrt_result_t r = iface->stream_create(inst, cap->cap_id, &si, &stream);
	check(r == XRT_SUCCESS, "stream_create on cap %u (result %d)", cap->cap_id, (int)r);
	if (r != XRT_SUCCESS) {
		return;
	}

	struct xrt_conv_output_desc od;
	memset(&od, 0, sizeof(od));
	od.struct_size = (uint32_t)sizeof(od);
	r = iface->stream_get_output_desc(inst, stream, W, H, &od);
	check(r == XRT_SUCCESS && od.depth_width > 0 && od.depth_height > 0 &&
	          od.depth_format == XRT_CONV_FMT_R32_FLOAT,
	      "stream_get_output_desc: depth %ux%u format %u", od.depth_width, od.depth_height, od.depth_format);
	if (r != XRT_SUCCESS || od.depth_width == 0 || od.depth_height == 0) {
		iface->stream_destroy(inst, stream);
		return;
	}

	uint8_t *src = make_image(W, H);
	float *dst = (float *)calloc((size_t)od.depth_width * od.depth_height, sizeof(float));
	if (src == NULL || dst == NULL) {
		check(false, "allocate test images");
		free(src);
		free(dst);
		iface->stream_destroy(inst, stream);
		return;
	}
	for (size_t i = 0; i < (size_t)od.depth_width * od.depth_height; i++) {
		dst[i] = NAN; // detect unwritten texels
	}

	struct xrt_conv_frame_in in;
	memset(&in, 0, sizeof(in));
	in.struct_size = (uint32_t)sizeof(in);
	in.image_count = 1;
	in.image_role[0] = XRT_CONV_IMG_COLOR;
	cpu_image(&in.images[0], src, W, H, XRT_CONV_FMT_RGBA8_UNORM, 4, 1);

	struct xrt_conv_params params;
	memset(&params, 0, sizeof(params));
	params.struct_size = (uint32_t)sizeof(params);
	params.frame_flags = XRT_CONV_FRAME_RESET; // first frame of a stream
	params.source_time_ns = 123456789ull;
	params.frame_id = 1;
	params.strength = 1.0f;
	params.content_hint = si.content_hint;
	params.input_scale = 1.0f;
	params.focal_px = 0.0f; // unknown: a provider must not invent one

	struct xrt_conv_frame_out out;
	memset(&out, 0, sizeof(out));
	out.struct_size = (uint32_t)sizeof(out);
	cpu_image(&out.depth, dst, od.depth_width, od.depth_height, XRT_CONV_FMT_R32_FLOAT, 4, 2);

	r = iface->stream_convert(inst, stream, &in, &params, &out);
	check(r == XRT_SUCCESS, "stream_convert (result %d)", (int)r);
	if (r == XRT_SUCCESS) {
		check((out.written & XRT_CONV_OUT_DEPTH) != 0, "written has DEPTH (0x%x)", out.written);
		const struct xrt_conv_depth_info *di = &out.depth_info;
		check(di->kind != XRT_CONV_DEPTH_UNKNOWN && di->kind <= XRT_CONV_DEPTH_STEREO_DISPARITY_PX,
		      "depth_info.kind is known (%u) and matches the capability (%u)", di->kind, cap->depth_kind);
		check(di->kind == cap->depth_kind, "depth_info.kind == capability depth_kind");
		check(di->source_time_ns == params.source_time_ns, "depth_info echoes source_time_ns");
		check(di->range_max >= di->range_min, "per-frame range is ordered [%g, %g]", (double)di->range_min,
		      (double)di->range_max);
		const bool affine = di->kind == XRT_CONV_DEPTH_AFFINE || di->kind == XRT_CONV_DEPTH_AFFINE_INVERSE ||
		                    di->kind == XRT_CONV_DEPTH_AFFINE_DISPARITY;
		if (affine) {
			check(di->fx <= 0.0f && di->fy <= 0.0f, "affine depth invents no focal length");
		}
		if (di->intrinsics_source == XRT_CONV_INTRINSICS_NONE) {
			check(di->fx <= 0.0f && di->cx <= 0.0f, "intrinsics NONE leaves fx..cy unset");
		}
		size_t bad = 0;
		float lo = INFINITY, hi = -INFINITY;
		for (size_t i = 0; i < (size_t)od.depth_width * od.depth_height; i++) {
			if (!isfinite(dst[i])) {
				bad++;
			} else {
				lo = dst[i] < lo ? dst[i] : lo;
				hi = dst[i] > hi ? dst[i] : hi;
			}
		}
		check(bad == 0, "every depth texel written and finite (%zu bad)", bad);
		check(bad > 0 || (fabsf(lo - di->range_min) < 1e-4f && fabsf(hi - di->range_max) < 1e-4f),
		      "the reported range matches the written values [%g, %g]", (double)lo, (double)hi);
	}
	iface->stream_destroy(inst, stream);
	free(src);
	free(dst);
}

static void
run_job(struct xrt_conversion_plugin_iface *iface,
        struct xrt_conversion_plugin_instance *inst,
        const struct xrt_conv_capability *cap)
{
	const uint32_t W = 32, H = 24;
	uint8_t *src = make_image(W, H);
	if (src == NULL) {
		check(false, "allocate job image");
		return;
	}
	struct xrt_conv_job_in in;
	memset(&in, 0, sizeof(in));
	in.struct_size = (uint32_t)sizeof(in);
	in.input_kind = XRT_CONV_INPUT_MONO;
	in.image_count = 1;
	in.image_role[0] = XRT_CONV_IMG_COLOR;
	cpu_image(&in.images[0], src, W, H, XRT_CONV_FMT_RGBA8_UNORM, 4, 3);

	struct xrt_conv_params params;
	memset(&params, 0, sizeof(params));
	params.struct_size = (uint32_t)sizeof(params);
	params.content_hint = XRT_CONV_CONTENT_PHOTO;
	params.strength = 1.0f;

	struct sink_ctx sc;
	memset(&sc, 0, sizeof(sc));
	struct xrt_conv_blob_sink sink;
	memset(&sink, 0, sizeof(sink));
	sink.struct_size = (uint32_t)sizeof(sink);
	sink.role = XRT_CONV_OUT_DEPTH;
	sink.ctx = &sc;
	sink.begin = sink_begin;
	sink.write = sink_write;

	struct xrt_conv_job_control ctl;
	memset(&ctl, 0, sizeof(ctl));
	ctl.struct_size = (uint32_t)sizeof(ctl);
	ctl.sink_count = 1;
	ctl.sinks = &sink;
	ctl.progress = job_progress;
	ctl.is_cancelled = job_is_cancelled;

	struct xrt_conv_job_out out;
	memset(&out, 0, sizeof(out));
	out.struct_size = (uint32_t)sizeof(out);

	xrt_result_t r = iface->job_execute(inst, cap->cap_id, &in, &params, &ctl, &out);
	check(r == XRT_SUCCESS, "job_execute on cap %u (result %d)", cap->cap_id, (int)r);
	if (r == XRT_SUCCESS) {
		check(sc.begun && sc.format == XRT_CONV_BLOB_RAW_IMAGE, "job wrote a RAW_IMAGE blob (format %u)",
		      sc.format);
		check(sc.bytes == (uint64_t)out.depth_info.source_width * out.depth_info.source_height * 4u &&
		          sc.bytes > 0,
		      "job blob size matches depth_info (%llu bytes)", (unsigned long long)sc.bytes);
		check((out.written & XRT_CONV_OUT_DEPTH) != 0, "job written has DEPTH");
	}
	free(src);
}


/*
 *
 * main.
 *
 */

static void
usage(void)
{
	fprintf(stderr,
	        "usage: example-conversion-fakehost <provider binary> [--set KEY=VALUE]...\n"
	        "                                   [--expect ready|absent|version] [--warm-timeout-ms N]\n");
}

int
main(int argc, char **argv)
{
	const char *path = NULL;
	const char *expect = "ready";
	uint32_t warm_timeout_ms = 10000;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--set") == 0 && i + 1 < argc) {
			const char *kv = argv[++i];
			const char *eq = strchr(kv, '=');
			if (eq == NULL || g_setting_count >= MAX_SETTINGS) {
				usage();
				return 2;
			}
			snprintf(g_settings[g_setting_count].key, sizeof(g_settings[0].key), "%.*s", (int)(eq - kv),
			         kv);
			snprintf(g_settings[g_setting_count].value, sizeof(g_settings[0].value), "%s", eq + 1);
			g_setting_count++;
		} else if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc) {
			expect = argv[++i];
		} else if (strcmp(argv[i], "--warm-timeout-ms") == 0 && i + 1 < argc) {
			warm_timeout_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else if (path == NULL && argv[i][0] != '-') {
			path = argv[i];
		} else {
			usage();
			return 2;
		}
	}
	if (path == NULL) {
		usage();
		return 2;
	}

	char err[256] = "";
	void *module = load_module(path, err, sizeof(err));
	if (module == NULL) {
		fprintf(stderr, "cannot load %s: %s\n", path, err);
		return 2;
	}
	xrt_conversion_plugin_negotiate_fn_t negotiate =
	    (xrt_conversion_plugin_negotiate_fn_t)find_symbol(module, XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME);
	check(negotiate != NULL, "exports %s", XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME);
	if (negotiate == NULL) {
		return 1;
	}

	struct xrt_conversion_plugin_iface *iface = NULL;
	uint32_t plugin_api = 0;
	xrt_result_t r = negotiate(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT, &g_host, &iface, &plugin_api);
	check(r == XRT_SUCCESS && iface != NULL, "negotiate (result %d)", (int)r);
	check(plugin_api == XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT, "ABI major %u == %u", plugin_api,
	      XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT);
	if (r != XRT_SUCCESS || iface == NULL || plugin_api != XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT) {
		return 1;
	}
	check(iface->struct_size >= sizeof(struct xrt_conversion_plugin_iface), "iface struct_size %u covers v1 (%u)",
	      iface->struct_size, (unsigned)sizeof(struct xrt_conversion_plugin_iface));
	check(iface->id != NULL && iface->id[0] != '\0' && iface->display_name != NULL,
	      "iface id and display_name set");
	check(iface->get_provider_status != NULL && iface->probe != NULL && iface->describe_cap != NULL &&
	          iface->get_generation != NULL && iface->get_state != NULL && iface->warm != NULL &&
	          iface->evict != NULL && iface->stream_create != NULL && iface->stream_get_output_desc != NULL &&
	          iface->stream_convert != NULL && iface->stream_destroy != NULL && iface->job_execute != NULL &&
	          iface->destroy != NULL,
	      "every v1 slot is set");
	if (iface->id == NULL || iface->probe == NULL || iface->describe_cap == NULL || iface->get_state == NULL) {
		return 1;
	}
	info("provider '%s' (%s), vendor %s, version %s", iface->id, iface->display_name,
	     iface->vendor != NULL ? iface->vendor : "-", iface->version != NULL ? iface->version : "-");

	// DXR_CONV_<ID>_ prefix: id upper-cased, '-' -> '_'.
	size_t n = (size_t)snprintf(g_env_prefix, sizeof(g_env_prefix), "DXR_CONV_");
	for (const char *c = iface->id; *c != '\0' && n + 2 < sizeof(g_env_prefix); c++) {
		g_env_prefix[n++] = *c == '-' ? '_' : (char)toupper((unsigned char)*c);
	}
	g_env_prefix[n++] = '_';
	g_env_prefix[n] = '\0';

	if (iface->get_provider_status != NULL) {
		struct xrt_conv_provider_status ps;
		memset(&ps, 0, sizeof(ps));
		ps.struct_size = (uint32_t)sizeof(ps);
		check(iface->get_provider_status(&ps), "get_provider_status");
		info("provider status %u: %s", ps.state, ps.hint);
	}

	struct xrt_conversion_plugin_instance *inst = NULL;
	r = iface->probe(&inst);
	check(r == XRT_SUCCESS, "probe (result %d)", (int)r);
	if (r != XRT_SUCCESS) {
		return 1;
	}

	uint32_t count = 0;
	r = iface->describe_cap(inst, 0, NULL, &count);
	check(r == XRT_SUCCESS && count >= 1 && count <= XRT_CONV_MAX_CAPABILITIES, "describe_cap count = %u", count);
	if (r != XRT_SUCCESS || count == 0 || count > XRT_CONV_MAX_CAPABILITIES) {
		return 1;
	}
	const uint64_t gen0 = iface->get_generation(inst);

	struct xrt_conv_capability caps[XRT_CONV_MAX_CAPABILITIES];
	for (uint32_t i = 0; i < count; i++) {
		memset(&caps[i], 0, sizeof(caps[i]));
		caps[i].struct_size = (uint32_t)sizeof(caps[i]);
		r = iface->describe_cap(inst, i, &caps[i], NULL);
		check(r == XRT_SUCCESS, "describe_cap(%u)", i);
		validate_cap(&caps[i], i);
		for (uint32_t j = 0; j < i; j++) {
			check(caps[j].cap_id != caps[i].cap_id, "cap_id %u is unique", caps[i].cap_id);
		}
	}
	{
		// Truncation: a runtime built against an older, shorter struct must never be written past.
		const size_t trunc = offsetof(struct xrt_conv_capability, licence_class);
		unsigned char buf[sizeof(struct xrt_conv_capability) + 64];
		memset(buf, 0xCD, sizeof(buf));
		struct xrt_conv_capability *c = (struct xrt_conv_capability *)(void *)buf;
		memset(c, 0, trunc);
		c->struct_size = (uint32_t)trunc;
		(void)iface->describe_cap(inst, 0, c, NULL);
		bool clean = true;
		for (size_t k = trunc; k < sizeof(buf); k++) {
			clean = clean && buf[k] == 0xCD;
		}
		check(clean, "describe_cap writes nothing past a truncated struct_size (%zu bytes)", trunc);
	}
	{
		struct xrt_conv_capability past;
		memset(&past, 0, sizeof(past));
		past.struct_size = (uint32_t)sizeof(past);
		check(iface->describe_cap(inst, count, &past, NULL) != XRT_SUCCESS && past.cap_id == 0,
		      "describe_cap(index == count) is refused and writes nothing");
	}
	check(iface->get_generation(inst) == gen0, "generation stable across the walk");

	// Readiness before warm.
	struct xrt_conv_state st;
	memset(&st, 0, sizeof(st));
	st.struct_size = (uint32_t)sizeof(st);
	r = iface->get_state(inst, caps[0].cap_id, &st);
	check(r == XRT_SUCCESS && st.state <= XRT_CONV_STATE_EVICTED, "get_state: %s (reason %u, retryable %u) '%s'",
	      state_name(st.state), st.failure_reason, st.retryable, st.detail);

	if (strcmp(expect, "absent") == 0) {
		check(st.state == XRT_CONV_STATE_ABSENT, "expected ABSENT");
	} else if (strcmp(expect, "version") == 0) {
		check(st.state == XRT_CONV_STATE_FAILED && st.failure_reason == XRT_CONV_FAIL_VERSION,
		      "expected FAILED(VERSION), never FAILED(MODEL)");
	} else {
		check(st.state == XRT_CONV_STATE_INSTALLED || st.state == XRT_CONV_STATE_READY ||
		          st.state == XRT_CONV_STATE_EVICTED,
		      "cold state before warm is INSTALLED / READY / EVICTED");
		for (uint32_t i = 0; i < count; i++) {
			r = iface->warm(inst, caps[i].cap_id, never_cancelled, NULL);
			check(r == XRT_SUCCESS, "warm cap %u (result %d)", caps[i].cap_id, (int)r);
			uint32_t waited = 0;
			do {
				memset(&st, 0, sizeof(st));
				st.struct_size = (uint32_t)sizeof(st);
				(void)iface->get_state(inst, caps[i].cap_id, &st);
				if (st.state == XRT_CONV_STATE_READY) {
					break;
				}
				sleep_ms(20);
				waited += 20;
			} while (waited < warm_timeout_ms);
			check(st.state == XRT_CONV_STATE_READY, "cap %u READY after warm (%s)", caps[i].cap_id,
			      state_name(st.state));
		}
		info("notify_changed calls so far: %d", g_notifications);
		check(g_notifications > 0, "the provider pushes state changes (notify_changed)");

		for (uint32_t i = 0; i < count; i++) {
			const bool depth =
			    caps[i].task == XRT_CONV_TASK_DEPTH_MONO || caps[i].task == XRT_CONV_TASK_DEPTH_VIDEO;
			if (!depth) {
				info("cap %u: %s is not exercised by this fake host (depth tasks only)", caps[i].cap_id,
				     task_name(caps[i].task));
				continue;
			}
			if ((caps[i].lanes & XRT_CONV_LANE_STREAM) != 0) {
				run_stream(iface, inst, &caps[i]);
			}
			if ((caps[i].lanes & XRT_CONV_LANE_JOB) != 0) {
				run_job(iface, inst, &caps[i]);
			}
		}

		for (uint32_t i = 0; i < count; i++) {
			check(iface->evict(inst, caps[i].cap_id) == XRT_SUCCESS, "evict cap %u", caps[i].cap_id);
			memset(&st, 0, sizeof(st));
			st.struct_size = (uint32_t)sizeof(st);
			(void)iface->get_state(inst, caps[i].cap_id, &st);
			check(st.state == XRT_CONV_STATE_EVICTED || st.state == XRT_CONV_STATE_INSTALLED,
			      "cap %u EVICTED / INSTALLED after evict (%s)", caps[i].cap_id, state_name(st.state));
		}
	}

	iface->destroy(inst);
	printf("%s: %d check(s) failed\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
	return g_failures == 0 ? 0 : 1;
}
