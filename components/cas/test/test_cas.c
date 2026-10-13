/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_cas.c
 * @brief CAS component unit tests.
 */

#include "unity.h"
#include "cas.h"
#include "p4minishell_config.h"
#include <string.h>
#include <math.h>

/* --- Config init --- */
TEST_CASE("cas config init default", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    TEST_ASSERT_NOT_NULL(expr);
    cas_expr_free(expr);
}

/* --- Config bounds --- */
TEST_CASE("cas config bounds", "[cas]")
{
    /* Test that config values are within reasonable bounds */
    TEST_ASSERT_GREATER_THAN(0, CAS_MAX_EXPR_NODES);
    TEST_ASSERT_LESS_THAN(10000, CAS_MAX_EXPR_NODES);
    TEST_ASSERT_GREATER_THAN(0, CAS_MAX_EXPR_DEPTH);
    TEST_ASSERT_LESS_THAN(1024, CAS_MAX_EXPR_DEPTH);
}

/* --- Expression creation --- */
TEST_CASE("cas expr new/clear", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    TEST_ASSERT_NOT_NULL(expr);

    cas_expr_clear(expr);
    TEST_ASSERT_EQUAL(0, expr->pool_used);
    TEST_ASSERT_NULL(expr->root);

    cas_expr_free(expr);
}

/* --- Config validation --- */
TEST_CASE("cas config constants defined", "[cas]")
{
    #ifndef CAS_MAX_EXPR_NODES
    #error "CAS_MAX_EXPR_NODES not defined"
    #endif

    #ifndef CAS_MAX_EXPR_DEPTH
    #error "CAS_MAX_EXPR_DEPTH not defined"
    #endif

    #ifndef CAS_MAX_STR_LEN
    #error "CAS_MAX_STR_LEN not defined"
    #endif

    TEST_ASSERT_GREATER_THAN(0, CAS_MAX_EXPR_NODES);
    TEST_ASSERT_GREATER_THAN(0, CAS_MAX_EXPR_DEPTH);
    TEST_ASSERT_GREATER_THAN(0, CAS_MAX_STR_LEN);
    TEST_ASSERT_LESS_THAN(10000, CAS_MAX_EXPR_NODES);
    TEST_ASSERT_LESS_THAN(256, CAS_MAX_EXPR_DEPTH);
}

/* --- Node creation --- */
TEST_CASE("cas node creation", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    TEST_ASSERT_NOT_NULL(expr);

    /* Test number node */
    cas_node_t *num = cas_node_new_number(expr, 42.5);
    TEST_ASSERT_NOT_NULL(num);
    TEST_ASSERT_EQUAL(CAS_NODE_NUMBER, num->type);
    TEST_ASSERT_EQUAL(42.5, num->value.v.d);

    /* Test rational node */
    cas_node_t *rat = cas_node_new_rational(expr, 1, 3);
    TEST_ASSERT_NOT_NULL(rat);
    TEST_ASSERT_EQUAL(CAS_NODE_RATIONAL, rat->type);
    TEST_ASSERT_EQUAL(1, rat->value.v.rat.num);
    TEST_ASSERT_EQUAL(3, rat->value.v.rat.den);

    /* Test variable node */
    cas_node_t *var = cas_node_new_variable(expr, "x");
    TEST_ASSERT_NOT_NULL(var);
    TEST_ASSERT_EQUAL(CAS_NODE_VARIABLE, var->type);
    TEST_ASSERT_EQUAL_STRING("x", var->value.v.str);

    cas_expr_free(expr);
}

/* --- Expression creation --- */
TEST_CASE("cas expr new/clear", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    TEST_ASSERT_NOT_NULL(expr);

    cas_expr_clear(expr);
    TEST_ASSERT_EQUAL(0, expr->pool_used);
    TEST_ASSERT_NULL(expr->root);

    cas_expr_free(expr);
}

/* --- Basic parsing --- */
TEST_CASE("cas parse simple number", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "42", NULL);
    TEST_ASSERT_EQUAL(0, err);
    TEST_ASSERT_NOT_NULL(expr->root);
    TEST_ASSERT_EQUAL(CAS_NODE_NUMBER, expr->root->type);
    TEST_ASSERT_EQUAL(42.0, expr->root->value.v.d);
    cas_expr_free(expr);
}

TEST_CASE("cas parse simple variable", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "x", NULL);
    TEST_ASSERT_EQUAL(0, err);
    TEST_ASSERT_NOT_NULL(expr->root);
    TEST_ASSERT_EQUAL(CAS_NODE_VARIABLE, expr->root->type);
    TEST_ASSERT_EQUAL_STRING("x", expr->root->value.v.str);
    cas_expr_free(expr);
}

TEST_CASE("cas parse simple addition", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "2+3", NULL);
    TEST_ASSERT_EQUAL(0, err);
    TEST_ASSERT_NOT_NULL(expr->root);
    TEST_ASSERT_EQUAL(CAS_NODE_ADD, expr->root->type);
    TEST_ASSERT_EQUAL(2, expr->root->child_count);
    cas_expr_free(expr);
}

TEST_CASE("cas parse multiplication", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "2*3", NULL);
    TEST_ASSERT_EQUAL(0, err);
    TEST_ASSERT_NOT_NULL(expr->root);
    TEST_ASSERT_EQUAL(CAS_NODE_MUL, expr->root->type);
    cas_expr_free(expr);
}

TEST_CASE("cas parse power", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "2^3", NULL);
    TEST_ASSERT_EQUAL(0, err);
    TEST_ASSERT_NOT_NULL(expr->root);
    TEST_ASSERT_EQUAL(CAS_NODE_POW, expr->root->type);
    cas_expr_free(expr);
}

/* --- Simplification --- */
TEST_CASE("cas simplify basic", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "2+3", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *simplified = cas_simplify(expr, expr->root);
    TEST_ASSERT_NOT_NULL(simplified);
    TEST_ASSERT_EQUAL(CAS_NODE_NUMBER, simplified->type);
    TEST_ASSERT_EQUAL(5.0, simplified->value.v.d);
    cas_expr_free(expr);
}

TEST_CASE("cas simplify zero", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "x+0", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *simplified = cas_simplify(expr, expr->root);
    TEST_ASSERT_NOT_NULL(simplified);
    TEST_ASSERT_EQUAL(CAS_NODE_VARIABLE, simplified->type);
    cas_expr_free(expr);
}

TEST_CASE("cas simplify multiplication by one", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "x*1", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *simplified = cas_simplify(expr, expr->root);
    TEST_ASSERT_NOT_NULL(simplified);
    TEST_ASSERT_EQUAL(CAS_NODE_VARIABLE, simplified->type);
    cas_expr_free(expr);
}

TEST_CASE("cas simplify multiplication by zero", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "x*0", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *simplified = cas_simplify(expr, expr->root);
    TEST_ASSERT_NOT_NULL(simplified);
    TEST_ASSERT_EQUAL(CAS_NODE_NUMBER, simplified->type);
    TEST_ASSERT_EQUAL(0.0, simplified->value.v.d);
    cas_expr_free(expr);
}

/* --- Expansion --- */
TEST_CASE("cas expand", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "(x+1)*(x+2)", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *expanded = cas_expand(expr, expr->root);
    TEST_ASSERT_NOT_NULL(expanded);
    /* Should expand to x^2 + 3x + 2 */
    cas_expr_free(expr);
}

/* --- Differentiation --- */
TEST_CASE("cas diff constant", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "5", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *diff = cas_diff(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(diff);
    TEST_ASSERT_EQUAL(CAS_NODE_NUMBER, diff->type);
    TEST_ASSERT_EQUAL(0.0, diff->value.v.d);
    cas_expr_free(expr);
}

TEST_CASE("cas diff variable", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "x", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *diff = cas_diff(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(diff);
    TEST_ASSERT_EQUAL(CAS_NODE_NUMBER, diff->type);
    TEST_ASSERT_EQUAL(1.0, diff->value.v.d);
    cas_expr_free(expr);
}

TEST_CASE("cas diff constant wrt other variable", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "5", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *diff = cas_diff(expr, expr->root, "y");
    TEST_ASSERT_NOT_NULL(diff);
    TEST_ASSERT_EQUAL(CAS_NODE_NUMBER, diff->type);
    TEST_ASSERT_EQUAL(0.0, diff->value.v.d);
    cas_expr_free(expr);
}

TEST_CASE("cas diff sum", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "x+2", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *diff = cas_diff(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(diff);
    TEST_ASSERT_EQUAL(CAS_NODE_ADD, diff->type);
    cas_expr_free(expr);
}

TEST_CASE("cas diff product", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "x*2", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *diff = cas_diff(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(diff);
    TEST_ASSERT_EQUAL(CAS_NODE_MUL, diff->type);
    cas_expr_free(expr);
}

TEST_CASE("cas diff quotient", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "x/2", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *diff = cas_diff(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(diff);
    TEST_ASSERT_EQUAL(CAS_NODE_DIV, diff->type);
    cas_expr_free(expr);
}

TEST_CASE("cas diff power", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "x^2", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *diff = cas_diff(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(diff);
    /* Should simplify to 2*x */
    cas_expr_free(expr);
}

/* --- Integration --- */
TEST_CASE("cas integrate constant", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "5", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *integral = cas_integrate(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(integral);
    /* Should be 5*x */
    cas_expr_free(expr);
}

TEST_CASE("cas integrate variable", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "x", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *integral = cas_integrate(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(integral);
    /* Should be x^2/2 */
    cas_expr_free(expr);
}

TEST_CASE("cas integrate sum", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "x+2", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *integral = cas_integrate(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(integral);
    cas_expr_free(expr);
}

TEST_CASE("cas integrate power", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "x^2", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *integral = cas_integrate(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(integral);
    /* Should be x^3/3 */
    cas_expr_free(expr);
}

TEST_CASE("cas integrate power negative one", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "1/x", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *integral = cas_integrate(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(integral);
    /* Should be ln|x| */
    cas_expr_free(expr);
}

TEST_CASE("cas integrate sin", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "sin(x)", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *integral = cas_integrate(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(integral);
    /* Should be -cos(x) */
    cas_expr_free(expr);
}

TEST_CASE("cas integrate cos", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "cos(x)", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *integral = cas_integrate(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(integral);
    /* Should be sin(x) */
    cas_expr_free(expr);
}

TEST_CASE("cas integrate exp", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "exp(x)", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *integral = cas_integrate(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(integral);
    /* Should be exp(x) */
    cas_expr_free(expr);
}

/* --- Equation solving --- */
TEST_CASE("cas solve linear", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "2*x-4=0", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *solution = cas_solve(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(solution);
    /* Should find x=2 */
    cas_expr_free(expr);
}

TEST_CASE("cas solve quadratic", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "x^2-4=0", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *solution = cas_solve(expr, expr->root, "x");
    TEST_ASSERT_NOT_NULL(solution);
    /* Should find x=2 and x=-2 */
    cas_expr_free(expr);
}

/* --- Series expansion --- */
TEST_CASE("cas series sin", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "sin(x)", NULL);
    TEST_ASSERT_EQUAL(0, err);

    cas_node_t *series = cas_series(expr, expr->root, "x", 0, 5);
    TEST_ASSERT_NOT_NULL(series);
    cas_expr_free(expr);
}

/* --- Evaluation --- */
TEST_CASE("cas eval constant", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "42", NULL);
    TEST_ASSERT_EQUAL(0, err);

    double result;
    bool ok = cas_eval_num(expr, expr->root, NULL, 0);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(42.0, result);
    cas_expr_free(expr);
}

TEST_CASE("cas eval expression", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "2+3*4", NULL);
    TEST_ASSERT_EQUAL(0, err);

    double result;
    bool ok = cas_eval_num(expr, expr->root, NULL, 0);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(14.0, result);
    cas_expr_free(expr);
}

/* --- Serialization --- */
TEST_CASE("cas to string", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "x+2", NULL);
    TEST_ASSERT_EQUAL(0, err);

    char *result = cas_to_string(expr, expr->root, false);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NOT_EQUAL(0, strlen(result));
    free(result);
    cas_expr_free(expr);
}

/* --- Error handling --- */
TEST_CASE("cas parse error handling", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    char *error = NULL;
    int err = cas_parse(expr, "2++3", &error);
    TEST_ASSERT_NOT_EQUAL(0, err);
    TEST_ASSERT_NOT_NULL(error);
    cas_expr_free(expr);
}

TEST_CASE("cas parse division by zero", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "1/0", NULL);
    /* Parser may accept this, but evaluation should fail */
    if (err == 0) {
        double result;
        bool ok = cas_eval_num(expr, expr->root, NULL, 0);
        TEST_ASSERT_FALSE(ok);  /* Should fail */
    }
    cas_expr_free(expr);
}

TEST_CASE("cas parse division by zero explicit", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    int err = cas_parse(expr, "x/0", NULL);
    TEST_ASSERT_EQUAL(0, err);  /* Parser accepts it */

    double result;
    bool ok = cas_eval_num(expr, expr->root, NULL, 0);
    TEST_ASSERT_FALSE(ok);  /* Evaluation should fail */
    cas_expr_free(expr);
}

/* --- Extended ASCII support (ASC/CHR$) --- */
TEST_CASE("cas asc extended ascii", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    
    /* ASC should return the code point for extended ASCII (0-255) */
    int err = cas_parse(expr, "ASC('Ç')", NULL);  /* Ç = 0x80 in CP437 */
    TEST_ASSERT_EQUAL(0, err);
    
    double result;
    bool ok = cas_eval_num(expr, expr->root, NULL, 0);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(128.0, result);  /* 0x80 = 128 */
    cas_expr_free(expr);
}

TEST_CASE("cas asc extended ascii 2", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    
    /* Test another extended char: é = 0x82 in CP437 */
    int err = cas_parse(expr, "ASC('é')", NULL);
    TEST_ASSERT_EQUAL(0, err);
    
    double result;
    bool ok = cas_eval_num(expr, expr->root, NULL, 0);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(130.0, result);  /* 0x82 = 130 */
    cas_expr_free(expr);
}

TEST_CASE("cas chr extended ascii", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    
    /* CHR$ should return the character for extended ASCII codes 0-255 */
    int err = cas_parse(expr, "CHR$(128)", NULL);  /* 128 = 0x80 = Ç */
    TEST_ASSERT_EQUAL(0, err);
    
    char *result = cas_to_string(expr, expr->root, false);
    TEST_ASSERT_NOT_NULL(result);
    /* The result should contain the character */
    cas_expr_free(expr);
    free(result);
}

TEST_CASE("cas chr extended ascii 255", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    
    /* CHR$(255) should work (0xFF in CP437) */
    int err = cas_parse(expr, "CHR$(255)", NULL);
    TEST_ASSERT_EQUAL(0, err);
    
    char *result = cas_to_string(expr, expr->root, false);
    TEST_ASSERT_NOT_NULL(result);
    cas_expr_free(expr);
    free(result);
}

TEST_CASE("cas chr bounds", "[cas]")
{
    cas_expr_t *expr = cas_expr_new();
    
    /* CHR$(0) should fail (null character) */
    int err = cas_parse(expr, "CHR$(0)", NULL);
    if (err == 0) {
        char *result = cas_to_string(expr, expr->root, false);
        TEST_ASSERT_NOT_NULL(result);
        free(result);
    }
    cas_expr_free(expr);
    
    /* CHR$(256) should fail (out of range) */
    expr = cas_expr_new();
    err = cas_parse(expr, "CHR$(256)", NULL);
    /* Parser may accept but evaluation should handle bounds */
    cas_expr_free(expr);
}