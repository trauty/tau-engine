#include "texture_cooker.h"

#include "tau/asset_sources.h"
#include "tau/assets/texture.h"
#include "tau/defines.h"
#include "tau/log.h"
#include "tau/rhi/vulkan.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ktx.h>
#include <stb/stb_image.h>
#include <stb/stb_image_resize2.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tau::cooker::texture
{
    namespace
    {
        std::string to_lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        texture_kind_e kind_from_name(const std::string& path)
        {
            const std::string lower = to_lower(std::filesystem::path(path).filename().string());
            if (lower.find("_normal") != std::string::npos || lower.find("_n.") != std::string::npos)
            {
                return texture_kind_e::NORMAL;
            }
            if (lower.find("_orm") != std::string::npos || lower.find("_mask") != std::string::npos ||
                lower.find("_roughness") != std::string::npos || lower.find("_metallic") != std::string::npos)
            {
                return texture_kind_e::DATA;
            }
            return texture_kind_e::COLOR;
        }

        const char* kind_key(texture_kind_e kind)
        {
            switch (kind)
            {
            case texture_kind_e::COLOR: return "color";
            case texture_kind_e::NORMAL: return "normal";
            case texture_kind_e::DATA: return "data";
            }
            return "color";
        }

        struct image_t
        {
            u32_t width = 0;
            u32_t height = 0;
            std::vector<u8_t> rgba;
        };

        // box filter from the previous level, sRGB in linear light, alpha unweighted
        image_t half_size(const image_t& from, bool srgb)
        {
            image_t to{std::max(1u, from.width / 2), std::max(1u, from.height / 2), {}};
            to.rgba.resize(static_cast<std::size_t>(to.width) * to.height * 4);
            stbir_resize(from.rgba.data(), static_cast<i32>(from.width), static_cast<i32>(from.height), 0,
                         to.rgba.data(), static_cast<i32>(to.width), static_cast<i32>(to.height), 0, STBIR_RGBA_NO_AW,
                         srgb ? STBIR_TYPE_UINT8_SRGB : STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP, STBIR_FILTER_BOX);
            return to;
        }

        void add_kv(ktxTexture2* tex, const char* key, const std::string& value)
        { ktxHashList_AddKVPair(&tex->kvDataHead, key, static_cast<u32_t>(value.length()) + 1, value.c_str()); }

        constexpr u32_t ZSTD_LEVEL = 18;

        bool deflate(ktxTexture2* tex, const std::string& owner)
        {
            if (tex->supercompressionScheme != KTX_SS_NONE) { return true; }
            if (const ktx_error_code_e res = ktxTexture2_DeflateZstd(tex, ZSTD_LEVEL); res != KTX_SUCCESS)
            {
                TAU_LOG_ERROR("TEXTURE_COOKER", "Failed to zstd '{}': {}", owner, ktxErrorString(res));
                return false;
            }
            return true;
        }

        bool encode(ktxTexture2* tex, const texture_import_t& settings, const std::string& owner)
        {
            if (settings.compression == texture_compression_e::NONE) { return deflate(tex, owner); }

            ktxBasisParams params = {};
            params.structSize = sizeof(params);
            params.threadCount = std::max(1u, std::thread::hardware_concurrency());
            params.normalMap = settings.kind == texture_kind_e::NORMAL ? KTX_TRUE : KTX_FALSE;

            switch (settings.compression)
            {
            case texture_compression_e::ETC1S:
                params.uastc = KTX_FALSE;
                params.compressionLevel = KTX_ETC1S_DEFAULT_COMPRESSION_LEVEL;
                break;
            case texture_compression_e::UASTC:
            case texture_compression_e::NONE:
                params.uastc = KTX_TRUE;
                params.uastcFlags = KTX_PACK_UASTC_LEVEL_DEFAULT;
                break;
            }

            if (const ktx_error_code_e res = ktxTexture2_CompressBasisEx(tex, &params); res != KTX_SUCCESS)
            {
                TAU_LOG_ERROR("TEXTURE_COOKER", "Failed to compress '{}': {}", owner, ktxErrorString(res));
                return false;
            }
            return settings.compression == texture_compression_e::ETC1S || deflate(tex, owner);
        }

        bool save(ktxTexture2* tex, const std::string& output_path, const std::string& owner)
        {
            std::filesystem::create_directories(std::filesystem::path(output_path).parent_path());
            const bool ok = ktxTexture_WriteToNamedFile(ktxTexture(tex), output_path.c_str()) == KTX_SUCCESS;
            if (!ok) { TAU_LOG_ERROR("TEXTURE_COOKER", "Failed to save '{}' to '{}'", owner, output_path); }
            return ok;
        }

        // already Basis (KHR_texture_basisu) or GPU-ready
        bool pass_through(std::span<const u8_t> bytes, const std::string& output_path, const texture_import_t& settings,
                          const std::string& owner)
        {
            ktxTexture2* tex = nullptr;
            if (ktxTexture2_CreateFromMemory(bytes.data(), bytes.size(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                             &tex) != KTX_SUCCESS)
            {
                TAU_LOG_ERROR("TEXTURE_COOKER", "Not a KTX2 texture: {}", owner);
                return false;
            }

            ktxHashList_DeleteKVPair(&tex->kvDataHead, "tau_tex_type");
            add_kv(tex, "tau_tex_type", kind_key(settings.kind));

            const bool ok = deflate(tex, owner) && save(tex, output_path, owner);
            ktxTexture_Destroy(ktxTexture(tex));
            return ok;
        }
    } // namespace

    nlohmann::json prepare_meta(const std::string& input_path, const nlohmann::json& project_defaults)
    {
        nlohmann::json meta = asset_sources::read_meta(input_path);
        if (meta.contains(import_settings::META_KEY)) { return meta; }

        const u64_t type_id = get_type_id<texture_t>();
        const reflection::any_t base = import_settings::base(type_id, project_defaults);
        reflection::any_t value = base;
        texture_import_t& settings = value.cast<texture_import_t&>();

        const bool migrating = meta.contains("type") || meta.contains("srgb");
        if (migrating)
        {
            const std::string type = meta.value("type", std::string("color"));
            settings.kind = type == "normal" ? texture_kind_e::NORMAL
                            : type == "data" ? texture_kind_e::DATA
                                             : texture_kind_e::COLOR;
            settings.srgb = meta.value("srgb", settings.kind == texture_kind_e::COLOR);
            meta.erase("type");
            meta.erase("srgb");
        }
        else
        {
            settings.kind = kind_from_name(input_path);
            if (settings.kind != texture_kind_e::COLOR) { settings.srgb = false; }
        }

        meta[import_settings::META_KEY] = import_settings::meta_object(value, base);
        asset_sources::write_meta(input_path, meta);
        TAU_LOG_INFO("TEXTURE_COOKER", "{} import settings of {}", migrating ? "Migrated the" : "Wrote first",
                     input_path);
        return meta;
    }

    bool cook_texture(const std::string& input_path, const std::string& output_path, const texture_import_t& settings)
    {
        std::ifstream file(input_path, std::ios::binary);
        const std::vector<u8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.empty())
        {
            TAU_LOG_ERROR("TEXTURE_COOKER", "Failed to read texture: {}", input_path);
            return false;
        }

        const std::string extension = to_lower(std::filesystem::path(input_path).extension().string());
        return cook_texture_bytes(bytes, extension, output_path, settings, input_path);
    }

    bool cook_texture_bytes(std::span<const u8_t> bytes, const std::string& extension, const std::string& output_path,
                            const texture_import_t& settings, const std::string& owner)
    {
        const std::string& input_path = owner;
        TAU_LOG_INFO("TEXTURE_COOKER", "Cooking texture: {} -> {}", input_path, output_path);

        if (extension == ".ktx2") { return pass_through(bytes, output_path, settings, owner); }

        const bool srgb = settings.kind == texture_kind_e::COLOR && settings.srgb;

        i32 width = 0;
        i32 height = 0;
        i32 channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<i32>(bytes.size()), &width, &height,
                                                &channels, STBI_rgb_alpha);
        if (!pixels)
        {
            TAU_LOG_ERROR("TEXTURE_COOKER", "Failed to decode texture: {}", input_path);
            return false;
        }

        image_t top{static_cast<u32_t>(width), static_cast<u32_t>(height), {}};
        top.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
        stbi_image_free(pixels);

        if (settings.max_size > 0)
        {
            while (std::max(top.width, top.height) > settings.max_size && std::max(top.width, top.height) > 1)
            {
                top = half_size(top, srgb);
            }
        }

        const u32_t mip_count =
            settings.mips ? static_cast<u32_t>(std::floor(std::log2(std::max(top.width, top.height)))) + 1 : 1;

        const ktxTextureCreateInfo create_info = {
            .vkFormat = static_cast<u32_t>(srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM),
            .baseWidth = top.width,
            .baseHeight = top.height,
            .baseDepth = 1,
            .numDimensions = 2,
            .numLevels = mip_count,
            .numLayers = 1,
            .numFaces = 1,
            .isArray = KTX_FALSE,
            .generateMipmaps = KTX_FALSE,
        };

        ktxTexture2* tex = nullptr;
        if (ktxTexture2_Create(&create_info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex) != KTX_SUCCESS)
        {
            TAU_LOG_ERROR("TEXTURE_COOKER", "Failed to create ktx2 texture: {}", input_path);
            return false;
        }

        image_t level = std::move(top);
        for (u32_t i = 0; i < mip_count; i++)
        {
            if (i > 0) { level = half_size(level, srgb); }
            ktxTexture_SetImageFromMemory(ktxTexture(tex), i, 0, 0, level.rgba.data(), level.rgba.size());
        }

        add_kv(tex, "tau_tex_type", kind_key(settings.kind));
        add_kv(tex, "tau_tex_channels", std::to_string(channels));

        const bool ok = encode(tex, settings, input_path) && save(tex, output_path, input_path);
        ktxTexture_Destroy(ktxTexture(tex));
        return ok;
    }
} // namespace tau::cooker::texture
