/*
 * EGLGPU — a Mesa-backed GPU::Device.
 *
 * Implements SerenityOS's GPU::Device fixed-function drawing interface on top of
 * a desktop OpenGL compatibility context obtained through EGL (surfaceless),
 * rendering into an offscreen framebuffer that is read back into a Gfx::Bitmap.
 *
 * When no usable Mesa/EGL context can be created the factory in EGLGPU.cpp hands
 * back a LibSoftGPU::Device instead, so GL always works — just slower.
 */
#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibGPU/Device.h>
#include <LibGPU/DeviceInfo.h>
#include <LibGPU/Enums.h>
#include <LibGPU/Image.h>
#include <LibGPU/ImageDataLayout.h>
#include <LibGPU/Light.h>
#include <LibGPU/LightModelParameters.h>
#include <LibGPU/Material.h>
#include <LibGPU/RasterPosition.h>
#include <LibGPU/RasterizerOptions.h>
#include <LibGPU/SamplerConfig.h>
#include <LibGPU/StencilConfiguration.h>
#include <LibGPU/TextureUnitConfiguration.h>
#include <LibGPU/Vertex.h>
#include <LibSoftGPU/Image.h>

#include <EGL/egl.h>
#include <GL/gl.h>

namespace EGLGPU {

class Device;

// A GPU::Image whose texels live in a SoftGPU::Image (RGBA FloatVector4, with
// full format conversion) and which owns a backing OpenGL texture for sampling.
class Image final : public GPU::Image {
public:
    Image(Device const& owner, GPU::PixelFormat const&, u32 width, u32 height, u32 depth, u32 max_levels);

    virtual void regenerate_mipmaps() override;
    virtual void write_texels(u32 level, Vector3<i32> const& output_offset, void const* input_data, GPU::ImageDataLayout const&) override;
    virtual void read_texels(u32 level, Vector3<i32> const& input_offset, void* output_data, GPU::ImageDataLayout const&) const override;
    virtual void copy_texels(GPU::Image const& source, u32 source_level, Vector3<u32> const& source_offset, Vector3<u32> const& size, u32 destination_level, Vector3<u32> const& destination_offset) override;

    SoftGPU::Image& storage() { return m_storage; }
    SoftGPU::Image const& storage() const { return m_storage; }

    GLuint texture() const { return m_texture; }
    void set_texture(GLuint texture) { m_texture = texture; }
    bool is_dirty() const { return m_dirty; }
    void mark_dirty() { m_dirty = true; }
    void clear_dirty() { m_dirty = false; }

private:
    NonnullRefPtr<SoftGPU::Image> m_storage;
    GLuint m_texture { 0 };
    bool m_dirty { true };
};

class Device final : public GPU::Device {
public:
    static ErrorOr<NonnullOwnPtr<Device>> try_create(Gfx::IntSize size);
    virtual ~Device() override;

    virtual GPU::DeviceInfo info() const override { return m_info; }

    virtual void draw_primitives(GPU::PrimitiveType, Vector<GPU::Vertex>& vertices) override;
    virtual void resize(Gfx::IntSize size) override;
    virtual void clear_color(FloatVector4 const&) override;
    virtual void clear_depth(GPU::DepthType) override;
    virtual void clear_stencil(GPU::StencilType) override;
    virtual void blit_from_color_buffer(Gfx::Bitmap& target) override;
    virtual void blit_from_color_buffer(NonnullRefPtr<GPU::Image>, u32 level, Vector2<u32> input_size, Vector2<i32> input_offset, Vector3<i32> output_offset) override;
    virtual void blit_from_color_buffer(void*, Vector2<i32> offset, GPU::ImageDataLayout const&) override;
    virtual void blit_from_depth_buffer(void*, Vector2<i32> offset, GPU::ImageDataLayout const&) override;
    virtual void blit_from_depth_buffer(NonnullRefPtr<GPU::Image>, u32 level, Vector2<u32> input_size, Vector2<i32> input_offset, Vector3<i32> output_offset) override;
    virtual void blit_to_color_buffer_at_raster_position(void const*, GPU::ImageDataLayout const&) override;
    virtual void blit_to_depth_buffer_at_raster_position(void const*, GPU::ImageDataLayout const&) override;
    virtual void set_options(GPU::RasterizerOptions const&) override;
    virtual void set_light_model_params(GPU::LightModelParameters const&) override;
    virtual GPU::RasterizerOptions options() const override { return m_options; }
    virtual GPU::LightModelParameters light_model() const override { return m_light_model; }

    virtual NonnullRefPtr<GPU::Image> create_image(GPU::PixelFormat const&, u32 width, u32 height, u32 depth, u32 max_levels) override;
    virtual ErrorOr<NonnullRefPtr<GPU::Shader>> create_shader(GPU::IR::Shader const&) override;

    virtual void set_model_view_transform(FloatMatrix4x4 const&) override;
    virtual void set_projection_transform(FloatMatrix4x4 const&) override;
    virtual void set_sampler_config(unsigned, GPU::SamplerConfig const&) override;
    virtual void set_light_state(unsigned, GPU::Light const&) override;
    virtual void set_material_state(GPU::Face, GPU::Material const&) override;
    virtual void set_stencil_configuration(GPU::Face, GPU::StencilConfiguration const&) override;
    virtual void set_texture_unit_configuration(GPU::TextureUnitIndex, GPU::TextureUnitConfiguration const&) override;
    virtual void set_clip_planes(Vector<FloatVector4> const&) override;

    virtual GPU::RasterPosition raster_position() const override { return m_raster_position; }
    virtual void set_raster_position(GPU::RasterPosition const&) override;
    virtual void set_raster_position(FloatVector4 const& position) override;

    virtual void bind_fragment_shader(RefPtr<GPU::Shader>) override;

private:
    Device(EGLDisplay, EGLContext, Gfx::IntSize);

    void make_current();
    void create_framebuffer(Gfx::IntSize);
    void apply_options();
    void bind_and_upload_image(Image&, GPU::SamplerConfig const&);
    void read_color_pixels(Vector2<i32> offset, Vector2<u32> size, Vector<u8>& out);

    EGLDisplay m_display { EGL_NO_DISPLAY };
    EGLContext m_context { EGL_NO_CONTEXT };
    GPU::DeviceInfo m_info;
    GLuint m_fbo { 0 };
    GLuint m_color_texture { 0 };
    GLuint m_depth_stencil_buffer { 0 };
    Gfx::IntSize m_size;
    GPU::RasterizerOptions m_options;
    GPU::LightModelParameters m_light_model;
    GPU::RasterPosition m_raster_position;
    bool m_raster_position_valid { false };
    Vector<GLuint> m_allocated_textures;
};

}

extern "C" GPU::Device* serenity_gpu_create_device(Gfx::IntSize size);
