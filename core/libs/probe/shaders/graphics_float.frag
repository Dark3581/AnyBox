#version 450

layout(location = 0) out vec4 target0;
layout(location = 1) out vec4 target1;
layout(location = 2) out vec4 target2;

void main() {
    target0 = vec4(1.0, 0.0, 0.0, 0.5);
    target1 = vec4(0.0, 1.0, 0.0, 1.0);
    target2 = vec4(0.0, 0.0, 1.0, 1.0);
}
