#include <brainview/OrbitCamera.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace brainview
{
    namespace
    {
        myengine::render::Matrix4 ToMatrix(const DirectX::XMMATRIX& matrix)
        {
            DirectX::XMFLOAT4X4 value{};
            DirectX::XMStoreFloat4x4(&value, matrix);
            myengine::render::Matrix4 result{};
            std::memcpy(result.data.data(), &value, sizeof(float) * 16);
            return result;
        }
    }

    void OrbitCamera::Advance(const double deltaSeconds)
    {
        idleSeconds += static_cast<float>(deltaSeconds);
        if (autoRotate && idleSeconds > kResumeAfterSeconds)
        {
            yaw += autoRotateSpeed * static_cast<float>(deltaSeconds);
            if (yaw > 6.2831853f)
            {
                yaw -= 6.2831853f;
            }
        }
    }

    void OrbitCamera::Orbit(const float dx, const float dy)
    {
        idleSeconds = 0.0f;
        yaw -= dx * 0.0085f;
        pitch = std::clamp(pitch + dy * 0.0085f, -1.45f, 1.45f);
    }

    void OrbitCamera::Zoom(const float steps)
    {
        idleSeconds = 0.0f;
        distance = std::clamp(distance * std::pow(0.88f, steps), kMinDistance, kMaxDistance);
    }

    void OrbitCamera::Matrices(const float aspect, myengine::render::Matrix4& view, myengine::render::Matrix4& projection) const
    {
        const float cp = std::cos(pitch);
        const DirectX::XMVECTOR target = DirectX::XMVectorSet(0.0f, centerY, 0.0f, 1.0f);
        const DirectX::XMVECTOR eye = DirectX::XMVectorSet(distance * cp * std::sin(yaw), centerY + distance * std::sin(pitch), distance * cp * std::cos(yaw), 1.0f);
        view = ToMatrix(DirectX::XMMatrixLookAtLH(eye, target, DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
        projection = ToMatrix(DirectX::XMMatrixPerspectiveFovLH(fovY, std::max(aspect, 0.1f), 0.05f, 40.0f));
    }
}
