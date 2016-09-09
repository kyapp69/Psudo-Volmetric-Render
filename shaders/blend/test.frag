#version 400
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

//layout (location = 0) in vec4 color;
layout (input_attachment_index=0, set=2, binding=0) uniform subpassInput subpass;
layout (input_attachment_index=1, set=3, binding=0) uniform subpassInput subpass1;
layout (input_attachment_index=2, set=4, binding=0) uniform subpassInput subpass2;
layout (input_attachment_index=3, set=5, binding=0) uniform subpassInput subpass3;
layout (location = 0) out vec4 outColor;

float viewSpaceDepth(float NDCDepth)
{
    //((n*f)/(f-n))/(NDCDepth-((f+n)/(2*(f-n)))+0.5))
    return 26.24/(NDCDepth-1.64);
}

void main() {
   vec4 thiscolor = subpassLoad(subpass);
   vec4 lastcolor = subpassLoad(subpass3);
   float thisDepth = subpassLoad(subpass1).r;
   float lastDepth = subpassLoad(subpass2).r;

   vec3 color;
   if (thiscolor.a<0.5) //If this is backface
       color=thiscolor.rgb;
   else if (lastcolor.a>0.5) //Else If last is frontface
       color=lastcolor.rgb;
   else
       discard;

   float depthdiff=viewSpaceDepth(lastDepth)-viewSpaceDepth(thisDepth);
   float alpha = depthdiff/3.0;
//   float alpha = 0.5;

   outColor = vec4(color.r*alpha, color.g*alpha, color.b*alpha, alpha);
//   outColor = vec4(color.a, 1.0-color.a, 0.0, 1.0);
//   outColor = color;
}
