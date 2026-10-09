/*
 * EGLGPU — a Mesa-backed GPU::Device (see EGLGPU.h).
 *
 * The GPU::Device interface is a fixed-function pipeline, so the natural backend
 * is a desktop OpenGL *compatibility* context: most of the interface maps
 * directly onto GL 1.x/2.x fixed-function calls. We render offscreen into an FBO
 * and read the colour buffer back into the caller's Gfx::Bitmap.
 *
 * Texture storage is delegated to LibSoftGPU's Image (RGBA FloatVector4, with
 * full PixelConverter-based format handling); we only upload it into a GL
 * texture for sampling. That keeps the format-conversion surface tiny.
 */

#define GL_GLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES

#include "EGLGPU.h"

#include <AK/Debug.h>
#include <AK/StdLibExtras.h>
#include <LibGPU/Shader.h>
#include <LibSoftGPU/Device.h>
#include <LibSoftGPU/PixelConverter.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Vector4.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

namespace EGLGPU {

static constexpr unsigned NUM_TEXTURE_UNITS = 2;
static constexpr unsigned MAX_LIGHTS = 8;
static constexpr unsigned MAX_CLIP_PLANES = 6;
static constexpr unsigned MAX_TEXTURE_SIZE = 2048;

static void make_context_current(EGLDisplay display, EGLContext context)
{
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

static GLenum to_gl_primitive(GPU::PrimitiveType type)
{
    switch (type) {
    case GPU::PrimitiveType::Lines:
        return GL_LINES;
    case GPU::PrimitiveType::LineLoop:
        return GL_LINE_LOOP;
    case GPU::PrimitiveType::LineStrip:
        return GL_LINE_STRIP;
    case GPU::PrimitiveType::Points:
        return GL_POINTS;
    case GPU::PrimitiveType::TriangleFan:
        return GL_TRIANGLE_FAN;
    case GPU::PrimitiveType::Triangles:
        return GL_TRIANGLES;
    case GPU::PrimitiveType::TriangleStrip:
        return GL_TRIANGLE_STRIP;
    case GPU::PrimitiveType::Quads:
        return GL_QUADS;
    }
    VERIFY_NOT_REACHED();
}

static GLenum to_gl_compare(int function)
{
    switch (function) {
    case 0: // Never
        return GL_NEVER;
    case 1: // Always
        return GL_ALWAYS;
    case 2: // Less
        return GL_LESS;
    case 3: // LessOrEqual
        return GL_LEQUAL;
    case 4: // Equal
        return GL_EQUAL;
    case 5: // NotEqual
        return GL_NOTEQUAL;
    case 6: // GreaterOrEqual
        return GL_GEQUAL;
    case 7: // Greater
        return GL_GREATER;
    default:
        VERIFY_NOT_REACHED();
    }
}

static GLenum to_gl_alpha_function(GPU::AlphaTestFunction function)
{
    return to_gl_compare(static_cast<int>(function));
}

static GLenum to_gl_depth_function(GPU::DepthTestFunction function)
{
    return to_gl_compare(static_cast<int>(function));
}

static GLenum to_gl_blend_equation(GPU::BlendEquation equation)
{
    switch (equation) {
    case GPU::BlendEquation::Add:
        return GL_FUNC_ADD;
    case GPU::BlendEquation::Subtract:
        return GL_FUNC_SUBTRACT;
    case GPU::BlendEquation::ReverseSubtract:
        return GL_FUNC_REVERSE_SUBTRACT;
    case GPU::BlendEquation::Min:
        return GL_MIN;
    case GPU::BlendEquation::Max:
        return GL_MAX;
    }
    VERIFY_NOT_REACHED();
}

static GLenum to_gl_blend_factor(GPU::BlendFactor factor)
{
    switch (factor) {
    case GPU::BlendFactor::Zero:
        return GL_ZERO;
    case GPU::BlendFactor::One:
        return GL_ONE;
    case GPU::BlendFactor::SrcColor:
        return GL_SRC_COLOR;
    case GPU::BlendFactor::OneMinusSrcColor:
        return GL_ONE_MINUS_SRC_COLOR;
    case GPU::BlendFactor::DstColor:
        return GL_DST_COLOR;
    case GPU::BlendFactor::OneMinusDstColor:
        return GL_ONE_MINUS_DST_COLOR;
    case GPU::BlendFactor::SrcAlpha:
        return GL_SRC_ALPHA;
    case GPU::BlendFactor::OneMinusSrcAlpha:
        return GL_ONE_MINUS_SRC_ALPHA;
    case GPU::BlendFactor::DstAlpha:
        return GL_DST_ALPHA;
    case GPU::BlendFactor::OneMinusDstAlpha:
        return GL_ONE_MINUS_DST_ALPHA;
    case GPU::BlendFactor::ConstantColor:
        return GL_CONSTANT_COLOR;
    case GPU::BlendFactor::OneMinusConstantColor:
        return GL_ONE_MINUS_CONSTANT_COLOR;
    case GPU::BlendFactor::ConstantAlpha:
        return GL_CONSTANT_ALPHA;
    case GPU::BlendFactor::OneMinusConstantAlpha:
        return GL_ONE_MINUS_CONSTANT_ALPHA;
    case GPU::BlendFactor::SrcAlphaSaturate:
        return GL_SRC_ALPHA_SATURATE;
    }
    VERIFY_NOT_REACHED();
}

static GLenum to_gl_winding_order(GPU::WindingOrder order)
{
    return order == GPU::WindingOrder::Clockwise ? GL_CW : GL_CCW;
}

static GLenum to_gl_wrap_mode(GPU::TextureWrapMode mode)
{
    switch (mode) {
    case GPU::TextureWrapMode::Repeat:
        return GL_REPEAT;
    case GPU::TextureWrapMode::MirroredRepeat:
        return GL_MIRRORED_REPEAT;
    case GPU::TextureWrapMode::Clamp:
        return GL_CLAMP;
    case GPU::TextureWrapMode::ClampToBorder:
        return GL_CLAMP_TO_BORDER;
    case GPU::TextureWrapMode::ClampToEdge:
        return GL_CLAMP_TO_EDGE;
    }
    VERIFY_NOT_REACHED();
}

static GLint to_gl_min_filter(GPU::TextureFilter filter, GPU::MipMapFilter mipmap)
{
    if (mipmap == GPU::MipMapFilter::None)
        return filter == GPU::TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR;
    if (filter == GPU::TextureFilter::Nearest)
        return mipmap == GPU::MipMapFilter::Nearest ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_LINEAR;
    return mipmap == GPU::MipMapFilter::Nearest ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
}

static GLenum to_gl_texture_env_mode(GPU::TextureEnvMode mode)
{
    switch (mode) {
    case GPU::TextureEnvMode::Add:
        return GL_ADD;
    case GPU::TextureEnvMode::Blend:
        return GL_BLEND;
    case GPU::TextureEnvMode::Combine:
        return GL_COMBINE;
    case GPU::TextureEnvMode::Decal:
        return GL_DECAL;
    case GPU::TextureEnvMode::Modulate:
        return GL_MODULATE;
    case GPU::TextureEnvMode::Replace:
        return GL_REPLACE;
    }
    VERIFY_NOT_REACHED();
}

static GLenum to_gl_combine_rgb(GPU::TextureCombinator combinator)
{
    switch (combinator) {
    case GPU::TextureCombinator::Add:
        return GL_ADD;
    case GPU::TextureCombinator::AddSigned:
        return GL_ADD_SIGNED;
    case GPU::TextureCombinator::Dot3RGB:
        return GL_DOT3_RGB;
    case GPU::TextureCombinator::Dot3RGBA:
        return GL_DOT3_RGBA;
    case GPU::TextureCombinator::Interpolate:
        return GL_INTERPOLATE;
    case GPU::TextureCombinator::Modulate:
        return GL_MODULATE;
    case GPU::TextureCombinator::Replace:
        return GL_REPLACE;
    case GPU::TextureCombinator::Subtract:
        return GL_SUBTRACT;
    }
    VERIFY_NOT_REACHED();
}

static GLenum to_gl_texture_operand(GPU::TextureOperand operand)
{
    switch (operand) {
    case GPU::TextureOperand::OneMinusSourceAlpha:
        return GL_ONE_MINUS_SRC_ALPHA;
    case GPU::TextureOperand::OneMinusSourceColor:
        return GL_ONE_MINUS_SRC_COLOR;
    case GPU::TextureOperand::SourceAlpha:
        return GL_SRC_ALPHA;
    case GPU::TextureOperand::SourceColor:
        return GL_SRC_COLOR;
    }
    VERIFY_NOT_REACHED();
}

static GLenum to_gl_texture_source(GPU::TextureSource source, u8 stage)
{
    switch (source) {
    case GPU::TextureSource::Constant:
        return GL_CONSTANT;
    case GPU::TextureSource::Previous:
        return GL_PREVIOUS;
    case GPU::TextureSource::PrimaryColor:
        return GL_PRIMARY_COLOR;
    case GPU::TextureSource::Texture:
        return GL_TEXTURE;
    case GPU::TextureSource::TextureStage:
        return GL_TEXTURE0 + stage;
    }
    VERIFY_NOT_REACHED();
}

static GLenum to_gl_tex_gen_mode(GPU::TexCoordGenerationMode mode)
{
    switch (mode) {
    case GPU::TexCoordGenerationMode::ObjectLinear:
        return GL_OBJECT_LINEAR;
    case GPU::TexCoordGenerationMode::EyeLinear:
        return GL_EYE_LINEAR;
    case GPU::TexCoordGenerationMode::SphereMap:
        return GL_SPHERE_MAP;
    case GPU::TexCoordGenerationMode::ReflectionMap:
        return GL_REFLECTION_MAP;
    case GPU::TexCoordGenerationMode::NormalMap:
        return GL_NORMAL_MAP;
    }
    VERIFY_NOT_REACHED();
}

static GLenum to_gl_stencil_function(GPU::StencilTestFunction function)
{
    switch (function) {
    case GPU::StencilTestFunction::Always:
        return GL_ALWAYS;
    case GPU::StencilTestFunction::Equal:
        return GL_EQUAL;
    case GPU::StencilTestFunction::Greater:
        return GL_GREATER;
    case GPU::StencilTestFunction::GreaterOrEqual:
        return GL_GEQUAL;
    case GPU::StencilTestFunction::Less:
        return GL_LESS;
    case GPU::StencilTestFunction::LessOrEqual:
        return GL_LEQUAL;
    case GPU::StencilTestFunction::Never:
        return GL_NEVER;
    case GPU::StencilTestFunction::NotEqual:
        return GL_NOTEQUAL;
    }
    VERIFY_NOT_REACHED();
}

static GLenum to_gl_stencil_operation(GPU::StencilOperation operation)
{
    switch (operation) {
    case GPU::StencilOperation::Decrement:
        return GL_DECR;
    case GPU::StencilOperation::DecrementWrap:
        return GL_DECR_WRAP;
    case GPU::StencilOperation::Increment:
        return GL_INCR;
    case GPU::StencilOperation::IncrementWrap:
        return GL_INCR_WRAP;
    case GPU::StencilOperation::Invert:
        return GL_INVERT;
    case GPU::StencilOperation::Keep:
        return GL_KEEP;
    case GPU::StencilOperation::Replace:
        return GL_REPLACE;
    case GPU::StencilOperation::Zero:
        return GL_ZERO;
    }
    VERIFY_NOT_REACHED();
}

static void to_gl_floats(FloatVector4 const& v, GLfloat out[4])
{
    out[0] = v.x();
    out[1] = v.y();
    out[2] = v.z();
    out[3] = v.w();
}

static void to_gl_floats(FloatVector3 const& v, GLfloat out[3])
{
    out[0] = v.x();
    out[1] = v.y();
    out[2] = v.z();
}

static void load_matrix(Gfx::FloatMatrix4x4 const& matrix)
{
    // Gfx matrices are row-major; glLoadTransposeMatrixf() accepts row-major and
    // yields the same matrix in GL's column-major convention.
    glLoadTransposeMatrixf(reinterpret_cast<GLfloat const*>(matrix.elements()));
}

static GPU::ImageDataLayout bgra8_layout(Vector2<u32> size)
{
    return {
        .pixel_type = {
            .format = GPU::PixelFormat::BGRA,
            .bits = GPU::PixelComponentBits::B8_8_8_8,
            .data_type = GPU::PixelDataType::UnsignedInt,
            .components_order = GPU::ComponentsOrder::Reversed,
        },
        .dimensions = { size.x(), size.y(), 1 },
        .selection = { 0, 0, 0, size.x(), size.y(), 1 },
    };
}

static GPU::ImageDataLayout depth_float_layout(Vector2<u32> size)
{
    return {
        .pixel_type = {
            .format = GPU::PixelFormat::DepthComponent,
            .bits = GPU::PixelComponentBits::AllBits,
            .data_type = GPU::PixelDataType::Float,
        },
        .dimensions = { size.x(), size.y(), 1 },
        .selection = { 0, 0, 0, size.x(), size.y(), 1 },
    };
}

static GPU::ImageDataLayout rgba8_layout(Vector2<u32> size)
{
    return {
        .pixel_type = {
            .format = GPU::PixelFormat::RGBA,
            .bits = GPU::PixelComponentBits::B8_8_8_8,
            .data_type = GPU::PixelDataType::UnsignedInt,
            .components_order = GPU::ComponentsOrder::Normal,
        },
        .dimensions = { size.x(), size.y(), 1 },
        .selection = { 0, 0, 0, size.x(), size.y(), 1 },
    };
}

// ---------------------------------------------------------------------------
// Image
// ---------------------------------------------------------------------------

Image::Image(Device const& owner, GPU::PixelFormat const& pixel_format, u32 width, u32 height, u32 depth, u32 max_levels)
    : GPU::Image(&owner, pixel_format, width, height, depth, max_levels)
    , m_storage(adopt_ref(*new SoftGPU::Image(&owner, pixel_format, width, height, depth, max_levels)))
{
}

void Image::regenerate_mipmaps()
{
    m_storage->regenerate_mipmaps();
    m_dirty = true;
}

void Image::write_texels(u32 level, Vector3<i32> const& output_offset, void const* input_data, GPU::ImageDataLayout const& input_layout)
{
    m_storage->write_texels(level, output_offset, input_data, input_layout);
    m_dirty = true;
}

void Image::read_texels(u32 level, Vector3<i32> const& input_offset, void* output_data, GPU::ImageDataLayout const& output_layout) const
{
    m_storage->read_texels(level, input_offset, output_data, output_layout);
}

void Image::copy_texels(GPU::Image const& source, u32 source_level, Vector3<u32> const& source_offset, Vector3<u32> const& size, u32 destination_level, Vector3<u32> const& destination_offset)
{
    m_storage->copy_texels(*static_cast<Image const&>(source).m_storage, source_level, source_offset, size, destination_level, destination_offset);
    m_dirty = true;
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

Device::Device(EGLDisplay display, EGLContext context, Gfx::IntSize size)
    : m_display(display)
    , m_context(context)
    , m_size(size)
{
    make_context_current(m_display, m_context);

    auto const* vendor = glGetString(GL_VENDOR);
    auto const* renderer = glGetString(GL_RENDERER);
    m_info.vendor_name = ByteString { vendor ? reinterpret_cast<char const*>(vendor) : "Unknown" };
    m_info.device_name = ByteString { renderer ? reinterpret_cast<char const*>(renderer) : "EGL/OpenGL" };
    m_info.num_texture_units = NUM_TEXTURE_UNITS;
    m_info.num_lights = MAX_LIGHTS;
    m_info.max_clip_planes = MAX_CLIP_PLANES;
    m_info.max_texture_size = MAX_TEXTURE_SIZE;
    m_info.max_texture_lod_bias = 2.f;
    m_info.stencil_bits = 8;
    m_info.supports_npot_textures = true;
    m_info.supports_texture_clamp_to_edge = true;
    m_info.supports_texture_env_add = true;

    // Match the GPU::RasterizerOptions defaults.
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glShadeModel(GL_SMOOTH);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FOG);
    glDisable(GL_NORMALIZE);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

    create_framebuffer(size);
}

Device::~Device()
{
    if (m_context == EGL_NO_CONTEXT)
        return;
    make_context_current(m_display, m_context);
    for (auto texture : m_allocated_textures)
        glDeleteTextures(1, &texture);
    m_allocated_textures.clear();
    if (m_fbo != 0)
        glDeleteFramebuffers(1, &m_fbo);
    if (m_color_texture != 0)
        glDeleteTextures(1, &m_color_texture);
    if (m_depth_stencil_buffer != 0)
        glDeleteRenderbuffers(1, &m_depth_stencil_buffer);
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(m_display, m_context);
}

ErrorOr<NonnullOwnPtr<Device>> Device::try_create(Gfx::IntSize size)
{
    auto display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY)
        return Error::from_string_literal("eglGetDisplay() failed");

    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(display, &major, &minor) == EGL_FALSE)
        return Error::from_string_literal("eglInitialize() failed");

    if (eglBindAPI(EGL_OPENGL_API) == EGL_FALSE)
        return Error::from_string_literal("eglBindAPI(EGL_OPENGL_API) failed");

    static EGLint const config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };

    EGLConfig config;
    EGLint number_of_configs = 0;
    if (eglChooseConfig(display, config_attributes, &config, 1, &number_of_configs) == EGL_FALSE || number_of_configs < 1)
        return Error::from_string_literal("eglChooseConfig() found no matching config");

    // Ask for a compatibility profile so the fixed-function entry points are
    // available. 3.3 is the lowest version that offers the profile mask.
    static EGLint const context_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
        EGL_NONE
    };

    auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT)
        return Error::from_string_literal("eglCreateContext() failed");

    if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) == EGL_FALSE) {
        eglDestroyContext(display, context);
        return Error::from_string_literal("eglMakeCurrent() failed");
    }

    return adopt_own(*new Device(display, context, size));
}

void Device::make_current()
{
    make_context_current(m_display, m_context);
}

void Device::create_framebuffer(Gfx::IntSize size)
{
    m_size = size;

    if (m_fbo == 0)
        glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

    if (m_color_texture == 0)
        glGenTextures(1, &m_color_texture);
    glBindTexture(GL_TEXTURE_2D, m_color_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.width(), size.height(), 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_color_texture, 0);

    if (m_depth_stencil_buffer == 0)
        glGenRenderbuffers(1, &m_depth_stencil_buffer);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depth_stencil_buffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, size.width(), size.height());
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_depth_stencil_buffer);

    auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        dbgln("EGLGPU: framebuffer is incomplete: {:#x}", status);

    glViewport(0, 0, size.width(), size.height());
}

void Device::resize(Gfx::IntSize size)
{
    make_current();
    create_framebuffer(size);
}

void Device::apply_options()
{
    make_current();

    glShadeModel(m_options.shade_smooth ? GL_SMOOTH : GL_FLAT);

    if (m_options.enable_depth_test)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    glDepthFunc(to_gl_depth_function(m_options.depth_func));
    glDepthMask(m_options.enable_depth_write ? GL_TRUE : GL_FALSE);
    glDepthRange(static_cast<GLdouble>(m_options.depth_min), static_cast<GLdouble>(m_options.depth_max));

    if (m_options.enable_alpha_test) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(to_gl_alpha_function(m_options.alpha_test_func), m_options.alpha_test_ref_value);
    } else {
        glDisable(GL_ALPHA_TEST);
    }

    if (m_options.enable_blending) {
        glEnable(GL_BLEND);
        glBlendEquationSeparate(to_gl_blend_equation(m_options.blend_equation_rgb), to_gl_blend_equation(m_options.blend_equation_alpha));
        glBlendFuncSeparate(to_gl_blend_factor(m_options.blend_source_factor), to_gl_blend_factor(m_options.blend_destination_factor), GL_ONE, GL_ONE);
        glBlendColor(m_options.blend_color.x(), m_options.blend_color.y(), m_options.blend_color.z(), m_options.blend_color.w());
    } else {
        glDisable(GL_BLEND);
    }

    if (m_options.enable_color_write) {
        GLboolean red = (m_options.color_mask & 0x00ff0000u) ? GL_TRUE : GL_FALSE;
        GLboolean green = (m_options.color_mask & 0x0000ff00u) ? GL_TRUE : GL_FALSE;
        GLboolean blue = (m_options.color_mask & 0x000000ffu) ? GL_TRUE : GL_FALSE;
        GLboolean alpha = (m_options.color_mask & 0xff000000u) ? GL_TRUE : GL_FALSE;
        glColorMask(red, green, blue, alpha);
    } else {
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    }

    glPolygonMode(GL_FRONT_AND_BACK, m_options.polygon_mode == GPU::PolygonMode::Point ? GL_POINT : (m_options.polygon_mode == GPU::PolygonMode::Line ? GL_LINE : GL_FILL));

    if (m_options.fog_enabled) {
        glEnable(GL_FOG);
        GLfloat fog_color[4];
        to_gl_floats(m_options.fog_color, fog_color);
        glFogfv(GL_FOG_COLOR, fog_color);
        glFogf(GL_FOG_DENSITY, m_options.fog_density);
        glFogi(GL_FOG_MODE, m_options.fog_mode == GPU::FogMode::Linear ? GL_LINEAR : (m_options.fog_mode == GPU::FogMode::Exp ? GL_EXP : GL_EXP2));
        glFogf(GL_FOG_START, m_options.fog_start);
        glFogf(GL_FOG_END, m_options.fog_end);
    } else {
        glDisable(GL_FOG);
    }

    if (m_options.line_smooth)
        glEnable(GL_LINE_SMOOTH);
    else
        glDisable(GL_LINE_SMOOTH);
    glLineWidth(m_options.line_width);

    if (m_options.point_smooth)
        glEnable(GL_POINT_SMOOTH);
    else
        glDisable(GL_POINT_SMOOTH);
    glPointSize(m_options.point_size);

    if (m_options.scissor_enabled) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(m_options.scissor_box.x(), m_options.scissor_box.y(), m_options.scissor_box.width(), m_options.scissor_box.height());
    } else {
        glDisable(GL_SCISSOR_TEST);
    }

    if (m_options.normalization_enabled)
        glEnable(GL_NORMALIZE);
    else
        glDisable(GL_NORMALIZE);

    if (m_options.enable_culling) {
        glEnable(GL_CULL_FACE);
        glFrontFace(to_gl_winding_order(m_options.front_face));
        if (m_options.cull_front && m_options.cull_back)
            glCullFace(GL_FRONT_AND_BACK);
        else if (m_options.cull_front)
            glCullFace(GL_FRONT);
        else
            glCullFace(GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }

    if (m_options.depth_offset_enabled) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(m_options.depth_offset_factor, m_options.depth_offset_constant);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    if (m_options.enable_stencil_test)
        glEnable(GL_STENCIL_TEST);
    else
        glDisable(GL_STENCIL_TEST);

    if (m_options.lighting_enabled)
        glEnable(GL_LIGHTING);
    else
        glDisable(GL_LIGHTING);

    if (m_options.color_material_enabled) {
        glEnable(GL_COLOR_MATERIAL);
        GLenum face = m_options.color_material_face == GPU::ColorMaterialFace::Front ? GL_FRONT : (m_options.color_material_face == GPU::ColorMaterialFace::Back ? GL_BACK : GL_FRONT_AND_BACK);
        GLenum mode = GL_AMBIENT_AND_DIFFUSE;
        switch (m_options.color_material_mode) {
        case GPU::ColorMaterialMode::Ambient:
            mode = GL_AMBIENT;
            break;
        case GPU::ColorMaterialMode::AmbientAndDiffuse:
            mode = GL_AMBIENT_AND_DIFFUSE;
            break;
        case GPU::ColorMaterialMode::Diffuse:
            mode = GL_DIFFUSE;
            break;
        case GPU::ColorMaterialMode::Emissive:
            mode = GL_EMISSION;
            break;
        case GPU::ColorMaterialMode::Specular:
            mode = GL_SPECULAR;
            break;
        }
        glColorMaterial(face, mode);
    } else {
        glDisable(GL_COLOR_MATERIAL);
    }

    if (m_options.viewport.width() > 0 && m_options.viewport.height() > 0)
        glViewport(m_options.viewport.x(), m_options.viewport.y(), m_options.viewport.width(), m_options.viewport.height());
}

void Device::set_options(GPU::RasterizerOptions const& options)
{
    m_options = options;
    apply_options();
}

void Device::set_light_model_params(GPU::LightModelParameters const& light_model)
{
    m_light_model = light_model;
    make_current();

    GLfloat ambient[4];
    to_gl_floats(m_light_model.scene_ambient_color, ambient);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, m_light_model.viewer_at_infinity ? GL_TRUE : GL_FALSE);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, m_light_model.two_sided_lighting ? GL_TRUE : GL_FALSE);
    glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, m_light_model.color_control == GPU::ColorControl::SeparateSpecularColor ? GL_SEPARATE_SPECULAR_COLOR : GL_SINGLE_COLOR);
}

void Device::clear_color(FloatVector4 const& color)
{
    make_current();
    glClearColor(color.x(), color.y(), color.z(), color.w());

    GLboolean color_mask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
}

void Device::clear_depth(GPU::DepthType depth)
{
    make_current();
    glClearDepth(static_cast<GLdouble>(depth));
    GLboolean depth_mask;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDepthMask(depth_mask);
}

void Device::clear_stencil(GPU::StencilType value)
{
    make_current();
    glClearStencil(value);
    GLint stencil_mask;
    glGetIntegerv(GL_STENCIL_WRITEMASK, &stencil_mask);
    glStencilMask(0xffffffffu);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(static_cast<GLuint>(stencil_mask));
}

void Device::draw_primitives(GPU::PrimitiveType primitive_type, Vector<GPU::Vertex>& vertices)
{
    make_current();
    if (vertices.is_empty())
        return;

    size_t const count = vertices.size();

    Vector<GLfloat> positions;
    Vector<GLfloat> colors;
    Vector<GLfloat> normals;
    Vector<GLfloat> tex_coords[NUM_TEXTURE_UNITS];

    positions.resize(count * 4);
    colors.resize(count * 4);
    normals.resize(count * 3);
    for (auto& tex : tex_coords)
        tex.resize(count * 4);

    for (size_t i = 0; i < count; ++i) {
        auto const& vertex = vertices[i];

        positions[i * 4 + 0] = vertex.position.x();
        positions[i * 4 + 1] = vertex.position.y();
        positions[i * 4 + 2] = vertex.position.z();
        positions[i * 4 + 3] = vertex.position.w();

        colors[i * 4 + 0] = vertex.color.x();
        colors[i * 4 + 1] = vertex.color.y();
        colors[i * 4 + 2] = vertex.color.z();
        colors[i * 4 + 3] = vertex.color.w();

        normals[i * 3 + 0] = vertex.normal.x();
        normals[i * 3 + 1] = vertex.normal.y();
        normals[i * 3 + 2] = vertex.normal.z();

        for (unsigned unit = 0; unit < NUM_TEXTURE_UNITS; ++unit) {
            tex_coords[unit][i * 4 + 0] = vertex.tex_coords[unit].x();
            tex_coords[unit][i * 4 + 1] = vertex.tex_coords[unit].y();
            tex_coords[unit][i * 4 + 2] = vertex.tex_coords[unit].z();
            tex_coords[unit][i * 4 + 3] = vertex.tex_coords[unit].w();
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(4, GL_FLOAT, 0, positions.data());
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_FLOAT, 0, colors.data());
    glEnableClientState(GL_NORMAL_ARRAY);
    glNormalPointer(GL_FLOAT, 0, normals.data());

    for (unsigned unit = 0; unit < NUM_TEXTURE_UNITS; ++unit) {
        glClientActiveTexture(GL_TEXTURE0 + unit);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(4, GL_FLOAT, 0, tex_coords[unit].data());
    }
    glClientActiveTexture(GL_TEXTURE0);

    glDrawArrays(to_gl_primitive(primitive_type), 0, static_cast<GLsizei>(count));

    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    for (unsigned unit = 0; unit < NUM_TEXTURE_UNITS; ++unit) {
        glClientActiveTexture(GL_TEXTURE0 + unit);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glClientActiveTexture(GL_TEXTURE0);
}

void Device::set_model_view_transform(FloatMatrix4x4 const& model_view_transform)
{
    make_current();
    glMatrixMode(GL_MODELVIEW);
    load_matrix(model_view_transform);
}

void Device::set_projection_transform(FloatMatrix4x4 const& projection_transform)
{
    make_current();
    glMatrixMode(GL_PROJECTION);
    load_matrix(projection_transform);
    glMatrixMode(GL_MODELVIEW);
}

void Device::set_light_state(unsigned light_id, GPU::Light const& light)
{
    if (light_id >= MAX_LIGHTS)
        return;
    make_current();

    GLenum gl_light = GL_LIGHT0 + light_id;
    if (light.is_enabled)
        glEnable(gl_light);
    else
        glDisable(gl_light);

    GLfloat floats[4];
    to_gl_floats(light.ambient_intensity, floats);
    glLightfv(gl_light, GL_AMBIENT, floats);
    to_gl_floats(light.diffuse_intensity, floats);
    glLightfv(gl_light, GL_DIFFUSE, floats);
    to_gl_floats(light.specular_intensity, floats);
    glLightfv(gl_light, GL_SPECULAR, floats);

    // SoftGPU treats light positions/directions as eye-space values; load them
    // with an identity model-view so OpenGL stores exactly the given values.
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    to_gl_floats(light.position, floats);
    glLightfv(gl_light, GL_POSITION, floats);
    GLfloat direction[3];
    to_gl_floats(light.spotlight_direction, direction);
    glLightfv(gl_light, GL_SPOT_DIRECTION, direction);
    glPopMatrix();

    glLightf(gl_light, GL_SPOT_EXPONENT, light.spotlight_exponent);
    glLightf(gl_light, GL_SPOT_CUTOFF, light.spotlight_cutoff_angle);
    glLightf(gl_light, GL_CONSTANT_ATTENUATION, light.constant_attenuation);
    glLightf(gl_light, GL_LINEAR_ATTENUATION, light.linear_attenuation);
    glLightf(gl_light, GL_QUADRATIC_ATTENUATION, light.quadratic_attenuation);
}

void Device::set_material_state(GPU::Face face, GPU::Material const& material)
{
    make_current();

    GLenum gl_face = face == GPU::Face::Front ? GL_FRONT : GL_BACK;
    GLfloat floats[4];
    to_gl_floats(material.ambient, floats);
    glMaterialfv(gl_face, GL_AMBIENT, floats);
    to_gl_floats(material.diffuse, floats);
    glMaterialfv(gl_face, GL_DIFFUSE, floats);
    to_gl_floats(material.specular, floats);
    glMaterialfv(gl_face, GL_SPECULAR, floats);
    to_gl_floats(material.emissive, floats);
    glMaterialfv(gl_face, GL_EMISSION, floats);
    glMaterialf(gl_face, GL_SHININESS, material.shininess);
}

void Device::set_stencil_configuration(GPU::Face face, GPU::StencilConfiguration const& configuration)
{
    make_current();

    GLenum gl_face = face == GPU::Face::Front ? GL_FRONT : GL_BACK;
    glStencilFuncSeparate(gl_face, to_gl_stencil_function(configuration.test_function), configuration.reference_value, configuration.test_mask);
    glStencilOpSeparate(gl_face, to_gl_stencil_operation(configuration.on_stencil_test_fail), to_gl_stencil_operation(configuration.on_depth_test_fail), to_gl_stencil_operation(configuration.on_pass));
    glStencilMaskSeparate(gl_face, configuration.write_mask);
}

void Device::set_texture_unit_configuration(GPU::TextureUnitIndex index, GPU::TextureUnitConfiguration const& configuration)
{
    if (index >= NUM_TEXTURE_UNITS)
        return;
    make_current();

    glActiveTexture(GL_TEXTURE0 + index);
    if (configuration.enabled)
        glEnable(GL_TEXTURE_2D);
    else
        glDisable(GL_TEXTURE_2D);

    glMatrixMode(GL_TEXTURE);
    load_matrix(configuration.transformation_matrix);
    glMatrixMode(GL_MODELVIEW);

    for (unsigned coordinate = 0; coordinate < 4; ++coordinate) {
        GLenum capability = GL_TEXTURE_GEN_S + coordinate;
        u8 bit = static_cast<u8>(1u << coordinate);
        if (configuration.tex_coord_generation_enabled & bit) {
            auto const& generation = configuration.tex_coord_generation[coordinate];
            glEnable(capability);
            glTexGeni(capability, GL_TEXTURE_GEN_MODE, to_gl_tex_gen_mode(generation.mode));
            GLfloat coefficients[4];
            to_gl_floats(generation.coefficients, coefficients);
            GLenum plane = generation.mode == GPU::TexCoordGenerationMode::ObjectLinear ? GL_OBJECT_PLANE : GL_EYE_PLANE;
            glTexGenfv(capability, plane, coefficients);
        } else {
            glDisable(capability);
        }
    }
    glActiveTexture(GL_TEXTURE0);
}

void Device::set_clip_planes(Vector<FloatVector4> const& clip_planes)
{
    make_current();
    for (unsigned i = 0; i < MAX_CLIP_PLANES; ++i) {
        GLenum plane = GL_CLIP_PLANE0 + i;
        if (i < clip_planes.size()) {
            auto const& coefficients = clip_planes[i];
            GLdouble equation[4] = {
                static_cast<GLdouble>(coefficients.x()),
                static_cast<GLdouble>(coefficients.y()),
                static_cast<GLdouble>(coefficients.z()),
                static_cast<GLdouble>(coefficients.w()),
            };
            glEnable(plane);
            glClipPlane(plane, equation);
        } else {
            glDisable(plane);
        }
    }
}

void Device::set_raster_position(GPU::RasterPosition const& raster_position)
{
    m_raster_position = raster_position;
    m_raster_position_valid = raster_position.valid;
    if (m_raster_position_valid) {
        make_current();
        auto const& window_coordinates = raster_position.window_coordinates;
        glWindowPos3f(window_coordinates.x(), window_coordinates.y(), window_coordinates.z());
    }
}

void Device::set_raster_position(FloatVector4 const& position)
{
    make_current();
    glRasterPos4f(position.x(), position.y(), position.z(), position.w());
    GLfloat window_coordinates[4];
    glGetFloatv(GL_CURRENT_RASTER_POSITION, window_coordinates);
    m_raster_position.window_coordinates = { window_coordinates[0], window_coordinates[1], window_coordinates[2], window_coordinates[3] };
    m_raster_position_valid = true;
}

NonnullRefPtr<GPU::Image> Device::create_image(GPU::PixelFormat const& pixel_format, u32 width, u32 height, u32 depth, u32 max_levels)
{
    make_current();
    return adopt_ref(*new Image(*this, pixel_format, width, height, depth, max_levels));
}

ErrorOr<NonnullRefPtr<GPU::Shader>> Device::create_shader(GPU::IR::Shader const&)
{
    return Error::from_string_literal("EGLGPU: GLSL shaders are not supported by the Mesa backend; falling back to LibSoftGPU won't help either");
}

void Device::bind_fragment_shader(RefPtr<GPU::Shader>)
{
    // The fixed-function pipeline does not use fragment shaders. GLSL programs
    // are not supported by this backend (create_shader() returns an error).
}

void Device::set_sampler_config(unsigned sampler, GPU::SamplerConfig const& config)
{
    if (sampler >= NUM_TEXTURE_UNITS)
        return;
    make_current();

    glActiveTexture(GL_TEXTURE0 + sampler);

    if (config.bound_image.is_null()) {
        glDisable(GL_TEXTURE_2D);
        glActiveTexture(GL_TEXTURE0);
        return;
    }

    auto& image = static_cast<Image&>(*config.bound_image);
    bind_and_upload_image(image, config);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, to_gl_wrap_mode(config.texture_wrap_u));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, to_gl_wrap_mode(config.texture_wrap_v));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, to_gl_wrap_mode(config.texture_wrap_w));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, to_gl_min_filter(config.texture_min_filter, config.mipmap_filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, config.texture_mag_filter == GPU::TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, config.level_of_detail_bias);

    GLfloat border_color[4];
    to_gl_floats(config.border_color, border_color);
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border_color);

    if (config.mipmap_filter != GPU::MipMapFilter::None)
        glGenerateMipmap(GL_TEXTURE_2D);

    auto const& environment = config.fixed_function_texture_environment;
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, to_gl_texture_env_mode(environment.env_mode));

    GLfloat env_color[4];
    to_gl_floats(environment.color, env_color);
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, env_color);

    if (environment.env_mode == GPU::TextureEnvMode::Combine) {
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, to_gl_combine_rgb(environment.rgb_combinator));
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, to_gl_combine_rgb(environment.alpha_combinator));
        glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, environment.rgb_scale);
        glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, environment.alpha_scale);

        static constexpr GLenum rgb_sources[] = { GL_SRC0_RGB, GL_SRC1_RGB, GL_SRC2_RGB };
        static constexpr GLenum alpha_sources[] = { GL_SRC0_ALPHA, GL_SRC1_ALPHA, GL_SRC2_ALPHA };
        static constexpr GLenum rgb_operands[] = { GL_OPERAND0_RGB, GL_OPERAND1_RGB, GL_OPERAND2_RGB };
        static constexpr GLenum alpha_operands[] = { GL_OPERAND0_ALPHA, GL_OPERAND1_ALPHA, GL_OPERAND2_ALPHA };

        for (unsigned i = 0; i < 3; ++i) {
            glTexEnvi(GL_TEXTURE_ENV, rgb_sources[i], to_gl_texture_source(environment.rgb_source[i], environment.rgb_source_texture_stage));
            glTexEnvi(GL_TEXTURE_ENV, alpha_sources[i], to_gl_texture_source(environment.alpha_source[i], environment.alpha_source_texture_stage));
            glTexEnvi(GL_TEXTURE_ENV, rgb_operands[i], to_gl_texture_operand(environment.rgb_operand[i]));
            glTexEnvi(GL_TEXTURE_ENV, alpha_operands[i], to_gl_texture_operand(environment.alpha_operand[i]));
        }
    }

    glActiveTexture(GL_TEXTURE0);
}

void Device::bind_and_upload_image(Image& image, GPU::SamplerConfig const& config)
{
    if (image.texture() == 0) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        image.set_texture(texture);
        m_allocated_textures.append(texture);
        image.mark_dirty();
    }

    glBindTexture(GL_TEXTURE_2D, image.texture());

    if (!image.is_dirty())
        return;

    auto& storage = image.storage();
    auto width = storage.width_at_level(0);
    auto height = storage.height_at_level(0);
    if (width == 0 || height == 0)
        return;

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, storage.texel_pointer(0, 0, 0, 0));
    if (config.mipmap_filter != GPU::MipMapFilter::None)
        glGenerateMipmap(GL_TEXTURE_2D);
    image.clear_dirty();
}

void Device::read_color_pixels(Vector2<i32> offset, Vector2<u32> size, Vector<u8>& out)
{
    make_current();
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    out.resize(size.x() * size.y() * 4);
    if (size.x() > 0 && size.y() > 0)
        glReadPixels(offset.x(), offset.y(), static_cast<GLsizei>(size.x()), static_cast<GLsizei>(size.y()), GL_BGRA, GL_UNSIGNED_BYTE, out.data());
}

void Device::blit_from_color_buffer(Gfx::Bitmap& target)
{
    make_current();
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    auto width = target.width();
    auto height = target.height();
    if (width <= 0 || height <= 0)
        return;

    Vector<u8> pixels;
    pixels.resize(width * height * 4);
    glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());

    // OpenGL's framebuffer origin is bottom-left; Gfx::Bitmap is top-left.
    for (int y = 0; y < height; ++y)
        __builtin_memcpy(target.scanline(y), pixels.data() + (height - 1 - y) * width * 4, width * 4);
}

void Device::blit_from_color_buffer(NonnullRefPtr<GPU::Image> image, u32 level, Vector2<u32> input_size, Vector2<i32> input_offset, Vector3<i32> output_offset)
{
    auto& target = static_cast<Image&>(*image);
    Vector<u8> pixels;
    read_color_pixels(input_offset, input_size, pixels);
    auto input_layout = bgra8_layout(input_size);
    target.storage().write_texels(level, output_offset, pixels.data(), input_layout);
    target.mark_dirty();
}

void Device::blit_from_color_buffer(void* output_data, Vector2<i32> offset, GPU::ImageDataLayout const& output_layout)
{
    auto const& selection = output_layout.selection;
    Vector2<u32> size { selection.width, selection.height };
    Vector<u8> pixels;
    read_color_pixels(offset, size, pixels);
    auto input_layout = bgra8_layout(size);
    SoftGPU::PixelConverter converter { input_layout, output_layout };
    if (auto result = converter.convert(pixels.data(), output_data, {}); result.is_error())
        dbgln("EGLGPU: pixel conversion failed: {}", result.error().string_literal());
}

void Device::blit_from_depth_buffer(void* output_data, Vector2<i32> offset, GPU::ImageDataLayout const& output_layout)
{
    make_current();
    auto const& selection = output_layout.selection;
    Vector2<u32> size { selection.width, selection.height };

    Vector<float> pixels;
    pixels.resize(size.x() * size.y());
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (size.x() > 0 && size.y() > 0)
        glReadPixels(offset.x(), offset.y(), static_cast<GLsizei>(size.x()), static_cast<GLsizei>(size.y()), GL_DEPTH_COMPONENT, GL_FLOAT, pixels.data());

    auto input_layout = depth_float_layout(size);
    SoftGPU::PixelConverter converter { input_layout, output_layout };
    if (auto result = converter.convert(pixels.data(), output_data, {}); result.is_error())
        dbgln("EGLGPU: depth pixel conversion failed: {}", result.error().string_literal());
}

void Device::blit_from_depth_buffer(NonnullRefPtr<GPU::Image> image, u32 level, Vector2<u32> input_size, Vector2<i32> input_offset, Vector3<i32> output_offset)
{
    auto& target = static_cast<Image&>(*image);
    make_current();

    Vector<float> pixels;
    pixels.resize(input_size.x() * input_size.y());
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (input_size.x() > 0 && input_size.y() > 0)
        glReadPixels(input_offset.x(), input_offset.y(), static_cast<GLsizei>(input_size.x()), static_cast<GLsizei>(input_size.y()), GL_DEPTH_COMPONENT, GL_FLOAT, pixels.data());

    auto input_layout = depth_float_layout(input_size);
    target.storage().write_texels(level, output_offset, pixels.data(), input_layout);
    target.mark_dirty();
}

void Device::blit_to_color_buffer_at_raster_position(void const* input_data, GPU::ImageDataLayout const& input_layout)
{
    if (!m_raster_position_valid)
        return;
    make_current();

    auto const& selection = input_layout.selection;
    Vector<u8> converted;
    converted.resize(selection.width * selection.height * 4);
    auto output_layout = rgba8_layout({ selection.width, selection.height });
    SoftGPU::PixelConverter converter { input_layout, output_layout };
    if (auto result = converter.convert(input_data, converted.data(), {}); result.is_error()) {
        dbgln("EGLGPU: pixel conversion failed: {}", result.error().string_literal());
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glDrawPixels(selection.width, selection.height, GL_RGBA, GL_UNSIGNED_BYTE, converted.data());
}

void Device::blit_to_depth_buffer_at_raster_position(void const*, GPU::ImageDataLayout const&)
{
    dbgln("EGLGPU: blit_to_depth_buffer_at_raster_position() is not implemented");
}

}

extern "C" GPU::Device* serenity_gpu_create_device(Gfx::IntSize size)
{
    auto device = EGLGPU::Device::try_create(size);
    if (device.is_error()) {
        dbgln("EGLGPU: Mesa/EGL unavailable ({}); falling back to LibSoftGPU", device.error().string_literal());
        return make<SoftGPU::Device>(size).leak_ptr();
    }
    return device.release_value().leak_ptr();
}
