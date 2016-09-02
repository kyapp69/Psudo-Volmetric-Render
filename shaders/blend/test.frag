#version 400
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

//layout (location = 0) in vec4 color;
layout (input_attachment_index=0, set=2, binding=0) uniform subpassInput subpass;
layout (input_attachment_index=1, set=3, binding=0) uniform subpassInput subpass1;
layout (input_attachment_index=2, set=4, binding=0) uniform subpassInput subpass2;
layout (location = 0) out vec4 outColor;

void main() {
   vec4 color = subpassLoad(subpass);
   float thisDepth = subpassLoad(subpass1).r;
   float lastDepth = subpassLoad(subpass2).r;
//   color.a=thisDepth;
//   outColor = vec4(color.r*color.a, color.g*color.a, color.b*color.a, color.a);
   float depthdiff=thisDepth-lastDepth;
//   outColor = vec4(thisDepth,thisDepth,thisDepth,1);
//   outColor = vec4(lastDepth,lastDepth,lastDepth,1);
   outColor = vec4(depthdiff,depthdiff,depthdiff,1);
//   outColor = color;
}
