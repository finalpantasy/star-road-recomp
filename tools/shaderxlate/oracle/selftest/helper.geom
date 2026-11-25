#version 450
layout(points) in;
layout(points, max_vertices = 2) out;
layout(location = 0) in vec4 v0[];
layout(location = 0) flat out vec4 g0;
void main() {
    for (int i = 0; i < 2; i++) {
        float x = (float(i) + 0.5) / 2.0 * 2.0 - 1.0;
        float y = (float(gl_PrimitiveIDIn) + 0.5) / 64.0 * 2.0 - 1.0;
        gl_Position = vec4(x, y, 0.0, 1.0);
        gl_PointSize = 1.0;
        g0 = (i == 0) ? gl_in[0].gl_Position : v0[0];
        EmitVertex();
        EndPrimitive();
    }
}
