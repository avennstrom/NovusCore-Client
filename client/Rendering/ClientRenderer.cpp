#include "ClientRenderer.h"
#include "UIRenderer.h"
#include "TerrainRenderer.h"
#include "CModelRenderer.h"
#include "PostProcessRenderer.h"
#include "RendertargetVisualizer.h"
#include "DebugRenderer.h"
#include "PixelQuery.h"
#include "CameraFreelook.h"
#include "../Utils/ServiceLocator.h"
#include "../ECS/Components/Singletons/MapSingleton.h"

#include <Memory/StackAllocator.h>
#include <Renderer/Renderer.h>
#include <Renderer/RenderGraph.h>
#include <Renderer/Renderers/Vulkan/RendererVK.h>
#include <Window/Window.h>
#include <InputManager.h>
#include <GLFW/glfw3.h>
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include "imgui/imgui_impl_glfw.h"
#include "imgui/implot.h"

#include "Renderer/Renderers/Vulkan/RendererVK.h"
#include "CullUtils.h"

AutoCVar_Int CVAR_LightLockEnabled("lights.lock", "lock the light", 0, CVarFlags::EditCheckbox);
AutoCVar_Int CVAR_LightUseDefaultEnabled("lights.useDefault", "Use the map's default light", 0, CVarFlags::EditCheckbox);

const size_t FRAME_ALLOCATOR_SIZE = 8 * 1024 * 1024; // 8 MB
u32 MAIN_RENDER_LAYER = "MainLayer"_h; // _h will compiletime hash the string into a u32
u32 DEPTH_PREPASS_RENDER_LAYER = "DepthPrepass"_h; // _h will compiletime hash the string into a u32

void KeyCallback(GLFWwindow* window, i32 key, i32 scancode, i32 action, i32 modifiers)
{
    Window* userWindow = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    ServiceLocator::GetInputManager()->KeyboardInputHandler(userWindow, key, scancode, action, modifiers);
}

void CharCallback(GLFWwindow* window, u32 unicodeKey)
{
    Window* userWindow = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    ServiceLocator::GetInputManager()->CharInputHandler(userWindow, unicodeKey);
}

void MouseCallback(GLFWwindow* window, i32 button, i32 action, i32 modifiers)
{
    Window* userWindow = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    ServiceLocator::GetInputManager()->MouseInputHandler(userWindow, button, action, modifiers);
}

void CursorPositionCallback(GLFWwindow* window, f64 x, f64 y)
{
    Window* userWindow = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    ServiceLocator::GetInputManager()->MousePositionHandler(userWindow, static_cast<f32>(x), static_cast<f32>(y));
}

void ScrollCallback(GLFWwindow* window, f64 x, f64 y)
{
    Window* userWindow = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    ServiceLocator::GetInputManager()->MouseScrollHandler(userWindow, static_cast<f32>(x), static_cast<f32>(y));
}

void WindowIconifyCallback(GLFWwindow* window, int iconified)
{
    Window* userWindow = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    userWindow->SetIsMinimized(iconified == 1);
}

ClientRenderer::ClientRenderer()
{
    _window = new Window();
    _window->Init(WIDTH, HEIGHT);
    ServiceLocator::SetWindow(_window);

    _inputManager = new InputManager();
    ServiceLocator::SetInputManager(_inputManager);

    glfwSetKeyCallback(_window->GetWindow(), KeyCallback);
    glfwSetCharCallback(_window->GetWindow(), CharCallback);
    glfwSetMouseButtonCallback(_window->GetWindow(), MouseCallback);
    glfwSetCursorPosCallback(_window->GetWindow(), CursorPositionCallback);
    glfwSetScrollCallback(_window->GetWindow(), ScrollCallback);
    glfwSetWindowIconifyCallback(_window->GetWindow(), WindowIconifyCallback);

    Renderer::TextureDesc debugTexture;
    debugTexture.path = "Data/textures/DebugTexture.bmp";
    
    _renderer = new Renderer::RendererVK(debugTexture);
    _renderer->InitWindow(_window);

    InitImgui();

    ServiceLocator::SetRenderer(_renderer);

    CreatePermanentResources();

    _debugRenderer = new DebugRenderer(_renderer);
    _uiRenderer = new UIRenderer(_renderer, _debugRenderer);
    _cModelRenderer = new CModelRenderer(_renderer, _debugRenderer);
    _postProcessRenderer = new PostProcessRenderer(_renderer);
    _rendertargetVisualizer = new RendertargetVisualizer(_renderer);
    _terrainRenderer = new TerrainRenderer(_renderer, _debugRenderer, _cModelRenderer);
    _pixelQuery = new PixelQuery(_renderer);

    ServiceLocator::SetClientRenderer(this);
}

bool ClientRenderer::UpdateWindow(f32 deltaTime)
{
    return _window->Update(deltaTime);
}

void ClientRenderer::Update(f32 deltaTime)
{
    // Reset the memory in the frameAllocator
    _frameAllocator->Reset();

    _terrainRenderer->Update(deltaTime);
    _cModelRenderer->Update(deltaTime);
    _postProcessRenderer->Update(deltaTime);
    _rendertargetVisualizer->Update(deltaTime);
    _pixelQuery->Update(deltaTime);
    _uiRenderer->Update(deltaTime);

    _debugRenderer->DrawLine3D(vec3(0.0f, 0.0f, 0.0f), vec3(100.0f, 0.0f, 0.0f), 0xff0000ff);
    _debugRenderer->DrawLine3D(vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 100.0f, 0.0f), 0xff00ff00);
    _debugRenderer->DrawLine3D(vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 0.0f, 100.0f), 0xffff0000);
}

void ClientRenderer::Render()
{
    ZoneScopedNC("ClientRenderer::Render", tracy::Color::Red2)

    // If the window is minimized we want to pause rendering
    if (_window->IsMinimized())
        return;

    Camera* camera = ServiceLocator::GetCamera();

    // Create rendergraph
    Renderer::RenderGraphDesc renderGraphDesc;
    renderGraphDesc.allocator = _frameAllocator; // We need to give our rendergraph an allocator to use
    Renderer::RenderGraph renderGraph = _renderer->CreateRenderGraph(renderGraphDesc);

    _renderer->FlipFrame(_frameIndex);

    // Update the view matrix to match the new camera position
    _viewConstantBuffer->resource.lastViewProjectionMatrix = _viewConstantBuffer->resource.viewProjectionMatrix;
    _viewConstantBuffer->resource.viewProjectionMatrix = camera->GetViewProjectionMatrix();
    _viewConstantBuffer->resource.eye = camera->GetPosition();
    _viewConstantBuffer->Apply(_frameIndex);

    entt::registry* registry = ServiceLocator::GetGameRegistry();
    MapSingleton& mapSingleton = registry->ctx<MapSingleton>();

    if (!CVAR_LightLockEnabled.Get())
    {
        _lightConstantBuffer->resource.ambientColor = vec4(mapSingleton.GetAmbientLight(), 1.0f);
        _lightConstantBuffer->resource.lightColor = vec4(mapSingleton.GetDiffuseLight(), 1.0f);
        _lightConstantBuffer->resource.lightDir = vec4(mapSingleton.GetLightDirection(), 1.0f);
        _lightConstantBuffer->Apply(_frameIndex);
    }

    _globalDescriptorSet.Bind("_viewData"_h, _viewConstantBuffer->GetBuffer(_frameIndex));
    _globalDescriptorSet.Bind("_lightData"_h, _lightConstantBuffer->GetBuffer(_frameIndex));

    _debugRenderer->AddUploadPass(&renderGraph);

    // Clear Pass
    {
        struct ClearPassData
        {
            Renderer::RenderPassMutableResource mainDepth;
        };

        renderGraph.AddPass<ClearPassData>("ClearPass",
            [=](ClearPassData& data, Renderer::RenderGraphBuilder& builder) // Setup
        {
            data.mainDepth = builder.Write(_mainDepth, Renderer::RenderGraphBuilder::WriteMode::RENDERTARGET, Renderer::RenderGraphBuilder::LoadMode::CLEAR);

            return true; // Return true from setup to enable this pass, return false to disable it
        },
            [&](ClearPassData& data, Renderer::RenderGraphResources& resources, Renderer::CommandList& commandList) // Execute
        {
            GPU_SCOPED_PROFILER_ZONE(commandList, MainPass);
            commandList.MarkFrameStart(_frameIndex);

            Renderer::GraphicsPipelineDesc pipelineDesc;
            resources.InitializePipelineDesc(pipelineDesc);

            // Shaders
            Renderer::VertexShaderDesc vertexShaderDesc;
            vertexShaderDesc.path = "depthprepass.vs.hlsl";
            pipelineDesc.states.vertexShader = _renderer->LoadShader(vertexShaderDesc);

            // Depth state
            pipelineDesc.states.depthStencilState.depthEnable = true;
            pipelineDesc.states.depthStencilState.depthWriteEnable = true;
            pipelineDesc.states.depthStencilState.depthFunc = Renderer::ComparisonFunc::GREATER;

            // Render targets
            pipelineDesc.depthStencil = data.mainDepth;

            // Clear TODO: This should be handled by the parameters in Setup, and it should definitely not act on ImageID and DepthImageID
            commandList.Clear(_mainColor, Color(135.0f / 255.0f, 206.0f / 255.0f, 250.0f / 255.0f, 1));
            commandList.Clear(_objectIDs, Color(0.0f, 0.0f, 0.0f, 0.0f));
            commandList.Clear(_mainDepth, 0.0f);

            // Set viewport
            commandList.SetViewport(0, 0, static_cast<f32>(WIDTH), static_cast<f32>(HEIGHT), 0.0f, 1.0f);
            commandList.SetScissorRect(0, WIDTH, 0, HEIGHT);
        });
    }

    _terrainRenderer->AddTerrainPass(&renderGraph, &_globalDescriptorSet, _debugRenderer->GetDescriptorSet(), _mainColor, _objectIDs, _mainDepth, _depthPyramid, _frameIndex);
    _cModelRenderer->AddComplexModelPass(&renderGraph, &_globalDescriptorSet, _debugRenderer->GetDescriptorSet(), _mainColor, _objectIDs, _mainDepth, _depthPyramid, _frameIndex);
    _postProcessRenderer->AddPostProcessPass(&renderGraph, &_globalDescriptorSet, _mainColor, _objectIDs, _mainDepth, _depthPyramid, _frameIndex);
    _rendertargetVisualizer->AddVisualizerPass(&renderGraph, &_globalDescriptorSet, _mainColor, _frameIndex);

    // UI Pass
    struct PyramidPassData
    {
        Renderer::RenderPassResource mainDepth;
    };

    renderGraph.AddPass<PyramidPassData>("PyramidPass",
        [=](PyramidPassData& data, Renderer::RenderGraphBuilder& builder) // Setup
        {
            data.mainDepth = builder.Read(_mainDepth, Renderer::RenderGraphBuilder::ShaderStage::PIXEL);

            return true; // Return true from setup to enable this pass, return false to disable it
        },
        [=](PyramidPassData& data, Renderer::RenderGraphResources& resources, Renderer::CommandList& commandList) // Execute
        {
            GPU_SCOPED_PROFILER_ZONE(commandList, ImguiPass);

            DepthPyramidUtils::BuildPyramid(_renderer,resources, commandList,_frameIndex, _mainDepth, _depthPyramid);
        });

    _pixelQuery->AddPixelQueryPass(&renderGraph, _mainColor, _objectIDs, _mainDepth, _frameIndex);

    _debugRenderer->AddDrawArgumentPass(&renderGraph, _frameIndex);
    _debugRenderer->Add3DPass(&renderGraph, &_globalDescriptorSet, _mainColor, _mainDepth, _frameIndex);

    _uiRenderer->AddUIPass(&renderGraph, _mainColor, _frameIndex);

    _debugRenderer->Add2DPass(&renderGraph, &_globalDescriptorSet, _mainColor, _mainDepth, _frameIndex);

    _uiRenderer->AddImguiPass(&renderGraph, _mainColor, _frameIndex);

    renderGraph.AddSignalSemaphore(_sceneRenderedSemaphore); // Signal that we are ready to present
    renderGraph.AddSignalSemaphore(_frameSyncSemaphores.Get(_frameIndex)); // Signal that this frame has finished, for next frames sake

    static bool firstFrame = true;
    if (firstFrame)
    {
        firstFrame = false;
    }
    else
    {
        renderGraph.AddWaitSemaphore(_frameSyncSemaphores.Get(!_frameIndex)); // Wait for previous frame to finish
    }

    renderGraph.Setup();
    renderGraph.Execute();
    
    {
        ZoneScopedNC("Present", tracy::Color::Red2);
        _renderer->Present(_window, _mainColor, _sceneRenderedSemaphore);
    }

    // Flip the frameIndex between 0 and 1
    _frameIndex = !_frameIndex;
}

uvec2 ClientRenderer::GetRenderResolution()
{
    return _renderer->GetImageDimension(_mainColor, 0);
}

void ClientRenderer::InitImgui()
{
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui_ImplGlfw_InitForVulkan(_window->GetWindow(),true);

    _renderer->InitImgui();
}

void ClientRenderer::ReloadShaders(bool forceRecompileAll)
{
    _renderer->ReloadShaders(forceRecompileAll);
}

const std::string& ClientRenderer::GetGPUName()
{
    return _renderer->GetGPUName();
}

size_t ClientRenderer::GetVRAMUsage()
{
    return _renderer->GetVRAMUsage();
}

size_t ClientRenderer::GetVRAMBudget()
{
    return _renderer->GetVRAMBudget();
}

void ClientRenderer::CreatePermanentResources()
{
    // Main color rendertarget
    Renderer::ImageDesc mainColorDesc;
    mainColorDesc.debugName = "MainColor";
    mainColorDesc.dimensions = vec2(1.0f, 1.0f);
    mainColorDesc.dimensionType = Renderer::ImageDimensionType::DIMENSION_SCALE;
    mainColorDesc.format = Renderer::ImageFormat::R16G16B16A16_FLOAT;
    mainColorDesc.sampleCount = Renderer::SampleCount::SAMPLE_COUNT_1;

    _mainColor = _renderer->CreateImage(mainColorDesc);

    // Object ID rendertarget
    Renderer::ImageDesc objectIDsDesc;
    objectIDsDesc.debugName = "ObjectIDs";
    objectIDsDesc.dimensions = vec2(1.0f, 1.0f);
    objectIDsDesc.dimensionType = Renderer::ImageDimensionType::DIMENSION_SCALE;
    objectIDsDesc.format = Renderer::ImageFormat::R32_UINT;
    objectIDsDesc.sampleCount = Renderer::SampleCount::SAMPLE_COUNT_1;

    _objectIDs = _renderer->CreateImage(objectIDsDesc);

    // depth pyramid ID rendertarget
    Renderer::ImageDesc pyramidDesc;
    pyramidDesc.debugName = "DepthPyramid";
    pyramidDesc.dimensions = vec2(1.0f, 1.0f);
    pyramidDesc.dimensionType = Renderer::ImageDimensionType::DIMENSION_PYRAMID;
    pyramidDesc.format = Renderer::ImageFormat::R32_FLOAT;
    pyramidDesc.sampleCount = Renderer::SampleCount::SAMPLE_COUNT_1;
    _depthPyramid = _renderer->CreateImage(pyramidDesc);

    // Main depth rendertarget
    Renderer::DepthImageDesc mainDepthDesc;
    mainDepthDesc.debugName = "MainDepth";
    mainDepthDesc.dimensions = vec2(1.0f, 1.0f);
    mainDepthDesc.dimensionType = Renderer::ImageDimensionType::DIMENSION_SCALE;
    mainDepthDesc.format = Renderer::DepthImageFormat::D32_FLOAT;
    mainDepthDesc.sampleCount = Renderer::SampleCount::SAMPLE_COUNT_1;

    _mainDepth = _renderer->CreateDepthImage(mainDepthDesc);

    // View Constant Buffer (for camera data)
    _viewConstantBuffer = new Renderer::Buffer<ViewConstantBuffer>(_renderer, "ViewConstantBuffer", Renderer::BufferUsage::UNIFORM_BUFFER, Renderer::BufferCPUAccess::WriteOnly);

    // Light Constant Buffer
    _lightConstantBuffer = new Renderer::Buffer<LightConstantBuffer>(_renderer, "LightConstantBufffer", Renderer::BufferUsage::UNIFORM_BUFFER, Renderer::BufferCPUAccess::WriteOnly);

    // Frame allocator, this is a fast allocator for data that is only needed this frame
    _frameAllocator = new Memory::StackAllocator(FRAME_ALLOCATOR_SIZE);
    _frameAllocator->Init();

    _sceneRenderedSemaphore = _renderer->CreateGPUSemaphore();
    for (u32 i = 0; i < _frameSyncSemaphores.Num; i++)
    {
        _frameSyncSemaphores.Get(i) = _renderer->CreateGPUSemaphore();
    }
}
