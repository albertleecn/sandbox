#pragma once

#ifndef scene_hpp
#define scene_hpp

#include "GL_API.hpp"
#include "geometry.hpp"
#include "camera.hpp"

namespace avl
{

struct ViewportRaycast
{
    GlCamera & cam; float2 viewport;
    ViewportRaycast(GlCamera & camera, float2 viewport) : cam(camera), viewport(viewport) {}
    Ray from(float2 cursor) { return cam.get_world_ray(cursor, viewport); };
};

struct RaycastResult
{
    bool hit = false;
    float distance = std::numeric_limits<float>::max();
    float3 normal = {0, 0, 0};
    RaycastResult(bool h, float t, float3 n) : hit(h), distance(t), normal(n) {}
};
    
struct Object
{
    Pose pose;
    float3 scale;
    Object() : scale(1, 1, 1) {}
    float4x4 get_model() const { return mul(pose.matrix(), make_scaling_matrix(scale)); }
    Bounds3D bounds;
};

struct Renderable : public Object
{
    GlMesh mesh;
    Geometry geom;
    bool castsShadow;
    bool isEmissive = false;
        
    Renderable() {}

    Renderable(const Geometry & g, bool shadow = true, GLenum renderMode = GL_TRIANGLE_STRIP) : geom(g), castsShadow(shadow)
    {
        rebuild_mesh();
		if (renderMode != GL_TRIANGLE_STRIP)
		{
			mesh.set_non_indexed(renderMode);
			glPointSize(5);
		}
    }
        
    void rebuild_mesh() 
	{ 
		bounds = geom.compute_bounds(); 
		mesh = make_mesh_from_geometry(geom); 
	}
        
    void draw() const 
	{ 
		mesh.draw_elements(); 
	};
        
    RaycastResult check_hit(const Ray & worldRay) const
    {
        auto localRay = pose.inverse() * worldRay;
        localRay.origin /= scale;
        localRay.direction /= scale;
        float outT = 0.0f;
        float3 outNormal = {0, 0, 0};
        bool hit = intersect_ray_mesh(localRay, geom, &outT, &outNormal);
        return {hit, outT, outNormal};
    }
};

} // end namespace avl

#endif