/*
 * Copyright 2004-2017 Cray Inc.
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
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

#include "passes.h"

#include "addAutoDestroyCalls.h"
#include "astutil.h"
#include "stringutil.h"
#include "expr.h"
#include "resolution.h"
#include "resolveIntents.h"
#include "stlUtil.h"
#include "stmt.h"
#include "symbol.h"

/************************************* | **************************************
*                                                                             *
* Noakes 2015/11/23                                                           *
*                                                                             *
* The transformation beginning at returnRecordsByReferenceArguments() locates *
* a subset of the calls to functions that return a record-like type by value  *
* and creates a clone of the function that                                    *
*                                                                             *
*   a) adds a formal with ref-intent                                          *
*   b) has return type void                                                   *
*   c) integrates a small amount of code past the call site in to the tail    *
*      of the cloned function.                                                *
*                                                                             *
* and then adjusts the neighborhood near the call-site appropriately.         *
*                                                                             *
* It has been determined that this transformation should be applied uniformly *
* to record-like types.  This new implementation follows the theme of the     *
* the current transformation but                                              *
*                                                                             *
*   1) Modifies the function rather than creating a clone                     *
*   2) Does not fold any other code in to the tail                            *
*                                                                             *
* This implementation should be broadly applicable to record-like types.      *
*                                                                             *
************************************** | *************************************/

//
// Capture a function and all of the calls to it
//
class ReturnByRef
{
  //
  // Class interface
  //
public:
  static void             apply();

private:
  typedef std::map<int, ReturnByRef*> RefMap;

  static void             returnByRefCollectCalls(RefMap& calls);
  static FnSymbol*        theTransformableFunction(CallExpr* call);
  static bool             isTransformableFunction(FnSymbol* fn);
  static void             transformFunction(FnSymbol* fn);
  static ArgSymbol*       addFormal(FnSymbol* fn);
  static void             insertAssignmentToFormal(FnSymbol*  fn,
                                                   ArgSymbol* formal);
  static void             updateAssignmentsFromRefArgToValue(FnSymbol* fn);
  static void             updateAssignmentsFromRefTypeToValue(FnSymbol* fn);
  static void             updateAssignmentsFromModuleLevelValue(FnSymbol* fn);
  static void             updateReturnStatement(FnSymbol* fn);
  static void             updateReturnType(FnSymbol* fn);

  //
  // Instance interface
  //
private:

                          ReturnByRef(FnSymbol* fn);
                          ReturnByRef();

  void                    addCall(CallExpr* call);

  void                    transform();
  void                    transformMove(CallExpr* moveExpr);

  FnSymbol*               mFunction;
  std::vector<CallExpr*>  mCalls;
};

void ReturnByRef::apply()
{
  RefMap           map;
  RefMap::iterator iter;

  returnByRefCollectCalls(map);

  for (iter = map.begin(); iter != map.end(); iter++)
    iter->second->transform();

  for (int i = 0; i < virtualMethodTable.n; i++)
  {
    if (virtualMethodTable.v[i].key)
    {
      int  numFns = virtualMethodTable.v[i].value->n;

      for (int j = 0; j < numFns; j++)
      {
        FnSymbol* fn = virtualMethodTable.v[i].value->v[j];

        if (isTransformableFunction(fn))
          transformFunction(fn);
      }
    }
  }
}

//
// Collect functions that should be converted to return by ref
// and all calls to these functions.
//

void ReturnByRef::returnByRefCollectCalls(RefMap& calls)
{
  RefMap::iterator iter;

  forv_Vec(CallExpr, call, gCallExprs)
  {
    if (FnSymbol* fn = theTransformableFunction(call))
    {
      RefMap::iterator iter = calls.find(fn->id);
      ReturnByRef*     info = NULL;

      if (iter == calls.end())
      {
        info          = new ReturnByRef(fn);
        calls[fn->id] = info;
      }
      else
      {
        info          = iter->second;
      }

      info->addCall(call);
    }
  }
}

FnSymbol* ReturnByRef::theTransformableFunction(CallExpr* call)
{
  // The common case of a user-level call to a resolved function
  FnSymbol* theCall = call->isResolved();

  // Also handle the PRIMOP for a virtual method call
  if (theCall == NULL)
  {
    if (call->isPrimitive(PRIM_VIRTUAL_METHOD_CALL) == true)
    {
      SymExpr* arg1 = toSymExpr(call->get(1));

      theCall = toFnSymbol(arg1->symbol());
    }
  }

  return (theCall && isTransformableFunction(theCall)) ? theCall : NULL;
}

//
// In this first effort, only functions that return strings
//

bool ReturnByRef::isTransformableFunction(FnSymbol* fn)
{
  bool retval = false;

  if (AggregateType* type = toAggregateType(fn->retType))
  {
    if      (fn->hasFlag(FLAG_INIT_COPY_FN) == true)
      retval = false;

    else if (fn->hasFlag(FLAG_AUTO_COPY_FN) == true)
      retval = false;

    // Function is an iterator "helper"
    else if (fn->hasFlag(FLAG_AUTO_II)      == true)
      retval = false;

    // Can't transform extern functions
    else if (fn->hasFlag(FLAG_EXTERN)       == true)
      retval = false;

    // Noakes: 2016/02/24.  Only "user defined records" for now
    else if (isUserDefinedRecord(type)      == true)
      retval = true;

    else
      retval = false;
  }

  return retval;
}

void ReturnByRef::transformFunction(FnSymbol* fn)
{
  ArgSymbol* formal = addFormal(fn);

  insertAssignmentToFormal(fn, formal);
  updateAssignmentsFromRefArgToValue(fn);
  updateAssignmentsFromRefTypeToValue(fn);
  updateAssignmentsFromModuleLevelValue(fn);
  updateReturnStatement(fn);
  updateReturnType(fn);
}

ArgSymbol* ReturnByRef::addFormal(FnSymbol* fn)
{
  SET_LINENO(fn);

  Type*          type    = fn->retType;
  AggregateType* refType = type->refType;
  IntentTag      intent  = blankIntentForType(refType);
  // Note: other code does strcmps against the name _retArg
  ArgSymbol*     formal  = new ArgSymbol(intent, "_retArg", refType);
  formal->addFlag(FLAG_RETARG);

  fn->insertFormalAtTail(formal);
  fn->addFlag(FLAG_FN_RETARG);

  return formal;
}

void ReturnByRef::insertAssignmentToFormal(FnSymbol* fn, ArgSymbol* formal)
{
  Expr*     returnPrim  = fn->body->body.tail;

  SET_LINENO(returnPrim);

  CallExpr* returnCall  = toCallExpr(returnPrim);
  Expr*     returnValue = returnCall->get(1)->remove();
  CallExpr* moveExpr    = new CallExpr(PRIM_ASSIGN, formal, returnValue);


  Expr* expr = returnPrim;
  // Walk backwards while the previous element is an autoDestroy call
  while (expr->prev != NULL) {
    bool stop = true;
    if (CallExpr* call = toCallExpr(expr->prev))
      if (FnSymbol* calledFn = call->isResolved())
        if (calledFn->hasFlag(FLAG_AUTO_DESTROY_FN))
          stop = false;

    if (stop) break;

    expr = expr->prev;
  }

  Expr* returnOrFirstAutoDestroy = expr;

  // Add the move to return before the first autoDestroy
  // At this point we could also invoke some other function
  // if that turns out to be necessary. It might well be
  // necessary in order to return array slices by value.
  returnOrFirstAutoDestroy->insertBefore(moveExpr);
}

//
// Consider a function that takes a formal of type Record by const ref
// and that returns that value from the function.  The compiler inserts
// a PRIM_MOVE operation.
//
// This work-around inserts an autoCopy to compensate
//
void ReturnByRef::updateAssignmentsFromRefArgToValue(FnSymbol* fn)
{
  std::vector<CallExpr*> callExprs;

  collectCallExprs(fn, callExprs);

  for (size_t i = 0; i < callExprs.size(); i++)
  {
    CallExpr* move = callExprs[i];

    if (move->isPrimitive(PRIM_MOVE) == true)
    {
      SymExpr* lhs = toSymExpr(move->get(1));
      SymExpr* rhs = toSymExpr(move->get(2));

      if (lhs != NULL && rhs != NULL)
      {
        VarSymbol* symLhs = toVarSymbol(lhs->symbol());
        ArgSymbol* symRhs = toArgSymbol(rhs->symbol());

        if (symLhs != NULL && symRhs != NULL)
        {
          if (isUserDefinedRecord(symLhs->type) == true &&
              symRhs->type                      == symLhs->type)
          {
            if (symLhs->hasFlag(FLAG_ARG_THIS) == false &&
                (symRhs->intent == INTENT_REF ||
                 symRhs->intent == INTENT_CONST_REF))
            {
              SET_LINENO(move);

              CallExpr* autoCopy = NULL;

              rhs->remove();
              autoCopy = new CallExpr(getAutoCopyForType(symRhs->type), rhs);
              move->insertAtTail(autoCopy);
            }
          }
        }
      }
    }
  }
}

//
// Consider a function that assigns a class-ref wrapper for a record to
// a value of that record type.  The compiler represents this as a
//
//      move dst PRIM_DEREF(src)
//
// but fails to insert the required autoCopy.
//
// This transformation adds a move/autoCopy statement immediately after
// the targeted statement.  The <dst> symbol is updated in place in the
// new statement
//
//

void ReturnByRef::updateAssignmentsFromRefTypeToValue(FnSymbol* fn)
{
  std::vector<CallExpr*> callExprs;

  collectCallExprs(fn, callExprs);

  Map<Symbol*,Vec<SymExpr*>*> defMap;
  Map<Symbol*,Vec<SymExpr*>*> useMap;
  buildDefUseMaps(fn, defMap, useMap);

  for (size_t i = 0; i < callExprs.size(); i++)
  {
    CallExpr* move = callExprs[i];

    if (move->isPrimitive(PRIM_MOVE) == true)
    {
      SymExpr*  symLhs  = toSymExpr (move->get(1));
      CallExpr* callRhs = toCallExpr(move->get(2));

      if (symLhs && callRhs && callRhs->isPrimitive(PRIM_DEREF))
      {
        VarSymbol* varLhs = toVarSymbol(symLhs->symbol());
        SymExpr*   symRhs = toSymExpr(callRhs->get(1));
        VarSymbol* varRhs = toVarSymbol(symRhs->symbol());

        // MPF 2016-10-02: It seems to me that this code should also handle the
        // case that symRhs is an ArgSymbol, but adding that caused problems
        // in the handling of out argument intents.

        if (varLhs != NULL && varRhs != NULL)
        {
          if (isUserDefinedRecord(varLhs->type) == true &&
              varRhs->type                      == varLhs->type->refType)
          {

            // HARSHBARGER 2015-12-11:
            // `init_untyped_var` in the `normalize` pass may insert an
            // initCopy, which means that we should not insert an autocopy
            // for that same variable.
            bool initCopied = false;
            for_uses(use, useMap, varLhs) {
              if (CallExpr* call = toCallExpr(use->parentExpr)) {
                if (FnSymbol* parentFn = call->isResolved()) {
                  if (parentFn->hasFlag(FLAG_INIT_COPY_FN)) {
                    initCopied = true;
                    break;
                  }
                }
              }
            }

            if (!initCopied) {
              SET_LINENO(move);

              SymExpr*  lhsCopy0 = symLhs->copy();
              SymExpr*  lhsCopy1 = symLhs->copy();
              FnSymbol* autoCopy = getAutoCopyForType(varLhs->type);
              CallExpr* copyExpr = new CallExpr(autoCopy, lhsCopy0);
              CallExpr* moveExpr = new CallExpr(PRIM_MOVE,lhsCopy1, copyExpr);

              move->insertAfter(moveExpr);
            }
          }
        }
      }
    }
  }
}

//
// Consider a function that returns a module-level variable as the value
// for the function.  The compiler inserts a PRIM_MOVE operation.
//
// This work-around inserts an autoCopy to compensate
//
void ReturnByRef::updateAssignmentsFromModuleLevelValue(FnSymbol* fn)
{
  std::vector<CallExpr*> callExprs;

  collectCallExprs(fn, callExprs);

  for (size_t i = 0; i < callExprs.size(); i++)
  {
    CallExpr* move = callExprs[i];

    if (move->isPrimitive(PRIM_MOVE) == true)
    {
      SymExpr* lhs = toSymExpr(move->get(1));
      SymExpr* rhs = toSymExpr(move->get(2));

      if (lhs != NULL && rhs != NULL)
      {
        VarSymbol* symLhs = toVarSymbol(lhs->symbol());
        VarSymbol* symRhs = toVarSymbol(rhs->symbol());

        if (symLhs != NULL && symRhs != NULL)
        {
          if (isUserDefinedRecord(symLhs->type) == true &&
              symRhs->type                      == symLhs->type)
          {
            DefExpr* def = symRhs->defPoint;

            if (isModuleSymbol(def->parentSymbol) == true &&
                def->parentSymbol                 != rootModule)
            {
              SET_LINENO(move);

              CallExpr* autoCopy = NULL;

              rhs->remove();
              autoCopy = new CallExpr(getAutoCopyForType(symRhs->type), rhs);
              move->insertAtTail(autoCopy);
            }
          }
        }
      }
    }
  }
}

void ReturnByRef::updateReturnStatement(FnSymbol* fn)
{
  Expr* returnPrim  = fn->body->body.tail;

  SET_LINENO(returnPrim);

  returnPrim->replace(new CallExpr(PRIM_RETURN, gVoid));
}

void ReturnByRef::updateReturnType(FnSymbol* fn)
{
  fn->retType = dtVoid;
}

ReturnByRef::ReturnByRef(FnSymbol* fn)
{
  mFunction = fn;
}

void ReturnByRef::addCall(CallExpr* call)
{
  mCalls.push_back(call);
}

void ReturnByRef::transform()
{
  // Transform all of the call sites
  for (size_t i = 0; i < mCalls.size(); i++)
  {
    CallExpr* call   = mCalls[i];
    Expr*     parent = call->parentExpr;

    if (CallExpr* parentCall = toCallExpr(parent))
    {
      if (parentCall->isPrimitive(PRIM_MOVE))
      {
        transformMove(parentCall);
      }
      else
      {
        INT_ASSERT(false);
      }
    }
    else
    {
      FnSymbol * calledFn = call->isResolved();
      if (!calledFn->hasFlag(FLAG_NEW_ALIAS_FN))
        // fixupNewAlias removes some - but not all - calls
        // to the newAlias function from the tree.
        INT_ASSERT(false);
    }
  }

  // Then update the function
  transformFunction(mFunction);
}

//
// Transform a call to a function that returns a record to be a call
// to a revised function that does not return a value and that accepts
// a reference to the destination i.e.
//
// replace
//
//     move dst func(a, b, c)
//
// with
//
//     define ref;
//
//     ref = &dst;
//     func(a, b, c, ref);
//
// In some cases the statement after the move is another move
// with a RHS that performs a superfluous initCopy/autoCopy.
// If so reduce to a simple move.  The called-function is responsible
// for performing a copy when needed.
//

void ReturnByRef::transformMove(CallExpr* moveExpr)
{
  SET_LINENO(moveExpr);

  Expr*     lhs      = moveExpr->get(1);

  CallExpr* callExpr = toCallExpr(moveExpr->get(2));
  FnSymbol* fn       = callExpr->isResolved();

  Expr*     nextExpr = moveExpr->next;
  CallExpr* copyExpr = NULL;

  Symbol*   useLhs   = toSymExpr(lhs)->symbol();
  Symbol*   refVar   = newTemp("ret_to_arg_ref_tmp_", useLhs->type->refType);

  // Make sure that we created a temp with a type
  INT_ASSERT(useLhs->type->refType);

  // Determine if
  //   a) current call is not a PRIMOP
  //   a) current call is not to a constructor
  //   c) the subsequent statement is PRIM_MOVE for an initCopy/autoCopy
  //   d) the initCopy/autoCopy has the same argument and return type
  //      (this accounts for tuples containing refs)
  if (fn                            != NULL  &&
      fn->hasFlag(FLAG_CONSTRUCTOR) == false &&
      nextExpr                      != NULL)
  {
    if (CallExpr* callNext = toCallExpr(nextExpr))
    {
      if (callNext->isPrimitive(PRIM_MOVE) == true)
      {
        if (CallExpr* rhsCall = toCallExpr(callNext->get(2)))
        {
          FnSymbol* rhsFn = rhsCall->isResolved();

          if (rhsFn                              != NULL &&
              (rhsFn->hasFlag(FLAG_AUTO_COPY_FN) == true ||
               rhsFn->hasFlag(FLAG_INIT_COPY_FN) == true))
          {
            ArgSymbol* formalArg  = rhsFn->getFormal(1);
            Type*      formalType = formalArg->type;
            Type*      actualType = rhsCall->get(1)->getValType();
            Type*      returnType = rhsFn->retType->getValType();
            // Cannot reduce initCopy/autoCopy when types differ
            // Cannot reduce initCopy/autoCopy for sync variables
            if (actualType == returnType &&
                isSyncType(formalType) == false &&
                isSingleType(formalType) == false)
            {
              copyExpr = rhsCall;
            }
          }
        }
      }
    }
  }

  // Introduce a reference to the return value
  moveExpr->insertBefore(new DefExpr(refVar));
  moveExpr->insertBefore(new CallExpr(PRIM_MOVE,
                                      refVar,
                                      new CallExpr(PRIM_ADDR_OF, useLhs)));

  // Convert the by-value call to a void call with an additional formal
  moveExpr->replace(callExpr->remove());
  callExpr->insertAtTail(refVar);

  // Possibly reduce a copy operation to a simple move
  if (copyExpr) {
    FnSymbol* rhsFn = copyExpr->isResolved();

    copyExpr->replace(copyExpr->get(1)->remove());

    // But... if we're replacing an init copy, we got to a user
    // variable, so add an unalias call if there is one
    if (rhsFn->hasFlag(FLAG_INIT_COPY_FN)) {
      FnSymbol* unaliasFn = getUnalias(useLhs->type);
      if (unaliasFn) {
        callExpr->insertAfter(new CallExpr(unaliasFn, refVar));
      }
    }
  }
}

/************************************* | **************************************
*                                                                             *
* Code to implement original style of return record by ref formal             *
*                                                                             *
************************************** | *************************************/

//
// Cache to avoid cloning functions that return records if the copy
// of the returned argument is done in the same way as at another
// call site; the key into the cache is the old function, the values
// are stored in a vector based on the copy function (copy function
// 1, new function 1, copy function 2, new function 2, ...)
//
static Map<FnSymbol*,Vec<FnSymbol*>*> retToArgCache;

// Helper method for changeRetToArgAndClone, assisting in symbol replacement
//
// This method takes in the current call which we are replacing around
// (focalPt), the VarSymbol we are trying to replace (oldSym), the symbol we are
// replacing it with (newSym), and the function that was called in the first
// use of oldSym in the callee, to replace oldSym with newSym without breaking
// the AST.
inline static void
replacementHelper(CallExpr* focalPt, VarSymbol* oldSym, Symbol* newSym,
                  FnSymbol* useFn) {
  focalPt->insertAfter(new CallExpr(PRIM_ASSIGN, newSym,
                                    new CallExpr(useFn, oldSym)));
}


// Clone fn, add a ref arg to the end of the argument list, remove the return
// primitive and change the return type of the function to void.
// In the body of the clone, replace updates to the return value variable with
// calls to the useFn in the calling context.
//
// This effectively replaces return-by-value from the given function into
// return-by-reference through the new argument.  It allows the result to be
// written directly into space allocated in the caller, thus avoiding a
// verbatim copy.
//
static FnSymbol*
createClonedFnWithRetArg(FnSymbol* fn, FnSymbol* useFn)
{
  SET_LINENO(fn);
  FnSymbol* newFn = fn->copy();
  // Note: other code does strcmps against the name _retArg
  ArgSymbol* arg = new ArgSymbol(blankIntentForType(useFn->retType->refType), "_retArg", useFn->retType->refType);
  arg->addFlag(FLAG_RETARG);
  newFn->insertFormalAtTail(arg);
  newFn->addFlag(FLAG_FN_RETARG);
  VarSymbol* ret = toVarSymbol(newFn->getReturnSymbol());
  INT_ASSERT(ret);
  Expr* returnPrim = newFn->body->body.tail;
  returnPrim->replace(new CallExpr(PRIM_RETURN, gVoid));
  newFn->retType = dtVoid;
  fn->defPoint->insertBefore(new DefExpr(newFn));

  std::vector<SymExpr*> symExprs;
  collectSymExprs(newFn, symExprs);

  // In the body of the function, replace references to the original
  // ret symbol with copies of the return value reference.  A local
  // deref temp is inserted if needed.  The result is fed through a
  // call to the useFn -- effectively sucking the use function call
  // inside the clone function.
  for_vector(SymExpr, se, symExprs) {
    if (se->symbol() == ret) {
      CallExpr* move = toCallExpr(se->parentExpr);
      if (move && move->isPrimitive(PRIM_MOVE) && move->get(1) == se) {
        SET_LINENO(move);
        replacementHelper(move, ret, arg, useFn);
      } else {
        // Any other call or primitive.
        FnSymbol* calledFn = move->isResolved();
        CallExpr* parent = toCallExpr(move->parentExpr);
        if (calledFn && !strcmp(calledFn->name, "=") &&
            // Filter out case handled above.
            (!parent || !parent->isPrimitive(PRIM_MOVE))) {
          replacementHelper(move, ret, arg, useFn);
        } else {
          Symbol* tmp = newTemp("ret_to_arg_tmp_", useFn->retType);
          se->getStmtExpr()->insertBefore(new DefExpr(tmp));
          se->getStmtExpr()->insertBefore(new CallExpr(PRIM_MOVE, tmp, new CallExpr(PRIM_DEREF, arg)));
          se->setSymbol(tmp);
        }
      }
    }
  }
  return newFn;
}


static void replaceRemainingUses(Vec<SymExpr*>& use, SymExpr* firstUse,
                                 Symbol* actual)
{
  // for each remaining use "se"
  //   replace se with deref of the actual return value argument, unless parent is
  //   accessing its address
  forv_Vec(SymExpr, se, use) {
    // Because we've already handled the first use
    if (se != firstUse) {
      CallExpr* parent = toCallExpr(se->parentExpr);
      if (parent) {
        SET_LINENO(parent);
        if (parent->isPrimitive(PRIM_ADDR_OF)) {
          parent->replace(new SymExpr(actual));
        } else {
          FnSymbol* parentFn = parent->isResolved();
          if (!(parentFn->hasFlag(FLAG_AUTO_COPY_FN) ||
                parentFn->hasFlag(FLAG_INIT_COPY_FN))) {
            // Leave the auto copies/inits in, we'll need them for
            // moving information back to us.

            // Copy the information we currently have into the temp
            se->getStmtExpr()->insertBefore(new CallExpr(PRIM_MOVE, se->symbol(), new CallExpr(PRIM_DEREF, actual)));
          }
        }
      }
    }
  }
}


// Create a copy of the called function, replacing
// the return statement in that function with a copy of the call which uses
// the result of the above call to that function.  Maybe a picture would
// help.
//   ('move' lhs    (fn args ...))
//   . . .
//   ('move' useLhs (useFn lhs))
// gets converted to
//   (newFn args ... useLhs)
//   . . .
//   <removed>
// where a call to useFn replaces the return that used to be at the end of
// newFn.  The use function is expected to be assignment, initCopy or
// autoCopy.  All other cases are ignored.
static void replaceUsesOfFnResultInCaller(CallExpr* move, CallExpr* call,
                                          Vec<SymExpr*>& use, FnSymbol* fn)
{
  SymExpr* firstUse = use.v[0];
  // If this isn't a call expression, we've got problems.
  if (CallExpr* useCall = toCallExpr(firstUse->parentExpr)) {
    if (FnSymbol* useFn = useCall->isResolved()) {
      if ((!strcmp(useFn->name, "=") && firstUse == useCall->get(2)) ||
          useFn->hasFlag(FLAG_AUTO_COPY_FN) ||
          useFn->hasFlag(FLAG_INIT_COPY_FN)) {
        Symbol* actual;
        FnSymbol* newFn = NULL;

        //
        // check cache for new function
        //
        if (Vec<FnSymbol*>* vfn = retToArgCache.get(fn)) {
          for (int i = 0; i < vfn->n; i++) {
            if (vfn->v[i] == useFn) {
              newFn = vfn->v[i+1];
            }
          }
        }

        if (!newFn) {
          newFn = createClonedFnWithRetArg(fn, useFn);

          //
          // add new function to cache
          //
          Vec<FnSymbol*>* vfn = retToArgCache.get(fn);
          if (!vfn)
            vfn = new Vec<FnSymbol*>();
          vfn->add(useFn);
          vfn->add(newFn);
          retToArgCache.put(fn, vfn);
        }

        SET_LINENO(call);
        call->baseExpr->replace(new SymExpr(newFn));

        CallExpr* useMove = toCallExpr(useCall->parentExpr);
        if (useMove)
        {
          INT_ASSERT(isMoveOrAssign(useMove));

          Symbol* useLhs = toSymExpr(useMove->get(1))->symbol();
          if (!useLhs->isRef())
          {
            useLhs = newTemp("ret_to_arg_ref_tmp_", useFn->retType->refType);
            move->insertBefore(new DefExpr(useLhs));
            move->insertBefore(new CallExpr(PRIM_MOVE, useLhs, new CallExpr(PRIM_ADDR_OF, useMove->get(1)->remove())));
          }

          move->replace(call->remove());
          useMove->remove();
          call->insertAtTail(useLhs);

          actual = useLhs;
        }
        else
        {
          // We assume the useFn is an assignment.
          if (strcmp(useFn->name, "="))
          {
            INT_FATAL(useFn, "should be an assignment function");
            return;
          }

          // We expect that the used symbol is the second actual passed to
          // the "=".  That is, it is an assignment from the result of the
          // call to fn to useLhs.
          INT_ASSERT(firstUse == useCall->get(2));

          Symbol* useLhs = toSymExpr(useCall->get(1))->symbol();
          move->replace(call->remove());
          call->insertAtTail(useLhs);

          actual = useLhs;
        }
        if (actual) {
          replaceRemainingUses(use, firstUse, actual);
        }
      }
    }
  }
}


static void
changeRetToArgAndClone(CallExpr* move, Symbol* lhs,
                       CallExpr* call, FnSymbol* fn,
                       Map<Symbol*,Vec<SymExpr*>*>& defMap,
                       Map<Symbol*,Vec<SymExpr*>*>& useMap) {
  // Here are some relations between the arguments that can be relied upon.
  INT_ASSERT(call->parentExpr == move);
  INT_ASSERT(call->isResolved() == fn);

  // In the suffix of the containing function, look for uses of the lhs of the
  // move containing the call to fn.
  Vec<SymExpr*> use;
  if (useMap.get(lhs) && useMap.get(lhs)->n == 1) {
    use = *useMap.get(lhs);
  } else {
    for (Expr* stmt = move->next; stmt; stmt = stmt->next) {
      std::vector<SymExpr*> symExprs;
      collectSymExprs(stmt, symExprs);
      for_vector(SymExpr, se, symExprs) {
        if (se->symbol() == lhs) {
          use.add(se);
        }
      }
    }
  }

  // If such a use is found, create a copy of the called function, replacing
  // the return statement in that function with a copy of the call which uses
  // the result of the above call to that function.
  if (use.n > 0) {
    replaceUsesOfFnResultInCaller(move, call, use, fn);
  }
}


static void
returnRecordsByReferenceArguments() {
  Map<Symbol*,Vec<SymExpr*>*> defMap;
  Map<Symbol*,Vec<SymExpr*>*> useMap;
  buildDefUseMaps(defMap, useMap);

  forv_Vec(CallExpr, call, gCallExprs) {
    if (call->parentSymbol) {
      if (FnSymbol* fn = requiresImplicitDestroy(call)) {
        if (fn->hasFlag(FLAG_EXTERN))
          continue;
        CallExpr* move = toCallExpr(call->parentExpr);
        INT_ASSERT(move->isPrimitive(PRIM_MOVE) ||
                   move->isPrimitive(PRIM_ASSIGN));
        SymExpr* lhs = toSymExpr(move->get(1));
        INT_ASSERT(!lhs->symbol()->hasFlag(FLAG_TYPE_VARIABLE));
        changeRetToArgAndClone(move, lhs->symbol(), call, fn, defMap, useMap);
      }
    }
  }
  freeDefUseMaps(defMap, useMap);
}

static void
fixupDestructors() {
  forv_Vec(FnSymbol, fn, gFnSymbols) {
    if (fn->hasFlag(FLAG_DESTRUCTOR)) {
      AggregateType* ct = toAggregateType(fn->_this->getValType());
      INT_ASSERT(ct);

      //
      // insert calls to destructors for all 'value' fields
      //
      for_fields_backward(field, ct) {
        SET_LINENO(field);
        if (field->type->destructor) {
          AggregateType* fct = toAggregateType(field->type);
          INT_ASSERT(fct);
          if (!isClass(fct)) {
            bool useRefType = !isRecordWrappedType(fct);
            VarSymbol* tmp = newTemp("_field_destructor_tmp_", useRefType ? fct->refType : fct);
            fn->insertBeforeReturnAfterLabel(new DefExpr(tmp));
            fn->insertBeforeReturnAfterLabel(new CallExpr(PRIM_MOVE, tmp,
              new CallExpr(useRefType ? PRIM_GET_MEMBER : PRIM_GET_MEMBER_VALUE, fn->_this, field)));
            FnSymbol* autoDestroyFn = autoDestroyMap.get(field->type);
            if (autoDestroyFn && autoDestroyFn->hasFlag(FLAG_REMOVABLE_AUTO_DESTROY))
              fn->insertBeforeReturnAfterLabel(new CallExpr(autoDestroyFn, tmp));
            else
              fn->insertBeforeReturnAfterLabel(new CallExpr(field->type->destructor, tmp));
          }
        } else if (FnSymbol* autoDestroyFn = autoDestroyMap.get(field->type)) {
          VarSymbol* tmp = newTemp("_field_destructor_tmp_", field->type);
          fn->insertBeforeReturnAfterLabel(new DefExpr(tmp));
          fn->insertBeforeReturnAfterLabel(
                new CallExpr(PRIM_MOVE, tmp,
                  new CallExpr(PRIM_GET_MEMBER_VALUE, fn->_this, field)));
          fn->insertBeforeReturnAfterLabel(new CallExpr(autoDestroyFn, tmp));
        }
      }

      //
      // insert call to parent destructor
      //
      INT_ASSERT(ct->dispatchParents.n <= 1);
      if (ct->dispatchParents.n >= 1 && isClass(ct)) {
        // avoid destroying record fields more than once
        if (FnSymbol* parentDestructor = ct->dispatchParents.v[0]->destructor) {
          SET_LINENO(fn);
          Type* tmpType = isClass(ct) ?
            ct->dispatchParents.v[0] : ct->dispatchParents.v[0]->refType;
          VarSymbol* tmp = newTemp("_parent_destructor_tmp_", tmpType);
          fn->insertBeforeReturnAfterLabel(new DefExpr(tmp));
          fn->insertBeforeReturnAfterLabel(new CallExpr(PRIM_MOVE, tmp,
            new CallExpr(PRIM_CAST, tmpType->symbol, fn->_this)));
          fn->insertBeforeReturnAfterLabel(new CallExpr(parentDestructor, tmp));
        }
      }
    }
  }
}


static void insertGlobalAutoDestroyCalls() {
  // --ipe does not build chpl_gen_main
  if (chpl_gen_main == NULL)
    return;

  SET_LINENO(baseModule);

  const char* name = "chpl__autoDestroyGlobals";
  FnSymbol*   fn   = new FnSymbol(name);

  fn->retType = dtVoid;

  chpl_gen_main->defPoint->insertBefore(new DefExpr(fn));
  chpl_gen_main->insertBeforeReturnAfterLabel(new CallExpr(fn));

  forv_Vec(DefExpr, def, gDefExprs) {
    if (isModuleSymbol(def->parentSymbol))
      if (def->parentSymbol != rootModule)
        if (VarSymbol* var = toVarSymbol(def->sym))
          if (!var->isParameter() && !var->isType())
            if (!var->hasFlag(FLAG_NO_AUTO_DESTROY))
              if (FnSymbol* autoDestroy = autoDestroyMap.get(var->type)) {
                SET_LINENO(var);
                fn->insertAtTail(new CallExpr(autoDestroy, var));
              }
  }

  fn->insertAtTail(new CallExpr(PRIM_RETURN, gVoid));
}


static void insertDestructorCalls() {
  forv_Vec(CallExpr, call, gCallExprs) {
    if (call->isPrimitive(PRIM_CALL_DESTRUCTOR)) {
      Type* type = call->get(1)->typeInfo();
      if (!type->destructor) {
        call->remove();
      } else {
        SET_LINENO(call);
        call->replace(new CallExpr(type->destructor, call->get(1)->remove()));
      }
    }
  }
}

/* For a variable marked with FLAG_INSERT_AUTO_COPY,
   call autoCopy when there is a MOVE to that variable
   from another expression (variable or call).

   Note that FLAG_INSERT_AUTO_COPY is only ever set for
   lhs variables in moves such as
      move lhs, someCall()
   where requiresImplicitDestroy(someCall). requiresImplicitDestroy is
   described as checking "if the function requires an implicit
   destroy of its returned value (i.e. reference count)".

   The code checks:
    - not in a "donor fn" (auto copy fn)
    - called function returns a record or ref-counted type by ref
    - called function does not have FLAG_NO_IMPLICIT_COPY (getter fn)
    - called function is not an iterator
    - called function is not returning a runtime type value
    - called function is not a "donor fn" (autocopy fns in modules)
    - called function is not init copy
    - called function is not =
    - called function is not _defaultOf
    - called function does not have FLAG_AUTO_II
    - called function is not a constructor
    - called function is not a type constructor

   Relevant commits are
     3788ee34fa created
     70d5ea4040 bug fix/workarounds
     93d5338f8a switch to using flags
     57a13e7c22 fix compiler warning
     c27afd6b4f adds FLAG_NO_IMPLICIT_COPY == FLAG_RETURN_VALUE_IS_NOT_OWNED?
     adfb566b00 FLAG_ITERATOR_FN -> isIterator
     a43758e6aa adds check for defaultOf, "fixes 4 test failures"
     61db88b637 flag cleanups


   Anyway, insertAutoCopyTemps does the following:

   when x has FLAG_INSERT_AUTO_COPY

   move x, y
   ->
   move atmp, y
   move x, autoCopy(atmp)

   or

   move x, someCall()      (where requiresImplicitDestroy(someCall))
   ->
   move atmp, someCall()
   move x, autoCopy(atmp)

 */
static void insertAutoCopyTemps() {
  Map<Symbol*,Vec<SymExpr*>*> defMap;
  Map<Symbol*,Vec<SymExpr*>*> useMap;
  buildDefUseMaps(defMap, useMap);

  forv_Vec(VarSymbol, sym, gVarSymbols) {
    if (sym->hasFlag(FLAG_INSERT_AUTO_COPY)) {
      CallExpr* move = NULL;
      for_defs(def, defMap, sym) {
        CallExpr* defCall = toCallExpr(def->parentExpr);
        if (defCall->isPrimitive(PRIM_MOVE)) {
          CallExpr* rhs = toCallExpr(defCall->get(2));
          if (!rhs || !rhs->isNamed("=")) {
            // We enter this block if:
            // - rhs is a variable (!rhs), or
            // - rhs is a call but not to =
            //
            // I think that calls to = no longer appear
            // in PRIM_MOVE in the AST, so I think that
            // this is actually if (!rhs || rhs)
            // since = used to return a value but no longer does.

            // This check ensures that there is only a single PRIM_MOVE
            // definition of a variable marked with FLAG_INSERT_AUTO_COPY.
            INT_ASSERT(!move);
            move = defCall;
          }
        }
      }

      // 2015/01/21 hilde: Workaround for incomplete implementation of
      // SymExpr::remove() in the context of a ForLoop (as its mIndex field).
      // This operation is required by the early operation of
      // deadBlockElimination().

      // In that case, the DefExpr for the symbol should no longer exist, so we
      // would never reach here.  Given that it is never defined and we *do*
      // reach here, there is no harm in not creating the autoCopy temp.  This
      // code will probably all be deprecated when the new AMM story comes
      // online anyway, so it would be a waste of time trying to "do things
      // right" in this routine.
      if (! move)
        continue;

      INT_ASSERT(move);
      SET_LINENO(move);

      // MPF 2016-10-02. This code should no longer be necessary
      // but it is currently being run. See the comment near the call
      // to requiresImplicitDestroy in functionResolution and the test
      // call-expr-tmp.chpl.

      Symbol* tmp = newTemp("_autoCopy_tmp_", sym->type);

      move->insertBefore(new DefExpr(tmp));
      move->insertAfter(new CallExpr(PRIM_MOVE,
                                     sym,
                                     new CallExpr(getAutoCopyForType(sym->type),
                                                  tmp)));
      move->get(1)->replace(new SymExpr(tmp));
    }
  }

  freeDefUseMaps(defMap, useMap);
}


// This routine inserts autoCopy calls ahead of yield statements as necessary,
// so the calling routine "owns" the returned value.
// The copy is necessary for yielded values of record type returned by value.
// In the current implementation, types marked as "iterator record" and
// "runtime type value" are excluded.
static void insertYieldTemps()
{
  // Examine all calls.
  forv_Vec(CallExpr, call, gCallExprs)
  {
    // Select only yield primitives.
    if (! call->isPrimitive(PRIM_YIELD))
      continue;

    // Filter out calls that are not in the tree.
    if (! call->parentSymbol)
      continue;

    // This is the symbol passed back in the yield.
    SymExpr* yieldExpr = toSymExpr(call->get(1));

    // The transformation is applied only if is has a normal record type
    // (passed by value).
    Type* type = yieldExpr->symbol()->type;

    if (isRecord(type) &&
        !type->symbol->hasFlag(FLAG_ITERATOR_RECORD) &&
        !type->symbol->hasFlag(FLAG_RUNTIME_TYPE_VALUE))
    {
      SET_LINENO(call);

      // Replace:
      //   yield <yieldExpr>
      // with:
      //   (def _yield_expr_tmp_:type)
      //   (move _yield_expr_tmp_ ("chpl__autoCopy" <yieldExpr>))
      //   yield _yield_expr_tmp_
      Symbol* tmp = newTemp("_yield_expr_tmp_", type);
      Expr* stmt = call->getStmtExpr();
      stmt->insertBefore(new DefExpr(tmp));
      stmt->insertBefore(new CallExpr(PRIM_MOVE, tmp, new CallExpr(getAutoCopyForType(type), yieldExpr->remove())));
      call->insertAtHead(new SymExpr(tmp)); // New first argument.
    }
  }
}


//
// Insert reference temps for function arguments that expect them.
//
void insertReferenceTemps(CallExpr* call) {
  for_formals_actuals(formal, actual, call) {
    if (formal->type == actual->typeInfo()->refType) {
      SET_LINENO(call);
      Expr* stmt = call->getStmtExpr();
      VarSymbol* tmp = newTemp("_ref_tmp_", formal->type);
      tmp->addFlag(FLAG_REF_TEMP);
      stmt->insertBefore(new DefExpr(tmp));
      actual->replace(new SymExpr(tmp));
      stmt->insertBefore(
        new CallExpr(PRIM_MOVE, tmp, new CallExpr(PRIM_ADDR_OF, actual)));
    }
  }
}


static void insertReferenceTemps() {
  forv_Vec(CallExpr, call, gCallExprs) {
    // Is call in the tree?
    if (call->parentSymbol != NULL) {
      if (call->isResolved() ||
          call->isPrimitive(PRIM_VIRTUAL_METHOD_CALL)) {
        insertReferenceTemps(call);
      }
    }
  }
}


// code like
//    var x => GlobalArray[1..10]; // create a slice
//    return x;
// or
//    var GlobalAlias => GlobalArray[1..10];
//
// poses problems because the call_tmp for the slice expression
// (GlobalArray[1..10] in these examples) is destroyed at the end
// of the current function. Instead, it should be used to initialize
// the slice.
//
// For similar reasons, code like
//
//     class C {
//      var A: [1..2] int;
//    }
//    var GA: [1..5] int = [i in 1..5] i;
//    var c2 = new C(A=>GA[1..2]);  // <- note A=>GA here
//
// also needs special treatment.
//
static
void fixupNewAlias(void) {

  std::vector<CallExpr*> newAliasCalls;
  std::vector<CallExpr*> hasAliasArgInCtor;
  const char* lookForAliasField = "chpl__aliasField_";
  size_t lookForAliasFieldLen = strlen(lookForAliasField);

  forv_Vec(CallExpr, call, gCallExprs) {
    FnSymbol* calledFn = call->isResolved();
    if (calledFn && calledFn->hasFlag(FLAG_NEW_ALIAS_FN)) {
        newAliasCalls.push_back(call);
    }
    if (calledFn && calledFn->hasFlag(FLAG_DEFAULT_CONSTRUCTOR)) {
      // Does the calledFn have a formal that starts with
      // chpl__aliasField_ ?

      // TODO -- could this use a flag?
      for_formals(formal, calledFn) {
        if (0 == strncmp(formal->name, lookForAliasField, lookForAliasFieldLen))
          hasAliasArgInCtor.push_back(call);
      }
    }
  }

  for_vector(CallExpr, call, newAliasCalls) {
    SymExpr* se = toSymExpr(call->get(1));
    if (se->symbol()->hasFlag(FLAG_TEMP) &&
        se->isRef() == false) {
      // Note: these flags are added in functionResolution's postFold
      se->symbol()->removeFlag(FLAG_INSERT_AUTO_COPY);
      se->symbol()->removeFlag(FLAG_INSERT_AUTO_DESTROY);
      call->replace(se->remove());
    }
  }

  for_vector(CallExpr, ctorCall, hasAliasArgInCtor) {
    FnSymbol* fn = ctorCall->isResolved();

    for_formals_actuals(formal, actual, ctorCall) {

      // TODO -- could this use a flag?
      bool isArrayAliasField = false;
      const char* aliasFieldArg = astr("chpl__aliasField_", formal->name);
      for_formals(fml, fn)
        if (fml->name == aliasFieldArg)
          isArrayAliasField = true;

      if (isArrayAliasField) {
        SymExpr* se = toSymExpr(actual);
        bool isTemp = se->symbol()->hasFlag(FLAG_TEMP);
        bool isAlias = se->symbol()->hasFlag(FLAG_ARRAY_ALIAS);
        if ((isTemp || isAlias) &&
             se->isRef() == false) {
          se->symbol()->removeFlag(FLAG_INSERT_AUTO_COPY);
          se->symbol()->removeFlag(FLAG_INSERT_AUTO_DESTROY);
        }
      }
    }
  }
}


// Function resolution adds "dummy" initCopy functions for types
// that cannot be copied. These "dummy" initCopy functions are marked
// with the flag FLAG_ERRONEOUS_INITCOPY. This pattern enables
// the compiler to continue to operate with its current structure
// even for types that cannot be copied. In particular, this pass
// has the ability to remove initCopy calls in some cases.
//
// This function simply checks that no function marked with that
// flag is ever called and raises an error if so.
static
void checkForErroneousInitCopies() {

  forv_Vec(FnSymbol, fn, gFnSymbols) {
    if (fn->hasFlag(FLAG_ERRONEOUS_INITCOPY)) {
      // Error on each call site
      for_SymbolSymExprs(se, fn) {
        USR_FATAL_CONT(se, "copy-initialization invoked for a type"
                           " that does not have a copy initializer");
      }
    }
  }
}


/************************************* | **************************************
*                                                                             *
* Entry point                                                                 *
*                                                                             *
************************************** | *************************************/

void callDestructors() {
  fixupNewAlias();
  fixupDestructors();

  insertDestructorCalls();
  insertAutoCopyTemps();

  // Execute this before conversion to return by ref
  // May fail to handle reference variables as desired
  addAutoDestroyCalls();

  ReturnByRef::apply();
  returnRecordsByReferenceArguments();

  insertYieldTemps();
  insertGlobalAutoDestroyCalls();
  insertReferenceTemps();

  checkForErroneousInitCopies();
}
