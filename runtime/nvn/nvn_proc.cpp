// Star Road Recomp: NVN proc table, logging stubs, runtime hooks (task-12). Our own code (D-008).
#include <algorithm>
#include <array>
#include <utility>

#include "nvn_core.h"
#include "nvn_proc.h"
#include "nvn_rt.h"

namespace nvn {

std::mutex& ApiMutex() { static std::mutex m; return m; }
bool& ApiHeld() { static thread_local bool held = false; return held; }

struct RegRec { std::string name; void* fn; void (*thunk)(Ctx*); std::string sig; };
static std::vector<RegRec>& Regs() { static std::vector<RegRec> r; return r; }

ProcReg::ProcReg(const char* name, void* fn, void (*thunk)(Ctx*), std::string (*sig)()) { Regs().push_back({name, fn, thunk, sig()}); }

// ---- the full name list (535 procs) with generated signatures ---------------------------------------------------------
struct SigRow { const char* name; const char* sig; };
static const SigRow kSigs[] = {
#define NVN_SIG(n, s) {#n, s},
#include "nvn_sigs.inc"
#undef NVN_SIG
};
constexpr size_t kProcN = sizeof(kSigs) / sizeof(kSigs[0]);

// Names the game statically references that stay stubs, and why. Everything else without an implementation is outside what the
// game's code reaches and keeps the generic reason.
struct StubWhy { const char* name; const char* why; };
static const StubWhy kStubWhy[] = {
  {"nvnCommandBufferBindStorageBuffersDeferred", "deferred storage buffer binding has no Vulkan equivalent worth emulating until a translated shader uses it"},
  {"nvnCommandBufferSetViewportSwizzles", "viewport swizzle (NV_viewport_swizzle) is not available on the Vulkan target; the game only sets identity"},
};

static void StubLog(size_t i) {
  LogOnce(kSigs[i].name, "STUB %s called (no implementation; returns 0)", kSigs[i].name);
}
template <size_t N> static void StubThunk(Ctx* c) {
  StubLog(N);
  c->x[0] = 0; c->v[0] = v128{0, 0};
}
template <size_t... Is> static constexpr std::array<void (*)(Ctx*), sizeof...(Is)> MakeStubs(std::index_sequence<Is...>) {
  return {{&StubThunk<Is>...}};
}
static const auto kStubThunks = MakeStubs(std::make_index_sequence<kProcN>{});

static std::vector<ProcEntry>& Table() {
  static std::vector<ProcEntry> t;
  static bool built = false;
  if (!built) {
    built = true;
    LinkAnchorDevice(); LinkAnchorMem(); LinkAnchorCmd();   // pull the API objects out of the static library
    std::unordered_map<std::string, const RegRec*> byName;
    for (auto& r : Regs()) byName[r.name] = &r;
    t.reserve(kProcN);
    for (size_t i = 0; i < kProcN; i++) {
      ProcEntry e{};
      e.name = kSigs[i].name; e.sig = kSigs[i].sig;
      auto it = byName.find(kSigs[i].name);
      if (it != byName.end()) { e.native = it->second->fn; e.thunk = (void (*)(void*))it->second->thunk; e.stubReason = nullptr; e.implSig = it->second->sig.c_str(); }
      else {
        e.native = nullptr; e.thunk = (void (*)(void*))kStubThunks[i];
        e.implSig = nullptr; e.stubReason = "not used by the game's code paths; logs once and returns 0";
        for (auto& w : kStubWhy) if (!strcmp(w.name, e.name)) e.stubReason = w.why;
      }
      t.push_back(e);
    }
    // An implementation registered under a name that is not in the signature list is a typo: surface it loudly.
    for (auto& r : Regs()) if (!ProcFind(r.name.c_str())) Err("NVN_API %s is not in nvn_sigs.inc", r.name.c_str());
  }
  return t;
}

size_t ProcCount() { return Table().size(); }
const ProcEntry* ProcAt(size_t i) { auto& t = Table(); return i < t.size() ? &t[i] : nullptr; }
const ProcEntry* ProcFind(const char* name) {
  static std::unordered_map<std::string, size_t> idx;
  static size_t builtFor = 0;
  auto& t = Table();
  if (builtFor != t.size()) { idx.clear(); for (size_t i = 0; i < t.size(); i++) idx[t[i].name] = i; builtFor = t.size(); }
  auto it = idx.find(name);
  return it == idx.end() ? nullptr : &t[it->second];
}

static void* (*g_resolver)(const char*) = nullptr;
void SetProcResolver(void* (*fn)(const char*)) { g_resolver = fn; }
void* ResolveProc(const char* name) {
  const ProcEntry* e = ProcFind(name);
  if (!e) { LogOnce(name, "GetProcAddress: unknown NVN name '%s'", name ? name : "(null)"); return nullptr; }
  if (g_resolver) return g_resolver(name);
  return e->native;           // direct host call (tests); stubs have no host entry
}

}  // namespace nvn

// ---- C interface for the runtime (nvn_register.c) -----------------------------------------------------------------
extern "C" {
size_t nvn_proc_count(void) { return nvn::ProcCount(); }
const char* nvn_proc_name(size_t i) { auto* e = nvn::ProcAt(i); return e ? e->name : nullptr; }
int nvn_proc_implemented(size_t i) { auto* e = nvn::ProcAt(i); return e && e->native ? 1 : 0; }
void (*nvn_proc_thunk(size_t i))(struct Ctx*) { auto* e = nvn::ProcAt(i); return e ? (void (*)(struct Ctx*))e->thunk : nullptr; }
void nvn_set_proc_resolver(void* (*fn)(const char*)) { nvn::SetProcResolver(fn); }
}
