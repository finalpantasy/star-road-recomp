#version 450
// Test vertex shader (our own): position passes through unchanged, so the NVN clip space is what the test measures.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec2 vUv;
void main() {
  gl_Position = vec4(inPos, 1.0);
  vUv = inUv;
}
