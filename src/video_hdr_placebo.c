#ifdef __x86_64__
#define GL_SILENCE_DEPRECATION
#include "video_hdr_placebo.h"
#include "video_hdr_native_av.h"
#include "video_hdr_color_shader.h"

#include <OpenGL/gl3.h>
#include <dlfcn.h>
#include <stdio.h>

static GLuint vertex_array, framebuffer, color_buffer, lattice, lattice_program, output_program;
static HdrColor saved_color;
static bool lattice_ready;

bool FRZZ_HDR_API(frzz_hdr_bind_av)(void* util)
{
    return bind_hdr_metadata(util, dlsym);
}

static GLuint compile_shader(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled)
        return shader;
    char message[1024] = {0};
    glGetShaderInfoLog(shader, sizeof(message), NULL, message);
    fprintf(stderr, "FRZZ HDR shader compilation failed: %s\n", message);
    glDeleteShader(shader);
    return 0;
}

static GLuint link_shader(GLuint vertex, const char* source)
{
    const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, source);
    if (!fragment)
        return 0;
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(fragment);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked)
        return program;
    char message[1024] = {0};
    glGetProgramInfoLog(program, sizeof(message), NULL, message);
    fprintf(stderr, "FRZZ HDR program link failed: %s\n", message);
    glDeleteProgram(program);
    return 0;
}

static bool prepare_renderer(void)
{
    if (vertex_array)
        return true;
    static const char vertex_source[] =
        "#version 410 core\n"
        "void main() {\n"
        "    vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);\n"
        "    gl_Position=vec4(p*2.0-1.0,0.0,1.0);\n"
        "}\n";
    static const char color_source[] =
        "#version 410 core\n"
        "#define V3 vec3\n#define V4 vec4\n#define BLEND mix\n"
        "layout(std140) uniform Color {\n" FRZZ_HDR_COLOR_FIELDS "};\n"
        FRZZ_HDR_COLOR_SHADER
        "layout(location=0) out vec4 result;\n"
        "void main() {\n"
        "    vec2 p=floor(gl_FragCoord.xy);\n"
        "    result=vec4(hdr_to_sdr(vec3(p.x,mod(p.y,65.0),floor(p.y/65.0))/64.0),1.0);\n"
        "}\n";
    static const char output_source[] =
        "#version 410 core\n"
        "uniform sampler2DRect source_y,source_uv;\n"
        "uniform sampler2D lattice_tex;\n"
        "uniform vec2 frame_size,origin,shift;\n"
        "uniform int chroma;\n"
        "layout(location=0) out vec4 result;\n"
        "vec3 read_sdr(vec2 p) {\n"
        "    vec2 uv=clamp((p+0.5-shift)*0.5,vec2(0.5),frame_size*0.5-0.5)+origin*0.5;\n"
        "    vec3 s=vec3(texelFetch(source_y,ivec2(p+origin)).r,texture(source_uv,uv).rg);\n"
        "    vec3 q=clamp(s,0.0,1.0)*64.0;\n"
        "    float z=floor(q.z);\n"
        "    vec2 a=(q.xy+vec2(0.5,0.5+65.0*z))/vec2(65.0,4225.0);\n"
        "    vec2 b=(q.xy+vec2(0.5,0.5+65.0*min(z+1.0,64.0)))/vec2(65.0,4225.0);\n"
        "    return mix(texture(lattice_tex,a).rgb,texture(lattice_tex,b).rgb,q.z-z);\n"
        "}\n"
        "void main() {\n"
        "    vec2 p=floor(gl_FragCoord.xy);\n"
        "    if (chroma!=0) {\n"
        "        p*=2.0;\n"
        "        result=vec4(0.5*(read_sdr(p).gb+read_sdr(p+vec2(0.0,1.0)).gb),0.0,1.0);\n"
        "    } else {\n"
        "        result=vec4(read_sdr(p).r,0.0,0.0,1.0);\n"
        "    }\n"
        "}\n";
    const GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
    if (!vertex)
        return false;
    lattice_program = link_shader(vertex, color_source);
    output_program = link_shader(vertex, output_source);
    glDeleteShader(vertex);
    if (!lattice_program || !output_program) {
        glDeleteProgram(lattice_program);
        glDeleteProgram(output_program);
        return false;
    }
    glGenVertexArrays(1, &vertex_array);
    glGenFramebuffers(1, &framebuffer);
    glGenBuffers(1, &color_buffer);
    glGenTextures(1, &lattice);
    if (!vertex_array || !framebuffer || !color_buffer || !lattice)
        goto fail;
    glBindTexture(GL_TEXTURE_2D, lattice);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 65, 4225, 0, GL_RGBA, GL_FLOAT, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lattice, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!complete || glGetError() != GL_NO_ERROR)
        goto fail;
    glUniformBlockBinding(lattice_program, glGetUniformBlockIndex(lattice_program, "Color"), 0);
    glUseProgram(output_program);
    glUniform1i(glGetUniformLocation(output_program, "source_y"), 0);
    glUniform1i(glGetUniformLocation(output_program, "source_uv"), 1);
    glUniform1i(glGetUniformLocation(output_program, "lattice_tex"), 2);
    return true;
fail:
    glDeleteTextures(1, &lattice);
    glDeleteBuffers(1, &color_buffer);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteVertexArrays(1, &vertex_array);
    glDeleteProgram(lattice_program);
    glDeleteProgram(output_program);
    lattice = color_buffer = framebuffer = vertex_array = lattice_program = output_program = 0;
    return false;
}

static bool draw(GLenum target, GLuint texture, int width, int height)
{
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return false;
    glViewport(0, 0, width, height);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return glGetError() == GL_NO_ERROR;
}

static bool make_lattice(const HdrColor* color)
{
    if (lattice_ready && !memcmp(&saved_color, color, sizeof(*color)))
        return true;
    glUseProgram(lattice_program);
    glBindBuffer(GL_UNIFORM_BUFFER, color_buffer);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(*color), color, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, color_buffer);
    lattice_ready = draw(GL_TEXTURE_2D, lattice, 65, 4225);
    if (lattice_ready)
        saved_color = *color;
    return lattice_ready;
}

bool FRZZ_HDR_API(frzz_hdr_render)(AVFrame* output, const AVFrame* src, const unsigned planes[2],
                                  const unsigned output_tex[2], const int widths[2], const int heights[2],
                                  bool apply_rpu, bool prepare_output_only, unsigned prepare_dovi_profile)
{
    (void)widths;
    (void)heights;
    (void)prepare_output_only;
    (void)prepare_dovi_profile;
    HdrColor color;
    if (!prepare_renderer() || !hdr_describe_color(&color, src, apply_rpu))
        return false;

    glBindVertexArray(vertex_array);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    bool ok = make_lattice(&color);
    if (ok) {
        glUseProgram(output_program);
        for (int p = 0; p < 2; ++p) {
            glActiveTexture(GL_TEXTURE0 + p);
            glBindTexture(GL_TEXTURE_RECTANGLE, planes[p]);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, p ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, p ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, lattice);
        float shift[2];
        hdr_chroma_shift(src->chroma_location, shift);
        glUniform2fv(glGetUniformLocation(output_program, "shift"), 1, shift);
        glUniform2f(glGetUniformLocation(output_program, "frame_size"), (float)output->width, (float)output->height);
        glUniform2f(glGetUniformLocation(output_program, "origin"), (float)src->crop_left, (float)src->crop_top);
        for (int p = 0; p < 2 && ok; ++p) {
            glUniform1i(glGetUniformLocation(output_program, "chroma"), p);
            ok = draw(GL_TEXTURE_RECTANGLE, output_tex[p], output->width >> p, output->height >> p);
        }
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    if (!ok)
        return false;

    output->format = AV_PIX_FMT_NV12;
    output->pts = src->pts;
#if LIBAVUTIL_VERSION_MAJOR < 58
    output->pkt_duration = src->pkt_duration;
#else
    output->duration = src->duration;
#endif
    output->best_effort_timestamp = src->best_effort_timestamp;
    output->sample_aspect_ratio = src->sample_aspect_ratio;
    output->color_range = AVCOL_RANGE_MPEG;
    output->color_trc = AVCOL_TRC_BT709;
    output->colorspace = AVCOL_SPC_BT709;
    output->color_primaries = AVCOL_PRI_BT709;
    output->chroma_location = AVCHROMA_LOC_LEFT;
    return true;
}
#endif
