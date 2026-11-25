#version 450
// Compiled with -DSET=.. -DUBO_B=..
layout(location = 0) in vec4 a0;
layout(set = SET, binding = UBO_B, std140) uniform U { vec4 c[4096]; } u;
layout(location = 0) out vec4 v0;
void main() {
    v0 = a0 * u.c[3];
    gl_Position = vec4(a0.xyz * 0.5, 1.0);
}
