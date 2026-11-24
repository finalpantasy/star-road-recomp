#version 450
// Test fragment shader: flat color from uniform buffer 0.
layout(set = 1, binding = 0) uniform Flat { vec4 color; };
layout(location = 0) out vec4 outColor;
void main() {
  outColor = color;
}
