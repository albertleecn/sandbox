#include "index.hpp"
#include "gl-gizmo.hpp"

struct scene_editor_app : public GLFWApp
{
    GlCamera cam;
    FlyCameraController flycam;
    ShaderMonitor shaderMonitor{ "../assets/" };
    std::unique_ptr<gui::ImGuiManager> igm;
    GlGpuTimer gpuTimer;
    std::shared_ptr<GlShader> wireframeShader;

    std::vector<SimpleStaticMesh> objects;

    scene_editor_app();
    ~scene_editor_app();

    virtual void on_window_resize(int2 size) override;
    virtual void on_input(const InputEvent & event) override;
    virtual void on_update(const UpdateEvent & e) override;
    virtual void on_draw() override;
};