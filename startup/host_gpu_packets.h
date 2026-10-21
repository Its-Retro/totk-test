#ifndef TOTK_HOST_GPU_PACKETS_H
#define TOTK_HOST_GPU_PACKETS_H
#include <stdint.h>
#include <stddef.h>
typedef struct BootGpuMethod {uint32_t method,value,subchannel,last,packet_word;} BootGpuMethod;
/* Decode only. A validation pass prevents partial output on a bad packet.
 * No register state, memory, queue or fence is changed here. */
static inline int boot_gpu_decode_packets(const uint32_t*words,size_t length,BootGpuMethod*out,size_t capacity,size_t*count,size_t*fault){
    if(!count || !fault)return 0;
    *count=0;*fault=0;
    if(!words || length>1048576)return 0;
    size_t required=0;
    for(unsigned pass=0;pass<2;++pass){
        size_t pos=0,written=0;
        while(pos<length){
            size_t start=pos;uint32_t header=words[pos++];
            if(!header)continue;
            unsigned op=header>>29,method=header&4095,sub=(header>>13)&7,n=(header>>16)&8191;
            *fault=start;
            if((header&4096) || (op!=1 && op!=3 && op!=4 && op!=5))return 0;
            unsigned values=op==4?1:n;
            if(op!=4 && values>length-pos)return 0;
            unsigned increment=op==1?(values?values-1:0):op==5 && values>1?1:0;
            if(increment>4095-method)return 0;
            if(pass){
                for(unsigned i=0;i<values;++i){
                    unsigned address=method+(op==1?i:op==5 && i?1:0);
                    out[written+i]=(BootGpuMethod){address*4,op==4?n:words[pos+i],sub,i+1==values,(uint32_t)start};
                }
            }
            written+=values;if(op!=4)pos+=values;
        }
        if(!pass){required=written;if(required>capacity || (required && !out)){*count=required;return 0;}}
    }
    *count=required;*fault=length;return 1;
}
#endif
