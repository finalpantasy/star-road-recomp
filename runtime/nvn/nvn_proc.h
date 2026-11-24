// Star Road Recomp: NVN proc registry and Ctx thunks (task-12). Our own code (D-008).
//
// Every NVN function is a plain typed C++ function declared with NVN_API. The macro also registers it, together with a thunk that
// reads the guest's AAPCS64 argument registers out of a runtime Ctx (integers and pointers from x0.., floats from v0..) and stores the
// result in x0 or v0. Names with no implementation get a logging stub (nvn_proc.cpp). The proc table is therefore the union of the
// NVN_API definitions and nvn_sigs.inc; nothing is written by hand.
#pragma once
#include <initializer_list>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include "nvn_internal.h"

extern "C" {
#include "../rt.h"
}

namespace nvn {

struct ArgReader {
  const Ctx* c; int xi = 0, vi = 0;
  template <class T> T get() {
    if constexpr (std::is_floating_point_v<T>) { T f; memcpy(&f, &c->v[vi++], sizeof f); return f; }
    else if constexpr (std::is_pointer_v<T>) return (T)(uintptr_t)c->x[xi++];
    else if constexpr (std::is_same_v<T, bool>) return (c->x[xi++] & 0xFF) != 0;
    else if constexpr (sizeof(T) == 8) return (T)c->x[xi++];
    else if constexpr (std::is_signed_v<T>) return (T)(int32_t)(uint32_t)c->x[xi++];
    else return (T)(uint32_t)c->x[xi++];
  }
};

template <class T> inline void StoreRet(Ctx* c, T r) {
  if constexpr (std::is_floating_point_v<T>) { c->v[0] = v128{0, 0}; memcpy(&c->v[0], &r, sizeof r); }
  else if constexpr (std::is_pointer_v<T>) c->x[0] = (uint64_t)(uintptr_t)r;
  else if constexpr (std::is_same_v<T, bool>) c->x[0] = r ? 1 : 0;
  else if constexpr (sizeof(T) == 8) c->x[0] = (uint64_t)r;
  else if constexpr (std::is_signed_v<T>) c->x[0] = (uint64_t)(uint32_t)(int32_t)r;
  else c->x[0] = (uint64_t)(uint32_t)r;
}

template <class T> constexpr char SigLetter() {
  if constexpr (std::is_void_v<T>) return 'v';
  else if constexpr (std::is_same_v<T, bool>) return 'b';
  else if constexpr (std::is_pointer_v<T>) return 'p';
  else if constexpr (std::is_same_v<T, float>) return 'f';
  else if constexpr (std::is_same_v<T, double>) return 'd';
  else if constexpr (sizeof(T) == 8) return 'l';
  else return 'i';
}

template <class F> struct FnTraits;
template <class R, class... A> struct FnTraits<R (*)(A...)> {
  template <auto Fn> static void Call(Ctx* c) {
    ArgReader rd{c};
    std::tuple<A...> t{rd.template get<A>()...};     // braced list: evaluated left to right
    if constexpr (std::is_void_v<R>) std::apply(Fn, t);
    else StoreRet(c, std::apply(Fn, t));
  }
  static std::string Sig() { std::string s; s += SigLetter<R>(); s += ':'; (void)std::initializer_list<int>{(s += SigLetter<A>(), 0)...}; return s; }
};
template <auto Fn> void CtxThunk(Ctx* c) { FnTraits<decltype(Fn)>::template Call<Fn>(c); }
template <auto Fn> std::string SigOf() { return FnTraits<decltype(Fn)>::Sig(); }

struct ProcReg {
  ProcReg(const char* name, void* fn, void (*thunk)(Ctx*), std::string (*sig)());
};

}  // namespace nvn

#define NVN_API(name, ret, params)                                                                                          \
  static ret name params;                                                                                                   \
  static ::nvn::ProcReg reg_##name(#name, (void*)&name, &::nvn::CtxThunk<&name>, &::nvn::SigOf<&name>);                     \
  static ret name params
