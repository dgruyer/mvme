#include <cmath>
#include <iostream>
#include "gtest/gtest.h"
#include "a2_exprtk.h"

using std::cout;
using std::endl;

TEST(a2Exprtk, SymbolTableAddThings)
{
    using namespace a2::a2_exprtk;

    SymbolTable symtab;
    double scalar1 = 42.0;
    std::string string1 = "Hello, world!";
    std::vector<double> empty_vec;
    std::vector<double> filled_vec(10, 42.0);

    {
        auto var_names = symtab.getSymbolNames();
        ASSERT_TRUE(var_names.size() == 0);
    }

    {
        ASSERT_TRUE(symtab.addScalar("scalar1", scalar1));
        ASSERT_FALSE(symtab.addScalar("scalar1", scalar1));
        auto var_names = symtab.getSymbolNames();
        ASSERT_EQ(var_names.size(), 1);
        ASSERT_TRUE(std::find(var_names.begin(), var_names.end(), "scalar1") != var_names.end());
    }

    {
        ASSERT_TRUE(symtab.addString("string1", string1));
        ASSERT_FALSE(symtab.addScalar("scalar1", scalar1));
        ASSERT_FALSE(symtab.addString("string1", string1));
        auto var_names = symtab.getSymbolNames();
        ASSERT_EQ(var_names.size(), 2);
        ASSERT_TRUE(std::find(var_names.begin(), var_names.end(), "scalar1") != var_names.end());
        ASSERT_TRUE(std::find(var_names.begin(), var_names.end(), "string1") != var_names.end());
    }

    {
        ASSERT_FALSE(symtab.addVector("empty_vec", empty_vec));
        auto var_names = symtab.getSymbolNames();
        ASSERT_FALSE(std::find(var_names.begin(), var_names.end(), "vector1") != var_names.end());
    }

    {
        ASSERT_TRUE(symtab.addVector("vector1", filled_vec));
        // NOTE: exprtk should return false here but doesn't. The reason is
        // that symbol_table<T>::symbol_exists() doesn't check the vector_store
        // for the name. This might be a bug or for some reason deliberate.
        // Update: I fixed the code in the local copy of exprtk.
        ASSERT_FALSE(symtab.addVector("vector1", filled_vec));

        auto var_names = symtab.getSymbolNames();
        ASSERT_EQ(var_names.size(), 3);
        ASSERT_TRUE(std::find(var_names.begin(), var_names.end(), "scalar1") != var_names.end());
        ASSERT_TRUE(std::find(var_names.begin(), var_names.end(), "string1") != var_names.end());
        ASSERT_TRUE(std::find(var_names.begin(), var_names.end(), "vector1") != var_names.end());
    }
}

TEST(a2Exprtk, SymbolTableCopyAndAssignAndGet)
{
    using namespace a2::a2_exprtk;

    double x = 42.0;
    std::string string1 = "Hello, world!";
    std::vector<double> filled_vec(10, 42.0);

    {
        SymbolTable src_symtab;

        src_symtab.addScalar("x",   x);
        src_symtab.addString("str", string1);
        src_symtab.addVector("vec", filled_vec);

        SymbolTable dst_symtab(src_symtab);

        ASSERT_NE(src_symtab.getScalar("x"), nullptr);
        ASSERT_EQ(src_symtab.getScalar("x"), dst_symtab.getScalar("x"));

        ASSERT_NE(src_symtab.getString("str"), nullptr);
        ASSERT_EQ(src_symtab.getString("str"), dst_symtab.getString("str"));

        ASSERT_NE(src_symtab.getVector("vec").first, nullptr);
        ASSERT_EQ(src_symtab.getVector("vec").first, dst_symtab.getVector("vec").first);
    }

    {
        SymbolTable src_symtab;

        src_symtab.addScalar("x",   x);
        src_symtab.addString("str", string1);
        src_symtab.addVector("vec", filled_vec);

        SymbolTable dst_symtab;

        dst_symtab = src_symtab;

        ASSERT_NE(src_symtab.getScalar("x"), nullptr);
        ASSERT_EQ(src_symtab.getScalar("x"), dst_symtab.getScalar("x"));

        ASSERT_NE(src_symtab.getString("str"), nullptr);
        ASSERT_EQ(src_symtab.getString("str"), dst_symtab.getString("str"));

        ASSERT_NE(src_symtab.getVector("vec").first, nullptr);
        ASSERT_EQ(src_symtab.getVector("vec").first, dst_symtab.getVector("vec").first);
    }
}

TEST(a2Exprtk, ExpressionBasicEval)
{
    using namespace a2::a2_exprtk;

    {
        // undefined variable x
        Expression expr("3*x + 42");
        ASSERT_ANY_THROW(expr.compile());
    }

    {
        // internal variable definition
        Expression expr("var x := 5; 3*x + 42");
        expr.compile();
        ASSERT_EQ(expr.value(), (3 * 5 + 42));
    }

    {
        // internal variable, using e_commutative_check feature
        Expression expr("var x := 5; 3x + 42");
        expr.compile();
        ASSERT_EQ(expr.value(), (3 * 5 + 42));
        ASSERT_EQ(expr.results().size(), 0);
    }

    {
        // using a constant
        Expression expr("3*x + 42");
        SymbolTable symtab;
        symtab.addConstant("x", 5.0);
        expr.registerSymbolTable(symtab);
        expr.compile();
        ASSERT_EQ(expr.value(), (3 * 5 + 42));
        ASSERT_EQ(expr.results().size(), 0);
    }

    {
        // using an external variable
        Expression expr("3*x + 42");
        SymbolTable symtab;
        double x = 5.0;
        symtab.addScalar("x", x);
        expr.registerSymbolTable(symtab);
        expr.compile();
        ASSERT_EQ(expr.value(), (3 * 5 + 42));
        ASSERT_EQ(expr.results().size(), 0);
    }
}

TEST(a2Exprtk, ExpressionReturnResult)
{
    using namespace a2::a2_exprtk;

    Expression expr(
        "var d    := 42.0;"
        "var v[3] := { 1, 2, 3};"
        "var s    := 'Hello, World!';"

        "return [d, v, s];"
        );

    expr.compile();
    ASSERT_TRUE(std::isnan(expr.value()));
    auto results = expr.results();
    ASSERT_EQ(results.size(), 3);
    ASSERT_EQ(results[0].type, Expression::Result::Scalar);
    ASSERT_EQ(results[1].type, Expression::Result::Vector);
    ASSERT_EQ(results[2].type, Expression::Result::String);
}
