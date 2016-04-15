#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "gi_deferred_renderer.h"

#include "voxelizer_renderer.h"
#include "shadow_map_renderer.h"
#include "../scene/camera.h"
#include "../scene/scene.h"
#include "../scene/material.h"
#include "../scene/light.h"
#include "../rendering/render_window.h"
#include "../core/assets_manager.h"
#include "../programs/geometry_program.h"
#include "../programs/lighting_program.h"

#include <oglplus/bound/texture.hpp>
#include <oglplus/context.hpp>

GIDeferredRenderer::GIDeferredRenderer(RenderWindow &window) : Renderer(window)
{
    // create textures and attachments for framebuffer in deferredhandler
    SetupGeometryBuffer(Window().Info().width, Window().Info().height);
    // initial values
    maxTracingDistance = 0.95f;
    globalIlluminationStrength = 2.0f;
    ambientOcclusionFalloff = 800.0f;
    ambientOcclusionAlpha = 0.0f;
    renderMode = 0;
    fsQuad.Load();
}

GIDeferredRenderer::~GIDeferredRenderer()
{
}

void GIDeferredRenderer::Render()
{
    using namespace oglplus;
    static Context gl;
    static auto &camera = Camera::Active();
    static auto &scene = Scene::Active();

    if (!camera || !scene || !scene->IsLoaded() || VoxelizerRenderer::ShowVoxels)
    {
        return;
    }

    SetAsActive();
    // bind g buffer for writing
    geometryBuffer.Bind(FramebufferTarget::Draw);
    gl.ColorMask(true, true, true, true);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl.Viewport(Window().Info().width, Window().Info().height);
    gl.Clear().ColorBuffer().DepthBuffer();
    // activate geometry pass shader program
    CurrentProgram<GeometryProgram>(GeometryPass());
    // rendering and GL flags
    gl.ClearDepth(1.0f);
    gl.Enable(Capability::DepthTest);
    gl.Disable(Capability::Blend);
    gl.Enable(Capability::CullFace);
    gl.FrontFace(FaceOrientation::CCW);
    gl.CullFace(Face::Back);
    UseFrustumCulling = true;
    // draw whole scene tree from root node
    scene->rootNode->DrawList();
    // start light pass
    DefaultFramebuffer().Bind(FramebufferTarget::Draw);
    gl.ColorMask(true, true, true, true);
    gl.Viewport(Window().Info().width, Window().Info().height);
    gl.Clear().ColorBuffer().DepthBuffer();
    CurrentProgram<LightingProgram>(LightingPass());
    // pass light info and texture locations for final light pass
    SetLightPassUniforms();
    // draw the result onto a fullscreen quad
    fsQuad.DrawElements();
}

void GIDeferredRenderer::SetMatricesUniforms(const Node &node) const
{
    auto &prog = CurrentProgram<GeometryProgram>();
    static auto &camera = Camera::Active();
    prog.matrices.normal.Set(node.InverseTranspose());
    prog.matrices.modelViewProjection.Set(camera->ViewProjectionMatrix() *
                                          node.transform.Matrix());
}

void GIDeferredRenderer::SetMaterialUniforms(const Material &material)
const
{
    using namespace oglplus;
    auto &prog = CurrentProgram<GeometryProgram>();
    prog.material.diffuse.Set(material.Diffuse());
    prog.material.specular.Set(material.Specular());
    prog.material.emissive.Set(material.Emissive());
    // shininess curve, a bit smoother than linear
    prog.material.shininess.Set(sin(pow(material.Shininess(),
                                        3.0f) * glm::half_pi<float>()));
    prog.material.useNormalsMap.Set(material.HasTexture(RawTexture::Normals));
    // set textures
    Texture::Active(RawTexture::Diffuse);
    material.BindTexture(RawTexture::Diffuse);
    prog.diffuseMap.Set(RawTexture::Diffuse);
    Texture::Active(RawTexture::Specular);
    material.BindTexture(RawTexture::Specular);
    prog.specularMap.Set(RawTexture::Specular);
    Texture::Active(RawTexture::Normals);
    material.BindTexture(RawTexture::Normals);
    prog.normalsMap.Set(RawTexture::Normals);
}

const std::array<oglplus::Texture, 5> &GIDeferredRenderer::BufferTextures()
const
{
    return bufferTextures;
}

const float &GIDeferredRenderer::MaxTracingDistance() const
{
    return maxTracingDistance;
}

void GIDeferredRenderer::MaxTracingDistance(const float &val)
{
    maxTracingDistance = val;
}

const float &GIDeferredRenderer::GlobalIlluminationStrength() const
{
    return globalIlluminationStrength;
}

void GIDeferredRenderer::GlobalIlluminationStrength(const float &val)
{
    globalIlluminationStrength = val;
}

const float &GIDeferredRenderer::AmbientOclussionFalloff() const
{
    return ambientOcclusionFalloff;
}

void GIDeferredRenderer::AmbientOclussionFalloff(const float &val)
{
    ambientOcclusionFalloff = val;
}

const float &GIDeferredRenderer::AmbientOclussionAlpha() const
{
    return ambientOcclusionAlpha;
}

void GIDeferredRenderer::AmbientOclussionAlpha(const float &val)
{
    ambientOcclusionAlpha = val;
}

const unsigned &GIDeferredRenderer::RenderMode() const
{
    return renderMode;
}

void GIDeferredRenderer::RenderMode(const unsigned &mode)
{
    renderMode = mode;
}

void GIDeferredRenderer::SetLightPassUniforms() const
{
    static auto &camera = Camera::Active();
    static auto &scene = Scene::Active();
    auto &prog = CurrentProgram<LightingProgram>();;
    prog.cameraPosition.Set(camera->Position());
    prog.inverseProjectionView.Set(camera->InverseViewMatrix() *
                                   camera->InverseProjectionMatrix());

    for (int i = 0; i < bufferTextures.size(); i++)
    {
        bufferTextures[i].Active(i);
        bufferTextures[i].Bind(oglplus::TextureTarget::_2D);
    }

    // uniform arrays of lights
    auto &uDirectionals = prog.directionalLight;
    auto &uPoints = prog.pointLight;
    auto &uSpots = prog.spotLight;
    auto &lights = scene->lights;
    // index of directional-point-spot lights
    auto typeIndex = glm::uvec3(0);
    // pass number of lights per type
    prog.lightTypeCount[0].Set(static_cast<const unsigned int>
                               (Light::Directionals().size()));
    prog.lightTypeCount[1].Set(static_cast<const unsigned int>
                               (Light::Points().size()));
    prog.lightTypeCount[2].Set(static_cast<const unsigned int>
                               (Light::Spots().size()));

    for (int i = 0; i < lights.size(); ++i)
    {
        auto &light = lights[i];
        auto &factor = light->Intensities();
        auto shadowingMethod = light->mode[0].to_ulong();
        // current light uniform
        auto &uLight = light->Type() == Light::Directional
                       ? uDirectionals[typeIndex.x++]
                       : light->Type() == Light::Point
                       ? uPoints[typeIndex.y++]
                       : uSpots[typeIndex.z++];
        // shared uniforms between types
        uLight.shadowingMethod.Set(shadowingMethod);
        uLight.ambient.Set(light->Ambient() * factor.x);
        uLight.diffuse.Set(light->Diffuse() * factor.y);
        uLight.specular.Set(light->Specular() * factor.z);

        if (light->Type() == Light::Spot || light->Type() == Light::Point)
        {
            uLight.position.Set(light->Position());
        }

        if (light->Type() == Light::Spot || light->Type() == Light::Directional)
        {
            uLight.direction.Set(light->Direction());
        }

        if (light->Type() == Light::Spot || light->Type() == Light::Point)
        {
            uLight.attenuation.constant.Set(light->attenuation.Constant());
            uLight.attenuation.linear.Set(light->attenuation.Linear());
            uLight.attenuation.quadratic.Set(light->attenuation.Quadratic());
        }

        if(light->Type() == Light::Spot)
        {
            uLight.angleInnerCone.Set(cos(light->AngleInnerCone()));
            uLight.angleOuterCone.Set(cos(light->AngleOuterCone()));
        }
    }

    // pass shadowing parameters
    static auto &shadowing = *static_cast<ShadowMapRenderer *>
                             (AssetsManager::Instance()
                              ->renderers["Shadowmapping"].get());

    if(shadowing.Caster() != nullptr)
    {
        prog.lightViewProjection.Set(shadowing.LightSpaceMatrix());
        shadowing.BindReading(5);
        prog.exponents.Set(shadowing.Exponents());
        prog.lightBleedingReduction.Set(shadowing.LightBleedingReduction());
    }

    static auto &voxel = *static_cast<VoxelizerRenderer *>(AssetsManager::Instance()
                         ->renderers["Voxelizer"].get());
    prog.volumeDimension.Set(voxel.VolumeDimension());
    prog.voxelScale.Set(1.0f / voxel.VolumeGridSize());
    prog.worldMinPoint.Set(scene->rootNode->boundaries.MinPoint());
    voxel.VoxelRadiance().Active(6);
    voxel.VoxelRadiance().Bind(oglplus::TextureTarget::_3D);
    auto &mips = voxel.VoxelTextureMipmap();

    for (auto i = 0; i < mips.size(); i++)
    {
        mips[0].Active(7 + i);
        mips[0].Bind(oglplus::TextureTarget::_3D);
    }

    // global illum setup
    prog.maxTracingDistanceGlobal.Set(maxTracingDistance);
    prog.bounceStrength.Set(globalIlluminationStrength);
    prog.aoFalloff.Set(ambientOcclusionFalloff);
    prog.aoAlpha.Set(ambientOcclusionAlpha);
    prog.mode.Set(renderMode);
}

GeometryProgram &GIDeferredRenderer::GeometryPass()
{
    static auto &assets = AssetsManager::Instance();
    static auto &prog = *static_cast<GeometryProgram *>
                        (assets->programs["Geometry"].get());
    return prog;
}

LightingProgram &GIDeferredRenderer::LightingPass()
{
    static auto &assets = AssetsManager::Instance();
    static auto &prog = *static_cast<LightingProgram *>
                        (assets->programs["Lighting"].get());
    return prog;
}

void GIDeferredRenderer::SetupGeometryBuffer(unsigned windowWidth,
        unsigned windowHeight)
{
    using namespace oglplus;
    static Context gl;
    // initialize geometry buffer
    geometryBuffer.Bind(FramebufferTarget::Draw);
    // build textures -- normal
    gl.Bound(TextureTarget::_2D, bufferTextures[0])
    .Image2D(0, PixelDataInternalFormat::RGB8SNorm, windowWidth, windowHeight,
             0, PixelDataFormat::RGB, PixelDataType::UnsignedByte, nullptr)
    .MinFilter(TextureMinFilter::Nearest)
    .MagFilter(TextureMagFilter::Nearest);
    geometryBuffer.AttachColorTexture(FramebufferTarget::Draw, 0, bufferTextures[0],
                                      0);
    // build textures -- albedo
    gl.Bound(TextureTarget::_2D, bufferTextures[1])
    .Image2D(0, PixelDataInternalFormat::RGB8, windowWidth, windowHeight,
             0, PixelDataFormat::RGB, PixelDataType::UnsignedByte, nullptr)
    .MinFilter(TextureMinFilter::Nearest)
    .MagFilter(TextureMagFilter::Nearest);
    geometryBuffer.AttachColorTexture(FramebufferTarget::Draw, 1, bufferTextures[1],
                                      0);
    // build textures -- specular color and shininess
    gl.Bound(TextureTarget::_2D, bufferTextures[2])
    .Image2D(0, PixelDataInternalFormat::RGBA8, windowWidth, windowHeight,
             0, PixelDataFormat::RGBA, PixelDataType::UnsignedByte, nullptr)
    .MinFilter(TextureMinFilter::Nearest)
    .MagFilter(TextureMagFilter::Nearest);
    geometryBuffer.AttachColorTexture(FramebufferTarget::Draw, 2, bufferTextures[2],
                                      0);
    // emissivenes
    gl.Bound(TextureTarget::_2D, bufferTextures[3])
    .Image2D(0, PixelDataInternalFormat::RGB8, windowWidth, windowHeight,
             0, PixelDataFormat::RGB, PixelDataType::UnsignedByte, nullptr)
    .MinFilter(TextureMinFilter::Nearest)
    .MagFilter(TextureMagFilter::Nearest);
    geometryBuffer.AttachColorTexture(FramebufferTarget::Draw, 3, bufferTextures[3],
                                      0);
    // attach depth texture for depth testing
    gl.Bound(TextureTarget::_2D, bufferTextures[4])
    .Image2D(0, PixelDataInternalFormat::DepthComponent24, windowWidth,
             windowHeight, 0, PixelDataFormat::DepthComponent,
             PixelDataType::Float, nullptr)
    .MinFilter(TextureMinFilter::Nearest)
    .MagFilter(TextureMagFilter::Nearest);
    geometryBuffer.AttachTexture(FramebufferTarget::Draw,
                                 FramebufferAttachment::Depth,
                                 bufferTextures[4], 0);
    // color textures
    auto attachments = std::vector<Context::ColorBuffer>
    {
        FramebufferColorAttachment::_0 ,
        FramebufferColorAttachment::_1,
        FramebufferColorAttachment::_2,
        FramebufferColorAttachment::_3
    };
    // set draw buffers
    gl.DrawBuffers(attachments);

    // check if success building frame buffer
    if (!Framebuffer::IsComplete(FramebufferTarget::Draw))
    {
        auto status = Framebuffer::Status(FramebufferTarget::Draw);
        Framebuffer::HandleIncompleteError(FramebufferTarget::Draw, status);
    }

    Framebuffer::Bind(Framebuffer::Target::Draw, FramebufferName(0));
}