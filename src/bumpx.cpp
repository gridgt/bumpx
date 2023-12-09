// iOrange, 2020
// takes normalmap + optional gloss and height maps and outputs bump and bump# textures for Stalker and Metro 2033 build 375 games
// Current version v0.7

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cmath>        // std::sqrt
#include <cstring>      // std::memcpy
#include <memory>       // std::unique_ptr

namespace fs = std::filesystem;

using BytesArray = std::vector<uint8_t>;


#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_GIF
#define STBI_NO_HDR
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#ifdef _WIN32
#define __STDC_LIB_EXT1__
#endif
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STBIR_DEFAULT_FILTER_DOWNSAMPLE  STBIR_FILTER_KAISER
#define STBIR_ASSERT(boolval)
#include "stb_image_resize.h"

#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"


#define SQUISH_USE_SSE 2
#include "squish/squish.h"
#include "squish/maths.cpp"
#include "squish/alpha.cpp"
#include "squish/clusterfit.cpp"
#include "squish/colourblock.cpp"
#include "squish/colourfit.cpp"
#include "squish/colourset.cpp"
#include "squish/rangefit.cpp"
#include "squish/singlecolourfit.cpp"
#include "squish/squish.cpp"


#define RGBCX_IMPLEMENTATION
#include "rgbcx.h"

// My own BCDEC library to decompress BC blocks :)
#define BCDEC_IMPLEMENTATION
#include "../bcdec/bcdec.h"

#ifdef _WIN32
using Char = wchar_t;
using String = std::wstring;
#define _T(str) L ## str
#define Cout std::wcout
#define Cerr std::wcerr
#define Main wmain
#define ENABLE_NVTT3 1
#else
using Char = char;
using String = std::string;
#define _T(str) str
#define Cout std::cout
#define Cerr std::cerr
#define Main main
#endif //  _WIN32

#ifdef ENABLE_NVTT3
#include "../nvtt3/nvtt.h"
#include "../nvtt3/nvtt_lowlevel.h"
#include "../nvtt3/nvtt_wrapper.h"
NvttCPUInputBuffer*(*func_nvttCreateCPUInputBuffer)(const NvttRefImage*, NvttValueType, int, int, int, float, float, float, float, NvttTimingContext*, unsigned*);
void(*func_nvttDestroyCPUInputBuffer)(NvttCPUInputBuffer*);
void(*func_nvttEncodeBC3CPU)(const NvttCPUInputBuffer*, NvttBoolean, void*, NvttBoolean, NvttBoolean, NvttTimingContext*);

extern "C" __declspec(dllimport) void* __stdcall LoadLibraryW(const wchar_t* lpLibFileName);
extern "C" __declspec(dllimport) void* __stdcall GetProcAddress(void* hModule, const char* lpProcName);
#endif // ENABLE_NVTT3

#ifdef __GNUC__
#define PACKED_STRUCT_BEGIN
#define PACKED_STRUCT_END __attribute__((__packed__))
#else
#define PACKED_STRUCT_BEGIN __pragma(pack(push, 1))
#define PACKED_STRUCT_END __pragma(pack(pop))
#endif


#define scast static_cast
#define rcast reinterpret_cast

static const size_t kMinMipSize = 4;    // 4 because the result is always BC compressed

#ifdef ENABLE_NVTT3
constexpr size_t kNumCompressors = 4;
#else
constexpr size_t kNumCompressors = 3;
#endif

static const String kCompressorsNames[kNumCompressors] = {
    _T("STB (nothings.org)"),
    _T("Squish"),
    _T("RGBCX (ryg)"),
#ifdef ENABLE_NVTT3
    _T("Nvidia Texture Tools 3")
#endif
};

static size_t Log2I(size_t v) {
    size_t result = 0;
    while (v >>= 1) {
        ++result;
    }
    return result;
}

template <typename T>
static T Clamp(const T& v, const T& left, const T& right) {
    return std::min(std::max(left, v), right);
}

constexpr size_t IsPowerOfTwo(const size_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}


inline bool StrEndsWith(const String& str, const String& ending) {
    return str.size() >= ending.size() && str.compare(str.size() - ending.size(), ending.size(), ending) == 0;
}


static void PrintUsage() {
    Cout << _T("Usage:") << std::endl;
    Cout << _T("  Mode 1 - Bump packing:") << std::endl;
    Cout << _T("    bumpx -n:path_to_normalmap -g:path_to_glossmap -h:path_to_heightmap -l:g -q:quality -o:output") << std::endl;
    Cout << _T("       here glossmap and heightmap can be ommited") << std::endl;
    Cout << _T("       -q:0 - fast compression, worst quality, -q:2 - slowest compression, best quality (default)") << std::endl;
    Cout << _T("       -l:g flag forces gloss to be stored in linear rather than log") << std::endl;
    Cout << _T("       if no output path provided - the output files will have same name as source and saved to the same folder") << std::endl;
    Cout << std::endl;
    Cout << _T("  Mode 2 - Bump unpacking:") << std::endl;
    Cout << _T("    bumpx path_to_bump.dds output_folder_path") << std::endl;
    Cout << std::endl;
}

PACKED_STRUCT_BEGIN
struct PixelMono {
    uint8_t r;
} PACKED_STRUCT_END;

PACKED_STRUCT_BEGIN
struct PixelRgb {
    uint8_t r, g, b;
} PACKED_STRUCT_END;

PACKED_STRUCT_BEGIN
struct PixelRgba {
    uint8_t r, g, b, a;
} PACKED_STRUCT_END;

template <typename T>
constexpr size_t BytesPerPixel() {
    return sizeof(T);
}

template <typename Tsrc, typename Tdst>
static Tdst ConvertPixel(const Tsrc& src) {
}

// simple l = 0.299 * r + 0.587 * g + 0.114 * b
// fast integer approximation (2 * r + 5 * g + b) / 8
template <>
PixelMono ConvertPixel(const PixelRgb& src) {
    size_t l = (src.r << 1) + ((src.g << 2) + src.g) + src.b;
    return { scast<uint8_t>((l >> 3) & 0xFF) };
}
template <>
PixelMono ConvertPixel(const PixelRgba& src) {
    size_t l = (src.r << 1) + ((src.g << 2) + src.g) + src.b;
    return { scast<uint8_t>((l >> 3) & 0xFF) };
}

// simple channels expansion (a will be 255)
template <>
PixelRgb ConvertPixel(const PixelMono& src) {
    return { src.r, src.r, src.r };
}
template <>
PixelRgba ConvertPixel(const PixelMono& src) {
    return { src.r, src.r, src.r , 0xFF };
}

// simple alpha remove-add
template <>
PixelRgb ConvertPixel(const PixelRgba& src) {
    return { src.r, src.g, src.b };
}
template <>
PixelRgba ConvertPixel(const PixelRgb& src) {
    return { src.r, src.g, src.b , 0xFF };
}


template <typename T>
struct Bitmap {
    using PixelType = T;

    Bitmap() = delete;
    Bitmap(const size_t w, const size_t h, const T& value = {0}) : width(w), height(h), pixels(w * h, value) {}

    inline bool empty() const { return pixels.empty(); }
    inline void clear() { width = 0; height = 0; pixels.clear(); }

    std::vector<PixelType>  pixels;
    size_t                  width;
    size_t                  height;
};

template <typename T>
struct Texture {
    using BitmapType = Bitmap<T>;

    std::vector<BitmapType> mips;

    Texture() = delete;
    Texture(const size_t w, const size_t h) {
        const size_t numMips = Log2I(std::max(w, h));
        mips.reserve(numMips);

        size_t mipW = w, mipH = h;
        for (size_t i = 0; i < numMips; ++i) {
            mips.push_back(BitmapType(mipW, mipH));

            mipW = std::max<size_t>(mipW / 2, kMinMipSize);
            mipH = std::max<size_t>(mipH / 2, kMinMipSize);
        }
    }
};


template <typename T>
Bitmap<T> LoadBitmap(const fs::path& path) {
    std::ifstream file(path, std::ifstream::binary);
    if (file.good()) {
        BytesArray bytes;
        file.seekg(0, std::ios::end);
        bytes.resize(file.tellg());
        file.seekg(0, std::ios::beg);
        file.read(rcast<char*>(bytes.data()), bytes.size());
        file.close();

        int w, h, comp;
        uint8_t* imgData = stbi_load_from_memory(bytes.data(), scast<int>(bytes.size()), &w, &h, &comp, STBI_default);
        if (!imgData) {
            return Bitmap<T>(0, 0);
        } else {
            std::unique_ptr<uint8_t, decltype(&stbi_image_free)> autoFreeImgData(imgData, stbi_image_free);

            Bitmap<T> result(scast<size_t>(w), scast<size_t>(h));

            const uint8_t* srcBegin = imgData;
            const uint8_t* srcEnd = imgData + (w * h * comp);

            const size_t desiredBpp = BytesPerPixel<T>();
            if (scast<size_t>(comp) == desiredBpp) {
                std::memcpy(result.pixels.data(), imgData, result.pixels.size() * desiredBpp);
            } else {
                const size_t permutation = comp * 10 + desiredBpp;
                switch (permutation) {
                    case 31: {
                        std::transform(rcast<const PixelRgb*>(srcBegin),
                                       rcast<const PixelRgb*>(srcEnd),
                                       rcast<PixelMono*>(result.pixels.data()),
                                       ConvertPixel<PixelRgb, PixelMono>);
                    } break;
                    case 41: {
                        std::transform(rcast<const PixelRgba*>(srcBegin),
                                       rcast<const PixelRgba*>(srcEnd),
                                       rcast<PixelMono*>(result.pixels.data()),
                                       ConvertPixel<PixelRgba, PixelMono>);
                    } break;
                    case 13: {
                        std::transform(rcast<const PixelMono*>(srcBegin),
                                       rcast<const PixelMono*>(srcEnd),
                                       rcast<PixelRgb*>(result.pixels.data()),
                                       ConvertPixel<PixelMono, PixelRgb>);
                    } break;
                    case 14: {
                        std::transform(rcast<const PixelMono*>(srcBegin),
                                       rcast<const PixelMono*>(srcEnd),
                                       rcast<PixelRgba*>(result.pixels.data()),
                                       ConvertPixel<PixelMono, PixelRgba>);
                    } break;
                    case 34: {
                        std::transform(rcast<const PixelRgb*>(srcBegin),
                                       rcast<const PixelRgb*>(srcEnd),
                                       rcast<PixelRgba*>(result.pixels.data()),
                                       ConvertPixel<PixelRgb, PixelRgba>);
                    } break;
                    case 43: {
                        std::transform(rcast<const PixelRgba*>(srcBegin),
                                       rcast<const PixelRgba*>(srcEnd),
                                       rcast<PixelRgb*>(result.pixels.data()),
                                       ConvertPixel<PixelRgba, PixelRgb>);
                    } break;

                    default:
                        return Bitmap<T>(0, 0);
                }
            }

            return std::move(result);
        }
    } else {
        return Bitmap<T>(0, 0);
    }
}

template <typename T, bool normalize>
static void MakeMip(const Bitmap<T>& src, Bitmap<T>& dst) {
    stbir_resize_uint8(rcast<const uint8_t*>(src.pixels.data()), scast<int>(src.width), scast<int>(src.height), 0,
                       rcast<uint8_t*>(dst.pixels.data()), scast<int>(dst.width), scast<int>(dst.height), 0,
                       scast<int>(BytesPerPixel<T>()));

    if constexpr(normalize && BytesPerPixel<T>() >= 3) {
        std::transform(dst.pixels.begin(), dst.pixels.end(), dst.pixels.begin(), [](const T& p)->T {
            float x = Clamp(scast<float>(p.r) / 255.0f, 0.0f, 1.0f) * 2.0f - 1.0f;
            float y = Clamp(scast<float>(p.g) / 255.0f, 0.0f, 1.0f) * 2.0f - 1.0f;
            float z = Clamp(scast<float>(p.b) / 255.0f, 0.0f, 1.0f) * 2.0f - 1.0f;
            const float il = 1.0f / std::sqrt(x * x + y * y + z * z);
            x *= il;
            y *= il;
            z *= il;
            T result = { 0 };
            result.r = scast<uint8_t>(Clamp((x * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            result.g = scast<uint8_t>(Clamp((y * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            result.b = scast<uint8_t>(Clamp((z * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            return result;
        });
    }
}

template <typename T, bool isNormalmap>
static void BuildMipchain(Texture<T>& texture) {
    const int numMips = scast<int>(texture.mips.size());
    for (int i = 1; i < numMips; ++i) {
        // for each subsequent mip we go as far as 3 steps up for a source for a compromise between quality and the speed
        const int srcMip = std::max(0, i - 3);
        MakeMip<T, isNormalmap>(texture.mips[srcMip], texture.mips[i]);
    }
}

void CompressBC3_STB(const Bitmap<PixelRgba>& bmp, void* outBlocks) {
    const uint8_t* srcPtr = rcast<const uint8_t*>(bmp.pixels.data());
    uint8_t* dst = rcast<uint8_t*>(outBlocks);

    uint8_t pixelsBlock[16 * 4] = { 0 };

    for (size_t y = 0; y < bmp.height; y += 4) {
        for (size_t x = 0; x < bmp.width; x += 4) {
            const uint8_t* src = srcPtr + (y * bmp.width + x) * 4;
            for (size_t i = 0; i < 4; ++i) {
                std::memcpy(&pixelsBlock[i * 16], src, 16);
                src += (bmp.width * 4);
            }

            stb_compress_dxt_block(dst, pixelsBlock, 1, STB_DXT_HIGHQUAL);
            dst += 16;
        }
    }
}

void CompressBC3_Squish(const Bitmap<PixelRgba>& bmp, void* outBlocks) {
    const uint8_t* srcPtr = rcast<const uint8_t*>(bmp.pixels.data());
    uint8_t* dst = rcast<uint8_t*>(outBlocks);

    uint8_t pixelsBlock[16 * 4] = { 0 };

    for (size_t y = 0; y < bmp.height; y += 4) {
        for (size_t x = 0; x < bmp.width; x += 4) {
            const uint8_t* src = srcPtr + (y * bmp.width + x) * 4;
            for (size_t i = 0; i < 4; ++i) {
                std::memcpy(&pixelsBlock[i * 16], src, 16);
                src += (bmp.width * 4);
            }

            squish::Compress(pixelsBlock, dst, squish::kDxt5 | squish::kColourIterativeClusterFit);
            dst += 16;
        }
    }
}

void CompressBC3_RGBCX(const Bitmap<PixelRgba>& bmp, void* outBlocks) {
    const uint8_t* srcPtr = rcast<const uint8_t*>(bmp.pixels.data());
    uint8_t* dst = rcast<uint8_t*>(outBlocks);

    uint8_t pixelsBlock[16 * 4] = { 0 };

    for (size_t y = 0; y < bmp.height; y += 4) {
        for (size_t x = 0; x < bmp.width; x += 4) {
            const uint8_t* src = srcPtr + (y * bmp.width + x) * 4;
            for (size_t i = 0; i < 4; ++i) {
                std::memcpy(&pixelsBlock[i * 16], src, 16);
                src += (bmp.width * 4);
            }

            rgbcx::encode_bc3(rgbcx::MAX_LEVEL, dst, pixelsBlock);
            dst += 16;
        }
    }
}

#ifdef ENABLE_NVTT3
void CompressBC3_NVTT3(const Bitmap<PixelRgba>& bmp, void* outBlocks) {
    NvttRefImage nvttImage = {};
    nvttImage.data = bmp.pixels.data();
    nvttImage.width = static_cast<int>(bmp.width);
    nvttImage.height = static_cast<int>(bmp.height);
    nvttImage.depth = 1;
    nvttImage.num_channels = 4;
    nvttImage.channel_swizzle[0] = NVTT_ChannelOrder_Red;
    nvttImage.channel_swizzle[1] = NVTT_ChannelOrder_Green;
    nvttImage.channel_swizzle[2] = NVTT_ChannelOrder_Blue;
    nvttImage.channel_swizzle[3] = NVTT_ChannelOrder_Alpha;
    nvttImage.channel_interleave = NVTT_True;

    NvttCPUInputBuffer* input = func_nvttCreateCPUInputBuffer(&nvttImage, NVTT_ValueType_UINT8, 1, 4, 4, 1, 1, 1, 1, nullptr, nullptr);
    func_nvttEncodeBC3CPU(input, NVTT_False, outBlocks, NVTT_False, NVTT_False, nullptr);
    func_nvttDestroyCPUInputBuffer(input);
}
#endif // ENABLE_NVTT3


void CompressBC3(const int quality, const Bitmap<PixelRgba>& bmp, void* outBlocks) {
    switch (quality) {
        case 0:
            CompressBC3_STB(bmp, outBlocks);
        break;
        case 1:
            CompressBC3_Squish(bmp, outBlocks);
        break;
#ifdef ENABLE_NVTT3
        case 3:
            CompressBC3_NVTT3(bmp, outBlocks);
        break;
#endif
        case 2:
        default:
            CompressBC3_RGBCX(bmp, outBlocks);
        break;
    }
}


void DecompressBC3_MY(const void* inputBlocks, Bitmap<PixelRgba>& output) {
    const uint8_t* src = rcast<const uint8_t*>(inputBlocks);
    uint8_t* dest = rcast<uint8_t*>(output.pixels.data());

    for (size_t y = 0; y < output.height; y += 4) {
        for (size_t x = 0; x < output.width; x += 4) {
            uint8_t* dst = dest + (y * output.width + x) * 4;
            bcdec_bc3(src, dst, static_cast<int>(output.width * 4));
            src += BCDEC_BC3_BLOCK_SIZE;
        }
    }
}


// "DDS "
const uint32_t kDDSFileSignature = 0x20534444;

struct DDCOLORKEY {
    uint32_t dwUnused0;
    uint32_t dwUnused1;
};

struct DDPIXELFORMAT {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwFourCC;
    uint32_t dwRGBBitCount;     // ATI compressonator will place a FOURCC code here for swizzled/cooked DXTn formats
    uint32_t dwRBitMask;
    uint32_t dwGBitMask;
    uint32_t dwBBitMask;
    uint32_t dwRGBAlphaBitMask;
};

struct DDSCAPS2 {
    uint32_t dwCaps;
    uint32_t dwCaps2;
    uint32_t dwCaps3;
    uint32_t dwCaps4;
};

struct DDSURFACEDESC2 {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwHeight;
    uint32_t dwWidth;
    union {
        int32_t lPitch;
        uint32_t dwLinearSize;
    };
    uint32_t dwBackBufferCount;
    uint32_t dwMipMapCount;
    uint32_t dwAlphaBitDepth;
    uint32_t dwUnused0;
    uint32_t lpSurface;
    DDCOLORKEY unused0;
    DDCOLORKEY unused1;
    DDCOLORKEY unused2;
    DDCOLORKEY unused3;
    DDPIXELFORMAT ddpfPixelFormat;
    DDSCAPS2 ddsCaps;
    uint32_t dwUnused1;
};

bool SaveAsDDS(const std::vector<BytesArray>& compressedMips, const size_t w, const size_t h, const fs::path& outPath) {
    std::ofstream file(outPath, std::ofstream::binary);
    if (file.good()) {
        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(DDSURFACEDESC2);
        desc.dwFlags = 0x00021007; // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT
        desc.dwWidth = scast<uint32_t>(w);
        desc.dwHeight = scast<uint32_t>(h);
        desc.dwMipMapCount = scast<uint32_t>(compressedMips.size());
        desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        desc.ddpfPixelFormat.dwFlags = 0x00000004; // DDPF_FOURCC
        desc.ddpfPixelFormat.dwFourCC = 0x35545844; // DXT5
        desc.ddsCaps.dwCaps = 0x00401000;// DDSCAPS_TEXTURE | DDSCAPS_MIPMAP;

        file.write(rcast<const char*>(&kDDSFileSignature), sizeof(kDDSFileSignature));
        file.write(rcast<const char*>(&desc), sizeof(desc));

        for (auto& cm : compressedMips) {
            file.write(rcast<const char*>(cm.data()), cm.size());
        }

        file.flush();
        file.close();

        return true;
    } else {
        return false;
    }
}

int PackBump(int argc, Char** argv) {
    std::error_code errorCode;
    fs::file_status fileStatus;

    String paramN, paramG, paramH, paramO, paramL, paramQ;

    std::vector<std::pair<Char, String*>> paramsMap = {
        { _T('n'), &paramN },
        { _T('g'), &paramG },
        { _T('h'), &paramH },
        { _T('o'), &paramO },
        { _T('l'), &paramL },
        { _T('q'), &paramQ }
    };

    Char** it = argv, **end = argv + argc;
    for (; it != end; ++it) {
        String s = *it;

        bool knownParam = false;
        if (s.length() > 3 && s[2] == ':') {
            if (s[0] == _T('-')) {
                const Char c = s[1];
                auto paramsIt = std::find_if(paramsMap.begin(), paramsMap.end(), [c](auto& v)->bool {
                    return c == v.first;
                });

                if (paramsIt != paramsMap.end()) {
                    knownParam = true;
                    *paramsIt->second = s.substr(3);
                    paramsMap.erase(paramsIt);
                } else {
                    const size_t paramIdx = std::distance(it, end);
                    Cerr << _T("Unknown param #") << paramIdx << _T(" \"") << s << _T("\"") << std::endl;
                }
            }
        }
    }

    const bool linearGloss = !paramL.empty() && paramL.front() == _T('g');
    int quality = !paramQ.empty() ? std::stoi(paramQ) : 2;

    if (quality < 0) {
        quality = 0;
    } else if (quality >= kNumCompressors) {
        quality = static_cast<int>(kNumCompressors - 1);
    }

    Cout << _T("Using quality level ") << quality << std::endl;

#ifdef ENABLE_NVTT3
    if (quality == 3) {
        void* hDll = LoadLibraryW(_T("nvtt30106.dll"));
        if (!hDll) {
            quality = 2;
        } else {
            func_nvttCreateCPUInputBuffer = reinterpret_cast<decltype(func_nvttCreateCPUInputBuffer)>(GetProcAddress(hDll, "nvttCreateCPUInputBuffer"));
            func_nvttDestroyCPUInputBuffer = reinterpret_cast<decltype(func_nvttDestroyCPUInputBuffer)>(GetProcAddress(hDll, "nvttDestroyCPUInputBuffer"));
            func_nvttEncodeBC3CPU = reinterpret_cast<decltype(func_nvttEncodeBC3CPU)>(GetProcAddress(hDll, "nvttEncodeBC3CPU"));

            if (!func_nvttCreateCPUInputBuffer || !func_nvttDestroyCPUInputBuffer || !func_nvttEncodeBC3CPU) {
                quality = 2;
            }
        }

        if (quality == 2) {
            Cerr << _T("Failed to load nvtt3 dll!") << std::endl << _T("Changing quality level to 2.") << std::endl;
        }
    }
#endif

    Cout << _T("This will use \"") << kCompressorsNames[quality] << _T("\" compressor") << std::endl;

    fs::path pathNormalmap, pathGlossmap, pathHeightmap, pathOutput;

    if (paramN.empty()) {
        Cerr << _T("No normalmap provided, nothing to do for me...") << std::endl;
        PrintUsage();
        return -1;
    }

    pathNormalmap = paramN;
    fileStatus = fs::status(pathNormalmap, errorCode);
    if (!fs::exists(fileStatus) || !fs::is_regular_file(fileStatus)) {
        Cerr << _T("Provided normalmap path does not exist or not a valid file!") << std::endl;
        return -1;
    }

    if (paramO.empty()) {
        Cout << _T("No output option provided, using source name and folder") << std::endl;
        pathOutput = pathNormalmap.parent_path() / pathNormalmap.stem();
    } else {
        pathOutput = paramO;
        fileStatus = fs::status(pathOutput, errorCode);
        if (fs::exists(fileStatus) && fs::is_directory(fileStatus)) {
            Cout << _T("A directory was provided as an output, source name will be used") << std::endl;
            pathOutput /= pathNormalmap.stem();
        }
    }

    if (!paramG.empty()) {
        pathGlossmap = paramG;
        fileStatus = fs::status(pathGlossmap, errorCode);
        if (!fs::exists(fileStatus) || !fs::is_regular_file(fileStatus)) {
            Cout << _T("Provided glossmap path does not exist or not a valid file.") << std::endl;
            Cout << _T("This is not a showstopper, just gloss will be omitted from the result.") << std::endl;
            pathGlossmap.clear();
        }
    }

    if (!paramH.empty()) {
        pathHeightmap = paramH;
        fileStatus = fs::status(pathHeightmap, errorCode);
        if (!fs::exists(fileStatus) || !fs::is_regular_file(fileStatus)) {
            Cout << _T("Provided heightmap path does not exist or not a valid file.") << std::endl;
            Cout << _T("This is not a showstopper, default (neutral) height will be used.") << std::endl;
            pathHeightmap.clear();
        }
    }

    Bitmap<PixelRgba> normalmap = LoadBitmap<PixelRgba>(pathNormalmap);
    if (normalmap.empty()) {
        Cerr << _T("Couldn't load normalmap, not an image or unsupported format?") << std::endl;
        return -1;
    } else if (!IsPowerOfTwo(normalmap.width) || !IsPowerOfTwo(normalmap.height)) {
        Cerr << _T("Normalmap width & height must be power of two!") << std::endl;
        return -1;
    }

    Bitmap<PixelMono> glossmap = pathGlossmap.empty() ? Bitmap<PixelMono>(0, 1) : LoadBitmap<PixelMono>(pathGlossmap);
    Bitmap<PixelMono> heightmap = pathHeightmap.empty() ? Bitmap<PixelMono>(0, 1) : LoadBitmap<PixelMono>(pathHeightmap);

    if (glossmap.empty() && !glossmap.height) {
        Cout << _T("Couldn't load glossmap, not an image or unsupported format?") << std::endl;
        Cout << _T("This is not a showstopper, just gloss will be omitted from the result.") << std::endl;
    } else if (glossmap.width != normalmap.width || glossmap.height != normalmap.height) {
        Cout << _T("Glossmap has different dimensions than normalmap!") << std::endl;
        Cout << _T("This is not a showstopper, just gloss will be omitted from the result.") << std::endl;
        glossmap.clear();
    }

    if (heightmap.empty() && !heightmap.height) {
        Cout << _T("Couldn't load heightmap, not an image or unsupported format?") << std::endl;
        Cout << _T("This is not a showstopper, default (neutral) height will be used.") << std::endl;
    } else if (heightmap.width != normalmap.width || heightmap.height != normalmap.height) {
        Cout << _T("Heightmap has different dimensions than normalmap!") << std::endl;
        Cout << _T("This is not a showstopper, default (neutral) height will be used.") << std::endl;
        heightmap.clear();
    }

    // make default heightmap
    if (heightmap.empty()) {
        heightmap = Bitmap<PixelMono>(normalmap.width, normalmap.height, { 128 });
    }

    //rgbcx::init(rgbcx::bc1_approx_mode::cBC1IdealRound4);
    rgbcx::init(rgbcx::bc1_approx_mode::cBC1NVidia);

    const size_t nwidth = normalmap.width;
    const size_t nheight = normalmap.height;

    // step 1: make mipchains with our source images
    Cout << _T("Computing mipmaps for the source normalmap...") << std::endl;
    Texture<PixelRgba> normalmapWithMips(nwidth, nheight);
    normalmapWithMips.mips[0] = normalmap; normalmap.clear();
    BuildMipchain<PixelRgba, true>(normalmapWithMips);
    Cout << _T("Successfully created ") << normalmapWithMips.mips.size() << _T(" mips") << std::endl;

    Texture<PixelMono> glossmapWithMips(nwidth, nheight);
    if (!glossmap.empty()) {
        Cout << _T("Computing mipmaps for the source glossmap...") << std::endl;
        glossmapWithMips.mips[0] = glossmap; glossmap.clear();
        BuildMipchain<PixelMono, false>(glossmapWithMips);
        Cout << _T("Successfully created ") << glossmapWithMips.mips.size() << _T(" mips") << std::endl;
    }

    Texture<PixelMono> heightmapWithMips(nwidth, nheight);
    if (!heightmap.empty()) {
        Cout << _T("Computing mipmaps for the source heightmap...") << std::endl;
        heightmapWithMips.mips[0] = heightmap; heightmap.clear();
        BuildMipchain<PixelMono, false>(heightmapWithMips);
        Cout << _T("Successfully created ") << heightmapWithMips.mips.size() << _T(" mips") << std::endl;
    }

    // step 2: assemble stalker normalmap
    Cout << _T("Assembling stalker bump (a - NX, b - NY, g - NZ, r - Gloss)...") << std::endl;
    for (size_t i = 0, end = normalmapWithMips.mips.size(); i != end; ++i) {
        auto& normalMip = normalmapWithMips.mips[i];
        auto& glossMip = glossmapWithMips.mips[i];
        std::transform(normalMip.pixels.begin(),
            normalMip.pixels.end(),
            glossMip.pixels.begin(),
            normalMip.pixels.begin(),
            [linearGloss](const auto& np, const auto& gp)->PixelRgba {
            return {
                // stalker stores gloss logarithmically to gain some precision for lower values (linearized back in shader)
                linearGloss ? gp.r : scast<uint8_t>(std::sqrt(scast<float>(gp.r) / 255.0f) * 255.0f),
                // swizzle is weird, as NZ typically doesn't require much precision (you can even omit one)
                // but meh, we must follow the original
                np.b,
                np.g,
                np.r
            };
        });
    }
    Cout << _T("Done") << std::endl;

    // step 3: compress the normalmap
    std::vector<BytesArray> normalmapWithMipsCompressed(normalmapWithMips.mips.size());
    for (size_t i = 0, end = normalmapWithMips.mips.size(); i != end; ++i) {
        auto& normalMip = normalmapWithMips.mips[i];
        auto& compressedMip = normalmapWithMipsCompressed[i];

        Cout << _T("Compressing bump mip ") << i << _T("...") << std::endl;

        const size_t compressedMipSize = ((normalMip.width / 4) * (normalMip.height / 4)) * 16;
        compressedMip.resize(compressedMipSize);

        CompressBC3(quality, normalMip, compressedMip.data());

        const size_t originalMipSize = normalMip.width * normalMip.height * BytesPerPixel<PixelRgba>();
        Cout << _T("Done, compressed ") << originalMipSize << _T(" bytes to ") << compressedMipSize << _T(" bytes") << std::endl;
    }

    // step 4: decompress the normalmap and calculate the error, assemble bump# with the error and the height
    //         the format is: RGB - error * 2, A - height
    Texture<PixelRgba> bumpXWithMips(nwidth, nheight);
    for (size_t i = 0, end = normalmapWithMips.mips.size(); i != end; ++i) {
        auto& normalMip = normalmapWithMips.mips[i];
        auto& compressedMip = normalmapWithMipsCompressed[i];
        auto& heightMip = heightmapWithMips.mips[i];
        auto& bumpXMip = bumpXWithMips.mips[i];

        Cout << _T("Calculating error for mip ") << i << _T("...") << std::endl;
        DecompressBC3_MY(compressedMip.data(), bumpXMip);

        // calculate the difference and un-swizzle back to RGB
        std::transform(normalMip.pixels.begin(),
            normalMip.pixels.end(),
            bumpXMip.pixels.begin(),
            bumpXMip.pixels.begin(),
            [](const auto& np, const auto& dp)->PixelRgba {
            return {
                scast<uint8_t>(Clamp((scast<int>(np.a) - scast<int>(dp.a)) * 2 + 128, 0, 255)),
                scast<uint8_t>(Clamp((scast<int>(np.b) - scast<int>(dp.b)) * 2 + 128, 0, 255)),
                scast<uint8_t>(Clamp((scast<int>(np.g) - scast<int>(dp.g)) * 2 + 128, 0, 255)),
                0
            };
        });

        // move height to alpha
        std::transform(bumpXMip.pixels.begin(),
            bumpXMip.pixels.end(),
            heightMip.pixels.begin(),
            bumpXMip.pixels.begin(),
            [](const auto& xp, const auto& hp)->PixelRgba {
            return { xp.r, xp.g, xp.b, hp.r };
        });

        Cout << _T("Done") << std::endl;
    }

    // step 5: compress bump#
    std::vector<BytesArray> bumpXMipsCompressed(bumpXWithMips.mips.size());
    for (size_t i = 0, end = bumpXWithMips.mips.size(); i != end; ++i) {
        auto& bumpXMip = bumpXWithMips.mips[i];
        auto& compressedMip = bumpXMipsCompressed[i];

        Cout << _T("Compressing bump# mip ") << i << _T("...") << std::endl;

        const size_t compressedMipSize = ((bumpXMip.width / 4) * (bumpXMip.height / 4)) * 16;
        compressedMip.resize(compressedMipSize);

        CompressBC3(quality, bumpXMip, compressedMip.data());

        const size_t originalMipSize = bumpXMip.width * bumpXMip.height * BytesPerPixel<PixelRgba>();
        Cout << _T("Done, compressed ") << originalMipSize << _T(" bytes to ") << compressedMipSize << _T(" bytes") << std::endl;
    }

    // step 6: save everything
    fs::path bumpOutputPath = pathOutput; bumpOutputPath += _T("_bump.dds");
    fs::path bumpXOutputPath = pathOutput; bumpXOutputPath += _T("_bump#.dds");

    if (!SaveAsDDS(normalmapWithMipsCompressed, nwidth, nheight, bumpOutputPath)) {
        Cerr << _T("Failed to write bump texture to ") << bumpOutputPath << std::endl;
        return -1;
    } else {
        Cout << _T("Successfully saved ") << bumpOutputPath << std::endl;
    }

    if (!SaveAsDDS(bumpXMipsCompressed, nwidth, nheight, bumpXOutputPath)) {
        Cerr << _T("Failed to write bump# texture to ") << bumpXOutputPath << std::endl;
        return -1;
    } else {
        Cout << _T("Successfully saved ") << bumpXOutputPath << std::endl;
    }

    return 0;
}

int UnpackBump(int argc, Char** argv) {
    auto loadAndDecompressDDS = [](const fs::path& ddsPath)->Bitmap<PixelRgba> {
        std::ifstream file(ddsPath, std::ifstream::in | std::ifstream::binary);
        if (file.is_open()) {
            uint32_t signature = 0;
            file.read(rcast<char*>(&signature), sizeof(signature));
            if (kDDSFileSignature == signature) {
                DDSURFACEDESC2 desc = {};
                file.read(rcast<char*>(&desc), sizeof(desc));
                if (desc.ddpfPixelFormat.dwFlags == 0x00000004 &&   // DDPF_FOURCC
                    desc.ddpfPixelFormat.dwFourCC == 0x35545844) {  // DXT5
                    const size_t compressedSize = ((desc.dwWidth / 4) * (desc.dwHeight / 4)) * 16;
                    std::vector<uint8_t> compressedImage(compressedSize);
                    file.read(rcast<char*>(compressedImage.data()), compressedImage.size());
                    file.close();

                    Bitmap<PixelRgba> bmp(desc.dwWidth, desc.dwHeight);
                    DecompressBC3_MY(compressedImage.data(), bmp);
                    return bmp;
                } else {
                    return Bitmap<PixelRgba>(0, 0);
                }
            } else {
                return Bitmap<PixelRgba>(0, 0);
            }
        } else {
            return Bitmap<PixelRgba>(0, 0);
        }
    };

    fs::path bumpPath = argv[1];

    Bitmap<PixelRgba> bump = loadAndDecompressDDS(bumpPath);
    if (bump.empty()) {
        Cout << _T("Failed to load ") << argv[1] << std::endl;
        return -1;
    }

    fs::path bumpXPath = (bumpPath.parent_path() / bumpPath.stem()).native() + _T("#.dds");
    Bitmap<PixelRgba> bumpX = loadAndDecompressDDS(bumpXPath);
    if (bumpX.empty()) {
        Cout << _T("Failed to load ") << bumpXPath << std::endl;
        return -1;
    }

    if (bump.width != bumpX.width || bump.height != bumpX.height) {
        Cout << _T("The two dds files are not of the same size! Aborting...") << std::endl;
        return -1;
    }

    fs::path bumpName = bumpPath.stem();

    fs::path outputFolder = (argc == 3) ? argv[2] : bumpPath.parent_path();

    Cout << _T("Saving out heightmap:") << std::endl;
    fs::path heightmapPath = outputFolder / (bumpName.native() + _T("_height.tga"));
    Cout << heightmapPath << std::endl;
    std::vector<uint8_t> heightmapData(bumpX.width * bumpX.height);
    for (size_t i = 0; i < bumpX.width * bumpX.height; ++i) {
        heightmapData[i] = bumpX.pixels[i].a;
    }
    int stbResult = stbi_write_tga(heightmapPath.u8string().c_str(), scast<int>(bumpX.width), scast<int>(bumpX.height), 1, heightmapData.data());
    if (!stbResult) {
        Cout << _T("Failed :(") << std::endl;
    }
    heightmapData.clear();

    Cout << _T("Saving out glossmap:") << std::endl;
    fs::path glossmapPath = outputFolder / (bumpName.native() + _T("_gloss.tga"));
    Cout << glossmapPath << std::endl;
    std::vector<uint8_t> glossmapData(bump.width * bump.height);
    for (size_t i = 0; i < bump.width * bump.height; ++i) {
        glossmapData[i] = bump.pixels[i].r;
    }
    stbResult = stbi_write_tga(glossmapPath.u8string().c_str(), scast<int>(bump.width), scast<int>(bump.height), 1, glossmapData.data());
    if (!stbResult) {
        Cout << _T("Failed :(") << std::endl;
    }
    glossmapData.clear();

    Cout << _T("Saving out normalmap:") << std::endl;
    fs::path normalmapPath = outputFolder / (bumpName.native() + _T("_normal.tga"));
    Cout << normalmapPath << std::endl;
    std::vector<PixelRgb> normalmapData(bump.width * bump.height);
    for (size_t i = 0; i < bump.width * bump.height; ++i) {
        const float nX = scast<float>(bump.pixels[i].a) / 255.0f;
        const float nY = scast<float>(bump.pixels[i].b) / 255.0f;
        const float nZ = scast<float>(bump.pixels[i].g) / 255.0f;

        const float eX = scast<float>(bumpX.pixels[i].r) / 255.0f;
        const float eY = scast<float>(bumpX.pixels[i].g) / 255.0f;
        const float eZ = scast<float>(bumpX.pixels[i].b) / 255.0f;

        float NX = nX
        float NY = nY + (eY + (eY - 1.0f));
        float NZ = nZ + (eZ - 1.0f);

        const float il = 1.0f / std::sqrt(NX * NX + NY * NY + NZ * NZ);
        NX *= il;
        NY *= il;
        NZ *= il;
        PixelRgb& result = normalmapData[i];
        result.r = scast<uint8_t>(Clamp((NX * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
        result.g = scast<uint8_t>(Clamp((NY * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
        result.b = scast<uint8_t>(Clamp((NZ * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
    }
    stbResult = stbi_write_tga(normalmapPath.u8string().c_str(), scast<int>(bump.width), scast<int>(bump.height), 3, normalmapData.data());
    if (!stbResult) {
        Cout << _T("Failed :(") << std::endl;
    }

    return 0;
}


int Main(int argc, Char** argv) {
    int returnCode = 0;

    if (argc <= 1 || (argc > 1 && String(_T("-help")) == argv[1])) {
        PrintUsage();
    } else {
        // detect the mode
        bool isPackingMode = true;
        if (argc == 2 || argc == 3) {
            if (StrEndsWith(argv[1], _T(".dds"))) {
                isPackingMode = false;
            }
        }

        Cout << _T("Selected mode - ") << (isPackingMode ? _T("1, packing.") : _T("2, unpacking.")) << std::endl;

        returnCode = isPackingMode ? PackBump(argc, argv) : UnpackBump(argc, argv);
    }

    return returnCode;
}


// Changelog:
// v0.7 - added nvtt3 library and option to use it (Windows only atm)
// v0.6 - added mode to decompose bump and bump# files back to normalmap gloss and heightmap
// v0.5 - added Kaiser resample filter to stbi_image_resize library and using it by default now
// v0.4 - added RGBCX BC compression for even better quality
// v0.3 - added Squish BC compression for better quality
// v0.2 - added "-l:g" option to store gloss in linear vs exponent
// v0.1 - Initial release
