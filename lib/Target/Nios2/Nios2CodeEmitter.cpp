//===-- Nios2/Nios2CodeEmitter.cpp - Convert Nios2 Code to Machine Code ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===---------------------------------------------------------------------===//
//
// This file contains the pass that transforms the Nios2 machine instructions
// into relocatable machine code.
//
//===---------------------------------------------------------------------===//

#define DEBUG_TYPE "jit"
#include "Nios2.h"
#include "MCTargetDesc/Nios2BaseInfo.h"
#include "Nios2InstrInfo.h"
#include "Nios2Relocations.h"
#include "Nios2Subtarget.h"
#include "Nios2TargetMachine.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/JITCodeEmitter.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/PassManager.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#ifndef NDEBUG
#include <iomanip>
#endif

using namespace llvm;

STATISTIC(NumEmitted, "Number of machine instructions emitted");

namespace {

class Nios2CodeEmitter : public MachineFunctionPass {
  const Nios2InstrInfo *II;
  const DataLayout *TD;
  const Nios2Subtarget *Subtarget;
  TargetMachine &TM;
  JITCodeEmitter &MCE;
  const std::vector<MachineConstantPoolEntry> *MCPEs;
  const std::vector<MachineJumpTableEntry> *MJTEs;
  bool IsPIC;

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<MachineModuleInfo> ();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  static char ID;

  public:
    Nios2CodeEmitter(TargetMachine &tm, JITCodeEmitter &mce) :
      MachineFunctionPass(ID),
      II((const Nios2InstrInfo *) tm.getInstrInfo()),
      TD(tm.getDataLayout()), TM(tm), MCE(mce), MCPEs(0), MJTEs(0),
      IsPIC(TM.getRelocationModel() == Reloc::PIC_) {
    }

    bool runOnMachineFunction(MachineFunction &MF);

    virtual const char *getPassName() const {
      return "Nios2 Machine Code Emitter";
    }

    /// getBinaryCodeForInstr - This function, generated by the
    /// CodeEmitterGenerator using TableGen, produces the binary encoding for
    /// machine instructions.
    uint64_t getBinaryCodeForInstr(const MachineInstr &MI) const;

    void emitInstruction(const MachineInstr &MI);

  private:

    void emitWord(unsigned Word);

    /// Routines that handle operands which add machine relocations which are
    /// fixed up by the relocation stage.
    void emitGlobalAddress(const GlobalValue *GV, unsigned Reloc,
                           bool MayNeedFarStub) const;
    void emitExternalSymbolAddress(const char *ES, unsigned Reloc) const;
    void emitConstPoolAddress(unsigned CPI, unsigned Reloc) const;
    void emitJumpTableAddress(unsigned JTIndex, unsigned Reloc) const;
    void emitMachineBasicBlock(MachineBasicBlock *BB, unsigned Reloc) const;

    /// getMachineOpValue - Return binary encoding of operand. If the machine
    /// operand requires relocation, record the relocation and return zero.
    unsigned getMachineOpValue(const MachineInstr &MI,
                               const MachineOperand &MO) const;

    unsigned getRelocation(const MachineInstr &MI,
                           const MachineOperand &MO) const;

    unsigned getJumpTargetOpValue(const MachineInstr &MI, unsigned OpNo) const;

    unsigned getBranchTargetOpValue(const MachineInstr &MI,
                                    unsigned OpNo) const;
    unsigned getMemEncoding(const MachineInstr &MI, unsigned OpNo) const;
    unsigned getSizeExtEncoding(const MachineInstr &MI, unsigned OpNo) const;
    unsigned getSizeInsEncoding(const MachineInstr &MI, unsigned OpNo) const;

    void emitGlobalAddressUnaligned(const GlobalValue *GV, unsigned Reloc,
                                    int Offset) const;
  };
}

char Nios2CodeEmitter::ID = 0;

bool Nios2CodeEmitter::runOnMachineFunction(MachineFunction &MF) {
  Nios2TargetMachine &Target = static_cast<Nios2TargetMachine &>(
                                const_cast<TargetMachine &>(MF.getTarget()));

  II = Target.getInstrInfo();
  TD = Target.getDataLayout();
  Subtarget = &TM.getSubtarget<Nios2Subtarget> ();
  MCPEs = &MF.getConstantPool()->getConstants();
  MJTEs = 0;
  if (MF.getJumpTableInfo()) MJTEs = &MF.getJumpTableInfo()->getJumpTables();
  MCE.setModuleInfo(&getAnalysis<MachineModuleInfo> ());

  do {
    DEBUG(errs() << "JITTing function '"
        << MF.getName() << "'\n");
    MCE.startFunction(MF);

    for (MachineFunction::iterator MBB = MF.begin(), E = MF.end();
        MBB != E; ++MBB){
      MCE.StartMachineBasicBlock(MBB);
      for (MachineBasicBlock::instr_iterator I = MBB->instr_begin(),
           E = MBB->instr_end(); I != E; ++I)
        emitInstruction(*I);
    }
  } while (MCE.finishFunction(MF));

  return false;
}

unsigned Nios2CodeEmitter::getRelocation(const MachineInstr &MI,
                                        const MachineOperand &MO) const {
  // NOTE: This relocations are for static.
  uint64_t TSFlags = MI.getDesc().TSFlags;
  uint64_t Form = TSFlags & Nios2II::FormMask;
  if (Form == Nios2II::FrmJ)
    return Nios2::reloc_nios2_26;
  if ((Form == Nios2II::FrmI))// || Form == Nios2II::FrmFI)
       //&& MI.isBranch())
    return Nios2::reloc_nios2_pc16;
//  if (Form == Nios2II::FrmI && MI.getOpcode() == Nios2::LUi)
//    return Nios2::reloc_nios2_hi;
  return Nios2::reloc_nios2_lo;
}

unsigned Nios2CodeEmitter::getJumpTargetOpValue(const MachineInstr &MI,
                                               unsigned OpNo) const {
  MachineOperand MO = MI.getOperand(OpNo);
  if (MO.isGlobal())
    emitGlobalAddress(MO.getGlobal(), getRelocation(MI, MO), true);
  else if (MO.isSymbol())
    emitExternalSymbolAddress(MO.getSymbolName(), getRelocation(MI, MO));
  else if (MO.isMBB())
    emitMachineBasicBlock(MO.getMBB(), getRelocation(MI, MO));
  else
    llvm_unreachable("Unexpected jump target operand kind.");
  return 0;
}

unsigned Nios2CodeEmitter::getBranchTargetOpValue(const MachineInstr &MI,
                                                 unsigned OpNo) const {
  MachineOperand MO = MI.getOperand(OpNo);
  emitMachineBasicBlock(MO.getMBB(), getRelocation(MI, MO));
  return 0;
}

unsigned Nios2CodeEmitter::getMemEncoding(const MachineInstr &MI,
                                         unsigned OpNo) const {
  // Base register is encoded in bits 20-16, offset is encoded in bits 15-0.
  assert(MI.getOperand(OpNo).isReg());
  unsigned RegBits = getMachineOpValue(MI, MI.getOperand(OpNo)) << 16;
  return (getMachineOpValue(MI, MI.getOperand(OpNo+1)) & 0xFFFF) | RegBits;
}

unsigned Nios2CodeEmitter::getSizeExtEncoding(const MachineInstr &MI,
                                             unsigned OpNo) const {
  // size is encoded as size-1.
  return getMachineOpValue(MI, MI.getOperand(OpNo)) - 1;
}

unsigned Nios2CodeEmitter::getSizeInsEncoding(const MachineInstr &MI,
                                             unsigned OpNo) const {
  // size is encoded as pos+size-1.
  return getMachineOpValue(MI, MI.getOperand(OpNo-1)) +
         getMachineOpValue(MI, MI.getOperand(OpNo)) - 1;
}

/// getMachineOpValue - Return binary encoding of operand. If the machine
/// operand requires relocation, record the relocation and return zero.
unsigned Nios2CodeEmitter::getMachineOpValue(const MachineInstr &MI,
                                            const MachineOperand &MO) const {
  if (MO.isReg())
    return TM.getRegisterInfo()->getEncodingValue(MO.getReg());
  else if (MO.isImm())
    return static_cast<unsigned>(MO.getImm());
  else if (MO.isGlobal())
    emitGlobalAddress(MO.getGlobal(), getRelocation(MI, MO), true);
  else if (MO.isSymbol())
    emitExternalSymbolAddress(MO.getSymbolName(), getRelocation(MI, MO));
  else if (MO.isCPI())
    emitConstPoolAddress(MO.getIndex(), getRelocation(MI, MO));
  else if (MO.isJTI())
    emitJumpTableAddress(MO.getIndex(), getRelocation(MI, MO));
  else if (MO.isMBB())
    emitMachineBasicBlock(MO.getMBB(), getRelocation(MI, MO));
  else
    llvm_unreachable("Unable to encode MachineOperand!");
  return 0;
}

void Nios2CodeEmitter::emitGlobalAddress(const GlobalValue *GV, unsigned Reloc,
                                        bool MayNeedFarStub) const {
  MCE.addRelocation(MachineRelocation::getGV(MCE.getCurrentPCOffset(), Reloc,
                                             const_cast<GlobalValue *>(GV), 0,
                                             MayNeedFarStub));
}

void Nios2CodeEmitter::emitGlobalAddressUnaligned(const GlobalValue *GV,
                                           unsigned Reloc, int Offset) const {
  MCE.addRelocation(MachineRelocation::getGV(MCE.getCurrentPCOffset(), Reloc,
                             const_cast<GlobalValue *>(GV), 0, false));
  MCE.addRelocation(MachineRelocation::getGV(MCE.getCurrentPCOffset() + Offset,
                      Reloc, const_cast<GlobalValue *>(GV), 0, false));
}

void Nios2CodeEmitter::
emitExternalSymbolAddress(const char *ES, unsigned Reloc) const {
  MCE.addRelocation(MachineRelocation::getExtSym(MCE.getCurrentPCOffset(),
                                                 Reloc, ES, 0, 0));
}

void Nios2CodeEmitter::emitConstPoolAddress(unsigned CPI, unsigned Reloc) const {
  MCE.addRelocation(MachineRelocation::getConstPool(MCE.getCurrentPCOffset(),
                                                    Reloc, CPI, 0, false));
}

void Nios2CodeEmitter::
emitJumpTableAddress(unsigned JTIndex, unsigned Reloc) const {
  MCE.addRelocation(MachineRelocation::getJumpTable(MCE.getCurrentPCOffset(),
                                                    Reloc, JTIndex, 0, false));
}

void Nios2CodeEmitter::emitMachineBasicBlock(MachineBasicBlock *BB,
                                            unsigned Reloc) const {
  MCE.addRelocation(MachineRelocation::getBB(MCE.getCurrentPCOffset(),
                                             Reloc, BB));
}

void Nios2CodeEmitter::emitInstruction(const MachineInstr &MI) {
  DEBUG(errs() << "JIT: " << (void*)MCE.getCurrentPCValue() << ":\t" << MI);

  MCE.processDebugLoc(MI.getDebugLoc(), true);

  // Skip pseudo instructions.
  if ((MI.getDesc().TSFlags & Nios2II::FormMask) == Nios2II::Pseudo)
    return;

  emitWord(getBinaryCodeForInstr(MI));
  ++NumEmitted;  // Keep track of the # of mi's emitted

  MCE.processDebugLoc(MI.getDebugLoc(), false);
}

void Nios2CodeEmitter::emitWord(unsigned Word) {
  DEBUG(errs() << "  0x";
        errs().write_hex(Word) << "\n");
  if (Subtarget->isLittle())
    MCE.emitWordLE(Word);
  else
    MCE.emitWordBE(Word);
}

/// createNios2JITCodeEmitterPass - Return a pass that emits the collected Nios2
/// code to the specified MCE object.
FunctionPass *llvm::createNios2JITCodeEmitterPass(Nios2TargetMachine &TM,
                                                 JITCodeEmitter &JCE) {
  return new Nios2CodeEmitter(TM, JCE);
}

#include "Nios2GenCodeEmitter.inc"
