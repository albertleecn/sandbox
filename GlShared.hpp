#ifndef gl_common_h
#define gl_common_h

#include <vector>
#include <type_traits>

#if defined(ANVIL_PLATFORM_WINDOWS)
    #define GLEW_STATIC
    #include <GL/glew.h>
#elif defined(ANVIL_PLATFORM_OSX)
    #include <OpenGL/gl3.h>
#endif

namespace gfx
{
    
    template<typename T>
    inline GLenum to_gl(T *)
    {
        if (std::is_same<T, int8_t *>::value) return GL_UNSIGNED_BYTE;
        else if (std::is_same<T, uint16_t *>::value) return GL_UNSIGNED_SHORT;
        else if (std::is_same<T, uint32_t *>::value) return GL_UNSIGNED_INT;
        else if (std::is_same<T, float *>::value) return GL_FLOAT;
    };
    
    
    class Ray
    {
        math::float3 origin;
        math::float3 direction;
        char signX, signY, signZ;
        math::float3 invDirection;
    public:
        
        Ray() {}
        Ray(const math::float3 &aOrigin, const math::float3 &aDirection) : origin(aOrigin) { set_direction(aDirection); }
        
        void set_origin(const math::float3 &aOrigin) { origin = aOrigin; }
        const math::float3& get_origin() const { return origin; }
        
        void set_direction(const math::float3 &aDirection)
        {
            direction = aDirection;
            invDirection = math::float3(1.0f / direction.x, 1.0f / direction.y, 1.0f / direction.z);
            signX = (direction.x < 0.0f) ? 1 : 0;
            signY = (direction.y < 0.0f) ? 1 : 0;
            signZ = (direction.z < 0.0f) ? 1 : 0;
        }

        const math::float3 & get_direction() const { return direction; }
        const math::float3 & get_inv_direction() const { return invDirection; }
        
        char getSignX() const { return signX; }
        char getSignY() const { return signY; }
        char getSignZ() const { return signZ; }
        
        void transform(const math::float4x4 & matrix)
        {
            origin = transform_vector(matrix, origin);
            set_direction(math::get_rotation_submatrix(matrix) * direction);
        }
        
        Ray transformed(const math::float4x4 & matrix) const
        {
            Ray result;
            result.origin = transform_vector(matrix, origin);
            result.set_direction(math::get_rotation_submatrix(matrix) * direction);
            return result;
        }
        
        math::float3 calculate_position(float t) const { return origin + direction * t; }
    };
    
    // Can be used for things like a vbo, ibo, or pbo
    class GlBuffer : public util::Noncopyable
    {
        GLuint buffer;
        GLsizeiptr bufferLen;
    public:
        
        enum class Type : int
        {
            Vertex,
            Index,
            Pixel,
            Uniform
        };
        
        enum class Usage : int
        {
            Static,
            Dynamic
        };
        
        GlBuffer() : buffer() {}
        GlBuffer(GlBuffer && r) : GlBuffer() { *this = std::move(r); }
        
        ~GlBuffer() { if (buffer) glDeleteBuffers(1, &buffer); }
        
        GLuint gl_handle() const { return buffer; }
        GLsizeiptr size() const { return bufferLen; }
        
        void bind(GLenum target) const { glBindBuffer(target, buffer); }
        void unbind(GLenum target) const { glBindBuffer(target, 0); }
        
        GlBuffer & operator = (GlBuffer && r) { std::swap(buffer, r.buffer); std::swap(bufferLen, r.bufferLen); return *this; }
        
        void set_buffer_data(GLenum target, GLsizeiptr length, const GLvoid * data, GLenum usage)
        {
            if (!buffer) glGenBuffers(1, &buffer);
            glBindBuffer(target, buffer);
            glBufferData(target, length, data, usage);
            glBindBuffer(target, 0);
            this->bufferLen = length;
        }
        
        void set_buffer_data(GLenum target, const std::vector<GLubyte> & bytes, GLenum usage)
        {
            set_buffer_data(target, bytes.size(), bytes.data(), usage);
        }
    };
    
    struct GlCamera
    {
        math::Pose pose;
        
        float fov = 45.0f;
        float nearClip = 0.1f;
        float farClip = 128.0f;
        
        math::Pose get_pose() const { return pose; }
        
        math::float3 get_view_direction() const { return -pose.zdir(); }
        
        math::float3 get_eye_point() const { return pose.position; }
        
        math::float4x4 get_view_matrix() const { return math::make_view_matrix_from_pose(pose); }
        
        math::float4x4 get_projection_matrix(float aspectRatio) const
        {
            const float top = nearClip * std::tan((fov * (ANVIL_PI / 2) / 360) / 2);
            const float right = top * aspectRatio;
            const float bottom = -top;
            const float left = -right;
            return math::make_projection_matrix_from_frustrum_rh_gl(left, right, bottom, top, nearClip, farClip);
        }
        
        math::float4x4 get_projection_matrix(float l, float r, float b, float t) const
        {
            float left = -tanf(math::to_radians(l)) * nearClip;
            float right = tanf(math::to_radians(r)) * nearClip;
            float bottom = -tanf(math::to_radians(b)) * nearClip;
            float top = tanf(math::to_radians(t)) * nearClip;
            return math::make_projection_matrix_from_frustrum_rh_gl(left, right, bottom, top, nearClip, farClip);
        }
        
        void set_orientation(math::float4 o) { pose.orientation = math::normalize(o); }
        
        void set_position(math::float3 p) { pose.position = p; }
        
        void set_perspective(float vFov, float nearClip, float farClip)
        {
            this->fov = vFov;
            this->nearClip = nearClip;
            this->farClip = farClip;
        }
        
        void look_at(math::float3 target) { look_at(pose.position, target); }
        
        void look_at(math::float3 eyePoint, math::float3 target)
        {
            const math::float3 worldUp = {0,1,0};
            pose.position = eyePoint;
            math::float3 zDir = math::normalize(eyePoint - target);
            math::float3 xDir = math::normalize(cross(worldUp, zDir));
            math::float3 yDir = math::cross(zDir, xDir);
            pose.orientation = math::normalize(math::make_rotation_quat_from_rotation_matrix({xDir, yDir, zDir}));
        }
        
        float get_focal_length() const
        {
            return (1.f / (tan(math::to_radians(fov) * 0.5f) * 2.0f));
        }

    };
    
    inline Ray make_ray(const GlCamera & camera, const float aspectRatio, float uPos, float vPos, float imagePlaneApectRatio)
    {
        const float top = camera.nearClip * std::tan((camera.fov * (ANVIL_PI / 2) / 360) / 2);
        const float right = top * aspectRatio; // Is this correct?
        const float left = -right;
        float s = (uPos - 0.5f) * imagePlaneApectRatio;
        float t = (vPos - 0.5f);
        float viewDistance = imagePlaneApectRatio / std::abs(right - left) * camera.nearClip;
        return Ray(camera.get_eye_point(), normalize(camera.pose.xdir() * s + camera.pose.ydir() * t - (camera.get_view_direction() * viewDistance)));
    }
    
    inline Ray make_ray(const GlCamera & camera, const float aspectRatio, const math::float2 & posPixels, const math::float2 & imageSizePixels)
    {
        return make_ray(camera, aspectRatio, posPixels.x / imageSizePixels.x, (imageSizePixels.y - posPixels.y) / imageSizePixels.y, imageSizePixels.x / imageSizePixels.y);
    }
    
    struct GlFramebuffer
    {
        
    };

    inline void gl_check_error(const char * file, int32_t line)
    {
        GLint error = glGetError();
        if (error)
        {
            const char * errorStr = 0;
            switch (error)
            {
                case GL_INVALID_ENUM: errorStr = "GL_INVALID_ENUM"; break;
                case GL_INVALID_VALUE: errorStr = "GL_INVALID_VALUE"; break;
                case GL_INVALID_OPERATION: errorStr = "GL_INVALID_OPERATION"; break;
                case GL_OUT_OF_MEMORY: errorStr = "GL_OUT_OF_MEMORY"; break;
                default: errorStr = "unknown error"; break;
            }
            printf("GL error : %s, line %d : %s\n", file, line, errorStr);
            error = 0;
        }
    }
}

#endif // end gl_common_h