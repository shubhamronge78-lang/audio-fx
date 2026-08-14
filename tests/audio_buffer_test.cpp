#include <cassert>

#include "audio_buffer.h"

using audiofx::core::AudioBuffer;

int main()
{
    AudioBuffer buffer(4);

    assert(buffer.size() == 4);

    buffer[0] = 1.0f;
    buffer[1] = 2.0f;
    buffer[2] = 3.0f;
    buffer[3] = 4.0f;

    assert(buffer[0] == 1.0f);
    assert(buffer[3] == 4.0f);

    buffer.clear();

    assert(buffer[0] == 0.0f);
    assert(buffer[3] == 0.0f);

    buffer.resize(8);
    assert(buffer.size() == 8);

    return 0;
}
