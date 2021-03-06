#include "SkyboxRenderer.h"
#include "DebugRenderer.h"
#include "MapObjectRenderer.h"
#include "../Utils/ServiceLocator.h"
#include "../ECS/Components/Singletons/MapSingleton.h"

#include <Renderer/Renderer.h>
#include <Renderer/RenderGraph.h>
#include <glm/gtc/matrix_transform.hpp>
#include <tracy/TracyVulkan.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <entt.hpp>
#include <InputManager.h>
#include <GLFW/glfw3.h>

#include "CameraFreelook.h"

SkyboxRenderer::SkyboxRenderer(Renderer::Renderer* renderer, DebugRenderer* debugRenderer)
    : _renderer(renderer)
    , _debugRenderer(debugRenderer)
{
    CreatePermanentResources();
}

SkyboxRenderer::~SkyboxRenderer()
{

}

void SkyboxRenderer::Update(f32 deltaTime, const CameraFreeLook& camera)
{

}

void SkyboxRenderer::AddSkyboxPass(Renderer::RenderGraph* renderGraph, Renderer::Buffer<ViewConstantBuffer>* viewConstantBuffer, Renderer::ImageID renderTarget, Renderer::DepthImageID depthTarget, u8 frameIndex, const CameraFreeLook& camera)
{
    struct TerrainPassData
    {
        Renderer::RenderPassMutableResource mainColor;
        Renderer::RenderPassMutableResource mainDepth;
    };

    const auto setup = [=](TerrainPassData& data, Renderer::RenderGraphBuilder& builder) 
    {
        data.mainColor = builder.Write(renderTarget, Renderer::RenderGraphBuilder::WriteMode::RENDERTARGET, Renderer::RenderGraphBuilder::LoadMode::CLEAR);
        data.mainDepth = builder.Write(depthTarget, Renderer::RenderGraphBuilder::WriteMode::RENDERTARGET, Renderer::RenderGraphBuilder::LoadMode::CLEAR);

        return true; // Return true from setup to enable this pass, return false to disable it
    };

    const auto execute = [=](TerrainPassData& data, Renderer::RenderGraphResources& resources, Renderer::CommandList& commandList)
    {
        GPU_SCOPED_PROFILER_ZONE(commandList, SkyboxPass);


    };

    renderGraph->AddPass<TerrainPassData>("Skybox Pass", setup, execute);
}

void SkyboxRenderer::CreatePermanentResources()
{
    
}
