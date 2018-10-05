/*
 * Copyright 2017 Advanced Micro Devices, Inc.
 * Copyright 2004-2018 Cray Inc.
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

#include "astutil.h"
#include "bb.h"
#include "bitVec.h"
#include "CForLoop.h"
#include "dominator.h"
#include "driver.h"
#include "expr.h"
#include "ForLoop.h"
#include "ParamForLoop.h"
#include "stlUtil.h"
#include "stmt.h"
#include "stringutil.h"
#include "symbol.h"
#include "optimizations.h"
#include "timer.h"
#include "WhileStmt.h"

#include <algorithm>
#include <set>
#include <queue>
#include <utility>


static
void addNoAliasSetForFormal(ArgSymbol* arg,
                            std::vector<ArgSymbol*> notAliasingThese) {
  FnSymbol* fn = arg->getFunction();
  if (fn->getModule()->modTag == MOD_USER) {
    if (notAliasingThese.size() > 0) {
      printf("in function %s\n", fn->name);
      printf(" arg %s does not alias args ", arg->name);
      for_vector(ArgSymbol, other, notAliasingThese) {
        printf(" %s", other->name);
      }
      printf("\n");
    }
  }
}

static
bool isAddrTaken(Symbol* var) {
  // Only handles values
  INT_ASSERT(!var->isRef());

  for_SymbolSymExprs(se, var) {
    if (CallExpr* call = toCallExpr(se->parentExpr)) {
      if (call->isPrimitive(PRIM_SET_REFERENCE) ||
          call->isPrimitive(PRIM_ADDR_OF)) {
        return true;
      } else if (call->isResolved()) {
        // Return true for non-ref passed by ref
        for_formals_actuals(formal, actual, call) {
          if (actual == se) {
            if (formal->intent & INTENT_FLAG_REF)
              return true;
          }
        }
      }
    }
  }
  return false;
}

static
bool isRefFormal(ArgSymbol* formal) {
  return (formal->intent & INTENT_FLAG_REF);
}

static
bool fnHasRefFormal(FnSymbol* fn) {
  for_formals(formal, fn) {
    if (isRefFormal(formal))
      return true;
  }

  return false;
}

static
BitVec makeBitVec(int size) {
  BitVec ret(size);
  return ret;
}

// Sets the bit at index in the bit vector for sym.
// Like
//   map[sym].set(index)
// but creates the BitVec if it doesn't exist yet, with size bitVecSize
//
// returns true if it changed something
static
bool addAlias(std::map<Symbol*, BitVec> &map,
              int bitVecSize,
              Symbol* sym,
              int index) {

  bool changed = false;
  std::map<Symbol*, BitVec>::iterator it = map.find(sym);

  if (it == map.end()) {
    it = map.insert(std::make_pair(sym, makeBitVec(bitVecSize))).first;
    changed = true;
  }

  BitVec &bits = it->second;
  if (bits.get(index) == false) {
    bits.set(index);
    changed = true;
  }

  return changed;
}

static
bool addAliases(std::map<Symbol*, BitVec> &map,
                int bitVecSize,
                Symbol* sym,
                BitVec& from) {

  bool changed = false;
  std::map<Symbol*, BitVec>::iterator it = map.find(sym);

  if (it == map.end()) {
    it = map.insert(std::make_pair(sym, makeBitVec(bitVecSize))).first;
    changed = true;
  }

  BitVec &bits = it->second;
  BitVec toSet = from - bits;
  if (toSet.any()) {
    bits += toSet;
    changed = true;
  }

  return changed;
}



static
void markNonAliasingRefArguments() {
  // Inspired by Kennedy "Optimizing Compilers for Modern Architectures" p 571

  // These are the main results of this function

  // map from ArgSymbol -> BitVecs of size nAddrTakenGlobals
  std::map<Symbol*, BitVec> formalsAliasingGlobals;

  // set ArgSymbol where we gave up on analysis
  std::set<ArgSymbol*> formalsAliasingAnything;

  // map from FnSymbol -> BitVec of size nFormalPairs,
  //   storing pairs of arguments that can alias
  std::map<Symbol*, BitVec> fpairs;


  // reused a few times in this function
  std::vector<CallExpr*> calls;

  // First, compute global variables that have their address taken.
  // The analysis will establish when these can alias ref arguments.
  std::map<Symbol*, int> addrTakenGlobalsToIds;

  {
    int id = 1;
    forv_Vec(VarSymbol, var, gVarSymbols) {
      if (isGlobal(var) &&
          !var->isRef() &&
          isAddrTaken(var)) {
        if (addrTakenGlobalsToIds.count(var) == 0) {
          addrTakenGlobalsToIds[var] = id;
          id++;
        }
      }
    }
  }

  // Now compute the global alias sets for procedure arguments
  size_t nAddrTakenGlobals = addrTakenGlobalsToIds.size();

  // Compute the starting point for the sets,
  // don't worry about transitivity/propagating yet.
  forv_Vec(FnSymbol, fn, gFnSymbols) {
    if (fnHasRefFormal(fn)) {
      if (fn->hasFlag(FLAG_EXPORT)) {
        // Assume exported functions can have formals aliasing each other
        for_formals(fnFormal, fn) {
          if (isRefFormal(fnFormal)) {
            formalsAliasingAnything.insert(fnFormal);
          }
        }
      } else {
        // Consider call sites to the function to propagate alias information
        for_SymbolSymExprs(se, fn) {
          // use of SymExpr(fn) is a call
          if (CallExpr* call = toCallExpr(se->parentExpr)) {
            // it's a call to fn
            if (fn == call->resolvedOrVirtualFunction()) {
              // Consider the actuals and formals and update.
              for_formals_actuals(fnFormal, actual, call) {
                // fnFormal is a formal from fn, which we're investigating
                if (isRefFormal(fnFormal)) {
                  // What is the actual?
                  SymExpr* actualSe = toSymExpr(actual);
                  if (VarSymbol* fromVar = toVarSymbol(actualSe->symbol())) {
                    if (fromVar->isRef()) {
                      // Give up
                      formalsAliasingAnything.insert(fnFormal);
                    } else if (isGlobal(fromVar) &&
                               addrTakenGlobalsToIds.count(fromVar) > 0) {
                      int globalId = addrTakenGlobalsToIds[fromVar];
                      // add the global to the set
                      addAlias(formalsAliasingGlobals,
                               nAddrTakenGlobals,
                               fnFormal, globalId);
                      // local *value* variables don't add to alias sets
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  // Construct the binding graph, which has nodes that are formal parameters,
  // where there is an edge between f1 of p and f2 of q if
  // f1 is passed to f2 in a call q(...) inside of the body of p.
  // See Kennedy p 564

  std::map<ArgSymbol*, std::set<ArgSymbol*> > bindingGraph;

  forv_Vec(FnSymbol, p, gFnSymbols) {
    if (fnHasRefFormal(p)) {
      calls.clear();
      collectFnCalls(p, calls);

      for_vector(CallExpr, call, calls) {
        FnSymbol* q = call->resolvedOrVirtualFunction();
        INT_ASSERT(q);

        for_formals_actuals(qFormal, actual, call) {
          if (isRefFormal(qFormal)) {
            SymExpr* actualSe = toSymExpr(actual);
            if (ArgSymbol* fFormal = toArgSymbol(actualSe->symbol())) {
              if (isRefFormal(fFormal)) {
                bindingGraph[fFormal].insert(qFormal);
              }
            }
          }
        }
      }
    }
  }

  // Now use the binding graph to compute the transitive closure

  bool changed;
  bool lastiter;
  int niters = 1;
  int maxiters = 3;
  do {
    lastiter = niters == maxiters;
    changed = false;
    std::map<ArgSymbol*, std::set<ArgSymbol*> >::const_iterator it;

    for (it = bindingGraph.begin(); it != bindingGraph.end(); ++it) {
      ArgSymbol* f1 = it->first;
      const std::set<ArgSymbol*> &toSet = it->second;
      for_set(ArgSymbol, f2, toSet) {
        // Propagate alias information from f1 to f2

        // propagate aliases anything
        if (formalsAliasingAnything.count(f1)) {
          if (formalsAliasingAnything.count(f2) == 0) {
            formalsAliasingAnything.insert(f2);
            changed = true;
          }
        }

        // propagate aliases to globals
        {
          bool newGlobals = false;
          std::map<Symbol*, BitVec>::iterator it =
            formalsAliasingGlobals.find(f1);
          if (it != formalsAliasingGlobals.end()) {
            BitVec &fromBits = it->second;
            newGlobals = addAliases(formalsAliasingGlobals,
                                    nAddrTakenGlobals,
                                    f2, fromBits);
            if (newGlobals) {
              changed = true;
              if (lastiter) {
                formalsAliasingAnything.insert(f2);
              }
            }
          }
        }
      }
    }
    niters++;
  } while (changed && !lastiter);

  // Compute the maximum number of formals
  int maxFormals = 1;
  forv_Vec(FnSymbol, fn, gFnSymbols) {
    int nFormals = fn->numFormals();
    if (nFormals > maxFormals) {
      maxFormals = nFormals;
    }
  }
  maxFormals++; // Add 1 so we can count formals from 1

  if (maxFormals > 100) {
    USR_WARN("Too many formal parameters! Optimization is inhibited");
    maxFormals = 100;
    return;
  }

  // Now compute the set of formals aliasing other formals
  // This follows Figure 11.8 in Allen & Kennedy
  int nFormalPairs = maxFormals * maxFormals;
  std::queue<std::pair<ArgSymbol*, ArgSymbol*> > worklist;

  // for each alias introduction site, e.g. f(X, X),
  // insert the resulting formal pair in fpairs and worklist
  forv_Vec(FnSymbol, fn, gFnSymbols) {
    if (fnHasRefFormal(fn)) {
      // Consider call sites to the function to propagate alias information
      for_SymbolSymExprs(se, fn) {
        // use of SymExpr(fn) is a call
        if (CallExpr* call = toCallExpr(se->parentExpr)) {
          // it's a call to fn
          if (fn == call->resolvedOrVirtualFunction()) {
            // Consider the actuals and formals and update
            // Look for f(X, X)
            int formalIdx1 = 1;
            for_formals_actuals(fnFormal1, actual, call) {
              // fnFormal is a formal from fn, which we're investigating
              SymExpr* actualSe = toSymExpr(actual);
              Symbol* actualSym = actualSe->symbol();
              VarSymbol* actualVar = toVarSymbol(actualSym);
              if (actualVar && actualVar->isRef()) {
                // don't worry about ref variables; covered above
              } else if (isRefFormal(fnFormal1)) {
                // Find cases where the same argument is passed
                // This could be implemented in a different way
                // but the nested loops is much clearer
                int formalIdx2 = 1;
                for_formals_actuals(fnFormal2, actual2, call) {
                  SymExpr* actual2Se = toSymExpr(actual2);
                  Symbol* actual2Sym = actual2Se->symbol();
                  if (formalIdx1 < formalIdx2 &&
                      actualSym == actual2Sym) {
                    // The same actual was passed in positions
                    // formalIdx1 and formalIdx2

                    // add the pair to fpairs
                    addAlias(fpairs, nFormalPairs,
                             fn, maxFormals*formalIdx1 + formalIdx2);

                    // add the pair to the worklist
                    worklist.push(std::make_pair(fnFormal1, fnFormal2));
                  }
                  formalIdx2++;
                }
              }
              formalIdx1++;
            }
          }
        }
      }
    }
  }

  // while worklist is not empty
  //   remove element from worklist, arguments f1 and f2 to procedure p
  //   for each call to q in that procedure passing both arguments f1, f2
  //     suppose q's corresponding formals are at positions f3, f4
  //     if (f3,f4) is not in fpairs[q]
  //       add (f3,f4) to fpairs[q]
  //       add (f3,f4) to the worklist

  while (!worklist.empty()) {
    std::pair<ArgSymbol*, ArgSymbol*> elt = worklist.front();
    worklist.pop();
    ArgSymbol* f1 = elt.first;
    ArgSymbol* f2 = elt.second;
    FnSymbol* p = f1->getFunction();
    INT_ASSERT(f2->getFunction() == p);
    INT_ASSERT(f1 != f2);

    calls.clear();
    collectFnCalls(p, calls);

    for_vector(CallExpr, call, calls) {
      FnSymbol* q = call->resolvedOrVirtualFunction();
      INT_ASSERT(q);
      ArgSymbol* f3 = NULL;
      int f3Idx = 0;
      ArgSymbol* f4 = NULL;
      int f4Idx = 0;

      // Figure out what f1 is passed to, and what f2 is passed to
      int formalIdx = 1;
      for_formals_actuals(qFormal, actual, call) {
        if (isRefFormal(qFormal)) {
          SymExpr* actualSe = toSymExpr(actual);
          if (ArgSymbol* actualArg = toArgSymbol(actualSe->symbol())) {
            if (actualArg == f1) {
              f3 = qFormal;
              f3Idx = formalIdx;
            }
            if (actualArg == f2) {
              f4 = qFormal;
              f4Idx = formalIdx;
            }
          }
        }
        formalIdx++;
      }
      if (f3 != NULL && f4 != NULL) {

        INT_ASSERT(f3Idx < f4Idx);

        // add the pair to fpairs
        bool added = addAlias(fpairs, nFormalPairs,
                              q, maxFormals*f3Idx + f4Idx);
        if (added) {
          worklist.push(std::make_pair(f3, f4));
        }
      }
    }
  }

  // OK, now we have computed answers to lots of aliasing questions!

  // in function p, we should consider formal f1 potentially aliasing
  // formal f2 if:
  //   - f1 or f2 is in formalsAliasingAnything
  //   - (f1,f2) is in fpairs[p]
  //   - formalsAliasingGlobals[f1] intersect formalsAliasingGlobals[f2] != {}

  std::vector<ArgSymbol*> noAliasArgs;

  forv_Vec(FnSymbol, p, gFnSymbols) {
    if (fnHasRefFormal(p)) {
      // Are the formals independent? Do they alias each other?
      int formalIdx1 = 1;
      for_formals(formal1, p) {
        if (isRefFormal(formal1)) {
          if (formalsAliasingAnything.count(formal1)) {
            // Don't emit any alias sets for this one
          } else {
            noAliasArgs.clear();
            int formalIdx2 = 1;
            for_formals(formal2, p) {
              if (formalIdx1 != formalIdx2 && isRefFormal(formal2)) {
                // normalize the pair to idx1 < idx2
                int idx1 = formalIdx1 < formalIdx2 ? formalIdx1 : formalIdx2;
                int idx2 = formalIdx1 < formalIdx2 ? formalIdx2 : formalIdx1;
                INT_ASSERT(idx1 < idx2);
                bool pairCanAlias = false;

                // f2 is in formalsAliasingAnything
                if (formalsAliasingAnything.count(formal2))
                  pairCanAlias = true;

                // Does (idx1,idx2) appear in fpairs(p) ?
                std::map<Symbol*, BitVec>::iterator it = fpairs.find(p);
                if (it != fpairs.end()) {
                  BitVec &bits = it->second;
                  if (bits.get(maxFormals*idx1 + idx2))
                    pairCanAlias = true;
                }
                // Is formalsAliasingGlobals non-intersecting?
                std::map<Symbol*, BitVec>::iterator it2;
                it = formalsAliasingGlobals.find(formal1);
                it2 = formalsAliasingGlobals.find(formal2);
                if (it != formalsAliasingGlobals.end() &&
                    it2 != formalsAliasingGlobals.end()) {
                  BitVec &bits1 = it->second;
                  BitVec &bits2 = it2->second;
                  BitVec intersect = bits1 & bits2;
                  if (intersect.any())
                    pairCanAlias = true;
                }

                if (pairCanAlias == false) {
                  noAliasArgs.push_back(formal2);
                }
              }
              formalIdx2++;
            }
            addNoAliasSetForFormal(formal1, noAliasArgs);
          }
        }
        formalIdx1++;
      }
    }
  }
}

static
void addNoAliasSetsInFn(FnSymbol* fn) {
}

void addNoAliasSets() {

  // First, mark ref arguments that definately don't alias,
  // as an interprocedural analysis.
  markNonAliasingRefArguments();

  // Then, construct alias sets at each scope defining array
  // variables, or each function body, and associate these with
  // PRIM_ARRAY_GET{_VALUE} primitives.
  forv_Vec(FnSymbol, fn, gFnSymbols) {
    addNoAliasSetsInFn(fn);
  }
}
