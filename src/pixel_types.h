#pragma once

// The host passes these POD values to CUDA by value. All strides are in pixels.
struct SourcePair {
    unsigned long long current, previous, flow;
    unsigned width, height, currentPitch, previousPitch, flowPitch;
    unsigned long long currentTexture{}, previousTexture{}, flowTexture{};
};
struct OutputImage {
    unsigned long long pixels;
    unsigned width, height;
};
static_assert(sizeof(SourcePair) == 72 && sizeof(OutputImage) == 16, "FG kernel ABI");

// Explicit kernel ABI: four sine/cosine pairs and focal lengths, flags.
struct CameraRotation { float sy{},cy{1},sp{},cp{1}; };
struct InputPose {
    CameraRotation latest{},historical{};
    float fx{},fy{};
    unsigned active{},history{},hud{},gun{};
};
static_assert(sizeof(CameraRotation)==16 && sizeof(InputPose)==56,"FG input ABI");
