#include "mod_texture_bridge.h"

#include "mod_texture_codec.h"
#include "mod_texture_conversion.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <linux/memfd.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>

namespace {

constexpr size_t kDdsHeaderBytes = 128u;
constexpr uint32_t kMaxMipLevels = 16u;
constexpr size_t kMaxTokens = 65536u;

struct MipSource {
    std::string path;
    off_t offset = 0;
    size_t size = 0;
    mod_texture::DxtFormat format = mod_texture::DxtFormat::kNone;
    uint32_t width = 0;
    uint32_t height = 0;
    bool texture_has_alpha = false;
    uint32_t stored_format = 0;
    uint32_t checksum = 0;
};

std::mutex g_token_mutex;
std::unordered_map<uint32_t, MipSource> g_sources;
std::unordered_map<std::string, uint32_t> g_source_ids;
std::atomic<uint32_t> g_next_token{1u};

uint32_t Read32(const uint8_t* source) {
    return static_cast<uint32_t>(source[0]) |
           (static_cast<uint32_t>(source[1]) << 8u) |
           (static_cast<uint32_t>(source[2]) << 16u) |
           (static_cast<uint32_t>(source[3]) << 24u);
}

void Write32(uint8_t* destination, uint32_t value) {
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8u);
    destination[2] = static_cast<uint8_t>(value >> 16u);
    destination[3] = static_cast<uint8_t>(value >> 24u);
}

bool EndsWithDds(const char* path) {
    if (!path) return false;
    const size_t length = strnlen(path, PATH_MAX);
    if (length == PATH_MAX || length < 4u) return false;
    const char* suffix = path + length - 4u;
    return suffix[0] == '.' &&
        (suffix[1] == 'd' || suffix[1] == 'D') &&
        (suffix[2] == 'd' || suffix[2] == 'D') &&
        (suffix[3] == 's' || suffix[3] == 'S');
}

mod_texture::DxtFormat FourCcFormat(const uint8_t* fourcc) {
    if (std::memcmp(fourcc, "DXT1", 4u) == 0) return mod_texture::DxtFormat::kDxt1;
    if (std::memcmp(fourcc, "DXT3", 4u) == 0) return mod_texture::DxtFormat::kDxt3;
    if (std::memcmp(fourcc, "DXT5", 4u) == 0) return mod_texture::DxtFormat::kDxt5;
    return mod_texture::DxtFormat::kNone;
}

mod_texture::DxtFormat HeaderFormat(const uint8_t* header) {
    constexpr uint32_t kDdpfFourCc = 0x4u;
    constexpr uint32_t kDdpfRgb = 0x40u;
    const uint32_t flags = Read32(header + 80u);
    if ((flags & kDdpfFourCc) != 0u) return FourCcFormat(header + 84u);
    if ((flags & kDdpfRgb) == 0u || Read32(header + 88u) != 32u ||
        Read32(header + 92u) != 0x00ff0000u ||
        Read32(header + 96u) != 0x0000ff00u ||
        Read32(header + 100u) != 0x000000ffu) {
        return mod_texture::DxtFormat::kNone;
    }
    const uint32_t alpha_mask = Read32(header + 104u);
    if (alpha_mask == 0xff000000u) return mod_texture::DxtFormat::kBgra8;
    if (alpha_mask == 0u) return mod_texture::DxtFormat::kBgrx8;
    return mod_texture::DxtFormat::kNone;
}

bool ReadExact(int fd, void* destination, size_t size, off_t offset) {
    auto* bytes = static_cast<uint8_t*>(destination);
    size_t done = 0;
    while (done < size) {
        const ssize_t count = pread(fd, bytes + done, size - done, offset + done);
        if (count <= 0) return false;
        done += static_cast<size_t>(count);
    }
    return true;
}

bool Dxt1MipUsesTransparency(int fd, off_t offset, size_t size, bool& transparent) {
    transparent = false;
    if (size == 0u || size % 8u != 0u) return false;
    constexpr size_t kChunkBytes = 64u * 1024u;
    std::vector<uint8_t> chunk(std::min(size, kChunkBytes));
    size_t consumed = 0u;
    while (consumed < size) {
        const size_t count = std::min(chunk.size(), size - consumed);
        if (!ReadExact(fd, chunk.data(), count, offset + static_cast<off_t>(consumed))) {
            return false;
        }
        for (size_t block = 0u; block < count; block += 8u) {
            const uint16_t color0 = static_cast<uint16_t>(chunk[block]) |
                static_cast<uint16_t>(static_cast<uint16_t>(chunk[block + 1u]) << 8u);
            const uint16_t color1 = static_cast<uint16_t>(chunk[block + 2u]) |
                static_cast<uint16_t>(static_cast<uint16_t>(chunk[block + 3u]) << 8u);
            if (color0 <= color1) {
                const uint32_t indices = Read32(chunk.data() + block + 4u);
                for (uint32_t pixel = 0u; pixel < 16u; ++pixel) {
                    if (((indices >> (pixel * 2u)) & 3u) == 3u) {
                        transparent = true;
                        return true;
                    }
                }
            }
        }
        consumed += count;
    }
    return true;
}

bool BgraMipUsesTransparency(int fd, off_t offset, size_t size, bool& transparent) {
    transparent = false;
    if (size == 0u || size % 4u != 0u) return false;
    constexpr size_t kChunkBytes = 64u * 1024u;
    std::vector<uint8_t> chunk(std::min(size, kChunkBytes));
    size_t consumed = 0u;
    while (consumed < size) {
        const size_t count = std::min(chunk.size(), size - consumed);
        if (!ReadExact(fd, chunk.data(), count, offset + static_cast<off_t>(consumed))) {
            return false;
        }
        for (size_t pixel = 3u; pixel < count; pixel += 4u) {
            if (chunk[pixel] != 255u) {
                transparent = true;
                return true;
            }
        }
        consumed += count;
    }
    return true;
}

bool WriteExact(int fd, const void* source, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(source);
    size_t done = 0;
    while (done < size) {
        const ssize_t count = write(fd, bytes + done, size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        done += static_cast<size_t>(count);
    }
    return true;
}

int MemoryFile(const std::vector<uint8_t>& bytes) {
    const long result = syscall(__NR_memfd_create, "warband-mod-dds", MFD_CLOEXEC);
    if (result < 0) return -1;
    const int fd = static_cast<int>(result);
    if (!WriteExact(fd, bytes.data(), bytes.size()) || lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

uint32_t RegisterSource(const MipSource& source) {
    const std::string key = source.path + "\n" + std::to_string(source.offset) + "\n" +
        std::to_string(source.size) + "\n" + std::to_string(source.width) + "x" +
        std::to_string(source.height) + "\n" +
        std::to_string(static_cast<unsigned>(source.format)) + "\n" +
        std::to_string(source.stored_format) + "\n" + std::to_string(source.checksum);
    std::lock_guard<std::mutex> lock(g_token_mutex);
    const auto old = g_source_ids.find(key);
    if (old != g_source_ids.end()) return old->second;
    if (g_sources.size() >= kMaxTokens) return 0;
    uint32_t id = g_next_token.fetch_add(1u, std::memory_order_relaxed);
    if (id == 0u) id = g_next_token.fetch_add(1u, std::memory_order_relaxed);
    g_sources.emplace(id, source);
    g_source_ids.emplace(key, id);
    return id;
}

size_t ConvertedVirtualSize(const mod_texture::ConvertedTexture& texture) {
    size_t size = kDdsHeaderBytes;
    for (const auto& mip : texture.mips)
        size += mod_texture::VirtualAstcBytes(texture.source_format, mip.width, mip.height);
    return size;
}

int OpenConverted(const mod_texture::ConvertedTexture& texture, off_t* out_size) {
    std::vector<uint8_t> synthetic(ConvertedVirtualSize(texture), 0);
    auto* header = synthetic.data();
    std::memcpy(header, "DDS ", 4);
    Write32(header + 4, 124);
    Write32(header + 8, 0x000a1007u);  // caps, dimensions, format, linear size, mips
    Write32(header + 12, texture.height);
    Write32(header + 16, texture.width);
    Write32(header + 20, static_cast<uint32_t>(mod_texture::VirtualAstcBytes(
        texture.source_format, texture.width, texture.height)));
    Write32(header + 28, static_cast<uint32_t>(texture.mips.size()));
    Write32(header + 76, 32);
    Write32(header + 80, 4);
    const char* fourcc = texture.source_format == mod_texture::DxtFormat::kDxt1
        ? "DXT1" : (texture.source_format == mod_texture::DxtFormat::kDxt5 ? "DXT5" : "DXT3");
    std::memcpy(header + 84, fourcc, 4);
    Write32(header + 108, texture.mips.size() > 1 ? 0x00401008u : 0x00001000u);
    size_t offset = kDdsHeaderBytes;
    for (const auto& mip : texture.mips) {
        MipSource source;
        source.path = texture.path;
        source.offset = static_cast<off_t>(mip.offset);
        source.size = static_cast<size_t>(mip.size);
        source.format = texture.source_format;
        source.width = mip.width;
        source.height = mip.height;
        source.texture_has_alpha = texture.has_alpha;
        source.stored_format = mip.format;
        source.checksum = mip.checksum;
        const uint32_t id = RegisterSource(source);
        if (id == 0) return -1;
        const auto token = mod_texture::MakeToken(id);
        std::memcpy(header + offset, &token, sizeof(token));
        // Only the token goes through the guest's block-row reversal. The
        // persisted bytes retain the original DDS pixel order for direct upload.
        const size_t last_row = mod_texture::VirtualAstcLastRowOffset(
            texture.source_format, mip.width, mip.height);
        if (last_row != 0) std::memcpy(header + offset + last_row, &token, sizeof(token));
        offset += mod_texture::VirtualAstcBytes(texture.source_format, mip.width, mip.height);
    }
    const int fd = MemoryFile(synthetic);
    if (fd >= 0 && out_size) *out_size = static_cast<off_t>(synthetic.size());
    return fd;
}

}  // namespace

int OpenModDdsVirtualized(const char* path, off_t* out_size, bool* handled) {
    if (handled) *handled = false;
    if (!EndsWithDds(path)) return -1;
    mod_texture::ConvertedTexture converted;
    bool converted_handled = false;
    if (mod_texture::ReadConvertedModTexture(path, converted, &converted_handled)) {
        if (handled) *handled = true;
        return OpenConverted(converted, out_size);
    }
    if (converted_handled) {
        if (handled) *handled = true;
        return -1;
    }
    int source_fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source_fd < 0) return -1;
    uint8_t header[kDdsHeaderBytes]{};
    if (!ReadExact(source_fd, header, sizeof(header), 0) ||
        std::memcmp(header, "DDS ", 4u) != 0 || Read32(header + 4u) != 124u ||
        Read32(header + 76u) != 32u) {
        close(source_fd);
        return -1;
    }
    const mod_texture::DxtFormat format = HeaderFormat(header);
    if (format == mod_texture::DxtFormat::kNone) {
        close(source_fd);
        return -1;
    }
    if (handled) *handled = true;

    const uint32_t height = Read32(header + 12u);
    const uint32_t width = Read32(header + 16u);
    uint32_t mip_count = Read32(header + 28u);
    if (mip_count == 0u) mip_count = 1u;
    const uint32_t caps2 = Read32(header + 112u);
    struct stat file_stat{};
    if (width == 0u || height == 0u || width > 16384u || height > 16384u ||
        mip_count > kMaxMipLevels || caps2 != 0u ||
        fstat(source_fd, &file_stat) != 0 || file_stat.st_size < 0) {
        close(source_fd);
        return -1;
    }

    size_t synthetic_size = kDdsHeaderBytes;
    off_t source_offset = static_cast<off_t>(kDdsHeaderBytes);
    uint32_t mip_width = width;
    uint32_t mip_height = height;
    struct PendingMip {
        MipSource source;
        size_t synthetic_offset;
        size_t synthetic_size;
        size_t last_row_offset;
    };
    std::vector<PendingMip> mips;
    mips.reserve(mip_count);
    bool texture_has_alpha = format == mod_texture::DxtFormat::kDxt3 ||
        format == mod_texture::DxtFormat::kDxt5;
    for (uint32_t mip = 0; mip < mip_count; ++mip) {
        const size_t source_bytes = mod_texture::DxtBytes(format, mip_width, mip_height);
        const size_t target_bytes =
            mod_texture::VirtualAstcBytes(format, mip_width, mip_height);
        const size_t last_row_offset =
            mod_texture::VirtualAstcLastRowOffset(format, mip_width, mip_height);
        if (source_bytes == 0u || target_bytes < sizeof(mod_texture::Token) ||
            last_row_offset > target_bytes - sizeof(mod_texture::Token) ||
            synthetic_size > SIZE_MAX - target_bytes ||
            source_offset > file_stat.st_size ||
            static_cast<uint64_t>(file_stat.st_size - source_offset) < source_bytes) {
            close(source_fd);
            return -1;
        }
        if (format == mod_texture::DxtFormat::kDxt1 && !texture_has_alpha) {
            bool mip_uses_transparency = false;
            if (!Dxt1MipUsesTransparency(source_fd, source_offset, source_bytes,
                                          mip_uses_transparency)) {
                close(source_fd);
                return -1;
            }
            texture_has_alpha = mip_uses_transparency;
        } else if (format == mod_texture::DxtFormat::kBgra8 &&
                   mip == 0u && !texture_has_alpha) {
            // Alpha in lower mips is derived from the top level by ordinary
            // DDS exporters. Scanning level zero avoids a second full-file read
            // while still choosing one consistent GLES format for every mip.
            bool mip_uses_transparency = false;
            if (!BgraMipUsesTransparency(source_fd, source_offset, source_bytes,
                                          mip_uses_transparency)) {
                close(source_fd);
                return -1;
            }
            texture_has_alpha = mip_uses_transparency;
        }
        mips.push_back(PendingMip{
            MipSource{path, source_offset, source_bytes, format, mip_width, mip_height,
                      false},
            synthetic_size, target_bytes, last_row_offset});
        synthetic_size += target_bytes;
        source_offset += static_cast<off_t>(source_bytes);
        mip_width = std::max(1u, mip_width / 2u);
        mip_height = std::max(1u, mip_height / 2u);
    }
    for (PendingMip& mip : mips) mip.source.texture_has_alpha = texture_has_alpha;
    close(source_fd);

    std::vector<uint8_t> synthetic(synthetic_size, 0u);
    std::memcpy(synthetic.data(), header, sizeof(header));
    if (format == mod_texture::DxtFormat::kBgra8 ||
        format == mod_texture::DxtFormat::kBgrx8) {
        // The Android guest maps desktop DXT3 metadata to its ASTC 4x4 upload
        // route. Pixel bytes stay as authenticated tokens and are decoded by
        // the host GLES thunk immediately before upload.
        Write32(synthetic.data() + 80u, 0x4u);
        std::memcpy(synthetic.data() + 84u, "DXT3", 4u);
        for (size_t field = 88u; field <= 104u; field += 4u) {
            Write32(synthetic.data() + field, 0u);
        }
    }
    Write32(synthetic.data() + 20u, static_cast<uint32_t>(mips.front().synthetic_size));
    for (const PendingMip& mip : mips) {
        const uint32_t id = RegisterSource(mip.source);
        if (id == 0u) return -1;
        const mod_texture::Token token = mod_texture::MakeToken(id);
        std::memcpy(synthetic.data() + mip.synthetic_offset, &token, sizeof(token));
        // The guest reverses compressed block rows before issuing the GLES
        // upload. Mirror the token into the last source row so it becomes the
        // first block after that transform. Keeping the first copy also makes
        // the virtual DDS valid on guest builds that do not flip DDS rows.
        if (mip.last_row_offset != 0u) {
            std::memcpy(synthetic.data() + mip.synthetic_offset + mip.last_row_offset,
                        &token, sizeof(token));
        }
    }
    const int virtual_fd = MemoryFile(synthetic);
    if (virtual_fd >= 0 && out_size) *out_size = static_cast<off_t>(synthetic.size());
    return virtual_fd;
}

bool StatModDdsVirtualized(const char* path, off_t* out_size, bool* handled) {
    if (handled) *handled = false;
    if (!EndsWithDds(path)) return false;
    mod_texture::ConvertedTexture converted;
    bool converted_handled = false;
    if (mod_texture::ReadConvertedModTexture(path, converted, &converted_handled)) {
        if (handled) *handled = true;
        if (out_size) *out_size = static_cast<off_t>(ConvertedVirtualSize(converted));
        return true;
    }
    if (converted_handled) {
        if (handled) *handled = true;
        return false;
    }
    const int source_fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source_fd < 0) return false;
    uint8_t header[kDdsHeaderBytes]{};
    const bool header_valid = ReadExact(source_fd, header, sizeof(header), 0) &&
        std::memcmp(header, "DDS ", 4u) == 0 && Read32(header + 4u) == 124u &&
        Read32(header + 76u) == 32u;
    if (!header_valid) {
        close(source_fd);
        return false;
    }
    const mod_texture::DxtFormat format = HeaderFormat(header);
    if (format == mod_texture::DxtFormat::kNone) {
        close(source_fd);
        return false;
    }
    if (handled) *handled = true;

    const uint32_t height = Read32(header + 12u);
    const uint32_t width = Read32(header + 16u);
    uint32_t mip_count = Read32(header + 28u);
    if (mip_count == 0u) mip_count = 1u;
    struct stat file_stat{};
    if (width == 0u || height == 0u || width > 16384u || height > 16384u ||
        mip_count > kMaxMipLevels || Read32(header + 112u) != 0u ||
        fstat(source_fd, &file_stat) != 0 || file_stat.st_size < 0) {
        close(source_fd);
        return false;
    }

    size_t synthetic_size = kDdsHeaderBytes;
    off_t source_offset = static_cast<off_t>(kDdsHeaderBytes);
    uint32_t mip_width = width;
    uint32_t mip_height = height;
    for (uint32_t mip = 0u; mip < mip_count; ++mip) {
        const size_t source_bytes = mod_texture::DxtBytes(format, mip_width, mip_height);
        const size_t target_bytes =
            mod_texture::VirtualAstcBytes(format, mip_width, mip_height);
        if (source_bytes == 0u || target_bytes < sizeof(mod_texture::Token) ||
            synthetic_size > SIZE_MAX - target_bytes ||
            source_offset > file_stat.st_size ||
            static_cast<uint64_t>(file_stat.st_size - source_offset) < source_bytes) {
            close(source_fd);
            return false;
        }
        synthetic_size += target_bytes;
        source_offset += static_cast<off_t>(source_bytes);
        mip_width = std::max(1u, mip_width / 2u);
        mip_height = std::max(1u, mip_height / 2u);
    }
    close(source_fd);
    if (out_size) *out_size = static_cast<off_t>(synthetic_size);
    return true;
}

bool DecodeModTextureToken(const void* data, size_t size, int width, int height,
                           std::vector<uint32_t>& rgba, bool& has_alpha) {
    uint32_t id = 0;
    if (!mod_texture::ReadToken(data, size, id) || width <= 0 || height <= 0) return false;
    MipSource source;
    {
        std::lock_guard<std::mutex> lock(g_token_mutex);
        const auto found = g_sources.find(id);
        if (found == g_sources.end()) return false;
        source = found->second;
    }
    if (source.width != static_cast<uint32_t>(width) ||
        source.height != static_cast<uint32_t>(height)) return false;
    if (source.stored_format != 0) return false;
    int fd = open(source.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    std::vector<uint8_t> compressed(source.size);
    const bool read = ReadExact(fd, compressed.data(), compressed.size(), source.offset);
    close(fd);
    if (!read) return false;
    std::vector<uint8_t> decoded;
    if (!mod_texture::DecodeDxt(source.format, compressed.data(), compressed.size(),
                                source.width, source.height, decoded, has_alpha)) {
        return false;
    }
    has_alpha = has_alpha || source.texture_has_alpha;
    rgba.resize(decoded.size() / sizeof(uint32_t));
    std::memcpy(rgba.data(), decoded.data(), decoded.size());
    return true;
}

int LoadConvertedModTextureToken(const void* data, size_t size, int width, int height,
                                 std::vector<uint8_t>& payload, uint32_t& format,
                                 bool& has_alpha) {
    uint32_t id = 0;
    if (!mod_texture::ReadToken(data, size, id)) return 0;
    MipSource source;
    {
        std::lock_guard<std::mutex> lock(g_token_mutex);
        const auto found = g_sources.find(id);
        if (found == g_sources.end()) return -1;
        source = found->second;
    }
    if (source.stored_format == 0) return 0;
    if (width <= 0 || height <= 0 || source.width != static_cast<uint32_t>(width) ||
        source.height != static_cast<uint32_t>(height)) return -1;
    const mod_texture::ConvertedMip mip{source.width, source.height, source.stored_format,
        source.checksum, static_cast<uint64_t>(source.offset), source.size};
    if (!mod_texture::ReadConvertedModMip(source.path, mip, payload)) return -1;
    format = source.stored_format;
    has_alpha = source.texture_has_alpha;
    return 1;
}
