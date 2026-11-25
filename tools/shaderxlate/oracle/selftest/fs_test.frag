#version 450
// Compiled with -DSET=.. -DUBO_B=.. -DTEX_B=.. and optionally -DSCALE=1.01
#ifndef SCALE
#define SCALE 1.0
#endif
layout(location = 0) in vec4 v0;
layout(set = SET, binding = UBO_B, std140) uniform U { vec4 c[4096]; } u;
layout(set = SET, binding = TEX_B) uniform sampler2D tex;
layout(location = 0) out vec4 o0;
void main() {
    o0 = (texture(tex, v0.xy) + u.c[5]) * SCALE;
}
