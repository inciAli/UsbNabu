#pragma once

#include <stddef.h>

namespace srcguard::upc {

constexpr unsigned kIoctlAcpiEvalMethod = 0x0032C004u;
constexpr unsigned kInputSignature = 0x42696541u;
constexpr unsigned kOutputSignature = 0x426F6541u;
constexpr unsigned kMethodUpc = 0x4350555Fu;
constexpr unsigned short kArgumentInteger = 0;
constexpr unsigned kConnectorTypeC = 0x09;
constexpr size_t kReplyBytes = 12 + 4 * 8;

inline unsigned Le32(const unsigned char* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<unsigned>(p[3]) << 24);
}
inline void Put16(unsigned char* p, unsigned v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
inline void Put32(unsigned char* p, unsigned v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

inline bool IsUpcRequest(const void* input, size_t length) {
    if (!input || length < 8) return false;
    const auto* p = static_cast<const unsigned char*>(input);
    return Le32(p) == kInputSignature && Le32(p + 4) == kMethodUpc;
}

inline size_t BuildUpcReply(void* output, size_t length, unsigned connectorType = kConnectorTypeC) {
    if (!output || length < kReplyBytes) return 0;
    auto* p = static_cast<unsigned char*>(output);
    Put32(p, kOutputSignature);
    Put32(p + 4, static_cast<unsigned>(kReplyBytes));
    Put32(p + 8, 4);
    const unsigned values[4] = {1, connectorType, 0, 0};
    for (int i = 0; i < 4; ++i) {
        unsigned char* a = p + 12 + 8 * i;
        Put16(a, kArgumentInteger);
        Put16(a + 2, 4);
        Put32(a + 4, values[i]);
    }
    return kReplyBytes;
}

}
