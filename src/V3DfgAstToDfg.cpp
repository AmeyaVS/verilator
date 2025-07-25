// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Convert AstModule to DfgGraph
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2025 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
//
// Convert and AstModule to a DfgGraph. We proceed by visiting convertible logic blocks (e.g.:
// AstAssignW of appropriate type and with no delays), recursively constructing DfgVertex instances
// for the expressions that compose the subject logic block. If all expressions in the current
// logic block can be converted, then we delete the logic block (now represented in the DfgGraph),
// and connect the corresponding DfgVertex instances appropriately. If some of the expressions were
// not convertible in the current logic block, we revert (delete) the DfgVertex instances created
// for the logic block, and leave the logic block in the AstModule. Any variable reference from
// non-converted logic blocks (or other constructs under the AstModule) are marked as being
// referenced in the AstModule, which is relevant for later optimization.
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3Dfg.h"
#include "V3DfgPasses.h"

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

// Create a DfgVertex out of a AstNodeExpr. For most AstNodeExpr subtypes, this can be done
// automatically. For the few special cases, we provide specializations below
template <typename T_Vertex, typename T_Node>
T_Vertex* makeVertex(const T_Node* nodep, DfgGraph& dfg) {
    return new T_Vertex{dfg, nodep->fileline(), DfgVertex::dtypeFor(nodep)};
}

//======================================================================
// Currently unhandled nodes
// LCOV_EXCL_START
// AstCCast changes width, but should not exists where DFG optimization is currently invoked
template <>
DfgCCast* makeVertex<DfgCCast, AstCCast>(const AstCCast*, DfgGraph&) {
    return nullptr;
}
// Unhandled in DfgToAst, but also operates on strings which we don't optimize anyway
template <>
DfgAtoN* makeVertex<DfgAtoN, AstAtoN>(const AstAtoN*, DfgGraph&) {
    return nullptr;
}
// Unhandled in DfgToAst, but also operates on strings which we don't optimize anyway
template <>
DfgCompareNN* makeVertex<DfgCompareNN, AstCompareNN>(const AstCompareNN*, DfgGraph&) {
    return nullptr;
}
// Unhandled in DfgToAst, but also operates on unpacked arrays which we don't optimize anyway
template <>
DfgSliceSel* makeVertex<DfgSliceSel, AstSliceSel>(const AstSliceSel*, DfgGraph&) {
    return nullptr;
}
// LCOV_EXCL_STOP

}  // namespace

template <bool T_Scoped>
class AstToDfgVisitor final : public VNVisitor {
    // NODE STATE

    // AstNode::user1p   // DfgVertex for this AstNode
    const VNUser1InUse m_user1InUse;

    // TYPES
    // Represents a driver during canonicalization
    struct Driver final {
        FileLine* m_fileline;
        DfgVertex* m_vtxp;
        uint32_t m_lsb;
        Driver(FileLine* flp, uint32_t lsb, DfgVertex* vtxp)
            : m_fileline{flp}
            , m_vtxp{vtxp}
            , m_lsb{lsb} {}
    };

    using RootType = std::conditional_t<T_Scoped, AstNetlist, AstModule>;
    using VariableType = std::conditional_t<T_Scoped, AstVarScope, AstVar>;

    // STATE

    DfgGraph* const m_dfgp;  // The graph being built
    V3DfgOptimizationContext& m_ctx;  // The optimization context for stats
    bool m_foundUnhandled = false;  // Found node not implemented as DFG or not implemented 'visit'
    std::vector<DfgVertex*> m_uncommittedVertices;  // Vertices that we might decide to revert
    bool m_converting = false;  // We are trying to convert some logic at the moment
    std::vector<DfgVarPacked*> m_varPackedps;  // All the DfgVarPacked vertices we created.
    std::vector<DfgVarArray*> m_varArrayps;  // All the DfgVarArray vertices we created.

    // METHODS
    static VariableType* getTarget(const AstVarRef* refp) {
        // TODO: remove the useless reinterpret_casts when C++17 'if constexpr' actually works
        if VL_CONSTEXPR_CXX17 (T_Scoped) {
            return reinterpret_cast<VariableType*>(refp->varScopep());
        } else {
            return reinterpret_cast<VariableType*>(refp->varp());
        }
    }

    static AstVar* getAstVar(VariableType* vp) {
        // TODO: remove the useless reinterpret_casts when C++17 'if constexpr' actually works
        if VL_CONSTEXPR_CXX17 (T_Scoped) {
            return reinterpret_cast<AstVarScope*>(vp)->varp();
        } else {
            return reinterpret_cast<AstVar*>(vp);
        }
    }

    void markReferenced(AstNode* nodep) {
        nodep->foreach([this](const AstVarRef* refp) {
            // No need to (and in fact cannot) mark variables with unsupported dtypes
            if (!DfgVertex::isSupportedDType(refp->varp()->dtypep())) return;
            VariableType* const tgtp = getTarget(refp);
            // Mark vertex as having a module reference outside current DFG
            getNet(tgtp)->setHasModRefs();
            // Mark variable as written from non-DFG logic
            if (refp->access().isWriteOrRW()) tgtp->user3(true);
        });
    }

    void commitVertices() { m_uncommittedVertices.clear(); }

    void revertUncommittedVertices() {
        for (DfgVertex* const vtxp : m_uncommittedVertices) vtxp->unlinkDelete(*m_dfgp);
        m_uncommittedVertices.clear();
    }

    DfgVertexVar* getNet(VariableType* vp) {
        if (!vp->user1p()) {
            // vp DfgVertexVar vertices are not added to m_uncommittedVertices, because we
            // want to hold onto them via AstVar::user1p, and the AstVar might be referenced via
            // multiple AstVarRef instances, so we will never revert a DfgVertexVar once
            // created. We will delete unconnected variable vertices at the end.
            if (VN_IS(vp->dtypep()->skipRefp(), UnpackArrayDType)) {
                DfgVarArray* const vtxp = new DfgVarArray{*m_dfgp, vp};
                vp->user1p();
                m_varArrayps.push_back(vtxp);
                vp->user1p(vtxp);
            } else {
                DfgVarPacked* const vtxp = new DfgVarPacked{*m_dfgp, vp};
                m_varPackedps.push_back(vtxp);
                vp->user1p(vtxp);
            }
        }
        return vp->user1u().template to<DfgVertexVar*>();
    }

    DfgVertex* getVertex(AstNode* nodep) {
        DfgVertex* vtxp = nodep->user1u().to<DfgVertex*>();
        UASSERT_OBJ(vtxp, nodep, "Missing Dfg vertex");
        return vtxp;
    }

    // Returns true if the expression cannot (or should not) be represented by DFG
    bool unhandled(AstNodeExpr* nodep) {
        // Short-circuiting if something was already unhandled
        if (!m_foundUnhandled) {
            // Impure nodes cannot be represented
            if (!nodep->isPure()) {
                m_foundUnhandled = true;
                ++m_ctx.m_nonRepImpure;
            }
            // Check node has supported dtype
            if (!DfgVertex::isSupportedDType(nodep->dtypep())) {
                m_foundUnhandled = true;
                ++m_ctx.m_nonRepDType;
            }
        }
        return m_foundUnhandled;
    }

    DfgVertexSplice* convertLValue(AstNode* nodep) {
        FileLine* const flp = nodep->fileline();

        if (AstVarRef* const vrefp = VN_CAST(nodep, VarRef)) {
            m_foundUnhandled = false;
            visit(vrefp);
            if (m_foundUnhandled) return nullptr;
            DfgVertexVar* const vtxp = getVertex(vrefp)->template as<DfgVertexVar>();
            // Ensure driving splice vertex exists
            if (!vtxp->srcp()) {
                if (VN_IS(vtxp->dtypep(), UnpackArrayDType)) {
                    vtxp->srcp(new DfgSpliceArray{*m_dfgp, flp, vtxp->dtypep()});
                } else {
                    vtxp->srcp(new DfgSplicePacked{*m_dfgp, flp, vtxp->dtypep()});
                }
            }
            return vtxp->srcp()->as<DfgVertexSplice>();
        }

        ++m_ctx.m_nonRepLhs;
        return nullptr;
    }

    // Build DfgEdge representing the LValue assignment. Returns false if unsuccessful.
    bool convertAssignment(FileLine* flp, AstNode* nodep, DfgVertex* vtxp) {
        // Concatenation on the LHS. Select parts of the driving 'vtxp' then convert each part
        if (AstConcat* const concatp = VN_CAST(nodep, Concat)) {
            AstNode* const lhsp = concatp->lhsp();
            AstNode* const rhsp = concatp->rhsp();

            {  // Convet LHS of concat
                FileLine* const lFlp = lhsp->fileline();
                DfgSel* const lVtxp = new DfgSel{*m_dfgp, lFlp, DfgVertex::dtypeFor(lhsp)};
                lVtxp->fromp(vtxp);
                lVtxp->lsb(rhsp->width());
                if (!convertAssignment(flp, lhsp, lVtxp)) return false;
            }

            {  // Convert RHS of concat
                FileLine* const rFlp = rhsp->fileline();
                DfgSel* const rVtxp = new DfgSel{*m_dfgp, rFlp, DfgVertex::dtypeFor(rhsp)};
                rVtxp->fromp(vtxp);
                rVtxp->lsb(0);
                return convertAssignment(flp, rhsp, rVtxp);
            }
        }

        if (AstSel* const selp = VN_CAST(nodep, Sel)) {
            AstVarRef* const vrefp = VN_CAST(selp->fromp(), VarRef);
            const AstConst* const lsbp = VN_CAST(selp->lsbp(), Const);
            if (!vrefp || !lsbp) {
                ++m_ctx.m_nonRepLhs;
                return false;
            }
            if (DfgVertexSplice* const splicep = convertLValue(vrefp)) {
                splicep->template as<DfgSplicePacked>()->addDriver(flp, lsbp->toUInt(), vtxp);
                return true;
            }
        } else if (AstArraySel* const selp = VN_CAST(nodep, ArraySel)) {
            AstVarRef* const vrefp = VN_CAST(selp->fromp(), VarRef);
            const AstConst* const idxp = VN_CAST(selp->bitp(), Const);
            if (!vrefp || !idxp) {
                ++m_ctx.m_nonRepLhs;
                return false;
            }
            if (DfgVertexSplice* const splicep = convertLValue(vrefp)) {
                splicep->template as<DfgSpliceArray>()->addDriver(flp, idxp->toUInt(), vtxp);
                return true;
            }
        } else if (VN_IS(nodep, VarRef)) {
            if (DfgVertexSplice* const splicep = convertLValue(nodep)) {
                splicep->template as<DfgSplicePacked>()->addDriver(flp, 0, vtxp);
                return true;
            }
        } else {
            ++m_ctx.m_nonRepLhs;
        }
        return false;
    }

    bool convertEquation(AstNode* nodep, FileLine* flp, AstNode* lhsp, AstNode* rhsp) {
        UASSERT_OBJ(m_uncommittedVertices.empty(), nodep, "Should not nest");

        // Currently cannot handle direct assignments between unpacked types. These arise e.g.
        // when passing an unpacked array through a module port.
        if (!DfgVertex::isSupportedPackedDType(lhsp->dtypep())
            || !DfgVertex::isSupportedPackedDType(rhsp->dtypep())) {
            markReferenced(nodep);
            ++m_ctx.m_nonRepDType;
            return false;
        }

        // Cannot handle mismatched widths. Mismatched assignments should have been fixed up in
        // earlier passes anyway, so this should never be hit, but being paranoid just in case.
        if (lhsp->width() != rhsp->width()) {  // LCOV_EXCL_START
            markReferenced(nodep);
            ++m_ctx.m_nonRepWidth;
            return false;
        }  // LCOV_EXCL_STOP

        VL_RESTORER(m_converting);
        m_converting = true;

        m_foundUnhandled = false;
        iterate(rhsp);
        // cppcheck-has-bug-suppress knownConditionTrueFalse
        if (m_foundUnhandled) {
            revertUncommittedVertices();
            markReferenced(nodep);
            return false;
        }

        if (!convertAssignment(flp, lhsp, getVertex(rhsp))) {
            revertUncommittedVertices();
            markReferenced(nodep);
            return false;
        }

        // Connect the rhs vertex to the driven edge
        commitVertices();

        // Remove node from Ast. Now represented by the Dfg.
        VL_DO_DANGLING(nodep->unlinkFrBack()->deleteTree(), nodep);

        //
        ++m_ctx.m_representable;
        return true;
    }

    // Sometime assignment ranges are coalesced by V3Const,
    // so we unpack concatenations for better error reporting.
    void addDriver(FileLine* flp, uint32_t lsb, DfgVertex* vtxp,
                   std::vector<Driver>& drivers) const {
        if (DfgConcat* const concatp = vtxp->cast<DfgConcat>()) {
            DfgVertex* const rhsp = concatp->rhsp();
            auto const rhs_width = rhsp->width();
            addDriver(rhsp->fileline(), lsb, rhsp, drivers);
            DfgVertex* const lhsp = concatp->lhsp();
            addDriver(lhsp->fileline(), lsb + rhs_width, lhsp, drivers);
            concatp->unlinkDelete(*m_dfgp);
        } else {
            drivers.emplace_back(flp, lsb, vtxp);
        }
    }

    // Canonicalize packed variables
    void canonicalizePacked() {
        for (DfgVarPacked* const varp : m_varPackedps) {
            // Delete variables with no sinks nor sources (this can happen due to reverting
            // uncommitted vertices, which does not remove variables)
            if (!varp->hasSinks() && !varp->srcp()) {
                VL_DO_DANGLING(varp->unlinkDelete(*m_dfgp), varp);
                continue;
            }

            // Nothing to do for un-driven (input) variables
            if (!varp->srcp()) continue;

            DfgSplicePacked* const splicep = varp->srcp()->as<DfgSplicePacked>();

            // Gather (and unlink) all drivers
            std::vector<Driver> drivers;
            drivers.reserve(splicep->arity());
            splicep->forEachSourceEdge([this, splicep, &drivers](DfgEdge& edge, size_t idx) {
                DfgVertex* const driverp = edge.sourcep();
                UASSERT(driverp, "Should not have created undriven sources");
                addDriver(splicep->driverFileLine(idx), splicep->driverLsb(idx), driverp, drivers);
                edge.unlinkSource();
            });

            const auto cmp = [](const Driver& a, const Driver& b) {
                if (a.m_lsb != b.m_lsb) return a.m_lsb < b.m_lsb;
                return a.m_fileline->operatorCompare(*b.m_fileline) < 0;
            };

            // Sort drivers by LSB
            std::stable_sort(drivers.begin(), drivers.end(), cmp);

            // Vertices that might have become unused due to multiple driver resolution. Having
            // multiple drivers is an error and is hence assumed to be rare, so performance is
            // not very important, set will suffice.
            std::set<DfgVertex*> prune;

            // Fix multiply driven ranges
            for (auto it = drivers.begin(); it != drivers.end();) {
                Driver& a = *it++;
                const uint32_t aWidth = a.m_vtxp->width();
                const uint32_t aEnd = a.m_lsb + aWidth;
                while (it != drivers.end()) {
                    Driver& b = *it;
                    // If no overlap, then nothing to do
                    if (b.m_lsb >= aEnd) break;

                    const uint32_t bWidth = b.m_vtxp->width();
                    const uint32_t bEnd = b.m_lsb + bWidth;
                    const uint32_t overlapEnd = std::min(aEnd, bEnd) - 1;

                    if (a.m_fileline->operatorCompare(*b.m_fileline) != 0
                        && !varp->varp()->isUsedLoopIdx()  // Loop index often abused, so suppress
                    ) {
                        AstNode* const vp = varp->varScopep()
                                                ? static_cast<AstNode*>(varp->varScopep())
                                                : static_cast<AstNode*>(varp->varp());
                        vp->v3warn(  //
                            MULTIDRIVEN,
                            "Bits ["  //
                                << overlapEnd << ":" << b.m_lsb << "] of signal "
                                << vp->prettyNameQ() << " have multiple combinational drivers\n"
                                << a.m_fileline->warnOther() << "... Location of first driver\n"
                                << a.m_fileline->warnContextPrimary() << '\n'
                                << b.m_fileline->warnOther() << "... Location of other driver\n"
                                << b.m_fileline->warnContextSecondary() << vp->warnOther()
                                << "... Only the first driver will be respected");
                    }

                    // If the first driver completely covers the range of the second driver,
                    // we can just delete the second driver completely, otherwise adjust the
                    // second driver to apply from the end of the range of the first driver.
                    if (aEnd >= bEnd) {
                        prune.emplace(b.m_vtxp);
                        it = drivers.erase(it);
                    } else {
                        const auto dtypep = DfgVertex::dtypeForWidth(bEnd - aEnd);
                        DfgSel* const selp = new DfgSel{*m_dfgp, b.m_vtxp->fileline(), dtypep};
                        selp->fromp(b.m_vtxp);
                        selp->lsb(aEnd - b.m_lsb);
                        b.m_lsb = aEnd;
                        b.m_vtxp = selp;
                        std::stable_sort(it, drivers.end(), cmp);
                    }
                }
            }

            // Coalesce adjacent ranges
            for (size_t i = 0, j = 1; j < drivers.size(); ++j) {
                Driver& a = drivers[i];
                Driver& b = drivers[j];

                // Coalesce adjacent range
                const uint32_t aWidth = a.m_vtxp->width();
                const uint32_t bWidth = b.m_vtxp->width();
                if (a.m_lsb + aWidth == b.m_lsb) {
                    const auto dtypep = DfgVertex::dtypeForWidth(aWidth + bWidth);
                    DfgConcat* const concatp = new DfgConcat{*m_dfgp, a.m_fileline, dtypep};
                    concatp->rhsp(a.m_vtxp);
                    concatp->lhsp(b.m_vtxp);
                    a.m_vtxp = concatp;
                    b.m_vtxp = nullptr;  // Mark as moved
                    ++m_ctx.m_coalescedAssignments;
                    continue;
                }

                ++i;

                // Compact non-adjacent ranges within the vector
                if (j != i) {
                    Driver& c = drivers[i];
                    UASSERT_OBJ(!c.m_vtxp, c.m_fileline, "Should have been marked moved");
                    c = b;
                    b.m_vtxp = nullptr;  // Mark as moved
                }
            }

            // Reinsert drivers in order
            splicep->resetSources();
            for (const Driver& driver : drivers) {
                if (!driver.m_vtxp) break;  // Stop at end of compacted list
                splicep->addDriver(driver.m_fileline, driver.m_lsb, driver.m_vtxp);
            }

            // Prune vertices potentially unused due to resolving multiple drivers.
            while (!prune.empty()) {
                // Pop last vertex
                const auto it = prune.begin();
                DfgVertex* const vtxp = *it;
                prune.erase(it);
                // If used (or a variable), then done
                if (vtxp->hasSinks() || vtxp->is<DfgVertexVar>()) continue;
                // If unused, then add sources to work list and delete
                vtxp->forEachSource([&](DfgVertex& src) { prune.emplace(&src); });
                vtxp->unlinkDelete(*m_dfgp);
            }

            // If the whole variable is driven, remove the splice node
            if (splicep->arity() == 1  //
                && splicep->driverLsb(0) == 0  //
                && splicep->source(0)->width() == varp->width()) {
                varp->srcp(splicep->source(0));
                varp->driverFileLine(splicep->driverFileLine(0));
                splicep->unlinkDelete(*m_dfgp);
            }
        }
    }

    // Canonicalize array variables
    void canonicalizeArray() {
        for (DfgVarArray* const varp : m_varArrayps) {
            // Delete variables with no sinks nor sources (this can happen due to reverting
            // uncommitted vertices, which does not remove variables)
            if (!varp->hasSinks() && !varp->srcp()) {
                VL_DO_DANGLING(varp->unlinkDelete(*m_dfgp), varp);
            }
        }
    }

    // VISITORS
    void visit(AstNode* nodep) override {
        // Conservatively treat this node as unhandled
        if (!m_foundUnhandled && m_converting) ++m_ctx.m_nonRepUnknown;
        m_foundUnhandled = true;
        markReferenced(nodep);
    }

    void visit(AstNetlist* nodep) override { iterateAndNextNull(nodep->modulesp()); }
    void visit(AstModule* nodep) override { iterateAndNextNull(nodep->stmtsp()); }
    void visit(AstTopScope* nodep) override { iterate(nodep->scopep()); }
    void visit(AstScope* nodep) override { iterateChildren(nodep); }
    void visit(AstActive* nodep) override {
        if (nodep->hasCombo()) {
            iterateChildren(nodep);
        } else {
            markReferenced(nodep);
        }
    }

    void visit(AstCell* nodep) override { markReferenced(nodep); }
    void visit(AstNodeProcedure* nodep) override { markReferenced(nodep); }

    void visit(AstVar* nodep) override {
        if VL_CONSTEXPR_CXX17 (T_Scoped) {
            return;
        } else {
            if (nodep->isSc()) return;
            // No need to (and in fact cannot) handle variables with unsupported dtypes
            if (!DfgVertex::isSupportedDType(nodep->dtypep())) return;

            // Mark variables with external references
            if (nodep->isIO()  // Ports
                || nodep->user2()  // Target of a hierarchical reference
                || nodep->isForced()  // Forced
            ) {
                getNet(reinterpret_cast<VariableType*>(nodep))->setHasExtRefs();
            }
        }
    }

    void visit(AstVarScope* nodep) override {
        if VL_CONSTEXPR_CXX17 (!T_Scoped) {
            return;
        } else {
            if (nodep->varp()->isSc()) return;
            // No need to (and in fact cannot) handle variables with unsupported dtypes
            if (!DfgVertex::isSupportedDType(nodep->dtypep())) return;

            // Mark variables with external references
            if (nodep->varp()->isIO()  // Ports
                || nodep->user2()  // Target of a hierarchical reference
                || nodep->varp()->isForced()  // Forced
            ) {
                getNet(reinterpret_cast<VariableType*>(nodep))->setHasExtRefs();
            }
        }
    }

    void visit(AstAssignW* nodep) override {
        ++m_ctx.m_inputEquations;

        // Cannot handle assignment with timing control yet
        if (nodep->timingControlp()) {
            markReferenced(nodep);
            ++m_ctx.m_nonRepTiming;
            return;
        }

        convertEquation(nodep, nodep->fileline(), nodep->lhsp(), nodep->rhsp());
    }

    void visit(AstAlways* nodep) override {
        // Ignore sequential logic, or if there are multiple statements
        const VAlwaysKwd kwd = nodep->keyword();
        if (nodep->sensesp() || !nodep->isJustOneBodyStmt()
            || (kwd != VAlwaysKwd::ALWAYS && kwd != VAlwaysKwd::ALWAYS_COMB)) {
            markReferenced(nodep);
            return;
        }

        AstNode* const stmtp = nodep->stmtsp();

        if (AstAssign* const assignp = VN_CAST(stmtp, Assign)) {
            ++m_ctx.m_inputEquations;
            if (assignp->timingControlp()) {
                markReferenced(stmtp);
                ++m_ctx.m_nonRepTiming;
                return;
            }
            convertEquation(nodep, assignp->fileline(), assignp->lhsp(), assignp->rhsp());
        } else if (AstIf* const ifp = VN_CAST(stmtp, If)) {
            // Will only handle single assignments to the same LHS in both branches
            AstAssign* const thenp = VN_CAST(ifp->thensp(), Assign);
            AstAssign* const elsep = VN_CAST(ifp->elsesp(), Assign);
            if (!thenp || !elsep || thenp->nextp() || elsep->nextp()
                || !thenp->lhsp()->sameTree(elsep->lhsp())) {
                markReferenced(stmtp);
                return;
            }

            ++m_ctx.m_inputEquations;
            if (thenp->timingControlp() || elsep->timingControlp()) {
                markReferenced(stmtp);
                ++m_ctx.m_nonRepTiming;
                return;
            }

            // Create a conditional for the rhs by borrowing the components from the AstIf
            AstCond* const rhsp = new AstCond{ifp->fileline(),  //
                                              ifp->condp()->unlinkFrBack(),  //
                                              thenp->rhsp()->unlinkFrBack(),  //
                                              elsep->rhsp()->unlinkFrBack()};

            if (!convertEquation(nodep, ifp->fileline(), thenp->lhsp(), rhsp)) {
                // Failed to convert. Mark 'rhsp', as 'convertEquation' only marks 'nodep'.
                markReferenced(rhsp);
                // Put the AstIf back together
                ifp->condp(rhsp->condp()->unlinkFrBack());
                thenp->rhsp(rhsp->thenp()->unlinkFrBack());
                elsep->rhsp(rhsp->elsep()->unlinkFrBack());
            }
            // Delete the auxiliary conditional
            VL_DO_DANGLING(rhsp->deleteTree(), rhsp);
        } else {
            markReferenced(stmtp);
        }
    }

    void visit(AstVarRef* nodep) override {
        UASSERT_OBJ(!nodep->user1p(), nodep, "Already has Dfg vertex");
        if (unhandled(nodep)) return;

        if (nodep->access().isRW()  // Cannot represent read-write references
            || nodep->varp()->isIfaceRef()  // Cannot handle interface references
            || nodep->varp()->delayp()  // Cannot handle delayed variables
            || nodep->classOrPackagep()  // Cannot represent cross module references
        ) {
            markReferenced(nodep);
            m_foundUnhandled = true;
            ++m_ctx.m_nonRepVarRef;
            return;
        }

        // If the referenced variable is not in a regular module, then do not
        // convert it. This is especially needed for variabels in interfaces
        // which might be referenced via virtual intefaces, which cannot be
        // resovled statically.
        if (T_Scoped && !VN_IS(nodep->varScopep()->scopep()->modp(), Module)) {
            markReferenced(nodep);
            m_foundUnhandled = true;
            ++m_ctx.m_nonRepVarRef;
            return;
        }

        // Sadly sometimes AstVarRef does not have the same dtype as the referenced variable
        if (!DfgVertex::isSupportedDType(nodep->varp()->dtypep())) {
            m_foundUnhandled = true;
            ++m_ctx.m_nonRepVarRef;
            return;
        }

        nodep->user1p(getNet(getTarget(nodep)));
    }

    void visit(AstConst* nodep) override {
        UASSERT_OBJ(!nodep->user1p(), nodep, "Already has Dfg vertex");
        if (unhandled(nodep)) return;
        DfgVertex* const vtxp = new DfgConst{*m_dfgp, nodep->fileline(), nodep->num()};
        m_uncommittedVertices.push_back(vtxp);
        nodep->user1p(vtxp);
    }

    void visit(AstSel* nodep) override {
        UASSERT_OBJ(!nodep->user1p(), nodep, "Already has Dfg vertex");
        if (unhandled(nodep)) return;

        iterate(nodep->fromp());
        if (m_foundUnhandled) return;

        FileLine* const flp = nodep->fileline();
        DfgVertex* vtxp = nullptr;
        if (AstConst* const constp = VN_CAST(nodep->lsbp(), Const)) {
            DfgSel* const selp = new DfgSel{*m_dfgp, flp, DfgVertex::dtypeFor(nodep)};
            selp->fromp(nodep->fromp()->user1u().to<DfgVertex*>());
            selp->lsb(constp->toUInt());
            vtxp = selp;
        } else {
            iterate(nodep->lsbp());
            if (m_foundUnhandled) return;
            DfgMux* const muxp = new DfgMux{*m_dfgp, flp, DfgVertex::dtypeFor(nodep)};
            muxp->fromp(nodep->fromp()->user1u().to<DfgVertex*>());
            muxp->lsbp(nodep->lsbp()->user1u().to<DfgVertex*>());
            vtxp = muxp;
        }
        m_uncommittedVertices.push_back(vtxp);
        nodep->user1p(vtxp);
    }

// The rest of the 'visit' methods are generated by 'astgen'
#include "V3Dfg__gen_ast_to_dfg.h"

    static DfgGraph* makeDfg(RootType& root) {
        if VL_CONSTEXPR_CXX17 (T_Scoped) {
            return new DfgGraph{nullptr, "netlist"};
        } else {
            AstModule* const modp = VN_AS((AstNode*)&(root), Module);  // Remove this when C++17
            return new DfgGraph{modp, modp->name()};
        }
    }

    // CONSTRUCTOR
    explicit AstToDfgVisitor(RootType& root, V3DfgOptimizationContext& ctx)
        : m_dfgp{makeDfg(root)}
        , m_ctx{ctx} {
        // Build the DFG
        iterate(&root);
        UASSERT_OBJ(m_uncommittedVertices.empty(), &root, "Uncommitted vertices remain");

        // Canonicalize variables
        canonicalizePacked();
        canonicalizeArray();
    }

public:
    static DfgGraph* apply(RootType& root, V3DfgOptimizationContext& ctx) {
        return AstToDfgVisitor{root, ctx}.m_dfgp;
    }
};

DfgGraph* V3DfgPasses::astToDfg(AstModule& module, V3DfgOptimizationContext& ctx) {
    return AstToDfgVisitor</* T_Scoped: */ false>::apply(module, ctx);
}

DfgGraph* V3DfgPasses::astToDfg(AstNetlist& netlist, V3DfgOptimizationContext& ctx) {
    return AstToDfgVisitor</* T_Scoped: */ true>::apply(netlist, ctx);
}
