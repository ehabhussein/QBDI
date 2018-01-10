/*
 * This file is part of QBDI.
 *
 * Copyright 2017 Quarkslab
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "Platform.h"
#include "ExecBlock/ExecBlockManager.h"
#include "Patch/PatchRule.h"
#include "Utility/LogSys.h"

#ifndef QBDI_OS_WIN
#if defined(QBDI_OS_LINUX) && !defined(__USE_GNU)
#define __USE_GNU
#endif
#include <dlfcn.h>
#endif

namespace QBDI {

ExecBlockManager::ExecBlockManager(llvm::MCInstrInfo& MCII, llvm::MCRegisterInfo& MRI, Assembly& assembly, VMInstanceRef vminstance) :
   total_translated_size(1), total_translation_size(1), vminstance(vminstance), MCII(MCII), MRI(MRI), assembly(assembly) {
        searchCache = {0, 0};
}

ExecBlockManager::~ExecBlockManager() {
    LogCallback(LogPriority::DEBUG, "ExecBlockManager::~ExecBlockManager", [&] (FILE *log) -> void {
        this->printCacheStatistics(log);
    });
    clearCache();
}

float ExecBlockManager::getExpansionRatio() const { 
    LogDebug("ExecBlockManager::getExpansionRatio", "%zu / %zu", total_translation_size, total_translated_size);
    return (float) total_translation_size / (float) total_translated_size; 
}

void ExecBlockManager::printCacheStatistics(FILE* output) const {
    float mean_occupation = 0.0;
    size_t region_overflow = 0;
    fprintf(output, "\tCache made of %zu regions:\n", regions.size());
    for(size_t i = 0; i < regions.size(); i++) {
        float occupation = 0.0;
        for(size_t j = 0; j < regions[i].blocks.size(); j++) {
            occupation += regions[i].blocks[j]->occupationRatio();
        }
        if(regions[i].blocks.size() > 1) {
            region_overflow += 1;
        }
        if(regions[i].blocks.size() > 0) {
            occupation /= regions[i].blocks.size();
        }
        mean_occupation += occupation;
        fprintf(output, "\t\t[0x%" PRIRWORD ", 0x%" PRIRWORD "]: %zu blocks, %f occupation ratio\n", regions[i].covered.start, regions[i].covered.end, regions[i].blocks.size(), occupation);
    }
    if(regions.size() > 0) {
        mean_occupation /= regions.size();
    }
    fprintf(output, "\tMean occupation ratio: %f\n", mean_occupation);
    fprintf(output, "\tRegion overflow count: %zu\n", region_overflow);
}

SeqLoc ExecBlockManager::getSeqLoc(rword address) {
    LogDebug("ExecBlockManager::getSeqLoc", "Looking up sequence at address %" PRIRWORD, address);
    size_t r = searchRegion(address);
    if(r < regions.size() && regions[r].covered.contains(address)) {
        // Attempting sequenceCache resolution
        if(regions[r].sequenceCache.count(address) == 1) {
            SeqLoc seqLoc = regions[r].sequenceCache[address];
            LogDebug("ExecBlockManager::getSeqLoc", "Found sequence 0x%" PRIRWORD " in ExecBlock %p as seqID %" PRIu16, 
                     address, seqLoc.execBlock, seqLoc.seqID);
            return seqLoc;
        }
        // Attempting instCache resolution    
        if(regions[r].instCache.count(address) == 1) {
            // Retrieving instruction and corresponding block
            InstLoc instLoc = regions[r].instCache[address];
            ExecBlock* block = regions[r].blocks[instLoc.blockIdx];
            // Registering new basic block
            uint16_t existingSeqId = block->getSeqID(instLoc.instID);
            rword existingBBAddress = block->getInstAddress(block->getSeqStart(existingSeqId));
            uint16_t existingBBIdx = regions[r].sequenceCache[existingBBAddress].bbIdx;
            regions[r].bbRegistry.push_back(BBInfo {
                address, 
                regions[r].bbRegistry[existingBBIdx].end
            });
            // Creating a new sequence at that instruction and saving it in the sequenceCache
            uint16_t newSeqID = block->splitSequence(instLoc.instID);
            uint16_t newBBIdx = (uint16_t) (regions[r].bbRegistry.size() - 1);
            SeqLoc seqLoc = SeqLoc {block, newSeqID, newBBIdx};
            regions[r].sequenceCache[address] = seqLoc;
            LogDebug("ExecBlockManager::getSeqLoc", "Splitted seqID %" PRIu16 " at instID %" PRIu16 " in ExecBlock %p as new sequence with seqID %" PRIu16,
                     existingSeqId, instLoc.instID, block, newSeqID);
            return seqLoc;
        }
    }
    LogDebug("ExecBlockManager::getSeqLoc", "Cache miss for sequence 0x%" PRIRWORD, address);
    return {nullptr, 0};
}

ExecBlock* ExecBlockManager::getExecBlock(rword address) {
    SeqLoc seqLoc = getSeqLoc(address);
    // Program the selector
    if(seqLoc.execBlock != nullptr) {
        seqLoc.execBlock->selectSeq(seqLoc.seqID);
    }
    return seqLoc.execBlock;
}

const BBInfo* ExecBlockManager::getBBInfo(rword address) {
    size_t r = searchRegion(address);
    if(r < regions.size() && regions[r].covered.contains(address) && regions[r].sequenceCache.count(address) == 1) {
        const SeqLoc &seqLoc = regions[r].sequenceCache.at(address);
        return &regions[r].bbRegistry[seqLoc.bbIdx];
    }
    return nullptr;
}

void ExecBlockManager::writeBasicBlock(const std::vector<Patch>& basicBlock) {
    unsigned translated = 0;
    unsigned translation = 0;
    size_t patchIdx = 0, patchEnd = basicBlock.size();
    const Patch& firstPatch = basicBlock.front();
    const Patch& lastPatch = basicBlock.back();

    // Locating an approriate cache region
    Range<rword> codeRange(firstPatch.metadata.address, lastPatch.metadata.address + lastPatch.metadata.instSize);
    size_t r = findRegion(codeRange);

    // Basic block truncation to prevent dedoubled sequence
    for(size_t i = 0; i < basicBlock.size(); i++) {
        if(regions[r].sequenceCache.count(basicBlock[i].metadata.address) != 0) {
            patchEnd = i;
            break;
        }
    }
    // Cache integrity safeguard
    if(patchEnd == 0) {
        LogDebug("ExecBlockManager::writeBasicBlock", "Cache hit, basic block 0x%" PRIRWORD " already exist", firstPatch.metadata.address);
        return;
    }
    LogDebug("ExecBlockManager::writeBasicBlock", "Writting new basic block 0x%" PRIRWORD, firstPatch.metadata.address);
    
    // Registering basic block
    regions[r].bbRegistry.push_back(BBInfo {
        firstPatch.metadata.address,
        lastPatch.metadata.address + lastPatch.metadata.instSize
    });
    // Writing the basic block as one or more sequences
    while(patchIdx < patchEnd) {
        // Attempting to find an ExecBlock in the region
        for(size_t i = 0; true; i++) {
            // If the region doesn't have enough space in its ExecBlocks, we add one.
            // Optimally, a region should only have one ExecBlocks but misspredictions or oversized 
            // basic blocks can cause overflows.
            if(i >= regions[r].blocks.size()) {
                regions[r].blocks.push_back(new ExecBlock(assembly, vminstance));
            }
            // Determine sequence type
            SeqType seqType = (SeqType) 0;
            if(patchIdx == 0) seqType = (SeqType) (seqType | SeqType::Entry);
            if(patchEnd == basicBlock.size()) seqType = (SeqType) (seqType | SeqType::Exit);
            // Write sequence
            SeqWriteResult res = regions[r].blocks[i]->writeSequence(basicBlock.begin() + patchIdx, basicBlock.begin() + patchEnd, seqType);
            // Successful write
            if(res.seqID != EXEC_BLOCK_FULL) {
                // Saving sequence in the sequence cache
                SeqLoc currentSeq;
                currentSeq.execBlock = regions[r].blocks[i];
                currentSeq.seqID = res.seqID;
                currentSeq.bbIdx = regions[r].bbRegistry.size() - 1;
                regions[r].sequenceCache[basicBlock[patchIdx].metadata.address] = currentSeq;
                // Generate instruction mapping cache
                uint16_t startID = regions[r].blocks[i]->getSeqStart(res.seqID);
                uint16_t endID = regions[r].blocks[i]->getSeqEnd(res.seqID);
                for(uint16_t id = startID; id <= endID; id++) {
                    regions[r].instCache[basicBlock[patchIdx + id - startID].metadata.address] = InstLoc {(uint16_t) i, id};
                }
                LogDebug("ExecBlockManager::writeBasicBlock", 
                         "Sequence 0x%" PRIRWORD "-0x%" PRIRWORD " written in ExecBlock %p as seqID %" PRIu16,
                         basicBlock[patchIdx].metadata.address,
                         basicBlock[patchIdx + res.patchWritten - 1].metadata.address,
                         currentSeq.execBlock, currentSeq.seqID);
                // Updating counters
                translated += (basicBlock[patchIdx + res.patchWritten - 1].metadata.address + 
                               basicBlock[patchIdx + res.patchWritten - 1].metadata.instSize) - 
                               basicBlock[patchIdx].metadata.address;
                translation += res.bytesWritten;
                patchIdx += res.patchWritten;
                break;
            }
        }
    }
    // Updating stats
    total_translation_size += translation;
    total_translated_size += translated;
    updateRegionStat(r, translated);
}

size_t ExecBlockManager::searchRegion(rword address) {
    size_t low = 0;
    size_t high = regions.size();
    if(regions.size() == 0) {
        return 0;
    }
    LogDebug("ExecBlockManager::searchRegion", "Searching for address 0x%" PRIRWORD, address);
    if(searchCache.address == address) {
        LogDebug("ExecBlockManager::searchRegion", "Cache hit for region %zu [0x%" PRIRWORD ", 0x%" PRIRWORD "]", 
                 searchCache.regionIdx, regions[searchCache.regionIdx].covered.start, regions[searchCache.regionIdx].covered.end);
        return searchCache.regionIdx;
    }
    // Binary search of the first region to look at
    while(low + 1 != high) {
        size_t idx = (low + high) / 2;
        if(regions[idx].covered.start > address) {
            high = idx;
        }
        else if(regions[idx].covered.end <= address) {
            low = idx;
        }
        else {
            LogDebug("ExecBlockManager::searchRegion", "Exact match for region %zu [0x%" PRIRWORD ", 0x%" PRIRWORD "]", 
                     idx, regions[idx].covered.start, regions[idx].covered.end);
            searchCache = {address, idx};
            return idx;
        }
    }
    LogDebug("ExecBlockManager::searchRegion", "Low match for region %zu [0x%" PRIRWORD ", 0x%" PRIRWORD "]", 
             low, regions[low].covered.start, regions[low].covered.end);
    searchCache = {address, low};
    return low;
}

size_t ExecBlockManager::findRegion(Range<rword> codeRange) {
    size_t best_region = regions.size();
    size_t low = searchRegion(codeRange.start);
    unsigned best_cost = 0xFFFFFFFF;
    for(size_t i = low; i < low + 3 && i < regions.size(); i++) {
        unsigned cost = 0;
        // Easy case: the code range is inside one of the region, we can return immediately
        if(regions[i].covered.contains(codeRange)) {
            LogDebug(
                "ExecBlockManager::findRegion", 
                "Basic block [0x%" PRIRWORD ", 0x%" PRIRWORD "] assigned to region %zu [0x%" PRIRWORD ", 0x%" PRIRWORD "]", 
                codeRange.start,
                codeRange.end,
                i,
                regions[i].covered.start,
                regions[i].covered.end
            );
            searchCache = {codeRange.start, i};
            return i;
        }
        // Hard case: it's in the available budget of one the region. Keep the lowest cost.
        // First compute the required cost for the region to cover this extended range.
        if(regions[i].covered.end < codeRange.end) {
            cost += (codeRange.end - regions[i].covered.end);
        }
        if(regions[i].covered.start > codeRange.start) {
            cost += (regions[i].covered.start - codeRange.start);
        }
        // Make sure that such cost is available and that it's better than previous candidates
        if((unsigned) (cost * getExpansionRatio()) < regions[i].available && cost < best_cost) {
            best_cost = cost;
            best_region = i;
        }
    }
    // We found an extension candidate
    if(best_region != regions.size()) {
        LogDebug(
            "ExecBlockManager::findRegion", 
            "Extending region %zu [0x%" PRIRWORD ", 0x%" PRIRWORD "] to cover basic block [0x%" PRIRWORD ", 0x%" PRIRWORD "]",
            best_region,
            regions[best_region].covered.start,
            regions[best_region].covered.end,
            codeRange.start,
            codeRange.end
        );
        if(regions[best_region].covered.end < codeRange.end) {
            regions[best_region].covered.end = codeRange.end;
        }
        if(regions[best_region].covered.start > codeRange.start) {
            regions[best_region].covered.start = codeRange.start;
        }
        searchCache = {codeRange.start, best_region};
        return best_region;
    }
    // Else we have to create a new region
    // Find a place to insert it
    size_t insert = low;
    for(; insert < regions.size(); insert++) {
        if(regions[insert].covered.start > codeRange.start) {
            break;
        }
    }
    LogDebug(
        "ExecBlockManager::findRegion", 
        "Creating new region %zu to cover basic block [0x%" PRIRWORD ", 0x%" PRIRWORD "]",
        insert,
        codeRange.start,
        codeRange.end
    );
    regions.insert(regions.begin() + insert, ExecRegion {codeRange, 0, 0, std::vector<ExecBlock*>()});
    searchCache = {codeRange.start, insert};
    return insert;
}

void ExecBlockManager::updateRegionStat(size_t r, rword translated) {
    regions[r].translated += translated;
    // Remaining code block space
    regions[r].available = regions[r].blocks[0]->getEpilogueOffset();
    // Space which needs to be reserved for the non translated part of the covered region
    unsigned reserved = (unsigned) (((float) (regions[r].covered.size() - regions[r].translated)) * getExpansionRatio());
    LogDebug(
        "ExecBlockManager::updateRegionStat", 
        "Region %zu has %zu bytes available of which %zu are reserved for %zu bytes of untranslated code",
        r,
        regions[r].available,
        reserved,
        (regions[r].covered.size() - regions[r].translated)
    );
    if(reserved > regions[r].available) {
        regions[r].available = 0;
    }
    else {
        regions[r].available -= reserved;
    }
}

static void analyseRegister(OperandAnalysis& opa, unsigned int regNo, const llvm::MCRegisterInfo& MRI) {
    opa.regName = MRI.getName(regNo);
    opa.value = regNo;
    opa.size = 0;
    opa.regOff = 0;
    opa.regCtxIdx = 0;
    // try to match register in our GPR context
    for (uint16_t j = 0; j < NUM_GPR; j++) {
        if (MRI.isSubRegisterEq(GPR_ID[j], regNo)) {
            if (GPR_ID[j] != regNo) {
                unsigned int subregidx = MRI.getSubRegIndex(GPR_ID[j], regNo);
                opa.size = MRI.getSubRegIdxSize(subregidx);
                opa.regOff = MRI.getSubRegIdxOffset(subregidx);
            } else {
                opa.size = sizeof(rword);
            }
            opa.regCtxIdx = j;
            break;
        }
    }
}

static void tryMergeCurrentRegister(InstAnalysis* instAnalysis) {
    OperandAnalysis& opa = instAnalysis->operands[instAnalysis->numOperands - 1];
    for (uint16_t j = 0; j < instAnalysis->numOperands - 1; j++) {
        OperandAnalysis& pop = instAnalysis->operands[j];
        if (pop.type != opa.type) {
            continue;
        }
        if (pop.regName == opa.regName &&
            pop.size == opa.size &&
            pop.regOff == opa.regOff) {
            // merge current one into previous one
            pop.regAccess |= opa.regAccess;
            memset(&opa, 0, sizeof(OperandAnalysis));
            instAnalysis->numOperands--;
            break;
        }
    }
}

static void analyseImplicitRegisters(InstAnalysis* instAnalysis, const uint16_t* implicitRegs, RegisterAccessType type, const llvm::MCRegisterInfo& MRI) {
    if (!implicitRegs) {
        return;
    }
    // Iteration style copied from LLVM code
    for (; *implicitRegs; ++implicitRegs) {
        OperandAnalysis topa;
        analyseRegister(topa, *implicitRegs, MRI);
        // we found a GPR (as size is only known for GPR)
        // TODO: add support for more registers
        if (topa.size != 0) {
            // fill a new operand analysis
            OperandAnalysis& opa = instAnalysis->operands[instAnalysis->numOperands];
            opa = topa;
            opa.type = OPERAND_GPR;
            opa.regAccess = type;
            instAnalysis->numOperands++;
            // try to merge with a previous one
            tryMergeCurrentRegister(instAnalysis);
        }
    }
}

static void analyseOperands(InstAnalysis* instAnalysis, const llvm::MCInst& inst, const llvm::MCInstrDesc& desc, const llvm::MCRegisterInfo& MRI) {
    if (!instAnalysis) {
        // no instruction analysis
        return;
    }
    instAnalysis->numOperands = 0; // updated later because we could skip some
    instAnalysis->operands = NULL;
    // Analysis of instruction operands
    uint8_t numOperands = inst.getNumOperands();
    uint8_t numOperandsMax = numOperands + desc.getNumImplicitDefs() + desc.getNumImplicitUses();
    if (numOperandsMax == 0) {
        // no operand to analyse
        return;
    }
    instAnalysis->operands = new OperandAnalysis[numOperandsMax]();
    // find written registers
    std::bitset<16> regWrites;
    for (unsigned i = 0,
            e = desc.isVariadic() ? inst.getNumOperands() : desc.getNumDefs();
            i != e; ++i) {
        const llvm::MCOperand& op = inst.getOperand(i);
        if (op.isReg()) {
            regWrites.set(i, true);
        }
    }
    unsigned int numClasses = MRI.getNumRegClasses();
    // for each instruction operands
    for (uint8_t i = 0; i < numOperands; i++) {
        const llvm::MCOperand& op = inst.getOperand(i);
        const llvm::MCOperandInfo& opdesc = desc.OpInfo[i];
        // fill a new operand analysis
        OperandAnalysis& opa = instAnalysis->operands[instAnalysis->numOperands];
        if (!op.isValid()) {
            continue;
        }
        if (op.isReg()) {
            unsigned int regNo = op.getReg();
            // validate that this is really a register operand, not
            // something else (memory access)
            if (opdesc.OperandType != llvm::MCOI::OPERAND_REGISTER) {
                continue;
            }
            // fill the operand analysis
            analyseRegister(opa, regNo, MRI);
            // we have'nt found a GPR (as size is only known for GPR)
            if (opa.size == 0) {
                // TODO: add support for more registers
                continue;
            }
            // update register size using class
            if (!opdesc.isLookupPtrRegClass() &&
                ((unsigned int) opdesc.RegClass < numClasses)) {
                const llvm::MCRegisterClass& regclass = MRI.getRegClass(opdesc.RegClass);
                opa.size = regclass.getSize();
            }
            opa.type = OPERAND_GPR;
            opa.regAccess = regWrites.test(i) ? REGISTER_WRITE : REGISTER_READ;
            instAnalysis->numOperands++;
            // try to merge with a previous one
            tryMergeCurrentRegister(instAnalysis);
        } else if (op.isImm()) {
            // FIXME: broken in LLVM 3.7
#ifndef QBDI_ARCH_ARM
            // validate that this is really a register operand, not
            // something else (memory access)
            if (opdesc.OperandType != llvm::MCOI::OPERAND_IMMEDIATE) {
                continue;
            }
#endif
            // fill the operand analysis
            if (opdesc.isPredicate()) {
                opa.type = OPERAND_PRED;
            } else {
                opa.type = OPERAND_IMM;
            }
            opa.value = (rword) op.getImm();
            opa.size = sizeof(rword);
            instAnalysis->numOperands++;
        }
    }

    // analyse implicit registers (R/W)
    analyseImplicitRegisters(instAnalysis, desc.getImplicitDefs(), REGISTER_WRITE, MRI);
    analyseImplicitRegisters(instAnalysis, desc.getImplicitUses(), REGISTER_READ, MRI);
}


static void freeInstAnalysis(InstAnalysis* analysis) {
    if (analysis == nullptr) {
        return;
    }
    delete[] analysis->operands;
    delete[] analysis->disassembly;
    delete analysis;
}


const InstAnalysis* ExecBlockManager::analyzeInstMetadata(const InstMetadata* instMetadata, AnalysisType type) {
    InstAnalysis* instAnalysis = nullptr;
    RequireAction("Engine::analyzeInstMetadata", instMetadata, return nullptr);

    size_t r = searchRegion(instMetadata->address);

    // Attempt to locate it in the sequenceCache
    if(r < regions.size() && regions[r].covered.contains(instMetadata->address) && 
       regions[r].analysisCache.count(instMetadata->address) == 1) {
        LogDebug("ExecBlockManager::analyzeInstMetadata", "Analysis of instruction 0x%" PRIRWORD " found in sequenceCache of region %zu", instMetadata->address, r);
        instAnalysis = regions[r].analysisCache[instMetadata->address];
    }
    // Do we have anything we want inside the cache ?
    if ((instAnalysis != nullptr) &&
        ((instAnalysis->analysisType & type) != type)) {
        LogDebug("ExecBlockManager::analyzeInstMetadata", "Analysis of instruction 0x%" PRIRWORD " need to be rebuilt", instMetadata->address);
        // Free current cache because we want more data
        freeInstAnalysis(instAnalysis);
        instAnalysis = nullptr;
    }
    // We have a usable cached analysis
    if (instAnalysis != nullptr) {
        return instAnalysis;
    }
    // Cache miss
    const llvm::MCInst &inst = instMetadata->inst;
    const llvm::MCInstrDesc &desc = MCII.get(inst.getOpcode());

    instAnalysis = new InstAnalysis;
    // set all values to NULL/0/false
    memset(instAnalysis, 0, sizeof(InstAnalysis));

    instAnalysis->analysisType      = type;

    if (type & ANALYSIS_DISASSEMBLY) {
        int len = 0;
        std::string buffer;
        llvm::raw_string_ostream bufferOs(buffer);
        assembly.printDisasm(inst, bufferOs);
        bufferOs.flush();
        len = buffer.size() + 1;
        instAnalysis->disassembly = new char[len];
        strncpy(instAnalysis->disassembly, buffer.c_str(), len);
        buffer.clear();
    }

    if (type & ANALYSIS_INSTRUCTION) {
        instAnalysis->address           = instMetadata->address;
        instAnalysis->instSize          = instMetadata->instSize;
        instAnalysis->affectControlFlow = instMetadata->modifyPC;
        instAnalysis->isBranch          = desc.isBranch();
        instAnalysis->isCall            = desc.isCall();
        instAnalysis->isReturn          = desc.isReturn();
        instAnalysis->isCompare         = desc.isCompare();
        instAnalysis->isPredicable      = desc.isPredicable();
        instAnalysis->mayLoad           = desc.mayLoad();
        instAnalysis->mayStore          = desc.mayStore();
        instAnalysis->mnemonic          = MCII.getName(inst.getOpcode()).data();
    }

    if (type & ANALYSIS_OPERANDS) {
        // analyse operands (immediates / registers)
        analyseOperands(instAnalysis, inst, desc, MRI);
    }

    if (type & ANALYSIS_SYMBOL) {
        // find nearest symbol (if any)
#ifndef QBDI_OS_WIN
        Dl_info info;
        const char* ptr;

        int ret = dladdr((void*) instAnalysis->address, &info);
        if (ret != 0) {
            if (info.dli_sname) {
                instAnalysis->symbol = info.dli_sname;
                instAnalysis->symbolOffset = instAnalysis->address - (rword) info.dli_saddr;
            }
            if (info.dli_fname) {
                // dirty basename, but thead safe
                if((ptr = strrchr(info.dli_fname, '/')) != nullptr) {
                    instAnalysis->module = ptr + 1;
                }
            }
        }
#endif
    }

    // If its part of a region, put in in the region cache
    if(r < regions.size() && regions[r].covered.contains(instMetadata->address)) {
        LogDebug("ExecBlockManager::analyzeInstMetadata", "Analysis of instruction 0x%" PRIRWORD " cached in region %zu", instMetadata->address, r);
        regions[r].analysisCache[instMetadata->address] = instAnalysis;
    }
    // Put it in the global cache. Should never happen under normal usage
    else {
        LogDebug("ExecBlockManager::analyzeInstMetadata", "Analysis of instruction 0x%" PRIRWORD " cached in global cache", instMetadata->address);
        analysisCache[instMetadata->address] = instAnalysis;
    }
    return instAnalysis;
}


void ExecBlockManager::eraseRegion(size_t r) {
    LogDebug("ExecBlockManager::eraseRegion", "Erasing region %zu [0x%" PRIRWORD ", 0x%" PRIRWORD "]", 
             r, regions[r].covered.start, regions[r].covered.end);
    // Delete cached blocks
    for(ExecBlock* block: regions[r].blocks) {
        LogDebug("ExecBlockManager::eraseRegion", "Dropping ExecBlock %p", block);
        delete block;
    }
    // Delete cached analysis
    for(std::pair<rword, InstAnalysis*> analysis: regions[r].analysisCache) {
        freeInstAnalysis(analysis.second);
    }
    regions.erase(regions.begin() + r);
}

void ExecBlockManager::clearCache(RangeSet<rword> rangeSet) {
    const std::vector<Range<rword>>& ranges = rangeSet.getRanges();
    for(Range<rword> r: ranges) {
        clearCache(r);
    }
    // Probably comming from an instrumentation change, reset translation counters
    total_translated_size = 1;
    total_translation_size = 1;
}

void ExecBlockManager::flushCommit() {
    // It needs to be erased from last to first to preserve index validity
    if(flushList.size() > 0) {
        LogDebug("ExecBlockManager::flushCommit", "Flushing analysis caches");
        std::sort(flushList.begin(), flushList.end(), std::greater<size_t>());
        // Remove duplicates
        flushList.erase(std::unique(flushList.begin(), flushList.end()), flushList.end());
        for(size_t r: flushList) {
            eraseRegion(r);
        }
        flushList.clear();
        // Clear global cache
        for(std::pair<rword, InstAnalysis*> analysis: analysisCache) {
            freeInstAnalysis(analysis.second);
        }
        analysisCache.clear();
        searchCache = {0, 0};
    }
}

void ExecBlockManager::clearCache(Range<rword> range) {
    size_t i = 0;
    LogDebug("ExecBlockManager::clearCache", "Erasing range [0x%" PRIRWORD ", 0x%" PRIRWORD "]", range.start, range.end);
    for(i = 0; i < regions.size(); i++) {
        if(regions[i].covered.overlaps(range)) {
            flushList.push_back(i);
        }
    }
}

void ExecBlockManager::clearCache() {
    LogDebug("ExecBlockManager::clearCache", "Erasing all cache");
    while(regions.size() > 0) {
        eraseRegion(regions.size() - 1);
    }
}

}
