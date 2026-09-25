// Empty cart dispatch table for public/bootstrap builds.
#include <cstdint>
struct DispatchEntry {
    std::uint32_t addr;
    std::uint8_t thumb;
    std::uint8_t resume;
    void (*fn)(void);
};
extern "C" {
extern const DispatchEntry kDispatchTable[] = {
    {0u, 0u, 0u, nullptr},
};
extern const unsigned kDispatchTableLen = 0u;
}
