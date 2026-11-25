#version 450
// Test fragment shader (our own) shaped like a reuse-v1 cache module: a 1600 byte support buffer at binding 0 and the program
// constants (c[1]) as a 64 KiB uniform block at binding 1. It encodes the words the runtime must provide:
//   R  viewport size x / 64              (support offset 160, a 64x64 viewport gives 1.0)
//   G  viewport inverse x * 32           (support offset 144, 2/64 * 32 gives 1.0)
//   B  render scale slots 0 and 72 and the alpha test word (offsets 192, 1344, 0): 0.5 + 0.25 + 0.0 = 0.75
//   A  constants c[0].x + c[1].x + c[4095].w (file bytes 0..3 and 16..19, zero padding for the rest): 0.25 + 0.125 + 0.0 = 0.375
layout(set = 1, binding = 0, std140) uniform Support { vec4 w[100]; } sup;
layout(set = 1, binding = 1, std140) uniform Consts { vec4 c[4096]; } k;
layout(location = 0) out vec4 o;
void main() {
  float b = sup.w[12].x * 0.5 + sup.w[84].x * 0.25 + sup.w[0].x + sup.w[1].x + sup.w[5].y;
  o = vec4(sup.w[10].x / 64.0, sup.w[9].x * 32.0, b, k.c[0].x + k.c[1].x + k.c[4095].w);
}
