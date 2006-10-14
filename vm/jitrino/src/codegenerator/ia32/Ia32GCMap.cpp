/*
 *  Licensed to the Apache Software Foundation (ASF) under one or more
 *  contributor license agreements.  See the NOTICE file distributed with
 *  this work for additional information regarding copyright ownership.
 *  The ASF licenses this file to You under the Apache License, Version 2.0
 *  (the "License"); you may not use this file except in compliance with
 *  the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */
/**
 * @author Intel, Mikhail Y. Fursov
 * @version $Revision: 1.10.12.2.4.4 $
 */

#include "Ia32GCMap.h"
#include "Ia32Inst.h"
#include "Ia32RuntimeInterface.h"
#include "Ia32StackInfo.h"
#include "Ia32GCSafePoints.h"
#include "XTimer.h"
#include "Ia32Printer.h"

//#define ENABLE_GC_RT_CHECKS

namespace Jitrino
{
namespace  Ia32 {

static ActionFactory<GCMapCreator> _gcmap("gcmap");
static ActionFactory<InfoBlockWriter> _info("info");

//_______________________________________________________________________
// GCMap
//_______________________________________________________________________

GCMap::GCMap(MemoryManager& memM) : mm(memM), gcSafePoints(mm), offsetsInfo(NULL) {
}

void GCMap::registerInsts(IRManager& irm) {
    assert(offsetsInfo == NULL);
    assert(gcSafePoints.empty());
    offsetsInfo = new (mm) GCSafePointsInfo(mm, irm, GCSafePointsInfo::MODE_2_CALC_OFFSETS);
    const Nodes& nodes = irm.getFlowGraph()->getNodes();
    for (Nodes::const_iterator it = nodes.begin(), end = nodes.end(); it!=end; ++it) {
        Node* node = *it;
        if (node->isBlockNode()) {
            processBasicBlock(irm, node);
        }
    }
}

void GCMap::processBasicBlock(IRManager& irm, const Node* block) {
    assert(block->isBlockNode());
    POINTER_SIZE_INT safePointsInBlock = GCSafePointsInfo::getNumSafePointsInBlock(block);
    if (safePointsInBlock == 0) {
        return;
    }
    gcSafePoints.reserve(gcSafePoints.size() + safePointsInBlock);
    BitSet ls(mm, irm.getOpndCount());
    irm.getLiveAtExit(block, ls);
    for (Inst* inst = (Inst*)block->getLastInst(); inst!=NULL; inst = inst->getPrevInst()) {
        if (IRManager::isGCSafePoint(inst)) {
            registerGCSafePoint(irm, ls, inst);
#ifndef _DEBUG   //for debug mode we collect all hardware exception points too
            if (--safePointsInBlock == 0) {
                break;
            }
#else
        }  else if (isHardwareExceptionPoint(inst)){  //check if hardware exception point
            registerHardwareExceptionPoint(inst); //save empty safe-point -> used for debugging and testing
#endif
        }
        irm.updateLiveness(inst, ls);
    }
}

void  GCMap::registerGCSafePoint(IRManager& irm, const BitSet& ls, Inst* inst) {
    POINTER_SIZE_INT eip = (POINTER_SIZE_INT)inst->getCodeStartAddr()+inst->getCodeSize();
    GCSafePoint* gcSafePoint = new (mm) GCSafePoint(mm, eip);
    GCSafePointPairs& pairs = offsetsInfo->getGCSafePointPairs(inst);
    const StlSet<Opnd*>& staticFieldsMptrs = offsetsInfo->getStaticFieldMptrs();
#ifdef GC_MAP_DUMP_ENABLED
    StlVector<int>* offsets = NULL;
    StlVector<Opnd*>* basesAndMptrs = NULL;
    bool loggingGCInst = Log::isEnabled();
    if (loggingGCInst) {
        offsets = new (mm) StlVector<int>(mm);
        basesAndMptrs = new (mm) StlVector<Opnd*>(mm);
    }
#endif    
#ifdef _DEBUG
    gcSafePoint->instId = inst->getId();
#endif
    BitSet::IterB liveOpnds(ls);
    assert(inst->hasKind(Inst::Kind_CallInst));
    Opnd * callRes = inst->getOpndCount(Inst::OpndRole_AllDefs)>0 ? inst->getOpnd(0) : NULL;
    for (int i = liveOpnds.getNext(); i != -1; i = liveOpnds.getNext()) {
        Opnd* opnd = irm.getOpnd(i);
        if (callRes == opnd) {
            continue;
        }
        if (!opnd->getType()->isManagedPtr() && !opnd->getType()->isObject()) {
            continue;
        }
        if (staticFieldsMptrs.find(opnd) != staticFieldsMptrs.end()) {
            continue;
        }
        if (opnd->getRefCount() == 0) {
            continue;
        }
        //ok register this opnd
        MPtrPair* pair = GCSafePointsInfo::findPairByMPtrOpnd(pairs, opnd);
        int32 offset = pair == NULL ? 0 : pair->getOffset();
        bool isObject = offset == 0;
#ifdef _EM64T_
        bool isCompressed = (opnd->getType()->tag <= Type::CompressedVTablePtr && opnd->getType()->tag >= Type::CompressedSystemObject);
#endif
        GCSafePointOpnd* gcOpnd;
        RegName reg = opnd->getRegName();
        if (reg != RegName_Null) {
#ifdef _EM64T_
            gcOpnd = new (mm) GCSafePointOpnd(isObject, TRUE, uint32(reg), offset, isCompressed);
#else
            gcOpnd = new (mm) GCSafePointOpnd(isObject, TRUE, uint32(reg), offset);
#endif
        } else if (opnd->getMemOpndKind() == MemOpndKind_StackAutoLayout) {
            const Opnd* displOpnd = opnd->getMemOpndSubOpnd(MemOpndSubOpndKind_Displacement);
            assert(displOpnd!=NULL);
            int offset_from_esp0 =  (int)displOpnd->getImmValue(); //opnd saving offset from the esp on method call
            int inst_offset_from_esp0 = (int)inst->getStackDepth();
            POINTER_SIZE_INT ptrToAddrOffset = inst_offset_from_esp0 + offset_from_esp0; //opnd saving offset from the esp on inst
            gcOpnd = new (mm) GCSafePointOpnd(isObject, false, ptrToAddrOffset, offset);
        } else {
            assert(opnd->getMemOpndKind() == MemOpndKind_Heap);
            continue;
        }
#ifdef _DEBUG
        gcOpnd->firstId = opnd->getFirstId();
#endif
        gcSafePoint->gcOpnds.push_back(gcOpnd);
#ifdef GC_MAP_DUMP_ENABLED
        if (loggingGCInst) {
            basesAndMptrs->push_back(opnd);
            offsets->push_back(gcOpnd->getMPtrOffset());
        }
#endif
    }
    gcSafePoints.push_back(gcSafePoint);
#ifdef GC_MAP_DUMP_ENABLED
    if (loggingGCInst && !offsets->empty()) {
        GCInfoPseudoInst* gcInst = irm.newGCInfoPseudoInst(*basesAndMptrs);
        gcInst->desc = "gcmap";
        gcInst->offsets.resize(offsets->size());
        std::copy(offsets->begin(), offsets->end(), gcInst->offsets.begin());
        gcInst->insertAfter(inst);
    }
#endif
}
bool GCMap::isHardwareExceptionPoint(const Inst* inst) const {
    Inst::Opnds opnds(inst, Inst::OpndRole_Explicit|Inst::OpndRole_UseDef);
    for (Inst::Opnds::iterator it = opnds.begin(), end = opnds.end(); it!=end; it = opnds.next(it)) {
        Opnd* opnd = inst->getOpnd(it);
        if (opnd->isPlacedIn(OpndKind_Mem) && opnd->getMemOpndKind() == MemOpndKind_Heap) {
            return true;
        }
    }
    return false;
}

void  GCMap::registerHardwareExceptionPoint(Inst* inst) {
    POINTER_SIZE_INT eip = (POINTER_SIZE_INT)inst->getCodeStartAddr();
    GCSafePoint* gcSafePoint = new (mm) GCSafePoint(mm, eip);
#ifdef _DEBUG
    gcSafePoint->instId = inst->getId();
    gcSafePoint->hardwareExceptionPoint = true;
#endif
    gcSafePoints.push_back(gcSafePoint);
}


POINTER_SIZE_INT GCMap::getByteSize() const {
    POINTER_SIZE_INT slotSize = sizeof(POINTER_SIZE_INT);
    POINTER_SIZE_INT size = slotSize/*byte number */ + slotSize/*number of safepoints*/ 
            + slotSize*gcSafePoints.size()/*space to save safepoints sizes*/;
    for (int i=0, n = gcSafePoints.size(); i<n;i++) {
        GCSafePoint* gcSite = gcSafePoints[i];
        size+= slotSize*gcSite->getUint32Size();
    }
    return size;
}

POINTER_SIZE_INT GCMap::readByteSize(const Byte* input) {
    POINTER_SIZE_INT* data = (POINTER_SIZE_INT*)input;
    POINTER_SIZE_INT gcMapSizeInBytes;
    
    gcMapSizeInBytes = data[0];
    return gcMapSizeInBytes;
}


#ifdef _DEBUG
struct hwecompare {
    bool operator() (const GCSafePoint* p1, const GCSafePoint* p2) const {
        if (p1->isHardwareExceptionPoint() == p2->isHardwareExceptionPoint()) {
            return p1 < p2;
        }
        return !p1->isHardwareExceptionPoint();
    }
};
#endif

void GCMap::write(Byte* output)  {
    POINTER_SIZE_INT* data = (POINTER_SIZE_INT*)output;
    data[0] = getByteSize();
    data[1] = gcSafePoints.size();
    POINTER_SIZE_INT offs = 2;

#ifdef _DEBUG
    // make sure that hwe-points are after normal gcpoints
    // this is depends on findGCSafePointStart algorithm -> choose normal gcpoint
    // if both hwe and normal points are registered for the same IP
    std::sort(gcSafePoints.begin(), gcSafePoints.end(), hwecompare());
#endif

    for (int i=0, n = gcSafePoints.size(); i<n;i++) {
        GCSafePoint* gcSite = gcSafePoints[i];
        POINTER_SIZE_INT siteUint32Size = gcSite->getUint32Size();
        data[offs++] = siteUint32Size;
        gcSite->write(data+offs);
        offs+=siteUint32Size;
    }
    assert(sizeof(POINTER_SIZE_INT)*offs == getByteSize());
}


const POINTER_SIZE_INT* GCMap::findGCSafePointStart(const POINTER_SIZE_INT* data, POINTER_SIZE_INT ip) {
    POINTER_SIZE_INT nGCSafePoints = data[1];
    POINTER_SIZE_INT offs = 2;
    for (POINTER_SIZE_INT i = 0; i < nGCSafePoints; i++) {
        POINTER_SIZE_INT siteIP = GCSafePoint::getIP(data + offs + 1);
        if (siteIP == ip) {
            return data+offs+1;
        }
        POINTER_SIZE_INT siteSize = data[offs];
        offs+=1+siteSize;
    }
    return NULL;
}


//_______________________________________________________________________
// GCSafePoint
//_______________________________________________________________________

GCSafePoint::GCSafePoint(MemoryManager& mm, const POINTER_SIZE_INT* image) : gcOpnds(mm) , ip(image[0]) {
    POINTER_SIZE_INT nOpnds = image[1];
    gcOpnds.reserve(nOpnds);
    POINTER_SIZE_INT offs = 2;
    for (uint32 i = 0; i< nOpnds; i++, offs+=3) {
        GCSafePointOpnd* gcOpnd= new (mm) GCSafePointOpnd(image[offs], int(image[offs+1]), int32(image[offs+2]));
#ifdef _DEBUG
        gcOpnd->firstId = image[offs+3];
        offs++;
#endif
        gcOpnds.push_back(gcOpnd);
    }
#ifdef _DEBUG
    instId = image[offs];
    hardwareExceptionPoint = (bool)image[offs+1];
#endif
}

POINTER_SIZE_INT GCSafePoint::getUint32Size() const {
    POINTER_SIZE_INT size = 1/*ip*/+1/*nOpnds*/+GCSafePointOpnd::IMAGE_SIZE_UINT32 * gcOpnds.size()/*opnds images*/;
#ifdef _DEBUG
    size++; //instId
    size++; //hardwareExceptionPoint
#endif
    return size;
}

void GCSafePoint::write(POINTER_SIZE_INT* data) const {
    POINTER_SIZE_INT offs=0;
    data[offs++] = ip;
    data[offs++] = gcOpnds.size();
    for (uint32 i = 0, n = gcOpnds.size(); i<n; i++, offs+=3) {
        GCSafePointOpnd* gcOpnd = gcOpnds[i];
        data[offs] = gcOpnd->flags;
        data[offs+1] = gcOpnd->val;
        data[offs+2] = gcOpnd->mptrOffset;
#ifdef _DEBUG
        data[offs+3] = gcOpnd->firstId;
        offs++;
#endif
    }
#ifdef _DEBUG
    data[offs++] = instId;
    data[offs++] = (POINTER_SIZE_INT)hardwareExceptionPoint;
#endif
    assert(offs == getUint32Size());
}

POINTER_SIZE_INT GCSafePoint::getIP(const POINTER_SIZE_INT* image) {
    return image[0];
}

static inline void m_assert(bool cond)  {
#ifdef _DEBUG
    assert(cond);
#else
#ifdef WIN32
    if (!cond) {
        __asm {
            int 3;
        }
    }
#endif
#endif    
}

void GCMap::checkObject(DrlVMTypeManager& tm, const void* p)  {
    if (p==NULL) return;
    m_assert (!(p<(const void*)0x10000)); //(INVALID PTR)
    m_assert((((POINTER_SIZE_INT)p)&0x3)==0); // check for valid alignment
    POINTER_SIZE_INT vtableOffset=tm.getVTableOffset();
    void * allocationHandle=*(void**)(((uint8*)p)+vtableOffset);
    m_assert (!(allocationHandle<(void*)0x10000 || (((POINTER_SIZE_INT)allocationHandle)&0x3)!=0)); //INVALID VTABLE PTR
    ObjectType * type=tm.getObjectTypeFromAllocationHandle(allocationHandle);
    m_assert(type!=NULL); // UNRECOGNIZED VTABLE PTR;
}

void GCSafePoint::enumerate(GCInterface* gcInterface, const JitFrameContext* context, const StackInfo& stackInfo) const {
#ifdef ENABLE_GC_RT_CHECKS
    MemoryManager mm(256, "tmp");
    DrlVMTypeManager tm(mm);
#endif
    //The algorithm of enumeration is
    //1. Derive all offsets for MPTRs with offsets unknown during compile time.
    //2. Report to VM
    //We need this 2 steps behavior because some GCs move objects during enumeration.
    //In this case it's impossible to derive valid base for mptr with unknown offset.

    //1. Derive all offsets. Use GCSafePointOpnd.mptrOffset to store the result.
    for (uint32 i=0, n = gcOpnds.size(); i<n; i++) {
        GCSafePointOpnd* gcOpnd = gcOpnds[i];
        POINTER_SIZE_INT valPtrAddr = getOpndSaveAddr(context, stackInfo, gcOpnd);
        if (gcOpnd->isObject() || gcOpnd->getMPtrOffset()!=MPTR_OFFSET_UNKNOWN) {
            continue;
        }

        POINTER_SIZE_INT mptrAddr = *((POINTER_SIZE_INT*)valPtrAddr); 
        //we looking for a base that  a) located before mptr in memory b) nearest to mptr
        GCSafePointOpnd* baseOpnd = NULL;
        POINTER_SIZE_INT basePtrAddr = 0, baseAddr = 0;
        for (uint32 j=0; j<n; j++) {
            GCSafePointOpnd* tmpOpnd = gcOpnds[j];   
            if (tmpOpnd->isObject()) {
                POINTER_SIZE_INT tmpPtrAddr = getOpndSaveAddr(context, stackInfo, tmpOpnd);
                POINTER_SIZE_INT tmpBaseAddr = *((POINTER_SIZE_INT*)tmpPtrAddr);
                if (tmpBaseAddr <= mptrAddr) {
                    if (baseOpnd == NULL || tmpBaseAddr > baseAddr) {
                        baseOpnd = tmpOpnd;
                        basePtrAddr = tmpPtrAddr;
                        baseAddr = tmpBaseAddr;
                    } 
                }
            }
        }
        assert(baseOpnd!=NULL);
#ifdef ENABLE_GC_RT_CHECKS
        GCMap::checkObject(tm,  *(void**)basePtrAddr);
#endif 
        int offset = (int)(mptrAddr-baseAddr);
        assert(offset>=0);
        gcOpnd->getMPtrOffset(offset);
    }

    //2. Report the results
    for (uint32 i=0, n = gcOpnds.size(); i<n; i++) {
        GCSafePointOpnd* gcOpnd = gcOpnds[i];
        POINTER_SIZE_INT valPtrAddr = getOpndSaveAddr(context, stackInfo, gcOpnd);
        if (gcOpnd->isObject()) {
#ifdef ENABLE_GC_RT_CHECKS
            GCMap::checkObject(tm, *(void**)valPtrAddr);
#endif
#ifdef _EM64T_
            if(gcOpnd->isCompressed())
                gcInterface->enumerateCompressedRootReference((uint32*)valPtrAddr);
            else
#endif
                gcInterface->enumerateRootReference((void**)valPtrAddr);
        } else { //mptr with offset
            assert(gcOpnd->isMPtr());
            int offset = gcOpnd->getMPtrOffset();
            assert(offset>=0);
#ifdef ENABLE_GC_RT_CHECKS
            char* mptrAddr = *(char**)valPtrAddr;
            GCMap::checkObject(tm, mptrAddr - offset);
#endif
            gcInterface->enumerateRootManagedReference((void**)valPtrAddr, offset);
        }
    }
}

POINTER_SIZE_INT GCSafePoint::getOpndSaveAddr(const JitFrameContext* ctx,const StackInfo& stackInfo,const GCSafePointOpnd *gcOpnd)const {
    POINTER_SIZE_INT addr = 0;  
    if (gcOpnd->isOnRegister()) {
        RegName regName = gcOpnd->getRegName();
        POINTER_SIZE_INT* stackPtr = stackInfo.getRegOffset(ctx, regName);
        addr = (POINTER_SIZE_INT)stackPtr;
    } else { 
        assert(gcOpnd->isOnStack());
#ifdef _EM64T_
        addr = ctx->rsp + gcOpnd->getDistFromInstESP();
#else
        addr = ctx->esp + gcOpnd->getDistFromInstESP();
#endif
    }
    return addr;
}


void GCMapCreator::runImpl() {
    MemoryManager& mm=irManager->getMemoryManager();
    GCMap * gcMap=new(mm) GCMap(mm);
    irManager->calculateOpndStatistics();
    gcMap->registerInsts(*irManager);
    irManager->setInfo("gcMap", gcMap);

    if (Log::isEnabled()) {
        gcMap->getGCSafePointsInfo()->dump(getTagName());
    }
}


//_______________________________________________________________________
// RuntimeInterface
//____________________________________________________ ___________________

static CountTime enumerateTimer("ia32::gcmap::rt_enumerate");
void RuntimeInterface::getGCRootSet(MethodDesc* methodDesc, GCInterface* gcInterface, 
                                   const JitFrameContext* context, bool isFirst)
{  
    AutoTimer tm(enumerateTimer);

/*    Class_Handle parentClassHandle = method_get_class(((DrlVMMethodDesc*)methodDesc)->getDrlVMMethod());
    const char* methodName = methodDesc->getName();
    const char* className = class_get_name(parentClassHandle);
    const char* methodSignature = methodDesc->getSignatureString();
    printf("enumerate: %s::%s %s\n", className, methodName, methodSignature);*/

    // Compute stack information
    uint32 stackInfoSize = StackInfo::getByteSize(methodDesc);
    Byte* infoBlock = methodDesc->getInfoBlock();
    Byte* gcBlock = infoBlock + stackInfoSize;
#ifdef _EM64T_
    const POINTER_SIZE_INT* gcPointImage = GCMap::findGCSafePointStart((POINTER_SIZE_INT*)gcBlock, *context->p_rip);
#else
    const POINTER_SIZE_INT* gcPointImage = GCMap::findGCSafePointStart((POINTER_SIZE_INT*)gcBlock, *context->p_eip);
#endif  
    if (gcPointImage != NULL) {
        MemoryManager mm(128,"RuntimeInterface::getGCRootSet");
        GCSafePoint gcSite(mm, gcPointImage);
        if (gcSite.getNumOpnds() > 0) { 
            //this is a performance filter for empty points 
            // and debug filter for hardware exception point that have no stack info assigned.
            StackInfo stackInfo(mm);
#ifdef _EM64T_
            stackInfo.read(methodDesc, *context->p_rip, false);
#else
            stackInfo.read(methodDesc, *context->p_eip, false);
#endif  
            gcSite.enumerate(gcInterface, context, stackInfo);
        }
    } else {
        //NPE + GC -> nothing to enumerate for this frame;
#ifdef _DEBUG
        assert(0); //in debug mode all hardware exceptions are saved as empty gcsafepoints
#endif 
    }
}

bool RuntimeInterface::canEnumerate(MethodDesc* methodDesc, NativeCodePtr eip) {  
    assert(0); 
    return FALSE;
}


void InfoBlockWriter::runImpl() {
    StackInfo * stackInfo = (StackInfo*)irManager->getInfo("stackInfo");
    assert(stackInfo != NULL);
    GCMap * gcMap = (GCMap*)irManager->getInfo("gcMap");
    assert(gcMap != NULL);
    BcMap *bcMap = (BcMap*)irManager->getInfo("bcMap");
    assert(bcMap != NULL);
    InlineInfoMap * inlineInfo = (InlineInfoMap*)irManager->getInfo("inlineInfo");
    assert(inlineInfo !=NULL);

    CompilationInterface& compIntf = irManager->getCompilationInterface();

    if ( !inlineInfo->isEmpty() ) {
        inlineInfo->write(compIntf.allocateJITDataBlock(inlineInfo->computeSize(), 8));
    }

    uint32 stackInfoSize = stackInfo->getByteSize();
    uint32 gcInfoSize = gcMap->getByteSize();
    uint32 bcMapSize = bcMap->getByteSize(); // we should write at least the size of map  in the info block
    assert(bcMapSize >= sizeof(POINTER_SIZE_INT));               // so  bcMapSize should be more than 4 for ia32

    Byte* infoBlock = compIntf.allocateInfoBlock(stackInfoSize + gcInfoSize + bcMapSize);
    stackInfo->write(infoBlock);
    gcMap->write(infoBlock+stackInfoSize);

    if (compIntf.isBCMapInfoRequired()) {
        bcMap->write(infoBlock + stackInfoSize + gcInfoSize);
    } else {
        // if no bc info write size equal to zerro
        // this will make possible handle errors in case
        bcMap->writeZerroSize(infoBlock + stackInfoSize + gcInfoSize);
    } 
}

}} //namespace
