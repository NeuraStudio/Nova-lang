// SafetyAnalyzer.cpp — Nova Safe Mode (Ownership, Borrowing & Basic Lifetimes)
#include "SafetyAnalyzer.hpp"

namespace nova {

SafetyAnalyzer::SafetyAnalyzer(SemanticAnalyzer& semantic, SemErrorSystem& errors)
    : semantic(semantic), errors(errors) {}

bool SafetyAnalyzer::isPrimitive(const std::string& typeName) const {
    return primitiveTypes.count(typeName) > 0 || typeName.empty();
}

void SafetyAnalyzer::pushScope() {
    scopes.emplace_back();
}

void SafetyAnalyzer::popScope() {
    if (scopes.empty()) return;

    ScopeFrame& frame = scopes.back();
    // References declared in this lexical scope cease to be live here. Release
    // their borrows before the source bindings themselves disappear.
    for (auto it = frame.ownedBindings.rbegin(); it != frame.ownedBindings.rend(); ++it) {
        releaseBorrow(*it);
    }

    for (Binding* binding : frame.ownedBindings) {
        if (!binding) continue;
        if (binding->state == VarState::Borrowed &&
            binding->borrows.immutableCount == 0 &&
            binding->borrows.mutableBorrow == nullptr) {
            binding->state = VarState::Valid;
        }
    }

    scopes.pop_back();
}

SafetyAnalyzer::Binding* SafetyAnalyzer::declareBinding(
    const std::string& name, bool isMutable, bool isParameter, int line, int col) {
    if (scopes.empty()) pushScope();

    auto binding = std::make_unique<Binding>();
    binding->name = name;
    binding->isMutable = isMutable;
    binding->isParameter = isParameter;
    binding->scopeDepth = static_cast<int>(scopes.size()) - 1;
    binding->declLine = line;
    binding->declCol = col;

    Binding* raw = binding.get();
    bindings.push_back(std::move(binding));

    scopes.back().bindings[name] = raw;
    scopes.back().ownedBindings.push_back(raw);
    varStates[name] = raw->state;
    return raw;
}

SafetyAnalyzer::Binding* SafetyAnalyzer::lookupBinding(const std::string& name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto found = it->bindings.find(name);
        if (found != it->bindings.end()) return found->second;
    }
    return nullptr;
}

bool SafetyAnalyzer::isLiveBinding(const Binding* binding) const {
    if (!binding) return false;
    for (const auto& frame : scopes) {
        for (Binding* candidate : frame.ownedBindings) {
            if (candidate == binding) return true;
        }
    }
    return false;
}

void SafetyAnalyzer::enterFunction(const FunctionDecl* fn) {
    functions.push_back({fn ? fn->name : "<function>", static_cast<int>(scopes.size())});
    pushScope();

    if (!fn) return;
    for (const auto& param : fn->params) {
        declareBinding(param.name, true, true, fn->line, fn->col);
    }
}

void SafetyAnalyzer::leaveFunction() {
    while (!scopes.empty() && !functions.empty() &&
           static_cast<int>(scopes.size()) > functions.back().depth) {
        popScope();
    }
    if (!functions.empty()) functions.pop_back();
}

bool SafetyAnalyzer::isBorrowOp(const UnaryExpr* unary) const {
    return unary && (unary->op == "&" || unary->op == "&mut");
}

bool SafetyAnalyzer::isMutableBorrowOp(const UnaryExpr* unary) const {
    return unary && unary->op == "&mut";
}

bool SafetyAnalyzer::validateIdentifierUse(const IdentifierExpr* id) {
    Binding* binding = lookupBinding(id->name);
    if (!binding) return true; // SemanticAnalyzer owns unknown-name diagnostics.

    if (binding->state == VarState::Moved) {
        errors.error(id->line, id->col, "E4001",
                     "Use of moved value: '" + id->name + "'. Ownership was transferred.");
        return false;
    }

    if (binding->isReference && binding->referent && !isLiveBinding(binding->referent)) {
        errors.error(id->line, id->col, "E4004",
                     "Dangling reference: '" + id->name +
                     "' refers to a value whose lifetime has ended.");
        return false;
    }

    return true;
}

bool SafetyAnalyzer::validateMutation(Binding* binding, int line, int col,
                                      const std::string& operation) {
    if (!binding) return true;

    if (binding->state == VarState::Moved) {
        errors.error(line, col, "E4001",
                     "Use of moved value: '" + binding->name + "'. Ownership was transferred.");
        return false;
    }

    if (binding->borrows.immutableCount > 0 || binding->borrows.mutableBorrow != nullptr) {
        errors.error(line, col, "E4003",
                     "cannot " + operation + " '" + binding->name +
                     "' while it is borrowed");
        return false;
    }

    if (!binding->isMutable) {
        errors.error(line, col, "E4003",
                     "cannot " + operation + " immutable binding '" + binding->name + "'");
        return false;
    }

    return true;
}

void SafetyAnalyzer::acquireImmutableBorrow(Binding* referent, Binding* owner,
                                            int line, int col) {
    if (!referent || !owner) return;

    if (referent->state == VarState::Moved) {
        errors.error(line, col, "E4001",
                     "cannot borrow moved value '" + referent->name + "'");
        return;
    }

    if (referent->borrows.mutableBorrow != nullptr) {
        errors.error(line, col, "E4003",
                     "cannot immutably borrow '" + referent->name +
                     "' because it is already mutably borrowed");
        return;
    }

    ++referent->borrows.immutableCount;
    referent->state = VarState::Borrowed;
    owner->isReference = true;
    owner->referenceMutable = false;
    owner->referent = referent;
}

void SafetyAnalyzer::acquireMutableBorrow(Binding* referent, Binding* owner,
                                          int line, int col) {
    if (!referent || !owner) return;

    if (referent->state == VarState::Moved) {
        errors.error(line, col, "E4001",
                     "cannot borrow moved value '" + referent->name + "'");
        return;
    }

    if (referent->borrows.mutableBorrow != nullptr ||
        referent->borrows.immutableCount > 0) {
        std::string reason;
        if (referent->borrows.mutableBorrow != nullptr) {
            reason = "it is already mutably borrowed";
        } else {
            reason = "it already has " +
                     std::to_string(referent->borrows.immutableCount) +
                     " active immutable borrow" +
                     (referent->borrows.immutableCount == 1 ? "" : "s");
        }
        errors.error(line, col, "E4003",
                     "cannot mutably borrow '" + referent->name + "' because " + reason);
        return;
    }

    if (!referent->isMutable) {
        errors.error(line, col, "E4003",
                     "cannot mutably borrow immutable binding '" + referent->name + "'");
        return;
    }

    referent->borrows.mutableBorrow = owner;
    referent->state = VarState::Borrowed;
    owner->isReference = true;
    owner->referenceMutable = true;
    owner->referent = referent;
}

void SafetyAnalyzer::releaseBorrow(Binding* owner) {
    if (!owner || !owner->isReference || !owner->referent) return;

    Binding* referent = owner->referent;
    if (owner->referenceMutable) {
        if (referent->borrows.mutableBorrow == owner) {
            referent->borrows.mutableBorrow = nullptr;
        }
    } else if (referent->borrows.immutableCount > 0) {
        --referent->borrows.immutableCount;
    }

    if (referent->borrows.immutableCount == 0 &&
        referent->borrows.mutableBorrow == nullptr &&
        referent->state == VarState::Borrowed) {
        referent->state = VarState::Valid;
        varStates[referent->name] = VarState::Valid;
    }

    owner->referent = nullptr;
    owner->isReference = false;
}

bool SafetyAnalyzer::resolveReferenceInitializer(const Expr* expr,
                                                 Binding*& referent,
                                                 bool& mutableReference) {
    referent = nullptr;
    mutableReference = false;

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
        if (!isBorrowOp(unary)) return false;
        const auto* id = dynamic_cast<const IdentifierExpr*>(unary->operand.get());
        if (!id) return false;
        referent = lookupBinding(id->name);
        mutableReference = isMutableBorrowOp(unary);
        return referent != nullptr;
    }

    // Copying a reference extends an alias to the same referent for this basic
    // checker. Its lifetime is still tied to the destination binding's scope.
    if (const auto* id = dynamic_cast<const IdentifierExpr*>(expr)) {
        Binding* source = lookupBinding(id->name);
        if (source && source->isReference) {
            referent = source->referent;
            mutableReference = source->referenceMutable;
            return referent != nullptr;
        }
    }

    return false;
}

bool SafetyAnalyzer::validateBorrow(const UnaryExpr* unary) {
    if (!unary || !isBorrowOp(unary)) return true;

    const auto* id = dynamic_cast<const IdentifierExpr*>(unary->operand.get());
    if (!id) {
        // General lvalues (fields/indexes) need place-expression tracking that
        // the current AST does not expose as stable storage identities. Do not
        // pretend they are ordinary variables; still validate their operands.
        checkExpr(unary->operand.get());
        return true;
    }

    Binding* referent = lookupBinding(id->name);
    if (!referent) return true;

    if (referent->state == VarState::Moved) {
        errors.error(unary->line, unary->col, "E4001",
                     "cannot borrow moved value '" + referent->name + "'");
        return false;
    }

    // A bare borrow expression is only useful if it is captured by a declaration
    // or another reference-producing expression. For a standalone `&x`, keep
    // the borrow through the statement only; this avoids leaking the borrow.
    return true;
}

bool SafetyAnalyzer::validateAssignmentTarget(const Expr* target) {
    if (const auto* id = dynamic_cast<const IdentifierExpr*>(target)) {
        Binding* binding = lookupBinding(id->name);
        return validateMutation(binding, id->line, id->col, "assign to");
    }

    if (const auto* member = dynamic_cast<const MemberExpr*>(target)) {
        checkExpr(member->target.get(), true);
        return true;
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(target)) {
        checkExpr(index->target.get(), true);
        checkExpr(index->index.get());
        return true;
    }

    checkExpr(target, true);
    return true;
}

bool SafetyAnalyzer::referencesLocalToCurrentFunction(Binding* referent) const {
    if (!referent || functions.empty()) return false;

    const int functionScopeDepth = functions.back().depth;
    // Parameters live directly in the function scope and are valid to return.
    if (referent->isParameter) return false;

    // Anything declared inside the function's lexical scope tree is local.
    return referent->scopeDepth >= functionScopeDepth;
}

bool SafetyAnalyzer::checkReturnReference(const Expr* expr, int line, int col) {
    if (!expr || functions.empty()) return true;

    Binding* referent = nullptr;
    bool mutableReference = false;
    if (resolveReferenceInitializer(expr, referent, mutableReference) && referent) {
        if (referencesLocalToCurrentFunction(referent)) {
            errors.error(line, col, "E4004",
                         "dangling reference: returning a reference to local variable '" +
                         referent->name + "' which will go out of scope");
            return false;
        }
        if (!isLiveBinding(referent)) {
            errors.error(line, col, "E4004",
                         "dangling reference: returned reference refers to a value whose lifetime has ended");
            return false;
        }
    }
    return true;
}

void SafetyAnalyzer::checkExpr(const Expr* expr, bool isAssignmentTarget) {
    if (!expr) return;

    if (const auto* id = dynamic_cast<const IdentifierExpr*>(expr)) {
        validateIdentifierUse(id);
        return;
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
        if (isBorrowOp(unary)) {
            validateBorrow(unary);
        } else {
            checkExpr(unary->operand.get(), unary->op == "++" || unary->op == "--");
        }
        return;
    }

    if (const auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        checkExpr(bin->left.get());
        checkExpr(bin->right.get());
        return;
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(expr)) {
        checkExpr(call->callee.get());
        for (const auto& arg : call->args) checkExpr(arg.get());
        return;
    }

    if (const auto* member = dynamic_cast<const MemberExpr*>(expr)) {
        checkExpr(member->target.get(), isAssignmentTarget);
        return;
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(expr)) {
        checkExpr(index->target.get(), isAssignmentTarget);
        checkExpr(index->index.get());
        return;
    }

    if (const auto* slice = dynamic_cast<const SliceExpr*>(expr)) {
        checkExpr(slice->target.get());
        checkExpr(slice->start.get());
        checkExpr(slice->end.get());
        checkExpr(slice->step.get());
        return;
    }

    if (const auto* ternary = dynamic_cast<const TernaryExpr*>(expr)) {
        checkExpr(ternary->cond.get());
        checkExpr(ternary->thenExpr.get());
        checkExpr(ternary->elseExpr.get());
        return;
    }

    if (const auto* elvis = dynamic_cast<const ElvisExpr*>(expr)) {
        checkExpr(elvis->left.get());
        checkExpr(elvis->fallback.get());
        return;
    }

    if (const auto* coalesce = dynamic_cast<const NullCoalesceExpr*>(expr)) {
        checkExpr(coalesce->left.get());
        checkExpr(coalesce->fallback.get());
        return;
    }

    if (const auto* range = dynamic_cast<const RangeExpr*>(expr)) {
        checkExpr(range->from.get());
        checkExpr(range->to.get());
        checkExpr(range->step.get());
        return;
    }

    if (const auto* lambda = dynamic_cast<const LambdaExpr*>(expr)) {
        // Lambda captures are outside this basic lifetime model. Analyze the
        // lambda body in an isolated lexical scope so local borrows cannot leak
        // into the surrounding function's scope state.
        pushScope();
        for (const auto& p : lambda->params) declareBinding(p, true, true, lambda->line, lambda->col);
        if (lambda->bodyExpr) checkExpr(lambda->bodyExpr.get());
        if (lambda->bodyBlock) checkBlock(lambda->bodyBlock.get(), true);
        popScope();
        return;
    }

    if (const auto* spread = dynamic_cast<const SpreadExpr*>(expr)) {
        checkExpr(spread->target.get());
        return;
    }

    if (const auto* array = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
        for (const auto& e : array->elements) checkExpr(e.get());
        return;
    }

    if (const auto* map = dynamic_cast<const MapLiteralExpr*>(expr)) {
        for (const auto& e : map->entries) {
            checkExpr(e.key.get());
            checkExpr(e.value.get());
        }
        return;
    }

    if (const auto* dict = dynamic_cast<const DictComprehensionExpr*>(expr)) {
        checkExpr(dict->iterable.get());
        pushScope();
        for (const auto& v : dict->loopVars) declareBinding(v, true, false, dict->line, dict->col);
        checkExpr(dict->keyExpr.get());
        checkExpr(dict->valueExpr.get());
        checkExpr(dict->condition.get());
        popScope();
        return;
    }

    if (const auto* tuple = dynamic_cast<const TupleLiteralExpr*>(expr)) {
        for (const auto& e : tuple->elements) checkExpr(e.get());
        return;
    }

    if (const auto* match = dynamic_cast<const MatchExpr*>(expr)) {
        checkExpr(match->subject.get());
        for (const auto& c : match->cases) {
            checkExpr(c.pattern.get());
            checkStmt(c.body.get());
        }
        checkStmt(match->defaultBody.get());
        return;
    }

    if (const auto* cast = dynamic_cast<const AsCastExpr*>(expr)) {
        checkExpr(cast->target.get());
        return;
    }

    if (const auto* exists = dynamic_cast<const ExistsExpr*>(expr)) {
        checkExpr(exists->target.get());
        return;
    }
}

void SafetyAnalyzer::checkBlock(const Block* block, bool createScope) {
    if (!block) return;
    if (createScope) pushScope();
    for (const auto& stmt : block->statements) checkStmt(stmt.get());
    if (createScope) popScope();
}

void SafetyAnalyzer::checkStmt(const Stmt* stmt) {
    if (!stmt) return;

    if (const auto* decl = dynamic_cast<const DeclarationStmt*>(stmt)) {
        // Initializer is evaluated before the new binding enters scope.
        if (decl->value) {
            if (const auto* unary = dynamic_cast<const UnaryExpr*>(decl->value.get());
                unary && isBorrowOp(unary)) {
                const auto* id = dynamic_cast<const IdentifierExpr*>(unary->operand.get());
                Binding* referent = id ? lookupBinding(id->name) : nullptr;
                Binding* owner = declareBinding(decl->name, decl->isMut, false,
                                                decl->line, decl->col);
                if (referent) {
                    if (isMutableBorrowOp(unary))
                        acquireMutableBorrow(referent, owner, unary->line, unary->col);
                    else
                        acquireImmutableBorrow(referent, owner, unary->line, unary->col);
                } else {
                    checkExpr(decl->value.get());
                }
            } else {
                checkExpr(decl->value.get());
                Binding* owner = declareBinding(decl->name, decl->isMut, false,
                                                decl->line, decl->col);

                Binding* referent = nullptr;
                bool mutableReference = false;
                if (resolveReferenceInitializer(decl->value.get(), referent, mutableReference) && referent) {
                    if (mutableReference)
                        acquireMutableBorrow(referent, owner, decl->value->line, decl->value->col);
                    else
                        acquireImmutableBorrow(referent, owner, decl->value->line, decl->value->col);
                } else if (const auto* rhsId = dynamic_cast<const IdentifierExpr*>(decl->value.get())) {
                    Binding* source = lookupBinding(rhsId->name);
                    if (source && !source->isReference) {
                        // Preserve the original ownership behavior. Primitive
                        // detection remains conservative when type inference is
                        // unavailable here.
                        if (source->state != VarState::Moved &&
                            source->borrows.immutableCount == 0 &&
                            source->borrows.mutableBorrow == nullptr) {
                            source->state = VarState::Moved;
                            varStates[source->name] = VarState::Moved;
                        }
                    }
                }
            }
        } else {
            declareBinding(decl->name, decl->isMut, false, decl->line, decl->col);
        }
        return;
    }

    if (const auto* asg = dynamic_cast<const AssignmentStmt*>(stmt)) {
        // RHS is evaluated before the target is mutated, matching normal
        // expression semantics and preventing accidental self-conflicts.
        checkExpr(asg->value.get());

        if (asg->op == "=") {
            if (const auto* rhsId = dynamic_cast<const IdentifierExpr*>(asg->value.get())) {
                Binding* source = lookupBinding(rhsId->name);
                if (source && !source->isReference && source->state != VarState::Moved) {
                    if (source->borrows.immutableCount == 0 && source->borrows.mutableBorrow == nullptr) {
                        source->state = VarState::Moved;
                        varStates[source->name] = VarState::Moved;
                    }
                }
            }
        }

        validateAssignmentTarget(asg->target.get());
        return;
    }

    if (const auto* destruct = dynamic_cast<const DestructuringStmt*>(stmt)) {
        checkExpr(destruct->value.get());
        for (const auto& name : destruct->targets) {
            declareBinding(name, true, false, destruct->line, destruct->col);
        }
        return;
    }

    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(stmt)) {
        // A standalone borrow lasts only for the statement. Since validateBorrow
        // does not acquire it, there is intentionally no persistent state here.
        checkExpr(exprStmt->expr.get());
        return;
    }

    if (const auto* ifs = dynamic_cast<const IfStmt*>(stmt)) {
        checkExpr(ifs->condition.get());
        checkBlock(ifs->thenBranch.get(), true);
        checkBlock(ifs->elseBranch.get(), true);
        if (ifs->elseIf) checkStmt(ifs->elseIf.get());
        return;
    }

    if (const auto* sw = dynamic_cast<const SwitchStmt*>(stmt)) {
        checkExpr(sw->subject.get());
        for (const auto& c : sw->cases) {
            pushScope();
            checkExpr(c.value.get());
            for (const auto& s : c.body) checkStmt(s.get());
            popScope();
        }
        pushScope();
        for (const auto& s : sw->defaultBody) checkStmt(s.get());
        popScope();
        return;
    }

    if (const auto* fr = dynamic_cast<const ForStmt*>(stmt)) {
        checkExpr(fr->from.get());
        checkExpr(fr->to.get());
        checkExpr(fr->step.get());
        pushScope();
        declareBinding(fr->varName, true, false, fr->line, fr->col);
        checkBlock(fr->body.get(), true);
        popScope();
        return;
    }

    if (const auto* wh = dynamic_cast<const WhileStmt*>(stmt)) {
        checkExpr(wh->condition.get());
        checkBlock(wh->body.get(), true);
        return;
    }

    if (const auto* rep = dynamic_cast<const RepeatStmt*>(stmt)) {
        checkExpr(rep->count.get());
        checkBlock(rep->body.get(), true);
        return;
    }

    if (const auto* fe = dynamic_cast<const ForeachStmt*>(stmt)) {
        checkExpr(fe->iterable.get());
        pushScope();
        declareBinding(fe->varName, true, false, fe->line, fe->col);
        checkBlock(fe->body.get(), true);
        popScope();
        return;
    }

    if (const auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
        if (ret->value) {
            checkExpr(ret->value.get());
            checkReturnReference(ret->value.get(), ret->line, ret->col);
        }
        return;
    }

    if (const auto* yield = dynamic_cast<const YieldStmt*>(stmt)) {
        checkExpr(yield->value.get());
        return;
    }

    if (const auto* fn = dynamic_cast<const FunctionDecl*>(stmt)) {
        // Function bodies have independent ownership/lifetime environments.
        enterFunction(fn);
        checkBlock(fn->body.get(), true);
        leaveFunction();
        return;
    }

    if (const auto* cls = dynamic_cast<const ClassDecl*>(stmt)) {
        pushScope();
        for (const auto& member : cls->members) {
            if (member.fieldDefault) checkExpr(member.fieldDefault.get());
            if (member.method) checkStmt(member.method.get());
        }
        popScope();
        return;
    }

    if (const auto* mod = dynamic_cast<const ModuleDecl*>(stmt)) {
        checkBlock(mod->body.get(), true);
        return;
    }

    if (const auto* tr = dynamic_cast<const TryStmt*>(stmt)) {
        checkBlock(tr->tryBlock.get(), true);
        pushScope();
        if (!tr->catchVar.empty()) declareBinding(tr->catchVar, true, false, tr->line, tr->col);
        checkBlock(tr->catchBlock.get(), true);
        popScope();
        checkBlock(tr->finallyBlock.get(), true);
        return;
    }

    if (const auto* th = dynamic_cast<const ThreadStmt*>(stmt)) {
        checkBlock(th->body.get(), true);
        return;
    }

    if (const auto* ev = dynamic_cast<const EventStmt*>(stmt)) {
        checkBlock(ev->body.get(), true);
        return;
    }

    if (const auto* us = dynamic_cast<const UnsafeStmt*>(stmt)) {
        checkBlock(us->body.get(), true);
        return;
    }

    if (const auto* sig = dynamic_cast<const SignalDecl*>(stmt)) {
        checkExpr(sig->initial.get());
        declareBinding(sig->name, true, false, sig->line, sig->col);
        return;
    }

    if (const auto* usingStmt = dynamic_cast<const UsingStmt*>(stmt)) {
        checkExpr(usingStmt->resource.get());
        pushScope();
        declareBinding(usingStmt->varName, true, false, usingStmt->line, usingStmt->col);
        checkBlock(usingStmt->body.get(), true);
        popScope();
        return;
    }

    if (const auto* guard = dynamic_cast<const GuardStmt*>(stmt)) {
        checkExpr(guard->condition.get());
        checkBlock(guard->elseBlock.get(), true);
        return;
    }

    if (const auto* lazy = dynamic_cast<const LazyDecl*>(stmt)) {
        checkExpr(lazy->initializer.get());
        declareBinding(lazy->name, true, false, lazy->line, lazy->col);
        return;
    }

    if (const auto* comptime = dynamic_cast<const ComptimeStmt*>(stmt)) {
        checkBlock(comptime->body.get(), true);
        return;
    }

    if (const auto* macro = dynamic_cast<const MacroDecl*>(stmt)) {
        pushScope();
        for (const auto& p : macro->params) declareBinding(p, true, true, macro->line, macro->col);
        checkBlock(macro->body.get(), true);
        popScope();
        return;
    }

    if (const auto* annotated = dynamic_cast<const AnnotatedStmt*>(stmt)) {
        for (const auto& ann : annotated->annotations)
            for (const auto& arg : ann.args) checkExpr(arg.get());
        checkStmt(annotated->inner.get());
        return;
    }
}

bool SafetyAnalyzer::analyze(const Program& program) {
    scopes.clear();
    bindings.clear();
    functions.clear();
    varStates.clear();

    pushScope(); // program/global lexical scope
    for (const auto& stmt : program.statements) checkStmt(stmt.get());
    popScope();

    return !errors.hasErrors();
}

} // namespace nova
