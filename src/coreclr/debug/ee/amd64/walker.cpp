// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

//*****************************************************************************
//
// AMD64 instruction decoding/stepping logic
//
//*****************************************************************************

#include "stdafx.h"

#include "walker.h"

#include "frames.h"
#include "openum.h"
#include "amd64InstrDecode.h"

#ifdef TARGET_AMD64

//
// The AMD64 walker is currently pretty minimal.  It only recognizes call and return opcodes, plus a few jumps.  The rest
// is treated as unknown.
//
void NativeWalker::Decode()
{
    LOG((LF_CORDB, LL_INFO100000, "NW:Decode: m_ip 0x%p\n", m_ip));

    const BYTE *ip = m_ip;

    m_type = WALK_UNKNOWN;
    m_skipIP = NULL;
    m_nextIP = NULL;

    BYTE rex = 0;
    BYTE rex2_payload = 0;
    bool has_rex2 = false;

    BYTE prefix = *ip;
    if (prefix == 0xcc)
    {
        prefix = (BYTE)DebuggerController::GetPatchedOpcode(m_ip); // REVIEW: change `m_ip` to `ip`?
        LOG((LF_CORDB, LL_INFO100000, "NW:Decode 1st byte was patched, might have been prefix\n"));
    }

    //
    // Skip instruction prefixes
    //
    do
    {
        switch (prefix)
        {
        // Segment overrides
        case 0x26: // ES
        case 0x2E: // CS
        case 0x36: // SS
        case 0x3E: // DS
        case 0x64: // FS
        case 0x65: // GS

        // Size overrides
        case 0x66: // Operand-Size
        case 0x67: // Address-Size

        // Lock
        case 0xf0:

        // String REP prefixes
        case 0xf2: // REPNE/REPNZ
        case 0xf3:
            LOG((LF_CORDB, LL_INFO10000, "NW:Decode: prefix:%02x ", prefix));
            ip++;
            // REVIEW: it looks like a bug that we don't loop here looking for additional
            // prefixes (the 'continue' branches to the 'while (0)' which exits the loop).
            // Thus, we will only process a single prefix. For example, we won't process
            // "66 40", which is an operand size prefix followed by a REX prefix, and is legal.
            // REX and REX2 need to be the final prefixes, but even then, looping would be safe.
            continue;

        // REX register extension prefixes
        case 0x40:
        case 0x41:
        case 0x42:
        case 0x43:
        case 0x44:
        case 0x45:
        case 0x46:
        case 0x47:
        case 0x48:
        case 0x49:
        case 0x4a:
        case 0x4b:
        case 0x4c:
        case 0x4d:
        case 0x4e:
        case 0x4f:
            LOG((LF_CORDB, LL_INFO10000, "NW:Decode: REX prefix:%02x ", prefix));
            // make sure to set rex to prefix, not *ip because *ip still represents the
            // codestream which has a 0xcc in it.
            rex = prefix;
            ip++;
            continue;

        // REX2 register extension prefix
        case 0xd5:
            LOG((LF_CORDB, LL_INFO10000, "NW:Decode: REX2 prefix:%02x ", prefix));
            has_rex2 = true;
            ip++;
            rex2_payload = *ip; // Get the REX2 payload byte
            if (rex2_payload == 0xcc)
            {
                rex2_payload = (BYTE)DebuggerController::GetPatchedOpcode(ip);
                LOG((LF_CORDB, LL_INFO100000, "NW:Decode 2nd byte was patched, REX2 prefix payload byte\n"));
            }
            ip++;
            continue;

        default:
            break;
        }
    } while (0);

    // Read the opcode
    m_opcode = *ip++;

    LOG((LF_CORDB, LL_INFO100000, "NW:Decode: ip 0x%p, m_opcode:%02x\n", ip, m_opcode));

    // Don't remove this, when we did the check above for the prefix we didn't modify the codestream
    // and since m_opcode was just taken directly from the code stream it will be patched if we
    // didn't have a prefix
    if (m_opcode == 0xcc)
    {
        m_opcode = (BYTE)DebuggerController::GetPatchedOpcode(m_ip); // REVIEW: it looks like a bug that we use 'm_ip' instead of 'ip' here.
        LOG((LF_CORDB, LL_INFO100000, "NW:Decode after patch look up: m_opcode:%02x\n", m_opcode));
    }

    // Setup REX bits if needed
    BYTE rex_b = 0;
    BYTE rex_x = 0;
    BYTE rex_r = 0;

    if (rex != 0)
    {
        rex_b = (rex & 0x1);       // high bit to modrm r/m field or SIB base field or OPCODE reg field    -- Hmm, when which?
        rex_x = (rex & 0x2) >> 1;  // high bit to sib index field
        rex_r = (rex & 0x4) >> 2;  // high bit to modrm reg field
    }

    // Setup REX2 bits if needed
    BYTE rex2_b3 = 0;
    BYTE rex2_b4 = 0;
    BYTE rex2_x3 = 0;
    BYTE rex2_x4 = 0;
    BYTE rex2_r3 = 0;
    BYTE rex2_r4 = 0;

    // We could have a REX2 prefix with a zero payload byte, but that would leave these fields all zero, which is correct.
    if (rex2_payload != 0)
    {
        rex2_b3 = rex2_payload & 0x1;
        rex2_x3 = (rex2_payload >> 1) & 0x1;
        rex2_r3 = (rex2_payload >> 2) & 0x1;
        rex2_b4 = (rex2_payload >> 4) & 0x1;
        rex2_x4 = (rex2_payload >> 5) & 0x1;
        rex2_r4 = (rex2_payload >> 6) & 0x1;
    }

    // Analyze what we can of the opcode
    switch (m_opcode)
    {
        // Look for CALL, JMP with opcode 0xFF, modrm.reg=2,3,4,5
        case 0xff:
        {
            BYTE modrm = *ip++;

            BYTE mod = (modrm & 0xC0) >> 6;
            BYTE reg = (modrm & 0x38) >> 3;
            BYTE rm  = (modrm & 0x07);

            if ((reg < 2) || (reg > 5)) {
                // Not a CALL/JMP instruction (modrm.reg field is an opcode extension for opcode FF)
                return;
            }

            BYTE rm_reg = rm;
            if (rex != 0)
            {
                rm_reg |= (rex_b << 3);
            }
            else if (rex2_payload != 0)
            {
                rm_reg |= (rex2_b3 << 3) | (rex2_b4 << 4);
            }

            BYTE *result;
            WORD displace;

            // See: Tables A-15,16,17 in AMD Dev Manual 3 for information
            //      about how the ModRM/SIB/REX bytes interact.

            switch (mod)
            {
            case 0:
            case 1:
            case 2:
                if (rm == 4) // we have an SIB byte following
                {
                    //
                    // Get values from the SIB byte
                    //
                    BYTE sib = *ip;

                    _ASSERT(sib != 0);

                    BYTE ss    = (sib & 0xC0) >> 6;
                    BYTE index = (sib & 0x38) >> 3;
                    BYTE base  = (sib & 0x07);

                    BYTE index_reg = index;
                    BYTE base_reg  = base;
                    if (rex != 0)
                    {
                        index_reg |= (rex_x << 3);
                        base_reg  |= (rex_b << 3);
                    }
                    else if (rex2_payload != 0)
                    {
                        index_reg |= (rex2_x3 << 3) | (rex2_x4 << 4);
                        base_reg  |= (rex2_b3 << 3) | (rex2_b4 << 4);
                    }

                    ip++;

                    //
                    // Get starting value
                    //
                    if ((mod == 0) && (base == 5))
                    {
                        result = 0;
                    }
                    else
                    {
                        result = (BYTE *)(size_t)GetRegisterValue(base_reg);
                    }

                    //
                    // Add in the [index]
                    //
                    if (index != 0x4)
                    {
                        result = result + (GetRegisterValue(index_reg) << ss);
                    }

                    //
                    // Finally add in the offset
                    //
                    if (mod == 0)
                    {
                        if (base == 5)
                        {
                            result = result + *((INT32*)ip);
                            displace = 7;
                        }
                        else
                        {
                            displace = 3;
                        }
                    }
                    else if (mod == 1)
                    {
                        result = result + *((INT8*)ip);
                        displace = 4;
                    }
                    else // mod == 2
                    {
                        result = result + *((INT32*)ip);
                        displace = 7;
                    }
                }
                else
                {
                    //
                    // Get the value we need from the register.
                    //

                    // Check for RIP-relative addressing mode.
                    if ((mod == 0) && (rm == 5))
                    {
                        // [RIP + disp32]
                        displace = 6;   // 1 byte opcode + 1 byte modrm + 4 byte displacement (signed)
                        result = const_cast<BYTE *>(m_ip) + displace + *(reinterpret_cast<const INT32*>(ip));
                    }
                    else
                    {
                        result = (BYTE *)GetRegisterValue(rm_reg);

                        if (mod == 0)
                        {
                            // [modrm.rm]
                            displace = 2; // 1 byte opcode + 1 byte modrm
                        }
                        else if (mod == 1)
                        {
                            // [modrm.rm + disp8]
                            result = result + *((INT8*)ip);
                            displace = 3; // 1 byte opcode + 1 byte modrm + 1 byte displacement
                        }
                        else // mod == 2
                        {
                            // [modrm.rm + disp32]
                            result = result + *((INT32*)ip);
                            displace = 6; // 1 byte opcode + 1 byte modrm + 4 byte displacement (signed)
                        }
                    }
                }

                //
                // Now dereference thru the result to get the resulting IP.
                //
                result = (BYTE *)(*((UINT64*)result));

                break;

            case 3:
            default:
                // The operand is stored in a register.
                // [modrm.rm]
                result = (BYTE *)GetRegisterValue(rm_reg);
                displace = 2; // 1 byte opcode + 1 byte modrm
                break;

            }

            // the instruction uses r8-r15, add in the extra byte to the displacement
            // for the REX prefix which was used to specify the extended register
            if (rex != 0)
            {
                displace++;
            }

            if (has_rex2) // Can't just check `rex2_payload` since that payload byte might be zero.
            {
                displace += 2; // adjust for the size of the REX2 prefix
            }

            // because we already checked register validity for CALL/BRANCH
            // instructions above we can assume that there is no other option
            if ((reg == 4) || (reg == 5))
            {
                m_type = WALK_BRANCH;
            }
            else
            {
                m_type = WALK_CALL;
            }
            m_nextIP = result;
            m_skipIP = m_ip + displace;
            break;
        }
        case 0xe8:
        {
            m_type = WALK_CALL;

            // Sign-extend the displacement is necessary.
            INT32 disp = *((INT32*)ip);
            m_nextIP = ip + 4 + (disp < 0 ? (disp | 0xffffffff00000000) : disp);
            m_skipIP = ip + 4;

            break;
        }
        case 0xe9:
        {
            m_type = WALK_BRANCH;

            // Sign-extend the displacement is necessary.
            INT32 disp = *((INT32*)ip);
            m_nextIP = ip + 4 + (disp < 0 ? (disp | 0xffffffff00000000) : disp);
            m_skipIP = ip + 4;

            break;
        }
        case 0xc2:
        case 0xc3:
        case 0xca:
        case 0xcb:
        {
            m_type = WALK_RETURN;
            break;
        }
        default:
            break;
    }
}


//
// Given a regdisplay and a register number, return the value of the register.
//

UINT64 NativeWalker::GetRegisterValue(int registerNumber)
{
    if (m_registers == NULL) {
        return 0;
    }

    switch (registerNumber)
    {
        case 0:
            return m_registers->pCurrentContext->Rax;
        case 1:
            return m_registers->pCurrentContext->Rcx;
        case 2:
            return m_registers->pCurrentContext->Rdx;
        case 3:
            return m_registers->pCurrentContext->Rbx;
        case 4:
            return m_registers->pCurrentContext->Rsp;
        case 5:
            return m_registers->pCurrentContext->Rbp;
        case 6:
            return m_registers->pCurrentContext->Rsi;
        case 7:
            return m_registers->pCurrentContext->Rdi;
        case 8:
            return m_registers->pCurrentContext->R8;
        case 9:
            return m_registers->pCurrentContext->R9;
        case 10:
            return m_registers->pCurrentContext->R10;
        case 11:
            return m_registers->pCurrentContext->R11;
        case 12:
            return m_registers->pCurrentContext->R12;
        case 13:
            return m_registers->pCurrentContext->R13;
        case 14:
            return m_registers->pCurrentContext->R14;
        case 15:
            return m_registers->pCurrentContext->R15;
#if 0
        // TODO-XArch-APX: The Windows SDK doesn't define the APX eGPR registers yet.
        case 16:
            return m_registers->pCurrentContext->R16;
        case 17:
            return m_registers->pCurrentContext->R17;
        case 18:
            return m_registers->pCurrentContext->R18;
        case 19:
            return m_registers->pCurrentContext->R19;
        case 20:
            return m_registers->pCurrentContext->R21;
        case 21:
            return m_registers->pCurrentContext->R21;
        case 22:
            return m_registers->pCurrentContext->R22;
        case 23:
            return m_registers->pCurrentContext->R23;
        case 24:
            return m_registers->pCurrentContext->R24;
        case 25:
            return m_registers->pCurrentContext->R25;
        case 26:
            return m_registers->pCurrentContext->R26;
        case 27:
            return m_registers->pCurrentContext->R27;
        case 28:
            return m_registers->pCurrentContext->R28;
        case 29:
            return m_registers->pCurrentContext->R29;
        case 30:
            return m_registers->pCurrentContext->R30;
        case 31:
            return m_registers->pCurrentContext->R31;
#endif
        default:
            _ASSERTE(!"Invalid register number!");
    }

    return 0;
}


//          mod     reg     r/m
// bits     7-6     5-3     2-0
struct ModRMByte
{
    ModRMByte(BYTE init) :
        rm(init & 0x7),
        reg((init >> 3) & 0x7),
        mod(init >> 6)
    {
    }

    BYTE rm :3;
    BYTE reg:3;
    BYTE mod:2;
};

static bool InstructionHasModRMByte(Amd64InstrDecode::InstrForm form, bool W)
{
    bool modrm = true;

    switch (form)
    {
    case Amd64InstrDecode::InstrForm::None:
    case Amd64InstrDecode::InstrForm::I1B:
    case Amd64InstrDecode::InstrForm::I2B:
    case Amd64InstrDecode::InstrForm::I3B:
    case Amd64InstrDecode::InstrForm::I4B:
    case Amd64InstrDecode::InstrForm::I8B:
    case Amd64InstrDecode::InstrForm::WP_I4B_or_I4B_or_I2B:
    case Amd64InstrDecode::InstrForm::WP_I8B_or_I4B_or_I2B:
        modrm = false;
        break;
    default:
        break;
    }
    return modrm;
}

static bool InstructionIsWrite(Amd64InstrDecode::InstrForm form)
{
    bool isWrite = false;
    switch (form)
    {
    // M1st cases (memory operand comes first)
    case Amd64InstrDecode::InstrForm::M1st_I1B_L_M16B_or_M8B:
    case Amd64InstrDecode::InstrForm::M1st_I1B_LL_M8B_M16B_M32B:
    case Amd64InstrDecode::InstrForm::M1st_I1B_W_M8B_or_M2B:
    case Amd64InstrDecode::InstrForm::M1st_I1B_W_M8B_or_M4B:
    case Amd64InstrDecode::InstrForm::M1st_I4B_W_M8B_or_M4B:
    case Amd64InstrDecode::InstrForm::M1st_I1B_WP_M8B_or_M4B_or_M2B:
    case Amd64InstrDecode::InstrForm::M1st_L_M32B_or_M16B:
    case Amd64InstrDecode::InstrForm::M1st_LL_M16B_M32B_M64B:
    case Amd64InstrDecode::InstrForm::M1st_LL_M2B_M4B_M8B:
    case Amd64InstrDecode::InstrForm::M1st_LL_M4B_M8B_M16B:
    case Amd64InstrDecode::InstrForm::M1st_LL_M8B_M16B_M32B:
    case Amd64InstrDecode::InstrForm::M1st_M16B:
    case Amd64InstrDecode::InstrForm::M1st_M16B_I1B:
    case Amd64InstrDecode::InstrForm::M1st_M1B:
    case Amd64InstrDecode::InstrForm::M1st_M1B_I1B:
    case Amd64InstrDecode::InstrForm::M1st_M2B:
    case Amd64InstrDecode::InstrForm::M1st_M2B_I1B:
    case Amd64InstrDecode::InstrForm::M1st_M32B_I1B:
    case Amd64InstrDecode::InstrForm::M1st_M4B:
    case Amd64InstrDecode::InstrForm::M1st_M4B_I1B:
    case Amd64InstrDecode::InstrForm::M1st_M8B:
    case Amd64InstrDecode::InstrForm::M1st_MUnknown:
    case Amd64InstrDecode::InstrForm::M1st_W_M4B_or_M1B:
    case Amd64InstrDecode::InstrForm::M1st_W_M8B_or_M2B:
    case Amd64InstrDecode::InstrForm::M1st_W_M8B_or_M4B:
    case Amd64InstrDecode::InstrForm::M1st_W_M8B_I4B_or_M2B_I2B:
    case Amd64InstrDecode::InstrForm::M1st_WP_M8B_I4B_or_M4B_I4B_or_M2B_I2B:
    case Amd64InstrDecode::InstrForm::M1st_WP_M8B_or_M4B_or_M2B:

    // MOnly cases (memory operand is the only operand)
    case Amd64InstrDecode::InstrForm::MOnly_M10B:
    case Amd64InstrDecode::InstrForm::MOnly_M1B:
    case Amd64InstrDecode::InstrForm::MOnly_M2B:
    case Amd64InstrDecode::InstrForm::MOnly_M4B:
    case Amd64InstrDecode::InstrForm::MOnly_M8B:
    case Amd64InstrDecode::InstrForm::MOnly_MUnknown:
    case Amd64InstrDecode::InstrForm::MOnly_P_M6B_or_M4B:
    case Amd64InstrDecode::InstrForm::MOnly_W_M16B_or_M8B:
    case Amd64InstrDecode::InstrForm::MOnly_W_M8B_or_M4B:
    case Amd64InstrDecode::InstrForm::MOnly_W_M8B_or_M2B:
    case Amd64InstrDecode::InstrForm::MOnly_WP_M8B_or_M4B_or_M2B:
    case Amd64InstrDecode::InstrForm::MOnly_WP_M8B_or_M8B_or_M2B:
        isWrite = true;
        break;
    default:
        break;
    }
    return isWrite;
}

static uint8_t InstructionOperandSize(Amd64InstrDecode::InstrForm form, int pp, bool W, bool L, bool evex_b, int LL, bool fPrefix66)
{
    uint8_t opSize = 0;
    const bool P = ((pp == 1) || fPrefix66);
    switch (form)
    {
    // M32B
    case Amd64InstrDecode::InstrForm::M1st_M32B_I1B:
    case Amd64InstrDecode::InstrForm::MOp_M32B:
    case Amd64InstrDecode::InstrForm::MOp_M32B_I1B:
        opSize = 32;
        break;
    // L_M32B_or_M16B
    case Amd64InstrDecode::InstrForm::M1st_L_M32B_or_M16B:
    case Amd64InstrDecode::InstrForm::MOp_I1B_L_M32B_or_M16B:
    case Amd64InstrDecode::InstrForm::MOp_L_M32B_or_M16B:
        opSize = L ? 32 : 16;
        break;
    // L_M32B_or_M8B
    case Amd64InstrDecode::InstrForm::MOp_L_M32B_or_M8B:
        opSize = L ? 32 : 8;
        break;
    // M16B
    case Amd64InstrDecode::InstrForm::M1st_M16B:
    case Amd64InstrDecode::InstrForm::M1st_M16B_I1B:
    case Amd64InstrDecode::InstrForm::MOp_M16B:
    case Amd64InstrDecode::InstrForm::MOp_M16B_I1B:
        opSize = 16;
        break;
    // L_M16B_or_M8B
    case Amd64InstrDecode::InstrForm::M1st_I1B_L_M16B_or_M8B:
    case Amd64InstrDecode::InstrForm::MOp_L_M16B_or_M8B:
        opSize = L ? 16 : 8;
        break;
    // W_M16B_or_M8B
    case Amd64InstrDecode::InstrForm::MOnly_W_M16B_or_M8B:
        opSize = W ? 16 : 8;
        break;
    // M10B
    case Amd64InstrDecode::InstrForm::MOnly_M10B:
        opSize = 10;
        break;
    // M8B
    case Amd64InstrDecode::InstrForm::MOp_M8B:
    case Amd64InstrDecode::InstrForm::MOp_M8B_I1B:
        opSize = 8;
        break;
    // L_M8B_or_M4B
    case Amd64InstrDecode::InstrForm::MOp_L_M8B_or_M4B:
        opSize = L ? 8 : 4;
        break;
    // W_M8B_or_M4B
    case Amd64InstrDecode::InstrForm::M1st_I1B_W_M8B_or_M4B:
    case Amd64InstrDecode::InstrForm::M1st_I4B_W_M8B_or_M4B:
    case Amd64InstrDecode::InstrForm::M1st_W_M8B_or_M4B:
    case Amd64InstrDecode::InstrForm::MOnly_W_M8B_or_M4B:
    case Amd64InstrDecode::InstrForm::MOp_I1B_W_M8B_or_M4B:
    case Amd64InstrDecode::InstrForm::MOp_W_M8B_or_M4B:
        opSize = W ? 8 : 4;
        break;
    // WP_M8B_or_M8B_or_M2B
    case Amd64InstrDecode::InstrForm::MOnly_WP_M8B_or_M8B_or_M2B:
        opSize = W ? 8 : P ? 2 : 8;
        break;
    // WP_M8B_or_M4B_or_M2B
    case Amd64InstrDecode::InstrForm::M1st_I1B_WP_M8B_or_M4B_or_M2B:
    case Amd64InstrDecode::InstrForm::M1st_WP_M8B_I4B_or_M4B_I4B_or_M2B_I2B:
    case Amd64InstrDecode::InstrForm::M1st_WP_M8B_or_M4B_or_M2B:
    case Amd64InstrDecode::InstrForm::MOnly_WP_M8B_or_M4B_or_M2B:
    case Amd64InstrDecode::InstrForm::MOp_I1B_WP_M8B_or_M4B_or_M2B:
    case Amd64InstrDecode::InstrForm::MOp_WP_M8B_I4B_or_M4B_I4B_or_M2B_I2B:
    case Amd64InstrDecode::InstrForm::MOp_WP_M8B_or_M4B_or_M2B:
        opSize = W ? 8 : P ? 2 : 4;
        break;
    // W_M8B_or_M2B
    case Amd64InstrDecode::InstrForm::M1st_I1B_W_M8B_or_M2B:
    case Amd64InstrDecode::InstrForm::M1st_W_M8B_or_M2B:
    case Amd64InstrDecode::InstrForm::M1st_W_M8B_I4B_or_M2B_I2B:
    case Amd64InstrDecode::InstrForm::MOp_W_M8B_or_M2B:
    case Amd64InstrDecode::InstrForm::MOnly_W_M8B_or_M2B:
        opSize = W ? 8 : 2;
        break;
    // M8B
    case Amd64InstrDecode::InstrForm::M1st_M8B:
    case Amd64InstrDecode::InstrForm::MOnly_M8B:
        opSize = 8;
        break;
    // M6B
    case Amd64InstrDecode::InstrForm::MOp_M6B:
        opSize = 6;
        break;
    // P_M6B_or_M4B
    case Amd64InstrDecode::InstrForm::MOnly_P_M6B_or_M4B:
        opSize = P ? 4 : 6;
        break;
    // M4B
    case Amd64InstrDecode::InstrForm::M1st_M4B:
    case Amd64InstrDecode::InstrForm::M1st_M4B_I1B:
    case Amd64InstrDecode::InstrForm::MOnly_M4B:
    case Amd64InstrDecode::InstrForm::MOp_M4B:
    case Amd64InstrDecode::InstrForm::MOp_M4B_I1B:
        opSize = 4;
        break;
    // L_M4B_or_M2B
    case Amd64InstrDecode::InstrForm::MOp_L_M4B_or_M2B:
        opSize = L ? 4 : 2;
        break;
    // W_M4B_or_M1B
    case Amd64InstrDecode::InstrForm::M1st_W_M4B_or_M1B:
    case Amd64InstrDecode::InstrForm::MOp_W_M4B_or_M1B:
        opSize = W ? 4 : 1;
        break;
    // M2B
    case Amd64InstrDecode::InstrForm::M1st_M2B:
    case Amd64InstrDecode::InstrForm::M1st_M2B_I1B:
    case Amd64InstrDecode::InstrForm::MOnly_M2B:
    case Amd64InstrDecode::InstrForm::MOp_M2B:
    case Amd64InstrDecode::InstrForm::MOp_M2B_I1B:
        opSize = 2;
        break;
    // M1B
    case Amd64InstrDecode::InstrForm::M1st_M1B:
    case Amd64InstrDecode::InstrForm::M1st_M1B_I1B:
    case Amd64InstrDecode::InstrForm::MOnly_M1B:
    case Amd64InstrDecode::InstrForm::MOp_M1B:
    case Amd64InstrDecode::InstrForm::MOp_M1B_I1B:
        opSize = 1;
        break;

    // LL_M8B_M16B_M32B
    case Amd64InstrDecode::InstrForm::M1st_I1B_LL_M8B_M16B_M32B:
    case Amd64InstrDecode::InstrForm::M1st_LL_M8B_M16B_M32B:
    case Amd64InstrDecode::InstrForm::MOp_LL_M8B_M16B_M32B:
        opSize = (LL == 0) ? 8 : (LL == 1) ? 16 : 32;
        break;

    // LL_M2B_M4B_M8B
    case Amd64InstrDecode::InstrForm::M1st_LL_M2B_M4B_M8B:
    case Amd64InstrDecode::InstrForm::MOp_LL_M2B_M4B_M8B:
        opSize = (LL == 0) ? 2 : (LL == 1) ? 4 : 8;
        break;

    // LL_M4B_M8B_M16B
    case Amd64InstrDecode::InstrForm::M1st_LL_M4B_M8B_M16B:
    case Amd64InstrDecode::InstrForm::MOp_LL_M4B_M8B_M16B:
        opSize = (LL == 0) ? 4 : (LL == 1) ? 8 : 16;
        break;

    // LL_M8B_M32B_M64B
    case Amd64InstrDecode::InstrForm::MOp_LL_M8B_M32B_M64B:
        opSize = (LL == 0) ? 8 : (LL == 1) ? 32 : 64;
        break;

    // LL_M16B_M32B_M64B
    case Amd64InstrDecode::InstrForm::M1st_LL_M16B_M32B_M64B:
    case Amd64InstrDecode::InstrForm::MOp_LL_M16B_M32B_M64B:
        opSize = (LL == 0) ? 16 : (LL == 1) ? 32 : 64;
        break;

    // bLL_M2B_M16B_M32B_M64B
    case Amd64InstrDecode::InstrForm::MOp_I1B_bLL_M2B_M16B_M32B_M64B:
        if (evex_b)
        {
            opSize = 2;
        }
        else
        {
            opSize = (LL == 0) ? 16 : (LL == 1) ? 32 : 64;
        }
        break;

    // bLL_M4B_M16B_M32B_M64B
    case Amd64InstrDecode::InstrForm::MOp_I1B_bLL_M4B_M16B_M32B_M64B:
    case Amd64InstrDecode::InstrForm::MOp_bLL_M4B_M16B_M32B_M64B:
        if (evex_b)
        {
            opSize = 4;
        }
        else
        {
            opSize = (LL == 0) ? 16 : (LL == 1) ? 32 : 64;
        }
        break;

    // bLL_M8B_M16B_M32B_M64B
    case Amd64InstrDecode::InstrForm::MOp_I1B_bLL_M8B_M16B_M32B_M64B:
    case Amd64InstrDecode::InstrForm::MOp_bLL_M8B_M16B_M32B_M64B:
        if (evex_b)
        {
            opSize = 8;
        }
        else
        {
            opSize = (LL == 0) ? 16 : (LL == 1) ? 32 : 64;
        }
        break;

    // bLL_M8B_None_M32B_M64B
    case Amd64InstrDecode::InstrForm::MOp_I1B_bLL_M8B_None_M32B_M64B:
        if (evex_b)
        {
            opSize = 8;
        }
        else
        {
            // We should never see LL == 0.
            opSize = (LL == 0) ? 0 : (LL == 1) ? 32 : 64;
        }
        break;

    // bLL_M4B_M8B_M16B_M32B
    case Amd64InstrDecode::InstrForm::MOp_bLL_M4B_M8B_M16B_M32B:
        if (evex_b)
        {
            opSize = 4;
        }
        else
        {
            opSize = (LL == 0) ? 8 : (LL == 1) ? 16 : 32;
        }
        break;

    // WbLL_M8B_M16B_M32B_M64B_or_M4B_M8B_M16B_M32B
    case Amd64InstrDecode::InstrForm::MOp_WbLL_M8B_M16B_M32B_M64B_or_M4B_M8B_M16B_M32B:
        if (W)
        {
            if (evex_b)
            {
                opSize = 8;
            }
            else
            {
                opSize = (LL == 0) ? 16 : (LL == 1) ? 32 : 64;
            }
        }
        else
        {
            if (evex_b)
            {
                opSize = 4;
            }
            else
            {
                opSize = (LL == 0) ? 8 : (LL == 1) ? 16 : 32;
            }
        }
        break;

    // bWLL_M4B_M8B_M16B_M32B_M64B
    case Amd64InstrDecode::InstrForm::MOp_I1B_bWLL_M4B_M8B_M16B_M32B_M64B:
    case Amd64InstrDecode::InstrForm::MOp_bWLL_M4B_M8B_M16B_M32B_M64B:
        if (evex_b)
        {
            if (W)
            {
                opSize = 8;
            }
            else
            {
                opSize = 4;
            }
        }
        else
        {
            opSize = (LL == 0) ? 16 : (LL == 1) ? 32 : 64;
        }
        break;

    // bWLL_M4B_M8B_None_M32B_M64B
    case Amd64InstrDecode::InstrForm::MOp_I1B_bWLL_M4B_M8B_None_M32B_M64B:
    case Amd64InstrDecode::InstrForm::MOp_bWLL_M4B_M8B_None_M32B_M64B:
        if (evex_b)
        {
            if (W)
            {
                opSize = 8;
            }
            else
            {
                opSize = 4;
            }
        }
        else
        {
            // We should never see LL == 0.
            opSize = (LL == 0) ? 0 : (LL == 1) ? 32 : 64;
        }
        break;

    // MUnknown
    case Amd64InstrDecode::InstrForm::M1st_MUnknown:
    case Amd64InstrDecode::InstrForm::MOnly_MUnknown:
    case Amd64InstrDecode::InstrForm::MOp_MUnknown:
        // These are not expected/supported. Most/all are not for user code.
        _ASSERT(false);
        break;
    default:
        break;
    }
    return opSize;
}

static int InstructionImmSize(Amd64InstrDecode::InstrForm form, int pp, bool W, bool fPrefix66)
{
    int immSize = 0;
    const bool P = ((pp == 1) || fPrefix66);
    switch (form)
    {
    case Amd64InstrDecode::InstrForm::I1B:
    case Amd64InstrDecode::InstrForm::M1st_I1B_L_M16B_or_M8B:
    case Amd64InstrDecode::InstrForm::M1st_I1B_W_M8B_or_M4B:
    case Amd64InstrDecode::InstrForm::M1st_I1B_WP_M8B_or_M4B_or_M2B:
    case Amd64InstrDecode::InstrForm::M1st_M1B_I1B:
    case Amd64InstrDecode::InstrForm::M1st_M2B_I1B:
    case Amd64InstrDecode::InstrForm::M1st_M4B_I1B:
    case Amd64InstrDecode::InstrForm::M1st_M16B_I1B:
    case Amd64InstrDecode::InstrForm::M1st_M32B_I1B:
    case Amd64InstrDecode::InstrForm::MOp_I1B_L_M32B_or_M16B:
    case Amd64InstrDecode::InstrForm::MOp_I1B_W_M8B_or_M4B:
    case Amd64InstrDecode::InstrForm::MOp_I1B_WP_M8B_or_M4B_or_M2B:
    case Amd64InstrDecode::InstrForm::MOp_M1B_I1B:
    case Amd64InstrDecode::InstrForm::MOp_M2B_I1B:
    case Amd64InstrDecode::InstrForm::MOp_M4B_I1B:
    case Amd64InstrDecode::InstrForm::MOp_M8B_I1B:
    case Amd64InstrDecode::InstrForm::MOp_M16B_I1B:
    case Amd64InstrDecode::InstrForm::MOp_M32B_I1B:
    case Amd64InstrDecode::InstrForm::M1st_I1B_LL_M8B_M16B_M32B:
    case Amd64InstrDecode::InstrForm::MOp_I1B_bLL_M2B_M16B_M32B_M64B:
    case Amd64InstrDecode::InstrForm::MOp_I1B_bLL_M4B_M16B_M32B_M64B:
    case Amd64InstrDecode::InstrForm::MOp_I1B_bLL_M8B_M16B_M32B_M64B:
    case Amd64InstrDecode::InstrForm::MOp_I1B_bLL_M8B_None_M32B_M64B:
    case Amd64InstrDecode::InstrForm::MOp_I1B_bWLL_M4B_M8B_M16B_M32B_M64B:
    case Amd64InstrDecode::InstrForm::MOp_I1B_bWLL_M4B_M8B_None_M32B_M64B:
        immSize = 1;
        break;
    case Amd64InstrDecode::InstrForm::I2B:
        immSize = 2;
        break;
    case Amd64InstrDecode::InstrForm::I3B:
        immSize = 3;
        break;
    case Amd64InstrDecode::InstrForm::I4B:
    case Amd64InstrDecode::InstrForm::M1st_I4B_W_M8B_or_M4B:
        immSize = 4;
        break;
    case Amd64InstrDecode::InstrForm::I8B:
        immSize = 8;
        break;
    case Amd64InstrDecode::InstrForm::M1st_WP_M8B_I4B_or_M4B_I4B_or_M2B_I2B:
    case Amd64InstrDecode::InstrForm::MOp_WP_M8B_I4B_or_M4B_I4B_or_M2B_I2B:
    case Amd64InstrDecode::InstrForm::WP_I4B_or_I4B_or_I2B:
        immSize = W ? 4 : P ? 2 : 4;
        break;
    case Amd64InstrDecode::InstrForm::WP_I8B_or_I4B_or_I2B:
        immSize = W ? 8 : P ? 2 : 4;
        break;
    case Amd64InstrDecode::InstrForm::M1st_W_M8B_I4B_or_M2B_I2B:
        immSize = W ? 4 : 2;
        break;

    default:
        break;
    }
    return immSize;
}

// static
void NativeWalker::DecodeInstructionForPatchSkip(const BYTE *address, InstructionAttribute * pInstrAttrib)
{
    LOG((LF_CORDB, LL_INFO10000, "Patch decode: "));

    if (pInstrAttrib == NULL)
    {
        return;
    }

    pInstrAttrib->Reset();

    // These three legacy prefixes are used to modify some of the two-byte opcodes.
    bool fPrefix66 = false;
    bool fPrefixF2 = false;
    bool fPrefixF3 = false;

    bool W       = false;
    bool L       = false;
    bool evex_b  = false;
    BYTE evex_LL = 0;

    int pp = 0;

    const BYTE* originalAddr = address;

    // Code below doesn't handle patched opcodes
    _ASSERT((*address != 0xcc) || ((BYTE)DebuggerController::GetPatchedOpcode(address) == 0xcc));

    //
    // Skip instruction prefixes
    //
    do
    {
        bool done = false;
        switch (*address)
        {
        // Operand-Size override
        case 0x66:
            fPrefix66 = true;
            break;

        // Repeat (REP/REPE/REPZ)
        case 0xf2:
            fPrefixF2 = true;
            break;

        // Repeat (REPNE/REPNZ)
        case 0xf3:
            fPrefixF3 = true;
            break;

        // Address-Size override
        case 0x67:          // fall through

        // Segment overrides
        case 0x26: // ES
        case 0x2E: // CS
        case 0x36: // SS
        case 0x3E: // DS
        case 0x64: // FS
        case 0x65: // GS    // fall through

        // Lock
        case 0xf0:
            break;

        // REX register extension prefixes w/o W
        case 0x40:
        case 0x41:
        case 0x42:
        case 0x43:
        case 0x44:
        case 0x45:
        case 0x46:
        case 0x47:
            break;

        // REX register extension prefixes with W
        case 0x48:
        case 0x49:
        case 0x4a:
        case 0x4b:
        case 0x4c:
        case 0x4d:
        case 0x4e:
        case 0x4f:
            W = true;
            break;

        default:
            done = true;
            break;
        }

        if (done)
            break;
        LOG((LF_CORDB, LL_INFO10000, "prefix:%0.2x ", *address));
        address++;
    } while (true);

    // See "AMD64 Architecture Programmer's Manual Volume 3 Rev 3.26 Figure 1-1 Instruction encoding syntax"
    enum OpcodeMap
    {
        Primary = 0x0,
        Secondary = 0xF,
        Escape0F_38 = 0x0F38,
        Escape0F_3A = 0x0F3A,
        VexMapC40F = 0xc401,
        VexMapC40F38 = 0xc402,
        VexMapC40F3A = 0xc403,
        EvexMap0F = 0x6201,
        EvexMap0F38 = 0x6202,
        EvexMap0F3A = 0x6203,
        EvexMap4 = 0x6204
    } opCodeMap;

    switch (*address)
    {
        case 0xf:
        {
            switch (address[1])
            {
            case 0x38:
                LOG((LF_CORDB, LL_INFO10000, "map:0F38 "));
                opCodeMap = Escape0F_38;
                address += 2;
                break;
            case 0x3A:
                LOG((LF_CORDB, LL_INFO10000, "map:0F3A "));
                opCodeMap = Escape0F_3A;
                address += 2;
                break;
            default:
                LOG((LF_CORDB, LL_INFO10000, "map:0F "));
                opCodeMap = Secondary;
                address += 1;
                break;
            }

            if (fPrefix66)
                pp = 0x1;

            if (fPrefixF2)
                pp = 0x3;
            else if (fPrefixF3)
                pp = 0x2;
            break;
        }

        case 0xc4: // Vex 3-byte
        {
            BYTE vex_mmmmm = address[1] & 0x1f;
            opCodeMap = (OpcodeMap)(int(address[0]) << 8 | vex_mmmmm);

            switch (opCodeMap)
            {
            case VexMapC40F:
                LOG((LF_CORDB, LL_INFO10000, "map:Vex0F "));
                break;
            case VexMapC40F38:
                LOG((LF_CORDB, LL_INFO10000, "map:Vex0F38 "));
                break;
            case VexMapC40F3A:
                LOG((LF_CORDB, LL_INFO10000, "map:Vex0F3A "));
                break;
            default:
                LOG((LF_CORDB, LL_INFO10000, "ILLEGAL vex_mmmmm value! "));
                break;
            }

            // W is the top bit of opcode2.
            if ((address[2] & 0x80) != 0)
            {
                W = true;
            }
            if ((address[2] & 0x04) != 0)
            {
                L = true;
            }

            pp = address[2] & 0x3;
            address += 3;
            break;
        }

        case 0xc5: // Vex 2-byte
            opCodeMap = VexMapC40F;
            LOG((LF_CORDB, LL_INFO10000, "map:VexC5 (Vex0F) "));

            W = true;
            if ((address[1] & 0x04) != 0)
            {
                L = true;
            }

            pp = address[1] & 0x3;
            address += 2;
            break;

        case 0x62: // Evex
        {
            BYTE evex_p0 = address[1];
            BYTE evex_p1 = address[2];
            BYTE evex_p2 = address[3];
            BYTE evex_mmm = evex_p0 & 0x7;
            switch (evex_mmm)
            {
            case 0x1:
                LOG((LF_CORDB, LL_INFO10000, "map:Evex0F "));
                opCodeMap = EvexMap0F;
                break;
            case 0x2:
                LOG((LF_CORDB, LL_INFO10000, "map:Evex0F38 "));
                opCodeMap = EvexMap0F38;
                break;
            case 0x3:
                LOG((LF_CORDB, LL_INFO10000, "map:Evex0F3A "));
                opCodeMap = EvexMap0F3A;
                break;
            case 0x4:
                LOG((LF_CORDB, LL_INFO10000, "map:Evex4 "));
                opCodeMap = EvexMap4;
                break;
            default:
                _ASSERT(!"Unknown Evex 'mmm' bytes");
                return;
            }

            BYTE evex_w = evex_p1 & 0x80;
            if (evex_w != 0)
            {
                W = true;
            }

            if (evex_mmm != 4)
            {
                if ((evex_p2 & 0x10) != 0)
                {
                    evex_b = true;
                }

                evex_LL = (evex_p2 >> 5) & 0x3;
            }

            pp = evex_p1 & 0x3;
            address += 4;
            break;
        }

        case 0xD5: // REX2
        {
            BYTE rex2_byte1 = address[1];
            address += 2;

            BYTE rex2_w = rex2_byte1 & 0x08;
            if (rex2_w != 0)
            {
                W = true;
            }

            if (fPrefix66)
            {
                pp = 0x1;
            }

            BYTE rex2_m0 = rex2_byte1 & 0x80;
            if (rex2_m0 == 0)
            {
                opCodeMap = Primary;
            }
            else
            {
                opCodeMap = Secondary;
            }
            break;
        }

        default:
            opCodeMap = Primary;
            break;
    }

    Amd64InstrDecode::InstrForm form = Amd64InstrDecode::InstrForm::None;
    
    size_t opCode    = size_t(*address);
    size_t opCodeExt = (opCode << 2) | pp;

    LOG((LF_CORDB, LL_INFO10000, "opCode:%02x pp:%d W:%d L:%d LL:%d ", opCode, pp, W ? 1 : 0, L ? 1 : 0, evex_LL));

    switch (opCodeMap)
    {
    case Primary:
        form = Amd64InstrDecode::instrFormPrimary[opCode]; // NOTE: instrFormPrimary is the only map that uses 'opCode', not 'opCodeExt'.
        break;
    case Secondary:
        form = Amd64InstrDecode::instrFormSecondary[opCodeExt];
        break;
    case Escape0F_38:
        form = Amd64InstrDecode::instrFormF38[opCodeExt];
        break;
    case Escape0F_3A:
        form = Amd64InstrDecode::instrFormF3A[opCodeExt];
        break;
    case VexMapC40F:
        form = Amd64InstrDecode::instrFormVex1[opCodeExt];
        break;
    case VexMapC40F38:
        form = Amd64InstrDecode::instrFormVex2[opCodeExt];
        break;
    case VexMapC40F3A:
        form = Amd64InstrDecode::instrFormVex3[opCodeExt];
        break;
    case EvexMap0F:
        form = Amd64InstrDecode::instrFormEvex_0F[opCodeExt];
        break;
    case EvexMap0F38:
        form = Amd64InstrDecode::instrFormEvex_0F38[opCodeExt];
        break;
    case EvexMap0F3A:
        form = Amd64InstrDecode::instrFormEvex_0F3A[opCodeExt];
        break;
    case EvexMap4:
        form = Amd64InstrDecode::instrFormEvex_4[opCodeExt];
        break;
    default:
        _ASSERTE(false);
    }

    bool fModRM = InstructionHasModRMByte(form, W);
    ModRMByte modrm = ModRMByte(address[1]);
    LOG((LF_CORDB, LL_INFO10000, "modrm .mod:%d .reg:%d .rm:%d form:%d ", modrm.mod, modrm.reg, modrm.rm, form));

    if (fModRM && (modrm.mod == 0x0) && (modrm.rm == 0x5))
    {
        // RIP-relative addressing.
        if (form & Amd64InstrDecode::InstrForm::Extension)
        {
            form = Amd64InstrDecode::instrFormExtension[(size_t(form ^ Amd64InstrDecode::InstrForm::Extension) << 3) | modrm.reg];
        }

        pInstrAttrib->m_dwOffsetToDisp = (DWORD)(address - originalAddr) + 1 /* op */ + 1 /* modrm */;
        _ASSERTE(pInstrAttrib->m_dwOffsetToDisp <= MAX_INSTRUCTION_LENGTH);

        const int dispBytes = 4;
        const int immBytes = InstructionImmSize(form, pp, W, fPrefix66);

        pInstrAttrib->m_cbInstr = pInstrAttrib->m_dwOffsetToDisp + dispBytes + immBytes;
        _ASSERTE(pInstrAttrib->m_cbInstr <= MAX_INSTRUCTION_LENGTH);

        pInstrAttrib->m_fIsWrite = InstructionIsWrite(form);
        pInstrAttrib->m_cOperandSize = InstructionOperandSize(form, pp, W, L, evex_b, evex_LL, fPrefix66);

        LOG((LF_CORDB, LL_INFO10000, "cb:%d o2disp:%d write:%s immBytes:%d opSize:%d ",
            pInstrAttrib->m_cbInstr,
            pInstrAttrib->m_dwOffsetToDisp,
            pInstrAttrib->m_fIsWrite ? "true" : "false",
            immBytes,
            pInstrAttrib->m_cOperandSize));
    }

    if (opCodeMap == Primary)
    {
        BYTE opcode0 = *address;
        //
        // Look at opcode to tell if it's a call or an absolute branch.
        //
        switch (opcode0)
        {
            case 0xC2: // RET
            case 0xC3: // RET N
                pInstrAttrib->m_fIsAbsBranch = true;
                LOG((LF_CORDB, LL_INFO10000, "ABS:%0.2x\n", opcode0));
                break;

            case 0xE8: // CALL relative
                pInstrAttrib->m_fIsCall = true;
                LOG((LF_CORDB, LL_INFO10000, "CALL REL:%0.2x\n", opcode0));
                break;

            case 0xC8: // ENTER
                pInstrAttrib->m_fIsCall = true;
                pInstrAttrib->m_fIsAbsBranch = true;
                LOG((LF_CORDB, LL_INFO10000, "CALL ABS:%0.2x\n", opcode0));
                break;

            case 0xFF: // CALL/JMP modr/m
                //
                // Read opcode modifier from modr/m
                //

                _ASSERTE(fModRM);
                switch (modrm.reg)
                {
                    case 2:
                    case 3:
                        pInstrAttrib->m_fIsCall = true;
                        FALLTHROUGH;
                    case 4:
                    case 5:
                        pInstrAttrib->m_fIsAbsBranch = true;
                        break;
                }
                LOG((LF_CORDB, LL_INFO10000, "CALL/JMP modr/m:%0.2x\n", opcode0));
                break;

            default:
                LOG((LF_CORDB, LL_INFO10000, "NORMAL:%0.2x\n", opcode0));
                break;
        }
    }
    else
    {
        LOG((LF_CORDB, LL_INFO10000, "\n"));
    }
}

#endif // TARGET_AMD64
