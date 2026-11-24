/* Register access macros for the IR interpreter: registers live in the Ctx. */
#pragma once
#define RX(n) ((n) == 31 ? 0ull : c->x[n])
#define WX(n, val) ({ u64 _w = (val); if ((n) != 31) c->x[n] = _w; })
#define RS(n) (c->x[n])
#define WS(n, val) (c->x[n] = (val))
#define RV(n) (c->v[n])
#define WV(n, val) (c->v[n] = (val))
#define FL (c->fl)
