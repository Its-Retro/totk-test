#ifndef TOTK_HOST_TEXTURE_LAYOUT_H
#define TOTK_HOST_TEXTURE_LAYOUT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* One 2D mip, explicit block-height exponent. Width is bytes, height is
 * format-block rows (pixel rows for uncompressed data). No format guessing,
 * mip chains, arrays, 3D textures or compressed-format decoding here.
 * GOB addressing follows the Tegra TRM's 64-byte by 8-row layout. */
static inline int boot_texture_sizes(uint32_t width_bytes,uint32_t rows,unsigned log_height,size_t *linear,size_t *tiled){
    if(!linear || !tiled || !width_bytes || !rows || log_height>5)return 0;
    uint64_t bh=UINT64_C(1)<<log_height;
    uint64_t gx=((uint64_t)width_bytes+63)/64,gy=((uint64_t)rows+8*bh-1)/(8*bh);
    uint64_t plain=(uint64_t)width_bytes*rows;
    /* Bound the product before multiplication, even for uint32-max inputs. */
    uint64_t block_bytes=512*bh;
    if(gx>UINT64_MAX/block_bytes || gy>UINT64_MAX/(gx*block_bytes))return 0;
    uint64_t packed=gx*gy*block_bytes;
    if(plain>UINT64_C(512)*1024*1024 || packed>UINT64_C(512)*1024*1024)return 0;
    *linear=(size_t)plain;*tiled=(size_t)packed;return 1;
}

static inline int boot_texture_detile(void *destination,size_t destination_size,const void *source,size_t source_size,uint32_t width_bytes,uint32_t rows,unsigned log_height){
    size_t linear,tiled;
    if(!destination || !source || !boot_texture_sizes(width_bytes,rows,log_height,&linear,&tiled) || destination_size<linear || source_size<tiled)return 0;
    uintptr_t dst=(uintptr_t)destination,src=(uintptr_t)source;
    if(dst>UINTPTR_MAX-linear || src>UINTPTR_MAX-tiled || (dst<src+tiled && src<dst+linear))return 0;
    uint64_t bh=UINT64_C(1)<<log_height,gx=((uint64_t)width_bytes+63)/64;
    for(uint32_t y=0;y<rows;++y){
        uint64_t row_block=(y/(8*bh))*gx*512*bh+((y%(8*bh))/8)*512;
        for(uint32_t x=0;x<width_bytes;x+=16){
            uint64_t address=row_block+(x/64)*512*bh+((x&63)/32)*256+((y&7)/2)*64+((x&31)/16)*32+(y&1)*16;
            size_t count=width_bytes-x<16?width_bytes-x:16;
            memcpy((uint8_t*)destination+(size_t)y*width_bytes+x,(const uint8_t*)source+address,count);
        }
    }
    return 1;
}

/* Tiled size of a block-linear 3D level: GOBs (64 B x 8 rows x 1 slice) grouped into blocks of
 * 1 x 2^log_height x 2^log_depth GOBs; blocks run x, then y, then z. Same bounds as above. */
static inline int boot_texture_sizes_3d(uint32_t width_bytes,uint32_t rows,uint32_t depth,unsigned log_height,unsigned log_depth,size_t *linear,size_t *tiled){
    if(!linear || !tiled || !width_bytes || !rows || !depth || log_height>5 || log_depth>5)return 0;
    uint64_t block_bytes=UINT64_C(512)<<(log_height+log_depth);
    uint64_t gx=((uint64_t)width_bytes+63)/64,gy=((uint64_t)rows+(UINT64_C(8)<<log_height)-1)/(UINT64_C(8)<<log_height);
    uint64_t gz=((uint64_t)depth+(UINT64_C(1)<<log_depth)-1)>>log_depth;
    const uint64_t limit=UINT64_C(1)<<30;   /* each factor is < 2^33 and every partial product is bounded before the next multiply */
    if(width_bytes>limit || rows>limit || depth>limit)return 0;
    uint64_t plain=(uint64_t)width_bytes*rows;
    if(plain>limit || (plain*=depth)>limit)return 0;
    uint64_t packed=gx*block_bytes;
    if(packed>limit || (packed*=gy)>limit || (packed*=gz)>limit)return 0;
    *linear=(size_t)plain;*tiled=(size_t)packed;return 1;
}
/* Detiles a 3D block-linear level (Tegra X1 TRM v1.3 block-linear addressing, pp. 1217-1218, as tegra_swizzle
 * (MIT) src/swizzle.rs writes it: gob_address_x/y/z + gob_offset): byte (x, y, z) lives at
 *   (z >> bd) * slab + (z & (2^bd - 1)) * 512 * 2^bh            slab = gy * gx * block
 *   + (y >> (3 + bh)) * gx * block + ((y >> 3) & (2^bh - 1)) * 512
 *   + (x >> 6) * block + GOB-internal offset(x & 63, y & 7)      block = 512 * 2^(bh + bd)
 * With log_depth 0 and depth 1 this is boot_texture_detile. Output is width_bytes x rows x depth. */
static inline int boot_texture_detile_3d(void *destination,size_t destination_size,const void *source,size_t source_size,
                                         uint32_t width_bytes,uint32_t rows,uint32_t depth,unsigned log_height,unsigned log_depth){
    size_t linear,tiled;
    if(!destination || !source || !boot_texture_sizes_3d(width_bytes,rows,depth,log_height,log_depth,&linear,&tiled) ||
       destination_size<linear || source_size<tiled)return 0;
    uintptr_t dst=(uintptr_t)destination,src=(uintptr_t)source;
    if(dst>UINTPTR_MAX-linear || src>UINTPTR_MAX-tiled || (dst<src+tiled && src<dst+linear))return 0;
    uint64_t block=UINT64_C(512)<<(log_height+log_depth),gx=((uint64_t)width_bytes+63)/64;
    uint64_t gy=((uint64_t)rows+(UINT64_C(8)<<log_height)-1)/(UINT64_C(8)<<log_height),slab=gy*gx*block;
    uint32_t bd_mask=(1u<<log_depth)-1,bh_mask=(1u<<log_height)-1;
    for(uint32_t z=0;z<depth;++z){
        uint64_t oz=(uint64_t)(z>>log_depth)*slab+((uint64_t)(z&bd_mask)<<(9+log_height));
        for(uint32_t y=0;y<rows;++y){
            uint64_t oy=oz+(uint64_t)(y>>(3+log_height))*gx*block+((uint64_t)((y>>3)&bh_mask)<<9)+((y&7)/2)*64+(y&1)*16;
            uint8_t *line=(uint8_t*)destination+((size_t)z*rows+y)*width_bytes;
            for(uint32_t x=0;x<width_bytes;x+=16){
                uint64_t address=oy+(x/64)*block+((x&63)/32)*256+((x&31)/16)*32;
                size_t count=width_bytes-x<16?width_bytes-x:16;
                memcpy(line+x,(const uint8_t*)source+address,count);
            }
        }
    }
    return 1;
}

/* One block-linear slice of width_bytes x rows starting at `tiled` (a 2D surface, or one slice of a
 * 3D surface addressed at the slice's own offset): blocks are 2^log_height GOBs tall and 2^log_depth
 * slices deep, so rows and columns step over the other slices' GOBs. Returns the bytes from `tiled`
 * to the end of the last byte this slice occupies (0 on bad arguments). */
static inline uint64_t boot_texture_slice_extent(uint32_t width_bytes,uint32_t rows,unsigned log_height,unsigned log_depth){
    if(!width_bytes || !rows || log_height>5 || log_depth>5)return 0;
    uint64_t block=UINT64_C(512)<<(log_height+log_depth),gx=((uint64_t)width_bytes+63)/64;
    uint32_t y=rows-1;
    return (uint64_t)(y>>(3+log_height))*gx*block+((uint64_t)((y>>3)&((1u<<log_height)-1))<<9)+(gx-1)*block+512;
}
/* Reads one block-linear slice (layout as boot_texture_tile_slice) into linear rows `linear_pitch` apart. */
static inline int boot_texture_detile_slice(void *linear,size_t linear_pitch,const void *tiled,size_t tiled_size,
                                            uint32_t width_bytes,uint32_t rows,unsigned log_height,unsigned log_depth){
    uint64_t extent=boot_texture_slice_extent(width_bytes,rows,log_height,log_depth);
    if(!tiled || !linear || !extent || extent>tiled_size || linear_pitch<width_bytes)return 0;
    uint64_t block=UINT64_C(512)<<(log_height+log_depth),gx=((uint64_t)width_bytes+63)/64;
    uint32_t bh_mask=(1u<<log_height)-1;
    for(uint32_t y=0;y<rows;++y){
        uint64_t oy=(uint64_t)(y>>(3+log_height))*gx*block+((uint64_t)((y>>3)&bh_mask)<<9)+((y&7)/2)*64+(y&1)*16;
        uint8_t *line=(uint8_t*)linear+(size_t)y*linear_pitch;
        for(uint32_t x=0;x<width_bytes;x+=16){
            uint64_t address=oy+(x/64)*block+((x&63)/32)*256+((x&31)/16)*32;
            size_t count=width_bytes-x<16?width_bytes-x:16;
            memcpy(line+x,(const uint8_t*)tiled+address,count);
        }
    }
    return 1;
}
/* Writes a linear slice (rows `linear_pitch` bytes apart) into its block-linear place, touching only
 * this slice's bytes (inverse of boot_texture_detile_3d for depth 1). */
static inline int boot_texture_tile_slice(void *tiled,size_t tiled_size,const void *linear,size_t linear_pitch,
                                          uint32_t width_bytes,uint32_t rows,unsigned log_height,unsigned log_depth){
    uint64_t extent=boot_texture_slice_extent(width_bytes,rows,log_height,log_depth);
    if(!tiled || !linear || !extent || extent>tiled_size || linear_pitch<width_bytes)return 0;
    uint64_t block=UINT64_C(512)<<(log_height+log_depth),gx=((uint64_t)width_bytes+63)/64;
    uint32_t bh_mask=(1u<<log_height)-1;
    for(uint32_t y=0;y<rows;++y){
        uint64_t oy=(uint64_t)(y>>(3+log_height))*gx*block+((uint64_t)((y>>3)&bh_mask)<<9)+((y&7)/2)*64+(y&1)*16;
        const uint8_t *line=(const uint8_t*)linear+(size_t)y*linear_pitch;
        for(uint32_t x=0;x<width_bytes;x+=16){
            uint64_t address=oy+(x/64)*block+((x&63)/32)*256+((x&31)/16)*32;
            size_t count=width_bytes-x<16?width_bytes-x:16;
            memcpy((uint8_t*)tiled+address,line+x,count);
        }
    }
    return 1;
}
#endif
