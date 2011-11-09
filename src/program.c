/*
 * PLUTO: An automatic parallelizer and locality optimizer
 * 
 * Copyright (C) 2007-2008 Uday Kumar Bondhugula
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * A copy of the GNU General Public Licence can be found in the file
 * `LICENSE' in the top-level directory of this distribution. 
 *
 * program.c
 *
 * This file contains functions that do the job interfacing the PLUTO 
 * core to the frontend and related matters
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>

#include "pluto.h"
#include "math_support.h"
#include "program.h"

#include "scoplib/statement.h"
#include "scoplib/access.h"

#include <isl/map.h>
#include <isl/mat.h>
#include <isl/set.h>
#include <isl/flow.h>
#include <isl/union_map.h>

/* Return a copy of the statement */
/* NO LONGER MAINTAINED: do not use */
Stmt *pluto_stmt_dup(const Stmt *src)
{
    Stmt *dest = pluto_stmt_alloc(src->dim, src->domain);

    *dest = *src;

    dest->trans = pluto_matrix_dup(src->trans);

    return dest;
}


PlutoMatrix *scoplib_matrix_to_pluto_matrix(scoplib_matrix_p smat)
{
    int i, j;

    PlutoMatrix *mat;

    mat = pluto_matrix_alloc(smat->NbRows, smat->NbColumns);
    for (i=0; i<smat->NbRows; i++)  {
        for (j=0; j<smat->NbColumns; j++)  {
            mat->val[i][j] = smat->p[i][j];
        }
    }

    return mat;
}


PlutoConstraints *scoplib_matrix_to_pluto_constraints(scoplib_matrix_p clanMatrix)
{
    int i, j;
    PlutoConstraints *cst;

    cst = pluto_constraints_alloc(clanMatrix->NbRows, clanMatrix->NbColumns-1);
    cst->nrows = clanMatrix->NbRows;

    for (i=0; i<clanMatrix->NbRows; i++)   {
        cst->is_eq[i] = (clanMatrix->p[i][0] == 0);
        for (j=0; j<cst->ncols; j++)   {
            cst->val[i][j] = (int) clanMatrix->p[i][j+1];
        }
    }
    return cst;
}


PlutoConstraints *candl_matrix_to_pluto_constraints(const CandlMatrix *candlMatrix)
{
    int i, j;
    PlutoConstraints *cst;

    cst = pluto_constraints_alloc(candlMatrix->NbRows, candlMatrix->NbColumns-1);
    cst->nrows = candlMatrix->NbRows;
    cst->ncols = candlMatrix->NbColumns-1;

    for (i=0; i<candlMatrix->NbRows; i++)   {
        if (candlMatrix->p[i][0] == 0) {
            cst->is_eq[i] = 1;
        }else{
            cst->is_eq[i] = 0;
        }

        for (j=0; j<cst->ncols; j++)   {
            cst->val[i][j] = (int) candlMatrix->p[i][j+1];
        }
    }

    // pluto_matrix_print(stdout, cst);

    return cst;
}



/* Get the position of this access given a CandlStmt access matrix
 * (concatenated)
 * ref: starting row for a particular access in concatenated rows of
 * access functions
 * Return the position of this access in the list  */
static int get_access_position(CandlMatrix *accesses, int ref)
{
    int num, i;

    num = -1;
    for (i=0; i<=ref; i++)  {
        if (accesses->p[i][0] != 0)   {
            num++;
        }
    }
    assert(num >= 0);
    return num;
}


/* Read dependences from candl structures */
static Dep *deps_read(CandlDependence *candlDeps, PlutoProg *prog)
{
    int i, ndeps;
    Dep *deps;
    int npar = prog->npar;
    Stmt **stmts = prog->stmts;

    ndeps = candl_num_dependences(candlDeps);

    deps = (Dep *) malloc(ndeps*sizeof(Dep));

    CandlDependence *candl_dep = candlDeps;

    candl_dep = candlDeps;

    IF_DEBUG(candl_dependence_pprint(stdout, candl_dep));

    /* Dependence polyhedra information */
    for (i=0; i<ndeps; i++)  {

        Dep *dep = &deps[i];

        dep->id = i;

        dep->type = candl_dep->type;

        dep->src = candl_dep->source->label;
        dep->dest = candl_dep->target->label;

        //candl_matrix_print(stdout, candl_dep->domain);
        dep->dpolytope = candl_matrix_to_pluto_constraints(candl_dep->domain);

        switch (dep->type) {
            case CANDL_RAW: 
                dep->src_acc = stmts[dep->src]->writes[
                    get_access_position(candl_dep->source->written, candl_dep->ref_source)];
                dep->dest_acc = stmts[dep->dest]->reads[
                    get_access_position(candl_dep->target->read, candl_dep->ref_target)];
                break;
            case CANDL_WAW: 
                dep->src_acc = stmts[dep->src]->writes[
                    get_access_position(candl_dep->source->written, candl_dep->ref_source)];
                dep->dest_acc = stmts[dep->dest]->writes[
                    get_access_position(candl_dep->target->written, candl_dep->ref_target)];
                break;
            case CANDL_WAR: 
                dep->src_acc = stmts[dep->src]->reads[
                    get_access_position(candl_dep->source->read, candl_dep->ref_source)];
                dep->dest_acc = stmts[dep->dest]->writes[
                    get_access_position(candl_dep->target->written, candl_dep->ref_target)];
                break;
            case CANDL_RAR: 
                dep->src_acc = stmts[dep->src]->reads[
                    get_access_position(candl_dep->source->read, candl_dep->ref_source)];
                dep->dest_acc = stmts[dep->dest]->reads[
                    get_access_position(candl_dep->target->read, candl_dep->ref_target)];
                break;
            default:
                assert(0);
        }

        /* Get rid of rows that are all zero */
        int r, c;
        bool *remove = (bool *) malloc(sizeof(bool)*dep->dpolytope->nrows);
        for (r=0; r<dep->dpolytope->nrows; r++) {
            for (c=0; c<dep->dpolytope->ncols; c++) {
                if (dep->dpolytope->val[r][c] != 0) {
                    break;
                }
            }
            if (c == dep->dpolytope->ncols) {
                remove[r] = true;
            }else{
                remove[r] = false;
            }
        }
        int orig_nrows = dep->dpolytope->nrows;
        int del_count = 0;
        for (r=0; r<orig_nrows; r++) {
            if (remove[r])  {
                pluto_constraints_remove_row(dep->dpolytope, r-del_count);
                del_count++;
            }
        }
        free(remove);


        int src_dim = stmts[dep->src]->dim;
        int target_dim = stmts[dep->dest]->dim;

        assert(candl_dep->domain->NbColumns-1 == src_dim+target_dim+npar+1);

        /* Initialize other fields used for auto transform */
        dep->satisfied = false;
        dep->satisfaction_level = -1;

        candl_dep = candl_dep->next;
    }

    return deps;
}

void pluto_dep_print(FILE *fp, Dep *dep)
{
    fprintf(fp, "--- Dep %d from S%d to S%d, Type: ",
            dep->id+1, dep->src+1, dep->dest+1);

    switch (dep->type) {
        case CANDL_UNSET : fprintf(fp, "UNSET"); break;
        case CANDL_RAW   : fprintf(fp, "RAW")  ; break;
        case CANDL_WAR   : fprintf(fp, "WAR")  ; break;
        case CANDL_WAW   : fprintf(fp, "WAW")  ; break;
        case CANDL_RAR   : fprintf(fp, "RAR")  ; break;
        default : fprintf(fp, "unknown"); break;
    }

    fprintf(fp, "\n");

    fprintf(fp, "Dependence polyhedron\n");
    pluto_constraints_print(fp, dep->dpolytope);
    fprintf(fp, "\n");
}


void pluto_deps_print(FILE *fp, Dep *deps, int ndeps)
{
    int i;
    for (i=0; i<ndeps; i++) {
        pluto_dep_print(fp, &deps[i]);
    }
}


/* Read statement info from scoplib structures (nvar: max domain dim) */
static Stmt **scoplib_to_pluto_stmts(const scoplib_scop_p scop)
{
    int i, j;
    Stmt **stmts;
    int npar, nvar, nstmts;
    scoplib_statement_p scop_stmt;

    npar = scop->nb_parameters;
    nstmts = scoplib_statement_number(scop->statement);

    /* Max dom dimensionality */
    nvar = -1;
    if (nstmts >= 1)    {
        scop_stmt = scop->statement;
        for (i=0; i<nstmts; i++) {
            nvar = PLMAX(nvar, scop_stmt->nb_iterators);
            scop_stmt = scop_stmt->next;
        }
    }

    /* Allocate more to account for unroll/jamming later on */
    stmts = (Stmt **) malloc(nstmts*sizeof(Stmt *));

    scop_stmt = scop->statement;

    for(i=0; i<nstmts; i++)  {
        PlutoConstraints *domain = 
            scoplib_matrix_to_pluto_constraints(scop_stmt->domain->elt);

        stmts[i] = pluto_stmt_alloc(scop_stmt->nb_iterators, domain);
        Stmt *stmt = stmts[i];

        pluto_constraints_free(domain);

        stmt->id = i;

        assert(scop_stmt->domain->elt->NbColumns-1 == stmt->dim + npar + 1);

        for (j=0; j<stmt->dim; j++)  {
            stmt->is_orig_loop[j] = true;
        }

        stmt->num_ind_sols = 0;

        /* Tile it if it's tilable unless turned off by .fst/.precut file */
        stmt->tile = 1;

        for (j=0; j<stmt->dim; j++)    {
            stmt->iterators[j] = strdup(scop_stmt->iterators[j]);
        }
        /* Statement text */
        stmt->text = (char *) malloc(sizeof(char)*(strlen(scop_stmt->body)+1));
        strcpy(stmt->text, scop_stmt->body);

        /* Read/write accesses */
        scoplib_access_list_p wlist = scoplib_access_get_write_access_list(scop, scop_stmt);
        scoplib_access_list_p rlist = scoplib_access_get_read_access_list(scop, scop_stmt);
        scoplib_access_list_p rlist_t, wlist_t;
        rlist_t = rlist;
        wlist_t = wlist;

        int count=0;
        scoplib_access_list_p tmp = wlist;
        while (tmp != NULL)   {
            count++;
            tmp = tmp->next;
        }
        stmt->nwrites = count;
        stmt->writes = (PlutoAccess **) malloc(stmt->nwrites*sizeof(PlutoAccess *));

        tmp = rlist;
        count = 0;
        while (tmp != NULL)   {
            count++;
            tmp = tmp->next;
        }
        stmt->nreads = count;
        stmt->reads = (PlutoAccess **) malloc(stmt->nreads*sizeof(PlutoAccess *));

        count = 0;
        while (wlist != NULL)   {
            PlutoMatrix *wmat = scoplib_matrix_to_pluto_matrix(wlist->elt->matrix);
            stmt->writes[count] = (PlutoAccess *) malloc(sizeof(PlutoAccess));
            stmt->writes[count]->mat = wmat;
            if (wlist->elt->symbol != NULL) {
                stmt->writes[count]->name = strdup(wlist->elt->symbol->identifier);
                stmt->writes[count]->symbol = scoplib_symbol_copy(wlist->elt->symbol);
            }else{
                stmt->writes[count]->symbol = NULL;
            }
            //scoplib_symbol_print(stdout, stmt->writes[count]->symbol);
            count++;
            wlist = wlist->next;
        }

        count = 0;
        while (rlist != NULL)   {
            PlutoMatrix *rmat = scoplib_matrix_to_pluto_matrix(rlist->elt->matrix);
            stmt->reads[count] = (PlutoAccess *) malloc(sizeof(PlutoAccess));
            stmt->reads[count]->mat = rmat;
            if (rlist->elt->symbol != NULL) {
                stmt->reads[count]->name = strdup(rlist->elt->symbol->identifier);
                stmt->reads[count]->symbol = scoplib_symbol_copy(rlist->elt->symbol);
            }
            else{
                stmt->reads[count]->symbol = NULL;
            }
            //scoplib_symbol_print(stdout, stmt->reads[count]->symbol);
            count++;
            rlist = rlist->next;
        }

        scoplib_access_list_free(wlist_t);
        scoplib_access_list_free(rlist_t);

        scop_stmt = scop_stmt->next;
    }

    return stmts;
}

void pluto_stmt_print(FILE *fp, const Stmt *stmt)
{
    int i;

    fprintf(fp, "S%d \"%s\"; dims: %d\n", stmt->id+1, stmt->text, stmt->dim);
    fprintf(fp, "Domain\n");
    pluto_constraints_print(fp, stmt->domain);
    fprintf(fp, "Transformation\n");
    pluto_matrix_print(fp, stmt->trans);

    if (stmt->nreads==0) {
        fprintf(fp, "No Read accesses\n");
    }else{
        fprintf(fp, "Read accesses\n");
        for (i=0; i<stmt->nreads; i++)  {
            pluto_matrix_print(fp, stmt->reads[i]->mat);
        }
    }

    for (i=0; i<stmt->dim; i++) {
        printf("Original loop: %d -> %d\n", i, stmt->is_orig_loop[i]);
    }

    fprintf(fp, "\n");

}


void pluto_stmts_print(FILE *fp, Stmt **stmts, int nstmts)
{
    int i;

    for(i=0; i<nstmts; i++)  {
        pluto_stmt_print(fp, stmts[i]);
    }
}



void pluto_dep_free(Dep *dep)
{
    pluto_constraints_free(dep->dpolytope);
    free(dep->direction);
}


/* Convert an isl_basic_map to a PlutoConstraints object */
static PlutoConstraints *isl_basic_map_to_pluto_constraints(
        __isl_keep isl_basic_map *bmap)
{
    int i, j;
    int eq_row;
    int ineq_row;
    int n_col;
    isl_int v;
    isl_mat *eq, *ineq;
    PlutoConstraints *cons;

    isl_int_init(v);

    eq = isl_basic_map_equalities_matrix(bmap,
            isl_dim_in, isl_dim_out, isl_dim_div, isl_dim_param, isl_dim_cst);
    ineq = isl_basic_map_inequalities_matrix(bmap,
            isl_dim_in, isl_dim_out, isl_dim_div, isl_dim_param, isl_dim_cst);

    eq_row = isl_mat_rows(eq);
    ineq_row = isl_mat_rows(ineq);
    n_col = isl_mat_cols(eq);

    cons = pluto_constraints_alloc(eq_row + ineq_row, n_col);
    cons->nrows = eq_row + ineq_row;

    for (i = 0; i < eq_row; ++i) {
        cons->is_eq[i] = 1;
        for (j = 0; j < n_col; ++j) {
            isl_mat_get_element(eq, i, j, &v);
            cons->val[i][j] = isl_int_get_si(v);
        }
    }

    for (i = 0; i < ineq_row; ++i) {
        cons->is_eq[eq_row+i] = 0;
        for (j = 0; j < n_col; ++j) {
            isl_mat_get_element(ineq, i, j, &v);
            cons->val[eq_row + i][j] = isl_int_get_si(v);
        }
    }

    isl_mat_free(eq);
    isl_mat_free(ineq);

    isl_int_clear(v);

    return cons;
}


/* Set the dimension names of type "type" according to the elements
 * in the array "names".
 */
static __isl_give isl_dim *set_names(__isl_take isl_dim *dim,
        enum isl_dim_type type, char **names)
{
    int i;

    for (i = 0; i < isl_dim_size(dim, type); ++i)
        dim = isl_dim_set_name(dim, type, i, names[i]);

    return dim;
}


/* Convert a scoplib_matrix_p containing the constraints of a domain
 * to an isl_set.
 */
static __isl_give isl_set *scoplib_matrix_to_isl_set(scoplib_matrix_p matrix,
        __isl_take isl_dim *dim)
{
    int i, j;
    int n_eq = 0, n_ineq = 0;
    isl_ctx *ctx;
    isl_mat *eq, *ineq;
    isl_int v;
    isl_basic_set *bset;

    isl_int_init(v);

    ctx = isl_dim_get_ctx(dim);

    for (i = 0; i < matrix->NbRows; ++i)
        if (SCOPVAL_zero_p(matrix->p[i][0]))
            n_eq++;
        else
            n_ineq++;

    eq = isl_mat_alloc(ctx, n_eq, matrix->NbColumns - 1);
    ineq = isl_mat_alloc(ctx, n_ineq, matrix->NbColumns - 1);

    n_eq = n_ineq = 0;
    for (i = 0; i < matrix->NbRows; ++i) {
        isl_mat **m;
        int row;

        if (SCOPVAL_zero_p(matrix->p[i][0])) {
            m = &eq;
            row = n_eq++;
        } else {
            m = &ineq;
            row = n_ineq++;
        }

        for (j = 0; j < matrix->NbColumns - 1; ++j) {
            int t = SCOPVAL_get_si(matrix->p[i][1 + j]);
            isl_int_set_si(v, t);
            *m = isl_mat_set_element(*m, row, j, v);
        }
    }

    isl_int_clear(v);

    bset = isl_basic_set_from_constraint_matrices(dim, eq, ineq,
            isl_dim_set, isl_dim_div, isl_dim_param, isl_dim_cst);
    return isl_set_from_basic_set(bset);
}


/* Convert a scoplib_matrix_list_p describing a union of domains
 * to an isl_set.
 */
static __isl_give isl_set *scoplib_matrix_list_to_isl_set(
        scoplib_matrix_list_p list, __isl_take isl_dim *dim)
{
    isl_set *set;

    set = isl_set_empty(isl_dim_copy(dim));
    for (; list; list = list->next) {
        isl_set *set_i;
        set_i = scoplib_matrix_to_isl_set(list->elt, isl_dim_copy(dim));
        set = isl_set_union(set, set_i);
    }

    isl_dim_free(dim);
    return set;
}


/* Convert an m x (1 + n + 1) scoplib_matrix_p [d A c]
 * to an m x (m + n + 1) isl_mat [-I A c].
 */
static __isl_give isl_mat *extract_equalities(isl_ctx *ctx,
        scoplib_matrix_p matrix, int first, int n)
{
    int i, j;
    int n_col;
    isl_int v;
    isl_mat *eq;

    n_col = matrix->NbColumns;

    isl_int_init(v);
    eq = isl_mat_alloc(ctx, n, n + n_col - 1);

    for (i = 0; i < n; ++i) {
        isl_int_set_si(v, 0);
        for (j = 0; j < n; ++j)
            eq = isl_mat_set_element(eq, i, j, v);
        isl_int_set_si(v, -1);
        eq = isl_mat_set_element(eq, i, i, v);
        for (j = 0; j < n_col - 1; ++j) {
            int t = SCOPVAL_get_si(matrix->p[first + i][1 + j]);
            isl_int_set_si(v, t);
            eq = isl_mat_set_element(eq, i, n + j, v);
        }
    }

    isl_int_clear(v);

    return eq;
}


/* Convert a scoplib_matrix_p schedule [0 A c] to
 * the isl_map { i -> A i + c } in the space prescribed by "dim".
 */
static __isl_give isl_map *scoplib_schedule_to_isl_map(
        scoplib_matrix_p schedule, __isl_take isl_dim *dim)
{
    int n_row, n_col;
    isl_ctx *ctx;
    isl_mat *eq, *ineq;
    isl_basic_map *bmap;

    ctx = isl_dim_get_ctx(dim);
    n_row = schedule->NbRows;
    n_col = schedule->NbColumns;

    ineq = isl_mat_alloc(ctx, 0, n_row + n_col - 1);
    eq = extract_equalities(ctx, schedule, 0, n_row);

    bmap = isl_basic_map_from_constraint_matrices(dim, eq, ineq,
            isl_dim_out, isl_dim_in, isl_dim_div, isl_dim_param, isl_dim_cst);
    return isl_map_from_basic_map(bmap);
}


/* Return the number of lines until the next non-zero element
 * in the first column of "access" or until the end of the matrix.
 */
static int access_len(scoplib_matrix_p access, int first)
{
    int i;

    for (i = first + 1; i < access->NbRows; ++i)
        if (!SCOPVAL_zero_p(access->p[i][0]))
            break;

    return i - first;
}


/* Convert a scoplib_matrix_p describing a series of accesses
 * to an isl_union_map with domain "dom" (in space "D").
 * Each access in "access" has a non-zero integer in the first column
 * of the first row identifying the array being accessed.  The remaining
 * entries of the first column are zero.
 * Let "A" be array identified by the first entry.
 * The remaining columns have the form [B c].
 * Each such access is converted to a map { D[i] -> A[B i + c] } * dom.
 *
 * Note that each access in the input is described by at least one row,
 * which means that there is no way of distinguishing between an access
 * to a scalar and an access to the first element of a 1-dimensional array.
 */
static __isl_give isl_union_map *scoplib_access_to_isl_union_map(
        scoplib_matrix_p access, __isl_take isl_set *dom, char **arrays)
{
    int i, len, n_col;
    isl_ctx *ctx;
    isl_dim *dim;
    isl_mat *eq, *ineq;
    isl_union_map *res;

    ctx = isl_set_get_ctx(dom);

    dim = isl_set_get_dim(dom);
    dim = isl_dim_drop(dim, isl_dim_set, 0, isl_dim_size(dim, isl_dim_set));
    res = isl_union_map_empty(dim);

    n_col = access->NbColumns;

    for (i = 0; i < access->NbRows; i += len) {
        isl_basic_map *bmap;
        isl_map *map;
        int arr = SCOPVAL_get_si(access->p[i][0]) - 1;

        len = access_len(access, i);

        dim = isl_set_get_dim(dom);
        dim = isl_dim_from_domain(dim);
        dim = isl_dim_add(dim, isl_dim_out, len);
        dim = isl_dim_set_tuple_name(dim, isl_dim_out, arrays[arr]);

        ineq = isl_mat_alloc(ctx, 0, len + n_col - 1);
        eq = extract_equalities(ctx, access, i, len);

        bmap = isl_basic_map_from_constraint_matrices(dim, eq, ineq,
            isl_dim_out, isl_dim_in, isl_dim_div, isl_dim_param, isl_dim_cst);
        map = isl_map_from_basic_map(bmap);
        map = isl_map_intersect_domain(map, isl_set_copy(dom));
        res = isl_union_map_union(res, isl_union_map_from_map(map));
    }

    isl_set_free(dom);

    return res;
}


static int basic_map_count(__isl_take isl_basic_map *bmap, void *user)
{
    int *count = user;

    *count += 1;
    isl_basic_map_free(bmap);
    return 0;
}


static int map_count(__isl_take isl_map *map, void *user)
{
    int r;

    r = isl_map_foreach_basic_map(map, &basic_map_count, user);
    isl_map_free(map);
    return r;
}


/* Temporary data structure used inside extract_deps.
 *
 * deps points to the array of Deps being constructed
 * type is the type of the next Dep
 * index is the index of the next Dep in the array.
 */
struct pluto_extra_dep_info {
    Dep *deps;
    int type;
    int index;
};


/* Convert an isl_basic_map describing part of a dependence to a Dep.
 * The names of the input and output spaces are of the form S_d
 * with d an integer identifying the statement.
 */
static int basic_map_extract(__isl_take isl_basic_map *bmap, void *user)
{
    Dep *dep;
    struct pluto_extra_dep_info *info;
    info = (struct pluto_extra_dep_info *)user;

    bmap = isl_basic_map_remove_divs(bmap);

    dep = &info->deps[info->index];

    dep->id = info->index;
    dep->dpolytope = isl_basic_map_to_pluto_constraints(bmap);
    dep->type = info->type;
    dep->src = atoi(isl_basic_map_get_tuple_name(bmap, isl_dim_in) + 2);
    dep->dest = atoi(isl_basic_map_get_tuple_name(bmap, isl_dim_out) + 2);

    /* No support yet to get these */
    dep->src_acc = NULL;
    dep->dest_acc = NULL;

    /* Initialize other fields used for auto transform */
    dep->satisfied = false;
    dep->satisfaction_level = -1;

    info->index++;
    isl_basic_map_free(bmap);
    return 0;
}


static int map_extract(__isl_take isl_map *map, void *user)
{
    int r;

    r = isl_map_foreach_basic_map(map, &basic_map_extract, user);
    isl_map_free(map);
    return r;
}


static int extract_deps(Dep *deps, int first, __isl_keep isl_union_map *umap,
    int type)
{
    struct pluto_extra_dep_info info = { deps, type, first };

    isl_union_map_foreach_map(umap, &map_extract, &info);

    return info.index - first;
}


/* Compute dependences based on the iteration domain and access
 * information in "scop" and put the result in "prog".
 *
 * If options->lastwriter is false, then
 *      RAW deps are those from any earlier write to a read
 *      WAW deps are those from any earlier write to a write
 *      WAR deps are those from any earlier read to a write
 *      RAR deps are those from any earlier read to a read
 * If options->lastwriter is true, then
 *      RAW deps are those from the last write to a read
 *      WAW deps are those from the last write to a write
 *      WAR deps are those from any earlier read not masked by an intermediate
 *      write to a write
 *      RAR deps are those from the last read to a read
 *
 * The RAR deps are only computed if options->rar is set.
 */
static void compute_deps(scoplib_scop_p scop, PlutoProg *prog,
        PlutoOptions *options)
{
    int i;
    int nstmts = scoplib_statement_number(scop->statement);
    isl_ctx *ctx;
    isl_dim *dim;
    isl_set *context;
    isl_union_map *empty;
    isl_union_map *write;
    isl_union_map *read;
    isl_union_map *schedule;
    isl_union_map *dep_raw, *dep_war, *dep_waw, *dep_rar;
    scoplib_statement_p stmt;

    ctx = isl_ctx_alloc();
    assert(ctx);

    dim = isl_dim_set_alloc(ctx, scop->nb_parameters, 0);
    dim = set_names(dim, isl_dim_param, scop->parameters);
    context = scoplib_matrix_to_isl_set(scop->context, isl_dim_copy(dim));

    if (!options->rar)
        dep_rar = isl_union_map_empty(isl_dim_copy(dim));
    empty = isl_union_map_empty(isl_dim_copy(dim));
    write = isl_union_map_empty(isl_dim_copy(dim));
    read = isl_union_map_empty(isl_dim_copy(dim));
    schedule = isl_union_map_empty(dim);

    for (i = 0, stmt = scop->statement; i < nstmts; ++i, stmt = stmt->next) {
        isl_set *dom;
        isl_map *schedule_i;
        isl_union_map *read_i;
        isl_union_map *write_i;
        char name[20];

        snprintf(name, sizeof(name), "S_%d", i);

        dim = isl_dim_set_alloc(ctx, scop->nb_parameters, stmt->nb_iterators);
        dim = set_names(dim, isl_dim_param, scop->parameters);
        dim = set_names(dim, isl_dim_set, stmt->iterators);
        dim = isl_dim_set_tuple_name(dim, isl_dim_set, name);
        dom = scoplib_matrix_list_to_isl_set(stmt->domain, dim);
        dom = isl_set_intersect(dom, isl_set_copy(context));

        dim = isl_dim_alloc(ctx, scop->nb_parameters, stmt->nb_iterators,
                            2 * stmt->nb_iterators + 1);
        dim = set_names(dim, isl_dim_param, scop->parameters);
        dim = set_names(dim, isl_dim_in, stmt->iterators);
        dim = isl_dim_set_tuple_name(dim, isl_dim_in, name);
        schedule_i = scoplib_schedule_to_isl_map(stmt->schedule, dim);

        read_i = scoplib_access_to_isl_union_map(stmt->read, isl_set_copy(dom),
                                                 scop->arrays);
        write_i = scoplib_access_to_isl_union_map(stmt->write, dom,
                                                 scop->arrays);

        read = isl_union_map_union(read, read_i);
        write = isl_union_map_union(write, write_i);
        schedule = isl_union_map_union(schedule,
                                        isl_union_map_from_map(schedule_i));

    }

    if (options->lastwriter) {
        isl_union_map_compute_flow(isl_union_map_copy(read),
                            isl_union_map_copy(write),
                            isl_union_map_copy(empty),
                            isl_union_map_copy(schedule),
                            &dep_raw, NULL, NULL, NULL);
        isl_union_map_compute_flow(isl_union_map_copy(write),
                            isl_union_map_copy(write),
                            isl_union_map_copy(read),
                            isl_union_map_copy(schedule),
                            &dep_waw, &dep_war, NULL, NULL);
        if (options->rar)
            isl_union_map_compute_flow(isl_union_map_copy(read),
                                isl_union_map_copy(read),
                                isl_union_map_copy(empty),
                                isl_union_map_copy(schedule),
                                &dep_rar, NULL, NULL, NULL);
    } else {
        isl_union_map_compute_flow(isl_union_map_copy(read),
                            isl_union_map_copy(empty),
                            isl_union_map_copy(write),
                            isl_union_map_copy(schedule),
                            NULL, &dep_raw, NULL, NULL);
        isl_union_map_compute_flow(isl_union_map_copy(write),
                            isl_union_map_copy(empty),
                            isl_union_map_copy(read),
                            isl_union_map_copy(schedule),
                            NULL, &dep_war, NULL, NULL);
        isl_union_map_compute_flow(isl_union_map_copy(write),
                            isl_union_map_copy(empty),
                            isl_union_map_copy(write),
                            isl_union_map_copy(schedule),
                            NULL, &dep_waw, NULL, NULL);
        if (options->rar)
            isl_union_map_compute_flow(isl_union_map_copy(read),
                                isl_union_map_copy(empty),
                                isl_union_map_copy(read),
                                isl_union_map_copy(schedule),
                                NULL, &dep_rar, NULL, NULL);
    }

    dep_raw = isl_union_map_coalesce(dep_raw);
    dep_war = isl_union_map_coalesce(dep_war);
    dep_waw = isl_union_map_coalesce(dep_waw);
    dep_rar = isl_union_map_coalesce(dep_rar);

    prog->ndeps = 0;
    isl_union_map_foreach_map(dep_raw, &map_count, &prog->ndeps);
    isl_union_map_foreach_map(dep_war, &map_count, &prog->ndeps);
    isl_union_map_foreach_map(dep_waw, &map_count, &prog->ndeps);
    isl_union_map_foreach_map(dep_rar, &map_count, &prog->ndeps);

    prog->deps = (Dep *)malloc(prog->ndeps * sizeof(Dep));
    prog->ndeps = 0;
    prog->ndeps += extract_deps(prog->deps, prog->ndeps, dep_raw, CANDL_RAW);
    prog->ndeps += extract_deps(prog->deps, prog->ndeps, dep_war, CANDL_WAR);
    prog->ndeps += extract_deps(prog->deps, prog->ndeps, dep_waw, CANDL_WAW);
    prog->ndeps += extract_deps(prog->deps, prog->ndeps, dep_rar, CANDL_RAR);

    isl_union_map_free(dep_raw);
    isl_union_map_free(dep_war);
    isl_union_map_free(dep_waw);
    isl_union_map_free(dep_rar);

    isl_union_map_free(empty);
    isl_union_map_free(write);
    isl_union_map_free(read);
    isl_union_map_free(schedule);
    isl_set_free(context);

    isl_ctx_free(ctx);
}


/* 
 * Extract necessary information from clan_scop to create PlutoProg - a
 * representation of the program sufficient to be used throughout Pluto. 
 * PlutoProg also includes dependences; so candl is run here.
 */
PlutoProg *scop_to_pluto_prog(scoplib_scop_p scop, PlutoOptions *options)
{
    int i;

    PlutoProg *prog = pluto_prog_alloc();

    prog->nstmts = scoplib_statement_number(scop->statement);
    prog->options = options;

    /* Program parameters */
    prog->npar = scop->nb_parameters;

    if (prog->npar >= 1)    {
        prog->params = (char **) malloc(sizeof(char *)*prog->npar);
    }
    for (i=0; i<prog->npar; i++)  {
        prog->params[i] = strdup(scop->parameters[i]);
    }

    prog->context = scoplib_matrix_to_pluto_constraints(scop->context);

    if (options->context != -1)	{
        for (i=0; i<prog->npar; i++)  {
            pluto_constraints_add_inequality(prog->context, prog->context->nrows);
            prog->context->val[i][i] = 1;
            prog->context->val[i][prog->context->ncols-1] = -options->context;
        }
    }

    scoplib_statement_p scop_stmt = scop->statement;

    prog->nvar = scop_stmt->nb_iterators;
    for (i=0; i<prog->nstmts; i++) {
        prog->nvar = PLMAX(prog->nvar, scop_stmt->nb_iterators);
        scop_stmt = scop_stmt->next;
    }

    prog->stmts = scoplib_to_pluto_stmts(scop);

    /* Compute dependences */
    if (options->isldep) {
        compute_deps(scop, prog, options);
    }else{
        /*  Using Candl */
        candl_program_p candl_program = candl_program_convert_scop(scop, NULL);

        CandlOptions *candlOptions = candl_options_malloc();
        if (options->rar)   {
            candlOptions->rar = 1;
        }
        candlOptions->lastwriter = options->lastwriter;
        candlOptions->scalar_privatization = options->scalpriv;
        // candlOptions->verbose = 1;

        CandlDependence *candl_deps = candl_dependence(candl_program,
                candlOptions);
        prog->deps = deps_read(candl_deps, prog);
        prog->ndeps = candl_num_dependences(candl_deps);
        candl_options_free(candlOptions);
        candl_dependence_free(candl_deps);
        candl_program_free(candl_program);
    }

    /* Allocate and initialize hProps (size doesn't matter; will resize when
     * necessary */
    prog->hProps = NULL;
    prog->num_hyperplanes = 0;

    // hack for linearized accesses
    FILE *lfp = fopen(".linearized", "r");
    FILE *nlfp = fopen(".nonlinearized", "r");
    char tmpstr[256];
    char linearized[256];
    if (lfp && nlfp) {
        for (i=0; i<prog->nstmts; i++)    {
            rewind(lfp);
            rewind(nlfp);
            while (!feof(lfp) && !feof(nlfp))      {
                fgets(tmpstr, 256, nlfp);
                fgets(linearized, 256, lfp);
                if (strstr(tmpstr, prog->stmts[i]->text))        {
                    prog->stmts[i]->text = (char *) realloc(prog->stmts[i]->text, sizeof(char)*(strlen(linearized)+1));
                    strcpy(prog->stmts[i]->text, linearized);
                }
            }
        }
        fclose(lfp);
        fclose(nlfp);
    }

    return prog;
}

/* Get an upper bound for transformation coefficients to prevent spurious
 * transformations that represent shifts or skews proportional to trip counts:
 * this happens when loop bounds are constants
 */
int get_coeff_upper_bound(PlutoProg *prog)
{
    int max, i, r;

    max = 0;
    for (i=0; i<prog->nstmts; i++)  {
        Stmt *stmt = prog->stmts[i];
        for (r=0; r<stmt->domain->nrows; r++) {
            max  = PLMAX(max,stmt->domain->val[r][stmt->domain->ncols-1]);
        }
    }

    return max-1;
}


PlutoProg *pluto_prog_alloc()
{
    PlutoProg *prog = (PlutoProg *) malloc(sizeof(PlutoProg));

    prog->nstmts = 0;
    prog->stmts = NULL;
    prog->npar = 0;
    prog->nvar = 0;
    prog->params = NULL;
    prog->context = pluto_constraints_alloc(0, prog->npar+1);
    prog->deps = NULL;
    prog->ndeps = 0;
    prog->ddg = NULL;
    prog->hProps = NULL;
    prog->num_hyperplanes = 0;

    return prog;
}



void pluto_prog_free(PlutoProg *prog)
{
    int i;

    /* Free dependences */
    for (i=0; i<prog->ndeps; i++) {
        pluto_dep_free(&prog->deps[i]);
    }
    if (prog->deps != NULL) {
        free(prog->deps);
    }

    /* Free DDG */
    if (prog->ddg != NULL)  {
        graph_free(prog->ddg);
    }

    if (prog->hProps != NULL)   {
        free(prog->hProps);
    }

    for (i=0; i<prog->npar; i++)  {
        free(prog->params[i]);
    }
    if (prog->npar >= 1)    {
        free(prog->params);
    }

    /* Statements */
    for (i=0; i<prog->nstmts; i++) {
        pluto_stmt_free(prog->stmts[i]);
    }
    if (prog->nstmts >= 1)  {
        free(prog->stmts);
    }

    pluto_constraints_free(prog->context);

    free(prog);
}


PlutoOptions *pluto_options_alloc()
{
    PlutoOptions *options;

    options  = (PlutoOptions *) malloc(sizeof(PlutoOptions));

    /* Initialize to default */
    options->tile = 0;
    options->debug = 0;
    options->moredebug = 0;
    options->scancount = 0;
    options->parallel = 0;
    options->unroll = 0;

    /* Unroll/jam factor */
    options->ufactor = 8;

    /* Ignore input deps */
    options->rar = 0;

    /* Override for first and last levels to tile */
    options->ft = -1;
    options->lt = -1;

    /* Override for first and last cloog options */
    options->cloogf = -1;
    options->cloogl = -1;

    options->multipipe = 0;
    options->l2tile = 0;
    options->prevector = 1;
    options->fuse = SMART_FUSE;

    /* Experimental */
    options->polyunroll = 0;

    /* Default context is no context */
    options->context = -1;

    options->bee = 0;

    options->isldep = 0;

    options->islsolve = 0;

    options->readscoplib = 0;

    options->lastwriter = 0;

    options->nobound = 0;

    options->scalpriv = 0;

    options->silent = 0;

    options->out_file = NULL;

    return options;
}


/* Add global/program parameter at position 'pos' */
void pluto_prog_add_param(PlutoProg *prog, const char *param, int pos)
{
    int i, j;

    for (i=0; i<prog->nstmts; i++) {
        Stmt *stmt = prog->stmts[i];
        pluto_constraints_add_dim(stmt->domain, stmt->domain->ncols-1-prog->npar+pos);
        pluto_matrix_add_col(stmt->trans, stmt->trans->ncols-1-prog->npar+pos);

        for (j=0; j<stmt->nwrites; j++)  {
            pluto_matrix_add_col(stmt->writes[j]->mat, stmt->dim+pos);
        }
        for (j=0; j<stmt->nreads; j++)  {
            pluto_matrix_add_col(stmt->reads[j]->mat, stmt->dim+pos);
        }
    }
    for (i=0; i<prog->ndeps; i++)   {
        pluto_constraints_add_dim(prog->deps[i].dpolytope, 
                prog->deps[i].dpolytope->ncols-1-prog->npar+pos);
    }
    pluto_constraints_add_dim(prog->context, prog->context->ncols-1-prog->npar+pos);

    prog->params = (char **) realloc(prog->params, sizeof(char *)*(prog->npar+1));

    for (i=prog->npar-1; i>=pos; i--)    {
        prog->params[i+1] = prog->params[i];
    }

    prog->params[pos] = strdup(param);
    prog->npar++;
}


void pluto_options_free(PlutoOptions *options)
{
    if (options->out_file != NULL)  {
        free(options->out_file);
    }
    free(options);
}


/* time_pos: position of time iterator; iter: domain iterator; supply -1
 * if you don't want a scattering function row added for it */
void pluto_stmt_add_dim(Stmt *stmt, int pos, int time_pos, const char *iter,
        PlutoProg *prog)
{
    int i, npar;

    npar = stmt->domain->ncols - stmt->dim - 1;

    assert(pos <= stmt->dim);
    assert(time_pos <= stmt->trans->nrows);
    assert(stmt->dim + npar + 1 == stmt->domain->ncols);

    pluto_constraints_add_dim(stmt->domain, pos);
    stmt->dim++;
    stmt->iterators = (char **) realloc(stmt->iterators, stmt->dim*sizeof(char *));
    for (i=stmt->dim-2; i>=pos; i--) {
        stmt->iterators[i+1] = stmt->iterators[i];
    }
    stmt->iterators[pos] = strdup(iter);

    pluto_matrix_add_col(stmt->trans, pos);

    if (time_pos != -1) {
        pluto_matrix_add_row(stmt->trans, time_pos);
        stmt->trans->val[time_pos][pos] = 1;
    }

    /* Update is_orig_loop */
    stmt->is_orig_loop = realloc(stmt->is_orig_loop, 
            sizeof(bool)*stmt->dim);
    for (i=stmt->dim-2; i>=pos; i--) {
        stmt->is_orig_loop[i+1] = stmt->is_orig_loop[i];
    }
    stmt->is_orig_loop[pos] = true;

    for (i=0; i<stmt->nwrites; i++)   {
        pluto_matrix_add_col(stmt->writes[i]->mat, pos);
    }

    for (i=0; i<stmt->nreads; i++)   {
        pluto_matrix_add_col(stmt->reads[i]->mat, pos);
    }

    for (i=0; i<prog->ndeps; i++) {
        if (prog->deps[i].src == stmt->id) {
            pluto_constraints_add_dim(prog->deps[i].dpolytope, pos);
        }
        if (prog->deps[i].dest == stmt->id) {
            pluto_constraints_add_dim(prog->deps[i].dpolytope, 
                    prog->stmts[prog->deps[i].src]->dim+pos);
        }
    }
}

/* Warning: use it only to knock off a dummy dimension (unrelated to 
 * anything else */
void pluto_stmt_remove_dim(Stmt *stmt, int pos, PlutoProg *prog)
{
    int i, npar;

    npar = stmt->domain->ncols - stmt->dim - 1;

    assert(pos <= stmt->dim);
    assert(stmt->dim + npar + 1 == stmt->domain->ncols);

    pluto_constraints_remove_dim(stmt->domain, pos);
    stmt->dim--;

    free(stmt->iterators[pos]);
    for (i=pos; i<=stmt->dim-1; i++) {
        stmt->iterators[i] = stmt->iterators[i+1];
    }
    stmt->iterators = (char **) realloc(stmt->iterators, stmt->dim*sizeof(char *));

    pluto_matrix_remove_col(stmt->trans, pos);

    /* Update is_orig_loop */
    for (i=pos; i<=stmt->dim-1; i++) {
        stmt->is_orig_loop[i] = stmt->is_orig_loop[i+1];
    }
    stmt->is_orig_loop = realloc(stmt->is_orig_loop, 
            sizeof(bool)*stmt->dim);

    for (i=0; i<stmt->nwrites; i++)   {
        pluto_matrix_remove_col(stmt->writes[i]->mat, pos);
    }

    for (i=0; i<stmt->nreads; i++)   {
        pluto_matrix_remove_col(stmt->reads[i]->mat, pos);
    }

    for (i=0; i<prog->ndeps; i++) {
        if (prog->deps[i].src == stmt->id) {
            pluto_constraints_remove_dim(prog->deps[i].dpolytope, pos);
        }
        if (prog->deps[i].dest == stmt->id) {
            // if (i==0)  printf("removing dim\n");
            pluto_constraints_remove_dim(prog->deps[i].dpolytope, 
                    prog->stmts[prog->deps[i].src]->dim+pos);
        }
    }
}


void pluto_prog_add_hyperplane(PlutoProg *prog, int pos)
{
    int i;

    prog->num_hyperplanes++;
    prog->hProps = (HyperplaneProperties *) realloc(prog->hProps, 
            prog->num_hyperplanes*sizeof(HyperplaneProperties));

    for (i=prog->num_hyperplanes-2; i>=pos; i--) {
        prog->hProps[i+1] = prog->hProps[i];
    }
    /* Initialize some */
    prog->hProps[pos].unroll = NO_UNROLL;
    prog->hProps[pos].band_num = -1;
    prog->hProps[pos].dep_prop = UNKNOWN;
    prog->hProps[pos].type = H_UNKNOWN;
}


/* Add a statement that is scheduled at the end of all statements 
 * scheduled at level 'level' (0-indexed)
 * domain: domain of the stmt
 * text: statement text
 */
void pluto_add_stmt_to_end(PlutoProg *prog, 
        const PlutoConstraints *domain,
        char **iterators,
        const char *text,
        int level
        )
{
    int i;

    Stmt **stmts = prog->stmts;
    int nstmts = prog->nstmts;

    for (i=0; i<nstmts; i++)    {
        pluto_matrix_add_row(stmts[i]->trans, level);
    }
    assert(prog->num_hyperplanes >= level+1);
    pluto_prog_add_hyperplane(prog, level);
    prog->hProps[level].type = H_SCALAR;
    prog->hProps[level].dep_prop = SEQ;

    PlutoMatrix *trans = pluto_matrix_alloc(prog->num_hyperplanes, domain->ncols);
    pluto_matrix_initialize(trans, 0);
    trans->nrows = 0;

    pluto_add_stmt(prog, domain, trans, iterators, text);
}


/* Add statement to program; can't reuse arg stmt pointer any more */
void pluto_add_given_stmt(PlutoProg *prog, Stmt *stmt)
{
    int i, j, max_nrows;

    prog->stmts = (Stmt **) realloc(prog->stmts, ((prog->nstmts+1)*sizeof(Stmt *)));

    stmt->id = prog->nstmts;

    prog->nvar = PLMAX(prog->nvar, stmt->dim);
    prog->stmts[prog->nstmts] = stmt;
    prog->nstmts++;

    /* Pad all trans matrices if necessary with zeros */
    Stmt **stmts = prog->stmts;
    int nstmts = prog->nstmts;

    max_nrows = 0;

    for (i=0; i<nstmts; i++)    {
        if (stmts[i]->trans != NULL)    {
            max_nrows = PLMAX(max_nrows, prog->stmts[i]->trans->nrows);
        }
    }

    if (max_nrows >= 1) {
        for (i=0; i<nstmts; i++)    {
            if (stmts[i]->trans == NULL)    {
                stmts[i]->trans = pluto_matrix_alloc(max_nrows, 
                        stmts[i]->dim+prog->npar+1);
                stmts[i]->trans->nrows = 0;
            }

            int curr_rows = stmts[i]->trans->nrows;

            /* Add all zero rows */
            for (j=curr_rows; j<max_nrows; j++)    {
                pluto_matrix_add_row(stmts[i]->trans, stmts[i]->trans->nrows);
            }
        }

        int old_hyp_num = prog->num_hyperplanes;
        for (i=old_hyp_num; i<max_nrows; i++) {
            pluto_prog_add_hyperplane(prog, prog->num_hyperplanes);
            prog->hProps[prog->num_hyperplanes-1].type = H_SCALAR;
        }
    }

}


/* Create a statement and add it to the program
 * iterators: domain iterators
 * trans: schedule/transformation
 * domain: domain
 * text: statement text
 */
void pluto_add_stmt(PlutoProg *prog, 
        const PlutoConstraints *domain,
        const PlutoMatrix *trans,
        char ** iterators,
        const char *text)
{
    int i, j, nstmts, max_nrows;

    assert(trans != NULL);
    assert(trans->ncols == domain->ncols);

    nstmts = prog->nstmts;

    prog->stmts = (Stmt **) realloc(prog->stmts, ((nstmts+1)*sizeof(Stmt *)));

    Stmt **stmts = prog->stmts;

    Stmt *stmt = pluto_stmt_alloc(domain->ncols-prog->npar-1, domain);

    stmt->id = nstmts;

    if (trans != NULL)  {
        pluto_matrix_free(stmt->trans);
        stmt->trans = pluto_matrix_dup(trans);
    }

    stmt->text = strdup(text);
    prog->nvar = PLMAX(prog->nvar, stmt->dim);

    for (i=0; i<stmt->dim; i++) {
        stmt->iterators[i] = strdup(iterators[i]);
    }

    prog->stmts[nstmts] = stmt;
    prog->nstmts++;
    nstmts = prog->nstmts;

    /* Pad all trans if necessary with zeros */
    max_nrows = 0;
    for (i=0; i<nstmts; i++)    {
        if (stmts[i]->trans != NULL)    {
            max_nrows = PLMAX(max_nrows, stmts[i]->trans->nrows);
        }
    }

    if (max_nrows >= 1) {
        for (i=0; i<nstmts; i++)    {
            if (stmts[i]->trans == NULL)    {
                stmts[i]->trans = pluto_matrix_alloc(max_nrows, 
                        stmts[i]->dim+prog->npar+1);
                stmts[i]->trans->nrows = 0;
            }

            int curr_rows = stmts[i]->trans->nrows;

            /* Add all zero rows */
            for (j=curr_rows; j<max_nrows; j++)    {
                pluto_matrix_add_row(stmts[i]->trans, stmts[i]->trans->nrows);
            }
        }

        int old_hyp_num = prog->num_hyperplanes;
        for (i=old_hyp_num; i<max_nrows; i++) {
            pluto_prog_add_hyperplane(prog, prog->num_hyperplanes);
            prog->hProps[prog->num_hyperplanes-1].type = H_SCALAR;
        }
    }
}


/* Only dimensionality and domain are essential */
Stmt *pluto_stmt_alloc(int dim, const PlutoConstraints *domain)
{
    int i, npar;

    npar = domain->ncols - 1 - dim;

    Stmt *stmt = (Stmt *) malloc(sizeof(Stmt));

    /* id will be assigned when added to PlutoProg */
    stmt->id = -1;
    stmt->dim = dim;
    stmt->dim_orig = dim;
    stmt->domain = pluto_constraints_dup(domain);

    /* Pre-allocate a little more to prevent frequent realloc */
    stmt->trans = pluto_matrix_alloc(2*dim+1, dim+npar+1);
    stmt->trans->nrows = 0;

    stmt->text = NULL;
    stmt->tile =  1;
    stmt->num_tiled_loops = 0;
    stmt->reads = NULL;
    stmt->writes = NULL;
    stmt->nreads = 0;
    stmt->nwrites = 0;

    if (dim >= 1)   {
        stmt->is_orig_loop = (bool *) malloc(dim*sizeof(bool));
        stmt->iterators = (char **) malloc(sizeof(char *)*dim);
        for (i=0; i<stmt->dim; i++) {
            stmt->iterators[i] = NULL;
        }
    }else{
        stmt->is_orig_loop = NULL;
        stmt->iterators = NULL;
    }

    return stmt;
}


void pluto_stmt_free(Stmt *stmt)
{
    int i, j;

    pluto_constraints_free(stmt->domain);

    pluto_matrix_free(stmt->trans);

    if (stmt->text != NULL) {
        free(stmt->text);
    }

    for (j=0; j<stmt->dim; j++)    {
        if (stmt->iterators[j] != NULL) {
            free(stmt->iterators[j]);
        }
    }

    /* If dim is zero, iterators, is_orig_loop are NULL */
    if (stmt->iterators != NULL)    {
        free(stmt->iterators);
        free(stmt->is_orig_loop);
    }

    PlutoAccess **writes = stmt->writes;
    PlutoAccess **reads = stmt->reads;

    if (writes != NULL) {
        for (i=0; i<stmt->nwrites; i++)   {
            pluto_matrix_free(writes[i]->mat);
            free(writes[i]);
        }
        free(writes);
    }
    if (reads != NULL) {
        for (i=0; i<stmt->nreads; i++)   {
            pluto_matrix_free(reads[i]->mat);
            free(reads[i]);
        }
        free(reads);
    }

    free(stmt);
}

/* Separates a list of statements */
void pluto_separate_stmts(PlutoProg *prog, Stmt **stmts, int num, int level)
{
    int i, nstmts, k;

    nstmts = prog->nstmts;

    // pluto_matrix_print(stdout, stmt->trans);
    for (i=0; i<nstmts; i++)    {
        pluto_matrix_add_row(prog->stmts[i]->trans, level);
    }
    // pluto_matrix_print(stdout, stmt->trans);
    for (k=0; k<num; k++)   {
        stmts[k]->trans->val[level][stmts[k]->trans->ncols-1] = 1+k;
    }

    pluto_prog_add_hyperplane(prog, level);
    prog->hProps[level].type = H_SCALAR;
    prog->hProps[level].dep_prop = SEQ;
}


/* Separates a statement from the rest (places it later) at that level;
 * this is done by inserting a scalar dimension separating them */
void pluto_separate_stmt(PlutoProg *prog, const Stmt *stmt, int level)
{
    int i, nstmts;

    nstmts = prog->nstmts;

    // pluto_matrix_print(stdout, stmt->trans);
    for (i=0; i<nstmts; i++)    {
        pluto_matrix_add_row(prog->stmts[i]->trans, level);
    }
    // pluto_matrix_print(stdout, stmt->trans);
    stmt->trans->val[level][stmt->trans->ncols-1] = 1;

    pluto_prog_add_hyperplane(prog, level);
    prog->hProps[level].type = H_SCALAR;
    prog->hProps[level].dep_prop = SEQ;
}
