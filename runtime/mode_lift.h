/* Register access macros for lifted code: guest registers are C locals (docs/RECOMPILER.md 3.4). */
#pragma once
#define RX(n) x##n
#define WX(n, val) ((n) == 31 ? (void)(val) : (void)(x##n = (val)))
#define RS(n) ((n) == 31 ? sp : x##n)
#define WS(n, val) ((n) == 31 ? (void)(sp = (val)) : (void)(x##n = (val)))
#define RV(n) v##n
#define WV(n, val) (v##n = (val))
#define FL fl
