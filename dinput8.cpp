// dinput8.cpp -- WoW 3.3.5a (this specific custom build) mouselook fix.
// Build as 32-bit DLL named dinput8.dll, place next to the game exe.
//
// How it works (verified against the binary):
//   * Mouselook state is the DWORD at 0xD4156C (==1 while looking), set by 0x86A020.
//   * On every WM_MOUSEMOVE while looking, the WndProc (0x86A619..) calls the
//     GetCursorPos resolver 0x868C10 -> GetPhysicalCursorPos, diffs against the anchor
//     at 0xD413EC/0xD413F0, emits a mouse-delta event, then 0x869DB0 recenters
//     the cursor. Hardware movement between the read and the recenter is lost.
//   * The resolver caches the function pointer in 0xD41590. We pre-fill that cache with
//     our own function, so no detour library or code patching is needed.
//   * Our function returns (anchor + raw-input deltas accumulated since the last call),
//     so the game's own delta math receives exact, lossless deltas.
//
// The exe has no ASLR (no DYNAMICBASE), so these absolute addresses are stable.

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <unordered_map>

// ---- dinput8.dll proxy: forward the real exports lazily ----
// The real system dinput8.dll is loaded on first use (not in DllMain), then each
// export forwards to it. The game only imports DirectInput8Create.
static HMODULE g_realDI;
static FARPROC RealDI(const char* name) {
    if (!g_realDI) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (!n || n > MAX_PATH - 16) return 0;
        lstrcatA(path, "\\dinput8.dll");
        g_realDI = LoadLibraryA(path);
        if (!g_realDI) return 0;
    }
    return GetProcAddress(g_realDI, name);
}

extern "C" {
HRESULT WINAPI fwd_DirectInput8Create(HINSTANCE h, DWORD ver, const void* riid, LPVOID* out, LPUNKNOWN outer) {
    typedef HRESULT (WINAPI *F)(HINSTANCE, DWORD, const void*, LPVOID*, LPUNKNOWN);
    F f = (F)RealDI("DirectInput8Create");
    return f ? f(h, ver, riid, out, outer) : E_FAIL;
}
HRESULT WINAPI fwd_DllCanUnloadNow() {
    typedef HRESULT (WINAPI *F)();
    F f = (F)RealDI("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
HRESULT WINAPI fwd_DllGetClassObject(const void* clsid, const void* riid, LPVOID* out) {
    typedef HRESULT (WINAPI *F)(const void*, const void*, LPVOID*);
    F f = (F)RealDI("DllGetClassObject");
    return f ? f(clsid, riid, out) : E_FAIL;
}
HRESULT WINAPI fwd_DllRegisterServer() {
    typedef HRESULT (WINAPI *F)();
    F f = (F)RealDI("DllRegisterServer");
    return f ? f() : E_FAIL;
}
HRESULT WINAPI fwd_DllUnregisterServer() {
    typedef HRESULT (WINAPI *F)();
    F f = (F)RealDI("DllUnregisterServer");
    return f ? f() : E_FAIL;
}
}

// ---- tunables ----
// Raw counts -> pixels multiplier. Read once at startup from the environment variable
// WOW_MOUSE_SCALE (e.g. "1.0", "0.75", "1.5"). Unset/invalid -> 1.0. Clamped to 0.05..20.
// Alternatively just use the in-game sensitivity slider.
static float g_scale = 1.0f;
static void LoadScale() {
    char buf[64];
    DWORD n = GetEnvironmentVariableA("WOW_MOUSE_SCALE", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return;
    char* end = 0;
    double v = strtod(buf, &end);
    if (end == buf || !(v == v)) return;              // not a number
    if (v < 0.05) v = 0.05;
    if (v > 20.0) v = 20.0;
    g_scale = (float)v;
}

// ---- game addresses (fixed image base 0x400000) ----
#define ADDR_LOOK_MODE      0xD4156C   // ==1 while mouselook is active
#define ADDR_ANCHOR_X       0xD413EC
#define ADDR_ANCHOR_Y       0xD413F0
#define ADDR_RESOLVER_CACHE 0xD41590   // cached GetPhysicalCursorPos/GetCursorPos pointer
#define ADDR_RESOLVER_FUNC  0x868C10

typedef BOOL (WINAPI *GetPos_t)(LPPOINT);
static GetPos_t g_realGet;

static volatile LONG g_dx, g_dy;       // whole-pixel deltas accumulated by WM_INPUT
static float g_remX, g_remY;           // fractional carry (only touched on raw-input thread)

static inline bool LookActive() { return *(volatile DWORD*)ADDR_LOOK_MODE == 1; }

// Replacement for GetPhysicalCursorPos, called only from the game's mouselook path.
static BOOL WINAPI hGetPos(LPPOINT p) {
    if (!p) return g_realGet(p);
    if (!LookActive()) return g_realGet(p);                  // not looking: untouched
    LONG dx = InterlockedExchange(&g_dx, 0);
    LONG dy = InterlockedExchange(&g_dy, 0);
    p->x = *(volatile LONG*)ADDR_ANCHOR_X + dx;
    p->y = *(volatile LONG*)ADDR_ANCHOR_Y + dy;
    return TRUE;
}

static bool OurProcessFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static LRESULT CALLBACK RawWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT) {
        if (!LookActive() || !OurProcessFocused()) {         // drop anything outside mouselook
            InterlockedExchange(&g_dx, 0); InterlockedExchange(&g_dy, 0);
            g_remX = g_remY = 0.0f;
        } else {
            RAWINPUT ri; UINT sz = sizeof(ri);
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                ri.header.dwType == RIM_TYPEMOUSE &&
                !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                float fx = ri.data.mouse.lLastX * g_scale + g_remX;
                float fy = ri.data.mouse.lLastY * g_scale + g_remY;
                LONG ix = (LONG)fx, iy = (LONG)fy;           // truncate toward zero
                g_remX = fx - ix; g_remY = fy - iy;
                InterlockedExchangeAdd(&g_dx, ix);
                InterlockedExchangeAdd(&g_dy, iy);
            }
        }
    }
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI RawThread(LPVOID) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = RawWndProc;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "WoWRawMouse";
    RegisterClassA(&wc);
    HWND hw = CreateWindowA(wc.lpszClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, wc.hInstance, 0);
    RAWINPUTDEVICE rid = { 0x01, 0x02, RIDEV_INPUTSINK, hw };   // generic desktop / mouse
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}

// ---- cvar limit raising -------------------------------------------------------------
// Both limits are float constants in .rdata read only by the respective clamp code
// (verified: 0x9E8CFC is referenced only by the groundEffectDist handler at 0x78DB2F;
//  0xA3E710 only by the farclip clamp at 0x780770). We rewrite them at attach time.
//
//   WOW_FARCLIP_MAX      max value accepted for SetCVar("farclip")          default 3200
//   WOW_GROUND_DIST_MAX  max value accepted for SetCVar("groundEffectDist") default 512
//
// A value at or below the original limit leaves that limit untouched.
// groundEffectDensity is deliberately NOT raised: see README notes (256-entry table).
static float EnvFloat(const char* name, float def, float lo, float hi) {
    char buf[64];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return def;
    char* end = 0;
    double v = strtod(buf, &end);
    if (end == buf || !(v == v)) return def;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (float)v;
}

static void PatchFloat(DWORD addr, float expectedOld, float newVal) {
    float cur;
    memcpy(&cur, (void*)addr, 4);
    if (cur != expectedOld) return;                    // different build: do nothing
    if (!(newVal > expectedOld)) return;               // only ever raise
    DWORD oldProt;
    if (!VirtualProtect((void*)addr, 4, PAGE_READWRITE, &oldProt)) return;
    memcpy((void*)addr, &newVal, 4);
    VirtualProtect((void*)addr, 4, oldProt, &oldProt);
}

static void PatchLimits() {
    // groundEffectDist: handler rejects values > [0x9E8CFC] (140.0)
    PatchFloat(0x9E8CFC, 140.0f, EnvFloat("WOW_GROUND_DIST_MAX", 512.0f, 140.0f, 4000.0f));
    // farclip: clamp(value, 183.33 [0xA3E708], max [0xA3E710]=1583.33 normal, [0xA3E70C]=791.67 low-memory)
    // Only the normal-memory max is raised; the low-memory protective cap is left alone.
    PatchFloat(0xA3E710, 1583.3333740234375f, EnvFloat("WOW_FARCLIP_MAX", 3200.0f, 1583.34f, 20000.0f));
}

static void InstallPatch() {
    // Safety: only act on the exact binary analysed (resolver prologue + empty cache).
    static const BYTE sig[] = { 0x55, 0x8B, 0xEC, 0xA1, 0x90, 0x15, 0xD4, 0x00 };
    if (memcmp((void*)ADDR_RESOLVER_FUNC, sig, sizeof(sig)) != 0) return;
    if (*(DWORD*)ADDR_RESOLVER_CACHE != 0) return;

    HMODULE u = GetModuleHandleA("user32.dll");       // already mapped: exe imports USER32
    if (!u) return;
    g_realGet = (GetPos_t)GetProcAddress(u, "GetPhysicalCursorPos");
    if (!g_realGet) g_realGet = (GetPos_t)GetProcAddress(u, "GetCursorPos");
    if (!g_realGet) return;

    // The resolver does: eax = [cache]; if (eax) return eax(args);  -> use our function.
    *(volatile DWORD*)ADDR_RESOLVER_CACHE = (DWORD)(uintptr_t)&hGetPos;
    LoadScale();
    CreateThread(0, 0, RawThread, 0, 0, 0);
}


// ---- ground effect distance falloff (generation-time scaling + regeneration) ------------
//
// Verified in the binary:
//   0x7D3390  chunk ground-effect generator, thiscall(chunk), plain ret. Its loops read the
//             density global [0xCD773C] (4 sites inside). Called from exactly one place.
//   0x7D3FE0  per-chunk draw/update, thiscall(chunk), plain ret. If chunk+0xA4 (effect set)
//             is null it calls the generator, then queues the set for rendering.
//   0x7B3960  effect-set destructor, cdecl(set). It unlinks the set from the game's lists and
//             zeroes chunk+0xA4 itself (7B3560). The game's own "free all" (79E744) does the same.
//   chunk+0x3C/40/44 = chunk position, 0xCD8F5C/60/64 = camera position (same space, yards).
//   0xADEEB8 = current groundEffectDist cvar (float).
//
// Tuned defaults (cvar cap 512, start 140, front-loaded power 2.56, 16 tiers): total generated density equals
// the old 256-distance/256-density setup (1.00x) while reaching about 416 yards. Tier count barely changes that
// parity (8 tiers: power 2.52, 32 tiers: power 2.57); 1.25x budget needs power ~1.82.
// Behaviour: density stays 100% out to (groundEffectDist * WOW_GROUND_FALLOFF_FRACTION) yards,
// then falls to 0 at the groundEffectDist cvar value along a curve (see TierForDist),
// quantised into WOW_GROUND_TIERS steps. Default fraction is 1/6, so the start tracks the cvar live. Each chunk
// remembers the tier it was generated at; when it needs a different tier its set is freed
// and the game regenerates it through the normal path (rate-limited per ~frame).
//
//   WOW_GROUND_STAGE         0 = off, 1 = generation-time scaling only, 2 = + regeneration (default 2)
//   WOW_GROUND_FALLOFF_FRACTION  start of falloff as a fraction of the cvar (default 0.1; only used when
//                            WOW_GROUND_FALLOFF_START is 0, since a non-zero START overrides it;
//                            1.0 disables the falloff entirely)
//   WOW_GROUND_FALLOFF_POWER curve exponent, 0.25..8                        (default 2.56; 1.0 = linear)
//   WOW_GROUND_FALLOFF_SHAPE 0 = accelerating drop (1 - t^p), 1 = front-loaded drop ((1-t)^p) (default 1)
//   WOW_GROUND_FALLOFF_START fixed start in yards (default 140); if >0 it overrides the fraction, 0 = use it.
//                            May be below 140; it only matters once the cvar exceeds 140.
//   The whole algorithm is OFF while groundEffectDist <= 140 (the GUI maximum). Chunks thinned
//   earlier are restored to full density if the cvar is lowered back to 140 or less.
//   WOW_GROUND_TIERS         number of density steps, 2..32                 (default 16)
//   WOW_GROUND_REGEN_BUDGET  max regenerations per ~16 ms                   (default 6)
//   WOW_GROUND_FADE_START   fraction (0..0.99) of groundEffectDist where the game's per-doodad alpha fade
//                            begins. Default: follows the falloff start (so doodads grow translucent as
//                            they thin out). 0.85 = stock look. Not applied while the cvar is <= 140.
//   WOW_GROUND_STABLE_PLACEMENT  1 = every density tier is an exact prefix of the denser one, so
//                            regeneration no longer re-rolls jitter/rotation (default 1; 0 = old behaviour)
//   WOW_GROUND_FADEIN_MS    fade-in time (default 500; 0 = off) for the doodads ADDED when a chunk moves to a
//                            denser tier, including the last step up to full density. Existing doodads stay
//                            static. A chunk's first generation (even at full density) just appears.
//                            Needs stable placement. Off while groundEffectDist <= 140 like the rest.
//   WOW_GROUND_LOG          1 = write dinput8_groundfx.log next to the exe     (default 0 = no log)

#define ADDR_GEN        0x7D3390
#define ADDR_DRAW       0x7D3FE0
#define ADDR_SET_FREE   0x7B3960
#define ADDR_DENSITY    0xCD773C
#define ADDR_GFX_FLAGS  0xCD774C     // bit 0x100000 = ground effects drawn
#define ADDR_DIST_CVAR  0xADEEB8
#define ADDR_CAMERA     0xCD8F5C

// BEGIN-PURE
// Returns the density tier (0..tiers) for a chunk at distance d. tiers = full density. 0 = nothing generated
// (the curve reaches zero before the cvar distance, which is what keeps long distances affordable).
//   t = (d - start) / (maxDist - start), 0..1 across the falloff region.
//   shape 0 (default): s = 1 - t^power   -> drop accelerates with distance (power 1 = linear)
//   shape 1:           s = (1 - t)^power -> drops fastest right after the start, then eases off
static int TierForDist(float d, float maxDist, float start, int tiers, float power, int shape) {
    if (maxDist <= start || d <= start) return tiers;     // no falloff region / inside it
    if (d >= maxDist) return 0;
    float t = (d - start) / (maxDist - start);
    float s = shape ? powf(1.0f - t, power) : 1.0f - powf(t, power);
    if (s < 0.0f) s = 0.0f;
    int q = (int)floorf(s * (float)tiers + 0.5f);         // nearest tier; 0 is allowed before the cvar distance
    if (q < 0) q = 0;
    if (q > tiers) q = tiers;
    return q;
}
// The GUI can only reach groundEffectDist 140. At or below that the whole algorithm is off
// (stock behaviour); it only engages for values set beyond the GUI limit via SetCVar.
static bool GroundFxActiveFor(float cvar) { return cvar > 140.0f + 0.01f; }
// Fraction of groundEffectDist at which the game's per-doodad alpha fade begins (stock 0.85).
// fixedFrac < 0 means "follow the density falloff start". Result clamped to 0.05..0.99.
static float FadeKFor(float cvar, float fixedFrac, float startAbs, float frac) {
    float k = fixedFrac;
    if (k < 0.0f) k = (startAbs > 0.0f) ? startAbs / cvar : frac;
    if (k < 0.05f) k = 0.05f;
    if (k > 0.99f) k = 0.99f;
    return k;
}
static unsigned DensityForTier(unsigned full, int q, int tiers) {
    if (q >= tiers) return full;
    if (q <= 0) return 0;
    unsigned v = (unsigned)(((unsigned long long)full * (unsigned)q) / (unsigned)tiers);
    return v ? v : 1;
}
// END-PURE

typedef void (__fastcall *ChunkFn)(void* chunk, void* edx);
typedef void (__cdecl *SetFreeFn)(void* set);

static ChunkFn   oGen, oDraw;
static SetFreeFn oSetFree = (SetFreeFn)ADDR_SET_FREE;

static float g_gfStartAbs = 140.0f; // 0 = use fraction
static float g_gfFrac = 0.1f;
static float g_gfPower = 2.56f;
static float g_fadeFixed = -1.0f;      // <0: follow falloff start
static float g_fadeK = 0.85f;          // read by the game at 0x7B1AD1 once patched
static bool  g_fadePatched = false;
static volatile DWORD g_loopADensity = 0;   // density read by generator loop A (cell picking) once patched
static bool  g_stablePlacement = true;
static bool  g_stablePatched = false;
static bool  g_fadeInPatched = false;
static int   g_gfShape = 1;
static int   g_gfTiers = 16;
static int   g_gfBudget = 6;
static std::unordered_map<void*, unsigned char> g_chunkTier;   // render thread only

static unsigned g_nGenScaled, g_nGenFull, g_nGenSkipped, g_nRegenUp, g_nRegenDown, g_nDenied;
static FILE* g_log;
static LARGE_INTEGER g_qf, g_winStart, g_lastLog;

static void GfLog(const char* fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

static void MaybeLogStats() {
    if (!g_log) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if ((now.QuadPart - g_lastLog.QuadPart) < 5 * g_qf.QuadPart) return;
    g_lastLog = now;
    GfLog("stats: gen full=%u scaled=%u skipped(0)=%u | regen up=%u down=%u denied(budget)=%u | tracked chunks=%u",
          g_nGenFull, g_nGenScaled, g_nGenSkipped, g_nRegenUp, g_nRegenDown, g_nDenied, (unsigned)g_chunkTier.size());
}

static bool GfActive() {
    static int last = -1;
    float cv = *(volatile float*)ADDR_DIST_CVAR;
    bool a = GroundFxActiveFor(cv);
    if (g_fadePatched) {                              // keep the game's fade start in sync every frame
        float nk = a ? FadeKFor(cv, g_fadeFixed, g_gfStartAbs, g_gfFrac) : 0.85f;
        if (nk != g_fadeK) g_fadeK = nk;
    }
    if ((int)a != last) {
        last = (int)a;
        GfLog(a ? "groundEffectDist=%.1f (>140): density falloff ACTIVE"
                : "groundEffectDist=%.1f (<=140): density falloff DISABLED (stock behaviour)", cv);
    }
    return a;
}

static inline float ChunkDist(const BYTE* chunk) {
    const float* c = (const float*)(chunk + 0x3C);
    const float* cam = (const float*)ADDR_CAMERA;
    float dx = cam[0] - c[0], dy = cam[1] - c[1], dz = cam[2] - c[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static inline int WantedTier(const BYTE* chunk) {
    float maxD = *(volatile float*)ADDR_DIST_CVAR;
    float start = (g_gfStartAbs > 0.0f) ? g_gfStartAbs : maxD * g_gfFrac;
    return TierForDist(ChunkDist(chunk), maxD, start, g_gfTiers, g_gfPower, g_gfShape);
}

// ---- fade-in of NEW doodads when a chunk moves to a denser (non-full) tier -------------------
// Verified in the binary:
//   0x7B36B0  per-set draw, thiscall(set), plain ret. For each of 4 entries at set+4 (stride 0x24)
//             with a non-zero key it calls 0x7B3390(entry). Only caller: 0x7986A8 (render loop, which
//             keeps its "shader path" flag at [ebp-0x48]).
//   0x7B3390  per-entry draw, thiscall(entry), plain ret; only caller is 0x7B36C7. entry+0 = key,
//             +4/+8 = total vertex/index counts of the entry's doodad instances. 0x7B12B0 fills the
//             index buffer instance by instance, so index range [0, old) is exactly the older doodads
//             and the newer ones follow contiguously. It builds a CGxBatch on its stack
//             (type @[ebp-0x14], start @[ebp-0x10] = 0, count @[ebp-0xC] = [entry+8]) and draws it.
//   Register c9 (block at 0xD1C5A8) is the distance fade; see the fade-start patch below.
// With stable placement a denser tier is a strict superset of the sparser one, so after a tier-up:
//   pass 1: draw the OLD index range with the normal constants (static),
//   pass 2: draw the NEW tail range with c9 scaled by fade progress, then restore c9.
// The sub-range is selected by overriding the batch start/count in a small code cave at 0x7B345C, so
// the game's own buffer preparation (which uses the true counts) is untouched. Nothing is drawn twice.
typedef void (__fastcall *DrawFn)(void* ecx, void* edx);          // thiscall(ecx), no stack args
typedef void (__fastcall *SetConstFn)(void* ecx, void* edx, int type, int start, const float* data, int count);
static DrawFn oDrawSet   = (DrawFn)0x7B36B0;
static DrawFn oDrawEntry = (DrawFn)0x7B3390;

struct FadeRec { LONGLONG birth; DWORD oldIdx[4]; };
static std::unordered_map<void*, FadeRec> g_fade;       // effect set -> pending fade-in of its new tail
static void*  g_regenChunk  = 0;                        // chunk being tier-up regenerated by hDraw
static bool   g_pendingFade = false;
static DWORD  g_pendKey[4], g_pendOld[4];               // old set's entry keys / index counts
static float  g_fadeInMs = 500.0f;

static volatile BYTE  g_ovrActive = 0;                  // read by the code cave at 0x7B345C
static volatile DWORD g_ovrStart  = 0, g_ovrCount = 0;

static inline LONGLONG NowQpc() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return n.QuadPart; }
static inline DWORD* Entry(void* set, int i) { return (DWORD*)((BYTE*)set + 4 + 0x24 * i); }

static void UploadC9(float scale) {
    const float* base = (const float*)0xD1C5A8;           // c9 as computed by the game this frame
    float c9[4] = { base[0] * scale, base[1] * scale, base[2], base[3] };
    void* dev = *(void**)0xC5DF88;
    if (!dev) return;
    SetConstFn fn = (SetConstFn)(*(void***)dev)[0x118 / 4];
    fn(dev, 0, 0 /*vertex shader*/, 9 /*register*/, c9, 1);
}

static inline void DrawRange(DWORD* e, DWORD start, DWORD count) {
    g_ovrStart = start; g_ovrCount = count; g_ovrActive = 1;
    oDrawEntry(e, 0);
    g_ovrActive = 0;
}

extern "C" __attribute__((used)) void __cdecl hDrawSetWrap(void* set, int shaderPath) {
    if (!shaderPath || g_fade.empty()) { oDrawSet(set, 0); return; }
    auto it = g_fade.find(set);
    if (it == g_fade.end()) { oDrawSet(set, 0); return; }
    float t = (float)((double)(NowQpc() - it->second.birth) * 1000.0 / (double)g_qf.QuadPart) / g_fadeInMs;
    if (!(t < 1.0f)) { g_fade.erase(it); oDrawSet(set, 0); return; }     // finished: stock drawing
    if (t < 0.0f) t = 0.0f;
    const FadeRec rec = it->second;

    bool anyTail = false;
    for (int i = 0; i < 4; i++) {                                         // pass 1: old doodads, static
        DWORD* e = Entry(set, i);
        if (!e[0]) continue;
        DWORD total = e[2], old = rec.oldIdx[i] < total ? rec.oldIdx[i] : total;
        if (old == total) { oDrawEntry(e, 0); continue; }                 // nothing new in this entry
        anyTail = true;
        if (old) DrawRange(e, 0, old);
    }
    if (!anyTail) { g_fade.erase(set); return; }
    UploadC9(t);                                                          // pass 2: new doodads, fading in
    for (int i = 0; i < 4; i++) {
        DWORD* e = Entry(set, i);
        if (!e[0]) continue;
        DWORD total = e[2], old = rec.oldIdx[i] < total ? rec.oldIdx[i] : total;
        if (old < total) DrawRange(e, old, total - old);
    }
    UploadC9(1.0f);                                                       // restore the game's constants
}

// Replaces the "call 0x7B36B0" at 0x7986A8. ecx = set. [ebp-0x48] is the render function's shader-path flag.
extern "C" __attribute__((naked, used)) void drawSetStub() {
    __asm__ volatile(
        "pushl -0x48(%ebp)\n\t"
        "pushl %ecx\n\t"
        "call _hDrawSetWrap\n\t"
        "addl $8, %esp\n\t"
        "ret\n\t");
}

// Code cave for 0x7B345C ("mov dword ptr [ebp-0x10], 0" = batch start). When g_ovrActive is set it
// overrides the batch count ([ebp-0xC]) and start ([ebp-0x10]); otherwise it executes the original.
static bool InstallDrawCave() {
    static const BYTE sig[] = { 0xC7, 0x45, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x66, 0xC7, 0x45, 0xF8, 0x00, 0x00 };
    if (memcmp((void*)0x7B345C, sig, sizeof(sig)) != 0) return false;
    BYTE* c = (BYTE*)VirtualAlloc(0, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!c) return false;
    const DWORD back = 0x7B3463;
    BYTE* p = c;
    *p++ = 0x80; *p++ = 0x3D; *(DWORD*)p = (DWORD)(uintptr_t)&g_ovrActive; p += 4; *p++ = 0x00;   // cmp byte [active],0
    BYTE* je = p; *p++ = 0x74; *p++ = 0x00;                                                        // je skip
    *p++ = 0x8B; *p++ = 0x15; *(DWORD*)p = (DWORD)(uintptr_t)&g_ovrCount; p += 4;                  // mov edx,[count]
    *p++ = 0x89; *p++ = 0x55; *p++ = 0xF4;                                                         // mov [ebp-0xC],edx
    *p++ = 0x8B; *p++ = 0x15; *(DWORD*)p = (DWORD)(uintptr_t)&g_ovrStart; p += 4;                  // mov edx,[start]
    *p++ = 0x89; *p++ = 0x55; *p++ = 0xF0;                                                         // mov [ebp-0x10],edx
    *p++ = 0xE9; *(DWORD*)p = back - (DWORD)(uintptr_t)(p + 4); p += 4;                            // jmp back
    je[1] = (BYTE)(p - (je + 2));                                                                  // skip:
    *p++ = 0xC7; *p++ = 0x45; *p++ = 0xF0; *(DWORD*)p = 0; p += 4;                                 // mov dword [ebp-0x10],0
    *p++ = 0xE9; *(DWORD*)p = back - (DWORD)(uintptr_t)(p + 4); p += 4;                            // jmp back
    FlushInstructionCache(GetCurrentProcess(), c, 64);

    DWORD old;
    if (!VirtualProtect((void*)0x7B345C, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(BYTE*)0x7B345C = 0xE9;
    *(DWORD*)0x7B345D = (DWORD)(uintptr_t)c - 0x7B3461;
    *(BYTE*)0x7B3461 = 0x90; *(BYTE*)0x7B3462 = 0x90;
    VirtualProtect((void*)0x7B345C, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)0x7B345C, 7);
    return true;
}

static void __fastcall hGen(void* chunk, void* /*edx*/) {
    volatile DWORD* dens = (volatile DWORD*)ADDR_DENSITY;
    DWORD full = *dens;
    bool active = GfActive();
    int q = active ? WantedTier((const BYTE*)chunk) : g_gfTiers;
    DWORD scaled = (q < g_gfTiers) ? DensityForTier(full, q, g_gfTiers) : full;
    if (active && scaled == 0) {
        // Beyond the useful range: leave chunk+0xA4 null, exactly like the generator's own early-outs
        // (0x7D33AB/0x7D33B8). The caller (0x7D407C) tests for a null set and simply draws nothing.
        g_chunkTier.erase(chunk);
        g_regenChunk = 0; g_pendingFade = false;
        g_nGenSkipped++;
        return;
    }

    // Loop A (cell picking, 0x7D34E5) consumes 2 RNG draws per pick; loop B (placement, 0x7D389F)
    // continues from that RNG state. If A always runs at full density, B starts from the same RNG
    // state at every tier, so a lower tier is an exact prefix of a higher one (no re-rolled jitter).
    // A skipped entirely (0) when nothing will be placed.
    g_loopADensity = (g_stablePatched && scaled != 0) ? full : scaled;

    *dens = scaled;                                    // loop B + everything else reads this (temporary)
    oGen(chunk, 0);
    *dens = full;
    if (scaled < full) g_nGenScaled++; else g_nGenFull++;

    // Fade-in bookkeeping. Only a tier-up regeneration (prepared by hDraw), including the final step up to
    // full density, gets a fade of its newly added doodads. First generations (even at full density),
    // tier-downs and cvar-driven rebuilds just appear.
    if (void* set = *(void**)((BYTE*)chunk + 0xA4)) {
        bool made = false;
        if (g_pendingFade && chunk == g_regenChunk) {
            FadeRec r; r.birth = NowQpc();
            bool ok = true, grew = false;
            for (int i = 0; i < 4; i++) {
                DWORD* e = Entry(set, i);
                r.oldIdx[i] = g_pendOld[i];
                if (g_pendKey[i] && (e[0] != g_pendKey[i] || e[2] < g_pendOld[i])) ok = false;  // not a superset
                if (e[0] && e[2] > g_pendOld[i]) grew = true;
            }
            if (ok && grew) { g_fade[set] = r; made = true; }
        }
        if (!made) g_fade.erase(set);
    }
    g_regenChunk = 0; g_pendingFade = false;

    // Only scaled chunks are tracked; absence means "generated at full density".
    if (q < g_gfTiers && *(void**)((BYTE*)chunk + 0xA4)) g_chunkTier[chunk] = (unsigned char)q;
    else g_chunkTier.erase(chunk);
    MaybeLogStats();
}

static bool TakeRegenBudget() {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    static int used;
    if ((now.QuadPart - g_winStart.QuadPart) * 1000 >= 16 * g_qf.QuadPart) { g_winStart = now; used = 0; }
    if (used >= g_gfBudget) { g_nDenied++; return false; }
    used++;
    return true;
}

static void __fastcall hDraw(void* chunk, void* /*edx*/) {
    BYTE* c = (BYTE*)chunk;
    void* set = *(void**)(c + 0xA4);
    if (set && (*(volatile DWORD*)ADDR_GFX_FLAGS & 0x100000)) {
        bool active = GfActive();
        // When inactive, only chunks still thinned from an earlier active period need work
        // (they are restored to full density); otherwise this is a straight pass-through.
        if (active || !g_chunkTier.empty()) {
            int want = active ? WantedTier(c) : g_gfTiers;
            auto it = g_chunkTier.find(chunk);
            int have = (it == g_chunkTier.end()) ? g_gfTiers : (int)it->second;
            bool up = want > have;
            bool down = want < have - 1;               // one tier of hysteresis when moving away
            if (up && g_fade.count(set)) up = false;   // let a running fade-in finish before the next tier-up
            if ((up || down) && TakeRegenBudget()) {
                g_pendingFade = false;
                if (up && active && g_fadeInMs > 0.0f && g_fadeInPatched && g_stablePatched) {
                    g_pendingFade = true;              // new doodads fade in, including the final step to full density
                    for (int i = 0; i < 4; i++) {
                        DWORD* e = Entry(set, i);
                        g_pendKey[i] = e[0];
                        g_pendOld[i] = e[0] ? e[2] : 0;
                    }
                }
                g_regenChunk = chunk;
                g_fade.erase(set);
                oSetFree(set);                         // game's own destructor (also zeroes chunk+0xA4)
                *(void**)(c + 0xA4) = 0;
                g_chunkTier.erase(chunk);
                if (up) g_nRegenUp++; else g_nRegenDown++;
            }
        }
    }
    oDraw(chunk, 0);                                   // regenerates via hGen if the set is now null
    g_regenChunk = 0; g_pendingFade = false;           // never leak regen state to a later first generation
}

static void* MakeDetour(DWORD target, size_t stolen, void* hook, const BYTE* expect) {
    if (memcmp((void*)target, expect, stolen) != 0) return 0;          // wrong build
    BYTE* t = (BYTE*)VirtualAlloc(0, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    memcpy(t, (void*)target, stolen);                                  // position-independent bytes only
    t[stolen] = 0xE9;
    *(DWORD*)(t + stolen + 1) = (DWORD)(target + stolen) - (DWORD)(t + stolen + 5);
    DWORD old;
    if (!VirtualProtect((void*)target, stolen, PAGE_EXECUTE_READWRITE, &old)) { VirtualFree(t, 0, MEM_RELEASE); return 0; }
    BYTE* p = (BYTE*)target;
    p[0] = 0xE9;
    *(DWORD*)(p + 1) = (DWORD)(uintptr_t)hook - (target + 5);
    for (size_t i = 5; i < stolen; i++) p[i] = 0xCC;
    VirtualProtect((void*)target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, stolen);
    return t;
}

static void InstallGroundFx() {
    int stage = (int)EnvFloat("WOW_GROUND_STAGE", 2.0f, 0.0f, 2.0f);
    if (stage == 0) return;
    g_gfStartAbs = EnvFloat("WOW_GROUND_FALLOFF_START", 140.0f, 0.0f, 4000.0f);
    g_gfFrac     = EnvFloat("WOW_GROUND_FALLOFF_FRACTION", 0.1f, 0.0f, 1.0f);
    g_gfPower    = EnvFloat("WOW_GROUND_FALLOFF_POWER", 2.56f, 0.25f, 8.0f);
    g_gfShape    = (int)EnvFloat("WOW_GROUND_FALLOFF_SHAPE", 1.0f, 0.0f, 1.0f);
    g_gfTiers  = (int)EnvFloat("WOW_GROUND_TIERS", 16.0f, 2.0f, 32.0f);
    g_gfBudget = (int)EnvFloat("WOW_GROUND_REGEN_BUDGET", 6.0f, 1.0f, 100.0f);

    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(0, path, MAX_PATH);
    while (n && path[n - 1] != '\\') n--;
    path[n] = 0;
    lstrcatA(path, "dinput8_groundfx.log");
    if (EnvFloat("WOW_GROUND_LOG", 0.0f, 0.0f, 1.0f) >= 1.0f) g_log = fopen(path, "w");   // off by default
    QueryPerformanceFrequency(&g_qf);
    QueryPerformanceCounter(&g_lastLog);
    g_chunkTier.reserve(8192);

    static const BYTE genSig[]  = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xF8, 0x00, 0x00, 0x00 };
    static const BYTE drawSig[] = { 0x56, 0x8B, 0xF1, 0x83, 0xBE, 0xA8, 0x00, 0x00, 0x00, 0x00 };
    static const BYTE freeSig[] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x35, 0xE4, 0xC4, 0xD1, 0x00 };

    if (memcmp((void*)ADDR_SET_FREE, freeSig, sizeof(freeSig)) != 0) { GfLog("set-free signature mismatch: ground fx disabled"); return; }
    oGen = (ChunkFn)MakeDetour(ADDR_GEN, sizeof(genSig), (void*)&hGen, genSig);
    if (!oGen) { GfLog("generator hook FAILED (signature or protection): ground fx disabled"); return; }
    g_stablePlacement = EnvFloat("WOW_GROUND_STABLE_PLACEMENT", 1.0f, 0.0f, 1.0f) >= 1.0f;
    if (g_stablePlacement) {
        static const BYTE sigGuard[] = { 0x39, 0x35, 0x3C, 0x77, 0xCD, 0x00 };   // 7D34CF: cmp [CD773C], esi
        static const BYTE sigCond[]  = { 0x3B, 0x35, 0x3C, 0x77, 0xCD, 0x00 };   // 7D3863: cmp esi, [CD773C]
        if (memcmp((void*)0x7D34CF, sigGuard, 6) == 0 && memcmp((void*)0x7D3863, sigCond, 6) == 0) {
            g_loopADensity = *(volatile DWORD*)ADDR_DENSITY;     // valid before the game can reach loop A
            DWORD old1, old2;
            if (VirtualProtect((void*)0x7D34D1, 4, PAGE_EXECUTE_READWRITE, &old1) &&
                VirtualProtect((void*)0x7D3865, 4, PAGE_EXECUTE_READWRITE, &old2)) {
                *(DWORD*)0x7D34D1 = (DWORD)(uintptr_t)&g_loopADensity;
                *(DWORD*)0x7D3865 = (DWORD)(uintptr_t)&g_loopADensity;
                VirtualProtect((void*)0x7D34D1, 4, old1, &old1);
                VirtualProtect((void*)0x7D3865, 4, old2, &old2);
                FlushInstructionCache(GetCurrentProcess(), (void*)0x7D34CF, 6);
                FlushInstructionCache(GetCurrentProcess(), (void*)0x7D3863, 6);
                g_stablePatched = true;
            }
        }
        GfLog(g_stablePatched ? "stable placement patch installed (generator loop A pinned to full density)"
                              : "stable placement patch NOT installed (signature mismatch)");
    } else GfLog("stable placement disabled by WOW_GROUND_STABLE_PLACEMENT=0");
    g_fadeInMs = EnvFloat("WOW_GROUND_FADEIN_MS", 500.0f, 0.0f, 10000.0f);
    if (g_fadeInMs > 0.0f && g_stablePatched) {
        bool cave = InstallDrawCave();                         // inert until g_ovrActive is set
        const BYTE* cs = (const BYTE*)0x7986A8;                // must be exactly "call 0x7B36B0"
        DWORD expectRel = (DWORD)0x7B36B0 - (DWORD)(0x7986A8 + 5);
        if (cave && cs[0] == 0xE8 && *(const DWORD*)(cs + 1) == expectRel) {
            DWORD old;
            if (VirtualProtect((void*)(0x7986A8 + 1), 4, PAGE_EXECUTE_READWRITE, &old)) {
                *(DWORD*)(0x7986A8 + 1) = (DWORD)(uintptr_t)&drawSetStub - (DWORD)(0x7986A8 + 5);
                VirtualProtect((void*)(0x7986A8 + 1), 4, old, &old);
                FlushInstructionCache(GetCurrentProcess(), (void*)0x7986A8, 5);
                g_fadeInPatched = true;
            }
        }
        GfLog(g_fadeInPatched ? "fade-in of new doodads installed (%.0f ms)" : "fade-in NOT installed (code mismatch)", g_fadeInMs);
    } else if (g_fadeInMs > 0.0f) GfLog("fade-in needs stable placement: not installed");
    else GfLog("fade-in disabled (WOW_GROUND_FADEIN_MS=0)");
    if (g_gfStartAbs > 0.0f) GfLog("stage %d: generator hook installed (fixed start=%.0f yd, tiers=%d)", stage, g_gfStartAbs, g_gfTiers);
    else GfLog("stage %d: generator hook installed (start=%.3f x groundEffectDist, tiers=%d, power=%.2f, shape=%d)", stage, g_gfFrac, g_gfTiers, g_gfPower, g_gfShape);
    // Alpha-fade start: repoint the one instruction at 0x7B1AD1 (fmul dword ptr [0x9F23D0], the stock
    // 0.85) at our own float. The constant at 0x9F23D0 is shared with two other sites, so it is not edited.
    {
        static const BYTE fadeSig[] = { 0xD8, 0x0D, 0xD0, 0x23, 0x9F, 0x00 };
        float stock; memcpy(&stock, (void*)0x9F23D0, 4);
        g_fadeFixed = EnvFloat("WOW_GROUND_FADE_START", -1.0f, -1.0f, 0.99f);
        if (memcmp((void*)0x7B1AD1, fadeSig, sizeof(fadeSig)) == 0 && stock == 0.85f) {
            DWORD old;
            if (VirtualProtect((void*)0x7B1AD3, 4, PAGE_EXECUTE_READWRITE, &old)) {
                *(DWORD*)0x7B1AD3 = (DWORD)(uintptr_t)&g_fadeK;
                VirtualProtect((void*)0x7B1AD3, 4, old, &old);
                FlushInstructionCache(GetCurrentProcess(), (void*)0x7B1AD1, 6);
                g_fadePatched = true;
                GfLog("fade-start patch installed (%s)", g_fadeFixed >= 0.0f ? "fixed fraction" : "follows falloff start");
            }
        }
        if (!g_fadePatched) GfLog("fade-start patch NOT installed (signature mismatch)");
    }
    if (stage >= 2) {
        oDraw = (ChunkFn)MakeDetour(ADDR_DRAW, sizeof(drawSig), (void*)&hDraw, drawSig);
        GfLog(oDraw ? "regeneration hook installed (budget %d per 16ms)" : "regeneration hook FAILED: running at stage 1", g_gfBudget);
    }
}

BOOL APIENTRY DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InstallPatch();   // single memory write + CreateThread; safe under loader lock
        PatchLimits();    // independent of the mouse patch; each constant is verified first
        InstallGroundFx();
    }
    return TRUE;
}
