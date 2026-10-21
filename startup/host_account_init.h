#ifndef TOTK_HOST_ACCOUNT_INIT_H
#define TOTK_HOST_ACCOUNT_INIT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* assets/control/control.nacp offset 0x3026; SHA-256
 * 7b71e61c545c66631707c6ac613b5e55691d0289a5e05a8228deeecbe8103ff4. */
#define BOOT_ACCOUNT_SWITCH_LOCK 0
/* Bounded local process binding. This does not create/select a user, link a
 * Nintendo account, authenticate online, or claim network-service access. */
static inline uint32_t boot_account_word(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static inline void boot_account_put(uint8_t*p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static inline int boot_account_init_plan(const uint8_t*data,size_t size,uint32_t object,uint64_t process,uint8_t reply[56]){
    if(size!=76 || !object || object>16 || process!=1 ||
       (boot_account_word(data)!=4 && boot_account_word(data)!=6) || boot_account_word(data+4)!=UINT32_C(0x8000000e) ||
       boot_account_word(data+8)!=1 || boot_account_word(data+32)!=UINT32_C(0x00180001) ||
       boot_account_word(data+36)!=object || boot_account_word(data+40) || boot_account_word(data+44) ||
       boot_account_word(data+48)!=UINT32_C(0x49434653) || boot_account_word(data+52)!=(boot_account_word(data)==6?1u:0u) ||
       boot_account_word(data+56)!=160 || boot_account_word(data+64) || boot_account_word(data+68))return 0;
    /* Bytes 12..19 are replaced by the kernel PID; the trusted process binding
     * above is authoritative. Padding outside the domain payload is ignored. */
    uint8_t result[56]={0};boot_account_put(result+4,12);boot_account_put(result+32,UINT32_C(0x4f434653));
    boot_account_put(result+36,boot_account_word(data+52));boot_account_put(result+44,boot_account_word(data+60));
    memcpy(reply,result,sizeof(result));return 1;
}
#endif
