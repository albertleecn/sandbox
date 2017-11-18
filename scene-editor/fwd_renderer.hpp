#pragma once

#ifndef vr_renderer_hpp
#define vr_renderer_hpp

#include "projection-math.hpp"
#include "linalg_util.hpp"
#include "geometric.hpp"
#include "geometry.hpp"
#include "procedural_mesh.hpp"
#include "simple_timer.hpp"
#include "uniforms.hpp"
#include "circular_buffer.hpp"
#include "human_time.hpp"

#include "gl-camera.hpp"
#include "gl-async-gpu-timer.hpp"
#include "gl-procedural-sky.hpp"
#include "gl-imgui.hpp"

#include "scene.hpp"
#include "bloom_pass.hpp"
#include "shadow_pass.hpp"

using namespace avl;

inline bool take_screenshot(int2 size)
{
    HumanTime t;
    std::vector<uint8_t> screenShot(size.x * size.y * 3);
    glReadPixels(0, 0, size.x, size.y, GL_DEPTH_COMPONENT, GL_FLOAT, screenShot.data());
    auto flipped = screenShot;
    for (int y = 0; y<size.y; ++y) memcpy(flipped.data() + y*size.x * 1, screenShot.data() + (size.y - y - 1)*size.x * 1, size.x * 1);
    stbi_write_png(std::string("depth_render_" + t.make_timestamp() + ".png").c_str(), size.x, size.y, 1, flipped.data(), 1 * size.x);
    return false;
}

struct CameraData
{
    uint32_t index;
    Pose pose;
    float4x4 viewMatrix;
    float4x4 projectionMatrix;
    float4x4 viewProjMatrix;
};

template<uint32_t NumEyes>
class PhysicallyBasedRenderer
{
    float2 renderSizePerEye;

    GlGpuTimer earlyZTimer;
    GlGpuTimer forwardTimer;
    GlGpuTimer shadowTimer;
    GlGpuTimer postTimer;

    SimpleTimer timer;

    GlBuffer perScene;
    GlBuffer perView;
    GlBuffer perObject;

    CameraData cameras[NumEyes];

    // MSAA 
    GlRenderbuffer multisampleRenderbuffers[2];
    GlFramebuffer multisampleFramebuffer;

    // Non-MSAA Targets
    GlFramebuffer eyeFramebuffers[NumEyes];
    GlTexture2D eyeTextures[NumEyes];
    GlTexture2D eyeDepthTextures[NumEyes];

    GLuint outputTextureHandles[NumEyes];
    GLuint outputDepthTextureHandles[NumEyes];

    std::vector<Renderable *> renderSet;
    std::vector<uniforms::point_light *> pointLights;

    uniforms::directional_light sunlight;
    ProceduralSky * skybox{ nullptr };

    std::unique_ptr<BloomPass> bloom;
    std::unique_ptr<StableCascadedShadowPass> shadow;

    GlShaderHandle earlyZPass = { "depth-prepass" };

    void run_depth_prepass(const CameraData & d)
    {
        glEnable(GL_DEPTH_TEST);

        earlyZTimer.start();

        /*
        auto distanceSortFunc = [&d](Renderable * lhs, Renderable * rhs)
        {
            const float3 cameraWorldspace = d.pose.position;
            const float lDist = distance(cameraWorldspace, lhs->get_pose().position);
            const float rDist = distance(cameraWorldspace, rhs->get_pose().position);
            return lDist < rDist;
        };

        std::priority_queue<Renderable *, std::vector<Renderable*>, decltype(distanceSortFunc)> renderQueueDefault(distanceSortFunc);
        */

       // for (auto obj : renderSet) renderQueueDefault.push(obj);

        glDepthFunc(GL_LESS);           // Nearest pixel
        glDepthMask(GL_TRUE);           // Need depth mask on
        glColorMask(0, 0, 0, 0);        // Do not write color

        // Update per-object uniform buffer
        auto update_per_object = [&](Renderable * top)
        {
            uniforms::per_object object = {};
            object.modelMatrix = mul(top->get_pose().matrix(), make_scaling_matrix(top->get_scale()));
            object.modelMatrixIT = inverse(transpose(object.modelMatrix));
            object.modelViewMatrix = mul(d.viewMatrix, object.modelMatrix);
            object.receiveShadow = (float)top->get_receive_shadow();
            perObject.set_buffer_data(sizeof(object), &object, GL_STREAM_DRAW);
        };

        auto & shader = earlyZPass.get();
        shader.bind();

        for (auto obj : renderSet)
        {
            update_per_object(obj);
            obj->draw();
        }

        /*
        while (!renderQueueDefault.empty())
        {
            Renderable * top = renderQueueDefault.top();
            renderQueueDefault.pop();
            update_per_object(top);
            top->draw();
        }
        */

        shader.unbind();

        earlyZTimer.stop();
    }

    void run_skybox_pass(const CameraData & d)
    {   
        if (!skybox) return;

        GLboolean wasDepthTestingEnabled = glIsEnabled(GL_DEPTH_TEST);
        glDisable(GL_DEPTH_TEST);
        skybox->render(d.viewProjMatrix, d.pose.position, near_far_clip_from_projection(d.projectionMatrix).y);
        if (wasDepthTestingEnabled) glEnable(GL_DEPTH_TEST);
    }

    void run_shadow_pass(const CameraData & d)
    {
        const float2 nearFarClip = near_far_clip_from_projection(d.projectionMatrix);

        shadow->update_cascades(make_view_matrix_from_pose(d.pose),
            nearFarClip.x,
            nearFarClip.y,
            aspect_from_projection(d.projectionMatrix),
            vfov_from_projection(d.projectionMatrix),
            sunlight.direction);

        shadow->pre_draw();

        gl_check_error(__FILE__, __LINE__);

        for (Renderable * obj : renderSet)
        {
            if (obj->get_cast_shadow())
            {
                float4x4 modelMatrix = mul(obj->get_pose().matrix(), make_scaling_matrix(obj->get_scale()));
                shadow->program.get().uniform("u_modelShadowMatrix", modelMatrix);
                obj->draw();
            }
        }

        shadow->post_draw();

        gl_check_error(__FILE__, __LINE__);
    }

    void run_forward_pass(const CameraData & d)
    {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glColorMask(1, 1, 1, 1);    // re-enable color mask after z prepass
        glDepthMask(GL_FALSE);      // depth already comes from the prepass

        // Follows sorting strategy outlined here: 
        // http://realtimecollisiondetection.net/blog/?p=86
        // todo - sorting is is done per-eye but should be done per frame instead

        auto materialSortFunc = [&d](Renderable * lhs, Renderable * rhs)
        {
            const float3 cameraWorldspace = d.pose.position;
            const float lDist = distance(cameraWorldspace, lhs->get_pose().position);
            const float rDist = distance(cameraWorldspace, rhs->get_pose().position);

            auto lid = lhs->get_material()->id();
            auto rid = rhs->get_material()->id();
            if (lid != rid) return lid > rid;

            return lDist < rDist;
        };

        auto distanceSortFunc = [&d](Renderable * lhs, Renderable * rhs)
        {
            const float3 cameraWorldspace = d.pose.position;
            const float lDist = distance(cameraWorldspace, lhs->get_pose().position);
            const float rDist = distance(cameraWorldspace, rhs->get_pose().position);
            return lDist < rDist;
        };

        std::priority_queue<Renderable *, std::vector<Renderable*>, decltype(materialSortFunc)> renderQueueMaterial(materialSortFunc);
        std::priority_queue<Renderable *, std::vector<Renderable*>, decltype(distanceSortFunc)> renderQueueDefault(distanceSortFunc);

        for (auto obj : renderSet) 
        { 
            // Can't sort by material if the renderable doesn't *have* a material; 
            // bucket all other objects 
            if (obj->get_material() != nullptr) renderQueueMaterial.push(obj);
            else renderQueueDefault.push(obj);
        }

        // Update per-object uniform buffer
        auto update_per_object = [&](Renderable * top)
        {
            uniforms::per_object object = {};
            object.modelMatrix = mul(top->get_pose().matrix(), make_scaling_matrix(top->get_scale()));
            object.modelMatrixIT = inverse(transpose(object.modelMatrix));
            object.modelViewMatrix = mul(d.viewMatrix, object.modelMatrix);
            object.receiveShadow = (float)top->get_receive_shadow();
            perObject.set_buffer_data(sizeof(object), &object, GL_STREAM_DRAW);
        };

        while (!renderQueueMaterial.empty())
        {
            Renderable * top = renderQueueMaterial.top();
            renderQueueMaterial.pop();

            update_per_object(top);
            Material * mat = top->get_material();

            mat->update_uniforms();

            if (auto * mr = dynamic_cast<MetallicRoughnessMaterial*>(mat))
            {
                mr->update_cascaded_shadow_array_handle(shadow->get_output_texture());
            }

            mat->use();

            top->draw();
        }

        // We assume that objects without a valid material take care of their own shading in the `draw()` function. 
        while (!renderQueueDefault.empty())
        {
            Renderable * top = renderQueueDefault.top();
            renderQueueDefault.pop();
            update_per_object(top);
            top->draw();
        }

        gl_check_error(__FILE__, __LINE__);
    }

    void run_post_pass(const CameraData & d)
    {
        GLboolean wasCullingEnabled = glIsEnabled(GL_CULL_FACE);
        GLboolean wasDepthTestingEnabled = glIsEnabled(GL_DEPTH_TEST);

        // Disable culling and depth testing for post processing
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);

        run_bloom_pass(d);

        if (wasCullingEnabled) glEnable(GL_CULL_FACE);
        if (wasDepthTestingEnabled) glEnable(GL_DEPTH_TEST);
    }

    void run_bloom_pass(const CameraData & d)
    {
        bloom->execute(eyeTextures[d.index]);
        glBlitNamedFramebuffer(bloom->get_output_texture(), eyeTextures[d.index], 0, 0, renderSizePerEye.x, renderSizePerEye.y, 0, 0, renderSizePerEye.x, renderSizePerEye.y, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        gl_check_error(__FILE__, __LINE__);
    }

public:

    PhysicallyBasedRenderer(const float2 render_target_size) : renderSizePerEye(render_target_size)
    {
        assert(renderSizePerEye.x >= 0 && renderSizePerEye.y >= 0);
        assert(NumEyes >= 1);

        // Generate multisample render buffers for color and depth, attach to multi-sampled framebuffer target
        glNamedRenderbufferStorageMultisampleEXT(multisampleRenderbuffers[0], 4, GL_RGBA8, renderSizePerEye.x, renderSizePerEye.y);
        glNamedFramebufferRenderbufferEXT(multisampleFramebuffer, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, multisampleRenderbuffers[0]);
        glNamedRenderbufferStorageMultisampleEXT(multisampleRenderbuffers[1], 4, GL_DEPTH_COMPONENT, renderSizePerEye.x, renderSizePerEye.y);
        glNamedFramebufferRenderbufferEXT(multisampleFramebuffer, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, multisampleRenderbuffers[1]);

        multisampleFramebuffer.check_complete();

        // Generate textures and framebuffers for `NumEyes`
        for (int eyeIndex = 0; eyeIndex < NumEyes; ++eyeIndex)
        {
            glTextureImage2DEXT(eyeTextures[eyeIndex], GL_TEXTURE_2D, 0, GL_RGBA8, renderSizePerEye.x, renderSizePerEye.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTextureParameteriEXT(eyeTextures[eyeIndex], GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteriEXT(eyeTextures[eyeIndex], GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteriEXT(eyeTextures[eyeIndex], GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteriEXT(eyeTextures[eyeIndex], GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTextureParameteriEXT(eyeTextures[eyeIndex], GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

            // Depth tex
            eyeDepthTextures[eyeIndex].setup(renderSizePerEye.x, renderSizePerEye.y, GL_DEPTH_COMPONENT32, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

            glNamedFramebufferTexture2DEXT(eyeFramebuffers[eyeIndex], GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eyeTextures[eyeIndex], 0);
            glNamedFramebufferTexture2DEXT(eyeFramebuffers[eyeIndex], GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, eyeDepthTextures[eyeIndex], 0);

            eyeFramebuffers[eyeIndex].check_complete();
        }

        shadow.reset(new StableCascadedShadowPass());
        bloom.reset(new BloomPass(renderSizePerEye));

        timer.start();
    }

    ~PhysicallyBasedRenderer()
    {
        timer.stop();
    }

    void render_frame()
    {
        // Renderer default state
        glEnable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);

        glEnable(GL_FRAMEBUFFER_SRGB);

        glBindBufferBase(GL_UNIFORM_BUFFER, uniforms::per_scene::binding, perScene);
        glBindBufferBase(GL_UNIFORM_BUFFER, uniforms::per_view::binding, perView);
        glBindBufferBase(GL_UNIFORM_BUFFER, uniforms::per_object::binding, perObject);

        // Update per-scene uniform buffer
        uniforms::per_scene b = {};
        b.time = timer.milliseconds().count();
        b.resolution = renderSizePerEye;
        b.invResolution = 1.f / b.resolution;
        b.activePointLights = pointLights.size();

        b.directional_light.color = sunlight.color;
        b.directional_light.direction = sunlight.direction;
        b.directional_light.amount = sunlight.amount;
        for (int i = 0; i < (int)std::min(pointLights.size(), size_t(uniforms::MAX_POINT_LIGHTS)); ++i) b.point_lights[i] = *pointLights[i];

        GLfloat defaultColor[] = { 0.0f, 0.0f, 1.f, 1.0f };
        GLfloat defaultDepth = 1.f;
    
        shadowTimer.start();

        if (shadow->enabled) // render shadows
        {
            // Default to the first camera
            CameraData shadowCamera = cameras[0];

            // In VR, we create a virtual camera in between both eyes.
            // todo - this is somewhat wrong since we need to actually create a superfrustum
            // which is max(left, right)
            if (NumEyes == 2)
            {
                // Average the positions and re-generate the relevant matrices
                const float3 centerPosition = (cameras[0].pose.position + cameras[1].pose.position) * 0.5f;
                shadowCamera.pose.position = centerPosition;
                shadowCamera.viewMatrix = make_view_matrix_from_pose(shadowCamera.pose);
                shadowCamera.viewProjMatrix = mul(shadowCamera.projectionMatrix, shadowCamera.viewMatrix);
            }

            run_shadow_pass(shadowCamera);

            for (int c = 0; c < uniforms::NUM_CASCADES; c++)
            {
                b.cascadesPlane[c] = float4(shadow->splitPlanes[c].x, shadow->splitPlanes[c].y, 0, 0);
                b.cascadesMatrix[c] = shadow->shadowMatrices[c];
                b.cascadesNear[c] = shadow->nearPlanes[c];
                b.cascadesFar[c] = shadow->farPlanes[c];
            }
        }

        shadowTimer.stop();

        forwardTimer.start();

        // Per-scene can be uploaded now that the shadow pass has completed
        perScene.set_buffer_data(sizeof(b), &b, GL_STREAM_DRAW);

        for (int eyeIdx = 0; eyeIdx < NumEyes; ++eyeIdx)
        {
            // Update per-view uniform buffer
            uniforms::per_view v = {};
            v.view = cameras[eyeIdx].pose.inverse().matrix();
            v.viewProj = mul(cameras[eyeIdx].projectionMatrix, cameras[eyeIdx].pose.inverse().matrix());
            v.eyePos = float4(cameras[eyeIdx].pose.position, 1);
            perView.set_buffer_data(sizeof(v), &v, GL_STREAM_DRAW);

            // Update render pass data
            cameras[eyeIdx].viewMatrix = v.view;
            cameras[eyeIdx].viewProjMatrix = v.viewProj;

            // Render into 4x multisampled fbo
            glEnable(GL_MULTISAMPLE);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, multisampleFramebuffer);

            glViewport(0, 0, renderSizePerEye.x, renderSizePerEye.y);

            glClearNamedFramebufferfv(multisampleFramebuffer, GL_COLOR, 0, &defaultColor[0]);
            glClearNamedFramebufferfv(multisampleFramebuffer, GL_DEPTH, 0, &defaultDepth);

            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Execute the forward passes
            //run_depth_prepass(cameras[eyeIdx]);
            run_skybox_pass(cameras[eyeIdx]);
            run_forward_pass(cameras[eyeIdx]);

            glDisable(GL_MULTISAMPLE);

            // Resolve multisample into per-eye framebuffers

            // blit color 
            glBlitNamedFramebuffer(multisampleFramebuffer, eyeFramebuffers[eyeIdx], 
                0, 0, renderSizePerEye.x, renderSizePerEye.y, 0, 0, 
                renderSizePerEye.x, renderSizePerEye.y, GL_COLOR_BUFFER_BIT, GL_LINEAR); // GL_LINEAR for color

            gl_check_error(__FILE__, __LINE__);
        }

        forwardTimer.stop();

        // fixme - cache handles.. don't need to do this on every frame
        for (int eyeIndex = 0; eyeIndex < NumEyes; ++eyeIndex)
        {
            outputTextureHandles[eyeIndex] = eyeTextures[eyeIndex];
            outputDepthTextureHandles[eyeIndex] = eyeDepthTextures[eyeIndex];
        }

        // Execute the post passes after having resolved the multisample framebuffers
        {
            postTimer.start();
            for (int eyeIdx = 0; eyeIdx < NumEyes; ++eyeIdx)
            {
                run_post_pass(cameras[eyeIdx]);
            }
            postTimer.stop();
        }

        glDisable(GL_FRAMEBUFFER_SRGB);

        renderSet.clear();
        pointLights.clear();

        // Compute frame GPU performance timing info
        {
            const float shadowMs = shadowTimer.elapsed_ms();
            const float earlyZMs = earlyZTimer.elapsed_ms();
            const float forwardMs = forwardTimer.elapsed_ms();
            const float postMs = postTimer.elapsed_ms();
            earlyZAverage.put(earlyZMs);
            forwardAverage.put(forwardMs);
            shadowAverage.put(shadowMs);
            postAverage.put(postMs);
            frameAverage.put(earlyZMs + shadowMs + forwardMs + postMs);
        }

        gl_check_error(__FILE__, __LINE__);
    }

    void add_camera(const CameraData & data)
    {
        assert(data.index <= NumEyes);
        cameras[data.index] = data;
    }

    GLuint get_output_texture(const uint32_t idx) const
    { 
        assert(idx <= NumEyes);
        return outputTextureHandles[idx]; 
    }

    GLuint get_output_texture_depth(const uint32_t idx) const
    {
        assert(idx <= NumEyes);
        return outputDepthTextureHandles[idx];
    }

    void set_procedural_sky(ProceduralSky * sky)
    {
        skybox = sky;

        sunlight.direction = sky->get_sun_direction();
        sunlight.color = float3(1.f, 1.0f, 1.0f);
        sunlight.amount = 1.0f;
    }
   
    uniforms::directional_light get_sunlight() const
    {
        return sunlight;
    }

    void set_sunlight(uniforms::directional_light sun)
    {
        sunlight = sun;
    }

    ProceduralSky * get_procedural_sky() const
    {
        if (skybox) return skybox;
        else return nullptr;
    }

    StableCascadedShadowPass * get_shadow_pass() const { if (shadow) return shadow.get(); }
    BloomPass * get_bloom_pass() const { if (bloom) return bloom.get(); }

    void add_objects(const std::vector<Renderable *> & set) 
    { 
        renderSet = set; 
    }

    void add_light(uniforms::point_light * light) 
    { 
        pointLights.push_back(light);
    }

    CircularBuffer<float> earlyZAverage = { 3 };
    CircularBuffer<float> forwardAverage = { 3 };
    CircularBuffer<float> shadowAverage = { 3 };
    CircularBuffer<float> postAverage = { 3 };
    CircularBuffer<float> frameAverage = { 3 };
};

#endif // end vr_renderer_hpp
  