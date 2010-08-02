/* GDC -- D front-end for GCC
   Copyright (C) 2004 David Friedman
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
 
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
 
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
/*
  This file is based on dmd/tocsym.c.  Original copyright:

// Copyright (c) 1999-2002 by Digital Mars
// All Rights Reserved
// written by Walter Bright
// www.digitalmars.com
// License for redistribution is by either the Artistic License
// in artistic.txt, or the GNU General Public License in gnu.txt.
// See the included readme.txt for details.

*/

#include "d-gcc-includes.h"
#include <assert.h>

#include "total.h"
#include "mars.h"
#include "statement.h"
#include "aggregate.h"
#include "init.h"
#include "attrib.h"

#include "symbol.h"
#include "d-lang.h"
#include "d-codegen.h"

void slist_add(Symbol */*s*/)
{
}
void slist_reset()
{
}

/********************************* SymbolDeclaration ****************************/

SymbolDeclaration::SymbolDeclaration(Loc loc, Symbol *s, StructDeclaration *dsym)
    : Declaration(new Identifier(s->Sident, TOKidentifier))
{
    this->loc = loc;
    sym = s;
    this->dsym = dsym;
    storage_class |= STCconst;
}

Symbol *SymbolDeclaration::toSymbol()
{
    // Create the actual back-end value if not yet done
    if (! sym->Stree) {
	if (dsym)
	    dsym->toInitializer();
	assert(sym->Stree);
    }
    return sym;
}

/*************************************
 * Helper
 */

Symbol *Dsymbol::toSymbolX(const char *prefix, int sclass, type *t, const char *suffix)
{
    Symbol *s;
    char *id;
    char *n;

    n = mangle();
    id = (char *) alloca(2 + strlen(n) + sizeof(size_t) * 3 + strlen(prefix) + strlen(suffix) + 1);
    sprintf(id,"_D%s%"PRIuSIZE"%s%s", n, strlen(prefix), prefix, suffix);
    s = symbol_name(id, sclass, t);
    return s;
}

/*************************************
 */

Symbol *Dsymbol::toSymbol()
{
    fprintf(stderr, "Dsymbol::toSymbol() '%s', kind = '%s'\n", toChars(), kind());
    assert(0);		// BUG: implement
    return NULL;
}

/*********************************
 * Generate import symbol from symbol.
 */

Symbol *Dsymbol::toImport()
{
    if (!isym)
    {
	if (!csym)
	    csym = toSymbol();
	isym = toImport(csym);
    }
    return isym;
}

/*************************************
 */

Symbol *Dsymbol::toImport(Symbol * /*sym*/)
{
    // not used in GCC (yet?)
    return 0;
}



/* When using -combine, there may be name collisions on compiler-generated
   or extern(C) symbols. This only should only apply to private symbols.
   Otherwise, duplicate names are an error. */

static StringTable * uniqueNames = 0;
static void uniqueName(Declaration * d, tree t, const char * asm_name) {
    Dsymbol * p = d->toParent2(); 
    const char * out_name = asm_name;
    char * alloc_name = 0;

    FuncDeclaration * f = d->isFuncDeclaration();
    VarDeclaration * v = d->isVarDeclaration();

    /* Check cases for which it is okay to have a duplicate symbol name.
       Otherwise, duplicate names are an error and the condition will
       be caught by the assembler. */
    if (!(f && ! f->fbody) &&
	!(v && (v->storage_class & STCextern)) &&
	(
	 // Static declarations in different scope statements
	 (p && p->isFuncDeclaration()) ||

	 // Top-level duplicate names are okay if private.
	 ((!p || p->isModule()) && d->protection == PROTprivate) ))
    {
	StringValue * sv;

	// Assumes one assembler output file per compiler run.  Otherwise, need
	// to reset this for each file.
	if (! uniqueNames)
	    uniqueNames = new StringTable;
	sv = uniqueNames->update(asm_name, strlen(asm_name));
	if (sv->intvalue) {
	    out_name = alloc_name = d_asm_format_private_name(asm_name, sv->intvalue );
	}
	sv->intvalue++;
    }

    tree id;
#if D_GCC_VER >= 43
    /* Only FUNCTION_DECLs and VAR_DECLs for variables with static storage
       duration need a mangled DECL_ASSEMBLER_NAME. */
    if (f || (v && (v->protection == PROTpublic || v->storage_class & (STCstatic | STCextern))))
    {
	id = targetm.mangle_decl_assembler_name(t, get_identifier(out_name));
    }
    else
    /* Mangling will be left to the decision of the backend */
#endif
    id = get_identifier(out_name);

    SET_DECL_ASSEMBLER_NAME(t, id);

    if (alloc_name)
	free(alloc_name);
}


/*************************************
 */

Symbol *VarDeclaration::toSymbol()
{
    if (! csym) {
	tree var_decl;

	// For field declaration, it is possible for toSymbol to be called
	// before the parent's toCtype()
	if (storage_class & STCfield) {
	    AggregateDeclaration * parent_decl = toParent()->isAggregateDeclaration();
	    assert(parent_decl);
	    parent_decl->type->toCtype();
	    assert(csym);
	    return csym;
	}

	const char * ident_to_use;
	if (isDataseg())
	    ident_to_use = mangle();
	else
	    ident_to_use = ident->string;

	enum tree_code decl_kind;

#if ! V2
	bool is_template_const = false;
	// Logic copied from toobj.c VarDeclaration::toObjFile

	Dsymbol * parent = this->toParent();
#if 1	/* private statics should still get a global symbol, in case
	 * another module inlines a function that references it.
	 */
	if (/*protection == PROTprivate ||*/
	    !parent || parent->ident == NULL || parent->isFuncDeclaration())
	{
	    // nothing
	}
	else
#endif
	{
	    do
	    {
		/* Global template data members need to be in comdat's
		 * in case multiple .obj files instantiate the same
		 * template with the same types.
		 */
		if (parent->isTemplateInstance() && !parent->isTemplateMixin())
		{
		    /* These symbol constants have already been copied,
		     * so no reason to output them.
		     * Note that currently there is no way to take
		     * the address of such a const.
		     */
		    if (isConst() && type->toBasetype()->ty != Tsarray &&
			init && init->isExpInitializer())
		    {
			is_template_const = true;
			break;
		    }

		    break;
		}
		parent = parent->parent;
	    } while (parent);
	}
#endif
	
	if (storage_class & STCparameter)
	    decl_kind = PARM_DECL;
	else if (
#if V2
# if 0
		 // from VarDeclaration::getConstInitializer
		 (isConst() || isInvariant()) && (storage_class & STCinit)
# else
		 (storage_class & STCmanifest)
# endif
#else
		 is_template_const ||
		 (
		   isConst()
		   && ! gen.isDeclarationReferenceType( this ) &&
		   type->isscalar() && ! isDataseg()
		 )
#endif
		 )
	    decl_kind = CONST_DECL;
	else
	    decl_kind = VAR_DECL;

	var_decl = build_decl(decl_kind, get_identifier(ident_to_use),
	    gen.trueDeclarationType( this ));

	csym = new Symbol();
	csym->Stree = var_decl;

	if (decl_kind != CONST_DECL) {	    
	    if (isDataseg())
		uniqueName(this, var_decl, ident_to_use);
	    if (c_ident)
		SET_DECL_ASSEMBLER_NAME(var_decl, get_identifier(c_ident->string));
	}
	dkeep(var_decl);
	g.ofile->setDeclLoc(var_decl, this);
	if ( decl_kind == VAR_DECL ) {
	    g.ofile->setupSymbolStorage(this, var_decl);
	    //DECL_CONTEXT( var_decl ) = gen.declContext(this);//EXPERkinda
	} else if (decl_kind == PARM_DECL)  {
	    /* from gcc code: Some languages have different nominal and real types.  */
	    // %% What about DECL_ORIGINAL_TYPE, DECL_ARG_TYPE_AS_WRITTEN, DECL_ARG_TYPE ?
	    DECL_ARG_TYPE( var_decl ) = TREE_TYPE (var_decl);
	    
	    DECL_CONTEXT( var_decl ) = gen.declContext(this);
	    assert( TREE_CODE(DECL_CONTEXT( var_decl )) == FUNCTION_DECL );
	} else if (decl_kind == CONST_DECL) {
	    /* Not sure how much of an optimization this is... It is needed
	       for foreach loops on tuples which 'declare' the index variable
	       as a constant for each iteration. */
	    Expression * e = NULL;

	    if (init)
	    {
		if (! init->isVoidInitializer())
		{
		    e = init->toExpression();
		    assert(e != NULL);
		}
	    }
	    else
		e = type->defaultInit();

	    if (e)
	    {
		DECL_INITIAL( var_decl ) = g.irs->assignValue(e, this);
		if (! DECL_INITIAL(var_decl))
		    DECL_INITIAL( var_decl ) = e->toElem(g.irs);
	    }
	}

	// Can't set TREE_STATIC, etc. until we get to toObjFile as this could be
	// called from a varaible in an imported module
	// %% (out const X x) doesn't mean the reference is const...
	if (
#if V2
	    (isConst() || isInvariant()) && (storage_class & STCinit)
#else
	    isConst()
#endif
	    && ! gen.isDeclarationReferenceType( this )) {
	    // %% CONST_DECLS don't have storage, so we can't use those,
	    // but it would be nice to get the benefit of them (could handle in
	    // VarExp -- makeAddressOf could switch back to the VAR_DECL

	    // if ( typs->isscalar() ) CONST_DECL...
	    TREE_READONLY( var_decl ) = 1;

	    // can at least do this...
	    //  const doesn't seem to matter for aggregates, so prevent problems..
	    if ( type->isscalar() ) {
		TREE_CONSTANT( var_decl ) = 1;
	    }
	}

#if D_GCC_VER < 40
	// With cgraph, backend can figure it out (improves inline nested funcs?)
	if (
#if V2
	    nestedrefs.dim
#else
	    nestedref
#endif
	    && decl_kind != CONST_DECL) {
	    DECL_NONLOCAL( var_decl ) = 1;
	    TREE_ADDRESSABLE( var_decl ) = 1;
	}
	
	TREE_USED( var_decl ) = 1;
#endif


#ifdef TARGET_DLLIMPORT_DECL_ATTRIBUTES
	// Have to test for import first
	if (isImportedSymbol())
	{
	    gen.addDeclAttribute( var_decl, "dllimport" );
	    DECL_DLLIMPORT_P( var_decl ) = 1;        
	}
	else if (isExport())
	    gen.addDeclAttribute( var_decl, "dllexport" );
#endif
    }
    return csym;
}

/*************************************
 */

Symbol *ClassInfoDeclaration::toSymbol()
{
    return cd->toSymbol();
}

/*************************************
 */

Symbol *ModuleInfoDeclaration::toSymbol()
{
    return mod->toSymbol();
}

/*************************************
 */

Symbol *TypeInfoDeclaration::toSymbol()
{
    if ( ! csym ) {
	VarDeclaration::toSymbol();
	
	// This variable is the static initialization for the
	// given TypeInfo.  It is the actual data, not a reference
	assert( TREE_CODE( TREE_TYPE( csym->Stree )) == REFERENCE_TYPE );
	TREE_TYPE( csym->Stree ) = TREE_TYPE( TREE_TYPE( csym->Stree ));

	/* DMD makes typeinfo decls one-only by doing:

	    s->Sclass = SCcomdat;

	   in TypeInfoDeclaration::toObjFile.  The difference is
	   that, in gdc, built-in typeinfo will be referenced as
	   one-only.
	*/


	D_DECL_ONE_ONLY( csym->Stree ) = 1;
	g.ofile->makeDeclOneOnly( csym->Stree );
    }
    return csym;
}

/*************************************
 */

Symbol *FuncAliasDeclaration::toSymbol()
{
    return funcalias->toSymbol();
}

/*************************************
 */

// returns a FUNCTION_DECL tree
Symbol *FuncDeclaration::toSymbol()
{
    if (! csym) {	
	csym  = new Symbol();
	
	if (! isym) {
	    tree id;
	    //struct prod_token_parm_item* parm;
	    //tree type_node;
	    Type * func_type = tintro ? tintro : type;
	    tree fn_decl;
	    char * mangled_ident_str = 0;
	    AggregateDeclaration * agg_decl;

	    if (ident) {
		id = get_identifier(ident->string);
	    } else {
		// This happens for assoc array foreach bodies

		// Not sure if idents are strictly necc., but announce_function
		//  dies without them.

		// %% better: use parent name

		static unsigned unamed_seq = 0;
		char buf[64];
		sprintf(buf, "___unamed_%u", ++unamed_seq);//%% sprintf
		id = get_identifier(buf);
	    }

	    tree fn_type = func_type->toCtype();
	    dkeep(fn_type); /* TODO: fix this. we need to keep the type because
			       the creation of a method type below leaves this fn_type
			       unreferenced. maybe lang_specific.based_on */

	    tree vindex = NULL_TREE;
	    if (isNested()) {
		/* Even if DMD-style nested functions are not implemented, add an
		   extra argument to be compatible with delegates. */

		// irs->functionType(func_type, Type::tvoid);
		fn_type = build_method_type(void_type_node, fn_type);
	    } else if ( ( agg_decl = isMember() ) ) {
		// Do this even if there is no debug info.  It is needed to make
		// sure member functions are not called statically
		if (isThis()) {
		    tree method_type = build_method_type(TREE_TYPE(agg_decl->handle->toCtype()), fn_type);
		    TYPE_ATTRIBUTES( method_type ) = TYPE_ATTRIBUTES( fn_type );
		    fn_type = method_type;

		    if (isVirtual())
			vindex = size_int(vtblIndex);
		}
	    } else if (isMain() && func_type->nextOf()->toBasetype()->ty == Tvoid) {
		fn_type = build_function_type(integer_type_node, TYPE_ARG_TYPES(fn_type));
	    }

	    // %%CHECK: is it okay for static nested functions to have a FUNC_DECL context?
	    // seems okay so far...

	    fn_decl = build_decl( FUNCTION_DECL, id, fn_type );
	    dkeep(fn_decl);
	    if (ident) {
		mangled_ident_str = mangle();
		uniqueName(this, fn_decl, mangled_ident_str);
	    }
	    if (c_ident)
		SET_DECL_ASSEMBLER_NAME(fn_decl, get_identifier(c_ident->string));
	    // %% What about DECL_SECTION_NAME ?
	    //DECL_ARGUMENTS(fn_decl) = NULL_TREE; // Probably don't need to do this until toObjFile
	    DECL_CONTEXT (fn_decl) = gen.declContext(this); //context;
	    if (vindex) {
		DECL_VINDEX    (fn_decl) = vindex;
		DECL_VIRTUAL_P (fn_decl) = 1;
	    }
	    if (! gen.functionNeedsChain(this)
    #if D_GCC_VER >= 40
		// gcc 4.0: seems to be an error to set DECL_NO_STATIC_CHAIN on a toplevel function
		// (tree-nest.c:1282:convert_all_function_calls)
		&& decl_function_context(fn_decl)
    #endif
		) {
		// Prevent backend from thinking this is a nested function.
		DECL_NO_STATIC_CHAIN( fn_decl ) = 1;
	    }
	    else
	    {
		/* If a template instance has a nested function (because
		   a template argument is a local variable), the nested
		   function may not have its toObjFile called before the
		   outer function is finished.  GCC requires that nested
		   functions be finished first so we need to arrange for
		   toObjFile to be called earlier.

		   It may be possible to defer calling the outer
		   function's cgraph_finalize_function until all nested
		   functions are finished, but this will only work for
		   GCC >= 3.4. */
		Dsymbol * p = this->parent;
		FuncDeclaration * outer_func = NULL;
		bool is_template_member = false;
		while (p)
		{
		    if (p->isTemplateInstance() && ! p->isTemplateMixin())
		    {
			is_template_member = true;
		    }
		    else if ( (outer_func = p->isFuncDeclaration()) )
			break;
		    p = p->parent;
		}
		if (is_template_member && outer_func)
		{
		    Symbol * outer_sym = outer_func->toSymbol();
		    assert(outer_sym->outputStage != Finished);
		    if (! outer_sym->otherNestedFuncs)
			outer_sym->otherNestedFuncs = new FuncDeclarations;
		    outer_sym->otherNestedFuncs->push(this);
		}
	    }

	    /* For now, inline asm means we can't inline (stack wouldn't be
	       what was expected and LDASM labels aren't unique.)
	       TODO: If the asm consists entirely
	       of extended asm, we can allow inlining. */
	    if (inlineAsm) {
		DECL_UNINLINABLE(fn_decl) = 1;
	    } else {
#if D_GCC_VER >= 40
		// see grokdeclarator in c-decl.c
		if (flag_inline_trees == 2 && fbody /* && should_emit? */)
		    DECL_INLINE (fn_decl) = 1;
#endif
	    }

	    if (naked) {
		D_DECL_NO_FRAME_POINTER( fn_decl ) = 1;
		DECL_NO_INSTRUMENT_FUNCTION_ENTRY_EXIT( fn_decl ) = 1;

		/* Need to do this or GCC will set up a frame pointer with -finline-functions.
		   Must have something to do with defered processing -- after we turn
		   flag_omit_frame_pointer back on. */
		DECL_UNINLINABLE( fn_decl ) = 1;
	    }

#ifdef TARGET_DLLIMPORT_DECL_ATTRIBUTES
	    // Have to test for import first
	    if (isImportedSymbol())
	    {
		gen.addDeclAttribute( fn_decl, "dllimport" );
		DECL_DLLIMPORT_P( fn_decl ) = 1;
	    }
	    else if (isExport())
		gen.addDeclAttribute( fn_decl, "dllexport" );
#endif

	    g.ofile->setDeclLoc(fn_decl, this);
	    g.ofile->setupSymbolStorage(this, fn_decl);
	    if (! ident)
		TREE_PUBLIC( fn_decl ) = 0;

	    TREE_USED (fn_decl) = 1; // %% Probably should be a little more intelligent about this

	    // if -mrtd is passed, how to handle this? handle in parsing or do
	    // we go back and find out if linkage was specified
	    switch (linkage)
	    {
		case LINKwindows:
		    gen.addDeclAttribute(fn_decl, "stdcall");
		    // The stdcall attribute also needs to be set on the function type.
		    assert( ((TypeFunction *) func_type)->linkage == LINKwindows );
		    break;
		case LINKpascal:
		    // stdcall and reverse params?
		    break;
		case LINKc:
		    // %% hack: on darwin (at least) using a DECL_EXTERNAL (IRState::getLibCallDecl)
		    // and TREE_STATIC FUNCTION_DECLs causes the stub label to be output twice.  This
		    // is a work around.  This doesn't handle the case in which the normal
		    // getLibCallDecl has already bee created and used.  Note that the problem only
		    // occurs with function inlining is used.
		    gen.replaceLibCallDecl(this);
		    break;
		case LINKd:
		    // %% If x86, regparm(1)
		    // not sure if reg struct return 
		    break;
		case LINKcpp:
		    break;
		default:
		    fprintf(stderr, "linkage = %d\n", linkage);
		    assert(0);
	    }
	    
	    csym->Sident = mangled_ident_str; // save for making thunks
	    csym->Stree = fn_decl;

	    gen.maybeSetUpBuiltin(this);
	} else {
	    csym->Stree = isym->Stree;
	}
    }
    return csym;
}

#define D_PRIVATE_THUNKS 1

/*************************************
 */
Symbol *FuncDeclaration::toThunkSymbol(target_ptrdiff_t offset)
{
    Symbol *sthunk;
    Thunk * thunk;

    toSymbol();
    
    /* If the thunk is to be static (that is, it is being emitted in this
       module, there can only be one FUNCTION_DECL for it.   Thus, there
       is a list of all thunks for a given function. */
    if ( ! csym->thunks )
	csym->thunks = new Array;
    Array & thunks = * csym->thunks;
    bool found = false;
	
    for (unsigned i = 0; i < thunks.dim; i++) {
	thunk = (Thunk *) thunks.data[i];
	if (thunk->offset == offset) {
	    found = true;
	    break;
	}
    }

    if (! found) {
	thunk = new Thunk;
	thunk->offset = offset;
	thunks.push(thunk);
    }

    if ( ! thunk->symbol ) {
	char *id;
	char *n;
	//type *t;

	n = csym->Sident; // dmd uses 'sym' -- not sure what that is...
	id = (char *) alloca(8 + 5 + strlen(n) + 1);
	sprintf(id,"__t%"PRIdTSIZE"_%s", offset, n);
	sthunk = symbol_calloc(id);
	slist_add(sthunk);
	/* todo: could use anonymous names like DMD, with ASM_FORMAT_RRIVATE_NAME*/
	//sthunk = symbol_generate(SCstatic, csym->Stype);
	//sthunk->Sflags |= SFLimplem;

	tree target_func_decl = csym->Stree;
	tree thunk_decl = build_decl(FUNCTION_DECL, get_identifier(id), TREE_TYPE( target_func_decl ));
	dkeep(thunk_decl);
	sthunk->Stree = thunk_decl;

	//SET_DECL_ASSEMBLER_NAME(thunk_decl, DECL_NAME(thunk_decl));//old
	DECL_CONTEXT(thunk_decl) = DECL_CONTEXT(target_func_decl); // from c++...
	TREE_READONLY(thunk_decl) = TREE_READONLY(target_func_decl);
	TREE_THIS_VOLATILE(thunk_decl) = TREE_THIS_VOLATILE(target_func_decl);

#ifdef D_PRIVATE_THUNKS
	TREE_PRIVATE(thunk_decl) = 1;
#else
	/* Due to changes in the assembler, it is not possible to emit
	   a private thunk that refers to an external symbol.
	   http://lists.gnu.org/archive/html/bug-binutils/2005-05/msg00002.html
	*/
	DECL_EXTERNAL(thunk_decl) = DECL_EXTERNAL(target_func_decl);
	TREE_STATIC(thunk_decl) = TREE_STATIC(target_func_decl);
	TREE_PRIVATE(thunk_decl) = TREE_PRIVATE(target_func_decl);
	TREE_PUBLIC(thunk_decl) = TREE_PUBLIC(target_func_decl);
#endif
	
	DECL_ARTIFICIAL(thunk_decl) = 1;
	DECL_IGNORED_P(thunk_decl) = 1;
	DECL_NO_STATIC_CHAIN(thunk_decl) = 1;
	DECL_INLINE(thunk_decl) = 0;
	DECL_DECLARED_INLINE_P(thunk_decl) = 0;
	//needed on some targets to avoid "causes a section type conflict"
	D_DECL_ONE_ONLY(thunk_decl) = D_DECL_ONE_ONLY(target_func_decl);
	if ( D_DECL_ONE_ONLY(thunk_decl) )
	    g.ofile->makeDeclOneOnly(thunk_decl);

	TREE_ADDRESSABLE(thunk_decl) = 1;
	TREE_USED (thunk_decl) = 1;

#ifdef D_PRIVATE_THUNKS
	//g.ofile->prepareSymbolOutput(sthunk);
	g.ofile->doThunk(thunk_decl, target_func_decl, offset);
#else
	if ( TREE_STATIC(thunk_decl) )	
	    g.ofile->doThunk(thunk_decl, target_func_decl, offset);
#endif

	thunk->symbol = sthunk;
    }
    return thunk->symbol;
}


/****************************************
 * Create a static symbol we can hang DT initializers onto.
 */

Symbol *static_sym()
{
    Symbol * s = symbol_tree(NULL_TREE);
    //OLD//s->Sfl = FLstatic_sym;
    /* Before GCC 4.0, it was possible to take the address of a CONSTRUCTOR
       marked TREE_STATIC and the backend would output the data as an
       anonymous symbol.  This doesn't work in 4.0.  To keep things, simple,
       the same method is used for <4.0 and >= 4.0. */
    // Can't build the VAR_DECL because the type is unknown
    slist_add(s);
    return s;
}

/**************************************
 * Fake a struct symbol.
 */

// Not used in GCC
/*
Classsym *fake_classsym(char * name)
{
    return 0;
}
*/

/*************************************
 * Create the "ClassInfo" symbol
 */

Symbol *ClassDeclaration::toSymbol()
{
    if (! csym)
    {
	tree decl;
	csym = toSymbolX("__Class", SCextern, 0, "Z");
	slist_add(csym);
	decl = build_decl( VAR_DECL, get_identifier( csym->Sident ),
	    TREE_TYPE( ClassDeclaration::classinfo->type->toCtype() )); // want the RECORD_TYPE, not the REFERENCE_TYPE
	csym->Stree = decl;
	dkeep(decl);
 
	g.ofile->setupStaticStorage(this, decl);
	g.ofile->setDeclLoc(decl, this);

	TREE_CONSTANT( decl ) = 0; // DMD puts this into .data, not .rodata...
	TREE_READONLY( decl ) = 0;
    }
    return csym;
}

/*************************************
 * Create the "InterfaceInfo" symbol
 */

Symbol *InterfaceDeclaration::toSymbol()
{
    if (!csym)
    {
	csym = ClassDeclaration::toSymbol();
	tree decl = csym->Stree;
	
	Symbol * temp_sym = toSymbolX("__Interface", SCextern, 0, "Z");
	DECL_NAME( decl ) = get_identifier( temp_sym->Sident );
	delete temp_sym;

	TREE_CONSTANT( decl ) = 1; // Interface ClassInfo images are in .rodata, but classes arent..?
    }
    return csym;
}

/*************************************
 * Create the "ModuleInfo" symbol
 */

Symbol *Module::toSymbol()
{
    if (!csym)
    {
	Type * some_type;

	/* This causes problems  .. workaround
	   is to call moduleinfo->toCtype() before we start processing
	   decls in genobjfile. ...
	   
	    if (moduleinfo) {
		some_type = moduleinfo->type;
	    } else {
		some_type = gen.getObjectType();
	    }
	*/
	some_type = gen.getObjectType();
	
	csym = toSymbolX("__ModuleInfo", SCextern, 0, "Z");
	slist_add(csym);

	tree decl = build_decl(VAR_DECL, get_identifier(csym->Sident),
	    TREE_TYPE(some_type->toCtype())); // want the RECORD_TYPE, not the REFERENCE_TYPE
	g.ofile->setDeclLoc(decl, this);
	csym->Stree = decl;
	
	dkeep(decl);
	
	g.ofile->setupStaticStorage(this, decl);
	
	TREE_CONSTANT( decl ) = 0; // *not* readonly, moduleinit depends on this
	TREE_READONLY( decl ) = 0; // Not an lvalue, tho
    }
    return csym;
}

/*************************************
 * This is accessible via the ClassData, but since it is frequently
 * needed directly (like for rtti comparisons), make it directly accessible.
 */

Symbol *ClassDeclaration::toVtblSymbol()
{
    if (!vtblsym)
    {
	tree decl;
	
	vtblsym = toSymbolX("__vtbl", SCextern, 0, "Z");
	slist_add(vtblsym);

	/* The DECL_INITIAL value will have a different type object from the
	   VAR_DECL.  The back end seems to accept this. */
	TypeSArray * vtbl_type = new TypeSArray(Type::tvoid->pointerTo(),
	    new IntegerExp(loc, vtbl.dim, Type::tindex));

	decl = build_decl( VAR_DECL, get_identifier( vtblsym->Sident ), vtbl_type->toCtype() );
	vtblsym->Stree = decl;
	dkeep(decl);

	g.ofile->setupStaticStorage(this, decl);
	g.ofile->setDeclLoc(decl, this);

	TREE_READONLY( decl ) = 1;
	TREE_CONSTANT( decl ) = 1;
	TREE_ADDRESSABLE( decl ) = 1;
	// from cp/class.c
	DECL_CONTEXT (decl) =  TREE_TYPE( type->toCtype() );
	DECL_VIRTUAL_P (decl) = 1;
	DECL_ALIGN (decl) = TARGET_VTABLE_ENTRY_ALIGN;
    }
    return vtblsym;
}

/**********************************
 * Create the static initializer for the struct/class.
 */

/* Because this is called from the front end (mtype.cc:TypeStruct::defaultInit()),
   we need to hold off using back-end stuff until the toobjfile phase.

   Specifically, it is not safe create a VAR_DECL with a type from toCtype()
   because there may be unresolved recursive references.
   StructDeclaration::toObjFile calls toInitializer without ever calling
   SymbolDeclaration::toSymbol, so we just need to keep checking if we
   are in the toObjFile phase.
*/

Symbol *AggregateDeclaration::toInitializer()
{
    Symbol *s;

    if (!sinit)
    {
	s = toSymbolX("__init", SCextern, 0, "Z");
	slist_add(s);
	sinit = s;
    }
    if (! sinit->Stree && g.ofile != NULL)
    {
	tree struct_type = type->toCtype();
	if ( POINTER_TYPE_P( struct_type ) )
	    struct_type = TREE_TYPE( struct_type ); // for TypeClass, want the RECORD_TYPE, not the REFERENCE_TYPE
	tree t = build_decl(VAR_DECL, get_identifier(sinit->Sident), struct_type);
	sinit->Stree = t;
	dkeep(t);

	g.ofile->setupStaticStorage(this, t);
	g.ofile->setDeclLoc(t, this);

	// %% what's the diff between setting this stuff on the DECL and the
	// CONSTRUCTOR itself?

	TREE_ADDRESSABLE( t ) = 1;
	TREE_READONLY( t ) = 1;
	TREE_CONSTANT( t ) = 1;
	DECL_CONTEXT( t ) = 0; // These are always global
    }
    return sinit;
}

Symbol *TypedefDeclaration::toInitializer()
{
    Symbol *s;

    if (!sinit)
    {
	s = toSymbolX("__init", SCextern, 0, "Z");
	s->Sfl = FLextern;
	s->Sflags |= SFLnodebug;
	slist_add(s);
	sinit = s;
	sinit->Sdt = ((TypeTypedef *)type)->sym->init->toDt();
    }
    return sinit;
}

Symbol *EnumDeclaration::toInitializer()
{
    Symbol *s;

    if (!sinit)
    {
	Identifier *ident_save = ident;
	if (!ident)
	{   static int num;
	    char name[6 + sizeof(num) * 3 + 1];
	    snprintf(name, sizeof(name), "__enum%d", ++num);
	    ident = Lexer::idPool(name);
	}
	s = toSymbolX("__init", SCextern, 0, "Z");
	ident = ident_save;
	s->Sfl = FLextern;
	s->Sflags |= SFLnodebug;
	slist_add(s);
	sinit = s;
    }
    if (! sinit->Stree && g.ofile != NULL)
    {
	tree t = build_decl(VAR_DECL, get_identifier(sinit->Sident), type->toCtype());
	sinit->Stree = t;
	dkeep(t);

	g.ofile->setupStaticStorage(this, t);
	g.ofile->setDeclLoc(t, this);
	TREE_CONSTANT( t ) = 1;
	TREE_READONLY( t ) = 1;
	DECL_CONTEXT( t ) = 0;
    }
    return sinit;
}


/******************************************
 */

Symbol *Module::toModuleAssert()
{
    // Not used in GCC
    return 0;
}

/******************************************
 */

Symbol *Module::toModuleArray()
{
    // Not used in GCC (all array bounds checks are inlined)
    return 0;
}

/********************************************
 * Determine the right symbol to look up
 * an associative array element.
 * Input:
 *	flags	0	don't add value signature
 *		1	add value signature
 */

Symbol *TypeAArray::aaGetSymbol(const char *func, int flags)
{
    // This is not used in GCC (yet?)
    return 0;
}

