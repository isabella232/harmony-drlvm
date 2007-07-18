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
 * @author Intel, Natalya V. Golovleva
 * @version $Revision$
 *
 */

#include "escanalyzer.h"
#include "FlowGraph.h"
#include "Inst.h"
#include "Stl.h"
#include "BitSet.h"
#include "Dominator.h"
#include "Loop.h"
#include "Log.h"
#include "Type.h"
#include "ssa/SSA.h"
#include "optpass.h"
#include "devirtualizer.h"
#include "VMInterface.h"

namespace Jitrino {

const  char* help =
    "  escape flags:\n"
    "    escape.max_level[=0]                     - max level callee method analysis\n"
    "    escape.do_sync_removal[={ON,off}]        - do synchronization removal optimization\n"
    "    escape.do_sync_removal_vc[={ON,off}]     - do synchronization removal optimization\n"
    "                                               for virtual call escaped operands\n"
    "    escape.do_sync_removal_sm[={ON,off}]     - do synchronization removal optimization\n"
    "                                               for synchronized methods\n"
    "    escape.do_scalar_repl[={ON,off}]         - do scalar replacement optimization for\n"
    "                                               local and escaped objects\n"
    "    escape.do_esc_scalar_repl[={ON,off}]     - scalar replacement for escaped objects\n"
    "    escape.do_scalar_repl_only_final_fields_in_use[={on,OFF}] \n"
    "    escape.do_scalar_repl_final_fields[={on,OFF}] \n"
    "                                             - scalarize final field usage when\n"
    "                                               escaped object wasn't optimized\n"
    "    escape.exec_count_mult[=0]               - entry node execCount multiplier\n";


DEFINE_SESSION_ACTION(EscapeAnalysisPass, escape, "Escape Analysis")


struct ComObjStat {
    ComObjStat()    :_n0(0), _n_ge(0), _n_ae(0), _n_ne(0), _n_lo(0) {};
    uint32 _n0;
    uint32 _n_ge;
    uint32 _n_ae;
    uint32 _n_ne;
    uint32 _n_lo;
};
static ComObjStat comObjStat;
EscAnalyzer::CalleeMethodInfos* EscAnalyzer::calleeMethodInfos=NULL;
Mutex EscAnalyzer::calleeMethodInfosLock;


void
EscapeAnalysisPass::_run(IRManager& irm) {

    MemoryManager escMemManager("EscapeAnalyzer:tmp_mm");
    EscAnalyzer ea(escMemManager, this, irm);

    if (Log::isEnabled() && (ea.allProps!=0)) {
        Log::out() << "E s c a p e   A n a l y s i s " << std::endl;
    }

    ea.doAnalysis();
    
    irm.getFlowGraph().purgeUnreachableNodes(); //needed to get valid log after this pass

}  // run(IRManager& irm) 


EscAnalyzer::EscAnalyzer(MemoryManager& mm, SessionAction* argSource, IRManager& irm)
: eaMemManager(mm), irManager(irm), mh(irm.getMethodDesc()), 
  compInterface(irm.getCompilationInterface()),os_sc(Log::out())
{
    maxMethodExamLevel = (uint32)argSource->getIntArg("max_level",maxMethodExamLevel_default);
    allProps = (uint32)argSource->getIntArg("d_prop",0);
    debug_method = argSource->getStringArg("d_method", NULL);
    method_ea_level = 0;  // determines level of method scan
    do_sync_removal = argSource->getBoolArg("do_sync_removal",true);
    do_sync_removal_vc = argSource->getBoolArg("do_sync_removal_vc",true);
    do_sync_removal_sm = argSource->getBoolArg("do_sync_removal_sm",true);
    do_scalar_repl = argSource->getBoolArg("do_scalar_repl",true);
    do_esc_scalar_repl = argSource->getBoolArg("do_esc_scalar_repl",true);
    print_scinfo = argSource->getBoolArg("sc_info",false);
    execCountMultiplier_string = argSource->getStringArg("exec_count_mult", NULL);
    ec_mult = ( execCountMultiplier_string==NULL ? 0 : atof(execCountMultiplier_string) );
    do_scalar_repl_only_final_fields_in_use = argSource->getBoolArg("do_scalar_repl_only_final_fields_in_use",false);
    do_scalar_repl_final_fields = argSource->getBoolArg("do_scalar_repl_final_fields",false);
    compressedReferencesArg = argSource->getBoolArg("compressedReferences", false);

    const char* translatorName = argSource->getStringArg("translatorActionName", "translator");
    translatorAction = (TranslatorAction*)PMF::getAction(argSource->getPipeline(), translatorName);
    assert(translatorAction);

    init();
}

EscAnalyzer::EscAnalyzer(EscAnalyzer* parent, IRManager& irm) 
: eaMemManager(parent->eaMemManager), irManager(irm), mh(irm.getMethodDesc()),
  compInterface(irm.getCompilationInterface()), os_sc(Log::out())
{
    maxMethodExamLevel = parent->maxMethodExamLevel;
    allProps = parent->allProps;
    debug_method = parent->debug_method;
    translatorAction  = parent->translatorAction;
    method_ea_level = parent->method_ea_level + 1;

    init();
}

void EscAnalyzer::init() {
    i32_0 = NULL;
    i32_1 = NULL;

    initNodeType = 0;     // type of initial scanned node

    scannedObjs = new (eaMemManager) ObjIds(eaMemManager);
    scannedObjsRev = new (eaMemManager) ObjIds(eaMemManager);
    scannedInsts = new (eaMemManager) ObjIds(eaMemManager);
    scannedSucNodes = new (eaMemManager) ObjIds(eaMemManager);
    monitorInstUnits = new (eaMemManager) MonInstUnits(eaMemManager);
    exam2Insts = new (eaMemManager) Insts(eaMemManager);
    methodEndInsts = new (eaMemManager) Insts(eaMemManager);
    checkInsts = new (eaMemManager) Insts(eaMemManager);

    _cfgirun=0;
    _instrInfo=0;
    _instrInfo2=0;
    _cngnodes=0;
    _cngedges=0;
    _scanMtds=0;
    _setState=0;
    _printstat=0;
    _eainfo=0;
    _seinfo=0;
    if (Log::isEnabled())
        _scinfo = 1;
    else
        if (print_scinfo)
            _scinfo=1;
        else 
            _scinfo=0;
    std::fill(prsArr, prsArr + prsNum, 0);


    int pr = allProps;
    int prsBound[] = {1000000000,100000000,10000000,1000000,100000,10000,1000,100,10,1};
    if (pr-prsBound[0]-prsBound[0]<0 && pr!= 0) {
        for (int i=0; i<prsNum; i++) {
            if (pr >= prsBound[i] && pr-prsBound[i] >= 0) {
                prsArr[i] = 1;
                pr -= prsBound[i];
            }
        }

        _eainfo     = prsArr[8];
        _seinfo     = prsArr[9];
        if (debug_method==NULL || strcmp(debug_method,"all")==0) {
            _cfgirun    = prsArr[0];
            _instrInfo  = prsArr[1];
            _instrInfo2 = prsArr[2];
            _cngnodes   = prsArr[3];
            _cngedges   = prsArr[4];
            _scanMtds   = prsArr[5];
            _setState   = prsArr[6];
            _printstat  = prsArr[7];
        }
    }
}

void 
EscAnalyzer::showFlags(std::ostream& os) {
    os << "  escape flags:"<<std::endl;
    os << "    escape.max_level[=0]                     - max level callee method analysis" << std::endl;
    os << "    escape.do_sync_removal[={ON,off}]        - do synchronization removal optimization" << std::endl;
    os << "    escape.do_sync_removal_vc[={ON,off}]     - do synchronization removal optimization" << std::endl;
    os << "                                               for virtual call escaped operands" << std::endl;
    os << "    escape.do_sync_removal_sm[={ON,off}]     - do synchronization removal optimization" << std::endl;
    os << "                                               for synchronized methods" << std::endl;
    os << "    escape.do_scalar_repl[={ON,off}]         - do scalar replacement optimization for" << std::endl;
    os << "                                               local and escaped objects" << std::endl;
    os << "    escape.do_esc_scalar_repl[={ON,off}]     - scalar replacement for escaped objects" << std::endl;
    os << "    escape.do_scalar_repl_only_final_fields_in_use[={on,OFF}] " << std::endl;
    os << "    escape.do_scalar_repl_final_fields[={on,OFF}]  " << std::endl;
    os << "                                             - scalarize final field usage when" << std::endl;
    os << "                                               escaped object wasn't optimized" << std::endl;
    os << "    escape.exec_count_mult[=0]               - entry node execCount multiplier" << std::endl;
}

void
EscAnalyzer::doAnalysis() {
    const char* mn = mh.getName();
    const Nodes& nodes = irManager.getFlowGraph().getNodes();
    ControlFlowGraph& fg = irManager.getFlowGraph();
    uint32 nodeNum = (uint32)nodes.size();
    uint32 num2 = fg.getNodeCount();
    uint32 num3 = fg.getMaxNodeId();
    Nodes::const_iterator niter;

    lastCnGNodeId = 0;    // initialization of private field
    defArgNumber = -1;   // initialization of private field

    if (Log::isEnabled() && debug_method!=NULL) {
        if (strcmp(mn,debug_method)==0) {
            _cfgirun    = prsArr[0];
            _instrInfo  = prsArr[1];
            _instrInfo2 = prsArr[2];
            _cngnodes   = prsArr[3];
            _cngedges   = prsArr[4];
            _scanMtds   = prsArr[5];
            _setState   = prsArr[6];
            _printstat  = prsArr[7];
            Log::out() << "_cfgirun     " << _cfgirun    << std::endl;
            Log::out() << "_instrInfo   " << _instrInfo  << std::endl;
            Log::out() << "_instrInfo2  " << _instrInfo2 << std::endl;
            Log::out() << "_cngnodes    " << _cngnodes   << std::endl;
            Log::out() << "_cngedges    " << _cngedges   << std::endl;
            Log::out() << "_scanMtds    " << _scanMtds   << std::endl;
            Log::out() << "_setState    " << _setState   << std::endl;
            Log::out() << "_printstat   " << _printstat  << std::endl;
            Log::out() << "_eainfo      " << _eainfo     << std::endl;
            Log::out() << "_seinfo      " << _seinfo     << std::endl;
            Log::out() << "_scinfo      " << _scinfo     << std::endl;
        }
    }

    if (_eainfo || _seinfo) {
        Log::out()<<"======  doAnalysis  ====== "<<mn<<"   level: ";
        Log::out()<<method_ea_level<<"   "; 
        if (mh.isSynchronized())
            Log::out()<<"sync   "; 
        if (mh.isStatic())
            Log::out()<<"stat   "; 
        Log::out()<<nodeNum<<" "<<num2<<" "<<num3<< "   ";
        mh.printFullName(Log::out()); 
        Log::out()<< std::endl;
    }
    int maxInd = 0;
    int cur = -1;
    Node* node;
    cngNodes=new (eaMemManager) CnGNodes(eaMemManager);   // Common part of connection graph (nodes)

    for(niter = nodes.begin(); niter != nodes.end(); ++niter) {
        node = *niter;
        cur = maxInd++;

//  exam instructions

#ifdef _DEBUG
        if (_cfgirun) {
            Log::out() <<std::endl;
            Log::out() <<"Scan "<< cur <<"  Node ";
            FlowGraph::printLabel(Log::out(),node);
            Log::out() <<"  Id. "<<node->getId() <<std::endl;
        }
#endif

        instrExam(node);

    };

#ifdef _DEBUG
    if (_cngnodes) {
        Log::out() <<"printCnGNodes: "; 
        mh.printFullName(Log::out()); 
        Log::out() << std::endl;
        printCnGNodes("First run result: nodes",Log::out());
    }
#endif

    cngEdges=new (eaMemManager) CnGEdges(eaMemManager);   // Common part of connection graph (edges)
    instrExam2();
    
    if (_cngedges) {
        Log::out() <<"printCnGEdges: "; 
        mh.printFullName(Log::out()); 
        Log::out() << std::endl;
        printCnGEdges("resulting OUT CnGEdges",Log::out());
    }

    setCreatedObjectStates();

    if (_eainfo) {
        printCreatedObjectsInfo(Log::out());
    }

    saveScannedMethodInfo();    // to save states of contained obj, if needed

#ifdef _DEBUG
    if (_cngedges) {
        Log::out() << "++++++++++++++ printRefInfo()    ";
        mh.printFullName(Log::out());
        Log::out() << std::endl;
        printRefInfo(Log::out());
        Log::out() << "++++++++++++++ end               "; 
        mh.printFullName(Log::out());
        Log::out() << std::endl;
    }
#endif

    if (_cngnodes) {
        Log::out() <<"printCnGNodes: "; 
        mh.printFullName(Log::out()); 
        Log::out() << std::endl;
        printCnGNodes("Marked nodes",Log::out());
    }

    if (method_ea_level == 0) {  
        createdObjectInfo();     // prints if _printstat==1
    }

    if (method_ea_level == 0) {
        if (do_sync_removal) {
            scanSyncInsts();
        }
        if (do_scalar_repl) {
            scanLocalObjects();
            eaFixupVars(irManager);
            if (do_esc_scalar_repl) {
                scanEscapedObjects();
                eaFixupVars(irManager);
            }
        }
#ifdef _DEBUG
        if (_seinfo && Log::isEnabled()) {
            printCreatedObjectsInfo(Log::out());
        }
#endif
    }

    if (_eainfo || _seinfo) {
        Log::out()<<"======  doAnalysis  ====== "<<mn<<"   level: ";
        Log::out()<<method_ea_level<<"   end   "; 
        if (mh.isSynchronized())
            Log::out()<<"sync   "; 
        if (mh.isStatic())
            Log::out()<<"stat   "; 
        mh.printFullName(Log::out()); 
        Log::out()<< std::endl;
    }

    if (Log::isEnabled() && debug_method!=NULL) {
        if (strcmp(mn,debug_method)==0) {
            _cfgirun    = 0;
            _instrInfo  = 0;
            _instrInfo2 = 0;
            _cngnodes   = 0;
            _cngedges   = 0;
            _scanMtds   = 0;
            _setState   = 0;
            _printstat  = 0;
        }
    }

    return;
}  // doAnalysis() 


void 
EscAnalyzer::instrExam(Node* node) {
    Inst *headInst = (Inst*)node->getFirstInst();
    int insnum=0;
    Type* type;
    CnGNode* cgnode;
    uint32 ntype=0;
    MethodDesc* md;
    uint32 n;
    bool addinst;
    Inst* method_inst;

    for (Inst* inst=headInst->getNextInst();inst!=NULL;inst=inst->getNextInst()) {
        insnum++;
        ntype=0;
        if (_instrInfo) {
            Log::out() <<"instrExam:" << std::endl;
            Log::out() <<"++Node Id."<<node->getId()<<"  "<<node->getDfNum()<<std::endl;
            Log::out() <<"instrExam++Node Id."<<node->getId()<<"  "<<node->getDfNum()<<std::endl;
            Log::out() << "==";
            inst->print(Log::out()); 
            Log::out() << std::endl;
        }

        switch (inst->getOpcode()) {
            case Op_NewObj:          // newobj
            case Op_NewArray:        // newarray
            case Op_NewMultiArray:   // newmultiarray
                ntype=NT_OBJECT;        // for 3 cases above
            case Op_LdRef:           // ldref
            case Op_LdConstant:      // ldc
                if (ntype==0) 
                    ntype=NT_LDOBJ;         // loads refs
            case Op_DefArg:          // defarg
                if (ntype==0) {
                    ntype=NT_DEFARG;    // for Op_DefArg
                    defArgNumber++;
                }
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif
                if (inst->getDst()->getType()->isObject()) {
                    type=inst->getDst()->getType();
                    assert(findCnGNode_op(inst->getDst()->getId())==NULL);
                    cgnode = addCnGNode_op(inst,type,ntype);
                }
                break;

            case Op_Conv:     // conv
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif                
                if (inst->getDst()->getType()->isObject()) {
                    type=inst->getDst()->getType();
                    assert(findCnGNode_op(inst->getDst()->getId())==NULL);
                    cgnode = addCnGNode_op(inst,type,NT_OBJECT);
                }
                break;                

            case Op_LdFieldAddr:     // ldflda
                ntype=NT_INSTFLD;
            case Op_LdStaticAddr:    // ldsflda
                if (ntype==0)
                    ntype=NT_STFLD;    // for LdStaticAddr
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif
                FieldAccessInst* fainst;
                if ((fainst=inst->asFieldAccessInst())!=NULL) {
                    type=fainst->getFieldDesc()->getFieldType();  //field type
                    if (type->isReference()) {
                        assert(findCnGNode_op(inst->getDst()->getId())==NULL);
                        cgnode = addCnGNode_op(inst,type,NT_REF);
                        CnGNode* n = findCnGNode_fl(inst,ntype);
                        if (n==NULL) {
                            n = addCnGNode_fl(inst,ntype);
                        }
                        cgnode->lNode=n;   // stick nodes 
                        exam2Insts->push_back(inst);
                    } else {
                        if (ntype==NT_INSTFLD) {
                            assert(findCnGNode_op(inst->getDst()->getId())==NULL);
                            cgnode = addCnGNode_op(inst,type,NT_REF);
                            CnGNode* n = findCnGNode_fl(inst,ntype);
                            if (n==NULL) {
                                n = addCnGNode_fl(inst,ntype);
                            }
                            exam2Insts->push_back(inst);
                            cgnode->lNode=n;   // stick nodes 
                        }
                    }
                }
                break;

            case Op_TauLdInd:        // ldind
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif
                type = inst->getDst()->getType();
                if (type->isReference()) {   //isObject()) {
                    assert(findCnGNode_op(inst->getDst()->getId())==NULL);
                    cgnode = addCnGNode_op(inst,type,NT_LDOBJ);
                    exam2Insts->push_back(inst);
                }
                if (inst->getSrc(0)->getInst()->getOpcode()==Op_LdStaticAddr)
                    break;
                if (type->isValue()) {
                    assert(findCnGNode_op(inst->getDst()->getId())==NULL);
                    cgnode = addCnGNode_op(inst,type,NT_LDVAL);
                    exam2Insts->push_back(inst);
                }
                break;

            case Op_LdArrayBaseAddr: // ldbase
            case Op_AddScaledIndex:  // addindex
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif
                if (inst->getDst()->getType()->isReference()) {
                    type=inst->getDst()->getType();
                    assert(findCnGNode_op(inst->getDst()->getId())==NULL);
                    cgnode = addCnGNode_op(inst,type,NT_ARRELEM);
                    exam2Insts->push_back(inst);
                }
                break;

            case Op_VMHelperCall:      // callvmhelper
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif
                if (!inst->getDst()->isNull()) {
                    if (inst->getDst()->getType()->isObject()) {
                        CompilationInterface::RuntimeHelperId  callId = 
                            inst->asVMHelperCallInst()->getVMHelperId();
                        switch(callId) {
                            case CompilationInterface::Helper_GetTLSBase:
                                ntype = NT_ARRELEM;
                                break;
                            case CompilationInterface::Helper_NewObj_UsingVtable:
                            case CompilationInterface::Helper_NewVector_UsingVtable:
                                ntype = NT_OBJECT;
                                break;
                            default:
                                ntype = 0;
                                assert(0);
                        }
                        type=inst->getDst()->getType();
                        assert(findCnGNode_op(inst->getDst()->getId())==NULL);
                        cgnode = addCnGNode_op(inst,type,ntype);
                    }
                }
#ifdef _DEBUG 
                if (_seinfo) {
                    CompilationInterface::RuntimeHelperId  callId = 
                        inst->asVMHelperCallInst()->getVMHelperId();
                    if (method_ea_level == 0 && callId != CompilationInterface::Helper_Throw_Lazy) {

                        Log::out() <<"iE:        callvmhelper    ";
                        inst->print(Log::out()); 
                        Log::out()<<std::endl;
                    }
                }
#endif
                break;

            case Op_JitHelperCall:      // calljithelper
                if (method_ea_level == 0) {
                    Log::out() <<"iE:        calljithelper    ";
                    inst->print(Log::out()); 
                    Log::out()<<std::endl;
                    switch(inst->asJitHelperCallInst()->getJitHelperId()) {
                        case InitializeArray:
                        case FillArrayWithConst:
                        case SaveThisState:
                        case ReadThisState:
                        case LockedCompareAndExchange:
                        case AddValueProfileValue:
                            break;
                        default:
                            assert(0);
                    }
                }
                break;

            case Op_TauStInd:        // stind    
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif
                if ((type=inst->getSrc(0)->getType())->isObject() || type->isValue()) {
                    exam2Insts->push_back(inst);
                }
                break;

            case Op_DirectCall:      // call
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif
                if (!inst->getDst()->isNull()) {
                    if (inst->getDst()->getType()->isObject()) {
                        type=inst->getDst()->getType();
                        assert(findCnGNode_op(inst->getDst()->getId())==NULL);
                        cgnode = addCnGNode_op(inst,type,NT_RETVAL);
                    }
                }
                md=inst->asMethodInst()->getMethodDesc();
                n=md->getNumParams();
                addinst=false;
                for (uint32 i = 0; i < n; i++) {
                    Type* tt = md->getParamType(i);
                    if (!tt->isReference()) {
                        continue;
                    }
                    addinst=true;
                    assert(findCnGNode_mp(inst->getId(),i)==NULL);
                    cgnode = addCnGNode_mp(inst,md,NT_ACTARG, i);
                }
                if (addinst) {
                    exam2Insts->push_back(inst);
                }
#ifdef _DEBUG  
                if (_seinfo) {
                    if (method_ea_level == 0) {
                        Log::out() <<"iE:        call        ";
                        if (md->isSynchronized())
                            Log::out() << "sync   "; 
                        if (md->isStatic())
                            Log::out() << "stat   ";
                        if (md->isNative())
                            Log::out() << "native   ";
                        inst->print(Log::out()); 
                        Log::out()<<std::endl;
                        Log::out() << "           ";
                        md->printFullName(Log::out()); 
                        Log::out() << std::endl;
                    }
                }
#endif
                break;

            case Op_IndirectMemoryCall:  //callimem
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif
                if (!inst->getDst()->isNull()) {
                    if (inst->getDst()->getType()->isObject()) {
                        type=inst->getDst()->getType();
                        assert(findCnGNode_op(inst->getDst()->getId())==NULL);
                        cgnode = addCnGNode_op(inst,type,NT_RETVAL);
                    }
                }
                method_inst=inst->getSrc(0)->getInst();
                if (method_inst->getOpcode() == Op_LdVar) {
                    Opnd* funptr = inst->getSrc(0);
                    Type* optype = funptr->getType();
                    MethodPtrType* opmtype = optype->asMethodPtrType();
                    md = opmtype->getMethodDesc();
                } else {
                    md=method_inst->asMethodInst()->getMethodDesc();
                }
                n=md->getNumParams();
                addinst=false;
                for (uint32 i = 0; i < n; i++) {
                    Type* tt = md->getParamType(i);
                    if (!tt->isReference()) {
                        continue;
                    }
                    addinst=true;
                    assert(findCnGNode_mp(inst->getId(),i)==NULL);
                    cgnode = addCnGNode_mp(inst,md,NT_ACTARG,i);
                }
                if (addinst) {
                    exam2Insts->push_back(inst);
                }
#ifdef _DEBUG 
                if (_seinfo) {
                    if (method_ea_level == 0) {
                        Log::out() <<"iE:        callimem    ";
                        if (md->isSynchronized())
                            Log::out() << "sync   "; 
                        if (md->isStatic())
                            Log::out() << "stat   ";
                        if (md->isNative())
                            Log::out() << "native   ";
                        inst->print(Log::out()); 
                        Log::out()<<std::endl;
                        Log::out() << "           ";
                        md->printFullName(Log::out()); 
                        Log::out() << std::endl;
                    }
                }
#endif
                break;

            case Op_IntrinsicCall:   // callintr
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif

                if (!inst->getDst()->isNull()) {
                    assert(0);
                }
                n=inst->getNumSrcOperands();
                addinst=false;
                for (uint32 i = 0; i < n; i++) {
                    Type* tt = inst->getSrc(i)->getType();
                    if (!tt->isReference()) {
                        continue;
                    }
                    addinst=true;
                    break;
                }
                if (addinst) {
                    exam2Insts->push_back(inst);
                }

#ifdef _DEBUG 
                if (_seinfo) {
                    if (method_ea_level == 0) {
                        Log::out() <<"iE:        callintr    ";
                        inst->print(Log::out()); 
                        Log::out()<<std::endl;
                    }
                }
#endif
                break;

            case Op_Catch:           // catch
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif
                if (inst->getDst()->getType()->isObject()) {
                    type=inst->getDst()->getType();
                    assert(findCnGNode_op(inst->getDst()->getId())==NULL);
                    cgnode = addCnGNode_op(inst,type,NT_CATCHVAL);
                }
                break;

            case Op_StVar:           // stvar
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif
                type = inst->getDst()->getType();
                if (type->isObject()) {
                    assert(findCnGNode_op(inst->getDst()->getId())==NULL);
                    cgnode = addCnGNode_op(inst,type,NT_VARVAL);
                    exam2Insts->push_back(inst);
                }    
                break;

            case Op_Phi:             // phi
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif
                type = inst->getDst()->getType();
                if (type->isReference()) {
                    assert(findCnGNode_op(inst->getDst()->getId())==NULL);
                    cgnode = addCnGNode_op(inst,type,NT_VARVAL);
                    exam2Insts->push_back(inst);
                }
                break;

            case Op_LdVar:             // ldvar
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif
                type = inst->getDst()->getType();
                if (type->isReference()) {
                    assert(findCnGNode_op(inst->getDst()->getId())==NULL);
                    cgnode = addCnGNode_op(inst,type,NT_LDOBJ);
                    exam2Insts->push_back(inst);
                }
                break;

            case Op_Return:          // return     
                ntype=NT_EXITVAL;
            case Op_Throw:           // throw
                if (ntype==0)
                    ntype=NT_THRVAL;
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif
                if (inst->getNumSrcOperands()>0) {
                    if (inst->getSrc(0)->getType()->isObject()) {
                        assert(findCnGNode_in(inst->getId())==NULL);
                        cgnode = addCnGNode_ex(inst,ntype);
                        exam2Insts->push_back(inst);
                    }
                }
                break;

            case Op_TauStaticCast:   // staticcast
            case Op_TauCast:         // cast
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif
                type = inst->getDst()->getType();
                if (type->isObject()) {
                    assert(findCnGNode_op(inst->getDst()->getId())==NULL);
                    cgnode = addCnGNode_op(inst,type,NT_REF);
                    exam2Insts->push_back(inst);
                }
                break;

            case Op_SaveRet:           // saveret
#ifdef _DEBUG
                if (_instrInfo) {
                    Log::out() <<"Node Id."<<node->getId()<<std::endl;
                    debug_inst_info(inst,Log::out()); 
                }
#endif
                type = inst->getDst()->getType();
                if (type->isIntPtr()) {
                    cgnode = findCnGNode_op(inst->getDst()->getId());
                    if (cgnode == NULL)
                        cgnode = addCnGNode_op(inst,type,NT_INTPTR);
                }
                break;

            case Op_TauMonitorEnter: // monenter
            case Op_TauMonitorExit:  // monexit
                if (do_sync_removal && method_ea_level == 0) {
                    addMonInst(inst);
#ifdef _DEBUG
                    if (_seinfo) {
                        Log::out() << "iE: monX  Node Id."
                            << node->getId() << "  ";
                        inst->print(Log::out()); 
                        Log::out()<<std::endl;
                    }
#endif
                }
                break;

            case Op_TypeMonitorEnter:// tmonenter
            case Op_TypeMonitorExit: // tmonexit
                break;

            case Op_TauVirtualCall:  // callvirt
            case Op_IndirectCall:    // calli

            case Op_TauStRef:  
            case Op_TauStField:     
            case Op_TauStElem:     
            case Op_TauStStatic:
            case Op_Copy:       

            case Op_Box:

                if (_instrInfo) {
                    Log::out() <<"--Node Id."<<node->getId()<<std::endl;
                    Log::out() << "    =="; 
                    inst->print(Log::out()); 
                    Log::out() << std::endl;
                }
                break;

            default:

                if (_instrInfo) {
                    Log::out() <<"~~Node Id."<<node->getId()<<std::endl;
                    Log::out() << "    =="; 
                    inst->print(Log::out()); 
                    Log::out() << std::endl;
                }
        }
    }
    return;
}  // instrExam(Node* node) 


void 
EscAnalyzer::instrExam2() {
    Insts *instrs = exam2Insts;
    int insnum=0;
    Insts::iterator it;
    Inst* inst;
    Type* type;
    CnGNode* cgnode;
    CnGNode* cgn_src;
    uint32 ntype=0;
    MethodDesc* md;
    uint32 n;
    Inst* method_inst;
    bool not_exam = false;

    for (it = instrs->begin( ); it != instrs->end( ); it++ ) {
        inst=*it;
        insnum++;
        ntype=0;

#ifdef _DEBUG
        if (_instrInfo2) {
            Log::out() <<"instrExam2:  Node Id."<<inst->getNode()->getId()<< " ";
            FlowGraph::printLabel(Log::out(),inst->getNode()); Log::out() << std::endl;
            inst->print(Log::out()); Log::out() << std::endl;
        }
#endif

        switch (inst->getOpcode()) {
            case Op_LdFieldAddr:     // ldflda
                ntype=NT_INSTFLD;
            case Op_LdStaticAddr:    // ldsflda
                if (ntype==0)
                    ntype=NT_STFLD;    // for LdStaticAddr
                FieldAccessInst* fainst;
                if ((fainst=inst->asFieldAccessInst())!=NULL) {
                    bool isref = fainst->getFieldDesc()->getFieldType()->isReference();
                    if (isref || ntype==NT_INSTFLD) {
                        cgn_src=findCnGNode_op(inst->getDst()->getId());  //  field address node
                        if (isref)
                            assert(cgn_src!=NULL);
                        cgnode=findCnGNode_fl(inst,ntype);    //  field node
                        assert(cgnode!=NULL);
                        if (ntype == NT_INSTFLD) {
                            cgn_src=findCnGNode_op(inst->getSrc(0)->getId());  // instance node
                            //  adding fld edge for ldflda
                            addEdge(cgn_src,cgnode,ET_FIELD,inst);
                            // special for java/lang/String::value
                            FieldDesc* fd=inst->asFieldAccessInst()->getFieldDesc();
                            if (fd->getParentType()->isSystemString()&&strcmp(fd->getName(),"value")==0) {
                                addEdge(cgnode,cgn_src,ET_DEFER,inst);
                            }
                            if (method_ea_level == 0 && cgn_src->nInst->getOpcode()==Op_NewObj) {
                                //fail if instance type is not compatible with 
                                //store/load type. Incompatibility can be the result of not cleaned dead code.
                                //see HARMONY-4115 for details.
                                Inst* instantceInst = cgn_src->nInst;
                                ObjectType* instanceType = instantceInst->asTypeInst()->getTypeInfo()->asObjectType();
                                assert(instanceType!=NULL);
                                ObjectType* fieldObjectType = fd->getParentType()->asObjectType();
                                assert(fieldObjectType!=NULL);
                                if (!instanceType->isSubClassOf(fieldObjectType)) {
                                    if (Log::isEnabled()) {
                                        Log::out()<<"FAILURE: instance type: "<<instanceType->getName();
                                        Log::out()<<" is not compatible with field object type: "<<fieldObjectType->getName()<<std::endl;
                                        Log::out()<<"Instance inst: ";instantceInst->print(Log::out());
                                        Log::out()<<" field inst: ";inst->print(Log::out()); Log::out()<<std::endl;
                                    }
                                    assert(0);
                                    Jitrino::crash("Jitrino failure:escape: illegal HIR sequence");
                                }
                            }
                        }
                    }
                }
                break;

            case Op_TauLdInd:        // ldind
                type=inst->getDst()->getType();
                if (type->isObject()) {
                    cgnode=findCnGNode_op(inst->getDst()->getId());
                    assert(cgnode!=NULL);
                    // ref to loaded object
                    cgn_src=findCnGNode_op(inst->getSrc(0)->getId());
                    assert(cgn_src!=NULL);
                    addEdge(cgn_src,cgnode,ET_POINT,inst);  
                }
                if (type->isValue()) {
                    cgnode=findCnGNode_op(inst->getDst()->getId());
                    assert(cgnode!=NULL);
                    // ref to loaded object
                    cgn_src=findCnGNode_op(inst->getSrc(0)->getId());
                    assert(cgn_src!=NULL);
                    addEdge(cgn_src,cgnode,ET_POINT,inst);  
                }
                break;

            case Op_LdArrayBaseAddr: // ldbase
            case Op_AddScaledIndex:  // addindex
                if (inst->getDst()->getType()->isReference()) {
                    cgnode=findCnGNode_op(inst->getDst()->getId());
                    assert(cgnode!=NULL);
                    // ref to loaded address
                    if (inst->getOpcode()==Op_LdArrayBaseAddr) {
                        cgn_src=findCnGNode_op(inst->getSrc(0)->getId());
                        assert(cgn_src!=NULL);
                        addEdge(cgn_src,cgnode,ET_FIELD,inst); // ref to base element
                    } 
                    if (inst->getOpcode()==Op_AddScaledIndex) {
                        if (inst->getSrc(0)->getInst()->getOpcode()==Op_LdArrayBaseAddr) {
                            cgn_src=findCnGNode_op(
                                inst->getSrc(0)->getInst()->getSrc(0)->getId());
                            assert(cgn_src!=NULL);
                            addEdge(cgn_src,cgnode,ET_FIELD,inst); // ref from array object to inner objects
                        }
                    }
                }
                break;

            case Op_TauStInd:        // stind    
                if (inst->getSrc(0)->getType()->isObject()) {
                    // ref to loaded address
                    cgn_src=findCnGNode_op(inst->getSrc(0)->getId());
                    assert(cgn_src!=NULL);
                    cgnode=findCnGNode_op(inst->getSrc(1)->getId());
                    assert(cgnode!=NULL);
                    addEdge(cgnode,cgn_src,ET_DEFER,inst); 
                    break;
                }
                if ((type=inst->getSrc(0)->getType())->isValue()) {
                    uint32 src_opcode = inst->getSrc(1)->getInst()->getOpcode();
                    if (src_opcode==Op_LdStaticAddr)
                        break;
                    cgn_src=findCnGNode_op(inst->getSrc(0)->getId());
                    if (cgn_src==NULL) {
                        cgn_src = addCnGNode_op(inst->getSrc(0)->getInst(),type,NT_LDVAL);
                    }
                    cgnode=findCnGNode_op(inst->getSrc(1)->getId());
                    assert(cgnode!=NULL);
                    addEdge(cgnode,cgn_src,ET_DEFER,inst); 
                }
                break;

            case Op_DirectCall:      // call
                md=inst->asMethodInst()->getMethodDesc();
                n=md->getNumParams();
                for (uint32 i = 0; i < n; i++) {
                    Type* tt = md->getParamType(i);
                    if (!tt->isReference()) 
                        continue;
                    cgnode=findCnGNode_mp(inst->getId(),i);
                    assert(cgnode!=NULL);
                    cgn_src=findCnGNode_op(inst->getSrc(2+i)->getId());
                    assert(cgn_src!=NULL);
                    addEdge(cgnode,cgn_src,ET_DEFER,inst); 
                }
                break;

            case Op_IndirectMemoryCall:  //callimem
                method_inst=inst->getSrc(0)->getInst();
                if (method_inst->getOpcode() == Op_LdVar) {
                    MethodPtrType* mpt = inst->getSrc(0)->getType()->asMethodPtrType();
                    md = mpt->getMethodDesc();
                } else {
                    md=method_inst->asMethodInst()->getMethodDesc();
                }
                n=md->getNumParams();
                for (uint32 i = 0; i < n; i++) {
                    Type* tt = md->getParamType(i);
                    if (!tt->isReference()) 
                        continue;
                    cgnode = findCnGNode_mp(inst->getId(),i);
                    assert(cgnode!=NULL);
                    cgn_src=findCnGNode_op(inst->getSrc(3+i)->getId());
                    assert(cgn_src!=NULL);
                    addEdge(cgnode,cgn_src,ET_DEFER,inst); 
                }
                break;

            case Op_IntrinsicCall:   // callintr
                n=inst->getNumSrcOperands();
                switch(inst->asIntrinsicCallInst()->getIntrinsicId()) {
                    case ArrayCopyDirect:
                    case ArrayCopyReverse:
                         cgnode = findCnGNode_op(inst->getSrc(4)->getId());
                         assert(cgnode!=NULL);
                         cgn_src = findCnGNode_op(inst->getSrc(2)->getId());
                         assert(cgn_src!=NULL);
                         addEdge(cgnode,cgn_src,ET_DEFER,inst);
                    Log::out() << "need to add edge from "<< cgnode->cngNodeId << " - " << cgnode->opndId << " to "
                        << cgn_src->cngNodeId << " - " << cgn_src->opndId << std::endl;
                         break;
                    default:
                         assert(0);
                }
                break;

            case Op_StVar:           // stvar
                if (inst->getDst()->getType()->isObject()) {
                    cgnode = findCnGNode_op(inst->getDst()->getId());
                    assert(cgnode!=NULL);
                    cgn_src=findCnGNode_op(inst->getSrc(0)->getId());
                    assert(cgn_src!=NULL);
                    addEdge(cgn_src,cgnode,ET_DEFER,inst);
                }
                break;

            case Op_Phi:             // phi
                if (inst->getDst()->getType()->isObject()) {
                    uint32 nsrc=inst->getNumSrcOperands();
                    cgnode = findCnGNode_op(inst->getDst()->getId());
                    assert(cgnode!=NULL);
                    for (uint32 i=0; i<nsrc; i++) {
                        cgn_src=findCnGNode_op(inst->getSrc(i)->getId());
                        assert(cgn_src!=NULL);
                        addEdge(cgn_src,cgnode,ET_DEFER,inst);
                    }
                }
                break;

            case Op_LdVar:             // ldvar
                type = inst->getDst()->getType();
                if (type->isReference()) {
                    cgnode = findCnGNode_op(inst->getDst()->getId());
                    assert(cgnode!=NULL);
                    cgn_src=findCnGNode_op(inst->getSrc(0)->getId());
                    assert(cgn_src!=NULL);
                    addEdge(cgn_src,cgnode,ET_DEFER,inst);   // load ldobj
                }
                break;

            case Op_Return:          // return     
                ntype=NT_EXITVAL;
            case Op_Throw:           // throw
                if (ntype==0)
                    ntype=NT_THRVAL;
                if (inst->getNumSrcOperands()>0) {
                    cgnode = findCnGNode_in(inst->getId());
                    assert(cgnode!=NULL);
                    cgn_src=findCnGNode_op(inst->getSrc(0)->getId());
                    assert(cgn_src!=NULL);
                    addEdge(cgnode,cgn_src,ET_DEFER,inst); 
                }
                break;

            case Op_TauStaticCast:   // staticcast
            case Op_TauCast:         // cast
                type = inst->getDst()->getType();
                cgnode = findCnGNode_op(inst->getDst()->getId());
                assert(cgnode!=NULL);
                if (cgnode->lNode == NULL) {
                    cgn_src=findCnGNode_op(inst->getSrc(0)->getId());
                    assert(cgn_src!=NULL);
                    cgnode->lNode = cgn_src;
                }
                break;

            case Op_TauMonitorEnter: // monenter
            case Op_TauMonitorExit:  // monexit
            case Op_TypeMonitorEnter:// tmonenter
            case Op_TypeMonitorExit: // tmonexit
                break;

            default:
                if (_instrInfo2) {
                    not_exam = true;
                }
        }
        if (_instrInfo2) {
            if (not_exam) {
                Log::out() <<"!!! Not examined. ";
                inst->print(Log::out()); Log::out() << std::endl;
                not_exam = false;
            }
        }
    }
    return;
}  // instrExam2() 


EscAnalyzer::CnGNode*
EscAnalyzer::addCnGNode(Inst* inst, Type* type, uint32 ntype) {
    CnGNode* cgnode = new (eaMemManager) CnGNode; // new CG node

    cgnode->cngNodeId = ++lastCnGNodeId;
    cgnode->instrId = inst->getId();
    cgnode->nodeType = ntype;
    cgnode->lNode = NULL;
    cgnode->nInst = inst;
    cgnode->outEdges = NULL;
    if (cgnode->nodeType==NT_DEFARG)
        cgnode->argNumber = defArgNumber;   // number of formal parameter
    else
        cgnode->argNumber = 0; 
    if ((ntype==NT_STFLD)||ntype==NT_THRVAL)
        setFullState(cgnode,GLOBAL_ESCAPE);
    else {
        if (ntype==NT_ACTARG) {
            setFullState(cgnode,ARG_ESCAPE);
            if (inst->getOpcode()==Op_IndirectMemoryCall)
                setVirtualCall(cgnode);
        } else
            setFullState(cgnode,NO_ESCAPE);
    }
    if (ntype==NT_EXITVAL)
        setOutEscaped(cgnode);
    cgnode->nodeMDs = NULL;
    if (type->isReference()) {
        if (type->isArray()) {
            if (type->asArrayType()->getElementType()->isReference()) {
                cgnode->nodeRefType = NR_REFARR;
            } else
                cgnode->nodeRefType = NR_ARR;
        } else
            cgnode->nodeRefType = NR_REF;
        if (ntype&(NT_OBJECT|NT_RETVAL)||ntype==NT_LDOBJ) { 
            cgnode->nodeMDs = new (eaMemManager) NodeMDs(eaMemManager);  // to collect methods receiving object
        }
    } else {
        cgnode->nodeRefType = NR_PRIM;
    }
    if (cgnode->nodeType==NT_OBJECT && cgnode->nodeRefType == NR_REF) {
        NamedType* nt = (NamedType*)(inst->getDst())->getType();
        if (nt->isFinalizable()) {  // finalized objects cannot be removed
            setOutEscaped(cgnode);
        }
    }
    cngNodes->push_back(cgnode);
    return cgnode;
}  // addCnGNode(Inst* inst, Type* type, uint32 ntype)


EscAnalyzer::CnGNode*
EscAnalyzer::addCnGNode_op(Inst* inst, Type* type, uint32 ntype) {
    CnGNode* cgnode = addCnGNode(inst, type, ntype); // new CG node
    Opnd* opnd = inst->getDst();

    cgnode->opndId = opnd->getId();
    cgnode->refObj = opnd;
#ifdef _DEBUG
    if (_cngnodes) {
        Log::out() <<"++++ addNode  "; printCnGNode(cgnode,Log::out());
        Log::out() << std::endl;
    }
#endif
    return cgnode;
}  // addCnGNode_op(Inst* inst, Type* type, uint32 ntype) 


EscAnalyzer::CnGNode*
EscAnalyzer::addCnGNode_mp(Inst* inst, MethodDesc* md, uint32 ntype, uint32 narg) {
    Type* type = md->getParamType(narg);
    CnGNode* cgnode = addCnGNode(inst, type, ntype); // new CG node

    cgnode->opndId = 0;
    cgnode->argNumber = narg;
    cgnode->refObj = md;
#ifdef _DEBUG
    if (_cngnodes) {
        Log::out() <<"++++ addNode  "; printCnGNode(cgnode,Log::out());
        Log::out() << std::endl;
    }
#endif
    return cgnode;
}  // addCnGNode_mp(Inst* inst, MethodDesc* md, uint32 ntype, uint32 narg) 


EscAnalyzer::CnGNode*
EscAnalyzer::addCnGNode_ex(Inst* inst, uint32 ntype) {
    Type* type = inst->getSrc(0)->getType();
    CnGNode* cgnode = addCnGNode(inst, type, ntype); // new CG node

    cgnode->opndId = 0;
    cgnode->refObj = inst->getSrc(0);   // returned or thrown operand
#ifdef _DEBUG
    if (_cngnodes) {
        Log::out() <<"++++ addNode  "; printCnGNode(cgnode,Log::out());
        Log::out() << std::endl;
    }
#endif
    return cgnode;
}  // addCnGNode_ex(Inst* inst, uint32 ntype) 


EscAnalyzer::CnGNode*
EscAnalyzer::addCnGNode_fl(Inst* inst, uint32 ntype) {
    Type* type = inst->getDst()->getType();
    CnGNode* cgnode = addCnGNode(inst, type, ntype); // new CG node

    cgnode->opndId = 0;
    cgnode->refObj = inst;   // returned or thrown operand
#ifdef _DEBUG
    if (_cngnodes) {
        Log::out() <<"++++ addNode  "; printCnGNode(cgnode,Log::out());
        Log::out() << std::endl;
    }
#endif
    return cgnode;
}  // addCnGNode_fl(Inst* inst, uint32 ntype) 


EscAnalyzer::CnGNode*
EscAnalyzer::findCnGNode_op(uint32 nId) {
    CnGNodes::iterator it;
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        if ((*it)->opndId==nId && ((*it)->nodeType & (NT_OBJS|NT_LDVAL)))
            return (*it);
    }
    return(NULL);
}  // findCnGNode_op(uint32 nId) 


EscAnalyzer::CnGNode*
EscAnalyzer::findCnGNode_id(uint32 nId) {
    CnGNodes::iterator it;
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        if ((*it)->cngNodeId==nId)
            return (*it);
    }
    return(NULL);
}  // findCnGNode_id(uint32 nId) 


EscAnalyzer::CnGNode*
EscAnalyzer::findCnGNode_in(uint32 nId) {
    CnGNodes::iterator it;
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        if ((*it)->instrId==nId)
            return (*it);
    }
    return(NULL);
}  // findCnGNode_in(uint32 nId) 


EscAnalyzer::CnGNode*
EscAnalyzer::findCnGNode_mp(uint32 iId, uint32 aId) {
    CnGNodes::iterator it;
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        if ((*it)->instrId==iId && (*it)->argNumber==aId &&
                (*it)->nodeType == NT_ACTARG)
            return (*it);
    }
    return(NULL);
}  // findCnGNode_mp(uint32 iId, uint32 aId) 


EscAnalyzer::CnGNode*
EscAnalyzer::findCnGNode_fl(Inst* inst, uint32 ntype) {
    CnGNodes::iterator it;
    FieldDesc* fd1;
    FieldDesc* fd2;
    uint32 idr = 0;
    if (ntype==NT_INSTFLD)
        idr=inst->getSrc(0)->getId();
    assert(inst->asFieldAccessInst()!=NULL);
    fd1 = inst->asFieldAccessInst()->getFieldDesc();
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        if ((*it)->nodeType==ntype) {
            assert(((Inst*)((*it)->refObj))->asFieldAccessInst()!=NULL);
            fd2 = ((Inst*)((*it)->refObj))->asFieldAccessInst()->getFieldDesc();
            if ( fd1->getParentType()==fd2->getParentType() && 
                    strcmp(fd1->getName(),fd2->getName())==0) {
                if (ntype==NT_INSTFLD) {
                    uint32 idf=((Inst*)((*it)->refObj))->getSrc(0)->getId();
                    if (idr!=idf) {
#ifdef _DEBUG
                        if (_cngedges) {
                            Log::out()
                                << "++++ findCnGNode_fl: required " << idr 
                                << " - found  " << idf << " not found" << std::endl;
                        }
#endif
                        continue;
                    }
#ifdef _DEBUG
                    if (_cngedges) {
                        Log::out()
                            << "++++ findCnGNode_fl: required " << idr
                            << " - found  " << idf << std::endl;
                    }
#endif
                }
                return (*it);
            }
        }
    }
    return(NULL);
}  // findCnGNode_fl(Opnd* opnd, uint32 ntype) 


void
EscAnalyzer::addEdge(CnGNode* cgnfrom, CnGNode* cgnto, 
                                    uint32 etype, Inst* inst) {
    CnGEdges::iterator it;
    CnGRefs* el = NULL;
    CnGRef* ref;
    CnGNode* cgn1=cgnfrom;
    CnGNode* cgn2=cgnto;

    if (cgnfrom->lNode) {
        cgn1 = findCnGNode_id(cgnfrom->lNode->cngNodeId); // to find CnG node using CnG node Id
        assert(cgn1!=NULL);
    }
    if (cgnfrom->nodeType == NT_REF && cgnfrom->lNode == NULL) {
        assert(cgnfrom->nInst->getOpcode()==Op_TauCast || cgnfrom->nInst->getOpcode()==Op_TauStaticCast);
        cgn1 = findCnGNode_op(cgnfrom->nInst->getSrc(0)->getId());
        assert(cgn1!=NULL);
        cgnfrom->lNode = cgn1;
    }

#ifdef _DEBUG
    if (_cngedges) {
        Log::out() 
            << "++++ addEdge: " << cgnfrom->cngNodeId << "-" << cgnfrom->opndId
            << " ( "<<cgn1->cngNodeId << "-" << cgn1->opndId << " ) to "
            << cgnto->cngNodeId << "-" << cgnto->opndId << " ( "
            << cgn2->cngNodeId << "-" << cgn2->opndId << " )" << std::endl;
    }
#endif
    if (cgn1==cgn2) {
#ifdef _DEBUG
       if (_cngedges) {
            Log::out() << "+++++++ equal " 
                << cgnfrom->cngNodeId<< "-" << cgnfrom->opndId
                << " ( "<<cgn1->cngNodeId << "-" << cgn1->opndId << " ) to "
                << cgnto->cngNodeId << "-" << cgnto->opndId << " ( "
                << cgn2->cngNodeId << "-" << cgn2->opndId << " )" << std::endl;
        }
#endif
        return;
    }
    for ( it = cngEdges->begin( ); it != cngEdges->end( ); it++ ) {
        if ((*it)->cngNodeFrom == cgn1) {
            CnGRefs::iterator itr;
            for ( itr = (*it)->refList->begin( ); itr != (*it)->refList->end( ); itr++ ) {
                if ((*itr)->cngNodeTo == cgn2 && (*itr)->edgeType == etype && cgn1->nodeType != NT_INSTFLD) {
                    if (etype==ET_FIELD || cgn1->nodeType==NT_ACTARG) {
#ifdef _DEBUG
                        if (_cngedges) {
                            Log::out() << "++++ addEdge: ET_FIELD || cgn1==NT_ACTARG && *->cngNodeTo == cgn2" <<  std::endl;
                        }
#endif
                        return;
                    }
                    if (inst->getOpcode() == Op_IntrinsicCall) { //callintr
                        uint32 iid = inst->asIntrinsicCallInst()->getIntrinsicId();
                        switch(iid) {
                            case ArrayCopyDirect:
                            case ArrayCopyReverse:
#ifdef _DEBUG
                                if (_cngedges) {
                                    Log::out() << "++++ addEdge: callintr" <<  std::endl;
                                }
#endif
                                return;
                            default:
                               assert(0);
                        }
                    }
                    if (inst->getOpcode() == Op_LdFieldAddr) {
                        FieldDesc* fd=inst->asFieldAccessInst()->getFieldDesc();
                        if (fd->getParentType()->isSystemString()&&strcmp(fd->getName(),"value")==0) {
#ifdef _DEBUG
                            if (_cngedges) {
                                Log::out() << "++++ addEdge: ldflda String.value" <<  std::endl;
                            }
#endif
                            return;
                        }
                    }
#ifdef _DEBUG
                    if (_cngedges) {
                        Log::out() << "++++ addEdge: edge already exists and new is added" <<  std::endl;
                    }
#endif
                }
            }
            ref = new (eaMemManager) CnGRef;
            ref->cngNodeTo=cgn2;
            ref->edgeType=etype;
            ref->edgeInst=inst;
            (*it)->refList->push_back(ref);
#ifdef _DEBUG
            if (_cngedges) {
                Log::out() << "++++ addEdge: added CnGRef" <<  std::endl;
            }
#endif
            return;
        } 
    }
    ref = new (eaMemManager) CnGRef;
    ref->cngNodeTo=cgn2;
    ref->edgeType=etype;
    ref->edgeInst=inst;
    CnGEdge* cgedge=new (eaMemManager) CnGEdge;
    el=new CnGRefs(eaMemManager);
    cgedge->cngNodeFrom=cgn1;
    el->push_back(ref);
    cgedge->refList=el;
    cngEdges->push_back(cgedge);
    cgn1->outEdges=el;
#ifdef _DEBUG
    if (_cngedges) {
        Log::out() << "++++ addEdge: added edge" <<  std::endl;
    }
#endif

}  // addEdge(CnGNode* cgnfrom, CnGNode* cgnto, uint32 etype, Inst* inst) 


void
EscAnalyzer::setCreatedObjectStates() {
    CnGNodes::iterator it;
    NodeMDs::iterator it1;

    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        uint32 nt = (*it)->nodeType;
        if (nt==NT_STFLD || nt==NT_CATCHVAL
            || (nt==NT_RETVAL && (*it)->nInst->getOpcode()==Op_IndirectMemoryCall)
            || (nt==NT_RETVAL && (*it)->nInst->asMethodInst()->getMethodDesc()->isNative())) {
            initNodeType = NT_STFLD;
#ifdef _DEBUG
            if (_setState) {
                Log::out() <<"-- before scanGE:  nodeId "
                    <<(*it)->cngNodeId<<"  opId "<<(*it)->opndId<<" state ";
                printState(*it); Log::out() << std::endl;
            }
#endif
            scanCnGNodeRefsGE(*it,false);
        }
    }
    scannedObjs->clear();
    scannedObjsRev->clear();
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        if ((*it)->nodeType&NT_EXITVAL || (*it)->nodeType==NT_DEFARG) {  // returned, thrown , defarg
#ifdef _DEBUG
            if (_setState) {
                Log::out() <<"-- before scanEVDA:  nodeId "
                    <<(*it)->cngNodeId<<"  opId "<<(*it)->opndId<<" state ";
                printState(*it); Log::out() << std::endl;
            }
#endif
            initNodeType = (*it)->nodeType;
            scanCnGNodeRefsGE(*it,false);
            scannedObjs->clear();
            scannedObjsRev->clear();
        }
    }
    scannedObjs->clear();
    scannedObjsRev->clear();
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        if ((*it)->nodeType==NT_ACTARG) {
            curMDNode=(*it)->cngNodeId;
#ifdef _DEBUG
            if (_setState) {
                Log::out() <<"-- before scanAE:  nodeId "
                    <<(*it)->cngNodeId<<"  opId "<<(*it)->opndId<<" state ";
                printState(*it);  Log::out() << std::endl;
            }
#endif
            initNodeType = NT_ACTARG;
            scanCnGNodeRefsAE(*it,false);
            scannedObjs->clear();
            scannedObjsRev->clear();
        }
    }
    scannedObjs->clear();
    scannedObjsRev->clear();
    if (method_ea_level==0) {
        DominatorTree* dominatorTree = irManager.getDominatorTree();
        if (!(dominatorTree && dominatorTree->isValid())) {
             OptPass::computeDominators(irManager);
             dominatorTree = irManager.getDominatorTree();
        }
        if (dominatorTree && dominatorTree->isValid()) {
            OptPass::computeLoops(irManager,false);
            LoopTree* ltree = irManager.getLoopTree();
            if (ltree->isValid())
                for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
                    if ( (*it)->nodeType==NT_OBJECT) {
                        if (ltree->getLoopHeader((*it)->nInst->getNode(),false)) {
#ifdef _DEBUG
                            if (_setState) {
                                Log::out() 
                                    <<"--setSt loop:  nodeId "
                                    <<(*it)->cngNodeId<<"  opId "
                                    <<(*it)->opndId<<" state ";
                                printState(*it);
                                Log::out() <<" to set loop" << std::endl;
                            }
#endif
                            setLoopCreated(*it);
                            comObjStat._n_lo++;
                        }
                    }
                }
        } else {
#ifdef _DEBUG
            mh.printFullName(Log::out()); Log::out() << std::endl;
            Log::out() << "DominatorTree isn't valid  " << std::endl;
#endif
        }
    }
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        uint32 nt = (*it)->nodeType;
        if ((nt&(NT_OBJECT|NT_RETVAL) || nt==NT_LDOBJ) && (getEscState(*it)==ARG_ESCAPE)) {
            for (it1 = (*it)->nodeMDs->begin(); it1 != (*it)->nodeMDs->end(); 
                    it1++) {
                CnGNode* n=findCnGNode_id(*it1);   // method argument node
                assert(n!=NULL);
                MethodDesc* mdesc = (MethodDesc*)n->refObj;
                Inst* callInst = n->nInst;
#ifdef _DEBUG
                if (_setState) {
                    Log::out()<<"--setSt chk arg_esc:  nodeId " <<(*it)->cngNodeId
                        <<"  opId " <<(*it)->opndId<<" state "; 
                    printState(*it); Log::out() << std::endl; 
                    Log::out() << "       "; callInst->print(Log::out());
                    Log::out() << std::endl;
                }
#endif
                if (mdesc->isNative()) {  // not scanned native methods 
                    setEscState(*it,GLOBAL_ESCAPE);
#ifdef _DEBUG
                    if (_scanMtds==1) {
                        mdesc->printFullName(Log::out());
                        Log::out() << std::endl;
                        Log::out() << "    isNative: " 
                            << mdesc->isNative() << std::endl;
                    }
                    if (_setState) {
                        Log::out() <<"--setSt 1:  nodeId "
                            <<(*it)->cngNodeId<<"  opId "<<(*it)->opndId
                            <<" state ";
                        printState(*it); 
                        Log::out()<<" to gl.esc."<< std::endl;
                    }
#endif
                    break;
                }
                if (callInst->getOpcode()!=Op_DirectCall) {   // not direct call
                    setVirtualCall(*it);
                    if (method_ea_level == 0 && do_sync_removal) {
                        MonUnit* mu = NULL;
                        if (monitorInstUnits!=NULL)
                            mu = findMonUnit((*it)->opndId);
                        if (mu != NULL) {
                            addMonUnitVCall(mu,callInst);
#ifdef _DEBUG
                            if (_seinfo) {
                                Log::out() << "=-=-=-=- callimem for this ";
                                Log::out() << std::endl;
                                Log::out() << "=-=- ";
                                printCnGNode(*it,Log::out());
                                Log::out() << std::endl;
                                printCnGNode(n,Log::out());
                                Log::out() << std::endl;
                                callInst->print(Log::out());
                                Log::out() << std::endl;
                                mdesc->printFullName(Log::out());
                                Log::out() << std::endl;
                                Log::out() << "=-=-=-=- end " << std::endl;
                            }
#endif
                            continue;
                        }
                    }
#ifdef _DEBUG
                    if (_scanMtds==1) {
                        callInst->print(Log::out());
                        Log::out() << std::endl;
                        mdesc->printFullName(Log::out());
                        Log::out() << std::endl;
                        Log::out() << "    isStatic: " 
                            << mdesc->isStatic() << std::endl;
                        Log::out() << "    isFinal: " 
                            << mdesc->isFinal() << std::endl;
                        Log::out() << "---------------" << std::endl;
                        Log::out() << "    NumParams: " 
                            << mdesc->getNumParams()<< std::endl;
                        Log::out() << "    isInstance: " 
                            << mdesc->isInstance() << std::endl;
                        Log::out() << "    isVirtual: " 
                            << mdesc->isVirtual() << std::endl;
                        Log::out() << "    isAbstract: " 
                            << mdesc->isAbstract() << std::endl;
                        Log::out() << "    isInstanceInitializer: " 
                            << mdesc->isInstanceInitializer() << std::endl;
                        Log::out() << "    isOverridden: " 
                            << mdesc->isOverridden() << std::endl;
                    }
                    if (_setState) {
                        Log::out() <<"--setSt 2:  nodeId "
                            <<(*it)->cngNodeId<<"  opId "<<(*it)->opndId
                            <<" state ";
                        printState(*it);
                        Log::out() <<" to v.call." << std::endl;
                    }
#endif
                    continue; //break;
                }
                CalleeMethodInfo* mtdInfo = findMethodInfo(mdesc,callInst);
                if (mtdInfo == NULL) {    // no info about called method
                    setEscState(*it,GLOBAL_ESCAPE);
#ifdef _DEBUG
                    if (_setState) {
                        Log::out() <<"--setSt 3:  nodeId "
                            <<(*it)->cngNodeId<<"  opId "<<(*it)->opndId
                            <<" state ";
                        printState(*it);
                        Log::out() <<" to gl.esc." << std::endl;
                    }
#endif
                    break;
                } else {  // to use info about scanned method
                    uint32 narg = n->argNumber;
                    ParamInfos::iterator it2;
                    uint32 state = 0;
#ifdef _DEBUG
                    if (_setState) {
                        Log::out() << "--setSt cmi: method " 
                            << mtdInfo->methodIdent->parentName << "::"
                            << mtdInfo->methodIdent->name << " "
                            << mtdInfo->methodIdent->signature << std::endl;
                    }
#endif
                    for (it2 = mtdInfo->paramInfos->begin( ); 
                            it2 != mtdInfo->paramInfos->end( ); it2++) {
#ifdef _DEBUG
                        if (_setState) {
                            Log::out() 
                                <<(*it2)->paramNumber<<" == "<<narg<<" state "
                                <<(*it2)->state <<" < "<< getEscState(*it)<<"  ";
                            printState(*it);  Log::out() << std::endl;
                        }
#endif
                        if ((*it2)->paramNumber == narg) { //???to add scanning of contained obj
                            if ((state=(*it2)->state&ESC_MASK) < getEscState(*it)) {
#ifdef _DEBUG
                                if (_setState) {
                                    Log::out()<<"--setSt cmi1:  nodeId " 
                                        <<(*it)->cngNodeId<<"  opId "<<(*it)->opndId
                                        <<" state "; 
                                    printState(*it);
                                    Log::out() <<" to state " <<state<< std::endl;
                                }
#endif
                                setEscState(*it,state);
                            }
                        }
                    }
                }
            }
            if (getEscState(*it)==GLOBAL_ESCAPE) { // to set gl.escape for contained objects
                initNodeType = NT_STFLD;
                CnGNode* cn = *it;
                CnGNode* nn = NULL;
                CnGRefs::iterator it2;
                if (cn->nodeType == NT_LDOBJ) {
                    scanCnGNodeRefsGE(cn,true);
                } else {
                    if (cn->outEdges != NULL) {
                        for (it2 = cn->outEdges->begin( ); it2 != cn->outEdges->end( ); it2++ ) {
                            nn = (*it2)->cngNodeTo;
                            if (getEscState(nn)==GLOBAL_ESCAPE) {
                                continue;
                            }
                            scanCnGNodeRefsGE(nn,false);
                        }
                    }
                }
                scannedObjs->clear();
                scannedObjsRev->clear();
            }
        }
    }
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        uint32 nt = (*it)->nodeType;
        if ((nt==NT_RETVAL) && getEscState(*it)>GLOBAL_ESCAPE) {
            MethodDesc* mdesc = (*it)->nInst->asMethodInst()->getMethodDesc();
            if (((*it)->nInst->getOpcode())!=Op_DirectCall) { // only direct call may be here
                assert(0);
                continue;
            }
            CalleeMethodInfo* mthInfo = findMethodInfo(mdesc,(*it)->nInst);
            if (mthInfo == NULL) {
#ifdef _DEBUG
                if (_setState) {
                    Log::out() <<"--setCOS 4:  nodeId "
                        <<(*it)->cngNodeId<<"  opId "<<(*it)->opndId <<" state ";
                    printState(*it); Log::out() <<" to gl.esc."<< std::endl;
                }
#endif
                initNodeType = NT_STFLD;
                scanCnGNodeRefsGE(*it,false);
            } else {
                if (getEscState(*it)>((mthInfo->retValueState)&ESC_MASK) || getOutEscaped(*it)==0) {
#ifdef _DEBUG
                    if (_setState) {
                        Log::out() <<"--setCOS 5:  nodeId "
                            <<(*it)->cngNodeId<<"  opId "<<(*it)->opndId <<" state ";
                        printState(*it);
                        Log::out() <<" to "<< mthInfo->retValueState<< std::endl;
                    }
#endif
                    if (((mthInfo->retValueState)&ESC_MASK) == GLOBAL_ESCAPE) {
                        initNodeType = NT_STFLD;   // global_escape propagate
                    } else {
                        initNodeType = NT_EXITVAL;  // out_escaped propagate
                    }
                    scanCnGNodeRefsGE(*it,false);
                }
            }
        }
    }
    scannedObjs->clear();
    scannedObjsRev->clear();
    scannedSucNodes->clear();
    // check states of array elements' and fields' values
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        uint32 nt = (*it)->nodeType;
        if ((nt&(NT_OBJECT|NT_RETVAL) || nt==NT_LDOBJ) && (getEscState(*it)!=GLOBAL_ESCAPE)) {
            if ((*it)->outEdges == NULL)
                continue;
#ifdef _DEBUG
            if (_setState) {
                Log::out() <<"--setCOS 6: to check subobj nodes ";
                printCnGNode(*it,Log::out()); Log::out() << std::endl;
            }
#endif
            checkSubobjectStates(*it);
        }
        scannedObjs->clear();
        scannedObjsRev->clear();
        scannedSucNodes->clear();
    }

}  // setCreatedObjectStates() 


void
EscAnalyzer::scanCnGNodeRefsGE(CnGNode* cgn, bool check_var_src) {
    CnGEdges::iterator it;
    CnGRefs::iterator it1;
    CnGNode* next_node;
    ObjIds* scObjs = check_var_src ? scannedObjsRev : scannedObjs;
    uint32 ni_opcode = cgn->nInst->getOpcode();
    bool needFix = isStateNeedGEFix(getFullState(cgn),cgn->nodeType);

    if (cgn->nodeType != NT_STFLD && cgn->nodeType != NT_THRVAL && cgn->nodeType != NT_EXITVAL 
        && cgn->nodeType != NT_DEFARG && cgn->nodeType != NT_RETVAL && cgn->nodeType != NT_LDOBJ)
        assert(needFix);
#ifdef _DEBUG
    if (_setState) {
        Log::out() <<"--scanGE 1:  nodeId "<<cgn->cngNodeId
            <<"  opId "<<cgn->opndId<<" state ";
        printState(cgn); 
        Log::out() <<" " << nodeTypeToString(cgn) <<cgn->nodeType
            <<"  check_var_src " << check_var_src << std::endl;
    }
#endif
    if (cgn->nodeType == NT_LDVAL) {
#ifdef _DEBUG
        if (_setState) {
            Log::out() <<"--scanGE > 1: primitive type " << std::endl;
        }
#endif
        return;
    }
    if (scObjs->size()!=0) {
        if (checkScanned(scObjs,cgn->cngNodeId)) {
#ifdef _DEBUG
            if (_setState) {
                Log::out() <<"--scanGE > 1: was scanned earlier " << std::endl;
            }
#endif
            return;
        }
    }

    if (initNodeType!=NT_EXITVAL && initNodeType!=NT_DEFARG) { // 
        if (getEscState(cgn) > GLOBAL_ESCAPE) {
#ifdef _DEBUG
            if (_setState) {
                Log::out() <<"--scanGE 2:  nodeId "
                    <<cgn->cngNodeId<<"  opId "<<cgn->opndId <<" state ";
                printState(cgn);
                Log::out() <<" to gl.esc."<< std::endl;
                Log::out() <<"--scanGE 2: "<< nodeTypeToString(cgn) 
                    << cgn->nodeType <<" initNode "<<initNodeType<< std::endl;
            }
#endif
            setEscState(cgn,GLOBAL_ESCAPE);
        }
    } 
    if (initNodeType==NT_EXITVAL) {
        if (getOutEscaped(cgn) == 0) {
#ifdef _DEBUG
            if (_setState) {
                Log::out() <<"--scanGE 3:  nodeId "
                    <<cgn->cngNodeId<<"  opId "<<cgn->opndId <<" state ";
                printState(cgn);
                Log::out() <<" to out.esc."<< std::endl;
                Log::out() <<"--scanGE 3: "<< nodeTypeToString(cgn) 
                    << cgn->nodeType <<" initNode "<<initNodeType<< std::endl;
            }
#endif
            setOutEscaped(cgn);
        }
        // The objects created in the method are not global escaped through return
    }
    if (initNodeType==NT_DEFARG) {
        if (needFix) {
#ifdef _DEBUG
            if (_setState) {
                Log::out() <<"--scanGE 4:  nodeId "
                    <<cgn->cngNodeId<<"  opId "<<cgn->opndId <<" state ";
                printState(cgn);
                if (cgn->nodeType == NT_OBJECT || cgn->nodeType == NT_LDOBJ || cgn->nodeType == NT_RETVAL) {
                    Log::out() <<" to gl.esc."<< std::endl;
                } else {
                    Log::out() <<" to out.esc."<< std::endl;
                } 
                Log::out() <<"--scanGE 4: "<< nodeTypeToString(cgn) 
                    << cgn->nodeType <<" initNode "<<initNodeType<< std::endl;
            }
#endif
            if (cgn->nodeType == NT_OBJECT || cgn->nodeType == NT_LDOBJ || cgn->nodeType == NT_RETVAL) {
                setEscState(cgn,GLOBAL_ESCAPE); //objects escaped through defarg - global escape
            } else {
                setOutEscaped(cgn);
            }
        }
    }

    scObjs->push_back(cgn->cngNodeId);
    if (cgn->outEdges != NULL) {
        bool to_check_var_src = false;
        for (it1 = cgn->outEdges->begin( ); it1 != cgn->outEdges->end( ); it1++ ) {
            next_node = (*it1)->cngNodeTo;
            needFix = isStateNeedGEFix(getFullState(next_node),next_node->nodeType);
            if (!needFix) {
#ifdef _DEBUG
                if (_setState) {
                    Log::out() <<"--scanGE 5.0 next:  already set  ";
                    printState(next_node); Log::out() << std::endl;
                }
#endif
                continue;
            }
            if (next_node->nodeType == NT_LDOBJ && next_node->nInst->getOpcode()==Op_LdVar 
                && cgn->nodeType!=NT_VARVAL) {
                to_check_var_src = true;
            }
#ifdef _DEBUG
            if (_setState) {
                Log::out() <<"--scanGE 5 next:  nodeId "
                    <<next_node->cngNodeId<<"  opId "<<next_node->opndId <<" state ";
                printState(next_node); Log::out() << std::endl;
            }
#endif
            scanCnGNodeRefsGE(next_node,to_check_var_src);
#ifdef _DEBUG
            if (_setState) {
                Log::out() <<"--scanGE ret 1 for node:  " << next_node->cngNodeId
                    <<"  opId " << next_node->opndId << std::endl;
            }
#endif
        }
    }
    if (check_var_src) {
        if (ni_opcode == Op_LdVar || ni_opcode == Op_StVar || ni_opcode ==Op_Phi) {
            uint32 nsrc=cgn->nInst->getNumSrcOperands();
            for (uint32 i=0; i<nsrc; i++) {
                next_node = findCnGNode_op(cgn->nInst->getSrc(i)->getId());
                needFix = isStateNeedGEFix(getFullState(next_node),next_node->nodeType);
                if (!needFix) {
#ifdef _DEBUG
                    if (_setState) {
                        Log::out() <<"--scanGE 6.0 next:  already set  ";
                        printState(next_node); Log::out() << std::endl;
                    }
#endif
                    continue;
                }
#ifdef _DEBUG
                if (_setState) {
                    Log::out() <<"--scanGE 6 next:  nodeId "
                        <<next_node->cngNodeId<<"  opId "<<next_node->opndId <<" state ";
                    printState(next_node); Log::out() << std::endl;
                }
#endif
                scanCnGNodeRefsGE(next_node,check_var_src);
#ifdef _DEBUG
                if (_setState) {
                    Log::out() <<"--scanGE ret 2 for node:  " << next_node->cngNodeId
                        <<"  opId " << next_node->opndId << std::endl;
                }
#endif
            }
        }
    }

}  // scanCnGNodeRefsGE(CnGNode* cgn, bool check_var_src)


void
EscAnalyzer::scanCnGNodeRefsAE(CnGNode* cgn, bool check_var_src) {
    CnGEdges::iterator it;
    CnGRefs::iterator it1;
    CnGNode* next_node;
    ObjIds* scObjs = check_var_src ? scannedObjsRev : scannedObjs;
    uint32 ni_opcode = cgn->nInst->getOpcode();
    uint32 curMDNode_saved = 0;

    if (curMDNode == 0) {
        assert(getEscState(cgn)!=ARG_ESCAPE);
    }
    assert(getEscState(cgn)!=GLOBAL_ESCAPE);
#ifdef _DEBUG
    if (_setState) {
        Log::out() <<"--scanAE < 1:  nodeId "<<cgn->cngNodeId
            <<"  opId "<<cgn->opndId<<" state ";
        printState(cgn);
        Log::out() <<" " << nodeTypeToString(cgn) <<cgn->nodeType
            <<"  check_var_src " << check_var_src << std::endl;
    }
#endif
    if (cgn->nodeType == NT_LDVAL) {   // primitive type value
#ifdef _DEBUG
        if (_setState) {
            Log::out() <<"--scanAE > 1: primitive type " << std::endl;
        }
#endif
        return;
    }
    if (scObjs->size()!=0) {
        if (checkScanned(scObjs,cgn->cngNodeId)) {
#ifdef _DEBUG
            if (_setState) {
                Log::out() <<"--scanAE > 2: was scanned earlier " << std::endl;
            }
#endif
            return;
        }
    }

    if (cgn->nodeType == NT_ACTARG) {
        curMDNode_saved = curMDNode;
    }

    if (cgn->nodeMDs!=NULL && curMDNode!=0) {
         cgn->nodeMDs->push_back(curMDNode);
#ifdef _DEBUG
        if (_setState) {
            Log::out() <<"--scanAE 1_1:  nodeId "<<cgn->cngNodeId
                <<"  opId "<<cgn->opndId <<" curMDNode "<<curMDNode<< std::endl;
        }
#endif
    }
    if (getEscState(cgn) > ARG_ESCAPE) {
#ifdef _DEBUG
        if (_setState) {
            Log::out() <<"--scanAE 2:  nodeId "<<cgn->cngNodeId
                <<"  opId "<<cgn->opndId<<" state ";
            printState(cgn);
            Log::out() <<" to arg.esc."<< std::endl;
        }
#endif
        setEscState(cgn,ARG_ESCAPE);
    }

    scObjs->push_back(cgn->cngNodeId);
    if (cgn->outEdges != NULL) {
        bool to_check_var_src = check_var_src;
        for (it1 = cgn->outEdges->begin( ); it1 != cgn->outEdges->end( ); it1++ ) {
            next_node = (*it1)->cngNodeTo;
            if (getEscState(next_node) < ARG_ESCAPE)
                continue;
            if (getEscState(next_node) == ARG_ESCAPE && cgn->nodeType != NT_ACTARG)
                continue;
#ifdef _DEBUG
            if (_setState) {
                Log::out() <<"--scanAE 3 next:  nodeId "
                    <<next_node->cngNodeId<<"  opId "<<next_node->opndId <<" state ";
                printState(next_node); 
                Log::out() << " " << nodeTypeToString(next_node) 
                    << " ref.type " << next_node->nodeRefType << std::endl;
            }
#endif
            if (next_node->nodeType == NT_LDVAL)
                continue;
            if (next_node->nodeType == NT_LDOBJ)
                if (next_node->nInst->getOpcode()==Op_LdVar && cgn->nodeType!=NT_VARVAL)
                    to_check_var_src = true;
            scanCnGNodeRefsAE(next_node,to_check_var_src);
            curMDNode = curMDNode_saved;
        }
    }
    if (check_var_src) {
        if (ni_opcode == Op_LdVar || ni_opcode == Op_StVar || ni_opcode ==Op_Phi) {
            uint32 nsrc=cgn->nInst->getNumSrcOperands();
            for (uint32 i=0; i<nsrc; i++) {
                next_node = findCnGNode_op(cgn->nInst->getSrc(i)->getId());
                if (getEscState(next_node) <= ARG_ESCAPE)
                    continue;
#ifdef _DEBUG
                if (_setState) {
                    Log::out() <<"--scanAE 4 next:  nodeId "
                        <<next_node->cngNodeId<<"  opId "<<next_node->opndId <<" state ";
                    printState(next_node); Log::out() << std::endl;
                }
#endif
                scanCnGNodeRefsAE(next_node,check_var_src);
                curMDNode = curMDNode_saved;
            }
        }
    }

#ifdef _DEBUG
    if (_setState) {
        Log::out() <<"--scanAE > 4: exit:  nodeId "<<cgn->cngNodeId
            <<"  opId "<<cgn->opndId<<" state "; printState(cgn);
        Log::out() <<" " << nodeTypeToString(cgn) <<cgn->nodeType
            <<"  check_var_src " << check_var_src << std::endl;
    }
#endif
}  // scanCnGNodeRefsAE(CnGNode* cgn, bool check_var_src)


void 
EscAnalyzer::checkSubobjectStates(CnGNode* node) {
    CnGRefs::iterator it;
    CnGRefs::iterator it1;
    CnGNode* cgn;
    CnGNode* cgn1;
    CnGNode* node_fld;
    bool arge = false;
    bool calle = false;
    bool gle = false;
    bool no_mod = false;
    Inst* n_inst = node->nInst;
    CnGNode* nt = NULL;

#ifdef _DEBUG
    if (_setState) {
        Log::out() <<"--checkSOS 0: ";
        printCnGNode(node,Log::out());Log::out() << std::endl;
    }
#endif
    if (node->outEdges==NULL) {
#ifdef _DEBUG
        if (_setState) {
            Log::out() <<"--checkSOS 1: return "  << std::endl;
        }
#endif
        return;
    }

    for (it1 = node->outEdges->begin( ); it1 != node->outEdges->end( ); it1++ ) {
        node_fld = (*it1)->cngNodeTo;
        if ((*it1)->edgeType != ET_FIELD) {
            continue;
        }
        if (node_fld->outEdges==NULL) {
            continue;
        }
        arge = false;
        calle = false;
        gle = false;
        no_mod = false;
        nt = node_fld;
        n_inst = node_fld->nInst;
        for (it = node_fld->outEdges->begin( ); it != node_fld->outEdges->end( ); it++ ) {
            cgn = (*it)->cngNodeTo;
#ifdef _DEBUG
            if (_setState) {
                Log::out() <<"--checkSOS 2: ";
                printCnGNode(cgn,Log::out());Log::out() << std::endl;
            }
#endif
            if (getEscState(cgn) < getEscState(node_fld)) {
                if (getEscState(cgn)==GLOBAL_ESCAPE) {
                    gle = true;
                    break;
                }
                if (getEscState(cgn)==ARG_ESCAPE) {
                    arge = true;
                    continue;
                }
            }
            if (getOutEscaped(node_fld) == 0 && getOutEscaped(cgn) != 0) {
                calle = true;
                continue;
            }
            no_mod = true;
        }
#ifdef _DEBUG
        if (_setState) {
            Log::out() <<"--checkSOS 2:  " << no_mod << " " << gle  << " " << calle << " " << arge
                << std::endl;
        }
#endif
        if (gle || calle || arge) {
            if (node_fld->nodeType==NT_ARRELEM) {
                if (n_inst->getOpcode() == Op_AddScaledIndex) {
                    n_inst = n_inst->getSrc(0)->getInst();
                    nt = findCnGNode_op(n_inst->getSrc(0)->getId());
#ifdef _DEBUG
                    if (_setState) {
                        Log::out() <<"--checkSOS 3:  "; printCnGNode(nt,Log::out()); Log::out() << std::endl;
                    }
#endif
                    if (nt->lNode != NULL) {
                        nt = nt->lNode;
                    }
                }
            }
#ifdef _DEBUG
            if (_setState) {
                Log::out() <<"--checkSOS 4: found  ";
                printCnGNode(nt,Log::out());Log::out() << std::endl;
            }
#endif
            if (gle) {
                initNodeType = NT_STFLD; 
            } else {
                if (calle) {
                    initNodeType = NT_DEFARG; 
                } else {
                    initNodeType = NT_ACTARG;
                }
            }
            if (node_fld->nodeType==NT_INSTFLD) { // set new state beginning with instance field
                if (gle || calle) {
                    scanCnGNodeRefsGE(node_fld,false);
                } else {
                    curMDNode = 0;
                    scanCnGNodeRefsAE(node_fld,false);
                }
            } else { // set new state beginning with array elements
                for (it = nt->outEdges->begin( ); it != nt->outEdges->end( ); it++ ) {
                    cgn1 = (*it)->cngNodeTo;
                    if (cgn1->nodeType != NT_ARRELEM)
                        continue;
                    if (gle || calle) {
                        if (isStateNeedGEFix(getFullState(cgn1),cgn1->nodeType))
                            scanCnGNodeRefsGE(cgn1,false);
                    } else {
                        curMDNode = 0;
                        if (getEscState(cgn1) > ARG_ESCAPE)
                            scanCnGNodeRefsAE((*it)->cngNodeTo,false);
                    }
                }
            }
        }
    }
}  // checkSubobjectStates(CnGNode* node)


EscAnalyzer::CalleeMethodInfo*
EscAnalyzer::findMethodInfo(MethodDesc* mdesc,Inst* callInst) {
    const char* ch1 = mdesc->getParentType()->getName(); 
    const char* ch2 = mdesc->getName(); 
    const char* ch3 = mdesc->getSignatureString();
    CalleeMethodInfo* mtdInfo = getMethodInfo(ch1,ch2,ch3);
    if (mtdInfo == NULL) {
#ifdef _DEBUG
        if (_scanMtds==1) {
            Log::out() << "      = = = = = = = =  To scan method " << std::endl;
            mdesc->printFullName(Log::out());
            Log::out() << std::endl;
            callInst->print(Log::out());
            Log::out() << std::endl;
        }
#endif
        if (method_ea_level < maxMethodExamLevel) {
            scanCalleeMethod(callInst);
            mtdInfo=getMethodInfo(ch1,ch2,ch3);
        }
    }
    return mtdInfo;
} // findMethodInfo(MethodDesc* mdesc) 


EscAnalyzer::CalleeMethodInfo*
EscAnalyzer::getMethodInfo(const char* ch1,const char* ch2,const char* ch3) {
    CalleeMethodInfos::iterator it;
    if (calleeMethodInfos==NULL)
        return NULL;
    for (it = calleeMethodInfos->begin( ); it != calleeMethodInfos->end( ); it++ ) {
        const char* c1 = (*it)->methodIdent->parentName; 
        const char* c2 = (*it)->methodIdent->name; 
        const char* c3 = (*it)->methodIdent->signature;
        if (strcmp(c1,ch1)==0 && strcmp(c2,ch2)==0 && strcmp(c3,ch3)==0)
            return (*it);
    }
    return NULL;
}  // getMethodInfo(const char* ch1,const char* ch2,const char* ch3) 


void
EscAnalyzer::scanCalleeMethod(Inst* call) {
    MethodDesc* methodDesc = 0;
    
#ifdef _DEBUG
    if (_scanMtds==1) {
        Log::out() << "=="; 
        call->print(Log::out()); 
        Log::out() << std::endl;
    }
#endif
    if (call == NULL) {  // scanned Op_DirectCall, not scanned Op_IndirectMemoryCall
        Log::out() << "scanMethod: NULL" << std::endl;
        return;
    }
    methodDesc = call->asMethodCallInst()->getMethodDesc();

#ifdef _DEBUG
    if (_scanMtds==1) {
        Log::out() << std::endl; 
        Log::out() << "scanMethod: " << methodDesc->getParentType()->getName() 
            << "." << methodDesc->getName() << methodDesc->getSignatureString() 
            << "  " << methodDesc << std::endl;
        Log::out() << "    NumParams: " 
            << methodDesc->getNumParams()<< std::endl;
        Log::out() << "    isNative: " << methodDesc->isNative() << std::endl;
        Log::out() << "    isStatic: " << methodDesc->isStatic() << std::endl;
        Log::out() << "    isInstance: " << methodDesc->isInstance() << std::endl;
        Log::out() << "    isFinal: " << methodDesc->isFinal() << std::endl;
        Log::out() << "    isVirtual: " << methodDesc->isVirtual() << std::endl;
        Log::out() << "    isAbstract: " << methodDesc->isAbstract() << std::endl;
        Log::out() << "    isInstanceInitializer: " 
            << methodDesc->isInstanceInitializer() << std::endl;
        Log::out() << "    isOverridden: " << methodDesc->isOverridden() << std::endl;
   }
#endif

    OpndManager& _opndManager(irManager.getOpndManager());
    Opnd *returnOpnd = 0;
    if(call->getDst()->isNull())
        returnOpnd = _opndManager.getNullOpnd();
    else 
        returnOpnd = _opndManager.createSsaTmpOpnd(call->getDst()->getType());

    IRManager* inlinedIRM = new (eaMemManager) IRManager(irManager.getMemoryManager(), irManager, *methodDesc, returnOpnd);
    CompilationInterface& ci= inlinedIRM->getCompilationInterface();
    bool cibcmap = ci.isBCMapInfoRequired();
    if (cibcmap) {
        ci.setBCMapInfoRequired(false);
    }
    
    {
        CompilationContext inlineCC(irManager.getMemoryManager(), &ci, irManager.getCurrentJITContext());
        inlineCC.setPipeline(irManager.getCompilationContext()->getPipeline());
        inlineCC.setHIRManager(inlinedIRM);
        runTranslatorSession(inlineCC);
    }
    
    optimizeTranslatedCode(*inlinedIRM);
    
    EscAnalyzer ea1(this, *inlinedIRM);
    ea1.doAnalysis();

    if (cibcmap) {
        ci.setBCMapInfoRequired(true);
    }
}  // scanCalleeMethod(Inst* call) 


void EscAnalyzer::runTranslatorSession(CompilationContext& inlineCC) {
    TranslatorSession* traSession = (TranslatorSession*)translatorAction->createSession(inlineCC.getCompilationLevelMemoryManager());
    traSession->setCompilationContext(&inlineCC);
    inlineCC.setCurrentSessionAction(traSession);
    traSession->run();
    inlineCC.setCurrentSessionAction(NULL);
}


void
EscAnalyzer::optimizeTranslatedCode(IRManager& irm) {
    // run ssa pass
    OptPass::computeDominators(irm);
    DominatorTree* dominatorTree = irm.getDominatorTree();
    ControlFlowGraph& flowGraph = irm.getFlowGraph();
   
    DomFrontier frontier(irm.getNestedMemoryManager(),*dominatorTree,&flowGraph);
    SSABuilder ssaBuilder(irm.getOpndManager(),irm.getInstFactory(),frontier,&flowGraph, 
                          irm.getOptimizerFlags());
    ssaBuilder.convertSSA(irm.getMethodDesc());
    irm.setInSsa(true);
    irm.setSsaUpdated();

    // run devirt pass
//    Devirtualizer pass(irm);
//    pass.guardCallsInRegion(irm, dominatorTree);

}  // optimizeTranslatedCode(IRManager& irManager) 


void
EscAnalyzer::saveScannedMethodInfo() {
    CnGNodes::iterator it;
    MethodDesc* mdesc = &irManager.getMethodDesc();
    MemoryManager& globalMM = irManager.getCurrentJITContext()->getGlobalMemoryManager();
    const char* ch1 = mdesc->getParentType()->getName();
    const char* ch2 = mdesc->getName(); 
    const char* ch3 = mdesc->getSignatureString();
    if (calleeMethodInfos==NULL) {
        calleeMethodInfosLock.lock();
        if (calleeMethodInfos==NULL)
            calleeMethodInfos = new (globalMM) CalleeMethodInfos(globalMM);
        calleeMethodInfosLock.unlock();
    } else {
        CalleeMethodInfo* mtdInfo = getMethodInfo(ch1,ch2,ch3);
        if (mtdInfo!=NULL)
            return;    // already saved for global analyzed method
    }
    if (getMethodInfo(ch1,ch2,ch3)!=NULL) // info was saved by another jit
        return;
    calleeMethodInfosLock.lock();  // Lock to save method info in common memory

    if (getMethodInfo(ch1,ch2,ch3)!=NULL) {   // already saved by another jit
        calleeMethodInfosLock.unlock();  // Unlock 
        return;
    }

    CalleeMethodInfo* minfo = new (globalMM) CalleeMethodInfo;
    char* mpname=new (globalMM) char[strlen(ch1)+1];
    strcpy(mpname,ch1);
    char* mname=new (globalMM) char[strlen(ch2)+1];
    strcpy(mname,ch2);
    char* msig=new (globalMM) char[strlen(ch3)+1];
    strcpy(msig,ch3);
    MemberIdent* mident = new (globalMM) MemberIdent;
    mident->parentName=mpname;
    mident->name=mname;
    mident->signature=msig;
    minfo->methodIdent=mident;
    uint32 numpar = mdesc->getNumParams();
    minfo->numberOfArgs=numpar;
    ParamInfos* prminfos = new (globalMM) ParamInfos(globalMM);
    minfo->paramInfos=prminfos;
    minfo->retValueState=0;
    if (mdesc->getReturnType()->isReference()) {
        uint32 escstate = 0;
        for (it = cngNodes->begin( ); it != cngNodes->end( ); it++) {
            if ((*it)->nodeType==NT_EXITVAL) {
                if (isGlobalState(getFullState((*it)->outEdges->front()->cngNodeTo))) {
                    escstate = GLOBAL_ESCAPE;
                    break;
                }
            }
        }
        if (escstate == 0) {
            escstate = OUT_ESCAPED|NO_ESCAPE;
        }
        minfo->retValueState=escstate;
    }
    bool pmt = checkMonitorsOnThis();
    minfo->mon_on_this = pmt;
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++) {
        if ((*it)->nodeType==NT_DEFARG) {
            ParamInfo* prminfo = new (globalMM) ParamInfo;
            prminfo->state=getFullState(*it);
            prminfo->paramNumber=(*it)->argNumber;
            prminfos->push_back(prminfo);
        }
    }
    calleeMethodInfos->push_back(minfo);
    calleeMethodInfosLock.unlock();  // Unlock 

#ifdef _DEBUG
    if (_scanMtds==1) {
        ParamInfos::iterator it2;
        Log::out() << "====     ===== calleeMethodInfo  " << std::endl;
        Log::out() << minfo->methodIdent->parentName << "  ";
        Log::out() << minfo->methodIdent->name << "  ";
        Log::out() << minfo->methodIdent->signature << "  ";
        Log::out() << minfo->numberOfArgs<< "  " << std::endl;
        for (it2 = minfo->paramInfos->begin( ); it2 != minfo->paramInfos->end( ); it2++) {
            Log::out() << (*it2)->paramNumber << "  st." 
                << (*it2)->state << std::endl;
        }
        Log::out() << "==== end ===== calleeMethodInfo  " << std::endl;
    }
#endif
}  // saveScannedMethodInfo(MemoryManager& mm)


uint32 
EscAnalyzer::getMethodParamState(CalleeMethodInfo* mi, uint32 np) {
    ParamInfos::iterator it;
    uint32 st = 0;
    if (mi==NULL)
        return 0; 
    for (it = mi->paramInfos->begin( ); it != mi->paramInfos->end( ); it++) {
        if ((*it)->paramNumber == np) {
             st = (*it)->state;
             return st;
        }
    }
    return st;
}  // getMethodParamState(CalleeMethodInfo* mi, uint32 np) 


void
EscAnalyzer::markNotEscInsts() {
    CnGNodes::iterator it;
    bool p2 = false;
    StlMap<uint32, uint32> nonEscInsts(eaMemManager);
    typedef ::std::pair <uint32,uint32> intPair;

    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        if ((*it)->nodeType == NT_OBJECT && getEscState(*it) > GLOBAL_ESCAPE && getVirtualCall(*it) == 0 
            && getOutEscaped(*it) == 0) {
            nonEscInsts.insert(intPair((*it)->instrId,getFullState(*it))); 
        }
    }
    if (p2 && Log::isEnabled()) {
        Log::out() << "================                       > "; 
        irManager.getMethodDesc().printFullName(Log::out()); 
        Log::out() << std::endl;
    }
}  // markNotEscInsts() 


void
EscAnalyzer::eaFixupVars(IRManager& irm) {
    OptPass::computeDominators(irm);
    DominatorTree* dominatorTree = irm.getDominatorTree();
    ControlFlowGraph& flowGraph = irm.getFlowGraph();
    
    DomFrontier frontier(irm.getNestedMemoryManager(),*dominatorTree,&flowGraph);
    SSABuilder ssaBuilder(irm.getOpndManager(),irm.getInstFactory(),frontier,&flowGraph, irm.getOptimizerFlags());
    bool phiInserted = ssaBuilder.fixupVars(&irm.getFlowGraph(), irm.getMethodDesc());
    irm.setInSsa(true);
    if (phiInserted)
        irm.setSsaUpdated();
}


void
EscAnalyzer::printCnGNodes(char* text,::std::ostream& os) {
    CnGNodes::const_iterator it;
    FieldDesc* fd;
    std::string t1;
    std::string t2;
    os << "    "<< text << std::endl;
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        os <<"    ";
        printCnGNode(*it,os);
        os << std::endl;
        os << "                                ";
        if ((*it)->nodeType & (NT_OBJS|NT_EXITVAL|NT_LDVAL)) {  //node of created or exit object
            ((Opnd*)(*it)->refObj)->printWithType(os);
        } 
        if ((*it)->nodeType == NT_RETVAL) {
            os << std::endl; os << "                                ";
            Inst* inst = ((Opnd*)(*it)->refObj)->getInst();
            inst->print(os);
            if (inst->getOpcode()==Op_IndirectMemoryCall) {
                MethodDesc* md;
                if (inst->getSrc(0)->getInst()->getOpcode()== Op_LdVar) {
                    md = inst->getSrc(0)->getType()->asMethodPtrType()->getMethodDesc();
                } else {
                    md = inst->getSrc(0)->getInst()->asMethodInst()->getMethodDesc();
                }
                os << std::endl; os << "                                ";
                md->printFullName(os);
            }
        }
        if ((*it)->nodeType & NT_ACTARG) {    //node of actual method parameter
            os << ((MethodDesc*)(*it)->refObj)->getParentType()->getName() << "::";
            os << ((MethodDesc*)(*it)->refObj)->getName() << std::endl;
            os << "                                ";
            os << ((MethodDesc*)(*it)->refObj)->getParamType((*it)->opndId)->getName();
        }
        if ((*it)->nodeType & NT_STFLD) {    //field node 
            fd = ((Inst*)(*it)->refObj)->asFieldAccessInst()->getFieldDesc();
            os << fd->getParentType()->getName() << "::"<< fd->getName() << std::endl;
            os << "                                "<<fd->getFieldType()->getName();
        }
        os << std::endl;
    }
}  // printCnGNodes(char* text,::std::ostream& os) 


void
EscAnalyzer::printCnGNode(CnGNode* cgn,::std::ostream& os) {
    std::string t2;

    os << "nodeId "<<cgn->cngNodeId<<"  ";
    if (cgn->nodeType & (NT_OBJS|NT_LDVAL)) {   //node of object created in the method
        os << "opId "<<cgn->opndId<<"  ";
    } else {
        if (cgn->nodeType & NT_ACTARG) {   //node of actual method parameter
            os << "nArg "<<cgn->argNumber<<"  ";
            os << "method "<<cgn->refObj<<"  ";
        } 
    }
    if (cgn->nodeType==NT_DEFARG)
        os << "  nArg "<<cgn->argNumber<<"  ";    //Arg number for defarg
    os << "  inst "<<cgn->instrId<<"  ("<<nodeTypeToString(cgn)<<cgn->nodeType<<",  ";
    switch (cgn->nodeRefType) {
        case NR_PRIM   : t2="Prim-"; break;
        case NR_REF    : t2="Ref -"; break;
        case NR_ARR    : t2="Arr -"; break;
        case NR_REFARR : t2="RArr-"; break;
        default        : t2="    -";
    }
    os <<t2<<cgn->nodeRefType<<")  ";
    if (cgn->lNode) {
        os << "( " << cgn->lNode->cngNodeId << "-" << cgn->lNode->opndId << " )  ";
    }
    os << "st. ";
    printState(cgn,os); os << "  ";
}  // printCnGNode(CnGNode* cgn,::std::ostream& os) 


std::string 
EscAnalyzer::nodeTypeToString(CnGNode* cgn) {
    std::string t1;
    switch (cgn->nodeType) {
        case NT_OBJECT  : t1="Obj -"; break;
        case NT_DEFARG  : t1="DArg-"; break;
        case NT_RETVAL  : t1="RVal-"; break;
        case NT_CATCHVAL: t1="CVal-"; break;
        case NT_STFLD   : t1="SFld-"; break;
        case NT_INSTFLD : t1="IFld-"; break;
        case NT_LDOBJ   : t1="LObj-"; break;
        case NT_INTPTR  : t1="IPtr-"; break;
        case NT_VARVAL  : t1="VVal-"; break;
        case NT_ARRELEM : t1="ArEl-"; break;
        case NT_REF     : t1="REF -"; break;
        case NT_ACTARG  : t1="AArg-"; break;
        case NT_EXITVAL : t1="EVal-"; break;
        case NT_THRVAL  : t1="TVal-"; break;
        case NT_LDVAL   : t1="LVal-"; break;
        default         : t1="    -";
    }
    return t1;
}  // nodeTypeToString(CnGNode* cgn) 


void
EscAnalyzer::printCnGEdges(char* text,::std::ostream& os) {
    CnGEdges* cge = cngEdges;
    CnGEdges::iterator it;
    CnGRefs::iterator it1;
    os << "    "<< text << std::endl;
    if (cge==NULL) {
        os <<"    NULL"<< std::endl;
        return;
    }
    for (it = cge->begin( ); it != cge->end( ); it++ ) {
        os << "    from ";
        printCnGNode((*it)->cngNodeFrom,os);
        os << "  to " << std::endl;
        for (it1 = (*it)->refList->begin( ); it1 != (*it)->refList->end( ); it1++ ) {
            os << "      ";
            os <<(*it1)->cngNodeTo->cngNodeId<<" (";
            os <<edgeTypeToString(*it1)<<(*it1)->edgeType<<"), ";
            printCnGNode(findCnGNode_id((*it1)->cngNodeTo->cngNodeId),os);
            os << std::endl;
        }
    }
}  // printCnGEdges(char* text,::std::ostream& os) 


std::string 
EscAnalyzer::edgeTypeToString(CnGRef* edr) {
    std::string t1;
    switch (edr->edgeType) {
        case ET_POINT : t1="poi-"; break;
        case ET_DEFER : t1="ref-"; break;
        case ET_FIELD : t1="fld-"; break;
        default       : t1="   -";
    }
    return t1;
}  // edgeTypeToString(CnGRef* edr) 


void
EscAnalyzer::printRefInfo(::std::ostream& os) {
    CnGNodes::iterator it;
    os << "================ Static Fields" << std::endl;
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        if ((*it)->nodeType==NT_STFLD) {
            printCnGNodeRefs(*it, "", os);
        }
    }
    scannedObjs->clear();          
    os << "================ Method Agruments" << std::endl;
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        if ((*it)->nodeType==NT_ACTARG) {
            printCnGNodeRefs(*it, "", os);
        }
    }
    scannedObjs->clear();          
    os << "================ Return Values" << std::endl;
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        if ((*it)->nodeType==NT_EXITVAL) {
            printCnGNodeRefs(*it, "", os);
        }
    }
    scannedObjs->clear();          
    os << "================ Thrown Values" << std::endl;
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        if ((*it)->nodeType==NT_THRVAL) {
            printCnGNodeRefs(*it, "", os);
        }
    }
    scannedObjs->clear();          
    os << "================ Instsnce Fields" << std::endl;
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        if ((*it)->nodeType==NT_INSTFLD) {
            printCnGNodeRefs(*it, "", os);
        }
    }
    scannedObjs->clear();          
}  // printRefInfo(::std::ostream& os)


void
EscAnalyzer::printCnGNodeRefs(CnGNode* cgn, std::string text,::std::ostream& os) {
    CnGNode* node;
    CnGRefs::iterator it1;
    Inst* inst; 
    os << text;
    if (scannedObjs->size()!=0) {
        if (checkScannedObjs(cgn->cngNodeId)) {
            os << "nodeId " << cgn->cngNodeId << "  .  .  . " << std::endl;
            return;
        }
    }
    printCnGNode(cgn,os);
    os << std::endl;
    os << text; cgn->nInst->print(os); os << std::endl; 
    scannedObjs->push_back(cgn->cngNodeId);
    if (cgn->outEdges != NULL) {
        for (it1 = cgn->outEdges->begin( ); it1 != cgn->outEdges->end( ); it1++ ) {
            os << text << edgeTypeToString(*it1) << std::endl;
            if ((node=findCnGNode_id((*it1)->cngNodeTo->cngNodeId))!=NULL)
                printCnGNodeRefs(node,text+"  ",os);
        } 
    }
    scannedObjs->pop_back();
    if (cgn->nodeType==NT_RETVAL) {
        inst = cgn->nInst;
        if (inst->getOpcode()==Op_IndirectMemoryCall) {
            MethodDesc* md;
            if (inst->getSrc(0)->getInst()->getOpcode()== Op_LdVar) {
                md = inst->getSrc(0)->getType()->asMethodPtrType()->getMethodDesc();
            } else {
                md = inst->getSrc(0)->getInst()->asMethodInst()->getMethodDesc();
            }
            os << text << "  ";         
            md->printFullName(os); os << std::endl;
        }
    }
    if (cgn->nodeType==NT_LDOBJ && getEscState(cgn)!=GLOBAL_ESCAPE) {
        inst = cgn->nInst;
        lObjectHistory(inst,text,os);
        scannedInsts->clear();
    }
}  // printCnGNodeRefs(CnGNode* cgn, std::string text,::std::ostream& os) 


void
EscAnalyzer::lObjectHistory(Inst* inst,std::string text,::std::ostream& os) {
    Inst* inst1;
    uint32 nsrc=inst->getNumSrcOperands();

    if (scannedInsts->size()!=0) {
        if (checkScannedInsts(inst->getId())) {
            os << text << "instId " << inst->getId() << "  .  .  . " << std::endl;
            return;
        }
    }
    os << text; inst->print(os); os << std::endl;
    if (inst->getOpcode()==Op_DirectCall || inst->getOpcode()==Op_IndirectMemoryCall) {
        Opnd *returnOpnd = inst->getDst(); 
        if (returnOpnd != NULL) {
            CnGNode* n = findCnGNode_op(returnOpnd->getId());
            if (n != NULL) {
                os<< text << "  "; printCnGNode(n,os); os<< std::endl;
            }
        }
        if (inst->getOpcode()==Op_IndirectMemoryCall) {
            MethodDesc* md;
            if (inst->getSrc(0)->getInst()->getOpcode()== Op_LdVar) {
                md = inst->getSrc(0)->getType()->asMethodPtrType()->getMethodDesc();
            } else {
                md = inst->getSrc(0)->getInst()->asMethodInst()->getMethodDesc();
            }
            os << text << "  "; md->printFullName(os); os << std::endl;
        }
        return;
    }
    scannedInsts->push_back(inst->getId());
    for (uint32 i=0; i<nsrc; i++) {
        inst1 = inst->getSrc(i)->getInst();
        if (!(Type::isTau(inst->getSrc(i)->getType()->tag)))
            lObjectHistory(inst1,text+" ",os);
    }
}  // lObjectHistory(Inst* inst,std::string text,::std::ostream& os) 


void
EscAnalyzer::printCreatedObjectsInfo(::std::ostream& os) {
    CnGNodes::iterator it;
    NodeMDs::iterator it1;
    os << "================ Created Object States   < "; 
    irManager.getMethodDesc().printFullName(os); 
    os << std::endl;
    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        uint32 nt = (*it)->nodeType;
        if (nt&(NT_OBJECT|NT_RETVAL) || nt==NT_LDOBJ) {
            printCnGNode(*it,os);
            ((Opnd*)(*it)->refObj)->printWithType(os);
            if (getEscState(*it)==ARG_ESCAPE) {
                for (it1 = (*it)->nodeMDs->begin(); it1 != (*it)->nodeMDs->end(); it1++) {
                    CnGNode* n=findCnGNode_id(*it1);
                    assert(n!=NULL);
                    os << std::endl; os <<"    ";
                    printCnGNode(n,os);
                    os << std::endl; os <<"      ";
                    ((MethodDesc*)n->refObj)->printFullName(os);
                }
            }
            os << std::endl;
            os << "  =="; ((Opnd*)(*it)->refObj)->getInst()->print(os); 
            os << std::endl;
        }
    }
    os << "================                         > " ;
    irManager.getMethodDesc().printFullName(os); 
    os << std::endl;
}  // printCreatedObjectsInfo(::std::ostream& os) 


void
EscAnalyzer::createdObjectInfo() {
    CnGNodes::iterator it;
    int n0 = 0;
    int n_ge = 0;
    int n_ae = 0;
    int n_ne = 0;
    uint32 state = 0;

    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        if ((*it)->nodeType == NT_OBJECT) {
            n0++;
            state = getEscState(*it);
            if (state == GLOBAL_ESCAPE)
                n_ge++;
            if (state == ARG_ESCAPE)
                n_ae++;
            if (state == NO_ESCAPE)
                n_ne++;
        }
    }
    if (_printstat==1) {
        Log::out() << "**************  Created object info: ";
        mh.printFullName(Log::out()); 
        Log::out() << std::endl; 
        Log::out() << "  Number of created objects: " << n0 << std::endl; 
        if (n0>0) {
            Log::out() << "    Global escaped objects: " << n_ge << std::endl; 
            Log::out() << "    Arg. escaped objects: " << n_ae << std::endl; 
            Log::out() << "    Non escaped objects: " << n_ne << std::endl; 
        }
        Log::out() << "**************  " << std::endl; 
    }

    comObjStat._n0+=n0;
    comObjStat._n_ge+=n_ge;
    comObjStat._n_ae+=n_ae;
    comObjStat._n_ne+=n_ne;

    if (_printstat==1) {
        Log::out() << "**************  Common created object info " << std::endl; 
        Log::out() << "  Number of created objects: " << comObjStat._n0 << std::endl; 
        Log::out() << "    Global escaped objects: " << comObjStat._n_ge << std::endl; 
        Log::out() << "    Arg. escaped objects: " << comObjStat._n_ae << std::endl; 
        Log::out() << "    Non escaped objects: " << comObjStat._n_ne << std::endl; 
        Log::out() << "    Objects in loop: " << comObjStat._n_lo << std::endl; 
        Log::out() << "**************  " << std::endl; 
    }
}  // createdObjectInfo() 


void
EscAnalyzer::printOriginObjects(Inst* inst, bool all, std::string text) {
    Inst* inst1 = NULL;
    uint32 nsrc=inst->getNumSrcOperands();

    if (scannedObjs->size()!=0) {
        if (checkScannedObjs(inst->getId())) {
            if (_seinfo || _scinfo) {
                Log::out() << "instId " << inst->getId() 
                    << "  .  .  . " << std::endl;
            }
            return;
        }
    }
    if (_seinfo || _scinfo) {
        Log::out() << text; 
        inst->print(Log::out()); 
        Log::out() << std::endl;
    }
    if (inst->getOpcode()==Op_DirectCall || inst->getOpcode()==Op_IndirectMemoryCall) {
        Opnd *returnOpnd = inst->getDst(); 
        if (returnOpnd != NULL) {
            CnGNode* n = findCnGNode_op(returnOpnd->getId());
            if (n != NULL) {
                if (_seinfo || _scinfo) {
                    Log::out()<< text << "  ";
                    printCnGNode(n,Log::out()); 
                    Log::out()<< std::endl;
                }
            }
        }
        if (inst->getOpcode()==Op_IndirectMemoryCall) {
            MethodDesc* md;
            if (inst->getSrc(0)->getInst()->getOpcode()== Op_LdVar) {
                md = inst->getSrc(0)->getType()->asMethodPtrType()->getMethodDesc();
            } else {
                md = inst->getSrc(0)->getInst()->asMethodInst()->getMethodDesc();
            }
            if (_seinfo || _scinfo) {
                Log::out() << text << "  "; 
                md->printFullName(Log::out() ); 
                Log::out() << std::endl;
            }
        }
        return;
    }
    if (inst->getOpcode()==Op_TauLdInd || inst->getOpcode()==Op_LdVar) { // ldind,ldvar 
        Opnd *dst = inst->getDst(); 
        CnGNode* n = findCnGNode_op(dst->getId());
        if (n != NULL) {
            if (_seinfo || _scinfo) {
                Log::out()<< text << "  "; 
                printCnGNode(n,Log::out()); 
                Log::out()<< std::endl;
            }
        }
    }
    switch (inst->getOpcode()) {
        case Op_LdRef:           // ldref
        case Op_NewObj:          // newobj
        case Op_NewArray:        // newarray
        case Op_NewMultiArray:   // newmultiarray
        case Op_DefArg:          // defarg
        {
            CnGNode* n = findCnGNode_in(inst->getId());
            if (n != NULL) {
                if (_seinfo || _scinfo) {
                    Log::out() << text << "  "; 
                    printCnGNode(n,Log::out() ); 
                    Log::out() << std::endl;
                }
            }
            break;
        }
        default:
            break;
    }
    scannedObjs->push_back(inst->getId());
    if (all) {
        for (uint32 i=0; i<nsrc; i++) {
            inst1 = inst->getSrc(i)->getInst();
            printOriginObjects(inst1,true,text+" ");
        }
    } else {
        switch (inst->getOpcode()) {
            case Op_TauLdInd:        // ldind
            case Op_AddScaledIndex:  // addindex
                inst1 = inst->getSrc(0)->getInst();
                printOriginObjects(inst1,false,text+" ");
                break;
            case Op_TauStInd:        // stind
                for (uint32 i=0; i<2; i++) {
                    inst1 = inst->getSrc(i)->getInst();
                    printOriginObjects(inst1,false,text+" ");
                }
                break;
            default:
                for (uint32 i=0; i<nsrc; i++) {
                    inst1 = inst->getSrc(i)->getInst();
                    printOriginObjects(inst1,false,text+" ");
                }
        }
    }
    scannedObjs->pop_back();
}  // printOriginObjects(Inst* inst, bool all, std::string text) 


void
EscAnalyzer::printMethodInfo(CalleeMethodInfo* mi) {
    ParamInfos::iterator it2;
    Log::out() << "==== debug ===== calleeMethodInfo  " << std::endl;
    if (mi==NULL) 
        Log::out() << "  calleeMethodInfo is NULL " << std::endl;
    else {
        Log::out() << mi->methodIdent->parentName << "  ";
        Log::out() << mi->methodIdent->name << "  ";
        Log::out() << mi->methodIdent->signature << "  ";
        Log::out() << "Number of parameters: " << mi->numberOfArgs<< "  " << std::endl;
        for (it2 = mi->paramInfos->begin( ); it2 != mi->paramInfos->end( ); it2++) {
            Log::out() << (*it2)->paramNumber << "  st.";
            printState((*it2)->state);
            Log::out() << std::endl;
        }
        Log::out() << "Return value state: ";
        printState(mi->retValueState);
        Log::out() << "  " << std::endl;
    }
    Log::out() << "====  end  ===== calleeMethodInfo  " << std::endl;
}  // printMethodInfo(CalleeMethodInfo* mi) 


void
EscAnalyzer::what_inst(Inst* inst,::std::ostream& os) {
    if (inst->asCallInst())
        os << "        CallInst" << std::endl;
    if (inst->asFieldAccessInst()) {
        os << "        FieldAccessInst" << std::endl;
        FieldAccessInst* fai=inst->asFieldAccessInst();
        FieldDesc* fd=fai->getFieldDesc();
        Type* tt=fd->getFieldType();
        os << "      isInitOnly        " << fd->isInitOnly() << std::endl;
        os << "      isVolatile        " << fd->isVolatile() << std::endl;
        os << "      fldT      " << tt->getName() <<" "<< tt->tag<< std::endl;
        os << "      isObject  " << tt->isObject() << std::endl;
        os << "      isRef     " << tt->isReference()<< std::endl;
        if (tt->isReference()) {
            ref_type_info(tt,os);
        }
        os << "      getName    " << fd->getName() << std::endl;
        os << "      signature  " << fd->getSignatureString() << std::endl;
        os << "      parentType " << fd->getParentType()->getName() << std::endl;
        os << "      getId      " << fd->getId() << std::endl;
        os << "      isPrivate  " << fd->isPrivate() << std::endl;
        os << "      isStatic   " << fd->isStatic() << std::endl;
        os << "      ";
        fd->printFullName(os);
        os << std::endl;
    }
    if (inst->asIntrinsicCallInst())
        os << "        IntrinsicCallInst" << std::endl;
    if (inst->asMethodCallInst())
        os << "        MethodCallInst" << std::endl;
    if (inst->asMultiSrcInst())
        os << "        MultiSrcInst" << std::endl;
    if (inst->asVarAccessInst()) {
        os << "        VarAccessInst" << std::endl;
        if (inst->asVarAccessInst()->getVar()!=NULL) {
            os << "          Var:  ";
            inst->asVarAccessInst()->getVar()->print(os);os << std::endl;
            os << "          ";
            inst->asVarAccessInst()->getVar()->printWithType(os);os<< std::endl;
        }
        if (inst->asVarAccessInst()->getBaseVar()!=NULL) {
            os << "          BaseVar:  ";
            inst->asVarAccessInst()->getBaseVar()->print(os);os << std::endl;
            os << "          ";
            inst->asVarAccessInst()->getBaseVar()->printWithType(os);os << std::endl;
        }
    }
    if (inst->asBranchInst())
        os << "        BranchInst" << std::endl;
    if (inst->asCatchLabelInst())
        os << "        CatchLabelInst" << std::endl;
    if (inst->asConstInst())
        os << "        ConstInst" << std::endl;
    if (inst->asDispatchLabelInst())
        os << "        DispatchLabelInst" << std::endl;
    if (inst->asLabelInst())
        os << "        LabelInst" << std::endl;
    if (inst->asMethodEntryInst())
        os << "        MethodEntryInst" << std::endl;
    if (inst->asMethodInst()) {
        os << "        MethodInst" << std::endl;
        MethodDesc* md=inst->asMethodInst()->getMethodDesc();
        os << "          isNative " << md->isNative() << std::endl;
        os << "          isSynchronized " << md->isSynchronized() << std::endl;
        os << "          isStatic " << md->isStatic() << std::endl;
        os << "          isInstsnce " << md->isInstance() << std::endl;
        os << "          isFinal " << md->isFinal() << std::endl;
        os << "          isVirtual " << md->isVirtual() << std::endl;
        os << "          isAbstract " << md->isAbstract() << std::endl;
        os << "          isClassInitializer " << md->isClassInitializer() << std::endl;
        os << "          isInstanceInitializer " << md->isInstanceInitializer() << std::endl;
        os << "          isOverridden " << md->isOverridden() << std::endl;
        os << "          Name " << md->getName() << std::endl;
        os << "          Signature " << md->getSignatureString() << std::endl;
        uint32 n=md->getNumParams();
        os << "          Params " << n << std::endl;
        for (uint32 i = 0; i < n; i++) {
            Type* tt = md->getParamType(i);
            os << "          << "<<i<<" >> " << tt->getName() <<" "<< tt->tag<< std::endl;
            os << "              isObject  " << tt->isObject();
            os << "  isRef  " << tt->isReference()<< std::endl;
            if (tt->isReference()) {
                ref_type_info(tt,os);
            }
        }
        os << "          Id " << md->getId() << std::endl;
        os << "          isPrivate " << md->isPrivate() << std::endl;
        os << "          ParentName " << md->getParentType()->getName();os << std::endl;
        md->printFullName(os);os << std::endl;
    }
    if (inst->asMethodMarkerInst())
       os << "        MethodMarkerInst" << std::endl;
    if (inst->asPhiInst())
       os << "        PhiInst" << std::endl;
    if (inst->asTauPiInst())
       os << "        TauPiInst" << std::endl;
    if (inst->asSwitchInst())
       os << "        SwitchInst" << std::endl;
    if (inst->asTokenInst())
       os << "        TokenInst" << std::endl;
    if (inst->asTypeInst()) {
       Type* tt = inst->asTypeInst()->getTypeInfo();
       os << "        TypeInst" << std::endl;
       os << "          "<< tt->getName() <<" "<< tt->tag<< std::endl;
    }
}  // what_inst(Inst* inst,::std::ostream& os) 


void
EscAnalyzer::ref_type_info(Type* type,::std::ostream& os) {
    NamedType* arreltype;
    os << "           isArr " << type->isArray();
    if (type->asArrayType()) {
        arreltype=type->asArrayType()->getElementType();
        os << " elmT " << arreltype->getName() << " " << arreltype->tag<<" ";
        os << " isRef " << arreltype->isReference();
    }
    os << " isArrElem " << type->isArrayElement()<< std::endl;
}  // ref_type_info(Type* type,::std::ostream& os) 


void
EscAnalyzer::debug_inst_info(Inst* inst,::std::ostream& os) {
    Opnd* dst;
    Opnd* src;
    uint32 nsrc;

    os << "  =="; inst->print(os); os << std::endl;
    os << "  Inst Info:" << std::endl;
    what_inst(inst,os);
    os << "  Dst & Src Info:" << std::endl;
    dst=inst->getDst();
    nsrc=inst->getNumSrcOperands();
    os << "  ";
    if (!dst->isNull())
        dst->print(os);
    else
        os << "dst NULL";
    os << "  --srcnum " << nsrc << std::endl;
    if (!dst->isNull()) {
        os << "  dst ";
        debug_opnd_info(dst, os);
    } else
        os << std::endl;
    if ( nsrc != 0 ) {
        os << "  sources" << std::endl;
        for (uint32 i=0; i<nsrc; i++) {
            src=inst->getSrc(i);
            os << "  <<" <<i<<">> ";
            debug_opnd_info(src, os);
        }
    }
}  // debug_inst_info(Inst* inst,::std::ostream& os) 


void
EscAnalyzer::debug_opnd_info(Opnd* opnd,::std::ostream& os) {
    Type* type=opnd->getType();

    opnd->print(os);
    os << " id. " <<opnd->getId();
    os << " type " << type->getName() << " " << type->tag<<" ";
    os << " isRef " << type->isReference();
    os << " isObj " << type->isObject();
    os << " isVal " << type->isValue() << std::endl;
    if (type->isReference()) 
        ref_type_info(type,os);
    os << "        ";
    opnd->printWithType(os );
    os << std::endl;
    os << "        prop " << opnd->getProperties();
    os << " isVar " << opnd->isVarOpnd();
    os << " isSsa " << opnd->isSsaOpnd();
    os << " isSsaVar " << opnd->isSsaVarOpnd();
    os << " isSsaTmp " << opnd->isSsaTmpOpnd();
    os << " isPi " << opnd->isPiOpnd() << std::endl;
    if (!opnd->isVarOpnd()) {
        os << "        ";
        opnd->getInst()->print(os);
        os << std::endl;
        what_inst(opnd->getInst(),os);
    }
}  // debug_opnd_info(Opnd* opnd,::std::ostream& os) 



/* ****************************************
    Monitors elimination optimization
**************************************** */

void 
EscAnalyzer::addMonInst(Inst* inst) {
    uint32 monOpndId = inst->getSrc(0)->getId();
    MonUnit* monUnit = NULL;
    monUnit = findMonUnit(monOpndId);
    if (monUnit == NULL) {
        monUnit = new (eaMemManager) MonUnit;  // new monitor unit
        monUnit->opndId = monOpndId;
        monUnit->monInsts = new (eaMemManager) Insts(eaMemManager);
        monitorInstUnits->push_back(monUnit);
        monUnit->icallInsts = NULL;
    }
    monUnit->monInsts->push_back(inst);
}  // addMonInst(Inst* inst) 


EscAnalyzer::MonUnit* 
EscAnalyzer::findMonUnit(uint32 opndId) {
    MonInstUnits::iterator it;
    assert(monitorInstUnits != NULL);
    for (it = monitorInstUnits->begin( ); it != monitorInstUnits->end( ); it++ ) {
        if ((*it)->opndId == opndId)
            return *it;
    }
    return NULL;
}


void 
EscAnalyzer::addMonUnitVCall(MonUnit* mu, Inst* inst) {
    if (mu->icallInsts == NULL) {
        mu->icallInsts = new (eaMemManager) Insts(eaMemManager);
    }
    mu->icallInsts->push_back(inst);
}  // addMonUnitVCall(MonUnit* mu, Inst* inst) 


bool
EscAnalyzer::checkMonitorsOnThis() {
    MonInstUnits::iterator it;
    Insts::iterator it1;
    CnGNode* node;

    if (monitorInstUnits==NULL)
        return false;
    for (it = monitorInstUnits->begin( ); it != monitorInstUnits->end( ); it++ ) {
        node = findCnGNode_op((*it)->opndId);

        Inst* opndInst = node->nInst;
#ifdef _DEBUG
        if (_seinfo) {
            Log::out() << "    checkMOT: ";
            opndInst->print(Log::out()); 
            Log::out()  << std::endl;
        }
#endif
        if (opndInst->getOpcode()==Op_DefArg && opndInst->getDefArgModifier()==NonNullThisArg) {
#ifdef _DEBUG
            if (_seinfo) {
                Log::out() << "    checkMOT: " << (int)(opndInst->getDefArgModifier()) << " "
                    << (opndInst->getDefArgModifier()==DefArgNoModifier) << " " 
                    << (opndInst->getDefArgModifier()==NonNullThisArg) << " " 
                    << (opndInst->getDefArgModifier()==DefArgBothModifiers) << "  state: ";
                printState(node,Log::out());
                Log::out() << std::endl;
                if (getEscState(node) != GLOBAL_ESCAPE) {
                    Log::out() << "    defarg.ths isn't global "<< std::endl; 
                }
            }
#endif
            return true;
        }
    }
    return false;
} // checkMonitorsOnThis()


void
EscAnalyzer::scanSyncInsts() {
    MonInstUnits::iterator it;
    Insts::iterator it1;
    NodeMDs::iterator it2;
    Insts* syncInsts;
    Insts* vcInsts;
    CnGNode* node;
    uint32 checkedState = 0;
    uint32 nState = 0;
    bool to_fix_ssa = false;
    BitSet fgnodes(eaMemManager,irManager.getFlowGraph().getMaxNodeId());

    if (monitorInstUnits == NULL)
        return;

#ifdef _DEBUG
    if (_seinfo) {
        if (monitorInstUnits->size() > 0) {
            Log::out() << "Synchronized units: " << monitorInstUnits->size() << std::endl;
        }
    }
#endif
    for (it = monitorInstUnits->begin( ); it != monitorInstUnits->end( ); it++ ) {
        node = findCnGNode_op((*it)->opndId);
        checkedState = 0;
        nState = getEscState(node);
        if (node->nodeType == NT_OBJECT || nState == GLOBAL_ESCAPE)
            checkedState = nState;
        if (nState != GLOBAL_ESCAPE && (node->nodeType != NT_OBJECT)) {
            checkedState = checkState(node->nInst,nState);
            scannedObjs->clear();
            if (checkedState != nState && checkedState != 0) {
#ifdef _DEBUG
                if (_setState) {
                    Log::out() <<"--scanSyncInsts 1: node "
                            <<node->cngNodeId<<" opndId "<<node->opndId
                            <<" state ";
                    printState(node); 
                    Log::out()<<" to esc.state "<< checkedState << std::endl;
                }
#endif
                setEscState(node,checkedState);
            }
        }
#ifdef _DEBUG
        if (_seinfo) {
            Log::out() <<"--sync: state "; 
            Log::out() << checkedState << "  "; 
            if (checkedState != nState)
                Log::out() << "initState " << nState << "  "; 
            Log::out() << node->cngNodeId << " - " << node->opndId << "  "; 
            printCnGNode(node,Log::out()); 
            Log::out() << std::endl;
            if (node->nodeMDs !=NULL) {
                Log::out() << "     Callee methods (nodeMDs): " << std::endl;
                for (it2 = node->nodeMDs->begin(); it2 != node->nodeMDs->end(); it2++) {
                    CnGNode* n=findCnGNode_id(*it2); 
                    assert(n!=NULL);
                    Log::out() << "    " << n->nInst->getNode()->getId() << "  "; 
                    FlowGraph::printLabel(Log::out(),n->nInst->getNode());
                    Log::out() << "   "; n->nInst->print(Log::out());
                    Log::out()<< std::endl;
                }
            } else {
                Log::out() << "      no Callee methods (nodeMDs) " << std::endl;
            }
        }
#endif

        syncInsts = (*it)->monInsts;
        vcInsts = (*it)->icallInsts;
#ifdef _DEBUG
        if (_seinfo) {
            for (it1 = syncInsts->begin( ); it1 != syncInsts->end( ); it1++ ) {
                Log::out() << "   -"; 
                (*it1)->print(Log::out());
                Log::out() << "  // node " 
                    << (*it1)->getNode()->getId() << "  ";
                FlowGraph::printLabel(Log::out(),(*it1)->getNode());
                Log::out()<< std::endl;
            }
            if (vcInsts != NULL) {
                Log::out() << "     VCallee methods (icallInsts): " << std::endl;
                for (it1 = vcInsts->begin( ); it1 != vcInsts->end( ); it1++ ) {
                    Log::out() << "    node " 
                        << (*it1)->getNode()->getId() << "  ";
                    FlowGraph::printLabel(Log::out(),(*it1)->getNode());
                    Log::out() << "   "; 
                    (*it1)->print(Log::out()); Log::out() << std::endl;
                }
            } else {
                Log::out() << "     no VCallee methods (icallInsts) " << std::endl;
            }
        }
#endif
        if (getVirtualCall(node)!=0) {
            if (checkedState > GLOBAL_ESCAPE && do_sync_removal_vc) {
                uint32 bs_size = irManager.getFlowGraph().getMaxNodeId();
                if (fgnodes.getSetSize() < bs_size) {
                    fgnodes.resizeClear(bs_size);
                } else {
                    fgnodes.clear();
                }
                markLockedNodes(&fgnodes,syncInsts);
#ifdef _DEBUG
                if (_seinfo) {
                    Log::out() << "=-=- vc loc.esc."  << std::endl;
                    printOriginObjects(node->nInst,false);
                    scannedObjs->clear();
                }
#endif
                if (node->nodeType==NT_OBJECT) {
                    if (_seinfo) {
                        Log::out() << "=-=- vc to optimize object  ";
                        node->nInst->print(Log::out());
                        Log::out() << std::endl;
                    }
                    fixMonitorInstsVCalls(*it,&fgnodes);
                }
                if (node->nodeType==NT_RETVAL) {
                    if (_seinfo) {
                        Log::out() << "=-=- vc to optimize retval  ";
                        node->nInst->print(Log::out());
                        Log::out() << std::endl;
                    }
                    fixMonitorInstsVCalls(*it,&fgnodes);
                }
                to_fix_ssa = true;
            } else {
#ifdef _DEBUG
               if (_seinfo && do_sync_removal_vc) {
                    Log::out() << "=-=- vc gl.esc."  << std::endl;
                    printOriginObjects(node->nInst,false);
                    scannedObjs->clear();
                }
#endif
            }
        } else {
            if (node->nodeType==NT_OBJECT && getEscState(node) != GLOBAL_ESCAPE) {
                if (_seinfo) {
                    Log::out() << "++++ to optimize (remove) object"  << std::endl;
                }
                removeMonitorInsts(syncInsts);
            }

            if (node->nodeType==NT_DEFARG && getEscState(node) != GLOBAL_ESCAPE
                    && node->nInst->getDefArgModifier()==NonNullThisArg && mh.isSynchronized()) {
                if (_seinfo) {
                    Log::out() << "++++ to optimize (fix) defarg.ths"  << std::endl;
                }
#ifndef PLATFORM_POSIX
                if (do_sync_removal_sm) {
                    fixSyncMethodMonitorInsts(syncInsts);
                }
#endif
            }
        }
    }
#ifndef PLATFORM_POSIX
    if (do_sync_removal_sm) {
        checkCallSyncMethod();
    }
#endif
    // to fix var operand inserted by fixMonitorInstsVCalls method
    if (to_fix_ssa) { 
        eaFixupVars(irManager);
    }
    return;
}  // scanSyncInsts() 


void 
EscAnalyzer::markLockedNodes(BitSet* bs, Insts* syncInsts) {
    Insts::iterator si_i;
    Opnd* mop = NULL;
    Inst* inst = NULL;
    Node* n = NULL;
    if (syncInsts == NULL) {
        return;
    }
    scannedObjs->clear();
    mop = syncInsts->front()->getSrc(0);
    for (si_i = syncInsts->begin(); si_i != syncInsts->end(); si_i++) {
        if ((*si_i)->getOpcode() == Op_TauMonitorEnter) { //monenter
            n = (*si_i)->getNode();
            bs->setBit(n->getId(),true);
#ifdef _DEBUG
            if (_seinfo) {
                Log::out() << "=-=-=Marked node: "  << n->getId() << " ";
                FlowGraph::printLabel(Log::out(),n); Log::out() << "   ";
                (*si_i)->print(Log::out());  Log::out() << std::endl;
            }
#endif
            inst = (Inst*)(n->getLastInst());
            assert(inst!=NULL);
            if (inst->getOpcode() == Op_TauMonitorExit) {
                continue;
            }
            if (n->getOutDegree() != 0) {
                markLockedNodes2(n, bs, mop);
            }
        }
    }
} // markLockedNodes(BitSet* bs, Insts* syncInsts)


bool 
EscAnalyzer::markLockedNodes2(Node* node, BitSet* bs, Opnd* moninstop) {
    const Edges& oedges = node->getOutEdges();
    Edges::const_iterator eit;
    Inst* inst;
    Node* n;
    bool found = true;

    if (scannedObjs->size()!=0) {
        if (checkScannedObjs(node->getId())) {
#ifdef _DEBUG
            if (_seinfo) {
                Log::out() << "=-=- marked node previously: "  << node->getId() << " ";
                FlowGraph::printLabel(Log::out(),node);
                Log::out() << std::endl;
            }
#endif
            return found;
        }
    }
    scannedObjs->push_back(node->getId());
    for (eit=oedges.begin(); eit!=oedges.end(); eit++) {
        n = (*eit)->getTargetNode();
        bs->setBit(n->getId(),true);
#ifdef _DEBUG
        if (_seinfo) {
            Log::out() << "=-=- Marked node: "  << n->getId() << " ";
            FlowGraph::printLabel(Log::out(),n);
            Log::out() << std::endl;
        }
#endif
        inst = (Inst*)(n->getLastInst());
        if (inst !=NULL) {
            if (inst->getOpcode() == Op_TauMonitorExit) {
#ifdef _DEBUG
                if (_seinfo) {
                    Log::out() << "=-=- Marked node: found monexit  "  << n->getId() << " ";
                    FlowGraph::printLabel(Log::out(),n); Log::out() << "   ";
                    inst->print(Log::out());  Log::out() << std::endl;
                }
#endif
                continue;
            }
        }
        if (n->getOutDegree() != 0) {
            found = markLockedNodes2(n,bs,moninstop);
        } else {
            found = false;
        }
    }
    return found;
} // markLockedNodes2(BitSet* bs, Insts* syncInsts)


uint32 
EscAnalyzer::checkState(Inst* inst,uint32 st) {
    uint32 st1;
    Inst* inst1;
    Opnd* opnd1;
    uint32 nsrc=inst->getNumSrcOperands();

    if (st <= GLOBAL_ESCAPE)
        return st;
    if (scannedObjs->size()!=0) {
        if (checkScannedObjs(inst->getId())) {
            return st;
        }
    }
    if (inst->getOpcode()==Op_DirectCall || inst->getOpcode()==Op_IndirectMemoryCall) {
        Opnd *returnOpnd = inst->getDst(); 
        if (returnOpnd != NULL) {
            CnGNode* n = findCnGNode_op(returnOpnd->getId());
            if (n != NULL) {
                st1 = getEscState(n);
                if (st > st1)
                    st=st1;
            }
        }
        return st;
    }
    if (st <= GLOBAL_ESCAPE)
        return st;
    if (inst->getOpcode()==Op_TauLdInd || inst->getOpcode()==Op_LdVar) {  // ldind, ldvar
        Opnd *dst = inst->getDst(); 
        CnGNode* n = findCnGNode_op(dst->getId());
        if (n != NULL) {
            st1 = getEscState(n);
            if (st > st1)
                st=st1;
        }
    }
    if (st <= GLOBAL_ESCAPE)
        return st;
    switch (inst->getOpcode()) {
        case Op_LdRef:           // ldref
        case Op_NewObj:          // newobj
        case Op_NewArray:        // newarray
        case Op_NewMultiArray:   // newmultiarray
        case Op_DefArg:          // defarg
        {
            CnGNode* n = findCnGNode_in(inst->getId());
            if (n != NULL) {
                st1 = getEscState(n);
                if (st > st1)
                    st=st1;
            }
            break;
        }
        default:
            break;
    }
    if (st <= GLOBAL_ESCAPE)
        return st;
    scannedObjs->push_back(inst->getId());
    for (uint32 i=0; i<nsrc; i++) {
        opnd1 = inst->getSrc(i);
        if (opnd1->isVarOpnd()) {
            inst1 = opnd1->asVarOpnd()->getVarAccessInsts();
        } else {
            inst1 = opnd1->getInst();
        }
        st1 = checkState(inst1,st);
        if (st > st1)
            st=st1;
        if (st<=GLOBAL_ESCAPE)
            break;
    }
    scannedObjs->pop_back();
    return st;
} // checkState(Inst* inst,uint32 st) 


void 
EscAnalyzer::fixMonitorInstsVCalls(MonUnit* mu, BitSet* bs) {
    Inst* opi = findCnGNode_op(mu->opndId)->nInst;
    OpndManager& _opndManager = irManager.getOpndManager();
    InstFactory& _instFactory = irManager.getInstFactory();
    TypeManager& _typeManager  = irManager.getTypeManager();
    Type* typeInt32 = _typeManager.getInt32Type();
    VarOpnd* muflag = _opndManager.createVarOpnd(typeInt32, false);
    Inst* stvar0; // = _instFactory.makeStVar(muflag, i32_0);
    Inst* stvar1;
    Insts::iterator inst_it;
    Insts* vcInsts = mu->icallInsts;;
    Insts* syncInsts = mu->monInsts;
    Node* oldCallNode = NULL;
    Node* addedMonNode = NULL;
    Node* newCallNode = NULL;
    ControlFlowGraph& fg = irManager.getFlowGraph();
#ifdef _DEBUG
    Node* entry_node = fg.getEntryNode();
    Node* muo_node = opi->getNode();
#endif

    // values 0 and 1 to set flag variable
#ifdef _DEBUG
    if (_seinfo) {
        Log::out() << "=-=- w0 Before " << std::endl;
        FlowGraph::print(Log::out(),entry_node);
    }
#endif
    insertLdConst(1);
    insertLdConst(0);
#ifdef _DEBUG
    if (_seinfo) {
        Log::out() << "=-=- w0 After " << std::endl;
        FlowGraph::print(Log::out(),entry_node);
    }
#endif

    // insert flag=0 before monitor instruction source opnd creation instruction
#ifdef _DEBUG
    if (_seinfo) {
        Log::out() << "=-=- w1 Before " << std::endl;
        FlowGraph::print(Log::out(),muo_node);
    }
#endif
    stvar0 = _instFactory.makeStVar(muflag, i32_0);
    stvar0->insertBefore(opi);
#ifdef _DEBUG
    if (_seinfo) {
        Log::out() << "=-=- w1 After " << std::endl;
        FlowGraph::print(Log::out(),muo_node);
    }
#endif

    // insert flag=1 before virtual call instructions
#ifdef _DEBUG
    if (_seinfo) {
        Log::out() << "=-=-=-=- Start w2" << std::endl;
    }
#endif
    for (inst_it = vcInsts->begin( ); inst_it != vcInsts->end( ); inst_it++ ) {
        oldCallNode = (*inst_it)->getNode();
        addedMonNode = NULL;
        newCallNode = NULL;
#ifdef _DEBUG
        if (_seinfo) {
            Log::out() << "=-=- w2 Before " << std::endl;
            FlowGraph::print(Log::out(),oldCallNode);
            if (bs->getBit(oldCallNode->getId())) {
                Log::out() << "=-=- monenter is needed " << std::endl;
            } else {
                Log::out() << "=-=- monenter isn't needed " << std::endl;
            }
        }
#endif
        stvar1 = _instFactory.makeStVar(muflag, i32_1);
        stvar1->insertBefore(*inst_it);
        if (bs->getBit((*inst_it)->getNode()->getId())) {
            Inst* mi = syncInsts->front();
            Inst* ime = _instFactory.makeTauMonitorEnter(mi->getSrc(0), mi->getSrc(1));
            ime->insertBefore(*inst_it);
            newCallNode = fg.splitNodeAtInstruction(ime,true,false,_instFactory.makeLabel());
            SsaTmpOpnd* opflag = _opndManager.createSsaTmpOpnd(typeInt32);
            _instFactory.makeLdVar(opflag,(VarOpnd*)muflag)->insertBefore(stvar1);
            Inst* branch_inst = _instFactory.makeBranch(Cmp_EQ, Type::Int32, 
                opflag, i32_1, (LabelInst*)(newCallNode->getFirstInst()));
            // insert flag check
            branch_inst->insertBefore(stvar1);
            addedMonNode =  fg.splitNodeAtInstruction(branch_inst,true, false, _instFactory.makeLabel());
            fg.addEdge(oldCallNode,newCallNode);        
        }
#ifdef _DEBUG
        if (_seinfo) {
            if (bs->getBit(oldCallNode->getId())) {
                Log::out() << "=-=- monenter is inserted " << std::endl;
            }
            Log::out() << "=-=- w2 After " << std::endl;
            FlowGraph::print(Log::out(),oldCallNode);
            if (addedMonNode != NULL) {
                FlowGraph::print(Log::out(),addedMonNode);
            }
            if (newCallNode != NULL) {
                FlowGraph::print(Log::out(),newCallNode);
            }
        }
#endif
    }
#ifdef _DEBUG
    if (_seinfo) {
        Log::out() << "=-=-=-=- Finish w2" << std::endl;
    }
#endif

    insertFlagCheck(syncInsts,muflag,0);

} // fixMonitorInstsVCalls(MonUnit* mu, BitSet* bs)


void 
EscAnalyzer::insertFlagCheck(Insts* syncInsts, Opnd* muflag, uint32 chk) {
    Insts::iterator inst_it;
    OpndManager& _opndManager = irManager.getOpndManager();
    InstFactory& _instFactory = irManager.getInstFactory();
    ControlFlowGraph& fg = irManager.getFlowGraph();
    TypeManager& _typeManager  = irManager.getTypeManager();
    Type* typeInt32 = _typeManager.getInt32Type();
    SsaTmpOpnd* chk_opnd = NULL;

    // check flag before monitor instructions
    assert(muflag->isVarOpnd()||muflag->isSsaTmpOpnd());
    if (chk == 0) {
        chk_opnd = insertLdConst(0);
    }
    if (chk == 1) {
        chk_opnd = insertLdConst(1);
    }
    assert(chk_opnd!=NULL);
#ifdef _DEBUG
    if (_seinfo) {
        Log::out() << "=-=-=-=- Start w3" << std::endl;
    }
#endif
    for (inst_it = syncInsts->begin( ); inst_it != syncInsts->end( ); inst_it++ ) {
        Inst* curMonInst = (*inst_it);
        Node* oldnode = curMonInst->getNode();
#ifdef _DEBUG
        if (_seinfo) {
            Log::out() << "=-=- w3 Before " << std::endl;
            FlowGraph::print(Log::out(),oldnode);
        }
#endif
        Node* afterMonInstBlock = NULL;
        Node* tiInstBlock = NULL;
        if ((*inst_it)->getNextInst()!=NULL) {
            // monitor inst isn'n last
            tiInstBlock = fg.splitNodeAtInstruction(curMonInst, true, false, _instFactory.makeLabel());
            afterMonInstBlock = tiInstBlock;
        } else {
            // monitor inst is last
            afterMonInstBlock = (Node*)(oldnode->getUnconditionalEdge()->getTargetNode());
        }
        SsaTmpOpnd* i32_flag;
        if (muflag->isVarOpnd()) {
            i32_flag = _opndManager.createSsaTmpOpnd(typeInt32);
            _instFactory.makeLdVar(i32_flag,(VarOpnd*)muflag)->insertBefore(curMonInst);
        } else {
            i32_flag = (SsaTmpOpnd*)muflag;
        }
        Inst* branch_inst = _instFactory.makeBranch(Cmp_EQ, Type::Int32, 
                i32_flag, chk_opnd, (LabelInst*)(afterMonInstBlock->getFirstInst()));
        // insert flag check
        branch_inst->insertBefore(curMonInst);
#ifdef _DEBUG
        Node* monInstBlock = 
#endif
        fg.splitNodeAtInstruction(branch_inst,true, false, _instFactory.makeLabel());
        fg.addEdge(oldnode,afterMonInstBlock);
#ifdef _DEBUG
        if (_seinfo) {
            Log::out() << "=-=- w3 After " << std::endl;
            FlowGraph::print(Log::out(),oldnode);
            FlowGraph::print(Log::out(),monInstBlock);
            if (tiInstBlock != NULL) {
                FlowGraph::print(Log::out(),tiInstBlock);
            }
        }
#endif
    }
#ifdef _DEBUG
    if (_seinfo) {
        Log::out() << "=-=-=-=- Finish w3" << std::endl;
    }
#endif
} // insertFlagCheck(Insts* syncInsts, Opnd* muflag, uint32 chk)


void
EscAnalyzer::removeMonitorInsts(Insts* syncInsts) {
    ControlFlowGraph& fg = irManager.getFlowGraph();
    Insts::iterator it1;
    Edge* excedge = NULL;
    Node* targetnode = NULL;
    Inst* reminst;
#ifdef _DEBUG
    bool canthrow = false;
#endif

    for (it1 = syncInsts->begin( ); it1 != syncInsts->end( ); it1++ ) {
        reminst = *it1;
        if (reminst->getOperation().canThrow()==true) {
            excedge = (Edge*)reminst->getNode()->getExceptionEdge();
            if (excedge != NULL)
                targetnode = excedge->getTargetNode();
            else 
                targetnode = NULL;
        } else {
            excedge = NULL;
            targetnode = NULL;
        }
#ifdef _DEBUG
        if (_seinfo) {
            canthrow = reminst->getOperation().canThrow();
            Log::out() << "  ";
            reminst->print(Log::out());
            Log::out() << std::endl;
            Log::out() << "    canThrow "<< canthrow;
            Log::out() << std::endl;
            if (excedge==NULL) {
                Log::out() << "    exception edge is NULL " << std::endl;
            } else {
                Log::out() << "    target node is " 
                    << targetnode->getId() << std::endl;
            }
            if (canthrow && (excedge==NULL)) {
                const Edges& out_edges = reminst->getNode()->getOutEdges();
                Edges::const_iterator eit;
                Node* n; 
                for (eit = out_edges.begin(); eit != out_edges.end(); ++eit) {
                    n = (*eit)->getTargetNode();
                    Log::out() << "    edge to node " 
                        << n->getId() << " kind " << (*eit)->getKind() << std::endl;
                }
            }
        }
#endif

        reminst->unlink();
        if (_seinfo) {
            Log::out() << "    unlinked: "; 
            reminst->print(Log::out());
            Log::out() << std::endl;
        }
        if (targetnode != NULL) {
            if (targetnode->getInEdges().size() > 1) {
                fg.removeEdge(excedge);
#ifdef _DEBUG
                if (_seinfo) {
                    Log::out() << "    removed edge: " 
                        << excedge->getSourceNode()->getId() << " -> "
                        << excedge->getTargetNode()->getId() << " kind " << excedge->getKind(); 
                    Log::out() << std::endl;
                }
#endif
            } else {
                scannedObjs->clear();
                removeNode(targetnode);
                scannedObjs->clear();
            }
        }
    }

} // removeMonitorInsts(Insts* syncInsts)


void 
EscAnalyzer::removeNode(Node* node) {
    const Edges& out_edges = node->getOutEdges();
    Edges::const_iterator eit;
    Node* n; 

#ifdef _DEBUG
    if (_seinfo) {
        Log::out() << "    to remove node " 
            << node->getId() << std::endl;
   }
#endif
    if (scannedObjs->size()!=0) {
        if (checkScannedObjs(node->getId())) {
            return;
        }
    }
    scannedObjs->push_back(node->getId());
    Nodes nodes2delete(irManager.getMemoryManager());
    for (eit = out_edges.begin(); eit != out_edges.end(); ++eit) {
        n = (*eit)->getTargetNode();
        if (n->getInEdges().size() == 1) {
            nodes2delete.push_back(n);
        }
    }
    Nodes::iterator iter = nodes2delete.begin(), end = nodes2delete.end();
    for (; iter != end; ++iter) {
        n = (*iter);
        removeNode(n);
    }
    irManager.getFlowGraph().removeNode(node);
    scannedObjs->pop_back();
#ifdef _DEBUG
    if (_seinfo) {
        Log::out() << "    removed node " 
            << node->getId() << std::endl;
    }
#endif
} // removeNode(Node* node) 


void
EscAnalyzer::fixSyncMethodMonitorInsts(Insts* syncInsts) {
    SsaTmpOpnd* stThis = (SsaTmpOpnd*)insertReadJitHelperCall();
    insertFlagCheck(syncInsts,stThis,1);
}  // fixSyncMethodMonitorInsts(Insts* syncInsts) 


Opnd* 
EscAnalyzer::insertReadJitHelperCall() {
    ControlFlowGraph& fg = irManager.getFlowGraph();
    Node* oldBlock = fg.getEntryNode();
    Inst* inst_after = (Inst*)oldBlock->getFirstInst();
    TypeManager& _typeManager = irManager.getTypeManager();
    Type* typeInt32 = _typeManager.getInt32Type();
    OpndManager& _opndManager = irManager.getOpndManager();
    SsaTmpOpnd* stThis = _opndManager.createSsaTmpOpnd(typeInt32);
    Node* newBlock = NULL;

#ifdef _DEBUG
    if (_seinfo) {
        Log::out() << "++++ insertRJHC: before"  << std::endl;
        FlowGraph::print(Log::out(),oldBlock);
        Log::out() << "++++ insertRJHC: before end"  << std::endl;
    }
#endif
        // insert jit helper call
    Opnd** args = NULL;
    InstFactory& instfactory = irManager.getInstFactory();
    Inst* jhcinst = instfactory.makeJitHelperCall(
            stThis, ReadThisState, 0, args);
    jhcinst->insertAfter(inst_after);
    newBlock = fg.splitNodeAtInstruction(jhcinst,true, false,instfactory.makeLabel());
    fg.addEdge(oldBlock,fg.getUnwindNode());
    insertLdConst(0);
#ifdef _DEBUG
    if (_seinfo) {
        Log::out() << "++++ insertRJHC: after"  << std::endl;
        FlowGraph::print(Log::out(),oldBlock);
        FlowGraph::print(Log::out(),newBlock);
        Log::out() << "++++ insertRJHC: after end"  << std::endl;
    }
#endif
    insertSaveJitHelperCall((Inst*)(newBlock->getFirstInst()),i32_0); // to restore default value
    return stThis;
}  // insertReadJitHelperCall() 


void
EscAnalyzer::checkCallSyncMethod() {
    CnGNodes::iterator it;
    CnGRefs::iterator it2;
    CnGNode* node;
    CnGNode* aanode;
    MethodDesc* md;
    CalleeMethodInfo* mtdInfo;
    uint32 callee_state;

    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        node = (*it);
        if (node->nodeType == NT_ACTARG && node->argNumber == 0) {
            md = (MethodDesc*)(node->refObj);
            if (md->isSynchronized() && node->nInst->getOpcode()==Op_DirectCall) {
                const char* ch1 = md->getParentType()->getName(); 
                const char* ch2 = md->getName(); 
                const char* ch3 = md->getSignatureString();
                mtdInfo = getMethodInfo(ch1,ch2,ch3);
                if (mtdInfo==NULL) {
                    callee_state = 0;
#ifdef _DEBUG
                    if (_seinfo) {
                        Log::out() << "=-   Methodinfo is NULL"; 
                        Log::out() << std::endl;
                    }
#endif
                } else {
                    callee_state = getMethodParamState(mtdInfo,0);
                }
#ifdef _DEBUG
                if (_seinfo) {
                    Log::out() << "---- checkCallSyncMethod:"  << std::endl;
                    node->nInst->print(Log::out()); 
                    Log::out() << std::endl;
                    printCnGNode(node,Log::out()); 
                    Log::out() << std::endl;
                    if (node->outEdges != NULL) {
                        for (it2 = node->outEdges->begin(); it2 != node->outEdges->end(); it2++ ) {
                            Log::out() << "  ccsm: ";
                            printState(callee_state);
                            Log::out() << " ";
                            printCnGNode((*it2)->cngNodeTo,Log::out());
                            Log::out() << std::endl;
                        }
                    } 
                    Log::out() << "++++ checkCallSyncMethod: ";
                    Log::out() << " instance: " << md->isInstance() <<
                        " initializer: " << md->isInstanceInitializer() << " end" << std::endl;
                }
#endif
                if (callee_state == 0)
                    callee_state = GLOBAL_ESCAPE;
                if (!isGlobalState(callee_state)) {
                    assert(node->outEdges->size()==1);
                    aanode = node->outEdges->front()->cngNodeTo;
                    if (!isGlobalState(aanode->state)&&
                        (aanode->nodeType==NT_OBJECT||aanode->nodeType==NT_RETVAL)) {
                        if (_seinfo) {
                            Log::out() << "=-=- sm this.agr.saving for  ";
                            node->nInst->print(Log::out());
                            Log::out() << std::endl;
                        }
                        insertLdConst(1);
                        insertSaveJitHelperCall(node->nInst,i32_1);
#ifdef _DEBUG
                        if (_seinfo) {
                            Log::out() 
                                << "  checkCSM: this was saved"   
                                << std::endl;
                        }
                    } else {
                        if (_seinfo) {
                            Log::out() 
                                << "  checkCSM: this wasn't saved"   
                                << std::endl;
                        }
#endif
                    }
                }
            }
        }
    }
}  // checkCallSyncMethod() 


void 
EscAnalyzer::insertSaveJitHelperCall(Inst* inst_before, SsaTmpOpnd* stVal) {
    Node* oldBlock = inst_before->getNode();
    ControlFlowGraph& fg = irManager.getFlowGraph();

#ifdef _DEBUG
    Node* icBlock = stVal->getInst()->getNode();
    if (_seinfo) {
        Log::out() << "++++ insertSJHC: before"  << std::endl;
        if (icBlock != oldBlock)
            FlowGraph::print(Log::out(),icBlock);
        FlowGraph::print(Log::out(),oldBlock);
        Log::out() << "++++ insertSJHC: before end"  << std::endl;
    }
#endif
        // create jit helper call
    Opnd* args[1] = {stVal};
    InstFactory& instfactory = irManager.getInstFactory();
    Inst* jhcinst = instfactory.makeJitHelperCall(
            OpndManager::getNullOpnd(), SaveThisState, 1, args);
       // insert jit helper
    if (inst_before->getNode()->getFirstInst() == inst_before) {
        jhcinst->insertAfter(inst_before);
    } else {
        jhcinst->insertBefore(inst_before);
    }
    UNUSED Node* newBlock = fg.splitNodeAtInstruction(jhcinst, true, false, instfactory.makeLabel());
        // add dispatch edge to oldBlock
    fg.addEdge(oldBlock,fg.getUnwindNode());
#ifdef _DEBUG
    if (_seinfo) {
        Log::out() << "++++ insertSJHC: after"  << std::endl;
        if (icBlock != oldBlock)
            FlowGraph::print(Log::out(),icBlock);
        FlowGraph::print(Log::out(),oldBlock);
        FlowGraph::print(Log::out(),newBlock);
        Log::out() << "++++ insertSJHC: after end"  << std::endl;
    }
#endif
}  // insertSaveJitHelperCall(Inst* inst_before, SsaTmpOpnd* stVal) 


SsaTmpOpnd* 
EscAnalyzer::insertLdConst(uint32 value) {
    TypeManager& _typeManager  = irManager.getTypeManager();
    Type* typeInt32 = _typeManager.getInt32Type();
    OpndManager& _opndManager = irManager.getOpndManager();
    InstFactory& _instFactory = irManager.getInstFactory();
    Inst* ildc = NULL;
    if (value == 0)
        if (i32_0 == NULL) {
            i32_0 = _opndManager.createSsaTmpOpnd(typeInt32);
            ildc = _instFactory.makeLdConst(i32_0, 0);
        }
    if (value == 1)
        if (i32_1 == NULL) {
            i32_1 = _opndManager.createSsaTmpOpnd(typeInt32);
            ildc = _instFactory.makeLdConst(i32_1, 1);
        }
    if (ildc != NULL) {
        ildc->insertAfter(irManager.getFlowGraph().getEntryNode()->getFirstInst());
    }
    if (value == 0)
        return i32_0;
    return i32_1;
} // insertLdConst(uint32 value) 



/* ****************************************
    Scalar replacement optimization
**************************************** */

void
EscAnalyzer::scanLocalObjects() {
    CnGNodes::iterator it;
    uint32 lo_count=0;             // number of local objects
    ObjIds* lnoids = NULL;         // list of new opnds to optimize
    ObjIds* lloids = NULL;         // list of load opnds to optimize
    ObjIds::iterator lo_it;
    bool prTitle = true;
    CnGNode* stnode = NULL;

    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
        if ((*it)->nodeType == NT_OBJECT && getEscState(*it)==NO_ESCAPE 
            && getOutEscaped(*it) == 0 && !((*it)->nInst->getOpcode()==Op_LdRef)) {
            if (prTitle) {
                if (_scinfo) {
                    os_sc << "================ Local Object States   < "; 
                    irManager.getMethodDesc().printFullName(os_sc); 
                    os_sc << std::endl;
                }
                prTitle = false;
            }
            lo_count++;  // number of local objects
            stnode = checkCnGtoScalarize(*it,true);
            if (stnode != NULL) {
                if (stnode->nodeType == NT_OBJECT) {
                    if (lnoids == NULL) {
                        lnoids = new (eaMemManager) ObjIds(eaMemManager);
                    }
                    lnoids->push_back(stnode->opndId);
                } else {
                    if (lloids == NULL) {
                        lloids = new (eaMemManager) ObjIds(eaMemManager);
                    }
                    lloids->push_back(stnode->opndId);
                }
            }
            if (_scinfo) {
                os_sc << "- - - checkCnG returns "; 
                if (stnode == NULL) {
                    os_sc << " null "; 
                    os_sc << std::endl;
                } else {
		            printCnGNode(stnode,os_sc); os_sc << std::endl;
                }
            }
        }
    }
    if (prTitle)
        return;
    if (_scinfo) {
        os_sc << "CFGOpnds: " << irManager.getOpndManager().getNumSsaOpnds() << "  CnGNodes: "
            << cngNodes->size() << "  Local Objects: " << lo_count << std::endl;
    }
    if (lnoids != NULL || lloids != NULL) {
        if (_scinfo) {
            if (lnoids != NULL) {
                if (lnoids->size()>0) {
                    os_sc << "lnoids size: " << lnoids->size() << " - ";
                    for (lo_it=lnoids->begin(); lo_it!=lnoids->end(); lo_it++) {
                        os_sc << " " << (*lo_it);
                    }
                    os_sc << std::endl;
                }
            }
            if (lloids != NULL) {
                if (lloids->size()>0) {
                    os_sc << "lloids size: " << lloids->size() << " - ";
                    for (lo_it=lloids->begin(); lo_it!=lloids->end(); lo_it++) {
                        os_sc << " " << (*lo_it);
                    }
                    os_sc << std::endl;
                }
            }
        }
        methodEndInsts->clear();
        checkInsts->clear();
        checkOpndUsage(lnoids,lloids,true);
        bool doopt = false;
        if (lnoids != NULL) {
            if (lnoids->size()>0) {
                doopt = true;
                if (_scinfo) {
                    os_sc << "lnoids size: " << lnoids->size() << " - ";
                    for (lo_it=lnoids->begin(); lo_it!=lnoids->end(); lo_it++) {
                        os_sc << " " << (*lo_it);
                    }
                    os_sc << std::endl;
                }
            } else {
                if (_scinfo) {
                    os_sc << "lnoids size: 0" << std::endl;
                }
            }
        }
        if (lloids != NULL) {
            if (lloids->size()>0) {
                doopt = true;
                if (_scinfo) {
                    os_sc << "lloids size: " << lloids->size() << " - ";
                    for (lo_it=lloids->begin(); lo_it!=lloids->end(); lo_it++) {
                        os_sc << " " << (*lo_it);
                    }
                    os_sc << std::endl;
                }
            } else {
                if (_scinfo) {
                    os_sc << "lloids size: 0" << std::endl;
                }
            }
        }
        if (doopt && do_scalar_repl) {
            if (lnoids != NULL) {
                doLOScalarReplacement(lnoids);
            }
            if (lloids != NULL) {
                doLOScalarReplacement(lloids);
            }
        }    }
    if (_scinfo) {
        os_sc << "================                       > " ;
        irManager.getMethodDesc().printFullName(os_sc); 
        os_sc << std::endl;
    }
}  // scanLocalObjects() 


void
EscAnalyzer::scanEscapedObjects() {
    CnGNodes::iterator it;
    uint32 vco_count=0;            // number of local objects
    uint32 ob_ref_type=0;          // object ref type
    ObjIds* lnoids = NULL;         // list of CnG node Ids for new opnds to optimize
    ObjIds* lloids = NULL;         // list of CnG node Ids for load opnds to optimize
    ObjIds::iterator lo_it;
    bool prTitle = true;
    CnGNode* stnode = NULL;

    for (it = cngNodes->begin( ); it != cngNodes->end( ); it++ ) {
//        if ((*it)->nodeType == NT_OBJECT && getEscState(*it)!=NO_ESCAPE
        if ((*it)->nodeType == NT_OBJECT 
            && getOutEscaped(*it) == 0 && !((*it)->nInst->getOpcode()==Op_LdRef)) {
            if ((*it)->nInst->getNode() == NULL && getEscState(*it)==NO_ESCAPE) {
				continue;   // already scalarized
            }
            ob_ref_type = (*it)->nodeRefType;   // object ref type
            if (ob_ref_type != NR_REF) {
                continue;   // vc arrays not scalarized
            }
            vco_count++;  // number of vc objects
            if (prTitle) {
                if (_scinfo) {
                    os_sc << "================ Escaped Object States   < "; 
                    irManager.getMethodDesc().printFullName(os_sc); 
                    os_sc << std::endl;
                }
                prTitle = false;
            }

            stnode = checkCnGtoScalarize(*it,false);
            if (stnode != NULL) {
                if (stnode->nodeType == NT_OBJECT) {
                    if (lnoids == NULL) {
                        lnoids = new (eaMemManager) ObjIds(eaMemManager);
                    }
                    lnoids->push_back(stnode->opndId);
                } else {
                    if (lloids == NULL) {
                        lloids = new (eaMemManager) ObjIds(eaMemManager);
                    }
                    lloids->push_back(stnode->opndId);
                }
            }
            if (_scinfo) {
                os_sc << "- - - checkCnG returns "; 
                if (stnode == NULL) {
                    os_sc << " null "; 
                    os_sc << std::endl;
                } else {
		            printCnGNode(stnode,os_sc); os_sc << std::endl;
                }
            }

        }
    }

    if (prTitle)
        return;

    if (_scinfo) {
        os_sc << "CFGOpnds: " << irManager.getOpndManager().getNumSsaOpnds() << "  CnGNodes: "
            << cngNodes->size() << "  VC Objects: " << vco_count << "  lnoids size "
            << (lnoids!=NULL?lnoids->size():0) << "  lloids size " 
            << (lloids!=NULL?lloids->size():0)<< std::endl;
    }
    if (lnoids != NULL || lloids != NULL) {
        if (_scinfo) {
            if (lnoids != NULL) {
                if (lnoids->size()>0) {
                    os_sc << "lnoids size: " << lnoids->size() << " - ";
                    for (lo_it=lnoids->begin(); lo_it!=lnoids->end(); lo_it++) {
                        os_sc << " " << (*lo_it);
                    }
                    os_sc << std::endl;
                }
            }
             if (lloids != NULL) {
                if (lloids->size()>0) {
                    os_sc << "lloids size: " << lloids->size() << " - ";
                    for (lo_it=lloids->begin(); lo_it!=lloids->end(); lo_it++) {
                        os_sc << " " << (*lo_it);
                    }
                    os_sc << std::endl;
                }
            }
        }
        methodEndInsts->clear();
        checkInsts->clear();
        checkOpndUsage(lnoids,lloids,false);
        bool doopt = false;
        if (lnoids != NULL) {
            if (lnoids->size()>0) {
                doopt = true;
                if (_scinfo) {
                    os_sc << "lnoids size: " << lnoids->size() << " - ";
                    for (lo_it=lnoids->begin(); lo_it!=lnoids->end(); lo_it++) {
                        os_sc << " " << (*lo_it);
                    }
                    os_sc << std::endl;
                }
            }
        }
        if (lloids != NULL) {
            if (lloids->size()>0) {
                doopt = true;
                if (_scinfo) {
                    os_sc << "lloids size: " << lloids->size() << " - ";
                    for (lo_it=lloids->begin(); lo_it!=lloids->end(); lo_it++) {
                        os_sc << " " << (*lo_it);
                    }
                    os_sc << std::endl;
                }
            }
        }
        if (doopt && do_scalar_repl) {
            if (lnoids != NULL) {
                doEOScalarReplacement(lnoids);
            }
            if (lloids != NULL) {
                doEOScalarReplacement(lloids);
            }
        }
    }

    if (_scinfo) {
        os_sc << "================                       > " ;
        irManager.getMethodDesc().printFullName(os_sc); 
        os_sc << std::endl;
    }
}  // scanEscapedObjects() 


void 
EscAnalyzer::doLOScalarReplacement(ObjIds* loids) {
    ObjIds::iterator lo_it;
    CnGNode* onode;
    Inst* inst;
    Inst* st_inst;
    Insts::iterator it3;
    OpndManager& _opndManager = irManager.getOpndManager();
    InstFactory& _instFactory = irManager.getInstFactory();
    ScObjFlds* scObjFlds = new (eaMemManager) ScObjFlds(eaMemManager);
    ScObjFld* sco = NULL;
    ScObjFlds::iterator ito;

    if (loids == NULL)
        return;
    if (loids->size() == 0)       
        return;
    for (lo_it=loids->begin(); lo_it!=loids->end(); lo_it++) {
        onode = findCnGNode_op(*lo_it);
        if (onode == NULL) {
            if (_scinfo) {
                os_sc << " - - no cng node for opId " << *lo_it << std::endl;
            }
            continue;
        }
        if (onode->nodeRefType == NR_PRIM) {
            continue;
        }
        if (_scinfo) {
            os_sc << " - - method: ";
            irManager.getMethodDesc().printFullName(os_sc); 
            os_sc << std::endl;
            if (onode->nodeRefType == NR_REF)
                os_sc << " - - scalarized local instance ";
            else
                os_sc << " - - scalarized local array ";
            os_sc << "  "; printCnGNode(onode,os_sc);
            ((Opnd*)onode->refObj)->printWithType(os_sc);
            os_sc << std::endl;
        }

        // to collect stind & ldind instructions
        scObjFlds->clear();
        collectStLdInsts(onode, scObjFlds);

        if (_scinfo) {
            os_sc << " doLOSR: found object fields " << scObjFlds->size() << std::endl;
        }

        if (onode->nodeType == NT_LDOBJ) {
            if (_scinfo) {
                os_sc << "*-*-*- not optimized " << std::endl;
            }
            continue;
        }

        if (scObjFlds->size() > 0) {
            for (ito = scObjFlds->begin( ); ito != scObjFlds->end( ); ito++ ){
                sco = (*ito);
                if (sco->ls_insts->size()==0) {
                    continue;
                }
                bool do_fld_sc = false;
                for (it3 = sco->ls_insts->begin(); it3 != sco->ls_insts->end( ); it3++ ) {
                    if ((*it3)->getOpcode() == Op_TauLdInd) {
                        do_fld_sc = true;
                        break;
                    }
                }
                if (do_fld_sc) {
                    Type* fl_type = NULL;
                    Type* fl_type1 = NULL;
                    Inst* ii = sco->ls_insts->front();
                    if (ii->getOpcode()==Op_TauStInd) {
                        fl_type1 = ii->getSrc(1)->getType()->asPtrType()->getPointedToType();
                        fl_type = ii->getSrc(0)->getType();
                    } else {
                        fl_type1 = ii->getSrc(0)->getType()->asPtrType()->getPointedToType();
                        fl_type = ii->getDst()->getType();
                    }
                    VarOpnd* fl_var_opnd = _opndManager.createVarOpnd(fl_type, false);
                    SsaTmpOpnd* fl_init_opnd = _opndManager.createSsaTmpOpnd(fl_type);
                    Inst* ld_init_val_inst = NULL;
                    sco->fldVarOpnd = fl_var_opnd;

                    if (_scinfo) {
                        os_sc<<" PointedType "; fl_type1->print(os_sc); os_sc <<std::endl;
                        os_sc<<" OperandType "; fl_type->print(os_sc); os_sc <<std::endl;
                    }
                    if (fl_type->isReference()) {
                        ld_init_val_inst = _instFactory.makeLdNull(fl_init_opnd);
                    } else {
                        ld_init_val_inst = _instFactory.makeLdConst(fl_init_opnd, 0);
                    }

                    scalarizeOFldUsage(sco);
                    if (_scinfo) {
                        os_sc << "++++ old newobj added fld_var: before"  << std::endl;
                        FlowGraph::print(os_sc,onode->nInst->getNode());
                        os_sc << "++++ old newobj: before end"  << std::endl;
                    }
                    ld_init_val_inst->insertBefore(onode->nInst);
                    _instFactory.makeStVar(fl_var_opnd,fl_init_opnd)->insertBefore(onode->nInst);
                    if (_scinfo) {
                        os_sc << "++++ old newobj added fld_var: after"  << std::endl;
                        FlowGraph::print(os_sc,onode->nInst->getNode());
                        os_sc << "++++ old newobj: after end"  << std::endl;
                    }        
                } else { // there was no ldind instructions
                    for (it3 = sco->ls_insts->begin(); it3 != sco->ls_insts->end( ); it3++ ) {
                        st_inst = *it3;
                        removeInst(st_inst);
                        removeInst(inst=st_inst->getSrc(1)->getInst());
                        removeInst(inst=inst->getSrc(0)->getInst());
                        if (inst->getOpcode() == Op_LdArrayBaseAddr) {
                            removeInst(inst->getSrc(0)->getInst()); 
                        }
                    }
                }
            }
        }
        Node* no_node = onode->nInst->getNode();
        if (no_node != NULL) {
            if (_scinfo) {
                os_sc << "++++ old newobj removed: before"  << std::endl;
                FlowGraph::print(os_sc,no_node);
                os_sc << "++++ old newobj: after end"  << std::endl;
            }        
            removeInst(onode->nInst);
            if (_scinfo) {
                os_sc << "++++ old newobj removed: after"  << std::endl;
                FlowGraph::print(os_sc,no_node);
                os_sc << "++++ old newobj: after end"  << std::endl;
            }        
        }
    }
} // doLOScalarReplacement(ObjIds* loids)


void 
EscAnalyzer::doEOScalarReplacement(ObjIds* loids) {
    ObjIds::iterator lo_it;
    CnGNode* onode;
    Inst* inst;
    Insts::iterator it3;
    OpndManager& _opndManager = irManager.getOpndManager();
    InstFactory& _instFactory = irManager.getInstFactory();
    Insts* vc_insts = new (eaMemManager) Insts(eaMemManager); 
    ObjIds* vcids = new (eaMemManager) ObjIds(eaMemManager);
    double entryNode_execCount = irManager.getFlowGraph().getEntryNode()->getExecCount(); 
    ScObjFlds* scObjFlds = new (eaMemManager) ScObjFlds(eaMemManager);
    ScObjFlds* nscObjFlds = NULL;
    bool lobj_opt = false;
    CnGNode* lonode = NULL;
    CnGNode* nonode = NULL;

    if (loids == NULL)
        return;
    if (loids->size() == 0)       
        return;
    for (lo_it=loids->begin(); lo_it!=loids->end(); lo_it++) {
        onode = findCnGNode_op(*lo_it);
        if (onode == NULL) {
            if (_scinfo) {
                os_sc << " - - no cng node for opId " << *lo_it << std::endl;
            }
            continue;
        }
        if (onode->nodeRefType == NR_PRIM) {
            continue;
        }
        if (onode->nodeType == NT_OBJECT) {
            lobj_opt = false;
        } else {
            lobj_opt = true;
        }
        double path_prob = -1;
        path_prob = checkLocalPath(onode->nInst);
        if (_scinfo) {
            os_sc<<"pp      " << (path_prob )<<std::endl;
            os_sc<<"en*m    " << (entryNode_execCount*ec_mult)<<std::endl;
            os_sc<<"pp<en*m " << (path_prob < entryNode_execCount*ec_mult)<<std::endl;
            os_sc<<"pp==0   " << (path_prob==0)<<std::endl;
            os_sc<<"*       " << (path_prob < entryNode_execCount*ec_mult || path_prob==0)<<std::endl;
        }
        if (path_prob < entryNode_execCount*ec_mult || path_prob==0) {
            if (do_scalar_repl_final_fields) {
                checkToScalarizeFinalFiels(onode, scObjFlds);
            }
            continue;
        }
        if (_scinfo) {
            os_sc << " - - method: ";
            irManager.getMethodDesc().printFullName(os_sc); 
            os_sc << std::endl;
            if (onode->nodeRefType == NR_REF)
                os_sc << " - - scalarized escaped instance ";
            else
                os_sc << " - - scalarized escaped array ";
            os_sc << "  "; printCnGNode(onode,os_sc);
            ((Opnd*)onode->refObj)->printWithType(os_sc);
            os_sc << std::endl;
        }
        if (onode->nodeRefType != NR_REF) {
            return;
        }
        lonode = NULL;
        nonode = NULL;
        if (lobj_opt) {
#ifdef _DEBUG
            if (_scinfo) {
                printOriginObjects(onode->nInst,false);
            }
#endif
            if (onode->nInst->getOpcode() != Op_LdVar) {
                if (_scinfo) {
                    os_sc << "  doEO 1 "; onode->nInst->print(os_sc);
                    os_sc << std::endl;
                }
                continue;
            }
            Inst* phi = onode->nInst->getSrc(0)->getInst();
            if (phi->getOpcode() != Op_Phi) {
                if (_scinfo) {
                    os_sc << "  doEO 2 "; phi->print(os_sc);
                    os_sc << std::endl;
                }
                continue;
            }
            uint32 nsrc = phi->getNumSrcOperands();
            if (nsrc > 2) {
                if (_scinfo) {
                    os_sc << "  doEO 3" << std::endl;
                }
                continue;
            }
            for (uint32 i=0; i<nsrc; i++) {
                inst = phi->getSrc(i)->getInst();
                if (_scinfo) {
                    os_sc << "  doEO phi "; inst->print(os_sc);
                    os_sc << std::endl;
                }
                if (inst->getOpcode() != Op_StVar) {
                    break;
                }
                inst = inst->getSrc(0)->getInst();
                if (_scinfo) {
                    os_sc << "  doEO stvar "; inst->print(os_sc);
                    os_sc << std::endl;
                }
                if (inst->getOpcode() == Op_NewObj && nonode == NULL) {
                    nonode = findCnGNode_op(inst->getDst()->getId());
                    continue;
                }
                if ((inst->getOpcode() == Op_LdVar || inst->getOpcode() == Op_TauLdInd) 
                    && lonode == NULL) {
                    lonode = findCnGNode_op(inst->getDst()->getId());
                    continue;
                }
            }
            if (nonode == NULL || lonode == NULL) {
                if (_scinfo) {
                    os_sc << "  doEO 4" << std::endl;
                    if (nonode == NULL) {
                        os_sc << "  nonode NULL" << std::endl;
                    }
                    if (lonode == NULL) {
                        os_sc << "  lonode NULL" << std::endl;
                    }
                }
                continue;
            }
            if (_scinfo) {
                os_sc << "  no_src "; printCnGNode(nonode,os_sc); os_sc << std::endl;
                os_sc << "  lo_src "; printCnGNode(lonode,os_sc); os_sc << std::endl;
            }
        } else {
            nonode = onode;
        }

        // to collect stind & ldind instructions
        scObjFlds->clear();
        collectStLdInsts(onode, scObjFlds);

        if (lobj_opt) {
            collectCallInsts(nonode->cngNodeId, vc_insts, vcids);
            if (vc_insts->size() > 0 ) {
                if (_scinfo) {
                    os_sc << "--- no opt: newobj escaped  " << std::endl;
                    for (it3=vc_insts->begin(); it3!=vc_insts->end(); it3++) {
                        (*it3)->print(os_sc); os_sc << std::endl;
                    }
                }
                continue;
            }
            // to collect stind & ldind instructions for newobj
            if (nscObjFlds == NULL) {
                nscObjFlds = new (eaMemManager) ScObjFlds(eaMemManager);
            } else {
                nscObjFlds->clear();
            }
            collectStLdInsts(nonode, nscObjFlds);
            if (_scinfo) {
                os_sc << " doEOSR: found object fields for newobj " << nscObjFlds->size() << std::endl;
            }
            if (!checkObjFlds(nscObjFlds, scObjFlds)) {
                if (_scinfo) {
                    os_sc << " checkObjFlds failed " << std::endl;
                }
                continue;
            }
                if (_scinfo) {
                    os_sc << " checkObjFlds passed " << std::endl;
                }
        }

        // to collect call & callimem instructions
        collectCallInsts(onode->cngNodeId, vc_insts, vcids);

        if (_scinfo) {
            os_sc << " doEOSR: found object fields " << scObjFlds->size() << " vc_inst " << vc_insts->size()  
                << std::endl;
        }
        if (_scinfo) {
            for (it3=vc_insts->begin(); it3!=vc_insts->end(); it3++) {
                (*it3)->print(os_sc); os_sc << std::endl;
            }
        }

        if (lobj_opt) {
            if (checkInsts != NULL) {
                fixCheckInsts(onode->opndId);
            }
        }
        TypeManager& _typeManager  = irManager.getTypeManager();
        Inst* nobj_inst = nonode->nInst;  // optimized newobj inst for ldvar opnd
        Inst* lobj_inst = NULL;           // load opnd inst for ldvar opnd
        VarOpnd* ob_var_opnd = _opndManager.createVarOpnd(nobj_inst->getDst()->getType(), false);
        SsaTmpOpnd* ob_init_opnd = _opndManager.createSsaTmpOpnd(ob_var_opnd->getType());
        uint32 ob_id = onode->opndId;
        Insts::iterator itvc;
        ScObjFlds::iterator ito;
        Node* node_no = nobj_inst->getNode();
        ScObjFld* sco = NULL;

        if (lobj_opt) {
            lobj_inst = lonode->nInst;
        }
        if (scObjFlds->size() > 0) {
            for (ito = scObjFlds->begin( ); ito != scObjFlds->end( ); ito++ ){
                sco = (*ito);
                if (sco->ls_insts->size()==0)
                    continue;
                Type* fl_type = NULL;
                Type* fl_type1 = NULL;
                Inst* ii = sco->ls_insts->front();
                Inst* iadr = NULL;
                if (ii->getOpcode()==Op_TauStInd) {
                    iadr=ii->getSrc(1)->getInst();
                    fl_type1 = ii->getSrc(1)->getType()->asPtrType()->getPointedToType();
                    fl_type = ii->getSrc(0)->getType();
                } else {
                    iadr=ii->getSrc(0)->getInst();
                    fl_type1 = ii->getSrc(0)->getType()->asPtrType()->getPointedToType();
                    fl_type = ii->getDst()->getType();
                }
                VarOpnd* fl_var_opnd = _opndManager.createVarOpnd(fl_type, false);
                SsaTmpOpnd* fl_init_opnd = _opndManager.createSsaTmpOpnd(fl_type);
                Inst* ld_init_val_inst = NULL;
                sco->fldVarOpnd = fl_var_opnd;

                if (_scinfo) {
                    os_sc<<" PoitedType "; fl_type1->print(os_sc); os_sc <<std::endl;
                    os_sc<<" OperandType "; fl_type->print(os_sc); os_sc <<std::endl;
                }
                if (fl_type->isReference()) {
                    ld_init_val_inst = _instFactory.makeLdNull(fl_init_opnd);
                } else {
                    ld_init_val_inst = _instFactory.makeLdConst(fl_init_opnd, 0);
                }
                scalarizeOFldUsage(sco);
                if (_scinfo) {
                    os_sc << "++++ old newobj added fld_var: before"  << std::endl;
                    FlowGraph::print(os_sc,node_no);
                    os_sc << "++++ old newobj: before end"  << std::endl;
                }
                ld_init_val_inst->insertBefore(nobj_inst);
                _instFactory.makeStVar(fl_var_opnd,fl_init_opnd)->insertBefore(nobj_inst);
                if (_scinfo) {
                    os_sc << "++++ old newobj added fld_var: after"  << std::endl;
                    FlowGraph::print(os_sc,node_no);
                    os_sc << "++++ old newobj: after end"  << std::endl;
                }
                if (lobj_opt) {
                    //
                    bool comprRefs = compressedReferencesArg
                        || (VMInterface::areReferencesCompressed());
                    Modifier mod1 = comprRefs ? AutoCompress_Yes : AutoCompress_No;
                    Opnd* ld_tau_op = _opndManager.createSsaTmpOpnd(_typeManager.getTauType());
                    Inst* itau = _instFactory.makeTauUnsafe(ld_tau_op);
                    itau->insertAfter(lobj_inst);
                    FieldDesc* fd = iadr->asFieldAccessInst()->getFieldDesc();
                    Opnd* dst_ld = _opndManager.createSsaTmpOpnd(iadr->getDst()->getType());
                    Opnd* ob_opnd = (Opnd*)(lonode->refObj);
                    Inst* lda = _instFactory.makeLdFieldAddr(dst_ld,ob_opnd,fd);
                    lda->insertAfter(itau);
                    SsaTmpOpnd* fl_tmp_opnd_ld = _opndManager.createSsaTmpOpnd(fl_type);
                    Inst* ldf = _instFactory.makeTauLdInd(mod1,fl_var_opnd->getType()->tag,
                        fl_tmp_opnd_ld,dst_ld,ld_tau_op,ld_tau_op);
                    ldf->insertAfter(lda);
                    Inst* stv = _instFactory.makeStVar(fl_var_opnd,fl_tmp_opnd_ld);
                    stv->insertAfter(ldf);
                }
            }
        }
        if ((nscObjFlds != NULL) && (nscObjFlds->size() > 0)) {
            for (ito = nscObjFlds->begin( ); ito != nscObjFlds->end( ); ito++ ) {
                sco = (*ito);
                if (sco->ls_insts->size()==0) {
                    continue;
                }
                ScObjFlds::iterator it2;
                for (it2=scObjFlds->begin(); it2!=scObjFlds->end(); it2++) {
                    if (sco->fd == (*it2)->fd) {
                        sco->fldVarOpnd = (*it2)->fldVarOpnd;
                        break;
                     }
                }
                assert(sco->fldVarOpnd!=NULL);
                scalarizeOFldUsage(sco);
            }
        }
        restoreEOCreation(vc_insts, scObjFlds, ob_var_opnd, ob_id);
        if (_scinfo) {
            os_sc << "++++ old newobj: before"  << std::endl;
            FlowGraph::print(os_sc,node_no);
            os_sc << "++++ old newobj: before end"  << std::endl;
            if (lobj_opt) {
                os_sc << "++++ old ldobj: before"  << std::endl;
                FlowGraph::print(os_sc,lonode->nInst->getNode());
                os_sc << "++++ old ldobj: before end"  << std::endl;
            }
        }
        _instFactory.makeLdNull(ob_init_opnd)->insertBefore(nobj_inst);
        _instFactory.makeStVar(ob_var_opnd,ob_init_opnd)->insertBefore(nobj_inst);
        if (lobj_opt) {
            _instFactory.makeStVar(ob_var_opnd,(Opnd*)(lonode->refObj))->insertAfter(lonode->nInst);
        }
        if (methodEndInsts->size()!=0)
            fixMethodEndInsts(ob_id);
        removeInst(nobj_inst);
        if (_scinfo) {
            os_sc << "++++ old newobj: after"  << std::endl;
            FlowGraph::print(os_sc,node_no);
            os_sc << "++++ old newobj: after end"  << std::endl;
            if (lobj_opt) {
                os_sc << "++++ old ldobj:after"  << std::endl;
                FlowGraph::print(os_sc,lonode->nInst->getNode());
                os_sc << "++++ old ldobj: after end"  << std::endl;
            }
        }

        if (lobj_opt) {  // remove ldvar, phi, stvar
            Inst* phi = onode->nInst->getSrc(0)->getInst();
            if (phi->getOpcode() != Op_Phi) {
                assert(0);
            }
            uint32 nsrc = phi->getNumSrcOperands();

            for (uint32 i=0; i<nsrc; i++) {
                inst = phi->getSrc(i)->getInst();
                removeInst(inst);
            }
            removeInst(phi);
            removeInst(onode->nInst);
        }

        scObjFlds->clear();
        if (nscObjFlds != NULL) {
            nscObjFlds->clear();
        }
    }

} // doEOScalarReplacement(ObjIds* loids)


void 
EscAnalyzer::collectStLdInsts(CnGNode* onode, ScObjFlds* scObjFlds) {
    ScObjFld* scObjFld = NULL;
    CnGRefs::iterator it1;
    CnGRefs::iterator it2;
    Inst* inst;
    CnGNode* fnode;

    if (onode->outEdges != NULL) {
        for (it1 = onode->outEdges->begin(); it1 != onode->outEdges->end(); it1++) {
            if ((*it1)->edgeType == ET_FIELD) {
                scObjFld = new (eaMemManager) ScObjFld;
                scObjFld->fldVarOpnd=NULL;
                scObjFld->isFinalFld=false;
                Insts* fl_insts = new (eaMemManager) Insts(eaMemManager);
                fnode =(*it1)->cngNodeTo;
                if (fnode->outEdges == NULL) {
                    if (_scinfo) {
                        os_sc << "collectStLdInsts: no ref from fld" << std::endl;
                        printCnGNode(onode, os_sc); os_sc << std::endl;
                        printCnGNode(fnode, os_sc); os_sc << std::endl;
                    }
                    continue;
                }
                for (it2 = fnode->outEdges->begin(); it2 != fnode->outEdges->end(); it2++) {
                    inst = (*it2)->edgeInst;
                    if (_scinfo) {
                        os_sc << "- - - to remove: ";
                        FlowGraph::printLabel(os_sc,inst->getNode()); os_sc<<" ";
                        inst->print(os_sc); os_sc<< std::endl;
                    }
                    fl_insts->push_back(inst);
                    if (_scinfo) {
                        if (inst->getOpcode() == Op_TauStInd) {
                            inst=inst->getSrc(1)->getInst();
                        }
                        if (inst->getOpcode() == Op_TauLdInd) {
                            inst=inst->getSrc(0)->getInst();
                        }
                        os_sc << "                 ";
                        FlowGraph::printLabel(os_sc,inst->getNode()); os_sc<<" ";
                        inst->print(os_sc); os_sc << std::endl;
                        os_sc << "                 ";
                        FlowGraph::printLabel(os_sc,inst->getSrc(0)->getInst()->getNode()); os_sc<<" ";
                        (inst->getSrc(0)->getInst())->print(os_sc); os_sc << std::endl;
                    }
                }
                if (fnode->nInst->getOpcode()==Op_LdFieldAddr) { 
                    FieldDesc* fdesc = fnode->nInst->asFieldAccessInst()->getFieldDesc();
                    scObjFld->fd = fdesc;
                    if (fdesc->isInitOnly()) {
                        scObjFld->isFinalFld=true;
                    }
                }
                scObjFld->ls_insts = fl_insts;
                scObjFlds->push_back(scObjFld);
            } else {
                os_sc << " --- col error: not a field "; (*it1)->edgeInst->print(os_sc);
                os_sc << std::endl;
            }
        }
    } else {
        if (_scinfo) {
            os_sc << "- - - to remove 2: ";
            onode->nInst->print(os_sc);
            os_sc << std::endl;
        }
    }
} // collectStLdInsts(CnGNode* onode, ScObjFld* scObjFlds)


void 
EscAnalyzer::collectCallInsts(uint32 n, Insts* vc_insts, ObjIds* vcids) {
    CnGEdges::iterator ite;
    CnGRefs::iterator it2;

    vc_insts->clear();
    vcids->clear();
    for (ite = cngEdges->begin( ); ite != cngEdges->end( ); ite++ ) {
        for (it2 = (*ite)->refList->begin( ); it2 != (*ite)->refList->end( ); it2++ ) {
            if ((*it2)->cngNodeTo->cngNodeId == n) {
                Inst* ii=(*ite)->cngNodeFrom->nInst;
                uint32 opc = ii->getOpcode();
                if (opc == Op_IndirectMemoryCall || opc == Op_DirectCall) {
                    if (!checkScanned(vcids,ii->getId())) {
                        vc_insts->push_back(ii);
                        vcids->push_back(ii->getId());
                    }
                } 
            }
        }
    }
} // collectCallInsts(CnGNode* onode, ScObjFld* scObjFlds)


void 
EscAnalyzer::scalarizeOFldUsage(ScObjFld* scfld) {
    Insts* sl_insts = scfld->ls_insts;
    VarOpnd* fl_var_opnd = scfld->fldVarOpnd;
    uint32 nsrco = 0;
    Inst* inst_ad;
    Inst* st_ld_var_inst;
    Inst* st_ld_inst;
    InstFactory& _instFactory = irManager.getInstFactory();
    Node* node_before = NULL;
    Insts::const_iterator it;

    for (it = sl_insts->begin( ); it != sl_insts->end( ); it++ ) {
        st_ld_inst = *it;
        node_before = st_ld_inst->getNode();
        if (node_before==NULL) {
            os_sc << "node_before NULL "; st_ld_inst->print(os_sc); os_sc << std::endl;
        }
        if (_scinfo) {
            os_sc << "++++ scalarizeOFldUsage: before"  << std::endl;
            FlowGraph::print(os_sc,node_before);
            os_sc << "++++ scalarizeOFldUsage: before end"  << std::endl;
        }
        if (st_ld_inst->getOpcode() == Op_TauStInd)
            nsrco = 1;
        else
            nsrco = 0;
        inst_ad = st_ld_inst->getSrc(nsrco)->getInst();    // receives address to load/store
        if (nsrco == 0) {
            st_ld_var_inst = _instFactory.makeLdVar(st_ld_inst->getDst(), fl_var_opnd);
        } else {
            st_ld_var_inst = _instFactory.makeStVar(fl_var_opnd, st_ld_inst->getSrc(0));
        }
        st_ld_var_inst->insertAfter(st_ld_inst);
        removeInst(st_ld_inst);
        removeInst(inst_ad);
        if (inst_ad->getOpcode()==Op_AddScaledIndex) {
            removeInst(inst_ad->getSrc(0)->getInst());
        }
        if (_scinfo) {
            os_sc << "++++ scalarizeOFldUsage: after"  << std::endl;
            FlowGraph::print(os_sc,node_before);

            os_sc << "++++ scalarizeOFldUsage: after end"  << std::endl;
        }
    }
} // scalarizeOFldUsage(ScObjFld* scfld)


void 
EscAnalyzer::checkOpndUsage(ObjIds* lnoids, ObjIds* lloids, bool check_loc) {
    const Nodes& nodes = irManager.getFlowGraph().getNodes();
    Nodes::const_iterator niter;
    bool do_break = false;

    if (_scinfo) {
        os_sc << "  -------- Objects used in:  check_loc " << check_loc << std::endl;
    }
    for(niter = nodes.begin(); niter != nodes.end(); ++niter) {
        Node* node = *niter;
        Inst *headInst = (Inst*)node->getFirstInst();
        Opnd* opnd;
        for (Inst* inst=headInst->getNextInst();inst!=NULL;inst=inst->getNextInst()) {
            uint32 nsrc = inst->getNumSrcOperands();
            for (uint32 i=0; i<nsrc; i++) {
                if (!(opnd=inst->getSrc(i))->isSsaOpnd()) { // check ssa operands
                    if (_scinfo) {
                        os_sc << "  not ssa opnd " << i << "  ";
                        inst->print(os_sc); os_sc << std::endl; 
                    }
                    continue;
                }
                uint32 opcode=inst->getOpcode();
                if (checkScanned(lnoids,opnd->getId())) {
                    if (_scinfo) {
                        os_sc << " no "; FlowGraph::printLabel(os_sc,inst->getNode()); 
                        os_sc << "  "; inst->print(os_sc); os_sc << std::endl; 
                    }
                    if (opcode == Op_MethodEnd) {
                        methodEndInsts->push_back(inst);
                        continue;
                    }
                    if (opcode == Op_LdFieldAddr || opcode == Op_LdArrayBaseAddr)
                        continue;
                    if (!check_loc)
                        if (opcode == Op_IndirectMemoryCall || opcode == Op_DirectCall)
                            continue;
                    if (_scinfo) {
                        os_sc << " no   remove " << opnd->getId() << std::endl; 
                    }
                    lnoids->remove(opnd->getId());
                }
                if (checkScanned(lloids,opnd->getId())) {
                    if (_scinfo) {
                        os_sc << " lo "; FlowGraph::printLabel(os_sc,inst->getNode()); 
                        os_sc << "  "; inst->print(os_sc); os_sc << std::endl; 
                    }
                    if (opcode == Op_Branch) {
                        checkInsts->push_back(inst);
                        continue;
                    }
                    if (opcode == Op_TauCheckNull || opcode == Op_TauIsNonNull || opcode == Op_TauHasType) {
                        if (checkTauOpnd(inst)) {
                            checkInsts->push_back(inst);
                            continue;
                        }
                    }
                    if (opcode == Op_MethodEnd) {
                        methodEndInsts->push_back(inst);
                        continue;
                    }
                    if (opcode == Op_LdFieldAddr || opcode == Op_LdArrayBaseAddr)
                        continue;
                    if (!check_loc) {
                        if (opcode == Op_IndirectMemoryCall || opcode == Op_DirectCall) {
                            continue;
                        }
                    }
                    if (_scinfo) {
                        os_sc << " lo   remove " << opnd->getId() << std::endl; 
                    }
                    lloids->remove(opnd->getId());
                }
                if ((lnoids == NULL || lnoids->size() == 0) 
                    && (lloids == NULL || lloids->size() == 0)) {
                    do_break = true;
                    break;
                }
            }
            if (do_break) {
                break;
            }
        }
    }
    if (_scinfo) {
        os_sc << "  -------- " << std::endl;
    }
    return;
} // checkOpndUsage(ObjIds* loids,bool check_loc,::std::ostream& os_sc)


bool 
EscAnalyzer::checkTauOpnd(Inst* tau_inst) {
    const Nodes& nodes = irManager.getFlowGraph().getNodes();
    Nodes::const_iterator niter;
    bool maydo = true;
    uint32 tau_opnd_id = tau_inst->getDst()->getId();
    uint32 sc_opnd_id = tau_inst->getSrc(0)->getId(); 
    Inst* inst1;

    if (_scinfo) {
        os_sc << "  -------- Check tau opnd " << std::endl;
    }
    for(niter = nodes.begin(); niter != nodes.end(); ++niter) {
        Node* node = *niter;
        Inst *headInst = (Inst*)node->getFirstInst();
        Opnd* opnd;
        for (Inst* inst=headInst->getNextInst();inst!=NULL;inst=inst->getNextInst()) {
            if (inst->getOpcode() == Op_InitObj) {
                if (_scinfo) {
                    os_sc << " Op_InitObj: ";
                    inst->print(os_sc); os_sc << std::endl;
                }
            }
            uint32 nsrc = inst->getNumSrcOperands();
            for (uint32 i=0; i<nsrc; i++) {
                if (!(opnd=inst->getSrc(i))->isSsaOpnd())  // check ssa operands
                    continue;
                if (opnd->getId()==tau_opnd_id) {
                    uint32 opcode=inst->getOpcode();
                    if (_scinfo) {
                        os_sc << "      "; FlowGraph::printLabel(os_sc,inst->getNode()); 
                        os_sc << "  "; inst->print(os_sc); os_sc << std::endl; 
                        if (opcode == Op_IndirectMemoryCall || opcode == Op_DirectCall) {
                            MethodDesc* md = getMD(inst);
                            if (md->isNative())
                                os_sc << "            native " << std::endl;
                            if (opcode==Op_IndirectMemoryCall) {
                                os_sc << "        ";md->printFullName(os_sc); os_sc << std::endl;
                            }
                        }
                    }
                    if (opcode == Op_IndirectMemoryCall || opcode == Op_DirectCall) {
                        for (uint32 i1=0; i1<nsrc; i1++) {
                            if (inst->getSrc(i)->getId() == sc_opnd_id)
                                break;
                            if (i1 == nsrc) {
                                maydo = false;
                            }
                        }
                    }
                    if (opcode == Op_TauLdInd || opcode == Op_TauStInd) {
                        if (opcode == Op_TauLdInd)
                            inst1 = inst->getSrc(0)->getInst();
                        else 
                            inst1 = inst->getSrc(1)->getInst();
                        if (inst1->getOpcode() != Op_LdFieldAddr)
                            maydo = false;
                        if (inst1->getSrc(0)->getId() != sc_opnd_id)
                            maydo = false;
                        if (_scinfo) {
                            os_sc << "      "; FlowGraph::printLabel(os_sc,inst1->getNode()); 
                            os_sc << "  "; inst1->print(os_sc); os_sc << std::endl; 
                        }
                    }
                    if (maydo == false) {
                        return maydo;
                    }
                }
            }
        }
    }
    return maydo;
} // checkTauOpnd(Inst* tau_inst)


bool 
EscAnalyzer::checkOpndUsage(uint32 lobjid) {
    const Nodes& nodes = irManager.getFlowGraph().getNodes();
    Nodes::const_iterator niter;
    bool maydo = true;
    uint32 n_used = 0;

    if (_scinfo) {
        os_sc << "  -------- Used in " << std::endl;
    }
    for(niter = nodes.begin(); niter != nodes.end(); ++niter) {
        Node* node = *niter;
        Inst *headInst = (Inst*)node->getFirstInst();
        Opnd* opnd;
        for (Inst* inst=headInst->getNextInst();inst!=NULL;inst=inst->getNextInst()) {
            if (inst->getOpcode() == Op_InitObj) {
                if (_scinfo) {
                    os_sc << " Op_InitObj: ";
                    inst->print(os_sc); os_sc << std::endl;
                }
            }
            uint32 nsrc = inst->getNumSrcOperands();
            for (uint32 i=0; i<nsrc; i++) {
                if (!(opnd=inst->getSrc(i))->isSsaOpnd())  // check ssa operands
                    continue;
                if (opnd->getId()==lobjid) {
                    n_used++;
                    uint32 opcode=inst->getOpcode();
                    if (_scinfo) {
                        os_sc << "    "; FlowGraph::printLabel(os_sc,inst->getNode()); 
                        os_sc << "  "; inst->print(os_sc); os_sc << std::endl; 
                        if (opcode == Op_IndirectMemoryCall || opcode == Op_DirectCall) {
                            MethodDesc* md = getMD(inst);
                            if (md->isNative())
                                os_sc << "            native " << std::endl;
                            if (opcode==Op_IndirectMemoryCall) {
                                os_sc << "        ";md->printFullName(os_sc); os_sc << std::endl;
                            }
                        }
                    }
                    if (opcode != Op_LdFieldAddr && opcode != Op_LdArrayBaseAddr && 
                        opcode != Op_IndirectMemoryCall && opcode != Op_DirectCall && 
                        opcode != Op_MethodEnd && opcode != Op_StVar) {
                        maydo = false;
                    }
                }
            }
        }
    }
    if (_scinfo) {
        os_sc << "  -------- opnd used: " << n_used << "  maydo " << maydo << std::endl;
    }
    return maydo;
} // checkOpndUsage(uint32 lobjid)


EscAnalyzer::CnGNode* 
EscAnalyzer::checkCnGtoScalarize(CnGNode* scnode, bool check_loc) {
    uint32 ob_ref_type=0;          // object ref type
    uint32 ob_fld_count=0;         // number of object references to fields
    uint32 ob_fin_fld_count=0;     // number of object references to final fields
    uint32 ob_nfld_count=0;        // number of object references to not a field
    uint32 ref_to_obj_count=0;     // number references to object
    uint32 vcall_count=0;          // number of virtual calls
    uint32 call_count=0;           // number of calls
    uint32 ob_fld_stind_count=0;   // number of stores in object field
    uint32 ob_fld_ldind_count=0;   // number of loads from object field
    uint32 ob_fld_field_count=0;   // number of fields of object field
    uint32 ob_fld_unkn_count=0;    // number of unknown references of object field
    uint32 ob_fld_usage_count=0;   // number of object field usage
    uint32 ob_stvar_count=0;       // number of object stvar
    bool storeId = false;
    CnGRefs::iterator it1;
    CnGRefs::iterator it2;
    CnGEdges::iterator ite;
    CnGNode* nf = NULL;
    CnGNode* vnode = NULL;
    CnGNode* rnode = scnode;
    uint32 scnode_type = scnode->nodeType;

    ob_ref_type = scnode->nodeRefType;   // object ref type
    if (!check_loc && (ob_ref_type != NR_REF))
        return NULL;   // vc arrays not scalarized
    if (_scinfo) {
        os_sc << "=="; printCnGNode(scnode,os_sc);
        ((Opnd*)scnode->refObj)->printWithType(os_sc);
        NamedType* nt = (NamedType*)((Opnd*)scnode->refObj)->getType();
        if (nt->isFinalizable()) {
            os_sc << " - finalizable ";
        }
        os_sc << std::endl;
        os_sc << "  =="; ((Opnd*)scnode->refObj)->getInst()->print(os_sc); 
        os_sc << std::endl;
        checkOpndUsage(scnode->opndId);
    }

    if (_scinfo) {
        os_sc << "  -------- begin - Ref from node  " << scnode->cngNodeId << " - " 
            << scnode->opndId << std::endl;
    }
    if (scnode->outEdges != NULL) {
        for (it1 = scnode->outEdges->begin(); it1 != scnode->outEdges->end(); it1++) {
            if ((*it1)->edgeType == ET_FIELD) {
                ob_fld_count++;  // number of object fields
                if (_scinfo) {
                    os_sc<<"++ "<<scnode->cngNodeId<< "  ->  " << 
                        (*it1)->cngNodeTo->cngNodeId << " - "<< 
                        (*it1)->cngNodeTo->opndId << std::endl;
                }
                nf = (*it1)->cngNodeTo;   // object field cng node
                if (nf->nInst->getOpcode()==Op_LdFieldAddr) { //  ???
                    FieldDesc* fdesc = nf->nInst->
                        asFieldAccessInst()->getFieldDesc();
                    if (fdesc->isInitOnly())
                        ob_fin_fld_count++;
                }
                if (nf->outEdges != NULL || 
                    (*it1)->cngNodeTo->nInst->getOpcode() != Op_LdArrayBaseAddr) {
                    ob_fld_usage_count++;
                    if (_scinfo) {
                        os_sc << "  fld-" << std::endl << "    ";
                        printCnGNode(nf,os_sc); os_sc << std::endl << "    ";
                        (*it1)->cngNodeTo->nInst->print(os_sc); os_sc << std::endl;
                    }
                }
                if (nf->outEdges != NULL) {
                    for (it2 = nf->outEdges->begin( ); it2 != nf->outEdges->end( ); it2++ ) {
                        if (_scinfo) {
                            os_sc << "    " << edgeTypeToString(*it2) << std::endl;
                        }
                        switch ((*it2)->edgeType) {
                            case ET_POINT: ob_fld_ldind_count++; break;
                            case ET_DEFER: ob_fld_stind_count++; break;
                            case ET_FIELD: ob_fld_field_count++; break;
                            default: ob_fld_unkn_count++;
                        }
                        if (_scinfo) {
                            CnGNode* node = NULL;
                            if ((node=findCnGNode_id((*it2)->cngNodeTo->cngNodeId))!=NULL) {
                                os_sc << "      ";
                                printCnGNode(node,os_sc); os_sc << std::endl << "      ";
                                node->nInst->print(os_sc); os_sc << std::endl;
                                os_sc << "        ";
                                (*it2)->edgeInst->print(os_sc);
                                os_sc << std::endl;
                            }
                        }
                    } 
                }
            } else {
                if ((*it1)->cngNodeTo->nInst->getOpcode() == Op_StVar) {
                    ob_stvar_count++;
                    if (ob_stvar_count == 1) {
                        vnode =(*it1)->cngNodeTo;
                    }
                } else {
                    ob_nfld_count++;
                }
                if (_scinfo) {
                    os_sc << "  not_fld  " << edgeTypeToString(*it1) << "  ";
                    (*it1)->cngNodeTo->nInst->print(os_sc); os_sc << std::endl;
                }
            }
        }
    }
    if (_scinfo) {
        os_sc << "  -------- end - Ref from node  " << std::endl;
    }
    uint32 n=scnode->cngNodeId;

    if (_scinfo) {
        os_sc << "  -------- begin - Ref to node  " << scnode->cngNodeId << " - " 
            << scnode->opndId << std::endl;
    }
    for (ite = cngEdges->begin( ); ite != cngEdges->end( ); ite++ ) {
        for (it2 = (*ite)->refList->begin( ); it2 != (*ite)->refList->end( ); it2++ ) {
            if ((*it2)->cngNodeTo->cngNodeId == n) {
                Inst* ii=(*ite)->cngNodeFrom->nInst;
                uint32 opc = ii->getOpcode();
                if (opc == Op_IndirectMemoryCall || opc == Op_DirectCall) {
                    if (opc == Op_IndirectMemoryCall)
                        vcall_count++;
                    else 
                        call_count++;
                } else
                    ref_to_obj_count++;  // number of ref to object
                if (_scinfo) {
                    os_sc << "  Ref to object. Edge: " << (*ite)->cngNodeFrom->cngNodeId 
                        << " - "  << (*ite)->cngNodeFrom->opndId << "  ->  " 
                        << (*it2)->cngNodeTo->cngNodeId 
                        << " - " << (*it2)->cngNodeTo->opndId << std::endl << "    ";
                    printCnGNode((*ite)->cngNodeFrom,os_sc);
                    os_sc << std::endl << "    ";
                    ii->print(os_sc); os_sc << std::endl;
                    if (ii->getOpcode()==Op_IndirectMemoryCall) {
                        MethodDesc* md = getMD(ii);
                        os_sc<<"    ";md->printFullName(os_sc);os_sc << std::endl;
                    }
                }
            }
        }
    }
    if (_scinfo) {
        os_sc << "  -------- end - Ref to node  " << std::endl;
    }

    if (ob_nfld_count == 0 && 
        ( (scnode_type == NT_OBJECT && ref_to_obj_count == 0) 
            || (scnode_type == NT_LDOBJ && ref_to_obj_count == 1 && ob_stvar_count == 0) ) ) {
        if (scnode_type == NT_LDOBJ) {
            if (ob_fin_fld_count==ob_fld_usage_count) {
                if ( (check_loc && (getEscState(scnode)==NO_ESCAPE)) ||
                    (!check_loc && (getEscState(scnode)!=NO_ESCAPE)) ) {
                    storeId = true;
                } else {
                    if (_scinfo) {
                        os_sc << "----scalar no opt:     check_loc " << check_loc 
                            << "  state " << getEscState(scnode) << std::endl;
                    }
                }
            } else {
                if (_scinfo) {
                    os_sc << "----scalar no opt: LOBJ  final fields " << ob_fin_fld_count 
                        << "  used field " << ob_fld_usage_count << std::endl;
                }
            }
            goto MM;
        }
        if (ob_ref_type == NR_REF) {   // NT_OBJECT - object
            if (ob_stvar_count == 1) {     // find ldvar target operand
                if (ob_fin_fld_count!=ob_fld_usage_count) {
                    if (_scinfo) {
                        os_sc << "----scalar no opt:  stvar=1  final fields " << ob_fin_fld_count 
                            << "  used field " << ob_fld_usage_count << std::endl;
                    }
                    goto MM;
                }

                if (_scinfo) {
                    os_sc << "************* " << ((Opnd*)scnode->refObj)->getType()->getName() 
                        << std::endl;
                }
                assert(vnode!=NULL);
                CnGNode* lobj = getLObj(vnode);  // scalarizable load object
                if (lobj != NULL) {
                    if (_scinfo) {
                        printOriginObjects(lobj->nInst,false,"  ");
                    }
                    bool notnullsrcs = checkVVarSrcs(lobj->nInst);
                    if (_scinfo) {
                        os_sc << "----checkCnG: stvar=1 checkVVSrcs   " << notnullsrcs << std::endl;
                    }
                    if (!notnullsrcs) {
                        goto MM;
                    }
                    if (_scinfo) {
                        os_sc << "----check srcs opernds for optimized load object (cng node): " 
                            << lobj->cngNodeId << std::endl;
                    }
                    rnode = checkCnGtoScalarize(lobj,check_loc);
                    if (rnode!=NULL) {
                        storeId =true;
                    }
                }
                goto MM;
            }
            // to optimize newobj target operand
            if (check_loc) { //local object
                assert(vcall_count==0 && call_count==0);
                storeId = true;
                goto MM;
            }
            // to optimize escaped object
            if (do_scalar_repl_only_final_fields_in_use) {
                if (ob_fin_fld_count==ob_fld_usage_count) {
                    storeId = true;
                } else {
                    if (_scinfo) {
                        os_sc << "----scalar no opt: onlyFF  final fields " << ob_fin_fld_count 
                            << "  used field " << ob_fld_usage_count << std::endl;
                    }
                }
            } else {
                if (ob_fin_fld_count==ob_fld_usage_count) { // objects with only final fields
                    storeId = true;
                } else {
                    // not global escaped objects may be scalarized
                    if (getEscState(scnode)!=GLOBAL_ESCAPE && getVirtualCall(scnode)==0) {
                        storeId = true;
                    } else {
                        if (_scinfo) {
                            os_sc << "----scalar no opt: GLOBAL,  fin.flds " << ob_fin_fld_count 
                                << "  used flds " << ob_fld_usage_count << std::endl;
                        }
                    }
                }
            }
            goto MM;
        }
        // NT_OBJECT - array
        if (ob_fld_usage_count <= 1 && check_loc && ob_stvar_count == 0) {
            storeId = true;
        } else {
            if (_scinfo) {
                os_sc << "----scalar no opt: arr:  fld " << ob_fld_count 
                    << "  used flds " << ob_fld_usage_count << std::endl; 
            }
        }
    } else {
        if (_scinfo) {
            os_sc << "----scalar no opt:     ref to " << ref_to_obj_count << "  not field " 
                << ob_nfld_count << "  obj stvar " << ob_stvar_count << std::endl;
        }
    }
MM:
    if (_scinfo) {
        os_sc << "----checkCnG: ntype " << scnode->nodeType   
            << "    flds " << ob_fld_count 
            << "  not flds " << ob_nfld_count
            << "  final flds " << ob_fin_fld_count
            << "  used flds " << ob_fld_usage_count
            << "  vcalls " << vcall_count 
            << "  calls " << call_count 
            << "  stored " << ob_fld_stind_count 
            << "  loaded " << ob_fld_ldind_count 
            << "  others " << ob_fld_field_count 
            << "  ref to " << ref_to_obj_count 
            << "  stvar " << ob_stvar_count 
            << "  unkn.refs " << ob_fld_unkn_count <<  std::endl;
    }
    if (storeId) {
        if (_scinfo) {
            os_sc << "----scalar may be opt:  "; printCnGNode(scnode,os_sc);   
            os_sc <<  std::endl;
        }
        return rnode;
    }
    if (_scinfo) {
        os_sc << "----scalar no opt:  "; printCnGNode(scnode,os_sc);   
        os_sc <<  std::endl;
    }
    return NULL;

} // checkCnGtoScalarize(CnGNode* scnode, bool check_loc)


double 
EscAnalyzer::checkLocalPath(Inst* nob_inst) {
    double prob = 0;
    Node* node = nob_inst->getNode();
    Opnd* objOpnd = nob_inst->getDst();
    uint32 objId = objOpnd ->getId();

    if (_scinfo) {
        os_sc << "  -------- Find local path for  "; FlowGraph::printLabel(os_sc,node); 
        os_sc << " id." << node->getId() << "  "; 
        nob_inst->print(os_sc); os_sc << std::endl;
    }

    scannedObjs->clear();
    scannedObjsRev->clear();  // to store ids of unsuccessful nodes
    scannedSucNodes->clear();  // to store ids of unsuccessful nodes
    if (node->getKind() != Node::Kind_Exit)
        prob = checkNextNodes(node,objId,0);

    if (_scinfo) {
        double enc = irManager.getFlowGraph().getEntryNode()->getExecCount();
        os_sc << "  Path count " << prob << "    ENTRY node count " << enc 
            << "  exec_count_mult " << ec_mult << std::endl;
        if (prob!=0 && prob>=enc*ec_mult)
            os_sc << "  -------- Local path found " << std::endl;
        else 
            os_sc << "  -------- Local path not found " << std::endl;
    }
    return prob;
}  // checkLocalPath(Inst* nob_inst)


double 
EscAnalyzer::checkNextNodes(Node* n, uint32 obId, double cprob) {
    Node* node = n;
    Inst *headInst = (Inst*)node->getFirstInst();
    Opnd* opnd;
    Edges::const_iterator eit;
    double cnprob = node->getExecCount();
    double r = cnprob>cprob?cnprob:cprob;
    double no = -4;

    if (node->getKind()==Node::Kind_Exit) {
        if (_scinfo) {
            os_sc << " ****  Node EXIT : "; FlowGraph::printLabel(os_sc,node); 
            os_sc << " id." << node->getId() << " execCount " << node->getExecCount()
                << "   in prob " << cprob << std::endl;
        }
        return r;
    }

    if (scannedObjs->size()!=0) {
        if (checkScannedObjs(node->getId())) {
            if (scannedSucNodes->size()!=0) {
                if (checkScannedSucNodes(node->getId())) 
                    return cprob;
            }
            if (scannedObjsRev->size()!=0) {
                if (checkScannedObjsRev(node->getId())) 
                    return no;
            }
            return no;   // node scan is not completed
        }
    }
    for (Inst* inst=headInst->getNextInst();inst!=NULL;inst=inst->getNextInst()) {
        if (inst->getOpcode()==Op_IndirectMemoryCall || inst->getOpcode()==Op_DirectCall) {
            uint32 nsrc = inst->getNumSrcOperands();
            for (uint32 i=0; i<nsrc; i++) {
                opnd=inst->getSrc(i);
                if (opnd->getId()==obId) {
                    if (_scinfo) {
                        inst->print(os_sc); os_sc << std::endl;
                    }
                    return no;
                }
            }
        }
    }

    scannedObjs->push_back(node->getId());
    Node* tn1;
    Node* tn2;
    double r0=-1;
    double r1=-1;
    double r2=-1;

    if ((tn1=node->getUnconditionalEdgeTarget())!=NULL) {
        r0 = checkNextNodes(tn1,obId,r);
    } else {
        if ((tn1=node->getTrueEdgeTarget())!=NULL) {
            r1 = checkNextNodes(tn1,obId,r);
        }
        if ((tn2=node->getFalseEdgeTarget())!=NULL) {
            r2 = checkNextNodes(tn2,obId,r);
        }
        if (r1==-1 && r2==-1)
            r0=no;
        else 
            r0=r1>r2?r1:r2;
    }
    if (r0==no) {
        scannedObjsRev->push_back(node->getId());
        return r0;
    } else {
        scannedSucNodes->push_back(node->getId());
    }
    if (r0>r)
        return r0;
    return r;

} // checkNextNodes(Node* n, uint32 obId, double cprob))


void 
EscAnalyzer::restoreEOCreation(Insts* vc_insts, ScObjFlds* scObjFlds, VarOpnd* ob_var_opnd, uint32 oid) {
    Insts::iterator itvc;
    InstFactory& _instFactory = irManager.getInstFactory();
    ControlFlowGraph& fg = irManager.getFlowGraph();
    OpndManager& _opndManager = irManager.getOpndManager();
    TypeManager& _typeManager = irManager.getTypeManager();
    Node* node_before = NULL;   // node with call inst before opt
    Node* node_obj = NULL;      // node with restored newobj after opt
    Node* node_obj1 = NULL;     // node with restored obj fields init after opt
    Node* node_var = NULL;      // node with call inst after opt or chknull tau
    Node* node_var1 = NULL;     // node with call inst after opt
    Node* node_after1 = NULL;   // node with field var opnds updating after call
    ScObjFlds::iterator ito;
    const bool splitAfter = true;
    SsaTmpOpnd* ob_opnd = NULL;

    for (itvc = vc_insts->begin( ); itvc != vc_insts->end( ); itvc++ ) {
        Inst* vc = *itvc;
        uint32 i;
        uint32 nsrc = vc->getNumSrcOperands();

        for (i=0; i<nsrc; i++) {
            Opnd* opnd=vc->getSrc(i);  
            if (opnd->getId()==oid) {
                break;
            }
        }
        if (i>=nsrc) {
            continue;
        }

        node_before=vc->getNode();
        Node* dispatchNode = node_before->getExceptionEdgeTarget(); //now object allocation inst will be connected to this dispatch
        assert(dispatchNode);
        node_obj1 = NULL;
        node_after1 = NULL;
        node_var1 = NULL;
        if (_scinfo) {
            os_sc << "++++ objectCreate: before"  << std::endl;
            FlowGraph::print(os_sc,node_before);
            os_sc << "++++ objectCreate: before end"  << std::endl;
        }
        // loading object var opnd
        ob_opnd = _opndManager.createSsaTmpOpnd(ob_var_opnd->getType());
        Inst* ldobj=_instFactory.makeLdVar(ob_opnd,ob_var_opnd);
        ldobj->insertBefore(vc);
        // reset call inst parameters
        for (uint32 i1=0; i1<nsrc; i1++) {
            if (vc->getSrc(i1)->getId()==oid) {
                vc->setSrc(i1,ob_opnd );
            }
            if (Type::isTau(vc->getSrc(i1)->getType()->tag)) {
                Inst* tau_inst = NULL;
                Insts::iterator iit;
                uint32 to_id = vc->getSrc(i1)->getId();
                for (iit=checkInsts->begin(); iit!=checkInsts->end(); iit++) {
                    if ((*iit)->getDst()->isNull()) {
                        continue;
                    }
                    if ((*iit)->getDst()->getId()!=to_id) {
                        continue;
                    }

                    Inst* ti = *iit;
                    uint32 ti_oc = ti->getOpcode();
                    if (ti_oc == Op_TauCheckNull || ti_oc == Op_TauIsNonNull || ti_oc == Op_TauHasType) {
                        tau_inst = NULL;
                        SsaTmpOpnd* to = _opndManager.createSsaTmpOpnd(_typeManager.getTauType());
                        if (ti_oc == Op_TauCheckNull) {
                            tau_inst = _instFactory.makeTauCheckNull(to,ob_opnd);
                        }
                        if (ti_oc == Op_TauIsNonNull) {
                            tau_inst = _instFactory.makeTauIsNonNull(to,ob_opnd);
                        }
                        if (ti_oc == Op_TauHasType) {
                            tau_inst = _instFactory.makeTauHasType(to,ob_opnd,ti->asTypeInst()->getTypeInfo());
                        }
                        if (tau_inst != NULL) {
                            break;
                        }
                    }
                }
                if (tau_inst == NULL) {
                    continue;
                }
                if (_scinfo) {
                    os_sc << "---- tau_inst can throw " << tau_inst->getOperation().canThrow() << "  "; 
                    tau_inst->print(os_sc); os_sc << std::endl;
                    os_sc << std::endl;
                }
                tau_inst->insertBefore(vc);
                vc->setSrc(i1,tau_inst->getDst());
                if (tau_inst->getOperation().canThrow() == true) {
                     node_var1=fg.splitNodeAtInstruction(tau_inst,splitAfter,false,
                         _instFactory.makeLabel());
                     fg.addEdge(tau_inst->getNode(),fg.getUnwindNode());
                }
            }

        }
        // adding newobj inst
        SsaTmpOpnd* nob_opnd = _opndManager.createSsaTmpOpnd(ob_var_opnd->getType());
        Inst* newobj=_instFactory.makeNewObj(nob_opnd,ob_var_opnd->getType());
        newobj->insertBefore(ldobj);
        newobj->setBCOffset(0);
        // storing created newobj result in object var opnd
        Inst* stvobj=_instFactory.makeStVar(ob_var_opnd,nob_opnd);
        stvobj->insertBefore(ldobj);
        // node with call inst after opt
        node_var=fg.splitNodeAtInstruction(stvobj,splitAfter,false,_instFactory.makeLabel());
        // checking, if the object is created goto node_var node 
        SsaTmpOpnd* ob_opnd_as_flag = _opndManager.createSsaTmpOpnd(ob_var_opnd->getType());
        _instFactory.makeLdVar(ob_opnd_as_flag,ob_var_opnd)->insertBefore(newobj);
        Inst* branch_inst = _instFactory.makeBranch(Cmp_NonZero, ob_opnd_as_flag->getType()->tag, 
            ob_opnd_as_flag, (LabelInst*)(node_var->getFirstInst()));
        branch_inst->insertBefore(newobj);
        // node with newobj instruction after opt
        node_obj=fg.splitNodeAtInstruction(branch_inst,splitAfter,false,_instFactory.makeLabel());
        fg.addEdge(node_before, node_var);
        // created object field initialization
        ScObjFld* sco = NULL;
        if (scObjFlds->size() > 0) {
            Opnd* tau_op = _opndManager.createSsaTmpOpnd(_typeManager.getTauType());
            _instFactory.makeTauUnsafe(tau_op)->insertBefore(branch_inst);
            for (ito = scObjFlds->begin( ); ito != scObjFlds->end( ); ito++ ){
                sco = (*ito);
                if (sco->ls_insts->size()==0) {
                    continue;
                }
                Type* fl_type = NULL;
                Type* type = NULL;
                Inst* ii = sco->ls_insts->front();
                Inst* iadr = NULL;
                if (ii->getOpcode()==Op_TauStInd) {
                    iadr=ii->getSrc(1)->getInst();
                    fl_type = ii->getSrc(1)->getType()->asPtrType()->getPointedToType();
                    type = ii->getSrc(0)->getType();
                } else {
                    iadr=ii->getSrc(0)->getInst();
                    fl_type = ii->getSrc(0)->getType()->asPtrType()->getPointedToType();
                    type = ii->getDst()->getType();
                }
                assert(iadr->getOpcode()==Op_LdFieldAddr); // only esc.objects may be optimized
                bool compress = (fl_type->isCompressedReference() &&
                    !type->isCompressedReference());
                Modifier compressMod = Modifier(compress ? AutoCompress_Yes
                    : AutoCompress_No);
                Modifier mod = (compInterface.needWriteBarriers())?
                    (Modifier(Store_WriteBarrier)|compressMod) : (Modifier(Store_NoWriteBarrier)|compressMod);

                SsaTmpOpnd* fl_tmp_opnd_st = _opndManager.createSsaTmpOpnd(type);
                Inst* ldflvar_inst = _instFactory.makeLdVar(fl_tmp_opnd_st, sco->fldVarOpnd);
                if (sco->isFinalFld) {
                    ldflvar_inst->insertBefore(stvobj); // after newobj
                } else {
                    ldflvar_inst->insertAfter(ldobj); // after ldvar obj_var_opnd
                }
                Opnd* dst = _opndManager.createSsaTmpOpnd(iadr->getDst()->getType());
                FieldDesc* fd = iadr->asFieldAccessInst()->getFieldDesc();
                Inst* ldfladr_inst = NULL;
                if (sco->isFinalFld) {
                    ldfladr_inst = _instFactory.makeLdFieldAddr(dst,nob_opnd,fd);
                    ldfladr_inst->insertBefore(stvobj); // after newobj
                } else {
                    ldfladr_inst = _instFactory.makeLdFieldAddr(dst,ob_opnd,fd);
                    ldfladr_inst->insertAfter(ldflvar_inst); // after ldvar ob_var_opnd
                }

                Inst* nstind=_instFactory.makeTauStInd(mod,type->tag,
                    fl_tmp_opnd_st,dst,tau_op,tau_op,tau_op);
                if (sco->isFinalFld) {
                    nstind->insertBefore(stvobj);
                    continue;
                } else {
                    nstind->insertAfter(ldfladr_inst);
                }
                // updating non-final field var.opnds after call
                if (node_after1 == NULL) {
                    node_after1=fg.createBlockNode(_instFactory.makeLabel());
                }
                // loading field value
                SsaTmpOpnd* fl_tmp_opnd_ld = _opndManager.createSsaTmpOpnd(type);
                bool comprRefs = compressedReferencesArg
                    || (VMInterface::areReferencesCompressed());
                Modifier mod1 = comprRefs ? AutoCompress_Yes : AutoCompress_No;
                Inst* ldf = _instFactory.makeTauLdInd(mod1,type->tag,fl_tmp_opnd_ld,dst,
                    tau_op,tau_op);
                node_after1->appendInst(ldf);
                // storing field value in field var opnd
                Inst* stv = _instFactory.makeStVar(sco->fldVarOpnd,fl_tmp_opnd_ld);
                node_after1->appendInst(stv);
            }
            if (node_after1 != NULL) {
                if (_scinfo) {
                    os_sc << "!!!! to restore not final fields "  << std::endl;
                }
                Node* node_call = node_var;
                if (node_var1 != NULL) {
                    node_call = node_var1;
                }
                // next node after node with call inst
                Node* node_after = node_call->getUnconditionalEdgeTarget();
                // inserting node with updating field var opnds
                fg.removeEdge(node_call->getUnconditionalEdge());
                fg.addEdge(node_call,node_after1);
                fg.addEdge(node_after1,node_after);
            }
        }
        
        node_obj1=fg.splitNodeAtInstruction(newobj,splitAfter,false, _instFactory.makeLabel());
        fg.addEdge(node_obj,dispatchNode);

        if (_scinfo) {
            os_sc << "++++ objectCreate: after"  << std::endl;
            FlowGraph::print(os_sc,node_before);
            FlowGraph::print(os_sc,node_obj);
            if (node_obj1 != NULL) {
                FlowGraph::print(os_sc,node_obj1);
            }
            FlowGraph::print(os_sc,node_var);
            if (node_var1 != NULL) {
                FlowGraph::print(os_sc,node_var1);
            }
            if (node_after1 != NULL) {
                FlowGraph::print(os_sc,node_after1);
            }
            os_sc << "++++ objectCreate: after end"  << std::endl;
        }
    }
} // restoreEOCreation(Insts* vc_insts, ScObjFlds* objs, VarOpnd* ob_var_opnd,...)


void 
EscAnalyzer::removeInst(Inst* reminst) {
    ControlFlowGraph& fg = irManager.getFlowGraph();
    Edge* excedge = NULL;

    if (reminst->getNode()!=NULL) {
        if (_scinfo) {
            os_sc <<"--rmInst done: CFGnode " << reminst->getNode()->getId() <<"  ";
            ((Inst*)(reminst->getNode()->getLabelInst()))->print(os_sc); os_sc << "  ";
            reminst->print(os_sc);
            os_sc << std::endl;
        }
        if (reminst->getOperation().canThrow()==true) {
            excedge = (Edge*)reminst->getNode()->getExceptionEdge();
            assert(excedge != NULL);
            if (_scinfo) {
                os_sc <<"--rmEdge done: to "; ((Inst*)(excedge->getTargetNode()->getLabelInst()))->print(os_sc);
                os_sc << std::endl;
            }
            fg.removeEdge(excedge);
        }
        reminst->unlink();
        return;
    }
    if (_scinfo) {
        os_sc <<"--rmInst null: CFGnode  NULL  ";
        reminst->print(os_sc);
        os_sc << std::endl;
    }
} // removeInst(Inst* reminst)


MethodDesc* 
EscAnalyzer::getMD(Inst* inst) {
    MethodDesc* md;
    if (inst->getOpcode()==Op_DirectCall)
        return inst->asMethodCallInst()->getMethodDesc();
    if (inst->getOpcode()!=Op_IndirectMemoryCall)
        return NULL;
    if (inst->getSrc(0)->getInst()->getOpcode()== Op_LdVar) {
        md = inst->getSrc(0)->getType()->asMethodPtrType()->getMethodDesc();
    } else {
        md = inst->getSrc(0)->getInst()->asMethodInst()->getMethodDesc();
    }
    return md;
}


void 
EscAnalyzer::fixMethodEndInsts(uint32 ob_id) {
    Insts::iterator itmei;
    InstFactory& _instFactory = irManager.getInstFactory();
    OpndManager& _opndManager = irManager.getOpndManager();
    for (itmei = methodEndInsts->begin( ); itmei != methodEndInsts->end( ); itmei++ ) {
        Inst* mei = *itmei;
        uint32 i = 0;
        uint32 nsrc = mei->getNumSrcOperands();
        
        if (nsrc == 0)
            continue;
        Opnd* o = mei->getSrc(i);
        if (o->getId() != ob_id)
            continue;

        if (_scinfo) {
            os_sc << "++++ Op_MethodEnd " << std::endl;
            mei->print(os_sc); os_sc << std::endl;
            os_sc << "  -- replaced by " << std::endl;
        }

        SsaTmpOpnd* null_opnd = _opndManager.createSsaTmpOpnd(o->getType());
        _instFactory.makeLdNull(null_opnd)->insertBefore(mei);
        mei->setSrc(i,null_opnd);

        if (_scinfo) {
            mei->getPrevInst()->print(os_sc); os_sc << std::endl;
            mei->print(os_sc); os_sc << std::endl;
            os_sc << "++++             " << std::endl;
        }
    }
} // fixMethodEndInsts(uint32 ob_id)


EscAnalyzer::CnGNode* 
EscAnalyzer::getLObj(CnGNode* vval) {
	CnGNode* n = vval;
	while (n->nodeType != NT_LDOBJ) {
		if (n->nodeType!=NT_VARVAL)
			return NULL;
		if (n->outEdges == NULL)
			return NULL;
		if ((n->outEdges)->size()!=1)
			return NULL;
		n=(n->outEdges)->front()->cngNodeTo;
	}
	return n;
} //getLObj(CnGNode* vval)


bool
EscAnalyzer::checkVVarSrcs(Inst* inst) {
    Inst* inst1;
    uint32 nsrc=inst->getNumSrcOperands();
    std::string text = " checkVVS  ";
    bool res = true;

    if (scannedObjs->size()!=0) {
        if (checkScannedObjs(inst->getId())) {
#ifdef _DEBUG
            if (_scinfo) {
                os_sc << "instId " << inst->getId() 
                    << "  .  .  .   returns false" << std::endl;
            }
#endif
            return false;
        }
    }
#ifdef _DEBUG
    if (_scinfo) {
        os_sc << text; 
        inst->print(os_sc); 
        os_sc << std::endl;
        if (inst->getOpcode()==Op_TauLdInd || inst->getOpcode()==Op_LdVar) { // ldind,ldvar 
            Opnd *dst = inst->getDst(); 
            CnGNode* n = findCnGNode_op(dst->getId());
            if (n != NULL) {
                os_sc<< text << "  ";       
                printCnGNode(n,os_sc); 
                os_sc<< std::endl;

            }
        }
        switch (inst->getOpcode()) {
        case Op_LdRef:           // ldref
        case Op_NewObj:          // newobj
        case Op_NewArray:        // newarray
        case Op_NewMultiArray:   // newmultiarray
        case Op_DefArg:          // defarg
            {
                CnGNode* n = findCnGNode_in(inst->getId());
                if (n != NULL) {
                    os_sc << text << "  ";     
                    printCnGNode(n,os_sc); 
                    os_sc << std::endl;
                }
                break;
            }
        default:
            break;
        }
    }
#endif
    
    if (inst->getOpcode()==Op_DirectCall || inst->getOpcode()==Op_IndirectMemoryCall) {
        return false;
    }
    if (nsrc == 0) {
        if (inst->getOpcode() == Op_LdStaticAddr) {
            FieldDesc* fd = inst->asFieldAccessInst()->getFieldDesc();
            const char* ptn = fd->getParentType()->getName();
            if (checkObjectType(ptn) && strcmp(fd->getName(),"CACHE")==0 ) {
                return true;
            } else {
                return false;
            }
        }
        if (inst->getOpcode() == Op_NewObj || inst->getOpcode() == Op_NewArray 
            || inst->getOpcode() == Op_NewMultiArray) {
            return true;
        }
        if (inst->getOpcode() == Op_LdRef || inst->getOpcode() == Op_DefArg ) {
            return false;
        }
    }
    scannedObjs->push_back(inst->getId());
    switch (inst->getOpcode()) {
        case Op_TauLdInd:        // ldind
        case Op_AddScaledIndex:  // addindex
            inst1 = inst->getSrc(0)->getInst();
            res = checkVVarSrcs(inst1);
            break;
        case Op_TauStInd:        // stind
            for (uint32 i=0; i<2; i++) {
                inst1 = inst->getSrc(i)->getInst();
                res = checkVVarSrcs(inst1);
            }
            break;
        default:
            for (uint32 i=0; i<nsrc; i++) {
                inst1 = inst->getSrc(i)->getInst();
                bool res1 = checkVVarSrcs(inst1);
                res = res && res1;
            }
    }
    scannedObjs->pop_back();
#ifdef _DEBUG
    if (_scinfo) {
        os_sc << "           1 returns " << res << std::endl;
    }
#endif
    return res;
}  // checkVVarSrcs(Inst* inst) 


bool 
EscAnalyzer::checkObjectType(const char* otn) {
    if ((strcmp(otn,"java/lang/Integer") == 0) || (strcmp(otn,"java/lang/Short") == 0) 
        || (strcmp(otn,"java/lang/Long") == 0) || (strcmp(otn,"java/lang/Character") == 0)
        || (strcmp(otn,"java/lang/Integer$valueOfCache") == 0)
        || (strcmp(otn,"java/lang/Short$valueOfCache") == 0)
        || (strcmp(otn,"java/lang/Long$valueOfCache") == 0)
        || (strcmp(otn,"java/lang/Character$valueOfCache") == 0) ) {
        return true;
    } 
    return false;
}


bool 
EscAnalyzer::checkObjFlds(ScObjFlds* nscObjFlds, ScObjFlds* lscObjFlds) {
    ScObjFlds::iterator it1;
    ScObjFlds::iterator it2;

    if (nscObjFlds->size() == 0) {
        return true;
    }
    if (lscObjFlds->size() == 0) {
        return false;
    }
    for (it1=lscObjFlds->begin(); it1!=lscObjFlds->end(); it1++) {
        for (it2=nscObjFlds->begin(); it2!=nscObjFlds->end(); it2++) {
            if ((*it1)->fd == (*it2)->fd) {
                break;
            }
            return false;
        }
    }
    return true;
} // checkObjFlds(ScObjFlds* nscObjFlds, ScObjFlds* scObjFlds)


void 
EscAnalyzer::fixCheckInsts(uint32 opId) {
    Insts::iterator iit;
    Edge* excedge;

    if (checkInsts == NULL) {
        return;
    }
    os_sc << "  checkInsts " << checkInsts->size() << std::endl;
    for (iit=checkInsts->begin(); iit!=checkInsts->end(); iit++) {
        if ((*iit)->getSrc(0)->getId()!=opId) {
            continue;
        }
        if (_scinfo) {
            os_sc << "  **  "; (*iit)->print(os_sc); os_sc << std::endl;
        }
        Inst* inst = (*iit);
        uint32 opcode = inst->getOpcode();
        if (opcode == Op_Branch) {
            excedge = NULL;
            if (inst->getComparisonModifier()==Cmp_Zero) {
                excedge = inst->getNode()->getTrueEdge();
            }
            if (inst->getComparisonModifier()==Cmp_NonZero) {
                excedge = inst->getNode()->getFalseEdge();
            }
            if (_scinfo) {
                os_sc <<"-- to remove edge: to "; 
                ((Inst*)( excedge->getTargetNode()->getLabelInst() ))->print(os_sc);
                os_sc << std::endl;
            }
            if (excedge != NULL) {
                irManager.getFlowGraph().removeEdge(excedge);
                removeInst(inst);
            }
        }
        if (opcode == Op_TauIsNonNull || opcode == Op_TauCheckNull || opcode == Op_TauHasType) {
            if (inst->getOperation().canThrow()==true) {
                excedge = (Edge*)inst->getNode()->getExceptionEdge();
                if (_scinfo) {
                    os_sc <<"-- exc edge to "; 
                    ((Inst*)(excedge->getTargetNode()->getLabelInst()))->print(os_sc);
                    os_sc << std::endl;
                }
            }
            removeInst(inst);
        }
    }
} // fixCheckInsts(uint32 opId)


void 
EscAnalyzer::checkToScalarizeFinalFiels(CnGNode* onode, ScObjFlds* scObjFlds) {
    ScObjFlds::iterator ito;
    ScObjFld* sco = NULL;
    bool do_fsc = false;
    OpndManager& _opndManager = irManager.getOpndManager();
    InstFactory& _instFactory = irManager.getInstFactory();
    TypeManager& _typeManager  = irManager.getTypeManager();

    if (_scinfo) {
        os_sc << "########################" << std::endl;
        printCnGNode(onode,os_sc); os_sc << std::endl;
    }
    scObjFlds->clear();
    collectStLdInsts(onode, scObjFlds);
    if (scObjFlds->size() != 0) {
        if (_scinfo) {
            os_sc << " used fields: " << scObjFlds->size() << "  final fields exist: ";
        }
        for (ito = scObjFlds->begin( ); ito != scObjFlds->end( ); ito++ ){
            sco = (*ito);
            if (_scinfo) {
                os_sc << sco->isFinalFld << " ";
            }
            if (sco->isFinalFld) {
                do_fsc = true;
            }
            if (_scinfo) {
                os_sc << " field usage : " << sco->ls_insts->size();
            }
        }
        if (_scinfo) {
            os_sc << std::endl;
        }
    }
    if (_scinfo) {
        if (scObjFlds->size() != 0 && do_fsc) {
            printCnGNode(onode,os_sc); os_sc << std::endl;
            os_sc << " ############# try to do" << std::endl;
        } else {
            os_sc << " ############# do_fsc " << do_fsc << std::endl;
        }
        os_sc << "# #######################" << std::endl;
    }
    if (onode->nodeType == NT_LDOBJ) {

        for (ito = scObjFlds->begin( ); ito != scObjFlds->end( ); ito++ ){
            sco = (*ito);
            if (!sco->isFinalFld || sco->ls_insts->size() < 2) {
                continue;
            }
            Type* fl_type = NULL;
            Type* fl_type1 = NULL;
            Inst* ii = sco->ls_insts->front();
            Inst* iadr = NULL;
            if (ii->getOpcode()==Op_TauStInd) {
                iadr=ii->getSrc(1)->getInst();
                fl_type1 = ii->getSrc(1)->getType()->asPtrType()->getPointedToType();
                fl_type = ii->getSrc(0)->getType();
            } else {
                iadr=ii->getSrc(0)->getInst();
                fl_type1 = ii->getSrc(0)->getType()->asPtrType()->getPointedToType();
                fl_type = ii->getDst()->getType();
            }
            VarOpnd* fl_var_opnd = _opndManager.createVarOpnd(fl_type, false);
            sco->fldVarOpnd = fl_var_opnd;
            if (_scinfo) {
                os_sc<<" PoitedType "; fl_type1->print(os_sc); os_sc <<std::endl;
                os_sc<<" OperandType "; fl_type->print(os_sc); os_sc <<std::endl;
            }
            scalarizeOFldUsage(sco);
            if (_scinfo) {
                os_sc << "++++ old newobj added fld_var: before"  << std::endl;
                FlowGraph::print(os_sc,onode->nInst->getNode());
                os_sc << "++++ old newobj: before end"  << std::endl;
            }
            bool comprRefs = compressedReferencesArg
                || (VMInterface::areReferencesCompressed());
            Modifier mod1 = comprRefs ? AutoCompress_Yes : AutoCompress_No;
            Opnd* ld_tau_op = _opndManager.createSsaTmpOpnd(_typeManager.getTauType());
            Inst* itau = _instFactory.makeTauUnsafe(ld_tau_op);
            itau->insertAfter(onode->nInst);
            FieldDesc* fd = iadr->asFieldAccessInst()->getFieldDesc();
            Opnd* dst_ld = _opndManager.createSsaTmpOpnd(iadr->getDst()->getType());
            Opnd* ob_opnd = (Opnd*)(onode->refObj);
            Inst* lda = _instFactory.makeLdFieldAddr(dst_ld,ob_opnd,fd);
            lda->insertAfter(itau);
            SsaTmpOpnd* fl_tmp_opnd_ld = _opndManager.createSsaTmpOpnd(fl_type);
            Inst* ldf = _instFactory.makeTauLdInd(mod1,fl_var_opnd->getType()->tag,
                        fl_tmp_opnd_ld,dst_ld,ld_tau_op,ld_tau_op);
            ldf->insertAfter(lda);
            Inst* stv = _instFactory.makeStVar(fl_var_opnd,fl_tmp_opnd_ld);
            stv->insertAfter(ldf);
            if (_scinfo) {
                os_sc << "++++ old newobj added fld_var: after"  << std::endl;
                FlowGraph::print(os_sc,onode->nInst->getNode());
                os_sc << "++++ old newobj: after end"  << std::endl;
            }
        }
    }
} // checkToScalarizeFinalFiels(CnGNode* onode, ScObjFlds* scObjFlds)

} //namespace Jitrino 

