#include "cleanup.h"
#include "expr.h"
#include "stmt.h"
#include "symtab.h"


/******************************************************************************
 *** Apply With
 ***
 *** This traversal expands with-statements that appear in class
 *** definitions; it replaces them with the definitions in the
 *** included class.
 ***
 ***/

class ApplyWith : public Traversal {
 public:
  void preProcessStmt(Stmt* &stmt);
};

void ApplyWith::preProcessStmt(Stmt* &stmt) {
  if (WithStmt* with = dynamic_cast<WithStmt*>(stmt)) {
    if (TypeSymbol* symType = dynamic_cast<TypeSymbol*>(with->parentSymbol)) {
      if (ClassType* ctype = dynamic_cast<ClassType*>(symType->type)) {
	Stmt* with_replacement = with->getClass()->definition->copyList(ctype->classScope);
	Stmt::replace(stmt, with_replacement);
	return;
      }
    }
    USR_FATAL(stmt, "With statement is not in a class type definition");
  }
}


/******************************************************************************
 *** Insert This
 ***
 *** This traversal inserts "this" as the first parameter in bound
 *** functions.
 ***
 ***/

class InsertThis : public Traversal {
 public:
  void preProcessStmt(Stmt* &stmt);
};

void InsertThis::preProcessStmt(Stmt* &stmt) {
  if (TypeDefStmt* tds = dynamic_cast<TypeDefStmt*>(stmt)) {
    if (ClassType* ctype = dynamic_cast<ClassType*>(tds->type)) {
      Stmt* stmt = ctype->definition;

      while (stmt) {
	Stmt* next = nextLink(Stmt, stmt);
	if (FnDefStmt* method = dynamic_cast<FnDefStmt*>(stmt)) {
	  Symbol* this_insert = new ParamSymbol(PARAM_INOUT, "this", ctype);
	  this_insert->parentScope = method->fn->paramScope;
	  Symboltable::defineInScope(this_insert, method->fn->paramScope);
	  this_insert = appendLink(this_insert, method->fn->formals);
	  method->fn->formals = this_insert;
	  method->fn->_this = this_insert;
	}
	stmt = next;
      }
    }
  }
}


/******************************************************************************
 *** Resolve Easiest
 ***
 *** This traversal resolves unresolved symbols that are easy to
 *** resolve, i.e., top-level, non-functions.
 ***
 ***/

class ResolveEasiest : public Traversal {
 public:
  Expr* LastExpr;
  ResolveEasiest::ResolveEasiest(void);
  void preProcessExpr(Expr* &expr);
  void preProcessSymbol(Symbol* &sym);
};

ResolveEasiest::ResolveEasiest(void) {
  LastExpr = NULL;
}

static void resolve_type_helper(Type* &type) {
  if (dynamic_cast<UnresolvedType*>(type)) {
    Symbol* new_type = Symboltable::lookup(type->name->name);
    if (new_type && !new_type->isNull()) {
      if (!dynamic_cast<UnresolvedType*>(new_type->type)) {
	type = new_type->type;
      }
      else {
	resolve_type_helper(new_type->type);
	type = new_type->type;
      }
    }
    else {
      INT_FATAL(type, "Error resolving type");
    }
  }
  if (UserType* user_type = dynamic_cast<UserType*>(type)) {
    resolve_type_helper(user_type->definition);
  }
  if (ArrayType* array_type = dynamic_cast<ArrayType*>(type)) {
    resolve_type_helper(array_type->elementType);
  }
}

void ResolveEasiest::preProcessExpr(Expr* &expr) {
  if (CastExpr* cast_expr = dynamic_cast<CastExpr*>(expr)) {
    resolve_type_helper(cast_expr->newType);
  }
  LastExpr = expr;
}

void ResolveEasiest::preProcessSymbol(Symbol* &sym) {
  if (dynamic_cast<UnresolvedSymbol*>(sym)) {
    Symbol* new_sym = Symboltable::lookup(sym->name);
    if (new_sym) {
      sym = new_sym;
    }
  }
  resolve_type_helper(sym->type);
}



/******************************************************************************
 *** Resolve Easy
 ***
 *** This traversal resolves unresolved symbols that are easy to
 *** resolve, i.e., dot-expressions where the base expression is of
 *** known class type.
 ***
 ***/

class ResolveEasy : public Traversal {
 public:
  void preProcessExpr(Expr* &expr);
};

void ResolveEasy::preProcessExpr(Expr* &expr) {
  if (MemberAccess* member_access = dynamic_cast<MemberAccess*>(expr)) {
    if (ClassType* class_type = dynamic_cast<ClassType*>(member_access->base->typeInfo())) {
      Symbol* member = Symboltable::lookupInScope(member_access->member->name, class_type->classScope);
      member_access->member = member;
    }
    else {
      INT_FATAL(expr, "Error resolving dot-expression");
    }
  }
}


/******************************************************************************
 *** SpecializeParens
 ***
 *** This traversal changes all ParenOpExpr that are arrays to
 *** ArrayRef and changes all ParenOpExpr that are write, writeln, and
 *** read to IOCall.  Also, this traversal corrects constructors.
 ***
 ***/

class SpecializeParens : public Traversal {
 public:
  void preProcessExpr(Expr* &expr);
};

void SpecializeParens::preProcessExpr(Expr* &expr) {
  Expr* paren_replacement = NULL;
  if (ParenOpExpr* paren = dynamic_cast<ParenOpExpr*>(expr)) {
    if (dynamic_cast<ArrayType*>(paren->baseExpr->typeInfo())) {
      paren_replacement = new ArrayRef(paren->baseExpr, paren->argList);
    }
    else if (Variable* baseVar = dynamic_cast<Variable*>(paren->baseExpr)) {
      if (ClassType* ctype = dynamic_cast<ClassType*>(baseVar->var->type)) {
	paren_replacement = new FnCall(new Variable(ctype->constructor->fn), paren->argList);
      }
      else if (strcmp(baseVar->var->name, "write") == 0) {
	paren_replacement = new IOCall(IO_WRITE, paren->baseExpr, paren->argList);
      }
      else if (strcmp(baseVar->var->name, "writeln") == 0) {
	paren_replacement = new IOCall(IO_WRITELN, paren->baseExpr, paren->argList);
      }
      else if (strcmp(baseVar->var->name, "read") == 0) {
	paren_replacement = new IOCall(IO_READ, paren->baseExpr, paren->argList);
      }
    }
  }
  if (paren_replacement) {
    Expr::replace(expr, paren_replacement);
  }
}


/******************************************************************************
 *** Apply This
 ***
 *** This traversal changes all references to variables and functions
 *** defined in a class within the definitions of bound functions.
 *** They are changed to MemberAccess objects where the base is "this"
 *** and the member is the variable or function.
 ***
 ***/

class ApplyThis : public Traversal {
 public:
  ClassType* CurrentClass;
  ApplyThis::ApplyThis(void);
  void preProcessStmt(Stmt* &stmt);
  void postProcessStmt(Stmt* &stmt);
  void preProcessExpr(Expr* &expr);
};

ApplyThis::ApplyThis(void) {
  CurrentClass = NULL;
}

void ApplyThis::preProcessStmt(Stmt* &stmt) {
  if (TypeDefStmt* tds = dynamic_cast<TypeDefStmt*>(stmt)) {
    if (ClassType* ctype = dynamic_cast<ClassType*>(tds->type)) {
      CurrentClass = ctype;
    }
  }
}

void ApplyThis::postProcessStmt(Stmt* &stmt) {
  if (TypeDefStmt* tds = dynamic_cast<TypeDefStmt*>(stmt)) {
    if (dynamic_cast<ClassType*>(tds->type)) {
      CurrentClass = NULL;
    }
  }
}

void ApplyThis::preProcessExpr(Expr* &expr) {
  if (CurrentClass) {
    if (Variable* member = dynamic_cast<Variable*>(expr)) {
      // BLC: Steve, shouldn't this be a pointer compare rather than
      // a string compare?  Could the user declare their own this which
      // shadows the built-in variable?
      if (!strcmp(member->var->name, "this")) {
	return;
      }
      if (Symboltable::lookupInScope(member->var->name, 
				     CurrentClass->classScope)) {

	/* replacement of expr variable by memberaccess */
	if (FnSymbol* parentFn = dynamic_cast<FnSymbol*>(member->stmt->parentSymbol)) {
	  MemberAccess* repl = new MemberAccess(new Variable(parentFn->formals),
						member->var);
	  Expr::replace(expr, repl);
	}
	else {
	  INT_FATAL(expr, "Statement is not in method in ApplyThis");
	}
      }
    }
  }
}


/******************************************************************************/

void Cleanup::run(ModuleSymbol* moduleList) { 
  ResolveEasiest* resolve_easiest = new ResolveEasiest();
  ApplyWith* apply_with = new ApplyWith();
  InsertThis* insert_this = new InsertThis();
  ResolveEasy* resolve_easy = new ResolveEasy();
  SpecializeParens* specialize_parens = new SpecializeParens();
  ApplyThis* apply_this = new ApplyThis();
  ModuleSymbol* mod = moduleList;
  while (mod) {
    mod->startTraversal(resolve_easiest);
    mod->startTraversal(insert_this);
    mod->startTraversal(apply_with);
    mod->startTraversal(resolve_easy);
    mod->startTraversal(specialize_parens);
    mod->startTraversal(apply_this);
    mod = nextLink(ModuleSymbol, mod);
  }
}
