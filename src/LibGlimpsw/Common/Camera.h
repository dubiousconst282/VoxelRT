#pragma once

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

namespace glim {  

struct Camera {
    enum InputMode { FirstPerson, Arcball };

    glm::dvec3 Position;
    glm::vec2 Euler;  // yaw, pitch
    float ArcDistance = 5.0f;

    InputMode Mode = FirstPerson;

    float FieldOfView = 90.0f;
    float AspectRatio = 1.0f;
    float MoveSpeed = 10.0f;
    float NearZ = 0.01f, FarZ = 1000.0f;

    // Smoothed values
    glm::dvec3 ViewPosition;
    glm::quat ViewRotation;

    glm::mat4 GetViewMatrix(bool translateToView = true) {
        if (Mode == InputMode::Arcball) {
            return glm::lookAt(ViewPosition, glm::dvec3(0, 0, 0), glm::dvec3(0, 1, 0));
        }
        glm::mat4 mat = glm::mat4_cast(ViewRotation);
        if (translateToView) mat = glm::translate(mat, glm::vec3(-ViewPosition));
        return mat;
    }
    glm::mat4 GetProjMatrix() { return glm::perspective(glm::radians(FieldOfView), AspectRatio, NearZ, FarZ); }

    void Update() {
        // TODO: decouple Camera from ImGui
        ImGuiIO& io = ImGui::GetIO();
        float sensitivity = 0.008f;
        float speed = io.DeltaTime * MoveSpeed;
        float pitchRange = glm::pi<float>() / 2.01f;         // a bit less than 90deg to prevent issues with LookAt()
        float blend = 1.0f - powf(0.7f, io.DeltaTime * 60);  // https://gamedev.stackexchange.com/a/149106
        glm::quat destRotation = glm::eulerAngleXY(-Euler.y, -Euler.x);

        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow)) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                float rx = io.MouseDelta.x * sensitivity;
                float ry = io.MouseDelta.y * sensitivity;

                Euler.x = NormalizeRadians(Euler.x - rx);
                Euler.y = glm::clamp(Euler.y - ry, -pitchRange, +pitchRange);

                destRotation = glm::eulerAngleXY(-Euler.y, -Euler.x);
            }

            if (Mode == InputMode::FirstPerson) {
                glm::vec3 mv(0.0f);

                if (ImGui::IsKeyDown(ImGuiKey_W)) mv.z--;
                if (ImGui::IsKeyDown(ImGuiKey_S)) mv.z++;
                if (ImGui::IsKeyDown(ImGuiKey_A)) mv.x--;
                if (ImGui::IsKeyDown(ImGuiKey_D)) mv.x++;
                if (ImGui::IsKeyDown(ImGuiKey_Space)) mv.y++;
                if (ImGui::IsKeyDown(ImGuiKey_LeftShift)) mv.y--;

                Position += mv * destRotation * speed;
            } else if (Mode == InputMode::Arcball) {
                if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
                    ArcDistance = glm::clamp(ArcDistance - io.MouseWheel * 0.5f, NearZ, FarZ * 0.8f);
                }
                Position = glm::vec3(0, 0, ArcDistance) * destRotation;
                // TODO: implement panning for arcball camera
            }
        }
        ViewRotation = glm::slerp(ViewRotation, destRotation, blend);
        ViewPosition = glm::lerp(ViewPosition, Position, (double)blend);

        AspectRatio = io.DisplaySize.x / io.DisplaySize.y;
    }

    static float NormalizeRadians(float ang) {
        const float tau = glm::two_pi<float>();
        float r = glm::round(ang * (1.0f / tau));
        return ang - (r * tau);
    }
};

};  // namespace glim
