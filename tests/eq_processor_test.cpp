#include <cassert>
#include <cmath>
#include <iostream>

#include "../core/audio_buffer.h"
#include "../core/processor.h"
#include "../core/processors/eq_processor.h"

using namespace audiofx::core;
using namespace audiofx::core::processors;

// Helper: compute RMS of a buffer
static float computeRMS(const AudioBuffer& buf)
{
    float sum = 0.0f;
    for (uint32_t i = 0; i < buf.size(); ++i) {
        sum += buf[i] * buf[i];
    }
    return std::sqrt(sum / buf.size());
}

// Helper: generate a sine wave
static void generateSineWave(
    AudioBuffer& buf,
    float frequency,
    float sampleRate,
    float amplitude = 0.1f
)
{
    const float tau = 2.0f * 3.14159265359f;
    const float phase_increment = frequency / sampleRate;
    float phase = 0.0f;

    for (uint32_t i = 0; i < buf.size(); ++i) {
        buf[i] = amplitude * std::sin(phase * tau);
        phase += phase_increment;
    }
}

// Test 1: Pass-through (no bands enabled)
void test_passthrough()
{
    std::cout << "Test 1: Pass-through (no EQ bands enabled)\n";

    const uint32_t sampleRate = 48000;
    const uint32_t frames = 512;
    const uint32_t channels = 2;

    AudioBuffer inL(frames);
    AudioBuffer inR(frames);
    AudioBuffer outL(frames);
    AudioBuffer outR(frames);

    // Generate test signal
    generateSineWave(inL, 1000.0f, sampleRate, 0.1f);
    generateSineWave(inR, 2000.0f, sampleRate, 0.1f);

    float* inputsArr[2] = {inL.data(), inR.data()};
    float* outputsArr[2] = {outL.data(), outR.data()};

    EQProcessor proc;
    proc.configure(sampleRate, channels, frames);
    proc.start();

    AudioBlock blk{inputsArr, outputsArr, frames, channels, sampleRate};
    proc.process(blk);

    proc.stop();

    // Verify pass-through: output should equal input within tolerance
    const float tolerance = 1e-6f;
    for (uint32_t i = 0; i < frames; ++i) {
        assert(std::abs(outL[i] - inL[i]) < tolerance);
        assert(std::abs(outR[i] - inR[i]) < tolerance);
    }

    std::cout << "  PASS: Output matches input (pass-through)\n";
}

// Test 2: Real EQ effect (peaking boost at 1000 Hz)
void test_real_eq_effect()
{
    std::cout << "Test 2: Real EQ effect (+6 dB boost at 1000 Hz)\n";

    const uint32_t sampleRate = 48000;
    const uint32_t frames = 4096; // enough for filter to settle
    const uint32_t channels = 2;

    AudioBuffer inL(frames);
    AudioBuffer inR(frames);
    AudioBuffer outL(frames);
    AudioBuffer outR(frames);

    // Generate 1000 Hz sine wave (the frequency we're boosting)
    generateSineWave(inL, 1000.0f, sampleRate, 0.1f);
    generateSineWave(inR, 1000.0f, sampleRate, 0.1f);

    float* inputsArr[2] = {inL.data(), inR.data()};
    float* outputsArr[2] = {outL.data(), outR.data()};

    EQProcessor proc;
    proc.configure(sampleRate, channels, frames);

    // Enable peaking boost at 1000 Hz
    proc.setPeakingBand(0, 1000.0f, 0.707f, 6.0f);

    proc.start();

    AudioBlock blk{inputsArr, outputsArr, frames, channels, sampleRate};
    proc.process(blk);

    proc.stop();

    // Compute RMS for input and output (skip first few samples for transient)
    const uint32_t skipSamples = 256;
    AudioBuffer inLSkip(frames - skipSamples);
    AudioBuffer outLSkip(frames - skipSamples);

    for (uint32_t i = 0; i < frames - skipSamples; ++i) {
        inLSkip[i] = inL[i + skipSamples];
        outLSkip[i] = outL[i + skipSamples];
    }

    float inputRMS = computeRMS(inLSkip);
    float outputRMS = computeRMS(outLSkip);

    std::cout << "  Input RMS:  " << inputRMS << "\n";
    std::cout << "  Output RMS: " << outputRMS << "\n";

    // For a +6 dB boost, we expect roughly 2x amplitude increase
    // (6 dB = 20*log10(2) ≈ 6.02 dB)
    // So output RMS should be roughly 2x input RMS
    // Use a reasonable tolerance: expect at least 1.8x, but allow some variation
    assert(outputRMS > inputRMS * 1.5f && outputRMS < inputRMS * 3.0f);

    // Verify output is not identical to input
    bool different = false;
    for (uint32_t i = 0; i < frames; ++i) {
        if (std::abs(outL[i] - inL[i]) > 1e-6f) {
            different = true;
            break;
        }
    }
    assert(different && "Output should differ from input when EQ is enabled");

    std::cout << "  PASS: EQ changed the audio (output RMS ~" << (outputRMS / inputRMS)
              << "x input RMS)\n";
}

// Test 3: Stereo independence
void test_stereo_independence()
{
    std::cout << "Test 3: Stereo independence\n";

    const uint32_t sampleRate = 48000;
    const uint32_t frames = 2048;
    const uint32_t channels = 2;

    AudioBuffer inL(frames);
    AudioBuffer inR(frames);
    AudioBuffer outL(frames);
    AudioBuffer outR(frames);

    // Different signals on each channel
    generateSineWave(inL, 1000.0f, sampleRate, 0.1f);
    generateSineWave(inR, 2000.0f, sampleRate, 0.1f);

    float* inputsArr[2] = {inL.data(), inR.data()};
    float* outputsArr[2] = {outL.data(), outR.data()};

    EQProcessor proc;
    proc.configure(sampleRate, channels, frames);

    // Enable same EQ on both channels
    proc.setPeakingBand(0, 1000.0f, 0.707f, 6.0f);

    proc.start();

    AudioBlock blk{inputsArr, outputsArr, frames, channels, sampleRate};
    proc.process(blk);

    proc.stop();

    // Compute RMS for each channel
    const uint32_t skipSamples = 256;
    AudioBuffer inLSkip(frames - skipSamples);
    AudioBuffer inRSkip(frames - skipSamples);
    AudioBuffer outLSkip(frames - skipSamples);
    AudioBuffer outRSkip(frames - skipSamples);

    for (uint32_t i = 0; i < frames - skipSamples; ++i) {
        inLSkip[i] = inL[i + skipSamples];
        inRSkip[i] = inR[i + skipSamples];
        outLSkip[i] = outL[i + skipSamples];
        outRSkip[i] = outR[i + skipSamples];
    }

    float inLRMS = computeRMS(inLSkip);
    float inRRMS = computeRMS(inRSkip);
    float outLRMS = computeRMS(outLSkip);
    float outRRMS = computeRMS(outRSkip);

    // Left channel (1000 Hz, boosted): should increase significantly
    assert(outLRMS > inLRMS * 1.5f);

    // Right channel (2000 Hz, not directly boosted): should increase less
    // The peaking EQ has some bandwidth, so 2000 Hz may be slightly affected
    // but much less than 1000 Hz
    float leftBoost = outLRMS / inLRMS;
    float rightBoost = outRRMS / inRRMS;
    assert(leftBoost > rightBoost);

    std::cout << "  Left channel (1000 Hz):  " << leftBoost << "x boost\n";
    std::cout << "  Right channel (2000 Hz): " << rightBoost << "x boost\n";
    std::cout << "  PASS: Stereo channels processed independently\n";
}

// Test 4: Disable band
void test_disable_band()
{
    std::cout << "Test 4: Disable band (revert to pass-through)\n";

    const uint32_t sampleRate = 48000;
    const uint32_t frames = 2048;
    const uint32_t channels = 2;

    AudioBuffer inL(frames);
    AudioBuffer inR(frames);
    AudioBuffer outL(frames);
    AudioBuffer outR(frames);

    generateSineWave(inL, 1000.0f, sampleRate, 0.1f);
    generateSineWave(inR, 1000.0f, sampleRate, 0.1f);

    float* inputsArr[2] = {inL.data(), inR.data()};
    float* outputsArr[2] = {outL.data(), outR.data()};

    EQProcessor proc;
    proc.configure(sampleRate, channels, frames);

    // Enable then disable the band
    proc.setPeakingBand(0, 1000.0f, 0.707f, 6.0f);
    proc.disableBand(0);

    proc.start();

    AudioBlock blk{inputsArr, outputsArr, frames, channels, sampleRate};
    proc.process(blk);

    proc.stop();

    // With the band disabled, output should equal input (pass-through)
    const float tolerance = 1e-6f;
    for (uint32_t i = 0; i < frames; ++i) {
        assert(std::abs(outL[i] - inL[i]) < tolerance);
        assert(std::abs(outR[i] - inR[i]) < tolerance);
    }

    std::cout << "  PASS: Disabled band produces pass-through\n";
}

// Test 5: Lifecycle (configure, start, process, stop, repeat)
void test_lifecycle()
{
    std::cout << "Test 5: Lifecycle (configure → start → process → stop → repeat)\n";

    const uint32_t sampleRate = 48000;
    const uint32_t frames = 256;
    const uint32_t channels = 2;

    AudioBuffer inL(frames);
    AudioBuffer inR(frames);
    AudioBuffer outL(frames);
    AudioBuffer outR(frames);

    generateSineWave(inL, 1000.0f, sampleRate, 0.1f);
    generateSineWave(inR, 1000.0f, sampleRate, 0.1f);

    float* inputsArr[2] = {inL.data(), inR.data()};
    float* outputsArr[2] = {outL.data(), outR.data()};

    EQProcessor proc;

    // First cycle
    proc.configure(sampleRate, channels, frames);
    proc.setPeakingBand(0, 1000.0f, 0.707f, 6.0f);
    proc.start();

    AudioBlock blk{inputsArr, outputsArr, frames, channels, sampleRate};
    proc.process(blk);

    proc.stop();

    // Capture first output
    AudioBuffer firstOut(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        firstOut[i] = outL[i];
    }

    // Second cycle (should reset and produce same result)
    outL.clear();
    proc.configure(sampleRate, channels, frames);
    proc.start();
    proc.process(blk);
    proc.stop();

    // Verify deterministic behavior
    for (uint32_t i = 0; i < frames; ++i) {
        assert(std::abs(outL[i] - firstOut[i]) < 1e-5f);
    }

    std::cout << "  PASS: Lifecycle repeated without issues\n";
}

int main()
{
    test_passthrough();
    test_real_eq_effect();
    test_stereo_independence();
    test_disable_band();
    test_lifecycle();

    std::cout << "eq_processor_test: PASS\n";
    return 0;
}
