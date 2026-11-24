#version 450
// Test fragment shader: one texture (separate NVN texture and sampler handles) times a tint from uniform buffer 0.
layout(set = 1, binding = 0) uniform sampler2D uTex;
layout(set = 1, binding = 1) uniform Tint { vec4 tint; };
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
void main() {
  outColor = texture(uTex, vUv) * tint;
}
