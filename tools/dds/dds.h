/* tools/dds/dds.h — minimal DDS container parser (header-only, C11).
 *
 * Decodes the DDS header (classic + DX10 extended) far enough to create a
 * Metal texture: BC1-5 (DXT1/3/5, BC4U, BC5U) and uncompressed 32/24/16-bit.
 * The harness maps `mtl_format` to a MTLPixelFormat (macOS supports BCn
 * natively — no software decompression needed); this header only decodes the
 * container and reports what it found. Verifiable with tools/dds/test_dds.c.
 */
#ifndef BINC_DDS_H
#define BINC_DDS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DDS_MAGIC 0x20534444u /* "DDS " little-endian */

/* DDS_PIXELFORMAT.dwFlags */
#define DDPF_ALPHAPIXELS 0x00000001u
#define DDPF_FOURCC      0x00000004u
#define DDPF_RGB         0x00000040u
#define DDPF_LUMINANCE   0x00020000u
#define DDPF_LUMINANCEA  0x00040000u

/* DDS_HEADER.dwFlags */
#define DDSD_CAPS       0x00000001u
#define DDSD_HEIGHT     0x00000002u
#define DDSD_WIDTH      0x00000004u
#define DDSD_PITCH      0x00000008u
#define DDSD_PIXELFORMAT 0x00001000u
#define DDSD_MIPMAPCOUNT 0x00020000u
#define DDSD_LINEARSIZE 0x00080000u
#define DDSD_DEPTH      0x00800000u

#define DDS_FOURCC_DX10 0x30315844u /* "DX10" */

typedef enum {
    DDS_OK = 0,
    DDS_ERR_MAGIC,
    DDS_ERR_SIZE,     /* buffer smaller than the header */
    DDS_ERR_HEADER,   /* dwSize / required flags wrong */
    DDS_ERR_FORMAT,   /* unknown fourcc / pixel format */
} DdsStatus;

typedef struct {
    uint32_t width, height, depth, mips;
    uint32_t fourcc;        /* 0 when uncompressed; 'DX10' for extended */
    uint32_t dx10_dxgi;     /* DXGI format when the DX10 header is present */
    uint32_t bits_per_pixel;
    uint32_t rmask, gmask, bmask, amask;
    int is_bc;              /* 1 when block-compressed */
    size_t data_offset;
    size_t data_size;
    const char *mtl_format; /* Metal pixel format name (macOS) */
    int is_srgb;
} DdsInfo;

/* classic fourccs */
#define FOURCC_DXT1 0x31545844u
#define FOURCC_DXT3 0x33545844u
#define FOURCC_DXT5 0x35545844u
#define FOURCC_BC4U 0x55344154u /* ATI1 */
#define FOURCC_BC5U 0x55324254u /* ATI2 */
#define FOURCC_BC4S 0x53344154u
#define FOURCC_BC5S 0x53324254u

/* D3D9-era floating-point DDS files store the D3DFMT enum value as the fourcc */
#define D3DFMT_R16F 111u
#define D3DFMT_G16R16F 112u
#define D3DFMT_A16B16G16R16F 113u
#define D3DFMT_R32F 114u
#define D3DFMT_G32R32F 115u
#define D3DFMT_A32B32G32R32F 116u

/* DXGI formats we map (subset of the full table) */
#define DXGI_BC1_UNORM 71u
#define DXGI_BC1_UNORM_SRGB 72u
#define DXGI_BC2_UNORM 74u
#define DXGI_BC2_UNORM_SRGB 75u
#define DXGI_BC3_UNORM 77u
#define DXGI_BC3_UNORM_SRGB 78u
#define DXGI_BC4_UNORM 80u
#define DXGI_BC5_UNORM 83u
#define DXGI_R16G16B16A16_FLOAT 10u
#define DXGI_R32G32B32A32_FLOAT 2u
#define DXGI_R8G8B8A8_UNORM 28u
#define DXGI_R8G8B8A8_UNORM_SRGB 29u
#define DXGI_B8G8R8A8_UNORM 87u
#define DXGI_B8G8R8A8_UNORM_SRGB 91u
#define DXGI_BC6H_UF16 95u
#define DXGI_BC6H_SF16 96u
#define DXGI_BC7_UNORM 98u
#define DXGI_BC7_UNORM_SRGB 99u

static uint32_t dds_rd32(const uint8_t *b){ return (uint32_t)b[0] | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24); }

static DdsStatus dds_parse(const uint8_t *buf, size_t len, DdsInfo *o){
    if (len < 4 || dds_rd32(buf) != DDS_MAGIC) return DDS_ERR_MAGIC;
    if (len < 4 + 124) return DDS_ERR_SIZE;
    const uint8_t *h = buf + 4;
    uint32_t size = dds_rd32(h), flags = dds_rd32(h + 4);
    if (size < 124 || !(flags & DDSD_WIDTH) || !(flags & DDSD_HEIGHT) || !(flags & DDSD_PIXELFORMAT))
        return DDS_ERR_HEADER;
    memset(o, 0, sizeof *o);
    o->height = dds_rd32(h + 8);
    o->width  = dds_rd32(h + 12);
    o->depth  = (flags & DDSD_DEPTH) ? dds_rd32(h + 20) : 1u;
    o->mips   = (flags & DDSD_MIPMAPCOUNT) ? dds_rd32(h + 24) : 1u;
    if (o->mips == 0) o->mips = 1; /* D3D9 tools wrote mips=0 to mean 1 */
    if (o->width == 0 || o->height == 0) return DDS_ERR_HEADER;

    const uint8_t *pf = h + 72; /* DDS_PIXELFORMAT */
    uint32_t pf_size = dds_rd32(pf), pf_flags = dds_rd32(pf + 4);
    o->fourcc = dds_rd32(pf + 8);
    if (pf_size < 32 || !(pf_flags & (DDPF_FOURCC | DDPF_RGB | DDPF_LUMINANCE | DDPF_LUMINANCEA)))
        return DDS_ERR_HEADER;

    o->bits_per_pixel = dds_rd32(pf + 12);
    o->rmask = dds_rd32(pf + 16); o->gmask = dds_rd32(pf + 20);
    o->bmask = dds_rd32(pf + 24); o->amask = dds_rd32(pf + 28);

    size_t px = 4 + 124;
    if (o->fourcc == DDS_FOURCC_DX10) {
        if (len < px + 20) return DDS_ERR_SIZE;
        o->dx10_dxgi = dds_rd32(buf + px);
        px += 20;
        switch (o->dx10_dxgi) {
            case DXGI_BC1_UNORM:      o->is_bc = 1; o->mtl_format = "BC1_RGBA";      break;
            case DXGI_BC1_UNORM_SRGB: o->is_bc = 1; o->mtl_format = "BC1_RGBA_sRGB"; o->is_srgb = 1; break;
            case DXGI_BC2_UNORM:      o->is_bc = 1; o->mtl_format = "BC2_RGBA";      break;
            case DXGI_BC2_UNORM_SRGB: o->is_bc = 1; o->mtl_format = "BC2_RGBA_sRGB"; o->is_srgb = 1; break;
            case DXGI_BC3_UNORM:      o->is_bc = 1; o->mtl_format = "BC3_RGBA";      break;
            case DXGI_BC3_UNORM_SRGB: o->is_bc = 1; o->mtl_format = "BC3_RGBA_sRGB"; o->is_srgb = 1; break;
            case DXGI_BC4_UNORM:      o->is_bc = 1; o->mtl_format = "BC4_RUnorm";    break;
            case DXGI_BC5_UNORM:      o->is_bc = 1; o->mtl_format = "BC5_RGUnorm";   break;
            case DXGI_BC6H_UF16:      o->is_bc = 1; o->mtl_format = "BC6H_RGBUfloat";  break;
            case DXGI_BC6H_SF16:      o->is_bc = 1; o->mtl_format = "BC6H_RSGBFloat";  break;
            case DXGI_BC7_UNORM:      o->is_bc = 1; o->mtl_format = "BC7_RGBAUnorm";   break;
            case DXGI_BC7_UNORM_SRGB: o->is_bc = 1; o->mtl_format = "BC7_RGBAUnorm_sRGB"; o->is_srgb = 1; break;
            case DXGI_R8G8B8A8_UNORM:      o->mtl_format = "RGBA8Unorm";   break;
            case DXGI_R8G8B8A8_UNORM_SRGB: o->mtl_format = "RGBA8Unorm_sRGB"; o->is_srgb = 1; break;
            case DXGI_B8G8R8A8_UNORM:      o->mtl_format = "BGRA8Unorm";   break;
            case DXGI_B8G8R8A8_UNORM_SRGB: o->mtl_format = "BGRA8Unorm_sRGB"; o->is_srgb = 1; break;
            case DXGI_R16G16B16A16_FLOAT:  o->mtl_format = "RGBA16Float";  break;
            case DXGI_R32G32B32A32_FLOAT:  o->mtl_format = "RGBA32Float";  break;
            default: return DDS_ERR_FORMAT;
        }
    } else if (pf_flags & DDPF_FOURCC) {
        switch (o->fourcc) {
            case FOURCC_DXT1: o->is_bc = 1; o->mtl_format = "BC1_RGBA";      break;
            case FOURCC_DXT3: o->is_bc = 1; o->mtl_format = "BC2_RGBA";      break;
            case FOURCC_DXT5: o->is_bc = 1; o->mtl_format = "BC3_RGBA";      break;
            case FOURCC_BC4U: o->is_bc = 1; o->mtl_format = "BC4_RUnorm";    break;
            case FOURCC_BC5U: o->is_bc = 1; o->mtl_format = "BC5_RGUnorm";   break;
            case FOURCC_BC4S: o->is_bc = 1; o->mtl_format = "BC4_RSnorm";    break;
            case FOURCC_BC5S: o->is_bc = 1; o->mtl_format = "BC5_RGSNorm";   break;
            case D3DFMT_R16F:          o->bits_per_pixel = 16; o->mtl_format = "R16Float";    break;
            case D3DFMT_G16R16F:       o->bits_per_pixel = 32; o->mtl_format = "RG16Float";   break;
            case D3DFMT_A16B16G16R16F: o->bits_per_pixel = 64; o->mtl_format = "RGBA16Float"; break;
            case D3DFMT_R32F:          o->bits_per_pixel = 32; o->mtl_format = "R32Float";    break;
            case D3DFMT_G32R32F:       o->bits_per_pixel = 64; o->mtl_format = "RG32Float";   break;
            case D3DFMT_A32B32G32R32F: o->bits_per_pixel = 128; o->mtl_format = "RGBA32Float"; break;
            default: return DDS_ERR_FORMAT;
        }
    } else if (pf_flags & DDPF_LUMINANCEA) {
        if (o->bits_per_pixel == 16) o->mtl_format = "RG8Unorm";
        else return DDS_ERR_FORMAT;
    } else if (pf_flags & DDPF_LUMINANCE) {
        if (o->bits_per_pixel == 8) o->mtl_format = "R8Unorm";
        else if (o->bits_per_pixel == 16) o->mtl_format = "R16Unorm";
        else return DDS_ERR_FORMAT;
    } else if (pf_flags & DDPF_RGB) {
        if (o->bits_per_pixel == 32) {
            /* D3D's canonical BGRA8 layout; R8G8B8A8 when the masks say so */
            o->mtl_format = (o->rmask == 0x00ff0000u && o->bmask == 0x000000ffu)
                              ? "RGBA8Unorm" : "BGRA8Unorm";
        } else if (o->bits_per_pixel == 24) {
            o->mtl_format = "INVALID_24BIT"; /* no native Metal 24-bit RGB */
        } else if (o->bits_per_pixel == 16) {
            o->mtl_format = (o->rmask == 0xf800u && o->gmask == 0x07e0u && o->bmask == 0x001fu)
                              ? "R5G6B5Unorm" : "INVALID_16BIT";
        } else {
            return DDS_ERR_FORMAT;
        }
    } else {
        return DDS_ERR_FORMAT;
    }

    o->data_offset = px;
    size_t lvl = 0; /* sum of all mip levels */
    uint32_t mw = o->width, mh = o->height;
    for (uint32_t m = 0; m < o->mips; m++) {
        size_t s = o->is_bc ? ((mw + 3) / 4) * ((mh + 3) / 4) * (o->fourcc == FOURCC_DXT1 || o->dx10_dxgi == DXGI_BC1_UNORM || o->dx10_dxgi == DXGI_BC1_UNORM_SRGB || o->fourcc == FOURCC_BC4U || o->fourcc == FOURCC_BC4S ? 8u : 16u)
                            : (size_t)mw * mh * (o->bits_per_pixel / 8);
        lvl += s * o->depth;
        if (mw > 1) mw >>= 1; if (mh > 1) mh >>= 1;
    }
    o->data_size = lvl;
    return (px + lvl <= len) ? DDS_OK : DDS_ERR_SIZE;
}

#endif /* BINC_DDS_H */
