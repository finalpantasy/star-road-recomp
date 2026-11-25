#version 450
layout(location = 0) out vec4 v0;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v0 = vec4(p * 0.5, p.x * 0.25, 1.0);
}
