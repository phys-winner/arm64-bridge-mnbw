#pragma once

#include <cstdint>
#include <optional>

#include <dynarmic/interface/A32/coprocessor.h>

class DynarmicHost;

// See guest_cp15.cpp: without this the first `mrc p15, 0, Rt, c13, c0, 3` the
// guest executes aborts the process from inside Dynarmic's emitter.
class GuestCp15 final : public Dynarmic::A32::Coprocessor {
public:
    GuestCp15(DynarmicHost& host, unsigned coproc_num);

    std::optional<Callback> CompileInternalOperation(bool two, unsigned opc1, Dynarmic::A32::CoprocReg CRd,
                                                     Dynarmic::A32::CoprocReg CRn,
                                                     Dynarmic::A32::CoprocReg CRm, unsigned opc2) override;
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, Dynarmic::A32::CoprocReg CRn,
                                               Dynarmic::A32::CoprocReg CRm, unsigned opc2) override;
    CallbackOrAccessTwoWords CompileSendTwoWords(bool two, unsigned opc, Dynarmic::A32::CoprocReg CRm) override;
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, Dynarmic::A32::CoprocReg CRn,
                                              Dynarmic::A32::CoprocReg CRm, unsigned opc2) override;
    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, Dynarmic::A32::CoprocReg CRm) override;
    std::optional<Callback> CompileLoadWords(bool two, bool long_transfer, Dynarmic::A32::CoprocReg CRd,
                                             std::optional<std::uint8_t> option) override;
    std::optional<Callback> CompileStoreWords(bool two, bool long_transfer, Dynarmic::A32::CoprocReg CRd,
                                              std::optional<std::uint8_t> option) override;
private:
    unsigned m_num;
};

// The guest also sets its thread pointer through the __ARM_NR_set_tls syscall,
// which has to land in the same place the coprocessor reads.
uint32_t GuestCp15CurrentTls();
void GuestCp15SetCurrentTls(uint32_t value);
