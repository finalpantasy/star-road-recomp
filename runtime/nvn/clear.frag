#version 450

layout(push_constant) uniform ClearValue {
  VALUE_TYPE color;
} clearValue;
layout(location = 0) out VALUE_TYPE color;

void main() {
  color = clearValue.color;
}
