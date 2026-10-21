"""AdvSIMD structure loads and stores for the AOT translator, decoded from the instruction word.

emit_simd_mem(i, pc, out, Unsupported) owns every AdvSIMD load/store (multiple structures)
and (single structure) encoding (Arm ARM C4.1.x: LD1-LD4/ST1-ST4 with 1-4 registers,
single-lane LDn/STn, LDnR replicate), in all three addressing forms: no offset, immediate
post-index and register post-index. It returns False for any other instruction.

Semantics follow the Arm ARM pseudocode (LoadStoreMultipleStructures /
LoadStoreSingleStructure): the address is computed once from Rn (SP allowed), every byte
of the transfer is checked by one boot_access over the contiguous range, multiple-structure
loads write full registers (64-bit forms clear bits 64..127), single-lane loads keep the
other lanes, replicate loads fill the datasize and clear above it, stores invalidate a
matching exclusive monitor, and the base is written back after the transfer.
"""
def _base(rn):
    return ('c->sp', True) if rn == 31 else (f'c->x[{rn}]', False)


def emit_simd_mem(i, pc, out, Unsupported):
    word = int.from_bytes(i.bytes, 'little')
    # Multiple structures: 0 Q 001100 0 p L 0 ... (bit 23 = post-index, bits 21..16 Rm / 0)
    multiple = (word & 0xbf800000) in (0x0c000000, 0x0c800000) and not (word >> 21) & 1
    single = (word & 0xbf800000) in (0x0d000000, 0x0d800000)
    if not (multiple or single):
        return False
    q = (word >> 30) & 1
    load = (word >> 22) & 1
    post = (word >> 23) & 1
    rm = (word >> 16) & 31
    rn = (word >> 5) & 31
    rt = word & 31
    size = (word >> 10) & 3
    if not post and rm != 0:
        raise Unsupported('AdvSIMD structure: nonzero Rm without post-index')
    base, uses_sp = _base(rn)
    if uses_sp:
        out.append(f'if(!boot_sp(c,{pc}))return c->status;')
    if multiple:
        opcode = (word >> 12) & 15
        table = {0b0000: (1, 4), 0b0010: (4, 1), 0b0100: (1, 3), 0b0110: (3, 1),
                 0b0111: (1, 1), 0b1000: (1, 2), 0b1010: (2, 1)}
        if opcode not in table:
            raise Unsupported('AdvSIMD multiple structures: reserved opcode')
        rpt, selem = table[opcode]
        if size == 3 and q == 0 and selem != 1:
            raise Unsupported('AdvSIMD multiple structures: reserved size')
        ebytes = 1 << size
        datasize = 16 if q else 8
        elements = datasize // ebytes
        total = rpt * selem * datasize
        if load:
            stmt = [f'{{uint64_t address={base};uint8_t*p=boot_access_read(c,address,{total},{pc});if(!p)return c->status;']
        else:
            stmt = [f'{{uint64_t address={base};uint8_t p[{total}];']
        if load:
            # Stage results so a register list that repeats the base never reads half-written state.
            stmt.append(f'uint8_t staged[{rpt * selem}][16]={{{{0}}}};')
            stmt.append(f'for(unsigned r=0;r<{rpt};++r)for(unsigned e=0;e<{elements};++e)for(unsigned s=0;s<{selem};++s)'
                        f'memcpy(staged[r+s]+e*{ebytes},p+((r*{elements}+e)*{selem}+s)*{ebytes},{ebytes});')
            stmt.append(f'for(unsigned r=0;r<{rpt * selem};++r)memcpy(c->q[({rt}+r)%32],staged[r],16);')
        else:
            stmt.append(f'for(unsigned r=0;r<{rpt};++r)for(unsigned e=0;e<{elements};++e)for(unsigned s=0;s<{selem};++s)'
                        f'memcpy(p+((r*{elements}+e)*{selem}+s)*{ebytes},c->q[({rt}+r+s)%32]+e*{ebytes},{ebytes});')
            stmt.append(f'if(!boot_store_bytes(c,address,{total},{pc},p))return c->status;')
        if post:
            step = f'UINT64_C({total})' if rm == 31 else f'c->x[{rm}]'
            stmt.append(f'{base}=address+{step};')
        stmt.append('}')
        out.extend(stmt)
        return True
    # Single structure: opcode bits 15..13, S bit 12, size 11..10, R bit 21.
    opcode = (word >> 13) & 7
    s_bit = (word >> 12) & 1
    r_bit = (word >> 21) & 1
    selem = ((opcode & 1) << 1 | r_bit) + 1
    scale = opcode >> 1
    replicate = False
    if scale == 3:
        if not load or s_bit:
            raise Unsupported('AdvSIMD single structure: reserved replicate form')
        replicate = True
        scale = size
    elif scale == 1:
        if size & 1:
            raise Unsupported('AdvSIMD single structure: reserved halfword size')
        index = (q << 2) | (s_bit << 1) | (size >> 1)
    elif scale == 2:
        if size & 2:
            raise Unsupported('AdvSIMD single structure: reserved word size')
        if size & 1:
            if s_bit:
                raise Unsupported('AdvSIMD single structure: reserved doubleword S')
            scale = 3
            index = q
        else:
            index = (q << 1) | s_bit
    else:
        index = (q << 3) | (s_bit << 2) | size
    ebytes = 1 << scale
    total = selem * ebytes
    if load:
        stmt = [f'{{uint64_t address={base};uint8_t*p=boot_access_read(c,address,{total},{pc});if(!p)return c->status;']
    else:
        stmt = [f'{{uint64_t address={base};uint8_t p[{total}];']
    if replicate:
        datasize = 16 if q else 8
        stmt.append(f'for(unsigned s=0;s<{selem};++s){{uint8_t value[16]={{0}};'
                    f'for(unsigned e=0;e<{datasize // ebytes};++e)memcpy(value+e*{ebytes},p+s*{ebytes},{ebytes});'
                    f'memcpy(c->q[({rt}+s)%32],value,16);}}')
    elif load:
        stmt.append(f'for(unsigned s=0;s<{selem};++s)memcpy(c->q[({rt}+s)%32]+{index * ebytes},p+s*{ebytes},{ebytes});')
    else:
        stmt.append(f'for(unsigned s=0;s<{selem};++s)memcpy(p+s*{ebytes},c->q[({rt}+s)%32]+{index * ebytes},{ebytes});')
        stmt.append(f'if(!boot_store_bytes(c,address,{total},{pc},p))return c->status;')
    if post:
        step = f'UINT64_C({total})' if rm == 31 else f'c->x[{rm}]'
        stmt.append(f'{base}=address+{step};')
    stmt.append('}')
    out.extend(stmt)
    return True
