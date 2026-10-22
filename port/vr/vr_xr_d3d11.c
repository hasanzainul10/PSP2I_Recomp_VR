/* VR through OpenXR on Windows, Direct3D 11: the theater (see vr.h).
 *
 * One thread owns everything OpenXR: the instance, a D3D11 device of its own on
 * the headset's adapter (the game's renderer may be OpenGL, and the picture
 * arrives as pixels anyway), the session, one quad-layer swapchain and the
 * controller actions. It loops at the headset's rate (xrWaitFrame paces it).
 *
 * Lifecycle: start -> load the loader -> instance -> wait for a headset (the
 * runtime may not have one yet: retried) -> session -> frames. When the runtime
 * ends the session (SteamVR closed, headset lost) the thread starts over from
 * the instance, so VR comes back when the headset does. vr_stop asks the
 * session to exit and waits (bounded) for the thread. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr.h"

#define GAME_W 480
#define GAME_H 272

/* PSP_CTRL_* */
#define P_SELECT   0x0001u
#define P_START    0x0008u
#define P_UP       0x0010u
#define P_RIGHT    0x0020u
#define P_DOWN     0x0040u
#define P_LEFT     0x0080u
#define P_L        0x0100u
#define P_R        0x0200u
#define P_TRIANGLE 0x1000u
#define P_CIRCLE   0x2000u
#define P_CROSS    0x4000u
#define P_SQUARE   0x8000u

/* ---- the loader's functions ------------------------------------------------- */

#define XR_GLOBAL_FUNCS(X) \
    X(xrEnumerateInstanceExtensionProperties) X(xrCreateInstance)
#define XR_INSTANCE_FUNCS(X) \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrGetSystem) X(xrGetSystemProperties) \
    X(xrGetD3D11GraphicsRequirementsKHR) X(xrCreateSession) X(xrDestroySession) X(xrPollEvent) \
    X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession) X(xrCreateReferenceSpace) \
    X(xrDestroySpace) X(xrLocateSpace) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) \
    X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) \
    X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) \
    X(xrStringToPath) X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction) \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrSyncActions) \
    X(xrGetActionStateBoolean) X(xrGetActionStateFloat) X(xrGetActionStateVector2f) X(xrResultToString) \
    X(xrLocateViews)

#define DECL(f) static PFN_##f p_##f;
XR_GLOBAL_FUNCS(DECL)
XR_INSTANCE_FUNCS(DECL)
#undef DECL
static PFN_xrGetInstanceProcAddr p_xrGetInstanceProcAddr;
static HMODULE g_loader;

/* ---- shared with the game thread -------------------------------------------- */

static volatile LONG g_status = VR_OFF;
static volatile LONG g_quit, g_recenter, g_swap_oc;
static HANDLE g_thread;
static char g_dir[600];

static SRWLOCK  g_cfg_lock = SRWLOCK_INIT;
static vr_config g_cfg = { VR_DISTANCE_DEFAULT, VR_WIDTH_DEFAULT, VR_SCALE_DEFAULT, 0 };
static float     g_ipd = VR_IPD_DEFAULT;    /* under g_cfg_lock */
static float     g_tan_fov = 0.7311f;       /* the game's horizontal half-FOV tangent (fovy 45, 480:272) */

static SRWLOCK  g_frame_lock = SRWLOCK_INIT;
/* The latest picture (0xAARRGGBB): 480x272, or k times that when the game
 * renders at a higher internal resolution (up to 4x). */
static uint32_t g_frame[GAME_W * 4 * GAME_H * 4];
static uint32_t g_frame_r[GAME_W * 4 * GAME_H * 4];   /* the right eye's (vr_submit_stereo) */
static int      g_frame_w = GAME_W, g_frame_h = GAME_H;
static int      g_frame_stereo;             /* g_frame is the left eye and g_frame_r the right */
static LONG     g_frame_seq;                /* bumped per vr_submit_frame / vr_submit_stereo(_shared) */
static void    *g_frame_shared;             /* the game's shared texture holds the newest frame (vr_submit_stereo_shared) */
static uint32_t g_frame_head;               /* the head pose the newest stereo frame was drawn with (0: the window) */

/* Head tracking (vr_head_get): the poses handed to the game, by id, in the
 * LOCAL space, so a picture is shown at the pose it was drawn with. */
#define HEAD_RING 128
static SRWLOCK  g_head_lock = SRWLOCK_INIT;
static struct { uint32_t id; XrPosef pose; } g_heads[HEAD_RING];
static vr_head  g_head;                     /* the newest, relative to the reference */
static uint32_t g_head_next;
static XrVector3f g_eye_off[2] = { { -0.032f, 0, 0 }, { 0.032f, 0, 0 } };   /* the eyes in the head's space */
static volatile LONG g_share_failed;        /* it can't be used here: the game sends pixels instead */

static SRWLOCK  g_pad_lock = SRWLOCK_INIT;
static vr_pad   g_pad = { 0, 128, 128, 128, 128, 0 };
static int      g_pad_live;

/* ---- the VR thread's state -------------------------------------------------- */

static XrInstance  g_inst;
static XrSystemId  g_sys;
static XrSession   g_sess;
static XrSpace     g_local, g_view;
static XrSwapchain g_chain;
static XrSwapchainImageD3D11KHR g_images[8];
static uint32_t    g_nimages;
static int         g_chain_w, g_chain_h, g_chain_bgra;
static int         g_chain_filled;           /* an image was released with a picture in it */
static ID3D11Device        *g_dev;
static ID3D11DeviceContext *g_ctx;
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static int   g_running;                      /* between xrBeginSession and xrEndSession */
static int   g_session_over;                 /* the runtime ended the session (start over) */
static int   g_instance_lost;
static int   g_placed;                       /* the screen has a pose */
static XrPosef g_screen_pose;
static float g_ref_yaw;                      /* the head's heading and position at recenter (head tracking's origin) */
static XrVector3f g_ref_pos;
static LONG  g_uploaded_seq = -1;
static uint32_t *g_upload;                   /* the converted, scaled picture */

/* The port's overlay (engine menu, FPS counter) as its own layer over the
 * screen (vr_submit_overlay): then the stereo pictures can stay on the GPU. */
static uint32_t g_ovl[GAME_W * GAME_H];      /* 0xAARRGGBB, alpha 0 = nothing (under g_frame_lock) */
static int      g_ovl_on;                    /* it has something to show */
static LONG     g_ovl_seq, g_ovl_uploaded = -1;
static XrSwapchain g_ochain;
static XrSwapchainImageD3D11KHR g_oimages[8];
static uint32_t g_noimages;
static int      g_ochain_s, g_ochain_filled;
static uint32_t *g_oupload;

/* Actions */
static XrActionSet g_aset;
enum { A_LSTICK, A_RSTICK, A_CROSS, A_CIRCLE, A_SQUARE, A_TRIANGLE, A_LTRIG, A_RTRIG,
       A_LGRIP, A_RGRIP, A_START, A_LCLICK, A_RCLICK, A_COUNT };
static XrAction g_act[A_COUNT];
static int g_ltrig_on, g_rtrig_on;           /* trigger hysteresis */
static int g_lclick_prev;

static const char *res_name(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_inst && p_xrResultToString && p_xrResultToString(g_inst, r, buf) == XR_SUCCESS) return buf;
    snprintf(buf, sizeof buf, "XrResult %d", (int)r);
    return buf;
}

#define XR_CHECK(call, what) do { XrResult r_ = (call); \
    if (XR_FAILED(r_)) { fprintf(stderr, "vr: %s failed: %s\n", what, res_name(r_)); return -1; } } while (0)

const char *vr_status_name(int s) {
    switch (s) {
    case VR_OFF:        return "OFF";
    case VR_NO_LOADER:  return "NO OPENXR LOADER";
    case VR_NO_RUNTIME: return "NO OPENXR RUNTIME";
    case VR_NO_HEADSET: return "NO HEADSET";
    case VR_WAITING:    return "WAITING";
    case VR_RUNNING:    return "RUNNING";
    case VR_FAILED:     return "FAILED";
    default:            return "?";
    }
}

static void set_status(int s) {
    if (InterlockedExchange(&g_status, s) != s) fprintf(stderr, "vr: %s\n", vr_status_name(s));
}

/* ---- loader ----------------------------------------------------------------- */

static int load_loader(void) {
    if (g_loader) return 0;
    char path[700];
    snprintf(path, sizeof path, "%s/openxr_loader.dll", g_dir);
    g_loader = LoadLibraryA(path);
    if (!g_loader) g_loader = LoadLibraryA("openxr_loader.dll");
    if (!g_loader) { fprintf(stderr, "vr: openxr_loader.dll not found next to the exe\n"); return -1; }
    p_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)(void (*)(void))GetProcAddress(g_loader, "xrGetInstanceProcAddr");
    if (!p_xrGetInstanceProcAddr) { fprintf(stderr, "vr: openxr_loader.dll has no xrGetInstanceProcAddr\n"); return -1; }
#define LOAD(f) if (XR_FAILED(p_xrGetInstanceProcAddr(XR_NULL_HANDLE, #f, (PFN_xrVoidFunction *)&p_##f))) return -1;
    XR_GLOBAL_FUNCS(LOAD)
#undef LOAD
    return 0;
}

static int has_ext(const XrExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static int g_ext_pico;   /* XR_BD_controller_interaction: the Pico 4 controllers' profile */

static int instance_create(void) {
    uint32_t n = 0;
    XR_CHECK(p_xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL), "xrEnumerateInstanceExtensionProperties");
    XrExtensionProperties *ext = (XrExtensionProperties *)calloc(n ? n : 1, sizeof *ext);
    for (uint32_t i = 0; i < n; i++) ext[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    p_xrEnumerateInstanceExtensionProperties(NULL, n, &n, ext);
    if (!has_ext(ext, n, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) {
        fprintf(stderr, "vr: the OpenXR runtime has no Direct3D 11 support\n");
        free(ext);
        return -1;
    }
    const char *want[4];
    uint32_t nw = 0;
    want[nw++] = XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
    g_ext_pico = has_ext(ext, n, "XR_BD_controller_interaction");
    if (g_ext_pico) want[nw++] = "XR_BD_controller_interaction";
    free(ext);

    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "PSP2i Recomp VR");
    ci.applicationInfo.applicationVersion = 1;
    snprintf(ci.applicationInfo.engineName, sizeof ci.applicationInfo.engineName, "psprecomp");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = nw;
    ci.enabledExtensionNames = want;
    XR_CHECK(p_xrCreateInstance(&ci, &g_inst), "xrCreateInstance");
#define LOAD(f) if (XR_FAILED(p_xrGetInstanceProcAddr(g_inst, #f, (PFN_xrVoidFunction *)&p_##f))) { \
        fprintf(stderr, "vr: the runtime has no %s\n", #f); return -1; }
    XR_INSTANCE_FUNCS(LOAD)
#undef LOAD
    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(p_xrGetInstanceProperties(g_inst, &ip)))
        fprintf(stderr, "vr: runtime %s %u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
                XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    return 0;
}

/* 0: a headset; 1: none yet (try again later); -1: error */
static int system_get(void) {
    XrSystemGetInfo gi = { XR_TYPE_SYSTEM_GET_INFO };
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = p_xrGetSystem(g_inst, &gi, &g_sys);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) return 1;
    if (XR_FAILED(r)) { fprintf(stderr, "vr: xrGetSystem failed: %s\n", res_name(r)); return -1; }
    XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
    if (XR_SUCCEEDED(p_xrGetSystemProperties(g_inst, g_sys, &sp)))
        fprintf(stderr, "vr: headset %s\n", sp.systemName);
    return 0;
}

/* ---- device ----------------------------------------------------------------- */

static int device_create(void) {
    XrGraphicsRequirementsD3D11KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    XR_CHECK(p_xrGetD3D11GraphicsRequirementsKHR(g_inst, g_sys, &req), "xrGetD3D11GraphicsRequirementsKHR");
    IDXGIFactory1 *fac = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac))) { fprintf(stderr, "vr: no DXGI factory\n"); return -1; }
    IDXGIAdapter1 *ad = NULL, *found = NULL;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(fac, i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        IDXGIAdapter1_GetDesc1(ad, &d);
        if (!memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof d.AdapterLuid)) { found = ad; break; }
        IDXGIAdapter1_Release(ad);
    }
    IDXGIFactory1_Release(fac);
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    UINT nl = 0;
    while (nl < 4 && levels[nl] >= req.minFeatureLevel) nl++;
    if (!nl) nl = 1;
    HRESULT hr = D3D11CreateDevice((IDXGIAdapter *)found, found ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   NULL, 0, levels, nl, D3D11_SDK_VERSION, &g_dev, NULL, &g_ctx);
    if (found) IDXGIAdapter1_Release(found);
    if (FAILED(hr)) { fprintf(stderr, "vr: D3D11CreateDevice failed (0x%08lX)\n", (unsigned long)hr); return -1; }
    return 0;
}

/* ---- actions ---------------------------------------------------------------- */

static XrPath path_of(const char *s) {
    XrPath p = XR_NULL_PATH;
    p_xrStringToPath(g_inst, s, &p);
    return p;
}

typedef struct { int act; const char *path; } binding;

static void suggest(const char *profile, const binding *b, int n) {
    XrActionSuggestedBinding sb[A_COUNT * 2];
    for (int i = 0; i < n; i++) { sb[i].action = g_act[b[i].act]; sb[i].binding = path_of(b[i].path); }
    XrInteractionProfileSuggestedBinding s = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    s.interactionProfile = path_of(profile);
    s.suggestedBindings = sb;
    s.countSuggestedBindings = (uint32_t)n;
    XrResult r = p_xrSuggestInteractionProfileBindings(g_inst, &s);
    if (XR_FAILED(r)) fprintf(stderr, "vr: bindings for %s refused: %s\n", profile, res_name(r));
}

static int actions_create(void) {
    XrActionSetCreateInfo si = { XR_TYPE_ACTION_SET_CREATE_INFO };
    snprintf(si.actionSetName, sizeof si.actionSetName, "game");
    snprintf(si.localizedActionSetName, sizeof si.localizedActionSetName, "Game");
    XR_CHECK(p_xrCreateActionSet(g_inst, &si, &g_aset), "xrCreateActionSet");
    static const struct { const char *name, *label; XrActionType type; } DEF[A_COUNT] = {
        { "move",       "Move (left stick)",          XR_ACTION_TYPE_VECTOR2F_INPUT },
        { "camera",     "Camera (right stick)",       XR_ACTION_TYPE_VECTOR2F_INPUT },
        { "cross",      "Cross",                      XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "circle",     "Circle",                     XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "square",     "Square",                     XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "triangle",   "Triangle",                   XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "l",          "L (left trigger)",           XR_ACTION_TYPE_FLOAT_INPUT },
        { "r",          "R (right trigger)",          XR_ACTION_TYPE_FLOAT_INPUT },
        { "dpad_mode",  "Left stick as D-pad (hold)", XR_ACTION_TYPE_FLOAT_INPUT },
        { "select",     "Select",                     XR_ACTION_TYPE_FLOAT_INPUT },
        { "start",      "Start",                      XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "recenter",   "Recenter the screen",        XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "engine_menu","Engine menu",                XR_ACTION_TYPE_BOOLEAN_INPUT },
    };
    for (int i = 0; i < A_COUNT; i++) {
        XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO };
        snprintf(ai.actionName, sizeof ai.actionName, "%s", DEF[i].name);
        snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "%s", DEF[i].label);
        ai.actionType = DEF[i].type;
        XR_CHECK(p_xrCreateAction(g_aset, &ai, &g_act[i]), "xrCreateAction");
    }

    /* Positional, as on a PlayStation pad: bottom face button Cross, right
     * Circle, left Square, top Triangle (Circle/Cross swap in the menu). */
    static const binding TOUCH[] = {
        { A_LSTICK, "/user/hand/left/input/thumbstick" },  { A_RSTICK, "/user/hand/right/input/thumbstick" },
        { A_CROSS, "/user/hand/right/input/a/click" },      { A_CIRCLE, "/user/hand/right/input/b/click" },
        { A_SQUARE, "/user/hand/left/input/x/click" },      { A_TRIANGLE, "/user/hand/left/input/y/click" },
        { A_LTRIG, "/user/hand/left/input/trigger/value" }, { A_RTRIG, "/user/hand/right/input/trigger/value" },
        { A_LGRIP, "/user/hand/left/input/squeeze/value" }, { A_RGRIP, "/user/hand/right/input/squeeze/value" },
        { A_START, "/user/hand/left/input/menu/click" },
        { A_LCLICK, "/user/hand/left/input/thumbstick/click" }, { A_RCLICK, "/user/hand/right/input/thumbstick/click" },
    };
    suggest("/interaction_profiles/oculus/touch_controller", TOUCH, (int)(sizeof TOUCH / sizeof TOUCH[0]));
    if (g_ext_pico) suggest("/interaction_profiles/bytedance/pico4_controller", TOUCH, (int)(sizeof TOUCH / sizeof TOUCH[0]));
    static const binding INDEX[] = {
        { A_LSTICK, "/user/hand/left/input/thumbstick" },  { A_RSTICK, "/user/hand/right/input/thumbstick" },
        { A_CROSS, "/user/hand/right/input/a/click" },      { A_CIRCLE, "/user/hand/right/input/b/click" },
        { A_SQUARE, "/user/hand/left/input/a/click" },      { A_TRIANGLE, "/user/hand/left/input/b/click" },
        { A_LTRIG, "/user/hand/left/input/trigger/value" }, { A_RTRIG, "/user/hand/right/input/trigger/value" },
        { A_LGRIP, "/user/hand/left/input/squeeze/value" }, { A_RGRIP, "/user/hand/right/input/squeeze/value" },
        { A_START, "/user/hand/left/input/trackpad/force" },
        { A_LCLICK, "/user/hand/left/input/thumbstick/click" }, { A_RCLICK, "/user/hand/right/input/thumbstick/click" },
    };
    suggest("/interaction_profiles/valve/index_controller", INDEX, (int)(sizeof INDEX / sizeof INDEX[0]));
    static const binding VIVE[] = {     /* no face buttons: trackpads as the sticks */
        { A_LSTICK, "/user/hand/left/input/trackpad" },    { A_RSTICK, "/user/hand/right/input/trackpad" },
        { A_LTRIG, "/user/hand/left/input/trigger/value" }, { A_RTRIG, "/user/hand/right/input/trigger/value" },
        { A_LGRIP, "/user/hand/left/input/squeeze/click" }, { A_RGRIP, "/user/hand/right/input/squeeze/click" },
        { A_START, "/user/hand/left/input/menu/click" },    { A_RCLICK, "/user/hand/right/input/menu/click" },
        { A_CROSS, "/user/hand/right/input/trackpad/click" }, { A_LCLICK, "/user/hand/left/input/trackpad/click" },
    };
    suggest("/interaction_profiles/htc/vive_controller", VIVE, (int)(sizeof VIVE / sizeof VIVE[0]));
    static const binding WMR[] = {
        { A_LSTICK, "/user/hand/left/input/thumbstick" },  { A_RSTICK, "/user/hand/right/input/thumbstick" },
        { A_CROSS, "/user/hand/right/input/trackpad/click" }, { A_SQUARE, "/user/hand/left/input/trackpad/click" },
        { A_LTRIG, "/user/hand/left/input/trigger/value" }, { A_RTRIG, "/user/hand/right/input/trigger/value" },
        { A_LGRIP, "/user/hand/left/input/squeeze/click" }, { A_RGRIP, "/user/hand/right/input/squeeze/click" },
        { A_START, "/user/hand/left/input/menu/click" },    { A_RCLICK, "/user/hand/right/input/menu/click" },
        { A_LCLICK, "/user/hand/left/input/thumbstick/click" },
    };
    suggest("/interaction_profiles/microsoft/motion_controller", WMR, (int)(sizeof WMR / sizeof WMR[0]));
    return 0;
}

static float get_float(int a) {
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = g_act[a];
    XrActionStateFloat s = { XR_TYPE_ACTION_STATE_FLOAT };
    return XR_SUCCEEDED(p_xrGetActionStateFloat(g_sess, &gi, &s)) && s.isActive ? s.currentState : 0.0f;
}
static int get_bool(int a) {
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = g_act[a];
    XrActionStateBoolean s = { XR_TYPE_ACTION_STATE_BOOLEAN };
    return XR_SUCCEEDED(p_xrGetActionStateBoolean(g_sess, &gi, &s)) && s.isActive && s.currentState;
}
static XrVector2f get_vec2(int a) {
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = g_act[a];
    XrActionStateVector2f s = { XR_TYPE_ACTION_STATE_VECTOR2F };
    XrVector2f z = { 0, 0 };
    return XR_SUCCEEDED(p_xrGetActionStateVector2f(g_sess, &gi, &s)) && s.isActive ? s.currentState : z;
}

static uint8_t axis_byte(float v) {          /* -1..1 -> 1..255, 128 centre */
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (uint8_t)lroundf(128.0f + v * 127.0f);
}

static int trigger(float v, int *on) {       /* pressed past 0.55, released under 0.45 */
    *on = *on ? v > 0.45f : v > 0.55f;
    return *on;
}

static void input_update(int focused) {
    vr_pad p = { 0, 128, 128, 128, 128, 0 };
    if (focused) {
        XrActiveActionSet as = { g_aset, XR_NULL_PATH };
        XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
        si.countActiveActionSets = 1;
        si.activeActionSets = &as;
        if (XR_FAILED(p_xrSyncActions(g_sess, &si))) focused = 0;
    }
    if (focused) {
        const XrVector2f ls = get_vec2(A_LSTICK), rs = get_vec2(A_RSTICK);
        if (get_float(A_LGRIP) > 0.5f) {     /* left grip held: the left stick is the D-pad */
            if (ls.y > 0.5f) p.buttons |= P_UP;
            if (ls.y < -0.5f) p.buttons |= P_DOWN;
            if (ls.x < -0.5f) p.buttons |= P_LEFT;
            if (ls.x > 0.5f) p.buttons |= P_RIGHT;
        } else {
            p.ax = axis_byte(ls.x);
            p.ay = axis_byte(-ls.y);         /* the PSP's stick: down is 255 */
        }
        p.rx = axis_byte(rs.x);
        p.ry = axis_byte(-rs.y);
        const int swap = g_swap_oc != 0;
        if (get_bool(A_CROSS))    p.buttons |= swap ? P_CIRCLE : P_CROSS;
        if (get_bool(A_CIRCLE))   p.buttons |= swap ? P_CROSS : P_CIRCLE;
        if (get_bool(A_SQUARE))   p.buttons |= P_SQUARE;
        if (get_bool(A_TRIANGLE)) p.buttons |= P_TRIANGLE;
        if (trigger(get_float(A_LTRIG), &g_ltrig_on)) p.buttons |= P_L;
        if (trigger(get_float(A_RTRIG), &g_rtrig_on)) p.buttons |= P_R;
        if (get_float(A_RGRIP) > 0.5f) p.buttons |= P_SELECT;
        if (get_bool(A_START)) p.buttons |= P_START;
        if (get_bool(A_RCLICK)) p.host |= VR_HOST_MENU;
        const int lc = get_bool(A_LCLICK);
        if (lc && !g_lclick_prev) InterlockedExchange(&g_recenter, 1);
        g_lclick_prev = lc;
    } else {
        g_ltrig_on = g_rtrig_on = g_lclick_prev = 0;
    }
    AcquireSRWLockExclusive(&g_pad_lock);
    g_pad = p;
    g_pad_live = focused;
    ReleaseSRWLockExclusive(&g_pad_lock);
}

/* ---- session ---------------------------------------------------------------- */

static int swapchain_create(int scale) {
    uint32_t n = 0;
    XR_CHECK(p_xrEnumerateSwapchainFormats(g_sess, 0, &n, NULL), "xrEnumerateSwapchainFormats");
    int64_t *fmts = (int64_t *)calloc(n ? n : 1, sizeof *fmts);
    p_xrEnumerateSwapchainFormats(g_sess, n, &n, fmts);
    /* sRGB first: the picture's bytes are sRGB, and a UNORM swapchain is taken
     * as linear by the compositor (washed out). RGBA first: the game's eye
     * textures are RGBA and are copied in as they are (vr_submit_stereo_shared). */
    static const struct { int64_t f; int bgra, srgb; } PREF[] = {
        { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0, 1 }, { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 1, 1 },
        { DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0 },      { DXGI_FORMAT_B8G8R8A8_UNORM, 1, 0 },
    };
    int pick = -1;
    for (int k = 0; k < 4 && pick < 0; k++)
        for (uint32_t i = 0; i < n; i++) if (fmts[i] == PREF[k].f) { pick = k; break; }
    free(fmts);
    if (pick < 0) { fprintf(stderr, "vr: no 8-bit RGBA swapchain format\n"); return -1; }
    if (!PREF[pick].srgb) fprintf(stderr, "vr: no sRGB swapchain format; colours will look washed out\n");
    g_chain_bgra = PREF[pick].bgra;
    g_chain_w = GAME_W * scale * 2;                     /* left eye | right eye */
    g_chain_h = GAME_H * scale;
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = PREF[pick].f;
    ci.sampleCount = 1;
    ci.width = (uint32_t)g_chain_w;
    ci.height = (uint32_t)g_chain_h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XR_CHECK(p_xrCreateSwapchain(g_sess, &ci, &g_chain), "xrCreateSwapchain");
    g_nimages = 0;
    XR_CHECK(p_xrEnumerateSwapchainImages(g_chain, 0, &g_nimages, NULL), "xrEnumerateSwapchainImages");
    if (g_nimages > 8) g_nimages = 8;
    for (uint32_t i = 0; i < g_nimages; i++) g_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    XR_CHECK(p_xrEnumerateSwapchainImages(g_chain, g_nimages, &g_nimages, (XrSwapchainImageBaseHeader *)g_images),
             "xrEnumerateSwapchainImages");
    free(g_upload);
    g_upload = (uint32_t *)malloc((size_t)g_chain_w * g_chain_h * 4);
    g_chain_filled = 0;
    g_uploaded_seq = -1;
    fprintf(stderr, "vr: screen swapchain %dx%d (one picture per eye), %d images\n", g_chain_w, g_chain_h, (int)g_nimages);
    /* The overlay's: the same scale, so its pixels stay sharp. */
    g_ochain_s = scale;
    ci.width = (uint32_t)(GAME_W * scale);
    ci.height = (uint32_t)(GAME_H * scale);
    XR_CHECK(p_xrCreateSwapchain(g_sess, &ci, &g_ochain), "xrCreateSwapchain (overlay)");
    g_noimages = 0;
    XR_CHECK(p_xrEnumerateSwapchainImages(g_ochain, 0, &g_noimages, NULL), "xrEnumerateSwapchainImages");
    if (g_noimages > 8) g_noimages = 8;
    for (uint32_t i = 0; i < g_noimages; i++) g_oimages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    XR_CHECK(p_xrEnumerateSwapchainImages(g_ochain, g_noimages, &g_noimages, (XrSwapchainImageBaseHeader *)g_oimages),
             "xrEnumerateSwapchainImages");
    free(g_oupload);
    g_oupload = (uint32_t *)malloc((size_t)GAME_W * scale * GAME_H * scale * 4);
    g_ochain_filled = 0;
    g_ovl_uploaded = -1;
    return g_upload && g_oupload ? 0 : -1;
}

static int session_create(void) {
    XrGraphicsBindingD3D11KHR gb = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    gb.device = g_dev;
    XrSessionCreateInfo ci = { XR_TYPE_SESSION_CREATE_INFO };
    ci.next = &gb;
    ci.systemId = g_sys;
    XR_CHECK(p_xrCreateSession(g_inst, &ci, &g_sess), "xrCreateSession");
    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XR_CHECK(p_xrCreateReferenceSpace(g_sess, &rs, &g_local), "xrCreateReferenceSpace(LOCAL)");
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(p_xrCreateReferenceSpace(g_sess, &rs, &g_view), "xrCreateReferenceSpace(VIEW)");
    AcquireSRWLockShared(&g_cfg_lock);
    int scale = g_cfg.scale;
    ReleaseSRWLockShared(&g_cfg_lock);
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    if (swapchain_create(scale) != 0) return -1;
    if (actions_create() != 0) return -1;
    XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    at.countActionSets = 1;
    at.actionSets = &g_aset;
    XR_CHECK(p_xrAttachSessionActionSets(g_sess, &at), "xrAttachSessionActionSets");
    g_state = XR_SESSION_STATE_UNKNOWN;
    g_running = g_session_over = 0;
    g_placed = 0;
    return 0;
}

static void session_destroy(void) {
    input_update(0);
    if (g_aset) { p_xrDestroyActionSet(g_aset); g_aset = XR_NULL_HANDLE; }
    memset(g_act, 0, sizeof g_act);
    if (g_chain) { p_xrDestroySwapchain(g_chain); g_chain = XR_NULL_HANDLE; }
    if (g_ochain) { p_xrDestroySwapchain(g_ochain); g_ochain = XR_NULL_HANDLE; }
    if (g_view) { p_xrDestroySpace(g_view); g_view = XR_NULL_HANDLE; }
    if (g_local) { p_xrDestroySpace(g_local); g_local = XR_NULL_HANDLE; }
    if (g_sess) { p_xrDestroySession(g_sess); g_sess = XR_NULL_HANDLE; }
    g_running = 0;
    AcquireSRWLockExclusive(&g_head_lock);
    g_head.id = 0;                                  /* no pose until the next session tracks one */
    ReleaseSRWLockExclusive(&g_head_lock);
}

/* The game's shared eye texture, opened on this thread's device. */
static void *g_shared_src;
static ID3D11Texture2D *g_shared_tex;
static IDXGIKeyedMutex *g_shared_mx;

static void shared_close(void) {
    if (g_shared_mx) IDXGIKeyedMutex_Release(g_shared_mx);
    if (g_shared_tex) ID3D11Texture2D_Release(g_shared_tex);
    g_shared_mx = NULL; g_shared_tex = NULL; g_shared_src = NULL;
}

static void share_fail(const char *why) {
    fprintf(stderr, "vr: the game's eye texture can't be used (%s); pictures go through the CPU\n", why);
    InterlockedExchange(&g_share_failed, 1);
    shared_close();
}

/* Copy the shared texture into a swapchain image: the GPU path of upload(). */
static void upload_shared(void *handle) {
    if (handle != g_shared_src) {
        shared_close();
        if (FAILED(ID3D11Device_OpenSharedResource(g_dev, (HANDLE)handle, &IID_ID3D11Texture2D, (void **)&g_shared_tex)) ||
            FAILED(ID3D11Texture2D_QueryInterface(g_shared_tex, &IID_IDXGIKeyedMutex, (void **)&g_shared_mx))) {
            share_fail("OpenSharedResource failed: another GPU?");
            return;
        }
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc(g_shared_tex, &d);
        if ((int)d.Width != g_chain_w || (int)d.Height != g_chain_h || g_chain_bgra) {
            share_fail("size or format differs from the swapchain");
            return;
        }
        g_shared_src = handle;
    }
    uint32_t idx;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(p_xrAcquireSwapchainImage(g_chain, &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_SUCCEEDED(p_xrWaitSwapchainImage(g_chain, &wi)) && IDXGIKeyedMutex_AcquireSync(g_shared_mx, 0, 50) == S_OK) {
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_images[idx].texture, (ID3D11Resource *)g_shared_tex);
        IDXGIKeyedMutex_ReleaseSync(g_shared_mx, 0);
        g_chain_filled = 1;
    }
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    p_xrReleaseSwapchainImage(g_chain, &ri);
}

static void device_destroy(void) {
    shared_close();
    if (g_ctx) { ID3D11DeviceContext_ClearState(g_ctx); ID3D11DeviceContext_Release(g_ctx); g_ctx = NULL; }
    if (g_dev) { ID3D11Device_Release(g_dev); g_dev = NULL; }
}

static void instance_destroy(void) {
    if (g_inst) { p_xrDestroyInstance(g_inst); g_inst = XR_NULL_HANDLE; }
    g_sys = XR_NULL_SYSTEM_ID;
}

static void poll_events(void) {
    XrEventDataBuffer ev;
    for (;;) {
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        ev.next = NULL;
        if (p_xrPollEvent(g_inst, &ev) != XR_SUCCESS) break;
        if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            fprintf(stderr, "vr: the runtime is going away\n");
            g_instance_lost = 1;
            g_session_over = 1;
        } else if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *sc = (const XrEventDataSessionStateChanged *)&ev;
            g_state = sc->state;
            {
                static const char *const NAMES[] = { "UNKNOWN", "IDLE", "READY", "SYNCHRONIZED", "VISIBLE",
                                                     "FOCUSED", "STOPPING", "LOSS_PENDING", "EXITING" };
                fprintf(stderr, "vr: session state %s\n", (unsigned)g_state < 9 ? NAMES[g_state] : "?");
            }
            switch (g_state) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = p_xrBeginSession(g_sess, &bi);
                if (XR_SUCCEEDED(r)) g_running = 1;
                else { fprintf(stderr, "vr: xrBeginSession failed: %s\n", res_name(r)); g_session_over = 1; }
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                p_xrEndSession(g_sess);
                g_running = 0;
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                g_session_over = 1;
                break;
            default: break;
            }
        }
    }
}

/* ---- the screen ------------------------------------------------------------- */

static XrVector3f qrot(XrQuaternionf q, XrVector3f v) {   /* v rotated by q */
    const XrVector3f u = { q.x, q.y, q.z };
    const XrVector3f t = { 2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x) };
    XrVector3f r = { v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
                     v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
                     v.z + q.w * t.z + (u.x * t.y - u.y * t.x) };
    return r;
}

/* The screen straight ahead of the head (yaw only: level, upright), at the
 * configured distance and eye height. */
static int place_screen(XrTime t) {
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(p_xrLocateSpace(g_view, g_local, t, &loc))) return -1;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return -1;
    XrVector3f head = { 0, 0, 0 };
    if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) head = loc.pose.position;
    const XrVector3f fwd0 = { 0, 0, -1 };
    XrVector3f f = qrot(loc.pose.orientation, fwd0);
    float len = sqrtf(f.x * f.x + f.z * f.z);
    if (len < 1e-3f) { f.x = 0; f.z = -1; len = 1; }            /* looking straight up or down */
    f.x /= len; f.z /= len;
    AcquireSRWLockShared(&g_cfg_lock);
    const float d = g_cfg.distance_m;
    ReleaseSRWLockShared(&g_cfg_lock);
    const float yaw = atan2f(-f.x, -f.z);
    g_ref_yaw = yaw;
    g_ref_pos = head;
    g_screen_pose.orientation.x = 0;
    g_screen_pose.orientation.y = sinf(yaw * 0.5f);
    g_screen_pose.orientation.z = 0;
    g_screen_pose.orientation.w = cosf(yaw * 0.5f);
    g_screen_pose.position.x = head.x + f.x * d;
    g_screen_pose.position.y = head.y;
    g_screen_pose.position.z = head.z + f.z * d;
    g_placed = 1;
    return 0;
}

/* The latest picture into a swapchain image, when it changed. */
static void upload(void) {
    const LONG seq = InterlockedCompareExchange(&g_frame_seq, 0, 0);
    if (seq == g_uploaded_seq || !g_upload) return;
    AcquireSRWLockShared(&g_frame_lock);
    void *shared = g_frame_shared;
    ReleaseSRWLockShared(&g_frame_lock);
    if (shared) { g_uploaded_seq = seq; upload_shared(shared); return; }
    AcquireSRWLockShared(&g_frame_lock);
    const int fw = g_frame_w, fh = g_frame_h, half = g_chain_w / 2;
    const int s = half / fw;                                     /* swapchain pixels per picture pixel */
    if (s < 1 || g_chain_h / fh != s) { ReleaseSRWLockShared(&g_frame_lock); g_uploaded_seq = seq; return; }
    for (int eye = 0; eye < 2; eye++) {                          /* mono: the same picture twice */
        const uint32_t *pic = eye && g_frame_stereo ? g_frame_r : g_frame;
        for (int y = 0; y < fh; y++) {
            const uint32_t *src = &pic[(size_t)y * fw];
            uint32_t *row = &g_upload[(size_t)y * s * g_chain_w + (size_t)eye * half];
            for (int x = 0; x < fw; x++) {
                uint32_t c = src[x] | 0xFF000000u;               /* 0xAARRGGBB = BGRA bytes */
                if (!g_chain_bgra) c = (c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16);
                for (int k = 0; k < s; k++) row[x * s + k] = c;
            }
            for (int k = 1; k < s; k++) memcpy(row + (size_t)k * g_chain_w, row, (size_t)half * 4);
        }
    }
    ReleaseSRWLockShared(&g_frame_lock);
    g_uploaded_seq = seq;

    uint32_t idx;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(p_xrAcquireSwapchainImage(g_chain, &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_SUCCEEDED(p_xrWaitSwapchainImage(g_chain, &wi))) {
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_images[idx].texture, 0, NULL,
                                              g_upload, (UINT)g_chain_w * 4, 0);
        g_chain_filled = 1;
    }
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    p_xrReleaseSwapchainImage(g_chain, &ri);
}

/* The IPD, from the distance between the two eye poses. */
static void update_ipd(XrTime t) {
    XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = t;
    li.space = g_view;
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    XrView v[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t n = 0;
    if (XR_FAILED(p_xrLocateViews(g_sess, &li, &vs, 2, &n, v)) || n != 2) return;
    if (!(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) return;
    const float dx = v[1].pose.position.x - v[0].pose.position.x, dy = v[1].pose.position.y - v[0].pose.position.y,
                dz = v[1].pose.position.z - v[0].pose.position.z;
    const float ipd = sqrtf(dx * dx + dy * dy + dz * dz);
    if (ipd < 0.04f || ipd > 0.09f) return;                      /* not a plausible IPD */
    AcquireSRWLockExclusive(&g_head_lock);
    g_eye_off[0] = v[0].pose.position;
    g_eye_off[1] = v[1].pose.position;
    ReleaseSRWLockExclusive(&g_head_lock);
    AcquireSRWLockExclusive(&g_cfg_lock);
    if (fabsf(ipd - g_ipd) > 0.0005f) {
        g_ipd = ipd;
        fprintf(stderr, "vr: IPD %.1f mm\n", ipd * 1000.0f);
    }
    ReleaseSRWLockExclusive(&g_cfg_lock);
}

static XrQuaternionf qmul(XrQuaternionf a, XrQuaternionf b) {   /* a then b, as one rotation: a * b */
    XrQuaternionf r = { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    return r;
}

/* Head tracking: the head's predicted pose for this headset frame, kept by
 * id (the pictures drawn with it come back with the id) and handed to the
 * game relative to the reference (place_screen). */
static void track_head(XrTime t) {
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(p_xrLocateSpace(g_view, g_local, t, &loc))) return;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) loc.pose.position = g_ref_pos;
    const XrQuaternionf unyaw = { 0, -sinf(g_ref_yaw * 0.5f), 0, cosf(g_ref_yaw * 0.5f) };
    const XrQuaternionf q = qmul(unyaw, loc.pose.orientation);
    const XrVector3f d = { loc.pose.position.x - g_ref_pos.x, loc.pose.position.y - g_ref_pos.y,
                           loc.pose.position.z - g_ref_pos.z };
    const XrVector3f p = qrot(unyaw, d);
    AcquireSRWLockExclusive(&g_head_lock);
    if (++g_head_next == 0) g_head_next = 1;                     /* 0 means none */
    g_heads[g_head_next % HEAD_RING].id = g_head_next;
    g_heads[g_head_next % HEAD_RING].pose = loc.pose;
    g_head.id = g_head_next;
    g_head.q[0] = q.x; g_head.q[1] = q.y; g_head.q[2] = q.z; g_head.q[3] = q.w;
    g_head.p[0] = p.x; g_head.p[1] = p.y; g_head.p[2] = p.z;
    for (int e = 0; e < 2; e++) {
        g_head.eye[e][0] = g_eye_off[e].x; g_head.eye[e][1] = g_eye_off[e].y; g_head.eye[e][2] = g_eye_off[e].z;
    }
    ReleaseSRWLockExclusive(&g_head_lock);
}

/* The LOCAL pose a picture was drawn with, if it is still kept. */
static int head_find(uint32_t id, XrPosef *out) {
    int ok = 0;
    AcquireSRWLockShared(&g_head_lock);
    if (id && g_heads[id % HEAD_RING].id == id) { *out = g_heads[id % HEAD_RING].pose; ok = 1; }
    ReleaseSRWLockShared(&g_head_lock);
    return ok;
}

/* The latest overlay into its swapchain, when it changed. */
static void upload_overlay(void) {
    const LONG seq = InterlockedCompareExchange(&g_ovl_seq, 0, 0);
    if (seq == g_ovl_uploaded || !g_oupload || !g_ochain) return;
    const int s = g_ochain_s, W = GAME_W * s;
    AcquireSRWLockShared(&g_frame_lock);
    for (int y = 0; y < GAME_H; y++) {
        uint32_t *row = &g_oupload[(size_t)y * s * W];
        for (int x = 0; x < GAME_W; x++) {
            uint32_t c = g_ovl[y * GAME_W + x];
            if (!g_chain_bgra) c = (c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16);
            for (int k = 0; k < s; k++) row[x * s + k] = c;
        }
        for (int k = 1; k < s; k++) memcpy(row + (size_t)k * W, row, (size_t)W * 4);
    }
    ReleaseSRWLockShared(&g_frame_lock);
    g_ovl_uploaded = seq;
    uint32_t idx;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(p_xrAcquireSwapchainImage(g_ochain, &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_SUCCEEDED(p_xrWaitSwapchainImage(g_ochain, &wi))) {
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_oimages[idx].texture, 0, NULL,
                                              g_oupload, (UINT)W * 4, 0);
        g_ochain_filled = 1;
    }
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    p_xrReleaseSwapchainImage(g_ochain, &ri);
}

static void frame(void) {
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    XrResult r = p_xrWaitFrame(g_sess, &wi, &fs);
    if (XR_FAILED(r)) { fprintf(stderr, "vr: xrWaitFrame failed: %s\n", res_name(r)); g_session_over = 1; return; }
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    r = p_xrBeginFrame(g_sess, &bi);
    if (XR_FAILED(r)) { fprintf(stderr, "vr: xrBeginFrame failed: %s\n", res_name(r)); g_session_over = 1; return; }

    input_update(g_state == XR_SESSION_STATE_FOCUSED);
    XrCompositionLayerQuad quad[3] = { { XR_TYPE_COMPOSITION_LAYER_QUAD }, { XR_TYPE_COMPOSITION_LAYER_QUAD },
                                       { XR_TYPE_COMPOSITION_LAYER_QUAD } };
    XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
                                               { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    const XrCompositionLayerBaseHeader *layers[3];
    uint32_t nl = 0;
    if (fs.shouldRender) {
        if (InterlockedExchange(&g_recenter, 0) || !g_placed) place_screen(fs.predictedDisplayTime);
        update_ipd(fs.predictedDisplayTime);
        if (g_placed) track_head(fs.predictedDisplayTime);
        upload();
        upload_overlay();
        AcquireSRWLockShared(&g_frame_lock);
        const uint32_t head = g_frame_stereo ? g_frame_head : 0;
        ReleaseSRWLockShared(&g_frame_lock);
        XrPosef hp;
        const int tracked = g_placed && g_chain_filled && head_find(head, &hp);
        static int was_tracked = -1;
        if (g_chain_filled && tracked != was_tracked) {
            fprintf(stderr, "vr: %s\n", tracked ? "pictures at the head pose they were drawn with (projection layer)"
                                                : "pictures on the screen (quad layers)");
            was_tracked = tracked;
        }
        if (tracked) {
            /* Head tracking: the eyes were drawn from the head at pose hp, with
             * the game's FOV; the compositor turns them with the head until
             * the next game frame. Around the game's FOV the view is black. */
            AcquireSRWLockShared(&g_cfg_lock);
            const float ax = atanf(g_tan_fov), ay = atanf(g_tan_fov * (float)GAME_H / (float)GAME_W);
            ReleaseSRWLockShared(&g_cfg_lock);
            AcquireSRWLockShared(&g_head_lock);
            const XrVector3f off[2] = { g_eye_off[0], g_eye_off[1] };
            ReleaseSRWLockShared(&g_head_lock);
            for (int eye = 0; eye < 2; eye++) {
                XrCompositionLayerProjectionView *v = &pv[eye];
                const XrVector3f o = qrot(hp.orientation, off[eye]);
                v->pose.orientation = hp.orientation;
                v->pose.position.x = hp.position.x + o.x;
                v->pose.position.y = hp.position.y + o.y;
                v->pose.position.z = hp.position.z + o.z;
                v->fov.angleLeft = -ax; v->fov.angleRight = ax;
                v->fov.angleUp = ay;    v->fov.angleDown = -ay;
                v->subImage.swapchain = g_chain;
                v->subImage.imageRect.offset.x = eye * (g_chain_w / 2);
                v->subImage.imageRect.extent.width = g_chain_w / 2;
                v->subImage.imageRect.extent.height = g_chain_h;
            }
            proj.space = g_local;
            proj.viewCount = 2;
            proj.views = pv;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&proj;
        } else if (g_placed && g_chain_filled) {
            AcquireSRWLockShared(&g_cfg_lock);
            /* The stereo window is as wide as the game's FOV at its distance:
             * then it shows the game world at its own angular size. */
            const float w = g_cfg.stereo ? 2.0f * g_cfg.distance_m * g_tan_fov : g_cfg.width_m;
            ReleaseSRWLockShared(&g_cfg_lock);
            for (int eye = 0; eye < 2; eye++) {          /* the same screen; each eye its own picture */
                XrCompositionLayerQuad *q = &quad[eye];
                q->space = g_local;
                q->eyeVisibility = eye ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
                q->subImage.swapchain = g_chain;
                q->subImage.imageRect.offset.x = eye * (g_chain_w / 2);
                q->subImage.imageRect.extent.width = g_chain_w / 2;
                q->subImage.imageRect.extent.height = g_chain_h;
                q->pose = g_screen_pose;
                q->size.width = w;
                q->size.height = w * (float)GAME_H / (float)GAME_W;
                layers[nl++] = (const XrCompositionLayerBaseHeader *)q;
            }
        }
        AcquireSRWLockShared(&g_frame_lock);
        const int ovl = g_ovl_on;
        ReleaseSRWLockShared(&g_frame_lock);
        if (nl && ovl && g_ochain_filled) {              /* the overlay on top, both eyes, at screen depth */
            AcquireSRWLockShared(&g_cfg_lock);
            const float w = g_cfg.stereo ? 2.0f * g_cfg.distance_m * g_tan_fov : g_cfg.width_m;
            ReleaseSRWLockShared(&g_cfg_lock);
            XrCompositionLayerQuad *q = &quad[2];
            q->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                            XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            q->space = g_local;
            q->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q->subImage.swapchain = g_ochain;
            q->subImage.imageRect.extent.width = GAME_W * g_ochain_s;
            q->subImage.imageRect.extent.height = GAME_H * g_ochain_s;
            q->pose = g_screen_pose;
            q->size.width = w;
            q->size.height = w * (float)GAME_H / (float)GAME_W;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)q;
        }
    }
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = nl;
    ei.layers = layers;
    r = p_xrEndFrame(g_sess, &ei);
    if (XR_FAILED(r)) { fprintf(stderr, "vr: xrEndFrame failed: %s\n", res_name(r)); g_session_over = 1; }
}

/* One session from creation to its end. Returns 1 when the thread should stop. */
static int run_session(void) {
    if (device_create() != 0 || session_create() != 0) return -1;
    set_status(VR_WAITING);
    ULONGLONG quit_at = 0;
    while (!g_session_over) {
        poll_events();
        if (g_quit) {
            if (!quit_at) {
                quit_at = GetTickCount64();
                if (g_running) p_xrRequestExitSession(g_sess);
                else break;
            }
            if (GetTickCount64() - quit_at > 2000) break;          /* the runtime did not answer: go anyway */
        }
        if (!g_running) { input_update(0); Sleep(10); continue; }
        set_status(g_state == XR_SESSION_STATE_VISIBLE || g_state == XR_SESSION_STATE_FOCUSED ? VR_RUNNING : VR_WAITING);
        frame();
    }
    return g_quit ? 1 : 0;
}

static DWORD WINAPI vr_thread(LPVOID unused) {
    (void)unused;
    if (load_loader() != 0) { set_status(VR_NO_LOADER); return 0; }
    while (!g_quit) {
        g_instance_lost = 0;
        if (instance_create() != 0) { instance_destroy(); set_status(VR_NO_RUNTIME); break; }
        int sys = 0;
        while (!g_quit && (sys = system_get()) == 1) {
            set_status(VR_NO_HEADSET);
            for (int i = 0; i < 30 && !g_quit; i++) Sleep(100);
        }
        if (g_quit) { instance_destroy(); break; }
        if (sys < 0) { instance_destroy(); set_status(VR_FAILED); break; }
        const int rs = run_session();
        session_destroy();
        device_destroy();
        instance_destroy();
        if (rs < 0) { set_status(VR_FAILED); break; }
        if (rs > 0) break;
        fprintf(stderr, "vr: session ended; waiting for the headset again\n");
        set_status(VR_NO_HEADSET);
        for (int i = 0; i < 20 && !g_quit; i++) Sleep(100);
    }
    if (g_quit) set_status(VR_OFF);
    return 0;
}

/* ---- the game thread's side ------------------------------------------------- */

int vr_start(const char *exe_dir, const vr_config *cfg) {
    if (g_thread) return 0;
    snprintf(g_dir, sizeof g_dir, "%s", exe_dir ? exe_dir : ".");
    if (cfg) vr_configure(cfg);
    if (cfg) { AcquireSRWLockExclusive(&g_cfg_lock); g_cfg.scale = cfg->scale; ReleaseSRWLockExclusive(&g_cfg_lock); }
    g_quit = 0;
    g_thread = CreateThread(NULL, 0, vr_thread, NULL, 0, NULL);
    if (!g_thread) { fprintf(stderr, "vr: cannot start the VR thread\n"); return -1; }
    return 0;
}

void vr_stop(void) {
    if (!g_thread) return;
    InterlockedExchange(&g_quit, 1);
    if (WaitForSingleObject(g_thread, 5000) != WAIT_OBJECT_0)
        fprintf(stderr, "vr: the VR thread did not stop in time\n");
    CloseHandle(g_thread);
    g_thread = NULL;
    AcquireSRWLockExclusive(&g_pad_lock);
    g_pad_live = 0;
    ReleaseSRWLockExclusive(&g_pad_lock);
    InterlockedExchange(&g_status, VR_OFF);
}

int vr_status(void) { return (int)InterlockedCompareExchange(&g_status, 0, 0); }

void vr_configure(const vr_config *cfg) {
    AcquireSRWLockExclusive(&g_cfg_lock);
    const int moved = cfg->distance_m != g_cfg.distance_m;
    g_cfg.distance_m = cfg->distance_m;
    g_cfg.width_m = cfg->width_m;
    g_cfg.stereo = cfg->stereo;
    ReleaseSRWLockExclusive(&g_cfg_lock);
    if (moved) InterlockedExchange(&g_recenter, 1);
}

void vr_recenter(void) { InterlockedExchange(&g_recenter, 1); }

void vr_submit_frame(const uint32_t *argb, int w, int h) {
    if (!g_thread || w < GAME_W || w % GAME_W || h * GAME_W != w * GAME_H || w > GAME_W * 4) return;
    AcquireSRWLockExclusive(&g_frame_lock);
    memcpy(g_frame, argb, (size_t)w * h * 4);
    g_frame_w = w; g_frame_h = h;
    g_frame_stereo = 0;
    g_frame_shared = NULL;
    g_frame_head = 0;
    ReleaseSRWLockExclusive(&g_frame_lock);
    InterlockedIncrement(&g_frame_seq);
}

void vr_submit_stereo(const uint32_t *left, const uint32_t *right, int w, int h, float tan_half_fov_x, uint32_t head) {
    if (!g_thread || w < GAME_W || w % GAME_W || h * GAME_W != w * GAME_H || w > GAME_W * 4) return;
    AcquireSRWLockExclusive(&g_frame_lock);
    memcpy(g_frame, left, (size_t)w * h * 4);
    memcpy(g_frame_r, right, (size_t)w * h * 4);
    g_frame_w = w; g_frame_h = h;
    g_frame_stereo = 1;
    g_frame_shared = NULL;
    g_frame_head = head;
    ReleaseSRWLockExclusive(&g_frame_lock);
    if (tan_half_fov_x > 0.1f && tan_half_fov_x < 10.0f) {
        AcquireSRWLockExclusive(&g_cfg_lock);
        g_tan_fov = tan_half_fov_x;
        ReleaseSRWLockExclusive(&g_cfg_lock);
    }
    InterlockedIncrement(&g_frame_seq);
}

void vr_submit_stereo_shared(void *handle, int w, int h, float tan_half_fov_x, uint32_t head) {
    if (!g_thread || !handle || g_share_failed) return;
    AcquireSRWLockExclusive(&g_frame_lock);
    g_frame_shared = handle;
    g_frame_w = w; g_frame_h = h;
    g_frame_stereo = 1;
    g_frame_head = head;
    ReleaseSRWLockExclusive(&g_frame_lock);
    if (tan_half_fov_x > 0.1f && tan_half_fov_x < 10.0f) {
        AcquireSRWLockExclusive(&g_cfg_lock);
        g_tan_fov = tan_half_fov_x;
        ReleaseSRWLockExclusive(&g_cfg_lock);
    }
    InterlockedIncrement(&g_frame_seq);
}

void vr_submit_overlay(const uint32_t *argb) {
    if (!g_thread) return;
    int on = 0;
    if (argb) for (int i = 0; i < GAME_W * GAME_H && !on; i++) on = (argb[i] >> 24) != 0;
    AcquireSRWLockExclusive(&g_frame_lock);
    if (on) memcpy(g_ovl, argb, sizeof g_ovl);
    g_ovl_on = on;
    ReleaseSRWLockExclusive(&g_frame_lock);
    InterlockedIncrement(&g_ovl_seq);
}

int vr_share_failed(void) { return (int)InterlockedCompareExchange(&g_share_failed, 0, 0); }

float vr_ipd(void) {
    AcquireSRWLockShared(&g_cfg_lock);
    const float ipd = g_ipd;
    ReleaseSRWLockShared(&g_cfg_lock);
    return ipd;
}

int vr_head_get(vr_head *out) {
    AcquireSRWLockShared(&g_head_lock);
    *out = g_head;
    ReleaseSRWLockShared(&g_head_lock);
    return out->id != 0;
}

int vr_pad_get(vr_pad *out) {
    AcquireSRWLockShared(&g_pad_lock);
    const int live = g_pad_live;
    *out = g_pad;
    ReleaseSRWLockShared(&g_pad_lock);
    if (!live) { out->buttons = 0; out->ax = out->ay = out->rx = out->ry = 128; out->host = 0; }
    return live;
}

void vr_set_swap_circle_cross(int on) { InterlockedExchange(&g_swap_oc, on ? 1 : 0); }
